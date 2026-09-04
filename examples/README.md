# examples/

This directory contains sample programs that demonstrate NimRTC's APIs.

Each example is self-contained and may depend on one or more NimRTC modules.
Examples are built only when `NIMRTC_BUILD_EXAMPLES=ON` is set at configure time.

## Current examples

| Example | Status | Description |
|---------|--------|-------------|
| — | P1 | Chrome interop demo (Opus audio loopback) |
| — | P1 | Video sender / receiver spike |
| — | P2 | PCM tap + priority dispatch demo |

## Adding an example

```cmake
# examples/CMakeLists.txt — add one line per example
nimrtc_add_example(example_name
    PATH    examples/example_name.cpp
    DEPS    nimrtc::rtp
    LIBS    nimrtc_vendor_libopus)
```

See `cmake/NimRTCTest.cmake` for the `nimrtc_add_example()` macro definition
(P1 when the first example lands).
