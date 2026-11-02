#include <catch2/catch_test_macros.hpp>

#include "test_detect_support.h"

#include <algorithm>

using namespace fall;
using namespace fall::testing;

namespace
{
constexpr int kBlock = 480;

struct Outcome
{
    bool   hit = false;
    double latency = 1e9;  // seconds from the key strike until the host received the hit
    double onsetErr = 1e9; // seconds, reported onset time minus the real strike
};

// Plays `notes`, with `expected` set before the first one, and reports whether pitch `target` was hit.
Outcome run (const std::vector<SynthNote>& notes, double seconds, const PitchSet& expected, int target,
             const Environment& env = {}, int block = kBlock, float sensitivity = 0.6f, double strikeTime = -1.0)
{
    NoteDetector det;
    det.prepare (env.sampleRate, block);
    det.setSensitivity (sensitivity);
    const auto audio = renderScene (notes, seconds, env);
    det.setExpected (expected);
    Outcome o;
    if (strikeTime < 0)
        for (auto& n : notes)
            if (n.pitch == target) { strikeTime = n.start; break; }
    for (auto& r : stream (det, audio, env.sampleRate, block))
        if (r.onset.expectedHits.test ((size_t) target) && ! o.hit)
        {
            o.hit = true;
            o.latency = r.blockEnd - strikeTime;
            o.onsetErr = r.onsetTime - strikeTime;
        }
    return o;
}
} // namespace

TEST_CASE ("single expected notes are found across the whole keyboard", "[detect]")
{
    for (int pitch : { 24, 28, 33, 36, 40, 45, 48, 52, 57, 60, 64, 69, 72, 76, 81, 84, 88, 93, 96, 100, 105 })
    {
        CAPTURE (pitch);
        const auto o = run ({ { pitch, 1.0, 0.8, 0.7f } }, 3.0, setOf ({ pitch }), pitch);
        REQUIRE (o.hit);
        // mid/high notes: ~30-60 ms; deep bass needs a longer window
        CHECK (o.latency < (pitch >= 48 ? 0.075 : 0.14));
        CHECK (std::abs (o.onsetErr) < 0.03);
    }
}

TEST_CASE ("detection does not depend on input level", "[detect]")
{
    for (double gain : { -30.0, -15.0, 0.0, 10.0 })
    {
        CAPTURE (gain);
        Environment env;
        env.gainDb = gain;
        env.noiseDb = -60.0;
        CHECK (run ({ { 60, 1.0, 0.8, 0.7f } }, 3.0, setOf ({ 60 }), 60, env).hit);
        CHECK (run ({ { 43, 1.0, 0.8, 0.7f } }, 3.0, setOf ({ 43 }), 43, env).hit);
    }
}

TEST_CASE ("quiet and loud strikes both register", "[detect]")
{
    for (float vel : { 0.25f, 0.5f, 1.0f })
    {
        CAPTURE (vel);
        CHECK (run ({ { 67, 1.0, 0.8, vel } }, 3.0, setOf ({ 67 }), 67).hit);
    }
}

TEST_CASE ("a neighbouring wrong note is not accepted at default sensitivity", "[detect]")
{
    // expected E4, played F#4 (and F4)
    CHECK_FALSE (run ({ { 66, 1.0, 0.8, 0.7f } }, 3.0, setOf ({ 64 }), 64).hit);
    CHECK_FALSE (run ({ { 65, 1.0, 0.8, 0.7f } }, 3.0, setOf ({ 64 }), 64).hit);
    CHECK_FALSE (run ({ { 63, 1.0, 0.8, 0.7f } }, 3.0, setOf ({ 64 }), 64).hit);
    // and a wrong note in the high register
    CHECK_FALSE (run ({ { 85, 1.0, 0.8, 0.7f } }, 3.0, setOf ({ 84 }), 84).hit);
    // sanity: the right note IS accepted in the same scene
    CHECK (run ({ { 66, 1.0, 0.8, 0.7f } }, 3.0, setOf ({ 66 }), 66).hit);
}

