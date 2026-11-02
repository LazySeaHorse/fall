#pragma once

// Real-time piano note detection from a single microphone (e.g. a laptop mic).
//
// Design brief:
//  * We usually KNOW which notes should be played next (learn mode), so the main job is
//    verification, not general transcription: "did a new note start, and does it look like
//    one of the expected pitches?"
//  * False positives are acceptable; missed notes and latency are not.
//  * A separate, looser "what did I press" estimate is shown on screen (display only).
//  * Jump keys (usually the extreme keys like A0/B0/C8) must be stricter: a false trigger
//    jumps the song, which is annoying.
//
// Threading: everything here is called from the audio thread. process() must not allocate,
// lock, or do I/O. prepare()/setCalibration() may allocate and are called while audio is stopped
// (or under the engine's lock).

#include "core/Song.h"

#include <array>
#include <cstdint>
#include <memory>

namespace fall
{

struct DetectedOnset
{
    int64_t  samplePosition = 0; // position of the onset in the detector's input stream
    int64_t  reportedAt     = 0; // stream position when it was reported (reportedAt - samplePosition = latency)
    float    strength       = 0; // onset strength, roughly 0..1+
    PitchSet expectedHits;       // expected pitches judged present (learn mode)
    PitchSet watchHits;          // watch keys (jump keys) judged present with high confidence
    PitchSet pitches;            // best-effort "what was played", for display only
};

// Per-key spectral fingerprints captured from the user's own piano + mic.
class Calibration
{
public:
    Calibration();
    ~Calibration();
    Calibration (const Calibration&);
    Calibration& operator= (const Calibration&);

    bool hasKey (int pitch) const;
    int  numKeys() const;
    void clear();

    juce::MemoryBlock toBinary() const;
    bool fromBinary (const juce::MemoryBlock&);

    struct Impl;
    std::unique_ptr<Impl> impl;
};

class NoteDetector
{
public:
    NoteDetector();
    ~NoteDetector();

    void prepare (double sampleRate, int maxBlockSize);
    void reset();

    // Learn mode: the pitches the song is waiting for (may be empty).
    void setExpected (const PitchSet& pitches);
    // Jump keys: reported in DetectedOnset::watchHits only with high confidence.
    void setWatchKeys (const PitchSet& pitches);
    // 0 = only very clear notes, 1 = trigger-happy. Default 0.6.
    void setSensitivity (float s);
    void setCalibration (const Calibration& c);

    // Feed mono microphone samples. Writes up to maxOut onsets, returns how many.
    int process (const float* mono, int numSamples, DetectedOnset* out, int maxOut);

    // Smoothed input level in dBFS, for an input meter.
    float getInputLevelDb() const noexcept;

    // Developer hook for the eval tool: override one tuning constant by name (e.g. "expectedMin"),
    // applied on top of the sensitivity mapping. Returns false for unknown names. Not for the app.
    bool setTuningOverride (const juce::String& name, float value);

    // --- Calibration capture: the user plays one key, we store its fingerprint. ---
    // Starts listening for the next clear onset and assigns it to `pitch`.
    void beginCapture (int pitch);
    // True once the capture for the current pitch finished (then copy it into a Calibration).
    bool captureFinished() const noexcept;
    void storeCapture (Calibration& into) const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl;
};

} // namespace fall
