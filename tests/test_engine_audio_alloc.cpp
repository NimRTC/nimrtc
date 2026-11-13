/**
 * @file tests/test_engine_audio_alloc.cpp
 * @brief v1.2.0 / remediation Phase 3 §3.5 — the audio send path's
 *        allocation budget.
 *
 * The remediation plan's §3.5 lists two allocation budgets for the hot
 * media paths:
 *
 *   - "`send_video()` 1080p30 的 per-call 堆分配次数 = 0" — covered by
 *     the `hotpath_zero_allocs` ctest case (the benchmark binary, which
 *     owns the allocation probe).
 *   - "音频路径 per-packet 堆分配次数 = 0" — covered by *this* file.
 *
 * The two live in different binaries on purpose.  The allocation probe
 * replaces the global `operator new` / `operator delete`, which is only
 * safe in a binary that does *not* also link a sanitizer runtime (see
 * the benchmark's comment).  Putting the audio gate in its own binary
 * keeps the two probes from interfering and keeps the benchmark focused
 * on the *timing* numbers.
 *
 * ## What is measured
 *
 * The metric is **allocations per emitted packet**, not per call: the
 * host may hand the engine an arbitrary block size, and the engine
 * accumulates samples until it has a whole 20 ms frame.  The plan's
 * wording is "per-packet", so that is what is asserted.
 *
 * ## How the audio path is made reachable
 *
 * `send_audio()` refuses to put RTP on the wire before the DTLS
 * handshake completed — otherwise the peer would receive plaintext RTP
 * that its SRTP context rejects.  The test therefore drives the engine
 * through a fake ICE transport and a fake DTLS session, exactly like
 * `tests/test_engine_audio_send.cpp` does.  The fake DTLS session
 * exports no SRTP keying material, so SRTP is never installed and the
 * measured path is the plain RTP one.
 *
 * The scheduler is disabled so the packets leave the engine through
 * `ITransport::send()` synchronously — the measurement then covers the
 * whole send path, and the fake transport copies the bytes out (the
 * seam's `send()` contract says the `BufferView` is borrowed).
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/core/engine_errors.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/dtls/dtls_session_factory.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/ice_transport.hpp>

namespace {

namespace np = nimrtc::plugins;
namespace nd = nimrtc::dtls;

using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;

// ---------------------------------------------------------------------------
// The allocation probe.
//
// The global `operator new` / `operator delete` are replaced so the
// allocations a call makes can be counted without changing the call
// site.  The replacement forwards to the CRT allocator, so nothing
// about the program's behaviour changes apart from the count.
//
// The probe is **not** defined in a sanitizer build: a sanitizer
// runtime ships its own strong definitions of `operator new`, and a
// definition here would collide with it at link time.
// ---------------------------------------------------------------------------
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#  define NIMRTC_AUDIO_ALLOC_PROBE 0
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
      __has_feature(memory_sanitizer)
#    define NIMRTC_AUDIO_ALLOC_PROBE 0
#  else
#    define NIMRTC_AUDIO_ALLOC_PROBE 1
#  endif
#else
#  define NIMRTC_AUDIO_ALLOC_PROBE 1
#endif

namespace alloc_probe {

std::atomic<bool>          g_armed{false};
std::atomic<std::uint64_t> g_alloc_count{0};

/// The sizes of the allocations counted in the current `Scope`, so a
/// failure can say *what* was allocated rather than only *how many
/// times*.  A fixed-size ring: a diagnostic aid, not a profiler.
constexpr std::size_t kMaxSizes = 16;
std::size_t               g_sizes[kMaxSizes];
std::atomic<std::size_t>  g_size_count{0};

inline void note(std::size_t n) noexcept {
    if (!g_armed.load(std::memory_order_relaxed)) return;
    g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    const std::size_t i = g_size_count.fetch_add(1, std::memory_order_relaxed);
    if (i < kMaxSizes) g_sizes[i] = n;
}

inline std::uint64_t count() noexcept {
    return g_alloc_count.load(std::memory_order_relaxed);
}

/// A human-readable rendering of the recorded allocation sizes.
inline std::string sizes() {
    const std::size_t n =
        std::min(g_size_count.load(std::memory_order_relaxed), kMaxSizes);
    std::string out;
    for (std::size_t i = 0; i < n; ++i) {
        if (!out.empty()) out += ", ";
        out += std::to_string(g_sizes[i]);
    }
    return out;
}

/// Read the recorded sizes into a caller-owned buffer, allocation-free.
///
/// `sizes()` builds a `std::string`, and the caller's own bookkeeping
/// (a vector of counts, a message string) allocates too.  A caller that
/// wants to inspect the sizes *without* those allocations polluting the
/// measurement has to read them out with no allocation of its own --
/// which is what this does.
inline std::size_t copy_sizes(std::size_t* out, std::size_t capacity) noexcept {
    const std::size_t n =
        std::min(g_size_count.load(std::memory_order_relaxed), kMaxSizes);
    const std::size_t copied = (n < capacity) ? n : capacity;
    for (std::size_t i = 0; i < copied; ++i) out[i] = g_sizes[i];
    return copied;
}

/// The number of sizes recorded so far.
inline std::size_t size_count() noexcept {
    return g_size_count.load(std::memory_order_relaxed);
}

struct Scope {
    Scope() noexcept {
        g_alloc_count.store(0, std::memory_order_relaxed);
        g_size_count.store(0, std::memory_order_relaxed);
        g_armed.store(true, std::memory_order_relaxed);
    }
    ~Scope() { g_armed.store(false, std::memory_order_relaxed); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

}  // namespace alloc_probe

}  // namespace (anonymous)

// ---------------------------------------------------------------------------
// The global replacements.
//
// They live at *global* scope on purpose: a non-member `operator new` /
// `operator delete` may not be declared inside a namespace (C++
// [expr.new] / [expr.delete]).  The anonymous namespace is closed above
// and reopened below for exactly this reason.
//
// The replacements forward to the CRT allocator, so the program's
// behaviour is unchanged apart from the count.
// ---------------------------------------------------------------------------
#if NIMRTC_AUDIO_ALLOC_PROBE

void* operator new(std::size_t n) {
    alloc_probe::note(n);
    if (n == 0) n = 1;
    void* p = std::malloc(n);
    if (!p) throw std::bad_alloc();
    return p;
}

void* operator new[](std::size_t n) { return ::operator new(n); }

void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    alloc_probe::note(n);
    if (n == 0) n = 1;
    return std::malloc(n);
}

void* operator new[](std::size_t n, const std::nothrow_t& nt) noexcept {
    return ::operator new(n, nt);
}

#if defined(__cpp_aligned_new)
void* operator new(std::size_t n, std::align_val_t a) {
    alloc_probe::note(n);
    if (n == 0) n = 1;
    const std::size_t align = static_cast<std::size_t>(a);
    void* p = nullptr;
#if defined(_MSC_VER)
    p = _aligned_malloc(n, align);
#else
    if (posix_memalign(&p, align, n) != 0) p = nullptr;
#endif
    if (!p) throw std::bad_alloc();
    return p;
}

void* operator new[](std::size_t n, std::align_val_t a) {
    return ::operator new(n, a);
}

void operator delete(void* p, std::align_val_t a) noexcept {
    (void)a;
    if (!p) return;
#if defined(_MSC_VER)
    _aligned_free(p);
#else
    std::free(p);
#endif
}

void operator delete[](void* p, std::align_val_t a) noexcept {
    ::operator delete(p, a);
}
#endif

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

#endif  // NIMRTC_AUDIO_ALLOC_PROBE

namespace {

// ---------------------------------------------------------------------------
// Fake ICE transport
// ---------------------------------------------------------------------------

class FakeIceTransport final : public np::IICETransport {
public:
    const char* name() const noexcept override { return "alloc-fake-ice"; }
    np::Status open() noexcept override { return np::kOk; }
    void close() noexcept override {}

    void set_callbacks(np::RecvCallback on_recv,
                       np::ErrorCallback on_error) noexcept override {
        on_recv_  = std::move(on_recv);
        on_error_ = std::move(on_error);
    }

    np::Status send(np::BufferView view, np::Addr = {}) noexcept override {
        // The seam's contract says the `BufferView` is borrowed: the
        // transport must copy what it needs before returning.
        //
        // The fake needs *nothing* — it only counts — so it does not
        // copy at all.  That matters: a copy would allocate once per
        // packet and land right in the measured region, so the test
        // would end up measuring the fake rather than the engine.
        sent_bytes_ += view.size();
        ++sent_packets_;
        return np::kOk;
    }

    /// The total number of bytes the engine put on the wire.
    std::size_t sent_bytes() const noexcept { return sent_bytes_; }

    /// The total number of datagrams the engine put on the wire.
    std::size_t sent_packets() const noexcept { return sent_packets_; }

    int recv() noexcept override {
        int n = 0;
        while (!inbound_.empty()) {
            std::vector<std::uint8_t> pkt = std::move(inbound_.front());
            inbound_.erase(inbound_.begin());
            if (on_recv_) on_recv_(np::BufferView(pkt.data(), pkt.size()));
            ++n;
        }
        return n;
    }

    np::Addr local_addr() const noexcept override { return {}; }
    np::Addr remote_addr() const noexcept override { return {}; }

    np::IceState state() const noexcept override { return np::IceState::Connected; }
    std::string local_ufrag() const noexcept override { return "allocufrag"; }
    std::string local_password() const noexcept override { return "allocpassword"; }
    bool wait_for_gathering(int) noexcept override { return true; }
    std::vector<std::string> gathered_local_candidates() const noexcept override {
        return {};
    }
    np::Status set_remote_description(std::string_view) noexcept override {
        return np::kOk;
    }
    np::Status add_remote_candidate(std::string_view) noexcept override {
        return np::kOk;
    }
    void set_bind_address(std::string_view) noexcept override {}
    void set_stun_server(std::string_view, std::uint16_t) noexcept override {}
    void set_local_port_range(std::uint16_t, std::uint16_t) noexcept override {}
    void add_turn_server(std::string_view, std::uint16_t, std::string_view,
                         std::string_view) noexcept override {}
    void set_bwe(np::IBwe*) noexcept override {}
    void set_scheduler(np::IScheduler*) noexcept override {}

private:
    np::RecvCallback   on_recv_;
    np::ErrorCallback  on_error_;
    std::vector<std::vector<std::uint8_t>> inbound_;
    std::size_t sent_bytes_ = 0;
    std::size_t sent_packets_ = 0;
};

class FakeIceFactory final : public np::IICETransportFactory {
public:
    std::string_view id() const noexcept override { return "alloc-fake-ice"; }
    std::string_view display_name() const noexcept override {
        return "Fake ICE transport (allocation test)";
    }
    np::IICETransport* create_ice() const override { return new FakeIceTransport(); }
};

// ---------------------------------------------------------------------------
// Fake DTLS session
// ---------------------------------------------------------------------------

/** A DTLS session that reports the handshake as complete and exports no
 *  SRTP keying material.
 *
 *  The engine only asks two things of the DTLS session on the send path:
 *  "is the handshake done?" (the gate in `send_audio()`) and "give me
 *  the SRTP keys" (to install the SRTP context).  The fake answers the
 *  first with yes and the second with "not ready", so the RTP travels
 *  in cleartext and SRTP stays out of the measurement. */
