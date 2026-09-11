/**
 * @file tests/test_dtls_retransmit_no_duplicate.cpp
 * @brief Regression test for the DTLS handshake-flight double-emission bug.
 *
 * ## Bug history
 *
 * DtlsSessionWolfSSL::tick() previously invoked pump_handshake() in
 * addition to flush_send_buf(), so on every retransmit cycle the same
 * handshake flight was emitted TWICE — once by tick(), once by
 * take_outbound()'s internal pump.  Chrome (and any DTLS peer that
 * parses the wire carefully) treats the duplicated flights as a
 * protocol violation: it sees the ServerHello/Cert/SKE/ServerHelloDone
 * flight at seq=0..3, then again at seq=4..7 (or worse, seq=8..11),
 * raises "unexpected_message" and tears down the handshake.
 *
 * The bug lived undetected as "intermittent Case D failure — Chrome
 * timing" because the duplication is conditional on the retransmit
 * timer having fired (>=1 s after the original transmission), so it
 * manifests non-deterministically depending on Chrome's startup latency.
 *
 * ## Fix
 *
 * tick() now only calls flush_send_buf(); take_outbound() is the
 * single owner of pump_handshake().  See the long comment at
 * DtlsSessionWolfSSL::tick() for the full rationale.
 *
 * ## What this test asserts
 *
 * 1. After feeding ClientHello and draining the first ServerHello flight,
 *    a single retransmit cycle (timer fires -> tick() -> take_outbound())
 *    must produce exactly ONE outbound record, not two.
 * 2. The retransmits stat must increase by exactly ONE per timer fire.
 * 3. After draining, take_outbound() called again WITHOUT another tick()
 *    must return ZERO records (the outbound buffer is empty).
 *
 * The test runs for ~3.6 s — just long enough to drive 2 retransmit
 * cycles (1 s initial timer, then 2 s after the first retransmit).
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <span>
#include <thread>
#include <vector>

#include <nimrtc/dtls/dtls.hpp>

using clk = std::chrono::steady_clock;

namespace {

// Drive `out -> in` once.
void pump(nimrtc::dtls::DtlsSession& out,
           nimrtc::dtls::DtlsSession& in) {
    for (auto& rec : out.take_outbound()) {
        nimrtc::dtls::DtlsAddr from{"127.0.0.1", 51000};
        in.feed_inbound(rec.bytes, from);
    }
}

}  // namespace

int main() {
    using namespace nimrtc::dtls;

    // ---- Set up server + client -----------------------------------------
    Config cfgS{};
    cfgS.role = DtlsRole::Server;
    cfgS.srtp_profile = SrtpProfile::Aes128CmSha1_80;
    DtlsSession server(cfgS);
    if (auto r = server.open(); !r) {
        std::fprintf(stderr, "server.open failed: %s\n",
                     r.error().message().c_str());
        return 1;
    }

    Config cfgC{};
    cfgC.role = DtlsRole::Client;
    cfgC.srtp_profile = SrtpProfile::Aes128CmSha1_80;
    DtlsSession client(cfgC);
    if (auto r = client.open(); !r) {
        std::fprintf(stderr, "client.open failed: %s\n",
                     r.error().message().c_str());
        return 1;
    }

    // Exchange fingerprints so the SPKI pin path doesn't fail-closed
    // before we get a chance to drive the retransmit.
    auto fp_server = server.local_fingerprint();
    auto fp_client = client.local_fingerprint();
    {
        std::vector<std::uint8_t> raw(fp_client.bytes.begin(),
                                       fp_client.bytes.end());
        server.set_peer_fingerprint("sha-256", std::move(raw));
    }
    {
        std::vector<std::uint8_t> raw(fp_server.bytes.begin(),
                                       fp_server.bytes.end());
        client.set_peer_fingerprint("sha-256", std::move(raw));
    }

    // ---- Drive ClientHello -> Server; drain initial ServerHello flight ---
    //
    // We do NOT drive the full handshake (no client->server exchange).
    // The server is left waiting for ClientKeyExchange, so wolfSSL's
    // internal retransmit timer will fire ~1 s after the ServerHello
    // flight went out — exactly the state that triggered the
    // double-emission in Case D.
    auto client_out = client.take_outbound();
    if (client_out.empty()) {
        // The client hasn't produced ClientHello yet — force it by
        // calling tick() once.  This is the same pattern the engine
        // uses in NimRTCEngine::drain_dtls().
        client.tick();
        client_out = client.take_outbound();
    }
    if (client_out.empty()) {
        std::fprintf(stderr, "client produced no outbound; cannot drive test\n");
        return 2;
    }
    // Feed the ClientHello to the server.
    {
        nimrtc::dtls::DtlsAddr from{"127.0.0.1", 50000};
        server.feed_inbound(client_out[0].bytes, from);
    }
    // Drain the ServerHello flight + any subsequent bytes.
    auto server_initial_out = server.take_outbound();
    std::size_t server_initial_bytes = 0;
    for (auto& r : server_initial_out) server_initial_bytes += r.bytes.size();
    std::fprintf(stderr,
        "[setup] initial server outbound records=%zu  total_bytes=%zu\n",
        server_initial_out.size(), server_initial_bytes);
    if (server_initial_out.empty()) {
        std::fprintf(stderr, "server produced no initial outbound\n");
        return 2;
    }

    // Snapshot baseline stats.
    const auto sstats0 = server.stats();
    const std::uint64_t retransmits_baseline = sstats0.retransmits;
    std::fprintf(stderr,
        "[setup] baseline retransmits=%llu  records_out=%llu\n",
        (unsigned long long)retransmits_baseline,
        (unsigned long long)sstats0.records_out);

    // ---- Wait for the wolfSSL initial retransmit timer (1 s) ------------
    //
    // RFC 6347 §4.2.4: the initial retransmit timer is 1 s, then doubles
    // on every subsequent retransmit up to 60 s.  We wait 1.4 s to give
    // a comfortable margin past the first fire.
    std::this_thread::sleep_for(std::chrono::milliseconds(1400));

    // ---- Drive ONE retransmit cycle -------------------------------------
    //
    // tick() is responsible for asking wolfSSL whether the timer has
    // fired and (if so) flushing the re-emitted bytes from send_buf_
    // to outbound_.  take_outbound() then drains outbound_ AND pumps
    // the handshake one more time so any state-machine progress shows
    // up in the same drain call.
    //
    // With the bug, the OLD tick() called pump_handshake() too, which
    // meant wolfSSL_accept() ran twice per retransmit cycle and the
    // re-emitted flight ended up in outbound_ TWICE.
    server.tick();
    auto retransmit_records = server.take_outbound();
    const auto sstats1 = server.stats();
    const std::uint64_t retransmits_after_first =
        sstats1.retransmits - retransmits_baseline;

    std::fprintf(stderr,
        "[retransmit-1] outbound_records=%zu  retransmits_delta=%llu\n",
        retransmit_records.size(),
        (unsigned long long)retransmits_after_first);

    // ---- Assertion 1: at most one record per retransmit cycle ----------
    //
    // The ServerHello flight is a single DTLS record (it fits in one
    // UDP datagram); if wolfSSL re-emitted it twice (the bug) we'd
    // see 2 records here.  Allow a tolerance of 1 in case the state
    // machine produced a stray CCS, but assert the upper bound.
    if (retransmit_records.size() > 1) {
        std::fprintf(stderr,
            "FAIL: retransmit produced %zu records (expected <=1).  "
            "This indicates the double-emission bug has returned.\n",
            retransmit_records.size());
        return 3;
    }

    // ---- Assertion 2: the retransmits stat increased by exactly one ----
    if (retransmits_after_first != 1) {
        std::fprintf(stderr,
            "FAIL: stats.retransmits delta=%llu (expected 1)\n",
            (unsigned long long)retransmits_after_first);
        return 4;
    }

    // ---- Assertion 3: a second take_outbound() without tick() is empty -
    //
    // After draining the single retransmission, no more bytes should
    // be queued.  If the bug were present, we'd have drained 2 records
    // in step 1 but the second pump_handshake() would still have
    // re-emitted one more copy into send_buf_, which take_outbound()
    // would then pull out — meaning this second call would return
    // >=1 record instead of 0.
    auto stray = server.take_outbound();
    if (!stray.empty()) {
        std::fprintf(stderr,
            "FAIL: stray records after single retransmit cycle: %zu.  "
            "Double-emission still present.\n", stray.size());
        return 5;
    }

    // ---- Drive a SECOND retransmit cycle (timer should be ~2 s) -------
    //
    // After the first retransmit, RFC 6347 §4.2.4 doubles the timer
    // to 2 s.  Wait that long, then verify the same single-flight
    // invariant holds.
    std::this_thread::sleep_for(std::chrono::milliseconds(2200));

    server.tick();
    auto retransmit_records_2 = server.take_outbound();
    const auto sstats2 = server.stats();
    const std::uint64_t retransmits_after_second =
        sstats2.retransmits - retransmits_baseline;

    std::fprintf(stderr,
        "[retransmit-2] outbound_records=%zu  retransmits_delta=%llu  "
        "total_records_out=%llu\n",
        retransmit_records_2.size(),
        (unsigned long long)retransmits_after_second,
        (unsigned long long)sstats2.records_out);

    if (retransmit_records_2.size() > 1) {
        std::fprintf(stderr,
            "FAIL: second retransmit produced %zu records (expected <=1)\n",
            retransmit_records_2.size());
        return 6;
    }
    if (retransmits_after_second != 2) {
        std::fprintf(stderr,
            "FAIL: second retransmit stat delta=%llu (expected 2)\n",
            (unsigned long long)retransmits_after_second);
        return 7;
    }

    std::fprintf(stderr,
        "PASS: retransmit single-emission invariant holds across 2 cycles\n");
    return 0;
}
