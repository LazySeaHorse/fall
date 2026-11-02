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

TEST_CASE ("snaps / claps / clicks do not satisfy an expected note", "[detect]")
{
    // Broadband bursts in a quiet room, each with a fresh expected chord: the old "fraction of
    // partials with a peak" test let these through; the fitted-partials and energy checks must not.
    const double sr = 48000.0;
    Environment env;
    env.noiseDb = -70.0;
    std::vector<float> audio = renderScene ({}, 40.0, env);
    Rng rng (99);
    std::vector<double> times;
    for (int i = 0; i < 24; ++i)
    {
        const double t = 1.0 + 1.6 * i;
        times.push_back (t);
        // band-limited noise burst (1-6 kHz-ish via a crude difference filter), 3-10 ms decay
        const double tau = 0.003 + 0.007 * rng.uniform();
        const float amp = 0.1f + 0.4f * rng.uniform();
        float prev = 0.0f;
        for (int k = 0; k < (int) (0.15 * sr); ++k)
        {
            const float w = rng.gauss();
            const float hp = w - prev;
            prev = w;
            audio[(size_t) (t * sr) + (size_t) k] += amp * hp * (float) std::exp (-(double) k / sr / tau);
        }
    }
    NoteDetector det;
    det.prepare (sr, kBlock);
    det.setSensitivity (0.7f);
    int accepted = 0;
    size_t next = 0;
    const int chords[6][3] = { { 60, 64, 67 }, { 72, 0, 0 }, { 55, 59, 62 }, { 84, 88, 0 }, { 48, 0, 0 }, { 65, 69, 72 } };
    for (size_t pos = 0; pos < audio.size(); pos += kBlock)
    {
        const double now = (double) pos / sr;
        if (next < times.size() && now >= times[next] - 0.5)
        {
            PitchSet c;
            for (int p : chords[next % 6]) if (p > 0) c.set ((size_t) p);
            det.setExpected (c);
            ++next;
        }
        DetectedOnset out[8];
        const int n = det.process (audio.data() + pos, (int) std::min<size_t> (kBlock, audio.size() - pos), out, 8);
        for (int i = 0; i < n; ++i)
            accepted += out[i].expectedHits.any() ? 1 : 0;
    }
    CHECK (accepted <= 1);
}

TEST_CASE ("the auto-tuner follows a piano that is not tuned to A440", "[detect]")
{
    // A piano 30 cents flat: after a few notes the detector knows, and still hears what is played.
    const double sr = 48000.0;
    std::vector<SynthNote> notes;
    const int melody[] = { 60, 62, 64, 65, 67, 69, 71, 72, 74, 76, 77, 79, 81, 72, 67, 64, 60, 64, 67, 72,
                           62, 65, 69, 74, 64, 67, 71, 76, 65, 69, 72, 77 };
    double t = 1.0;
    for (int p : melody)
    {
        notes.push_back ({ p, t, 0.4, 0.7f, -30.0 });
        t += 0.6;
    }
    const auto audio = renderScene (notes, t + 1.0, Environment());
    NoteDetector det;
    det.prepare (sr, kBlock);
    size_t g = 0;
    det.setExpected (setOf ({ notes[0].pitch }));
    int hitsLate = 0;
    for (size_t pos = 0; pos < audio.size(); pos += kBlock)
    {
        DetectedOnset out[8];
        const int n = det.process (audio.data() + pos, (int) std::min<size_t> (kBlock, audio.size() - pos), out, 8);
        for (int i = 0; i < n; ++i)
            if (g < notes.size() && out[i].expectedHits.test ((size_t) notes[g].pitch))
            {
                hitsLate += g >= 16 ? 1 : 0;
                ++g;
                if (g < notes.size())
                    det.setExpected (setOf ({ notes[g].pitch }));
            }
        // a missed note: move on shortly before the next one, like the eval tool does
        if (g < notes.size() && g + 1 < notes.size() && (double) pos / sr > notes[g + 1].start - 0.15)
        {
            ++g;
            det.setExpected (setOf ({ notes[g].pitch }));
        }
    }
    bool confident = false;
    const float cents = det.getTuningCents (&confident);
    CHECK (confident);
    CHECK (std::abs (cents + 30.0f) < 6.0f);
    CHECK (hitsLate >= 14); // of the 16 notes after the tuner settled
}

TEST_CASE ("the octave above does not satisfy an expected note, and is shown for what it is", "[detect]")
{
    // C7 has a partial wherever C6 has an even one, so it scores almost like C6. Learners do play the
    // right key in the wrong octave; it must not count, and the display must show the key they hit.
    const int keys[] = { 60, 62, 64, 67, 69, 72, 76, 77, 79, 83, 84, 88 };
    for (float sens : { 0.6f, 0.8f })
    {
        int accepted = 0, shownRight = 0, total = 0, rightHits = 0;
        for (int p : keys)
            for (int up : { 12, 24 })
            {
                if (p + up > 108)
                    continue;
                ++total;
                NoteDetector det;
                det.prepare (48000.0, kBlock);
                det.setSensitivity (sens);
                det.setExpected (setOf ({ p }));
                const auto audio = renderScene ({ { p + up, 1.0, 0.8, 0.7f } }, 3.0, Environment());
                bool hit = false, shown = false;
                for (auto& r : stream (det, audio, 48000.0, kBlock))
                {
                    hit = hit || r.onset.expectedHits.test ((size_t) p);
                    shown = shown || r.onset.pitches.test ((size_t) (p + up));
                }
                accepted += hit ? 1 : 0;
                shownRight += shown ? 1 : 0;
                // sanity: the right key in the same scene is heard
                rightHits += run ({ { p, 1.0, 0.8, 0.7f } }, 3.0, setOf ({ p }), p, {}, kBlock, sens).hit ? 1 : 0;
            }
        INFO ("sensitivity " << sens << ": octave accepted " << accepted << ", shown " << shownRight << ", right key heard " << rightHits << " of " << total);
        CHECK (accepted <= 1);
        CHECK (shownRight >= total - 3);
        CHECK (rightHits == total);
    }
}