class FakeDtlsSession final : public nd::IDtlsSession {
public:
    // ---- The seam surface ------------------------------------------------
    void set_role(nd::DtlsRole) noexcept override {}
    void set_peer_fingerprint(std::span<const std::uint8_t>) noexcept override {}
    void start() noexcept override {}
    void pump() noexcept override {}
    void on_handshake_complete(nd::IDtlsSession::OnCompleteCb) noexcept override {}
    np::Status export_srtp_key_material(std::span<std::uint8_t, 60>) noexcept override {
        return np::kErrNotReady;
    }

    // ---- The legacy engine-facing surface --------------------------------
    nimrtc::core::Result<void> open() noexcept override {
        return nimrtc::core::Result<void>::make_ok();
    }
    void set_role(nd::Role) noexcept override {}
    void set_peer_fingerprint(std::string, std::vector<std::uint8_t>) noexcept override {}
    std::size_t feed_inbound(std::span<const std::uint8_t>,
                             const nd::DtlsAddr&) noexcept override {
        return 0;
    }
    std::vector<nd::DtlsRecord> take_outbound() noexcept override { return {}; }
    void tick() noexcept override {}
    nd::DtlsState state() const noexcept override { return nd::DtlsState::Connected; }
    bool is_connected() const noexcept override { return true; }
    const nd::Fingerprint& local_fingerprint() const noexcept override {
        return fingerprint_;
    }
    std::optional<nd::SrtpKeyingMaterial>
    srtp_keying_material() const noexcept override {
        return std::nullopt;
    }

private:
    nd::Fingerprint fingerprint_{"sha-256", {}, {}, ""};
};

