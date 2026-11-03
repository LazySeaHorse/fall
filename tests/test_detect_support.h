#pragma once

// Synthetic "piano heard through a laptop microphone" for the detector tests, so CI needs no
// audio files: inharmonic partials with per-partial exponential decay, two slightly detuned
// strings (beating), a hammer noise burst, then a crude laptop-mic chain (high-pass, low-pass,
// small-room reverb, background noise).

#include "detect/NoteDetector.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace fall::testing
{

constexpr double kPi = 3.14159265358979323846;

struct SynthNote
{
    int    pitch;
    double start;      // seconds
    double duration;   // key held this long (the string then decays quickly)
    float  velocity = 0.7f; // 0..1
    double cents = 0.0;     // detuning of this string from A440 equal temperament
};

struct Environment
{
    double sampleRate = 48000.0;
    double noiseDb    = -55.0;  // pink-ish noise level (RMS dBFS)
    double reverbMix  = 0.25;   // 0 = dry
    bool   laptopMic  = true;   // 150 Hz high-pass, 8 kHz low-pass
    double gainDb     = 0.0;    // overall level change (tests level independence)
    uint32_t seed     = 1;
};

class Rng
{
public:
    explicit Rng (uint32_t s) : state (s * 2654435761u + 12345u) {}
    float uniform() { state = state * 1664525u + 1013904223u; return (float) (state >> 8) / 16777216.0f; }
    float gauss()   { float s = 0; for (int i = 0; i < 6; ++i) s += uniform(); return (s - 3.0f) * 1.4142f; }
private:
    uint32_t state;
};

inline double hz (int pitch) { return 440.0 * std::pow (2.0, (pitch - 69) / 12.0); }

inline void addPianoNote (std::vector<float>& buf, const SynthNote& n, double sr, Rng& rng)
{
    const double f0 = hz (n.pitch) * std::pow (2.0, n.cents / 1200.0);
    // Inharmonicity, capped in the top octaves: uncapped, this curve makes C7 read ~55 cents sharp
    // through a pitch tracker, while a real grand recorded through a laptop mic reads within ~15
    // cents (C7 96.11, C8 108.1 in a user's recording).
    const double B  = n.pitch < 45 ? 1.5e-4 : std::min (2.5e-3, 8e-5 * std::pow (2.0, (n.pitch - 45) / 8.0));
    // sustain time depends on register: bass strings ring for many seconds, treble ones die quickly
    const double tau0 = 6.0 * std::pow (2.0, -(n.pitch - 21) / 22.0) + 0.5;
    const double amp  = 0.02 + 0.25 * (double) n.velocity * (double) n.velocity;
    const size_t start = (size_t) std::llround (n.start * sr);
    const size_t len = std::min (buf.size() - start, (size_t) (sr * (n.duration + 3.0)));
    const double release = n.start + n.duration;

    for (int h = 1; h <= 60; ++h)
    {
        const double f = f0 * h * std::sqrt (1.0 + B * h * h);
        if (f > 0.45 * sr)
            break;
        // bass fundamentals are weak in real pianos; upper partials fall off with a soft slope
        double a = amp / std::pow ((double) h, 0.9) * (0.7 + 0.6 * rng.uniform());
        if (n.pitch < 40 && h < 3)
            a *= 0.2;
        const double tau = tau0 / (1.0 + 0.25 * h);
        const double detune = 1.0 + (rng.uniform() - 0.5) * 0.0016; // second string
        const double ph0 = rng.uniform() * 6.2831853, ph1 = rng.uniform() * 6.2831853;
        const double w0 = 2.0 * kPi * f / sr, w1 = w0 * detune;
        for (size_t i = 0; i < len; ++i)
        {
            const double t = (double) i / sr;
            double env = std::exp (-t / tau) * std::min (1.0, t / 0.002);
            if (n.start + t > release) // damper falls
                env *= std::exp (-(n.start + t - release) / 0.12);
            if (env < 1e-5 && t > 0.05)
                break;
            buf[start + i] += (float) (a * env * 0.5 * (std::sin (w0 * (double) i + ph0) + std::sin (w1 * (double) i + ph1)));
        }
    }

    // hammer thump + click: short band-limited noise burst
    float lp = 0;
    const size_t nl = std::min (len, (size_t) (sr * 0.03));
    for (size_t i = 0; i < nl; ++i)
    {
        lp += 0.35f * (rng.gauss() - lp);
        buf[start + i] += (float) (amp * 0.15 * lp * std::exp (-(double) i / (sr * 0.006)));
    }
}

// Schroeder-style small room: 4 parallel combs + 2 all-passes.
inline void addReverb (std::vector<float>& x, double sr, double mix)
{
    if (mix <= 0.0)
        return;
    const int delays[4] = { 1557, 1617, 1491, 1422 };
    const float fb[4]   = { 0.84f, 0.83f, 0.85f, 0.82f };
    std::vector<float> wet (x.size(), 0.0f);
    for (int c = 0; c < 4; ++c)
    {
        const size_t d = (size_t) std::llround (delays[c] * sr / 44100.0);
        std::vector<float> line (d, 0.0f);
        float damp = 0;
        size_t idx = 0;
        for (size_t i = 0; i < x.size(); ++i)
        {
            const float y = line[idx];
            damp += 0.45f * (y - damp);
            line[idx] = x[i] + fb[c] * damp;
            idx = (idx + 1) % d;
            wet[i] += 0.25f * y;
        }
    }
    for (int a = 0; a < 2; ++a)
    {
        const size_t d = (size_t) std::llround ((a == 0 ? 225 : 556) * sr / 44100.0);
        std::vector<float> line (d, 0.0f);
        size_t idx = 0;
        for (size_t i = 0; i < wet.size(); ++i)
        {
            const float in = wet[i], z = line[idx];
            const float out = -0.5f * in + z;
            line[idx] = in + 0.5f * out;
            idx = (idx + 1) % d;
            wet[i] = out;
        }
    }
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = (float) ((1.0 - 0.5 * mix) * x[i] + mix * 1.2 * wet[i]);
}

// Two-pole biquad helpers (RBJ) so we don't depend on JUCE here.
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    static Biquad highpass (double fc, double sr)
    {
        const double w = 2 * kPi * fc / sr, c = std::cos (w), a = std::sin (w) / (2 * 0.7071);
        Biquad q; const double a0 = 1 + a;
        q.b0 = (1 + c) / 2 / a0; q.b1 = -(1 + c) / a0; q.b2 = (1 + c) / 2 / a0; q.a1 = -2 * c / a0; q.a2 = (1 - a) / a0;
        return q;
    }
    static Biquad lowpass (double fc, double sr)
    {
        const double w = 2 * kPi * fc / sr, c = std::cos (w), a = std::sin (w) / (2 * 0.7071);
        Biquad q; const double a0 = 1 + a;
        q.b0 = (1 - c) / 2 / a0; q.b1 = (1 - c) / a0; q.b2 = (1 - c) / 2 / a0; q.a1 = -2 * c / a0; q.a2 = (1 - a) / a0;
        return q;
    }
    float process (float x)
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return (float) y;
    }
};

