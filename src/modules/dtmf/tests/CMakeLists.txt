# modules/dtmf/tests/CMakeLists.txt
# =============================================================================
# RFC 4733 (telephone-event) codec module unit tests.
#
# The wire format is the whole contract of this module, so the tests
# pin it directly: the position of the end-of-event bit, the reserved
# bit, the volume field and — the detail that is easy to get wrong —
# the unit of the duration field (8000 Hz timestamp units, not
# milliseconds).
# =============================================================================
nimrtc_add_test(${CMAKE_CURRENT_SOURCE_DIR}/test_dtmf_codec.cpp
    DEPS nimrtc::dtmf)
