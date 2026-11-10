# modules/session/tests/CMakeLists.txt
# =============================================================================
# Session module unit tests.
# Built via nimrtc_add_test() defined in cmake/NimRTCTest.cmake.
# =============================================================================
nimrtc_add_test(${CMAKE_CURRENT_SOURCE_DIR}/test_session.cpp
    DEPS nimrtc::session)

# v1.2.0 (session capability plan N1 / T1): the `plugins::ISession`
# implementation the engine drives for renegotiation.
nimrtc_add_test(${CMAKE_CURRENT_SOURCE_DIR}/test_session_model.cpp
    DEPS nimrtc::session)