inline std::vector<float> renderScene (const std::vector<SynthNote>& notes, double totalSeconds, const Environment& env)
{
    Rng rng (env.seed);
    std::vector<float> x ((size_t) std::llround (totalSeconds * env.sampleRate), 0.0f);
    for (auto& n : notes)
        addPianoNote (x, n, env.sampleRate, rng);

    addReverb (x, env.sampleRate, env.reverbMix);

    if (env.laptopMic)
    {
        Biquad hp1 = Biquad::highpass (150.0, env.sampleRate), hp2 = hp1, lp1 = Biquad::lowpass (8000.0, env.sampleRate);
        for (auto& s : x)
            s = lp1.process (hp2.process (hp1.process (s)));
    }

    // background noise (pink-ish: white through a one-pole low-pass) at the requested level
    const float noiseGain = (float) std::pow (10.0, env.noiseDb / 20.0) * 3.0f;
    float lp = 0;
    for (auto& s : x)
    {
        lp += 0.15f * (rng.gauss() - lp);
        s += noiseGain * lp;
    }

    const float g = (float) std::pow (10.0, env.gainDb / 20.0);
    for (auto& s : x)
        s *= g;
    return x;
}

// Streams audio through a detector in blocks and collects the reports.
struct Report
{
    double onsetTime;      // seconds
    double reportedAt;     // seconds
    double blockEnd;       // seconds: when the host would have received it
    DetectedOnset onset;
};

inline std::vector<Report> stream (NoteDetector& det, const std::vector<float>& audio, double sr, int blockSize)
{
    std::vector<Report> out;
    std::vector<DetectedOnset> buf (32);
    size_t i = 0;
    while (i < audio.size())
    {
        const int n = (int) std::min<size_t> ((size_t) blockSize, audio.size() - i);
        const int cnt = det.process (audio.data() + i, n, buf.data(), (int) buf.size());
        i += (size_t) n;
        for (int k = 0; k < cnt; ++k)
            out.push_back ({ (double) buf[(size_t) k].samplePosition / sr, (double) buf[(size_t) k].reportedAt / sr, (double) i / sr, buf[(size_t) k] });
    }
    return out;
}

inline PitchSet setOf (std::initializer_list<int> pitches)
{
    PitchSet s;
    for (int p : pitches)
        s.set ((size_t) p);
    return s;
}

} // namespace fall::testing