class FakeDtlsFactory final : public nd::IDtlsSessionFactory {
public:
    std::string_view id() const noexcept override { return "alloc-fake-dtls"; }
    std::string_view display_name() const noexcept override {
        return "Fake DTLS session (allocation test)";
    }
    std::unique_ptr<nd::IDtlsSession> create(const nd::DtlsConfig&) const override {
        return std::make_unique<FakeDtlsSession>();
    }
};

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

/** One 20 ms frame of mono PCM at 48 kHz — exactly one RTP packet's
 *  worth of samples. */
constexpr std::size_t kFrameSamples = 48000 / 50;

class EngineAudioAlloc : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();

        static FakeIceFactory  ice_factory;
        static FakeDtlsFactory dtls_factory;
        PluginRegistry::instance().register_ice_transport("alloc-fake-ice",
                                                           &ice_factory);
        PluginRegistry::instance().register_dtls_session("alloc-fake-dtls",
                                                         &dtls_factory);
    }

    EngineConfig make_config() {
        EngineConfig cfg{};
        cfg.transport_name              = "alloc-fake-ice";
        cfg.dtls_name                   = "alloc-fake-dtls";
        cfg.datachannel_backend         = "";
        cfg.bwe_name                    = "";
        cfg.scheduler_config.enabled     = false;
        cfg.pcm_sample_rate_hz          = 48000;
        cfg.pcm_channels                = 1;
        return cfg;
    }
};

