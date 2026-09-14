# plugins/tests/CMakeLists.txt
# =============================================================================
# Plugin seam tests — verify that each plugin interface (plugins::IFoo)
# integrates correctly with the core PluginRegistry and that
# SimpleFooFactory<T> templates work as documented.
#
# Each test target here is *seam-only*: it drives the registry / factory
# contract, NOT the implementation details of any concrete module. Algorithm
# behaviour tests live next to the module they exercise.
#
# The BWE seam test (test_bwe_plugin_seam) does still call
# `nimrtc::bwe::register_default_plugins()` to populate the registry — this
# is the cheapest way to round-trip through the registry contract. That
# means it links both `nimrtc::plugins` and `nimrtc::bwe`, which is
# intentional: the seam is between the registry (plugins) and the
# concrete adapter (bwe), not just between two header-only libraries.
# =============================================================================

nimrtc_add_test(${CMAKE_CURRENT_SOURCE_DIR}/test_bwe_plugin_seam.cpp
    DEPS nimrtc::plugins nimrtc::bwe)

nimrtc_add_test(${CMAKE_CURRENT_SOURCE_DIR}/test_scheduler_plugin_seam.cpp
    DEPS nimrtc::plugins)

# Zero-copy GPU surface path — verifies the GpuBuffer / GpuBufferPool /
# HwVideoBackendRegistry contracts independently of any platform SDK.
nimrtc_add_test(${CMAKE_CURRENT_SOURCE_DIR}/test_zero_copy_surface_path.cpp
    DEPS nimrtc::plugins)
