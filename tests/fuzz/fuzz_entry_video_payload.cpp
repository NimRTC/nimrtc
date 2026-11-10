/**
 * @file fuzz_entry_video_payload.cpp
 * @brief libFuzzer entry point for the video payload parsers and the
 *        FU-A depacketizer.
 *
 * See fuzz_entry_sdp.cpp for why the entry point is a separate
 * translation unit.
 */
#include "fuzz_targets.hpp"

#include <cstddef>
#include <cstdint>

#ifdef NIMRTC_FUZZING
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                       std::size_t size) {
    nimrtc::fuzz::run_video_payload(data, size);
    return 0;
}
#endif // NIMRTC_FUZZING
