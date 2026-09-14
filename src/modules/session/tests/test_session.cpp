// modules/session/tests/test_session.cpp
// =============================================================================
// Session module tests — Track, Stream, Session, BUNDLE, rtcp-mux.
// =============================================================================

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nimrtc/session/track.hpp>
#include <nimrtc/session/session.hpp>

namespace {

using namespace nimrtc::session;

// -----------------------------------------------------------------------------
// Track — construction and identity
// -----------------------------------------------------------------------------
TEST(Track, DefaultDirectionIsSendRecv) {
    Track t("t1", MediaType::kAudio);
    EXPECT_EQ(t.id(), "t1");
    EXPECT_EQ(t.type(), MediaType::kAudio);
    EXPECT_EQ(t.direction(), TrackDirection::kSendRecv);
}

TEST(Track, AllMediaTypes) {
    EXPECT_NE(static_cast<int>(MediaType::kAudio), static_cast<int>(MediaType::kVideo));
    EXPECT_NE(static_cast<int>(MediaType::kVideo), static_cast<int>(MediaType::kData));
}

TEST(Track, AllDirections) {
    EXPECT_NE(static_cast<int>(TrackDirection::kSendOnly),
              static_cast<int>(TrackDirection::kRecvOnly));
    EXPECT_NE(static_cast<int>(TrackDirection::kSendRecv),
              static_cast<int>(TrackDirection::kInactive));
}

// -----------------------------------------------------------------------------
// Track — direction
// -----------------------------------------------------------------------------
TEST(Track, DirectionRoundTrip) {
    Track t("t1", MediaType::kVideo, TrackDirection::kSendOnly);
    EXPECT_EQ(t.direction(), TrackDirection::kSendOnly);

    t.set_direction(TrackDirection::kRecvOnly);
    EXPECT_EQ(t.direction(), TrackDirection::kRecvOnly);

    t.set_direction(TrackDirection::kInactive);
    EXPECT_EQ(t.direction(), TrackDirection::kInactive);
}

// -----------------------------------------------------------------------------
// Track — SSRC
// -----------------------------------------------------------------------------
TEST(Track, SsrcAbsentByDefault) {
    Track t("t1", MediaType::kAudio);
    EXPECT_FALSE(t.has_ssrc());
    EXPECT_EQ(t.ssrc(), 0u);
}

TEST(Track, SsrcRoundTrip) {
    Track t("t1", MediaType::kAudio);
    t.set_ssrc(0xDEADBEEF);
    EXPECT_TRUE(t.has_ssrc());
    EXPECT_EQ(t.ssrc(), 0xDEADBEEFu);

    // Setting again is allowed.
    t.set_ssrc(0x12345678);
    EXPECT_TRUE(t.has_ssrc());
    EXPECT_EQ(t.ssrc(), 0x12345678u);
}

// -----------------------------------------------------------------------------
// Track — BUNDLE
// -----------------------------------------------------------------------------
TEST(Track, BundledFalseByDefault) {
    Track t("t1", MediaType::kVideo);
    EXPECT_FALSE(t.bundled());
}

TEST(Track, BundledRoundTrip) {
    Track t("t1", MediaType::kVideo);
    t.set_bundled(true);
    EXPECT_TRUE(t.bundled());

    t.set_bundled(false);
    EXPECT_FALSE(t.bundled());
}

// -----------------------------------------------------------------------------
// Stream — construction
// -----------------------------------------------------------------------------
TEST(Stream, ConstructionId) {
    Stream s("stream-1");
    EXPECT_EQ(s.id(), "stream-1");
}

TEST(Stream, EmptyByDefault) {
    Stream s("s1");
    EXPECT_TRUE(s.tracks().empty());
}

// -----------------------------------------------------------------------------
// Stream — add / remove track
// -----------------------------------------------------------------------------
TEST(Stream, AddTrack) {
    Stream s("s1");
    auto t = std::make_shared<Track>("audio-1", MediaType::kAudio);
    s.add_track(t);

    auto tracks = s.tracks();
    ASSERT_EQ(tracks.size(), 1u);
    EXPECT_EQ(tracks[0]->id(), "audio-1");
}

TEST(Stream, AddNullTrackIsNoOp) {
    Stream s("s1");
    s.add_track(nullptr);
    EXPECT_TRUE(s.tracks().empty());
}

TEST(Stream, RemoveTrack) {
    Stream s("s1");
    auto t1 = std::make_shared<Track>("t1", MediaType::kAudio);
    auto t2 = std::make_shared<Track>("t2", MediaType::kVideo);
    s.add_track(t1);
    s.add_track(t2);

    s.remove_track("t1");
    auto tracks = s.tracks();
    ASSERT_EQ(tracks.size(), 1u);
    EXPECT_EQ(tracks[0]->id(), "t2");
}

TEST(Stream, RemoveNonexistentIsNoOp) {
    Stream s("s1");
    auto t = std::make_shared<Track>("t1", MediaType::kAudio);
    s.add_track(t);
    s.remove_track("does-not-exist");
    EXPECT_EQ(s.tracks().size(), 1u);
}

TEST(Stream, TrackById) {
    Stream s("s1");
    auto t = std::make_shared<Track>("video-1", MediaType::kVideo);
    s.add_track(t);

    EXPECT_EQ(s.track_by_id("video-1"), t);
    EXPECT_EQ(s.track_by_id("missing"), nullptr);
}

// -----------------------------------------------------------------------------
// Stream — multiple tracks
// -----------------------------------------------------------------------------
TEST(Stream, MultipleTracks) {
    Stream s("s1");
    s.add_track(std::make_shared<Track>("a1", MediaType::kAudio));
    s.add_track(std::make_shared<Track>("a2", MediaType::kAudio));
    s.add_track(std::make_shared<Track>("v1", MediaType::kVideo));

    auto tracks = s.tracks();
    EXPECT_EQ(tracks.size(), 3u);
}

// -----------------------------------------------------------------------------
// Session — construction
// -----------------------------------------------------------------------------
TEST(Session, Construction) {
    Session sess;
    EXPECT_TRUE(sess.streams().empty());
    EXPECT_FALSE(sess.rtcp_mux());
    EXPECT_TRUE(sess.bundled_track_ids().empty());
}

// -----------------------------------------------------------------------------
// Session — rtcp-mux
// -----------------------------------------------------------------------------
TEST(Session, RtcpMuxDefaultFalse) {
    Session sess;
    EXPECT_FALSE(sess.rtcp_mux());
}

TEST(Session, RtcpMuxRoundTrip) {
    Session sess;
    sess.set_rtcp_mux(true);
    EXPECT_TRUE(sess.rtcp_mux());

    sess.set_rtcp_mux(false);
    EXPECT_FALSE(sess.rtcp_mux());
}

// -----------------------------------------------------------------------------
// Session — add / remove stream
// -----------------------------------------------------------------------------
TEST(Session, AddStream) {
    Session sess;
    auto s = std::make_shared<Stream>("stream-1");
    sess.add_stream(s);

    auto streams = sess.streams();
    ASSERT_EQ(streams.size(), 1u);
    EXPECT_EQ(streams[0]->id(), "stream-1");
}

TEST(Session, AddNullStreamIsNoOp) {
    Session sess;
    sess.add_stream(nullptr);
    EXPECT_TRUE(sess.streams().empty());
}

TEST(Session, RemoveStream) {
    Session sess;
    auto s1 = std::make_shared<Stream>("s1");
    auto s2 = std::make_shared<Stream>("s2");
    sess.add_stream(s1);
    sess.add_stream(s2);

    sess.remove_stream("s1");
    auto streams = sess.streams();
    ASSERT_EQ(streams.size(), 1u);
    EXPECT_EQ(streams[0]->id(), "s2");
}

// -----------------------------------------------------------------------------
// Session — BUNDLE
// -----------------------------------------------------------------------------
TEST(Session, BundleTracksEmptyByDefault) {
    Session sess;
    EXPECT_TRUE(sess.bundled_track_ids().empty());
}

TEST(Session, BundleTracksRoundTrip) {
    Session sess;
    sess.bundle_tracks({"audio-1", "video-1"});

    auto ids = sess.bundled_track_ids();
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_TRUE(std::count(ids.begin(), ids.end(), "audio-1"));
    EXPECT_TRUE(std::count(ids.begin(), ids.end(), "video-1"));
}

TEST(Session, BundleTracksReplacesPrevious) {
    Session sess;
    sess.bundle_tracks({"a", "b"});
    sess.bundle_tracks({"c"});

    auto ids = sess.bundled_track_ids();
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], "c");
}

