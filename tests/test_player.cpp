#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Player.h"

using namespace fall;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr double kRate = 1000.0; // 1 sample = 1 ms keeps the arithmetic readable
constexpr int    kBlock = 100;

struct Ev { int64_t sample; bool on; int pitch; int cc = -1; int value = 0; };

std::shared_ptr<Song> makeSong()
{
    // Right hand: 72 @0, 74 @0.5, 76 @1.0 ; Left hand chord 48+55 @0, 43 @1.0
    auto s = std::make_shared<Song>();
    auto add = [&] (int p, double a, double b, Hand h) {
        Note n; n.pitch = p; n.start = a; n.end = b; n.hand = h; s->notes.push_back (n);
    };
    add (48, 0.0, 0.9, Hand::Left);
    add (55, 0.0, 0.9, Hand::Left);
    add (72, 0.0, 0.4, Hand::Right);
    add (74, 0.5, 0.9, Hand::Right);
    add (43, 1.0, 1.9, Hand::Left);
    add (76, 1.0, 1.4, Hand::Right);
    s->controls.push_back ({ 0.0, 1, 64, 127 });
    s->controls.push_back ({ 0.95, 1, 64, 0 });
    s->length = 1.9;
    return s;
}

struct Rig
{
    Player p;
    std::vector<Ev> events;
    int64_t pos = 0;

    explicit Rig (std::shared_ptr<Song> s = makeSong())
    {
        p.setSong (s);
        p.seek (0.0);
    }

    void run (double seconds)
    {
        const int blocks = (int) std::lround (seconds * kRate / kBlock);
        for (int i = 0; i < blocks; ++i)
        {
            juce::MidiBuffer buf;
            p.render (buf, kBlock, kRate);
            for (const auto m : buf)
            {
                const auto& msg = m.getMessage();
                if (msg.isNoteOn()) events.push_back ({ pos + m.samplePosition, true, msg.getNoteNumber() });
                else if (msg.isNoteOff()) events.push_back ({ pos + m.samplePosition, false, msg.getNoteNumber() });
                else if (msg.isController())
                    events.push_back ({ pos + m.samplePosition, false, -1, msg.getControllerNumber(), msg.getControllerValue() });
            }
            pos += kBlock;
        }
    }

    int64_t onAt (int pitch, int nth = 0) const
    {
        for (auto& e : events)
            if (e.on && e.pitch == pitch && nth-- == 0) return e.sample;
        return -1;
    }
    int64_t offAt (int pitch) const
    {
        for (auto& e : events)
            if (! e.on && e.pitch == pitch) return e.sample;
        return -1;
    }
    int countOn (int pitch) const
    {
        int c = 0;
        for (auto& e : events) c += (e.on && e.pitch == pitch);
        return c;
    }
};

PitchSet ps (std::initializer_list<int> pitches)
{
    PitchSet s;
    for (int p : pitches) s.set ((size_t) p);
    return s;
}
} // namespace

TEST_CASE ("Freeplay plays notes on time", "[player]")
{
    Rig r;
    r.p.setPlaying (true);
    r.run (3.5);

    CHECK (r.onAt (72) == 0);
    CHECK (r.onAt (74) == 500);
    CHECK (r.onAt (76) == 1000);
    CHECK (r.offAt (72) == 400);
    CHECK (r.offAt (48) == 900);
    CHECK_FALSE (r.p.isPlaying()); // stopped at the end
}

TEST_CASE ("Speed scales real time", "[player]")
{
    Rig r;
    r.p.setSpeed (0.5);
    r.p.setPlaying (true);
    r.run (3.0);
    CHECK (std::abs (r.onAt (74) - 1000) <= 1); // +-1 sample of float drift is fine
    CHECK (std::abs (r.onAt (76) - 2000) <= 1);
}

TEST_CASE ("Paused player emits nothing and keeps time", "[player]")
{
    Rig r;
    r.run (1.0);
    CHECK (r.countOn (72) == 0);
    CHECK (r.p.getTime() == 0.0);
}

TEST_CASE ("Learn mode waits for the right notes", "[player][learn]")
{
    Rig r;
    r.p.setMode (Mode::Learn);
    r.p.setPracticeHands (HandRight);
    r.p.setPlaying (true);

    r.run (1.0);
    CHECK (r.p.isWaiting());
    CHECK (r.p.getTime() == 0.0);
    CHECK (r.p.expectedPitches() == ps ({ 72 }));
    CHECK (r.countOn (72) == 0); // the chord sounds only once it's played
    CHECK (r.countOn (48) == 0); // ...and the other hand waits with it

    r.p.notePlayed (ps ({ 60 })); // wrong note
    r.run (0.2);
    CHECK (r.p.isWaiting());

    r.p.notePlayed (ps ({ 72 }));
    CHECK_FALSE (r.p.isWaiting());
    r.run (0.6);
    CHECK (r.onAt (72) == 1200);
    CHECK (r.onAt (48) == 1200);
    CHECK (r.p.isWaiting()); // next chord at 0.5
    CHECK_THAT (r.p.getTime(), WithinAbs (0.5, 1e-9));
}

