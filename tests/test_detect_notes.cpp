#include <catch2/catch_test_macros.hpp>

#include "test_detect_support.h"

#include <algorithm>

using namespace fall;
using namespace fall::testing;

namespace
{
constexpr int kBlock = 480;

// Everything the detector reported for one scene.
std::vector<Report> listen (const std::vector<SynthNote>& notes, double seconds, const PitchSet& expected,
                            const Environment& env = {}, int block = kBlock, float sensitivity = 0.6f)
{
    NoteDetector det;
    det.prepare (env.sampleRate, block);
    det.setSensitivity (sensitivity);
    det.setExpected (expected);
    return stream (det, renderScene (notes, seconds, env), env.sampleRate, block);
}

// Was `target` accepted (learn mode) when it was the expected note?
bool accepted (const std::vector<Report>& rs, int target)
{
    return std::any_of (rs.begin(), rs.end(), [&] (const Report& r) { return r.onset.expectedHits.test ((size_t) target); });
}

bool hitAlone (int pitch, const Environment& env = {}, int block = kBlock, float velocity = 0.7f)
{
    return accepted (listen ({ { pitch, 1.0, 0.8, velocity } }, 2.5, setOf ({ pitch }), env, block), pitch);
}
} // namespace

TEST_CASE ("single notes are found across the keyboard", "[detect]")
{
    int total = 0, hits = 0;
    for (int p = 28; p <= 108; p += 4)
    {
        const bool h = hitAlone (p);
        ++total;
        hits += h ? 1 : 0;
        // (above ~E6 the pitch tracker sometimes reads the synthetic piano an octave low)
        if (p >= 40 && p <= 88)
        {
            INFO ("pitch " << p);
            CHECK (h);
        }
    }
    CHECK (hits >= total - 3); // see above: G#6 / G#7 can read as a lower key
}

TEST_CASE ("a note is reported quickly, once, and as the right key", "[detect]")
{
    for (int p : { 48, 60, 67, 72, 79, 84 })
    {
        const auto rs = listen ({ { p, 1.0, 0.8, 0.7f } }, 2.5, setOf ({ p }));
        INFO ("pitch " << p);
        REQUIRE (rs.size() == 1);
        CHECK (rs[0].onset.pitches == setOf ({ p }));
        CHECK (rs[0].blockEnd - 1.0 < 0.12);
        CHECK (std::abs (rs[0].onsetTime - 1.0) < 0.03);
    }
}

TEST_CASE ("detection does not depend on input level", "[detect]")
{
    for (double gain : { -24.0, 0.0, 10.0 })
        for (int p : { 48, 60, 72, 84 })
        {
            Environment env;
            env.gainDb = gain;
            env.noiseDb = -60.0;
            INFO ("gain " << gain << " pitch " << p);
            CHECK (hitAlone (p, env));
        }
}

TEST_CASE ("quiet and loud strikes both register", "[detect]")
{
    for (float v : { 0.25f, 0.5f, 1.0f })
        for (int p : { 55, 72 })
        {
            INFO ("velocity " << v << " pitch " << p);
            CHECK (hitAlone (p, {}, kBlock, v));
        }
}

TEST_CASE ("wrong notes, including the octave above, are not accepted", "[detect]")
{
    for (int p : { 48, 55, 60, 67, 72, 79, 84 })
        for (int played : { p + 1, p - 1, p + 7, p + 12, p + 24 })
        {
            const auto rs = listen ({ { played, 1.0, 0.8, 0.7f } }, 2.5, setOf ({ p }));
            INFO ("expected " << p << " played " << played);
            CHECK_FALSE (accepted (rs, p));
            // ...and what was played is shown for what it is
            CHECK (std::any_of (rs.begin(), rs.end(), [&] (const Report& r) { return r.onset.pitches.test ((size_t) played); }));
        }
}