TEST(Session, BundleTracksSingle) {
    Session sess;
    sess.bundle_tracks({"audio-only"});
    auto ids = sess.bundled_track_ids();
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], "audio-only");
}

// -----------------------------------------------------------------------------
// Session — end-to-end: stream + track + BUNDLE
// -----------------------------------------------------------------------------
TEST(Session, EndToEnd) {
    Session sess;

    // Create two streams: one audio, one video.
    auto audio_stream = std::make_shared<Stream>("audio-stream");
    auto video_stream = std::make_shared<Stream>("video-stream");

    auto audio_track = std::make_shared<Track>("audio-1", MediaType::kAudio, TrackDirection::kSendOnly);
    audio_track->set_ssrc(0x11111111);
    audio_track->set_bundled(true);

    auto video_track = std::make_shared<Track>("video-1", MediaType::kVideo, TrackDirection::kSendRecv);
    video_track->set_ssrc(0x22222222);
    video_track->set_bundled(true);

    audio_stream->add_track(audio_track);
    video_stream->add_track(video_track);

    sess.add_stream(std::move(audio_stream));
    sess.add_stream(std::move(video_stream));

    // BUNDLE the tracks.
    sess.bundle_tracks({"audio-1", "video-1"});
    sess.set_rtcp_mux(true);

    // Verify stream list.
    auto streams = sess.streams();
    ASSERT_EQ(streams.size(), 2u);

    // Verify tracks reachable via streams.
    auto audio_s = sess.streams()[0]->track_by_id("audio-1");
    ASSERT_NE(audio_s, nullptr);
    EXPECT_TRUE(audio_s->has_ssrc());
    EXPECT_EQ(audio_s->ssrc(), 0x11111111u);
    EXPECT_TRUE(audio_s->bundled());
    EXPECT_EQ(audio_s->direction(), TrackDirection::kSendOnly);

    auto video_s = sess.streams()[1]->track_by_id("video-1");
    ASSERT_NE(video_s, nullptr);
    EXPECT_TRUE(video_s->bundled());
    EXPECT_EQ(video_s->direction(), TrackDirection::kSendRecv);

    // Verify BUNDLE / rtcp-mux state.
    auto bundle_ids = sess.bundled_track_ids();
    ASSERT_EQ(bundle_ids.size(), 2u);
    EXPECT_TRUE(sess.rtcp_mux());
}

} // namespace
