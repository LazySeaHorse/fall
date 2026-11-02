#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Song.h"

using namespace fall;
using Catch::Matchers::WithinAbs;

static Song load (const char* name)
{
    Song s;
    juce::String err;
    const auto file = juce::File (FALL_TEST_DATA_DIR).getChildFile (name);
    REQUIRE (loadMidiFile (file, s, err));
    return s;
}

TEST_CASE ("MIDI import honours the tempo map", "[song]")
{
    auto s = load ("two_tracks.mid");
    REQUIRE (s.notes.size() == 11);

    std::vector<double> rh;
    for (auto& n : s.notes) if (n.hand == Hand::Right) rh.push_back (n.start);
    REQUIRE (rh.size() == 6);
    CHECK_THAT (rh[1], WithinAbs (0.5, 1e-6));
    CHECK_THAT (rh[4], WithinAbs (2.0, 1e-6)); // tempo halves after beat 4
    CHECK_THAT (rh[5], WithinAbs (3.0, 1e-6));
    CHECK_THAT (s.length, WithinAbs (4.0, 1e-6));
    CHECK (s.title == "two_tracks");
}

TEST_CASE ("Hands come from named tracks", "[song]")
{
    auto s = load ("two_tracks.mid");
    CHECK (s.handsFromTracks);
    for (auto& n : s.notes)
        CHECK ((n.hand == Hand::Right) == (n.pitch >= 72));
}

TEST_CASE ("Single track files split at the split point", "[song]")
{
    auto s = load ("single_track.mid");
    CHECK_FALSE (s.handsFromTracks);
    for (auto& n : s.notes)
        CHECK ((n.hand == Hand::Right) == (n.pitch >= 60));

    ImportOptions o; o.splitPitch = 55;
    assignHands (s, o);
    for (auto& n : s.notes)
        CHECK ((n.hand == Hand::Right) == (n.pitch >= 55));
}

TEST_CASE ("Sustain pedal events are kept", "[song]")
{
    auto s = load ("two_tracks.mid");
    REQUIRE (s.controls.size() == 2);
    CHECK (s.controls[0].controller == 64);
    CHECK (s.controls[0].value == 127);
    CHECK_THAT (s.controls[1].time, WithinAbs (1.95, 1e-6));
}

TEST_CASE ("Chord groups per hand", "[song]")
{
    auto s = load ("two_tracks.mid");
    auto left = buildGroups (s, HandLeft);
    REQUIRE (left.size() == 3);
    CHECK (left[0].pitches.count() == 2);
    CHECK (left[0].pitches.test (48));
    CHECK (left[0].pitches.test (55));

    auto both = buildGroups (s, HandBoth);
    REQUIRE (both.size() == 6);
    CHECK (both[0].pitches.count() == 3);
}

TEST_CASE ("Garbage is rejected", "[song]")
{
    auto tmp = juce::File::createTempFile (".mid");
    tmp.replaceWithText ("not midi");
    Song s; juce::String err;
    CHECK_FALSE (loadMidiFile (tmp, s, err));
    CHECK (err.isNotEmpty());
    tmp.deleteFile();
}
