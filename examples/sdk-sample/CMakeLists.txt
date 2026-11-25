cmake_minimum_required(VERSION 3.20)
project(nimrtc_sdk_sample LANGUAGES CXX)

# The sample is a host project: it consumes the *installed* engine through
# the public package (spec §五).  It builds against an install root
#   cmake -S examples/sdk-sample -B build/sdk-sample \
#     -DCMAKE_PREFIX_PATH=<install-root>
# (cxx_std_20 comes from nimrtc::engine's INTERFACE_COMPILE_FEATURES).
find_package(NimRTC 1.0 REQUIRED CONFIG)

add_executable(sdk-sample sdk-sample.cpp)
target_link_libraries(sdk-sample PRIVATE nimrtc::engine)

# The Windows bundle links the dynamic CRT (MultiThreadedDLL); a host that
# compiles with the default (static) runtime cannot load the engine DLL.
# The engine header carries UTF-8 (Chinese) doc comments, so MSVC must read
# it as UTF-8; under a legacy code page the comments mis-parse and members
# drop out (a host-side mirror of NimRTCOptions.cmake's /utf-8).
if(MSVC)
  set_property(TARGET sdk-sample PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
  target_compile_options(sdk-sample PRIVATE /utf-8)
endif()
