#include <catch2/catch_test_macros.hpp>

#include "test_detect_support.h"

using namespace fall;
using namespace fall::testing;

namespace
{
// Captures `pitch` from a synthetic strike at 1.0 s, returns whether it finished.
bool captureKey (NoteDetector& det, int pitch, const Environment& env, Calibration& into)
{
    det.reset();
    det.beginCapture (pitch);
    const auto audio = renderScene ({ { pitch, 1.0, 1.0, 0.8f } }, 2.5, env);
    stream (det, audio, env.sampleRate, 480);
    if (! det.captureFinished())
        return false;
    det.storeCapture (into);
    return true;
}
} // namespace

TEST_CASE ("calibration capture stores a key and the binary format round-trips", "[detect][calibration]")
{
    Environment env;
    NoteDetector det;
    det.prepare (env.sampleRate, 480);

    Calibration cal;
    REQUIRE (cal.numKeys() == 0);
    REQUIRE_FALSE (cal.hasKey (60));

    REQUIRE (captureKey (det, 60, env, cal));
    REQUIRE (captureKey (det, 45, env, cal));
    REQUIRE (captureKey (det, 84, env, cal));
    CHECK (cal.numKeys() == 3);
    CHECK (cal.hasKey (60));
    CHECK (cal.hasKey (45));
    CHECK (cal.hasKey (84));
    CHECK_FALSE (cal.hasKey (61));

    const auto bin = cal.toBinary();
    CHECK (bin.getSize() > 16);

    Calibration restored;
    REQUIRE (restored.fromBinary (bin));
    CHECK (restored.numKeys() == 3);
    CHECK (restored.hasKey (45));
    CHECK (restored.toBinary() == bin); // byte-identical

    // copy semantics
    Calibration copy (restored);
    CHECK (copy.toBinary() == bin);
    copy.clear();
    CHECK (copy.numKeys() == 0);
    CHECK (restored.numKeys() == 3);

    // corrupt / foreign data is rejected and leaves the target untouched
    Calibration other;
    juce::MemoryBlock junk (bin.getData(), bin.getSize() / 2);
    CHECK_FALSE (other.fromBinary (junk));
    CHECK_FALSE (other.fromBinary (juce::MemoryBlock()));
    juce::MemoryBlock badMagic (bin);
    static_cast<char*> (badMagic.getData())[0] ^= 0x55;
    CHECK_FALSE (restored.fromBinary (badMagic));
    CHECK (restored.numKeys() == 3);
    juce::MemoryBlock badVersion (bin);
    static_cast<char*> (badVersion.getData())[4] = 99;
    CHECK_FALSE (restored.fromBinary (badVersion));
    CHECK (restored.numKeys() == 3);

    Calibration empty;
    CHECK (other.fromBinary (empty.toBinary()));
    CHECK (other.numKeys() == 0);
}

TEST_CASE ("capture waits for a clear onset and ignores silence", "[detect][calibration]")
{
    Environment env;
    NoteDetector det;
    det.prepare (env.sampleRate, 480);
    det.beginCapture (60);
    stream (det, renderScene ({}, 3.0, env), env.sampleRate, 480);
    CHECK_FALSE (det.captureFinished());
    stream (det, renderScene ({ { 60, 0.5, 1.0, 0.8f } }, 2.5, env), env.sampleRate, 480);
    CHECK (det.captureFinished());
}

TEST_CASE ("calibrated detection works, including interpolation to uncalibrated neighbours", "[detect][calibration]")
{
    Environment env;
    NoteDetector det;
    det.prepare (env.sampleRate, 480);
    Calibration cal;
    for (int p : { 48, 55, 60, 67, 72 })
        REQUIRE (captureKey (det, p, env, cal));

    for (int pitch : { 60, 62, 69, 55 }) // calibrated and uncalibrated
    {
        CAPTURE (pitch);
        NoteDetector d;
        d.prepare (env.sampleRate, 480);
        d.setCalibration (cal);
        d.setExpected (setOf ({ pitch }));
        bool hit = false;
        for (auto& r : stream (d, renderScene ({ { pitch, 1.0, 0.8, 0.7f } }, 3.0, env), env.sampleRate, 480))
            hit |= r.onset.expectedHits.test ((size_t) pitch);
        CHECK (hit);
    }

    // a wrong neighbour is still rejected with calibration on
    NoteDetector d;
    d.prepare (env.sampleRate, 480);
    d.setCalibration (cal);
    d.setExpected (setOf ({ 60 }));
    bool hit = false;
    for (auto& r : stream (d, renderScene ({ { 61, 1.0, 0.8, 0.7f } }, 3.0, env), env.sampleRate, 480))
        hit |= r.onset.expectedHits.test (60);
    CHECK_FALSE (hit);
}
