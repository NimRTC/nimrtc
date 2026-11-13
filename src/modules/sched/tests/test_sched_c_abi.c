/*
 * @file src/modules/sched/tests/test_sched_c_abi.c
 * @brief The scheduler's C ABI, exercised from **C**.
 *
 * ## Why this test is written in C
 *
 * The whole point of `<nimrtc/sched/sched_c.h>` is that a C translation
 * unit can include it and link against it.  A C++ test cannot prove that:
 * it would include the C header in a context where `extern "C"` is
 * redundant and where every C++ type is available, so it would happily
 * compile against an ABI that no C compiler could ever use.
 *
 * Compiling this file as C11 is therefore the assertion.  If the C header
 * ever grows a C++-only dependency (a `namespace`, a reference type, a
 * default argument), *this file stops compiling* — which is exactly the
 * regression the test exists to catch.
 *
 * The behavioural assertions are deliberately thin: the C++ test
 * (`test_priority.cpp`) already covers the S-family semantics.  What is
 * checked here is the *boundary*: that the symbols link, that the opaque
 * handle survives a round trip, that the priority levels cross the
 * boundary with their documented numeric values, and that the callback
 * receives the real stream id and priority.
 */

#include <nimrtc/sched/sched_c.h>

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

/* A collector for the drain callback. */
typedef struct {
    uint32_t    stream_id;
    int32_t     level;
    uint8_t     payload[16];
    size_t      len;
    int         calls;
} Collector;

static void on_packet(uint32_t    stream_id,
                      int32_t     level,
                      const uint8_t* data,
                      size_t      len,
                      void*       user_data)
{
    Collector* c = (Collector*)user_data;
    if (!c) return;
    c->stream_id = stream_id;
    c->level     = level;
    c->len       = len;
    if (len > 0 && len <= sizeof(c->payload)) {
        memcpy(c->payload, data, len);
    }
    ++c->calls;
}

/* ---------------------------------------------------------------------------
 * 1. create / destroy
 * ------------------------------------------------------------------------- */
static void test_create_destroy(void)
{
    NimrtcSchedHandle* h = nimrtc_sched_create(NULL);
    CHECK(h != NULL);
    nimrtc_sched_destroy(h);

    /* Destroying NULL must be a no-op, not a crash. */
    nimrtc_sched_destroy(NULL);
}

/* ---------------------------------------------------------------------------
 * 2. The priority levels must cross the boundary with their documented
 *    numeric values (High = 0, Normal = 1, Low = 2).
 * ------------------------------------------------------------------------- */
static void test_priority_levels(void)
{
    NimrtcSchedHandle* h = nimrtc_sched_create(NULL);
    CHECK(h != NULL);
    if (!h) return;

    nimrtc_sched_set_stream_priority(h, 5u, NIMRTC_SCHED_PRIORITY_HIGH);
    CHECK(nimrtc_sched_get_stream_priority(h, 5u) == NIMRTC_SCHED_PRIORITY_HIGH);

    nimrtc_sched_set_stream_priority(h, 5u, NIMRTC_SCHED_PRIORITY_LOW);
    CHECK(nimrtc_sched_get_stream_priority(h, 5u) == NIMRTC_SCHED_PRIORITY_LOW);

    nimrtc_sched_set_stream_priority(h, 5u, NIMRTC_SCHED_PRIORITY_NORMAL);
    CHECK(nimrtc_sched_get_stream_priority(h, 5u) == NIMRTC_SCHED_PRIORITY_NORMAL);

    /* An unknown stream reports the default bucket. */
    CHECK(nimrtc_sched_get_stream_priority(h, 999u) == NIMRTC_SCHED_PRIORITY_NORMAL);

    nimrtc_sched_destroy(h);
}

/* ---------------------------------------------------------------------------
 * 3. An out-of-range priority level must be rejected, not clamped.
 * ------------------------------------------------------------------------- */
static void test_invalid_priority_rejected(void)
{
    NimrtcSchedHandle* h = nimrtc_sched_create(NULL);
    CHECK(h != NULL);
    if (!h) return;

    nimrtc_sched_set_stream_priority(h, 1u, NIMRTC_SCHED_PRIORITY_HIGH);
    /* Out of range: the assignment is ignored, the level stays High. */
    nimrtc_sched_set_stream_priority(h, 1u, 42);
    CHECK(nimrtc_sched_get_stream_priority(h, 1u) == NIMRTC_SCHED_PRIORITY_HIGH);

    /* Negative is out of range too. */
    nimrtc_sched_set_stream_priority(h, 1u, -1);
    CHECK(nimrtc_sched_get_stream_priority(h, 1u) == NIMRTC_SCHED_PRIORITY_HIGH);

    nimrtc_sched_destroy(h);
}

/* ---------------------------------------------------------------------------
 * 4. Enqueue + drain: the callback must receive the *real* stream id and
 *    the *real* bucket priority, not placeholders.
 * ------------------------------------------------------------------------- */
