/**
 * @file track.cpp
 * @brief Track implementation.
 */

#include <nimrtc/session/track.hpp>

namespace nimrtc::session {

Track::Track(std::string id, MediaType type, TrackDirection dir)
    : id_(std::move(id)), type_(type), direction_(dir) {}

Track::~Track() = default;

} // namespace nimrtc::session