TEST_CASE ("Learn mode accumulates chord notes", "[player][learn]")
{
    Rig r;
    r.p.setMode (Mode::Learn);
    r.p.setPracticeHands (HandLeft);
    r.p.setPlaying (true);
    r.run (0.2);
    CHECK (r.p.expectedPitches() == ps ({ 48, 55 }));

    r.p.notePlayed (ps ({ 48 }));
    CHECK (r.p.isWaiting());
    CHECK (r.p.satisfiedPitches() == ps ({ 48 }));
    r.p.notePlayed (ps ({ 55, 90 }));
    CHECK_FALSE (r.p.isWaiting());
    CHECK (r.p.getGroupsCompleted() == 1);
}

TEST_CASE ("Notes played slightly early count", "[player][learn]")
{
    Rig r;
    r.p.setMode (Mode::Learn);
    r.p.setPracticeHands (HandRight);
    r.p.setEarlyWindow (0.25);
    r.p.seek (-0.5);
    r.p.setPlaying (true);

    r.run (0.2); // t = -0.3: too early
    r.p.notePlayed (ps ({ 72 }));
    CHECK (r.p.expectedPitches() == ps ({ 72 }));

    r.run (0.1); // t = -0.2: inside the window
    r.p.notePlayed (ps ({ 72 }));
    CHECK (r.p.expectedPitches() == ps ({ 74 }));
    r.run (0.3);
    CHECK (r.onAt (72) == 500); // played on time, never stopped
}

// 14 single-note chords, one every 0.5 s, pitches 60..73.
std::shared_ptr<Song> makeChordRun()
{
    auto s = std::make_shared<Song>();
    for (int i = 0; i < 14; ++i)
    {
        Note n; n.pitch = 60 + i; n.start = 0.5 * i; n.end = 0.5 * i + 0.3; n.hand = Hand::Right;
        s->notes.push_back (n);
    }
    s->length = 7.0;
    return s;
}

// Learn mode on makeChordRun(), waiting at chord 0.
struct ChordRun : Rig
{
    explicit ChordRun (LearnStyle style) : Rig (makeChordRun())
    {
        p.setMode (Mode::Learn);
        p.setLearnStyle (style);
        p.setPlaying (true);
        run (0.1);
    }

    void play() { p.notePlayed (p.expectedPitches()); run (0.1); }
    void playChords (int n) { for (int i = 0; i < n; ++i) { play(); run (0.5); } }
};

TEST_CASE ("Learn mode waits forever in wait-for-every-note style", "[player][learn][forgiving]")
{
    ChordRun r (LearnStyle::WaitEveryNote);
    r.playChords (9);
    REQUIRE (r.p.isWaiting());
    const int done = r.p.getGroupsCompleted();
    r.run (10.0);
    CHECK (r.p.isWaiting());
    CHECK (r.p.getGroupsCompleted() == done);
    CHECK (r.p.getGroupsSkipped() == 0);
}

TEST_CASE ("Forgiving learn mode skips a chord after 0.5 s once 8 of 10 were played", "[player][learn][forgiving]")
{
    ChordRun r (LearnStyle::MostlyRight);
    r.playChords (8);
    REQUIRE (r.p.isWaiting());
    CHECK (r.p.expectedPitches() == ps ({ 68 }));

    r.run (0.3);
    CHECK (r.p.getGroupsSkipped() == 0); // not yet 0.5 s
    CHECK (r.p.isWaiting());
    r.run (0.3);
    CHECK (r.p.getGroupsSkipped() == 1);
    CHECK (r.p.getGroupsCompleted() == 8);
    CHECK (r.p.lastSkippedPitches() == ps ({ 68 }));
    CHECK (r.p.expectedPitches() == ps ({ 69 })); // the song moved on to the next chord
}

TEST_CASE ("Forgiving learn mode never skips two chords in a row", "[player][learn][forgiving]")
{
    ChordRun r (LearnStyle::MostlyRight);
    r.playChords (9);
    r.run (1.0);
    CHECK (r.p.getGroupsSkipped() == 1);
    r.run (5.0); // the next chord is not forgiven right after a miss
    CHECK (r.p.getGroupsSkipped() == 1);
    CHECK (r.p.isWaiting());

    r.play(); // a hit makes the one after skippable again
    r.run (1.0);
    CHECK (r.p.getGroupsSkipped() == 2);
}