TEST_CASE ("chords: every expected note of a six-note chord is reported", "[detect]")
{
    const std::vector<int> pitches { 36, 43, 52, 60, 64, 67 };
    std::vector<SynthNote> notes;
    PitchSet expected;
    for (size_t i = 0; i < pitches.size(); ++i)
    {
        notes.push_back ({ pitches[i], 1.0 + 0.004 * (double) i, 1.0, 0.7f });
        expected.set ((size_t) pitches[i]);
    }
    NoteDetector det;
    det.prepare (48000.0, kBlock);
    det.setExpected (expected);
    PitchSet got;
    double last = 0;
    for (auto& r : stream (det, renderScene (notes, 3.0, {}), 48000.0, kBlock))
    {
        got |= r.onset.expectedHits;
        if (r.onset.expectedHits.any())
            last = r.blockEnd;
    }
    CHECK (got == expected);
    CHECK (last - 1.0 < 0.2);
}

TEST_CASE ("only NEW energy counts: a note that is still ringing is not re-reported", "[detect]")
{
    // C4 is struck and rings on (pedal). We start waiting for F#4 (and later for C4 itself) only once
    // the strike is over: the ringing must not trigger anything; a fresh F#4 / C4 strike must.
    std::vector<SynthNote> notes { { 60, 0.5, 5.0, 0.8f }, { 66, 3.0, 0.8, 0.7f }, { 60, 4.5, 0.8, 0.8f } };
    NoteDetector det;
    det.prepare (48000.0, kBlock);
    const auto audio = renderScene (notes, 6.0, {});
    std::vector<DetectedOnset> buf (16);
    int early66 = 0, early60 = 0, late66 = 0, late60 = 0;
    for (size_t i = 0; i < audio.size(); i += kBlock)
    {
        const double t = (double) i / 48000.0;
        if (i == (size_t) (1.5 * 48000.0))
            det.setExpected (setOf ({ 66, 60 }));
        const int n = (int) std::min<size_t> (kBlock, audio.size() - i);
        const int cnt = det.process (audio.data() + i, n, buf.data(), 16);
        for (int k = 0; k < cnt; ++k)
        {
            const double on = (double) buf[(size_t) k].samplePosition / 48000.0;
            if (buf[(size_t) k].expectedHits.test (66)) (on < 2.9 ? early66 : late66)++;
            // (the F#4 strike in between shares a few partials with C4, so only the quiet stretch counts)
            if (buf[(size_t) k].expectedHits.test (60) && (on < 2.9 || on > 4.4)) (on < 2.9 ? early60 : late60)++;
        }
        (void) t;
    }
    CHECK (early66 == 0);
    CHECK (early60 == 0);
    CHECK (late66 >= 1);
    CHECK (late60 >= 1);
}

TEST_CASE ("repeated notes: the same key struck again while ringing is a new hit", "[detect]")
{
    for (int pitch : { 36, 48, 60, 72, 84 })
    {
        CAPTURE (pitch);
        std::vector<SynthNote> notes { { pitch, 1.0, 1.5, 0.8f }, { pitch, 1.6, 1.0, 0.8f }, { pitch, 2.3, 1.0, 0.9f } };
        NoteDetector det;
        det.prepare (48000.0, kBlock);
        det.setExpected (setOf ({ pitch }));
        const auto audio = renderScene (notes, 4.0, {});
        std::vector<DetectedOnset> buf (16);
        std::vector<double> hits;
        int target = 0;
        const double strikes[3] = { 1.0, 1.6, 2.3 };
        for (size_t i = 0; i < audio.size(); i += kBlock)
        {
            const int n = (int) std::min<size_t> (kBlock, audio.size() - i);
            const int cnt = det.process (audio.data() + i, n, buf.data(), 16);
            for (int k = 0; k < cnt; ++k)
                if (buf[(size_t) k].expectedHits.test ((size_t) pitch))
                {
                    hits.push_back ((double) buf[(size_t) k].samplePosition / 48000.0);
                    // the same chord again, like a repeated note in the song; nothing after the last one
                    det.setExpected (++target < 3 ? setOf ({ pitch }) : PitchSet());
                }
        }
        INFO ("hit times: " << (hits.size() > 0 ? hits[0] : 0) << " " << (hits.size() > 1 ? hits[1] : 0) << " " << (hits.size() > 2 ? hits[2] : 0) << " " << (hits.size() > 3 ? hits[3] : 0));
        REQUIRE (hits.size() == 3);
        for (int i = 0; i < 3; ++i)
            CHECK (std::abs (hits[(size_t) i] - strikes[i]) < 0.04);
    }
}