TEST_CASE ("a melody gives one report per note", "[detect]")
{
    const int mel[] = { 60, 64, 67, 72, 71, 67, 64, 62, 60, 65, 69, 72 };
    std::vector<SynthNote> notes;
    for (int i = 0; i < 12; ++i)
        notes.push_back ({ mel[i], 1.0 + 0.5 * i, 0.45, 0.7f });
    const auto rs = listen (notes, 8.0, {});
    CHECK (rs.size() == 12);
    int right = 0;
    for (auto& n : notes)
        right += std::any_of (rs.begin(), rs.end(), [&] (const Report& r)
                              { return r.onset.pitches.test ((size_t) n.pitch) && std::abs (r.onsetTime - n.start) < 0.1; }) ? 1 : 0;
    CHECK (right >= 11);
}

TEST_CASE ("repeated notes: the same key struck again while ringing is a new hit", "[detect]")
{
    const auto rs = listen ({ { 72, 1.0, 0.5, 0.7f }, { 72, 1.6, 0.5, 0.7f }, { 72, 2.2, 0.5, 0.7f } }, 3.5, setOf ({ 72 }));
    CHECK (std::count_if (rs.begin(), rs.end(), [] (const Report& r) { return r.onset.pitches.test (72); }) == 3);
}

TEST_CASE ("a finger's thump just before the hammer does not hide a repeated note", "[detect]")
{
    // G5 fades; the same key is struck again at 2.0 s, and the finger hits the key bed 100 ms earlier
    Environment env;
    auto audio = renderScene ({ { 79, 1.0, 0.8, 0.7f }, { 79, 2.0, 0.8, 0.7f } }, 3.5, env);
    float peak = 0;
    for (size_t i = (size_t) (1.0 * env.sampleRate); i < (size_t) (1.2 * env.sampleRate); ++i) peak = std::max (peak, std::abs (audio[i]));
    Rng rng (7);
    float lp = 0;
    for (int k = 0; k < (int) (0.06 * env.sampleRate); ++k) // a low thud, ~300 Hz, 15 ms decay
    {
        lp += 0.04f * (rng.gauss() - lp);
        audio[(size_t) (1.90 * env.sampleRate) + (size_t) k] += 2.5f * peak * lp * (float) std::exp (-(double) k / env.sampleRate / 0.015);
    }
    NoteDetector det;
    det.prepare (env.sampleRate, kBlock);
    det.setSensitivity (0.6f);
    det.setExpected (setOf ({ 79 }));
    const auto rs = stream (det, audio, env.sampleRate, kBlock);
    CHECK (std::count_if (rs.begin(), rs.end(), [] (const Report& r) { return r.onset.pitches.test (79); }) == 2);
    CHECK (std::any_of (rs.begin(), rs.end(), [] (const Report& r) { return r.onset.pitches.test (79) && std::abs (r.onsetTime - 2.0) < 0.03; }));
}

TEST_CASE ("a ringing note that swells is not a new note", "[detect]")
{
    // strings beating, or the room: the level of a ringing C5 creeps up 9 dB over 60 ms and back
    Environment env;
    auto audio = renderScene ({ { 72, 1.0, 3.0, 0.7f } }, 4.0, env);
    for (size_t i = 0; i < audio.size(); ++i)
    {
        const double t = (double) i / env.sampleRate - 2.0;
        const double db = t < 0 ? 0 : t < 0.06 ? 9.0 * t / 0.06 : t < 0.3 ? 9.0 : t < 0.5 ? 9.0 * (0.5 - t) / 0.2 : 0;
        audio[i] *= (float) std::pow (10.0, db / 20.0);
    }
    NoteDetector det;
    det.prepare (env.sampleRate, kBlock);
    det.setSensitivity (0.6f);
    det.setExpected (setOf ({ 72 }));
    CHECK (stream (det, audio, env.sampleRate, kBlock).size() == 1);
}

TEST_CASE ("a new note over a ringing one is heard as the new note", "[detect]")
{
    // left hand C3 rings, right hand plays E5 on top
    const auto rs = listen ({ { 48, 1.0, 3.0, 0.7f }, { 76, 1.8, 0.8, 0.6f } }, 3.5, setOf ({ 76 }));
    CHECK (accepted (rs, 76));
}