TEST_CASE ("Forgiving learn mode needs 8 hits, also right after a seek", "[player][learn][forgiving]")
{
    ChordRun r (LearnStyle::MostlyRight);
    r.playChords (7);
    r.run (5.0);
    CHECK (r.p.getGroupsSkipped() == 0); // only 7 played so far

    r.p.seek (0.0); // history starts over
    r.playChords (7);
    r.run (5.0);
    CHECK (r.p.getGroupsSkipped() == 0);
    r.play(); // the 8th hit since the seek
    r.run (1.0);
    CHECK (r.p.getGroupsSkipped() == 1);
}

TEST_CASE ("Wait-for-every-note ignores keys that are not the chord", "[player][learn][whileplaying]")
{
    ChordRun r (LearnStyle::WaitEveryNote);
    r.p.heardSomething();
    r.run (5.0);
    CHECK (r.p.isWaiting());
    CHECK (r.p.getGroupsPassed() == 0);
}

TEST_CASE ("Keep going while I play passes chords as long as onsets keep coming", "[player][learn][whileplaying]")
{
    ChordRun r (LearnStyle::WhilePlaying);
    REQUIRE (r.p.isWaiting()); // nothing heard yet
    r.p.heardSomething();      // any key starts it
    for (int i = 0; i < 10; ++i)
    {
        r.run (0.5);
        r.p.heardSomething();
    }
    CHECK_FALSE (r.p.isWaiting());
    CHECK_THAT (r.p.getTime(), WithinAbs (5.0, 0.01));
    CHECK (r.p.getGroupsPassed() >= 10);
    CHECK (r.p.getGroupsSkipped() == 0); // passing is not missing
    CHECK (r.p.getGroupsCompleted() == 0);
}

TEST_CASE ("Keep going while I play stops 1.5 s after the last key", "[player][learn][whileplaying]")
{
    ChordRun r (LearnStyle::WhilePlaying);
    r.p.heardSomething();
    r.run (10.0);
    REQUIRE (r.p.isWaiting());
    CHECK (r.p.getTime() > 1.0);
    CHECK (r.p.getTime() < 2.5);
    CHECK (r.p.getGroupsPassed() >= 2);
    CHECK (r.p.getGroupsPassed() <= 4);
    const double stoppedAt = r.p.getTime();
    r.run (5.0);
    CHECK (r.p.getTime() == stoppedAt);
}

TEST_CASE ("Keep going while I play never stops in a slow passage with one key per chord", "[player][learn][whileplaying]")
{
    auto s = std::make_shared<Song>();
    for (int i = 0; i < 5; ++i)
    {
        Note n; n.pitch = 60 + i; n.start = 3.0 * i; n.end = 3.0 * i + 1.0; n.hand = Hand::Right;
        s->notes.push_back (n);
    }
    s->length = 13.0;
    Rig r (s);
    r.p.setMode (Mode::Learn);
    r.p.setLearnStyle (LearnStyle::WhilePlaying);
    r.p.setPlaying (true);
    r.run (0.1);
    r.p.heardSomething(); // the first chord
    for (int i = 0; i < 4; ++i)
    {
        r.run (3.05); // the next chord comes and goes, then a key a bit after it
        CHECK_FALSE (r.p.isWaiting());
        r.p.heardSomething();
    }
    CHECK (r.p.getGroupsPassed() == 5);

    // Without further keys it stops at the second chord after the last one heard.
    Rig q (s);
    q.p.setMode (Mode::Learn);
    q.p.setLearnStyle (LearnStyle::WhilePlaying);
    q.p.setPlaying (true);
    q.run (0.1);
    q.p.heardSomething();
    q.run (10.0);
    CHECK (q.p.isWaiting());
    CHECK (q.p.getGroupsPassed() == 2);
}

TEST_CASE ("Any key resumes a stopped song while playing along", "[player][learn][whileplaying]")
{
    ChordRun r (LearnStyle::WhilePlaying);
    r.run (3.0);
    REQUIRE (r.p.isWaiting());
    const PitchSet stuckAt = r.p.expectedPitches();
    r.p.heardSomething();
    CHECK (r.p.expectedPitches() != stuckAt); // moved on at once
    CHECK (r.p.getGroupsPassed() == 1);
    CHECK (r.p.getGroupsSkipped() == 0);
    r.run (0.5);
    CHECK_FALSE (r.p.isWaiting()); // and keeps going for a while
}