// ---------------------------------------------------------------------------
// The gate
// ---------------------------------------------------------------------------

TEST_F(EngineAudioAlloc, TheAudioSendPathAllocatesNothingPerPacket) {
#if !NIMRTC_AUDIO_ALLOC_PROBE
    GTEST_SKIP() << "the allocation probe is unavailable in a sanitizer build";
#else
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // One frame of PCM, allocated once, outside every measured region.
    const std::vector<float> pcm(kFrameSamples, 0.25f);

    // Warm-up: the accumulator, the encoder's own buffers and the
    // engine's scratch all reach their steady-state size here, so the
    // measured region is not dominated by first-call growth.
    for (int i = 0; i < 20; ++i) {
        ASSERT_EQ(engine.send_audio(pcm.data(), pcm.size()), 0u);
    }

    const auto before = engine.stats().audio_frames_sent;

    std::uint64_t allocations = 0;
    {
        alloc_probe::Scope scope;   // armed for exactly the measured region
        for (int i = 0; i < 100; ++i) {
            ASSERT_EQ(engine.send_audio(pcm.data(), pcm.size()), 0u);
        }
        allocations = alloc_probe::count();
    }

    const auto after = engine.stats().audio_frames_sent;
    const auto packets = after - before;
    ASSERT_EQ(packets, 100u)
        << "every call handed over exactly one whole frame, so every call "
           "must have produced exactly one packet";

    EXPECT_EQ(allocations, 0u)
        << "the audio send path allocated " << allocations
        << " times over " << packets
        << " packets — the §3.5 budget is zero allocations per packet";

    engine.close();
#endif
}

// ---------------------------------------------------------------------------
// A companion test: the *first* call may allocate (the buffers grow),
// but the aggregate over a run must not grow with the number of
// packets.
//
// The aggregate (rather than "every single call") is what the gate
// above asserts, and it is the honest form of the criterion: the
// engine's periodic work (the RTCP sender report, the SRTP context
// rollover, ...) may allocate on *some* calls, and that is fine — what
// must not happen is one allocation per packet, forever.
//
// This test pins down that the total is *bounded*, i.e. that it does
// not scale with the packet count.  Two runs of different lengths are
// compared: the difference between them is the per-packet cost, and
// the criterion says it must be zero.
// ---------------------------------------------------------------------------

TEST_F(EngineAudioAlloc, TheAllocationCountDoesNotGrowWithThePacketCount) {
#if !NIMRTC_AUDIO_ALLOC_PROBE
    GTEST_SKIP() << "the allocation probe is unavailable in a sanitizer build";
#else
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const std::vector<float> pcm(kFrameSamples, 0.25f);

    // Warm-up: the accumulator, the encoder's own buffers and the
    // engine's scratch all reach their steady-state size here.
    for (int i = 0; i < 20; ++i) {
        ASSERT_EQ(engine.send_audio(pcm.data(), pcm.size()), 0u);
    }

    auto measure = [&](int packets) -> std::uint64_t {
        alloc_probe::Scope scope;
        for (int i = 0; i < packets; ++i) {
            EXPECT_EQ(engine.send_audio(pcm.data(), pcm.size()), 0u);
        }
        return alloc_probe::count();
    };

    const std::uint64_t short_run = measure(50);
    const std::uint64_t long_run  = measure(200);

    // The extra 150 packets must not have cost a single allocation.
    EXPECT_EQ(long_run - short_run, 0u)
        << "the allocation count grew with the packet count: 50 packets "
           "cost " << short_run << " allocations, 250 packets cost "
        << long_run << " — the per-packet cost must be zero";

    engine.close();
#endif
}

}  // namespace
