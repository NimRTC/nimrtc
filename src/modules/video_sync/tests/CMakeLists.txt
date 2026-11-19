# modules/video_sync/tests/CMakeLists.txt
# ----------------------------------------------------------------------------
# The render clock's unit tests (v1.3 plan Q6).
#
# The clock is a pure function of the skew plus its own counters: no
# clock, no allocation, no thread, no GPU, no window-system type.  So
# these cases run on every platform, including the CI legs that have no
# display at all.
# ----------------------------------------------------------------------------
nimrtc_add_test(${CMAKE_CURRENT_SOURCE_DIR}/test_render_clock.cpp
    DEPS nimrtc::video_sync)

# The `plugins::IVideoSyncController` adapter -- the seam the engine
# drives, and the registry slot it resolves through.
#
# NOTE: this binary calls `video_sync::register_default_plugins()`
# directly rather than `core::register_all_default_plugins()`.  The
# latter walks the *whole* default-registrar table, which references
# every built-in module's entry point -- so a module-level test that
# called it would have to link every module in the tree.  The rd_qoe
# plugin test uses the same shape.
nimrtc_add_test(${CMAKE_CURRENT_SOURCE_DIR}/test_video_sync_plugin.cpp
    DEPS nimrtc::video_sync nimrtc::core)