TEST_CASE ("Keep going while I play still completes a chord that is played", "[player][learn][whileplaying]")
{
    ChordRun r (LearnStyle::WhilePlaying);
    r.p.notePlayed (r.p.expectedPitches());
    r.p.heardSomething(); // the engine reports the same strike
    CHECK (r.p.getGroupsCompleted() == 1);
    CHECK (r.p.getGroupsPassed() == 0);
    CHECK (r.p.expectedPitches() == ps ({ 61 }));
}

TEST_CASE ("Seeking makes playing along wait for a key again", "[player][learn][whileplaying]")
{
    ChordRun r (LearnStyle::WhilePlaying);
    r.p.heardSomething();
    r.run (0.5);
    REQUIRE_FALSE (r.p.isWaiting());
    r.p.seek (2.0);
    r.run (0.5);
    CHECK (r.p.isWaiting());
    CHECK (r.p.expectedPitches() == ps ({ 64 }));
    r.p.heardSomething();
    CHECK (r.p.expectedPitches() == ps ({ 65 }));
}

TEST_CASE ("Freeplay ignores what's played", "[player]")
{
    Rig r;
    r.p.setPlaying (true);
    r.p.notePlayed (ps ({ 72 }));
    CHECK (r.p.expectedPitches().none());
    CHECK_FALSE (r.p.isWaiting());
}

TEST_CASE ("Loop wraps and progressive speed ramps", "[player][loop]")
{
    Rig r;
    LoopSettings l;
    l.enabled = true; l.a = 0.5; l.b = 1.0;
    l.progressive = true; l.startSpeed = 0.5; l.step = 0.25; l.targetSpeed = 1.0;
    r.p.setLoop (l);
    CHECK (r.p.getTime() == 0.5); // jumped into the loop
    CHECK (r.p.getSpeed() == 0.5);
    r.p.setPlaying (true);

    r.run (1.0); // first pass at 0.5x takes exactly 1 s
    CHECK (r.p.getLoopPass() == 1);
    CHECK (r.p.getSpeed() == 0.75);
    r.run (2.0);
    CHECK (r.p.getSpeed() == 1.0); // capped at target
    CHECK (r.p.getTime() >= 0.5);
    CHECK (r.p.getTime() < 1.0);
    CHECK (r.countOn (74) >= 3);
    CHECK (r.countOn (76) == 0); // never escapes the loop
}

TEST_CASE ("Jump keys seek to their marker", "[player][markers]")
{
    Rig r;
    r.p.setMode (Mode::Learn);
    r.p.setPracticeHands (HandRight);
    r.p.setJumpLeadIn (0.25);
    r.p.setMarkers ({ { 1.0, 21, "B section" }, { 0.5, 22, "A" } });
    r.p.setPlaying (true);
    r.run (0.1);

    CHECK (r.p.notePlayed (ps ({ 21 })).marker == 0);
    CHECK_THAT (r.p.getTime(), WithinAbs (0.75, 1e-9));
    CHECK (r.p.expectedPitches() == ps ({ 76 }));
    CHECK (r.p.notePlayed (ps ({ 22 })).marker == 1);
    CHECK_THAT (r.p.getTime(), WithinAbs (0.25, 1e-9));
}

TEST_CASE ("Seeking releases held notes and restores the pedal", "[player]")
{
    Rig r;
    r.p.setPlaying (true);
    r.run (0.2);        // 48, 55, 72 sounding, pedal down
    r.p.seek (1.2);
    r.run (0.1);
    CHECK (r.offAt (72) == 200);
    CHECK (r.offAt (48) == 200);
    // pedal state at 1.2 is "up" -> a CC64=0 is sent at the seek
    bool pedalUp = false;
    for (auto& e : r.events) pedalUp |= (e.cc == 64 && e.value == 0 && e.sample == 200);
    CHECK (pedalUp);
}

TEST_CASE ("Muted hand isn't sent to the instrument", "[player]")
{
    Rig r;
    r.p.setAudibleHands (HandRight);
    r.p.setPlaying (true);
    r.run (2.0);
    CHECK (r.countOn (72) == 1);
    CHECK (r.countOn (48) == 0);
    CHECK (r.countOn (43) == 0);
}

TEST_CASE ("Re-striking a held pitch releases it first", "[player]")
{
    auto s = std::make_shared<Song>();
    Note a; a.pitch = 60; a.start = 0.0; a.end = 1.0; s->notes.push_back (a);
    Note b; b.pitch = 60; b.start = 0.5; b.end = 0.8; s->notes.push_back (b);
    s->length = 1.0;

    Rig r (s);
    r.p.setPlaying (true);
    r.run (1.5);
    // on@0, off@500, on@500, off@800, and no stray off at 1000
    int offs = 0;
    for (auto& e : r.events) offs += (! e.on && e.pitch == 60);
    CHECK (offs == 2);
    CHECK (r.onAt (60, 1) == 500);
}