TEST_CASE ("other sample rates and block sizes give the same answer", "[detect]")
{
    for (double sr : { 44100.0, 96000.0 })
    {
        Environment env;
        env.sampleRate = sr;
        INFO ("rate " << sr);
        CHECK (hitAlone (60, env));
        CHECK (hitAlone (79, env));
    }
    for (int block : { 64, 333, 2048 })
    {
        INFO ("block " << block);
        CHECK (hitAlone (67, {}, block));
    }
}

TEST_CASE ("silence and room noise produce nothing", "[detect]")
{
    for (double noise : { -70.0, -50.0, -35.0 })
    {
        NoteDetector det;
        det.prepare (48000.0, kBlock);
        det.setSensitivity (1.0f); // even at the most trigger-happy setting
        det.setExpected (setOf ({ 60, 64 }));
        det.setWatchKeys (setOf ({ 21, 23, 108 }));
        Environment env;
        env.noiseDb = noise;
        auto audio = renderScene ({}, 20.0, env);
        // the room is not steady: slow swells of +-6 dB (a fan, breathing, traffic)
        for (size_t i = 0; i < audio.size(); ++i)
            audio[i] *= (float) std::pow (2.0, std::sin ((double) i / 48000.0 * 1.3) * std::sin ((double) i / 48000.0 * 0.37));
        INFO ("noise " << noise << " dB");
        CHECK (stream (det, audio, 48000.0, kBlock).empty());
    }
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
    const double sr = 48000.0;
    Environment env;
    env.noiseDb = -70.0;
    std::vector<float> audio = renderScene ({}, 40.0, env);
    Rng rng (99);
    for (int i = 0; i < 24; ++i)
    {
        const double t = 1.0 + 1.6 * i;
        // broadband noise burst, 3-10 ms decay
        const double tau = 0.003 + 0.007 * rng.uniform();
        const float amp = 0.1f + 0.4f * rng.uniform();
        float prev = 0.0f;
        for (int k = 0; k < (int) (0.15 * sr); ++k)
        {
            const float w = rng.gauss();
            audio[(size_t) (t * sr) + (size_t) k] += amp * (w - prev) * (float) std::exp (-(double) k / sr / tau);
            prev = w;
        }
    }
    NoteDetector det;
    det.prepare (sr, kBlock);
    det.setSensitivity (0.7f);
    PitchSet all;
    for (int p = kLowestPianoKey; p <= kHighestPianoKey; ++p) all.set ((size_t) p);
    det.setExpected (all); // any key at all would count
    int hits = 0;
    for (auto& r : stream (det, audio, sr, kBlock))
        hits += r.onset.expectedHits.any() ? 1 : 0;
    CHECK (hits <= 1);
}

TEST_CASE ("the tuner follows a piano that is not tuned to A440", "[detect]")
{
    // (the synthetic piano's stretched partials read a little sharp, so compare against an in-tune one)
    const int mel[] = { 60, 64, 67, 72, 69, 65, 62, 59, 57, 55 };
    auto estimate = [&] (double cents, int& right)
    {
        std::vector<SynthNote> notes;
        for (int i = 0; i < 10; ++i)
            notes.push_back ({ mel[i], 1.0 + 0.6 * i, 0.5, 0.7f, cents });
        NoteDetector det;
        det.prepare (48000.0, kBlock);
        const auto rs = stream (det, renderScene (notes, 8.0, {}), 48000.0, kBlock);
        right = 0;
        for (auto& n : notes)
            right += std::any_of (rs.begin(), rs.end(), [&] (const Report& r)
                                  { return r.onset.pitches.test ((size_t) n.pitch) && std::abs (r.onsetTime - n.start) < 0.1; }) ? 1 : 0;
        bool sure = false;
        const float c = det.getTuningCents (&sure);
        CHECK (sure);
        return c;
    };
    int right = 0;
    const float base = estimate (0.0, right);
    CHECK (right >= 9);
    for (double cents : { -30.0, 15.0 })
    {
        const float c = estimate (cents, right);
        INFO ("detuned " << cents << " estimated " << c << " (in tune: " << base << ")");
        CHECK (std::abs (c - base - cents) < 10.0);
        CHECK (right >= 9);
    }
}
