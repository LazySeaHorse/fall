#pragma once

// Internal helpers shared by the detector's translation units. Not part of the public API.

#include "NoteDetector.h"

#include <array>
#include <cmath>

namespace fall::detect
{

constexpr int kNumKeys     = kHighestPianoKey - kLowestPianoKey + 1; // 88
constexpr int kMaxPartials = 48;

inline int keyIndex (int pitch) noexcept { return pitch - kLowestPianoKey; }
inline bool isPianoKey (int pitch) noexcept { return pitch >= kLowestPianoKey && pitch <= kHighestPianoKey; }

inline double pitchToHz (int pitch, double cents = 0.0) noexcept
{
    return 440.0 * std::pow (2.0, (double (pitch - 69) * 100.0 + cents) / 1200.0);
}

// Typical grand-piano inharmonicity coefficient B (f_h = h f0 sqrt (1 + B h^2)); measured on a
// few real/sampled pianos: roughly flat in the bass, doubling every ~8 semitones above the break.
inline double inharmonicityB (int pitch) noexcept
{
    if (pitch < 45)
        return 1.2e-4;
    return 8e-5 * std::pow (2.0, double (pitch - 45) / 8.0);
}

inline double partialHz (double f0, double B, int h) noexcept
{
    return f0 * h * std::sqrt (1.0 + B * h * h);
}

// The partial series we look for for one key: either the generic piano model, a measured
// fingerprint (Calibration) or an interpolation between measured neighbours.
struct KeyPartials
{
    int   n = 0;
    float hz[kMaxPartials] {};   // expected centre frequency
    float lo[kMaxPartials] {};   // search window (Hz)
    float hi[kMaxPartials] {};
    float w[kMaxPartials] {};    // weight; sums to 1
    int   h[kMaxPartials] {};    // harmonic number
    bool  measured = false;      // built from a calibration capture (sharper, so judged a bit more leniently)
    float chance = 0.0f;         // 1 / sqrt (effective number of partials): how easily noise alone fills the series
};

// Builds the partial series for all 88 keys: the generic piano model for uncalibrated keys
// (or no calibration at all), measured partials for calibrated keys and, in between, the generic
// model bent towards the nearest measured neighbours (tuning, inharmonicity, weights).
void buildKeyModels (double sampleRate, const Calibration::Impl* cal, std::array<KeyPartials, kNumKeys>& out);

// Only the generic model, exposed for tests/tools.
void buildGenericModel (int pitch, double sampleRate, KeyPartials& out);

} // namespace fall::detect

namespace fall
{

struct Calibration::Impl
{
    struct Key
    {
        bool  valid = false;
        int   n = 0;
        float tuneCents = 0.0f;
        float B = 0.0f;
        uint8_t h[detect::kMaxPartials] {};
        float hz[detect::kMaxPartials] {};
        float w[detect::kMaxPartials] {};   // 0.25 .. 1
    };

    std::array<Key, detect::kNumKeys> keys {};
};

} // namespace fall
