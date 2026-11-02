#include <catch2/catch_test_macros.hpp>

#include "test_detect_support.h"

using namespace fall;
using namespace fall::testing;

namespace
{
PitchSet jumpKeys()
{
    return setOf ({ 21, 23, 108 });
}

int countWatchHits (const std::vector<SynthNote>& notes, double seconds, int pitch, Environment env = {})
{
    NoteDetector det;
    det.prepare (env.sampleRate, 480);
    det.setWatchKeys (jumpKeys());
    int hits = 0;
    for (auto& r : stream (det, renderScene (notes, seconds, env), env.sampleRate, 480))
    {
        hits += r.onset.watchHits.test ((size_t) pitch) ? 1 : 0;
    }
    return hits;
}
} // namespace

TEST_CASE ("jump keys stay quiet during normal playing", "[detect][jump]")
{
    // A melody over the middle of the keyboard, left-hand A-major / C-major chords (their overtones
    // cover many of A0's and C8's partials), octaves on C, in a roomy mic.
    std::vector<SynthNote> notes;
    double t = 1.0;
    const int bass[] = { 33, 40, 45, 36, 43, 48, 28, 35 };
    const int mel[]  = { 72, 76, 79, 84, 72, 69, 64, 60, 84, 96, 72, 60 };
    for (int bar = 0; bar < 8; ++bar)
    {
        notes.push_back ({ bass[bar], t, 1.8, 0.7f });
        notes.push_back ({ bass[bar] + 7, t + 0.01, 1.8, 0.6f });
        notes.push_back ({ bass[bar] + 12, t + 0.02, 1.8, 0.6f });
        notes.push_back ({ bass[bar] + 16, t + 0.02, 1.8, 0.5f });
        for (int i = 0; i < 4; ++i)
            notes.push_back ({ mel[(bar * 4 + i) % 12], t + 0.5 * i, 0.45, 0.7f });
        t += 2.0;
    }
    notes.push_back ({ 96, t, 0.5, 0.8f });  // C7: its second partial is C8's fundamental
    notes.push_back ({ 84, t + 1, 0.5, 0.8f });
    notes.push_back ({ 72, t + 2, 0.5, 0.8f });
    notes.push_back ({ 57, t + 3, 0.5, 0.8f });
    t += 5.0;

    Environment env;
    env.noiseDb = -50.0;
    NoteDetector det;
    det.prepare (env.sampleRate, 480);
    det.setWatchKeys (jumpKeys());
    int fires = 0;
    for (auto& r : stream (det, renderScene (notes, t, env), env.sampleRate, 480))
    {
        fires += r.onset.watchHits.any() ? 1 : 0;
    }
    CHECK (fires == 0);
}

TEST_CASE ("an isolated high jump key strike is recognised", "[detect][jump]")
{
    // C8's only usable partial is its fundamental (4186 Hz); it must be heard as the sole new tone.
    int hits = 0;
    for (double sr : { 48000.0, 44100.0 })
    {
        Environment env;
        env.sampleRate = sr;
        env.noiseDb = -60.0;
        hits += countWatchHits ({ { 108, 1.0, 0.6, 0.9f } }, 3.0, 108, env) > 0 ? 1 : 0;
    }
    CHECK (hits >= 1);
}
