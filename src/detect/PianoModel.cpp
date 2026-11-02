#include "DetectInternal.h"

#include <algorithm>

namespace fall::detect
{

namespace
{
    constexpr double kGenericTolCents = 28.0;
    constexpr double kCalTolCents     = 22.0;
    constexpr double kMinWeight       = 0.04;

    double smoothstep (double a, double b, double x) noexcept
    {
        const double t = std::clamp ((x - a) / (b - a), 0.0, 1.0);
        return t * t * (3.0 - 2.0 * t);
    }

    double centsRatio (double cents) noexcept { return std::pow (2.0, cents / 1200.0); }

    // How well a typical laptop mic + room carries energy at f (rolls off below ~250 Hz and
    // above ~6 kHz). Only used to weight the generic model; measured models learn it.
    double micResponse (double f, double sampleRate) noexcept
    {
        if (f > 0.45 * sampleRate)
            return 0.0;
        return smoothstep (120.0, 300.0, f) * (1.0 - 0.85 * smoothstep (4500.0, 9500.0, f));
    }

    double genericWeight (int h, double f, double sampleRate) noexcept
    {
        return std::pow ((double) h, -0.6) * micResponse (f, sampleRate);
    }

    void normalise (KeyPartials& kp) noexcept
    {
        double sum = 0.0;
        for (int i = 0; i < kp.n; ++i)
            sum += (double) kp.w[i];
        if (sum <= 0.0)
        {
            kp.n = 0;
            return;
        }
        double sq = 0.0;
        for (int i = 0; i < kp.n; ++i)
        {
            kp.w[i] = (float) ((double) kp.w[i] / sum);
            sq += (double) kp.w[i] * (double) kp.w[i];
        }
        kp.chance = (float) std::sqrt (sq); // = 1 / sqrt (n_eff) for weights summing to one
    }

    // Generic model, optionally bent: tuneCents shifts everything, B overrides the default.
    void buildBent (int pitch, double sampleRate, double tuneCents, double B, double calWeightMix,
                    const float* calW /* indexed by harmonic number, may be null */, KeyPartials& out)
    {
        const double f0 = pitchToHz (pitch, tuneCents);
        out.n = 0;
        for (int h = 1; h <= kMaxPartials * 2 && out.n < kMaxPartials; ++h)
        {
            const double f = partialHz (f0, B, h);
            double w = genericWeight (h, f, sampleRate);
            if (calW != nullptr)
            {
                const double wc = h <= 64 ? (double) calW[h] : 0.0;
                w = (1.0 - calWeightMix) * w + calWeightMix * wc * micResponse (f, sampleRate) * 1.2;
            }
            if (w < kMinWeight)
                continue;
            const int i = out.n++;
            out.h[i]  = h;
            out.hz[i] = (float) f;
            out.lo[i] = (float) (partialHz (f0, B * 0.3, h) / centsRatio (kGenericTolCents));
            out.hi[i] = (float) (partialHz (f0, B * 2.2, h) * centsRatio (kGenericTolCents));
            out.w[i]  = (float) w;
        }
        normalise (out);
    }

    void buildFromCal (int pitch, const Calibration::Impl::Key& k, KeyPartials& out)
    {
        (void) pitch;
        out.n = 0;
        for (int j = 0; j < k.n && out.n < kMaxPartials; ++j)
        {
            if (k.w[j] <= 0.0f)
                continue;
            const int i = out.n++;
            out.h[i]  = k.h[j];
            out.hz[i] = k.hz[j];
            out.lo[i] = (float) (k.hz[j] / centsRatio (kCalTolCents));
            out.hi[i] = (float) (k.hz[j] * centsRatio (kCalTolCents));
            out.w[i]  = k.w[j];
        }
        normalise (out);
    }
}

void buildGenericModel (int pitch, double sampleRate, KeyPartials& out)
{
    buildBent (pitch, sampleRate, 0.0, inharmonicityB (pitch), 0.0, nullptr, out);
}

void buildKeyModels (double sampleRate, const Calibration::Impl* cal, std::array<KeyPartials, kNumKeys>& out)
{
    for (int idx = 0; idx < kNumKeys; ++idx)
    {
        const int pitch = kLowestPianoKey + idx;
        const Calibration::Impl::Key* here = (cal != nullptr && cal->keys[(size_t) idx].valid) ? &cal->keys[(size_t) idx] : nullptr;

        if (here != nullptr)
        {
            buildFromCal (pitch, *here, out[(size_t) idx]);
            if (out[(size_t) idx].n >= 3)
                continue;
        }

        // Nearest measured neighbours below and above.
        int below = -1, above = -1;
        if (cal != nullptr)
        {
            for (int j = idx - 1; j >= 0; --j) if (cal->keys[(size_t) j].valid) { below = j; break; }
            for (int j = idx + 1; j < kNumKeys; ++j) if (cal->keys[(size_t) j].valid) { above = j; break; }
        }

        if (below < 0 && above < 0)
        {
            buildGenericModel (pitch, sampleRate, out[(size_t) idx]);
            continue;
        }

        // Interpolate tuning, log-B and per-harmonic weights between the neighbours.
        double tune = 0.0, logB = 0.0, t = 0.0;
        float wTab[65] {};
        auto addNeighbour = [&] (int j, double share)
        {
            auto& k = cal->keys[(size_t) j];
            tune += share * k.tuneCents;
            logB += share * std::log (std::max (1e-6, (double) k.B));
            for (int i = 0; i < k.n; ++i)
                if (k.h[i] <= 64)
                    wTab[k.h[i]] += (float) (share * k.w[i]);
        };
        if (below >= 0 && above >= 0)
        {
            t = double (idx - below) / double (above - below);
            addNeighbour (below, 1.0 - t);
            addNeighbour (above, t);
        }
        else
        {
            addNeighbour (below >= 0 ? below : above, 1.0);
        }
        const double B = std::clamp (std::exp (logB), 2e-5, 3e-2);
        buildBent (pitch, sampleRate, tune, B, 0.6, wTab, out[(size_t) idx]);
        if (out[(size_t) idx].n == 0)
            buildGenericModel (pitch, sampleRate, out[(size_t) idx]);
    }
}

} // namespace fall::detect
