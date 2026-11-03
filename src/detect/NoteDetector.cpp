// Single-note piano detector: loudness gate + strike detection + aubio's YIN-FFT pitch tracker.
// See NoteDetector.h and docs/detection.md.
#include "NoteDetector.h"

#include <aubio.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>
#ifdef FALL_DETECT_TRACE
 #include <cstdio>
 #include <cstdlib>
#endif

namespace fall
{

namespace
{
    constexpr double kHopSeconds      = 1.0 / 187.5; // ~5.3 ms (256 samples at 48 kHz)
    constexpr double kPitchWindowSecs = 0.085;       // YIN-FFT window (4096 samples at 48 kHz)
    constexpr double kRefractorySecs  = 0.09;        // no new strike this soon after the last one
    constexpr double kSameKeySecs     = 0.16;        // the same key again this soon: one strike rising in steps
    constexpr double kDeadlineSecs    = 0.50;        // a strike must show a steady pitch within this (bass notes are slow)
    constexpr double kFloorUpDbPerSec = 1.0;         // room level estimate: rises slowly...
    constexpr double kFloorDownDbPerSec = 10.0;      // ...and falls quickly
    constexpr float  kSnap = 0.65f;                   // semitones, see keyFor()
    constexpr int    kHist = 16;                     // hop levels kept for the strike test

    float toDb (double power) noexcept { return (float) (10.0 * std::log10 (power + 1e-12)); }

    struct HighPass // RBJ biquad, Q = 0.707
    {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        void set (double hz, double fs)
        {
            const double w = 2.0 * 3.14159265358979323846 * hz / fs, cw = std::cos (w), al = std::sin (w) / (2.0 * 0.7071);
            const double a0 = 1.0 + al;
            b0 = (float) ((1.0 + cw) / 2.0 / a0); b1 = (float) (-(1.0 + cw) / a0); b2 = b0;
            a1 = (float) (-2.0 * cw / a0); a2 = (float) ((1.0 - al) / a0);
        }
        float operator() (float x)
        {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };

    // Ring of per-hop powers, read back in dB.
    struct History
    {
        std::array<double, kHist> power {};
        void   put (int64_t hop, double p) { power[(size_t) (hop % kHist)] = p; }
        double at (int64_t hop) const { return power[(size_t) ((hop + kHist * 1000) % kHist)]; }
        float  db (int64_t hop, int n) const // mean over the n hops ending at `hop`
        {
            double p = 0;
            for (int i = 0; i < n; ++i) p += at (hop - i);
            return toDb (p / n);
        }
        // lowest 2-hop level in the hops 3..12 before `hop`: the level just before a strike
        float before (int64_t hop) const
        {
            float b = 1000.0f;
            for (int i = 3; i <= 12; ++i) b = std::min (b, db (hop - i, 2));
            return b;
        }
    };
}

struct NoteDetector::Impl
{
    // ---- setup ----
    double fs = 48000.0;
    int    hop = 256;
    int    decim = 1; // pitch tracker runs at fs / decim
    aubio_pitch_t* pitch = nullptr;
    fvec_t* pitchIn = nullptr;
    fvec_t* pitchOut = nullptr;
    HighPass hpLevel, hpAttack; // 60 Hz (level without rumble), 2 kHz (hammer noise)

    // ---- settings ----
    PitchSet expected, watch;
    float sensitivity = 0.6f;
    float gateDb = 10.0f, riseDb = 5.6f, minConf = 0.55f;
    int   needHops = 6, needWatchHops = 12, deadlineHops = 56, earlyHops = 8;
    float floorUp = 0, floorDown = 0;

    // ---- state ----
    std::vector<float> hopBuf;
    int     hopFill = 0;
    int64_t pos = 0;     // samples consumed
    int64_t hopIdx = 0;
    History level, attack;
    float   floorDb = -100.0f;
    bool    floorSet = false;
    int64_t lastStrikePos = -100000000; // samples
    int64_t refractory = 7200;          // samples

    // pitch run (consecutive hops agreeing within half a semitone)
    float runMidi = 0, runSum = 0;
    int   runLen = 0;

    // the strike being examined
    bool     pending = false;
    int64_t  strikeHop = 0, strikePos = 0;
    float    strikeStrength = 0;
    int64_t  strikeGap = 0;  // samples since the previous strike
    int      prevKey = -1;   // the note that was already sounding before the strike
    int      lastKey = -1;   // the key of the latest steady pitch

