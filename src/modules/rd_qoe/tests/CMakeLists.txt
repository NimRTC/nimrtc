# modules/rd_qoe/tests/CMakeLists.txt
# =============================================================================
# Remote-desktop QoE module unit tests (plan J3 / J6).
#
# The acceptance criteria are covered by six suites in one binary:
#
#   - qoe_freeze_*             J3: the freeze count / duration accounting
#   - qoe_presented_fps_*      J3: the frame rate the *viewer* got
#   - qoe_latency_percentile_* J3: the end-to-end latency percentiles and
#                            the estimator's memory bound
#   - qoe_input_to_photon_*    J6: the input event -> photon pairing
#   - qoe_sharpness_*          J3: the sharpness proxy's ordering (the
#                            sharp picture must score above the blurred one)
#   - qoe_bound_*              J3: the collector's own boundedness
# =============================================================================
nimrtc_add_test(${CMAKE_CURRENT_SOURCE_DIR}/test_qoe_metrics.cpp
    DEPS nimrtc::rd_qoe)
