/**
 * @file fuzz_entry_sdp.cpp
 * @brief libFuzzer entry point for the SDP parser.
 *
 * The entry point lives in its own translation unit so that the
 * target library (`nimrtc_fuzz_targets`) stays free of the
 * `LLVMFuzzerTestOneInput` symbol — otherwise every libFuzzer
 * harness would collide on it when linking against the shared
 * library.
 */
#include "fuzz_targets.hpp"

#include <cstddef>
#include <cstdint>

#ifdef NIMRTC_FUZZING
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                       std::size_t size) {
    nimrtc::fuzz::run_sdp(data, size);
    return 0;
}
#endif // NIMRTC_FUZZING
