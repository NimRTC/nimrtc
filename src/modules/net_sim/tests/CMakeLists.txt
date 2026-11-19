# modules/net_sim/tests/CMakeLists.txt
# =============================================================================
# The network-simulation module's unit tests.
#
# Two test binaries:
#
#   * `test_net_sim_impairment` — the impairment profile's *behaviour*:
#     the loss rate, the jitter spread, the reorder (the packet really
#     does arrive after its successors) and the rate limiter (the
#     serialization delay, and the bounded queue).
#
#   * `test_net_sim_nat_relay` — the relay's *translation*: two peers
#     that talk to each other through the relay never see each other's
#     real address, the traffic arrives with the source address the
#     peer was told to expect, and the impairments reach the wire.
#
# The relay test binds loopback sockets, so it runs on every platform
# the project supports.
# =============================================================================
nimrtc_add_test(${CMAKE_CURRENT_SOURCE_DIR}/test_net_sim_impairment.cpp
    DEPS nimrtc::net_sim)

nimrtc_add_test(${CMAKE_CURRENT_SOURCE_DIR}/test_net_sim_nat_relay.cpp
    DEPS nimrtc::net_sim)