    // tuning (cents from A440), learned from clear notes
    float tuningCents = 0;
    int   tuningCount = 0;
    std::array<float, 15> tuningSeen {}; // the latest notes' offsets

    std::atomic<float> levelDb { -100.0f }, floorOut { -100.0f }, tuningOut { 0.0f };
    std::atomic<bool>  tuningSure { false };

    DetectedOnset* out = nullptr;
    int outCount = 0, outMax = 0;

    ~Impl() { freePitch(); }

    void freePitch()
    {
        if (pitch != nullptr) del_aubio_pitch (pitch);
        if (pitchIn != nullptr) del_fvec (pitchIn);
        if (pitchOut != nullptr) del_fvec (pitchOut);
        pitch = nullptr; pitchIn = pitchOut = nullptr;
    }

    int hops (double seconds) const { return std::max (1, (int) std::lround (seconds * fs / hop)); }

    void prepare (double sampleRate)
    {
        fs = sampleRate > 0 ? sampleRate : 48000.0;
        // aubio's pitch tracker gets at most ~48 kHz (it finds nothing at 96 kHz): average pairs
        decim = fs > 60000.0 ? (int) std::lround (fs / 48000.0) : 1;
        const double pitchRate = fs / decim;
        hop = std::max (64, (int) std::lround (pitchRate * kHopSeconds)) * decim;
        int win = 1024;
        while (win < pitchRate * kPitchWindowSecs) win *= 2;

        freePitch();
        pitch = new_aubio_pitch ("yinfft", (uint_t) win, (uint_t) (hop / decim), (uint_t) std::lround (pitchRate));
        aubio_pitch_set_unit (pitch, "midi");
        aubio_pitch_set_silence (pitch, -120.0f); // we do our own gating
        pitchIn = new_fvec ((uint_t) (hop / decim));
        pitchOut = new_fvec (1);
        hopBuf.assign ((size_t) hop, 0.0f);

        hpLevel.set (60.0, fs);
        hpAttack.set (2000.0, fs);

        floorUp = (float) (kFloorUpDbPerSec * hop / fs);
        floorDown = (float) (kFloorDownDbPerSec * hop / fs);
        refractory = (int64_t) std::lround (kRefractorySecs * fs);
        deadlineHops = hops (kDeadlineSecs);
        earlyHops = hops (kPitchWindowSecs * 0.5);
        applySensitivity();
        reset();
    }

    void applySensitivity()
    {
        const float s = std::clamp (sensitivity, 0.0f, 1.0f);
        gateDb   = 16.0f - 10.0f * s;
        riseDb   = 8.0f - 4.0f * s;
        minConf  = 0.85f - 0.5f * s;
        // aubio's own threshold, also used by its check for octave errors on high notes
        if (pitch != nullptr) aubio_pitch_set_tolerance (pitch, 1.0f - minConf);
        needHops = hops (0.045 - 0.025 * s);
        needWatchHops = needHops * 2;
    }

    void reset()
    {
        hopFill = 0; pos = 0; hopIdx = 0;
        hpLevel.z1 = hpLevel.z2 = hpAttack.z1 = hpAttack.z2 = 0;
        level = {}; attack = {};
        floorDb = -100.0f; floorSet = false;
        lastStrikePos = -100000000;
        runMidi = runSum = 0; runLen = 0;
        pending = false;
        lastKey = -1;
        levelDb.store (-100.0f);
        floorOut.store (-100.0f);
        if (pitch != nullptr) // flush the tracker's history
        {
            fvec_zeros (pitchIn);
            for (int i = 0; i < 64; ++i) aubio_pitch_do (pitch, pitchIn, pitchOut);
        }
    }