TEST_CASE ("a new note over ringing pedal chords is detected", "[detect]")
{
    std::vector<SynthNote> notes { { 48, 0.2, 6.0, 0.8f }, { 55, 0.2, 6.0, 0.8f }, { 64, 0.2, 6.0, 0.8f }, { 71, 0.2, 6.0, 0.8f },
                                   { 79, 3.0, 1.0, 0.6f }, { 41, 4.5, 1.0, 0.7f } };
    CHECK (run (notes, 6.0, setOf ({ 79 }), 79).hit);
    CHECK (run (notes, 6.0, setOf ({ 41 }), 41).hit);
}

TEST_CASE ("wrong sample rates and block sizes give the same answer", "[detect]")
{
    for (double sr : { 44100.0, 48000.0, 96000.0 })
        for (int block : { 32, 480, 1024, 4096 })
        {
            CAPTURE (sr, block);
            Environment env;
            env.sampleRate = sr;
            const auto o = run ({ { 62, 1.0, 0.8, 0.7f } }, 2.5, setOf ({ 62 }), 62, env, block);
            REQUIRE (o.hit);
            CHECK (std::abs (o.onsetErr) < 0.03);
        }
}

TEST_CASE ("results do not depend on the block size", "[detect]")
{
    const std::vector<SynthNote> notes { { 60, 1.0, 0.5, 0.7f }, { 64, 1.5, 0.5, 0.7f }, { 67, 2.0, 0.5, 0.7f }, { 40, 2.5, 0.8, 0.7f } };
    const auto audio = renderScene (notes, 4.0, {});
    auto collect = [&] (int block)
    {
        NoteDetector det;
        det.prepare (48000.0, 4096);
        det.setExpected (setOf ({ 60, 64, 67, 40 }));
        std::vector<std::pair<int64_t, size_t>> v;
        for (auto& r : stream (det, audio, 48000.0, block))
            v.push_back ({ r.onset.samplePosition, r.onset.expectedHits.count() });
        return v;
    };
    const auto ref = collect (480);
    CHECK_FALSE (ref.empty());
    CHECK (collect (64) == ref);
    CHECK (collect (1777) == ref);
}

TEST_CASE ("silence and room noise produce nothing", "[detect]")
{
    NoteDetector det;
    det.prepare (48000.0, kBlock);
    det.setExpected (setOf ({ 60, 64 }));
    PitchSet watch;
    watch.set (21); watch.set (23); watch.set (108);
    det.setWatchKeys (watch);
    Environment env;
    env.noiseDb = -50.0;
    const auto audio = renderScene ({}, 20.0, env);
    int events = 0;
    for (auto& r : stream (det, audio, 48000.0, kBlock))
        events += r.onset.expectedHits.any() || r.onset.watchHits.any() ? 1 : 0;
    CHECK (events == 0);
    CHECK (det.getInputLevelDb() < -40.0f);
}

TEST_CASE ("input level meter tracks the signal", "[detect]")
{
    NoteDetector det;
    det.prepare (48000.0, kBlock);
    std::vector<float> sine (48000);
    for (size_t i = 0; i < sine.size(); ++i)
        sine[i] = 0.5f * (float) std::sin (2.0 * kPi * 440.0 * (double) i / 48000.0);
    stream (det, sine, 48000.0, kBlock);
    CHECK (det.getInputLevelDb() > -12.0f);
    CHECK (det.getInputLevelDb() < -6.0f);
}