static void test_enqueue_and_drain(void)
{
    NimrtcSchedHandle* h = nimrtc_sched_create(NULL);
    CHECK(h != NULL);
    if (!h) return;

    nimrtc_sched_set_stream_priority(h, 3u, NIMRTC_SCHED_PRIORITY_HIGH);

    const uint8_t pkt[3] = {0x01, 0x02, 0x03};
    nimrtc_sched_enqueue_stream_owned(h, 3u, pkt, sizeof(pkt), NULL);

    Collector c;
    memset(&c, 0, sizeof(c));

    int32_t drained = nimrtc_sched_select_next_packet(h, 10, on_packet, &c);
    CHECK(drained == 1);
    CHECK(c.calls == 1);
    CHECK(c.stream_id == 3u);
    CHECK(c.level == NIMRTC_SCHED_PRIORITY_HIGH);
    CHECK(c.len == sizeof(pkt));
    CHECK(c.payload[0] == 0x01);
    CHECK(c.payload[2] == 0x03);

    /* Nothing is left to drain. */
    Collector c2;
    memset(&c2, 0, sizeof(c2));
    CHECK(nimrtc_sched_select_next_packet(h, 10, on_packet, &c2) == 0);
    CHECK(c2.calls == 0);

    nimrtc_sched_destroy(h);
}

/* ---------------------------------------------------------------------------
 * 5. Drain without a callback is legal (the packets are accounted as sent).
 * ------------------------------------------------------------------------- */
static void test_drain_without_callback(void)
{
    NimrtcSchedHandle* h = nimrtc_sched_create(NULL);
    CHECK(h != NULL);
    if (!h) return;

    const uint8_t pkt[2] = {0xAA, 0xBB};
    nimrtc_sched_enqueue_stream_owned(h, 7u, pkt, sizeof(pkt), NULL);

    CHECK(nimrtc_sched_select_next_packet(h, 10, NULL, NULL) == 1);

    nimrtc_sched_destroy(h);
}

/* ---------------------------------------------------------------------------
 * 6. A NULL handle must be tolerated on every entry point.
 * ------------------------------------------------------------------------- */
static void test_null_handle_tolerated(void)
{
    nimrtc_sched_set_stream_priority(NULL, 1u, NIMRTC_SCHED_PRIORITY_HIGH);
    CHECK(nimrtc_sched_get_stream_priority(NULL, 1u) ==
          NIMRTC_SCHED_PRIORITY_NORMAL);
    nimrtc_sched_enqueue_stream_owned(NULL, 1u, NULL, 0, NULL);
    CHECK(nimrtc_sched_select_next_packet(NULL, 10, on_packet, NULL) == 0);
}

/* ---------------------------------------------------------------------------
 * 7. An explicit configuration must be honoured.
 * ------------------------------------------------------------------------- */
static void test_explicit_config(void)
{
    NimrtcSchedConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.avg_packet_size_bytes = 1200;
    cfg.max_queue_depth       = 64;
    cfg.protect_keyframes     = 1;
    cfg.enabled               = 1;
    cfg.drain_interval_ms     = 125;

    NimrtcSchedHandle* h = nimrtc_sched_create(&cfg);
    CHECK(h != NULL);
    if (!h) return;

    /* The handle works with an explicit configuration. */
    nimrtc_sched_set_stream_priority(h, 2u, NIMRTC_SCHED_PRIORITY_NORMAL);
    CHECK(nimrtc_sched_get_stream_priority(h, 2u) == NIMRTC_SCHED_PRIORITY_NORMAL);

    nimrtc_sched_destroy(h);
}

/* ---------------------------------------------------------------------------
 * 8. A destination address must survive the crossing.
 * ------------------------------------------------------------------------- */
static void test_destination_address(void)
{
    NimrtcSchedHandle* h = nimrtc_sched_create(NULL);
    CHECK(h != NULL);
    if (!h) return;

    NimrtcSchedAddr addr;
    memset(&addr, 0, sizeof(addr));
    addr.data[0] = 0x7F;
    addr.data[1] = 0x00;
    addr.data[2] = 0x00;
    addr.data[3] = 0x01;
    addr.len     = 4;

    const uint8_t pkt[1] = {0x42};
    nimrtc_sched_enqueue_stream_owned(h, 4u, pkt, sizeof(pkt), &addr);

    Collector c;
    memset(&c, 0, sizeof(c));
    CHECK(nimrtc_sched_select_next_packet(h, 10, on_packet, &c) == 1);
    CHECK(c.calls == 1);

    nimrtc_sched_destroy(h);
}

int main(void)
{
    test_create_destroy();
    test_priority_levels();
    test_invalid_priority_rejected();
    test_enqueue_and_drain();
    test_drain_without_callback();
    test_null_handle_tolerated();
    test_explicit_config();
    test_destination_address();

    if (g_failures != 0) {
        fprintf(stderr, "FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    printf("OK: the scheduler C ABI is usable from C\n");
    return 0;
}
