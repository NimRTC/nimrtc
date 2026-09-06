/**
 * @file session.cpp
 * @brief Stream and Session implementation.
 */

#include <algorithm>
#include <shared_mutex>

#include <nimrtc/session/session.hpp>

namespace nimrtc::session {

// ===========================================================================
// Stream
// ===========================================================================
Stream::Stream(std::string id) : impl_(std::make_unique<Impl>(std::move(id))) {}

Stream::~Stream() = default;

void Stream::add_track(std::shared_ptr<Track> track) {
    if (!track) return;
    std::unique_lock<std::shared_mutex> lock(impl_->mu);
    impl_->tracks_.emplace(track->id(), std::move(track));
}

void Stream::remove_track(const std::string& track_id) {
    std::unique_lock<std::shared_mutex> lock(impl_->mu);
    impl_->tracks_.erase(track_id);
}

std::vector<std::shared_ptr<Track>> Stream::tracks() const {
    std::shared_lock<std::shared_mutex> lock(impl_->mu);
    std::vector<std::shared_ptr<Track>> result;
    result.reserve(impl_->tracks_.size());
    for (const auto& [_, t] : impl_->tracks_) {
        result.push_back(t);
    }
    return result;
}

std::shared_ptr<Track> Stream::track_by_id(const std::string& id) const {
    std::shared_lock<std::shared_mutex> lock(impl_->mu);
    auto it = impl_->tracks_.find(id);
    if (it != impl_->tracks_.end()) return it->second;
    return nullptr;
}

// ===========================================================================
// Session
// ===========================================================================
Session::Session() : impl_(std::make_unique<Impl>()) {}

Session::~Session() = default;

void Session::bundle_tracks(const std::vector<std::string>& track_ids) {
    std::unique_lock<std::shared_mutex> lock(impl_->mu);
    impl_->bundled_track_ids_.clear();
    for (const auto& id : track_ids) {
        impl_->bundled_track_ids_.insert(id);
    }
}

std::vector<std::string> Session::bundled_track_ids() const {
    std::shared_lock<std::shared_mutex> lock(impl_->mu);
    return {impl_->bundled_track_ids_.begin(), impl_->bundled_track_ids_.end()};
}

void Session::add_stream(std::shared_ptr<Stream> stream) {
    if (!stream) return;
    std::unique_lock<std::shared_mutex> lock(impl_->mu);
    impl_->streams_.emplace(stream->id(), std::move(stream));
}

void Session::remove_stream(const std::string& stream_id) {
    std::unique_lock<std::shared_mutex> lock(impl_->mu);
    impl_->streams_.erase(stream_id);
}

std::vector<std::shared_ptr<Stream>> Session::streams() const {
    std::shared_lock<std::shared_mutex> lock(impl_->mu);
    std::vector<std::shared_ptr<Stream>> result;
    result.reserve(impl_->streams_.size());
    for (const auto& [_, s] : impl_->streams_) {
        result.push_back(s);
    }
    return result;
}

} // namespace nimrtc::session