    void hopStep()
    {
        // level of this hop (high-passed, so rumble doesn't count)
        double p = 0, a = 0;
        for (int i = 0; i < hop; ++i)
        {
            const float y = hpLevel (hopBuf[(size_t) i]), h = hpAttack (y);
            p += (double) y * y;
            a += (double) h * h;
        }
        for (int i = 0; i < hop / decim; ++i)
        {
            float sum = 0.0f;
            for (int k = 0; k < decim; ++k) sum += hopBuf[(size_t) (i * decim + k)];
            pitchIn->data[i] = sum / (float) decim;
        }
        level.put (hopIdx, p / hop);
        attack.put (hopIdx, a / hop);

        const float fast = level.db (hopIdx, 2);  // ~10 ms
        const float slow = level.db (hopIdx, 8);  // ~40 ms

        // room level: follows quiet stretches quickly, sustained notes only slowly
        if (! floorSet) { if (hopIdx >= 8) { floorDb = slow; floorSet = true; } }
        else if (slow > floorDb) floorDb += floorUp;
        else floorDb = std::max (slow, floorDb - floorDown);
        floorOut.store (floorDb, std::memory_order_relaxed);
        const float meter = levelDb.load (std::memory_order_relaxed);
        levelDb.store (slow > meter ? slow : meter + 0.15f * (slow - meter), std::memory_order_relaxed);

        // pitch run
        aubio_pitch_do (pitch, pitchIn, pitchOut);
        // only confident pitch counts (the thump of a finger on a key, noise, clicks: low confidence)
        const float m = aubio_pitch_get_confidence (pitch) >= minConf ? pitchOut->data[0] : 0.0f;
        if (m > 0.0f && runLen > 0 && std::abs (m - runMidi) < 0.5f) { ++runLen; runSum += m; }
        else if (m > 0.0f) { runMidi = m; runSum = m; runLen = 1; }
        else runLen = 0;
        if (runLen > 0) runMidi = runSum / (float) runLen;

        // strike: loud enough above the room AND a sudden jump in level, or (a softer strike of a
        // key that is still ringing) a big burst of hammer noise above 2 kHz
        const float before = level.before (hopIdx);
        const float attackRise = attack.db (hopIdx, 2) - attack.before (hopIdx);
       #ifdef FALL_DETECT_TRACE
        {
            static const double t0 = std::getenv ("FALL_T0") ? std::atof (std::getenv ("FALL_T0")) : 1e9;
            static const double t1 = std::getenv ("FALL_T1") ? std::atof (std::getenv ("FALL_T1")) : -1.0;
            const double now = (double) pos / fs;
            if (now >= t0 && now <= t1)
                std::fprintf (stderr, "[hop] t=%.3f fast=%.1f floor=%.1f before=%.1f midi=%.2f run=%d conf=%.2f att=%.1f\n", now, fast, floorDb, before, m, runLen, (double) aubio_pitch_get_confidence (pitch), attackRise);
        }
       #endif
        const bool loud = floorSet && fast >= floorDb + gateDb;
        if (loud && pos - hop - lastStrikePos >= refractory && (fast - before >= riseDb || attackRise >= 2.0f * riseDb))
        {
            prevKey = runLen >= needHops ? keyFor (runMidi) : lastKey;
            startStrike (pos - hop, fast);
            runLen = 0; // only pitch heard after the strike counts
        }
        // Legato: a new key can start without a clear jump in level (the old note was still loud).
        // A pitch that differs from the note that was sounding and holds steady for twice as long
        // counts as a strike too.
        if (runLen >= 2 * needHops)
        {
            const int key = keyFor (runMidi);
            // (a decaying note can flip to its octave: not a new note)
            const bool octave = lastKey >= 0 && key >= 0 && std::abs (key - lastKey) % 12 == 0;
            const int64_t at = pos - (int64_t) runLen * hop;
            if (loud && ! pending && lastKey >= 0 && key >= 0 && key != lastKey && ! octave && at - lastStrikePos >= refractory)
            {
                prevKey = lastKey;
                startStrike (at, fast);
            }
            if (key >= 0) lastKey = key;
        }
        else if (fast < floorDb + 3.0f) // back to the room's level: nothing is sounding
            lastKey = -1;

        if (pending)
        {
            const int64_t since = hopIdx - strikeHop;
            if (since > deadlineHops)
                pending = false;
            else if (runLen >= needHops)
            {
                const int key = keyFor (runMidi);
                // the note that was already sounding, still in the pitch window
                const bool stale = key == prevKey && since < earlyHops;
                // the same key right after its strike: that strike's level rising in steps, not a new one
                const bool sameStrike = key == prevKey && strikeGap < (int64_t) (kSameKeySecs * fs);
                const bool isWatch = key >= 0 && watch.test ((size_t) key);
                if (sameStrike && ! stale)
                    pending = false;
                else if (key >= 0 && ! stale && (! isWatch || runLen >= needWatchHops))
                {
                    pending = false; // one note per strike
                    lastKey = key;
                    learnTuning (key, runMidi);
                    emit (key);
                }
            }
        }
        ++hopIdx;
    }

