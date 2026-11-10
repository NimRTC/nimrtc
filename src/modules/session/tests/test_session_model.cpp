// modules/session/tests/test_session_model.cpp
// =============================================================================
// SessionModel tests — the `plugins::ISession` implementation.
//
// v1.2.0 — `docs/plan/session-capability-plan.md` N1 / T1.
//
// The properties pinned here are the ones the engine relies on when it
// turns the session state into an SDP media list:
//
//   * a mid is handed out once and never reused (OD-A: a removed track
//     keeps its m-line, so the mid must stay dead),
//   * a removed track leaves its transceiver alive but empty,
//   * a stopped transceiver keeps its mid and reports `inactive`.
// =============================================================================

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <nimrtc/session/session_model.hpp>

namespace {

using nimrtc::plugins::Direction;
using nimrtc::plugins::MediaKind;
using nimrtc::session::SessionModel;

// -----------------------------------------------------------------------------
// Tracks
// -----------------------------------------------------------------------------
TEST(SessionModel, AddTrackAndSnapshot) {
    SessionModel m;
    ASSERT_TRUE(m.add_track("a", MediaKind::kAudio, Direction::kSendRecv));
    ASSERT_TRUE(m.add_track("v", MediaKind::kVideo, Direction::kSendOnly));

    const auto tracks = m.tracks();
    ASSERT_EQ(tracks.size(), 2u);
    EXPECT_EQ(tracks[0].id, "a");
    EXPECT_EQ(tracks[0].kind, MediaKind::kAudio);
    EXPECT_EQ(tracks[1].id, "v");
    EXPECT_EQ(tracks[1].direction, Direction::kSendOnly);
}

TEST(SessionModel, DuplicateTrackIdIsRejected) {
    SessionModel m;
    EXPECT_TRUE(m.add_track("a", MediaKind::kAudio, Direction::kSendRecv));
    EXPECT_FALSE(m.add_track("a", MediaKind::kVideo, Direction::kSendRecv));
    EXPECT_EQ(m.tracks().size(), 1u);
}

TEST(SessionModel, RemoveTrackReportsExistence) {
    SessionModel m;
    ASSERT_TRUE(m.add_track("a", MediaKind::kAudio, Direction::kSendRecv));
    EXPECT_TRUE(m.remove_track("a"));
    EXPECT_FALSE(m.remove_track("a"));
    EXPECT_TRUE(m.tracks().empty());
}

TEST(SessionModel, SetTrackDirection) {
    SessionModel m;
    ASSERT_TRUE(m.add_track("a", MediaKind::kAudio, Direction::kSendRecv));
    EXPECT_TRUE(m.set_track_direction("a", Direction::kRecvOnly));
    EXPECT_EQ(m.tracks()[0].direction, Direction::kRecvOnly);
    EXPECT_FALSE(m.set_track_direction("nope", Direction::kRecvOnly));
}

// -----------------------------------------------------------------------------
// Transceivers — mid allocation and stability
// -----------------------------------------------------------------------------
TEST(SessionModel, TransceiverMidIsUnique) {
    SessionModel m;
    const std::string m0 = m.add_transceiver(MediaKind::kAudio, Direction::kSendRecv);
    const std::string m1 = m.add_transceiver(MediaKind::kVideo, Direction::kSendRecv);
    EXPECT_FALSE(m0.empty());
    EXPECT_FALSE(m1.empty());
    EXPECT_NE(m0, m1);
    EXPECT_EQ(m.transceivers().size(), 2u);
}

TEST(SessionModel, MidIsNeverReused) {
    SessionModel m;
    const std::string first = m.add_transceiver(MediaKind::kAudio, Direction::kSendRecv);
    ASSERT_FALSE(first.empty());
    ASSERT_TRUE(m.stop_transceiver(first));
    ASSERT_TRUE(m.remove_track("nothing"));  // no-op, keeps the flow linear

    // A brand-new transceiver must not take the dead mid.
    const std::string second = m.add_transceiver(MediaKind::kVideo, Direction::kSendRecv);
    EXPECT_NE(second, first);
}

TEST(SessionModel, AddTransceiverAtHonoursAFreeMid) {
    SessionModel m;
    const std::string mid = m.add_transceiver_at("1", MediaKind::kVideo, Direction::kSendRecv);
    EXPECT_EQ(mid, "1");
    ASSERT_EQ(m.transceivers().size(), 1u);
    EXPECT_EQ(m.transceivers()[0].mid, "1");
}

TEST(SessionModel, AddTransceiverAtFallsBackWhenTaken) {
    SessionModel m;
    ASSERT_EQ(m.add_transceiver_at("0", MediaKind::kAudio, Direction::kSendRecv), "0");
    const std::string second = m.add_transceiver_at("0", MediaKind::kVideo, Direction::kSendRecv);
    EXPECT_NE(second, "0");
    EXPECT_EQ(m.transceivers().size(), 2u);
}

TEST(SessionModel, AddTransceiverAtDoesNotCollideWithLaterAllocations) {
    SessionModel m;
    ASSERT_EQ(m.add_transceiver_at("7", MediaKind::kAudio, Direction::kSendRecv), "7");
    const std::string next = m.add_transceiver(MediaKind::kVideo, Direction::kSendRecv);
    EXPECT_NE(next, "7");
}

// -----------------------------------------------------------------------------
// Transceiver slots
// -----------------------------------------------------------------------------
TEST(SessionModel, AttachAndDetachTrack) {
    SessionModel m;
    const std::string mid = m.add_transceiver(MediaKind::kAudio, Direction::kSendRecv);
    ASSERT_TRUE(m.add_track("a", MediaKind::kAudio, Direction::kSendRecv));

    EXPECT_TRUE(m.set_transceiver_track(mid, "a", true));
    EXPECT_EQ(m.transceivers()[0].send_track_id, "a");

    EXPECT_TRUE(m.set_transceiver_track(mid, "", true));
    EXPECT_TRUE(m.transceivers()[0].send_track_id.empty());
}

TEST(SessionModel, AttachingAnUnknownTrackFails) {
    SessionModel m;
    const std::string mid = m.add_transceiver(MediaKind::kAudio, Direction::kSendRecv);
    EXPECT_FALSE(m.set_transceiver_track(mid, "ghost", true));
    EXPECT_TRUE(m.transceivers()[0].send_track_id.empty());
}

TEST(SessionModel, RemovingATrackClearsTheSlotButKeepsTheTransceiver) {
    SessionModel m;
    const std::string mid = m.add_transceiver(MediaKind::kAudio, Direction::kSendRecv);
    ASSERT_TRUE(m.add_track("a", MediaKind::kAudio, Direction::kSendRecv));
    ASSERT_TRUE(m.set_transceiver_track(mid, "a", true));

    ASSERT_TRUE(m.remove_track("a"));
    ASSERT_EQ(m.transceivers().size(), 1u);
    EXPECT_TRUE(m.transceivers()[0].send_track_id.empty());
}

// -----------------------------------------------------------------------------
// Transceiver direction / stop / restart
// -----------------------------------------------------------------------------
TEST(SessionModel, SetTransceiverDirection) {
    SessionModel m;
    const std::string mid = m.add_transceiver(MediaKind::kAudio, Direction::kSendRecv);
    EXPECT_TRUE(m.set_transceiver_direction(mid, Direction::kRecvOnly));
    EXPECT_EQ(m.transceivers()[0].direction, Direction::kRecvOnly);
    EXPECT_FALSE(m.set_transceiver_direction("nope", Direction::kRecvOnly));
}

TEST(SessionModel, StopMarksInactiveAndKeepsTheMid) {
    SessionModel m;
    const std::string mid = m.add_transceiver(MediaKind::kAudio, Direction::kSendRecv);
    ASSERT_TRUE(m.stop_transceiver(mid));

    ASSERT_EQ(m.transceivers().size(), 1u);
    EXPECT_TRUE(m.transceivers()[0].stopped);
    EXPECT_EQ(m.transceivers()[0].direction, Direction::kInactive);
    EXPECT_EQ(m.transceivers()[0].mid, mid);
}

TEST(SessionModel, RestartRestoresThePreviousDirection) {
    SessionModel m;
    const std::string mid = m.add_transceiver(MediaKind::kAudio, Direction::kSendOnly);
    ASSERT_TRUE(m.stop_transceiver(mid));
    ASSERT_TRUE(m.restart_transceiver(mid));

    EXPECT_FALSE(m.transceivers()[0].stopped);
    EXPECT_EQ(m.transceivers()[0].direction, Direction::kSendOnly);
}

TEST(SessionModel, StopAndRestartOfAnUnknownMidFail) {
    SessionModel m;
    EXPECT_FALSE(m.stop_transceiver("42"));
    EXPECT_FALSE(m.restart_transceiver("42"));
}

// -----------------------------------------------------------------------------
// Insertion order is stable
// -----------------------------------------------------------------------------
TEST(SessionModel, SnapshotsKeepInsertionOrder) {
    SessionModel m;
    ASSERT_TRUE(m.add_track("z", MediaKind::kAudio, Direction::kSendRecv));
    ASSERT_TRUE(m.add_track("a", MediaKind::kAudio, Direction::kSendRecv));
    const auto tracks = m.tracks();
    ASSERT_EQ(tracks.size(), 2u);
    EXPECT_EQ(tracks[0].id, "z");
    EXPECT_EQ(tracks[1].id, "a");
}

} // namespace
