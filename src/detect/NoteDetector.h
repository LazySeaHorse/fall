#pragma once

// Real-time piano note detection from one microphone: one note at a time (no chords).
//
//   1. Loudness gate: a note must start clearly above the room's noise level (tracked
//      continuously), and the level must jump (a strike), so silence can never trigger anything.
//   2. Pitch: aubio's YIN-FFT pitch tracker. After a strike, the first pitch that stays steady for
//      a few milliseconds is the note that was played.
//   3. Learn mode: that note counts if it is one the song is waiting for. Jump keys must hold
//      their pitch a little longer.
//
// Threading: process() runs on the audio thread and does not allocate, lock or do I/O.
// prepare() allocates and is called while audio is stopped (or under the engine's lock).

#include "core/Song.h"

#include <cstdint>
#include <memory>

namespace fall
{

struct DetectedOnset
{
    int64_t  samplePosition = 0; // where the strike began in the detector's input stream
    int64_t  reportedAt     = 0; // stream position when it was reported (reportedAt - samplePosition = latency)
    float    strength       = 0; // how far above the room noise the strike was, roughly 0..1+
    PitchSet expectedHits;       // the heard note, if the song was waiting for it (learn mode)
    PitchSet watchHits;          // the heard note, if it is a jump key
    PitchSet pitches;            // the heard note (display)
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
    // Jump keys: reported in DetectedOnset::watchHits.
    void setWatchKeys (const PitchSet& pitches);
    // 0 = only loud, clear notes; 1 = trigger-happy. Default 0.6.
    void setSensitivity (float s);

    // Feed mono microphone samples. Writes up to maxOut detections, returns how many.
    int process (const float* mono, int numSamples, DetectedOnset* out, int maxOut);

    // Smoothed input level and the room noise level, in dBFS (for meters / logs).
    float getInputLevelDb() const noexcept;
    float getNoiseFloorDb() const noexcept;

    // The piano's tuning relative to A440, in cents (-50..50), from the notes heard so far.
    // `confident` is false until a few clear notes were heard.
    float getTuningCents (bool* confident = nullptr) const noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> impl;
};

} // namespace fall