    void startStrike (int64_t at, float fast)
    {
        strikeGap = at - lastStrikePos;
        pending = true;
        strikeHop = hopIdx;
        strikePos = std::max<int64_t> (0, at);
        strikeStrength = std::clamp ((fast - floorDb) / 30.0f, 0.0f, 2.0f);
        lastStrikePos = strikePos;
       #ifdef FALL_DETECT_TRACE
        std::fprintf (stderr, "[strike] at %.3f (now %.3f) level %.1f floor %.1f prev %d\n", (double) strikePos / fs, (double) pos / fs, fast, floorDb, prevKey);
       #endif
    }

    // The key for a measured pitch: the nearest key, or an expected key if the pitch is within
    // kSnap of it (high piano notes are often tuned/measured sharp).
    int keyFor (float midi) const
    {
        const float m = midi - tuningCents / 100.0f;
        int k = (int) std::lround (m);
        for (int e : { k - 1, k + 1 })
            if (e >= 0 && e < 128 && expected.test ((size_t) e) && ! expected.test ((size_t) k) && std::abs (m - (float) e) < kSnap)
                k = e;
        return (k >= kLowestPianoKey && k <= kHighestPianoKey) ? k : -1;
    }

    void learnTuning (int key, float midi)
    {
        if (key < 48 || key > 84) // the extremes are stretched; the middle tells the tuning
            return;
        // Offset from the nearest A440 semitone (not from `key`, which was chosen with the current
        // estimate and would reinforce it). The median ignores attacks that read sharp.
        tuningSeen[(size_t) (tuningCount++ % (int) tuningSeen.size())] = (midi - std::round (midi)) * 100.0f;
        std::array<float, 15> v = tuningSeen;
        const auto n = (size_t) std::min (tuningCount, (int) v.size());
        std::nth_element (v.begin(), v.begin() + (std::ptrdiff_t) (n / 2), v.begin() + (std::ptrdiff_t) n);
        tuningCents = v[n / 2];
        tuningOut.store (tuningCents, std::memory_order_relaxed);
        tuningSure.store (tuningCount >= 5, std::memory_order_relaxed);
    }

    void emit (int key)
    {
        if (out == nullptr || outCount >= outMax) return;
        DetectedOnset& o = out[outCount++];
        o = DetectedOnset();
        o.samplePosition = strikePos;
        o.reportedAt = pos;
        o.strength = strikeStrength;
        o.pitches.set ((size_t) key);
        if (expected.test ((size_t) key)) o.expectedHits.set ((size_t) key);
        if (watch.test ((size_t) key))    o.watchHits.set ((size_t) key);
    }
};

NoteDetector::NoteDetector() : impl (std::make_unique<Impl>()) { impl->prepare (48000.0); }
NoteDetector::~NoteDetector() = default;

void NoteDetector::prepare (double sampleRate, int) { impl->prepare (sampleRate); }
void NoteDetector::reset() { impl->reset(); }
void NoteDetector::setExpected (const PitchSet& p) { impl->expected = p; }
void NoteDetector::setWatchKeys (const PitchSet& p) { impl->watch = p; }

void NoteDetector::setSensitivity (float s)
{
    impl->sensitivity = s;
    impl->applySensitivity();
}

int NoteDetector::process (const float* mono, int numSamples, DetectedOnset* out, int maxOut)
{
    auto& d = *impl;
    d.out = out; d.outCount = 0; d.outMax = maxOut;
    for (int i = 0; i < numSamples; ++i)
    {
        d.hopBuf[(size_t) d.hopFill++] = mono[i];
        ++d.pos;
        if (d.hopFill == d.hop)
        {
            d.hopFill = 0;
            d.hopStep();
        }
    }
    d.out = nullptr;
    return d.outCount;
}

float NoteDetector::getInputLevelDb() const noexcept { return impl->levelDb.load (std::memory_order_relaxed); }
float NoteDetector::getNoiseFloorDb() const noexcept { return impl->floorOut.load (std::memory_order_relaxed); }

float NoteDetector::getTuningCents (bool* confident) const noexcept
{
    if (confident != nullptr) *confident = impl->tuningSure.load (std::memory_order_relaxed);
    return impl->tuningOut.load (std::memory_order_relaxed);
}

} // namespace fall
