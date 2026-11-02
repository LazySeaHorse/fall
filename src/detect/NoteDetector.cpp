// Real-time piano note detector. See docs/detection.md for the full story; in short:
//
//   mic -> ring buffer -> short STFT every 256 samples -> log-magnitude spectral flux (global, plus a
//   "comb flux" restricted to the expected pitches' partials) -> adaptive median/MAD threshold ->
//   onset. At fixed delays after each onset (~30, ~65, ~105 ms) we take a longer FFT that ends
//   there, subtract the power spectrum from just BEFORE the onset (so only new energy remains),
//   turn it into a per-bin "evidence" value (peak above the local continuum, in dB) and score
//   every key by the weighted fraction of its partials (inharmonic series, or measured ones from
//   the calibration) that show evidence. Expected keys are accepted at a low score; jump keys need
//   a high score AND must survive a greedy explain-and-subtract pass; the same greedy pass gives
//   the display pitches.
#include "DetectInternal.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace fall
{
using namespace detect;

namespace
{
    constexpr int kNumStages   = 4;   // 0..2 normal evaluations, 3 = calibration capture only
    constexpr int kMaxPending  = 8;
    constexpr int kHistFrames  = 320; // ~1.7 s of short-STFT history at 48k/256
    constexpr int kStatFrames  = 256; // frames used for the adaptive threshold statistics
    constexpr int kFluxLag     = 2;   // frames
    constexpr int kMaxPicks    = 8;
    constexpr int64_t kNever   = std::numeric_limits<int64_t>::max();

    int nextPow2 (double x)
    {
        int n = 64;
        while ((double) n < x)
            n <<= 1;
        return n;
    }

    int log2Int (int n)
    {
        int o = 0;
        while ((1 << o) < n)
            ++o;
        return o;
    }

    inline float clamp01 (float x) noexcept { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
}

struct NoteDetector::Impl
{
    // ---------------------------------------------------------------------------------------------
    // Tunables (derived from sensitivity)
    struct Tuning
    {
        float zOnset      = 5.0f;   // global flux z-score needed for an onset
        float zComb       = 4.0f;   // comb flux z-score (expected pitches only)
        float promLo = 5.0f, promHi = 15.0f;  // peak prominence over the local floor (dB): evidence 0 .. 1
        float newLo  = 6.0f, newHi  = 18.0f;  // level rise vs. just before the onset (dB): evidence 0 .. 1
        float expectedMin = 0.30f;  // partial fraction for an expected key
        float watchMin    = 0.62f;  // partial fraction for a jump key
        float displayMin  = 0.40f;  // partial fraction for a display pitch
        float chanceCoef = 0.45f;       // chance level subtracted from the score of keys with few partials: coef / sqrt (n_eff)
        float watchSel = 0.40f;         // ... and the chance-corrected score a jump key needs
        float subharmonicMin = 0.34f;   // a lower key scoring this explains a jump key's partials away
        float displaySel = 0.82f;       // chance-corrected score a display pitch needs
        float stage1Bump = 0.0f, stage2Bump = 0.0f; // extra score required by the later (second-chance) evaluations
        float watchIndependent = 0.75f;  // score over the partials a lower note cannot produce (rich jump keys)
        float watchPrefilter = 0.3f;    // stage-2 score a jump key needs for the late stage to run at all
        float measuredScale = 0.75f;    // calibrated keys: threshold multiplier (their templates are sharper)
        float watchStrictDb = 6.0f;     // extra prominence/newness (dB) demanded for jump keys
        float contrastMin = -1.0f;      // expected key must beat the median of its non-expected neighbours by this much
        float weakOnsetPenalty = 0.3f; // extra score an onset of strength 1.0 (x threshold) must show
        float expectedNewRelief = 8.0f; // dB of extra tolerance on newness for expected keys
        float settleStrength = 2.2f;  // onset strength (x threshold) needed within kSettleMs of the previous onset
        float watchContrast = 0.35f; // jump key must beat its neighbours' score by this much
        float neighbourMargin = 0.15f; // a non-expected adjacent key must beat an expected one by this much to veto it
        // Tonality: a real partial is ONE stable sinusoid, so the reassigned frequency of the peak bin and
        // of its neighbours agree. Noise (snaps, claps, hammer thump, reverb tails) has peaks too, but
        // their neighbours point elsewhere. Disagreement (bins) up to tonalTol is fine, tonalTol + tonalSoft is noise.
        float tonalTol = 10.0f, tonalSoft = 0.25f; // (off by default: too weak at onsets, see docs)
        // Explanation: of the new tonal peaks in an expected key's frequency range, at least this fraction
        // (evidence-weighted) must sit on partials of the expected chord. Voices, claps and wrong notes
        // leave most of their peaks unexplained.
        float explainMin = 0.0f;
        float explainFloor = 1.5f;     // below this much total new evidence in the range the check is skipped
        // Precision ("tuner") check for expected keys: fit one tuning offset + one inharmonicity scale to
        // the matched partials, then count only partials within fitTolCents (or fitTolBins of an FFT bin,
        // whichever is wider) of the fit. A real string's partials line up; random peaks don't.
        float fitTolCents = 6.0f, fitTolBins = 0.3f;
        float useFit = 1.0f;           // 0 = old window-only score (for comparisons)
        float useReassign = 0.0f;      // 1 = reassigned frequency as the peak position (else parabola)
        float energyMin = 0.5f;        // energyRatio() an expected key needs
        float energyLobe = 1.5f;       // bins around each fitted partial that count as "on the partial"
        float expectedFitMin = 0.20f;  // like expectedMin, for the (stricter, so lower) fitted score
        float fitMinPitch = 55.0f;
        float autoTune = 1.0f;         // 0 = keep the models at A440     // keys below this use the plain window score (bass partials are messy)
    } tune;

    // Fixed design constants (milliseconds converted in prepare()).
    // Stage 3 (jump keys, calibration) analyses only the LAST kLateMs: the hammer click is over by
    // then, so what is left is sustained tone.
    static constexpr double kStageDelayMs[kNumStages] = { 30.0, 65.0, 90.0, 240.0 };
    static constexpr double kStageWinMs[kNumStages]   = { 40.0, 80.0, 160.0, 160.0 };
    static constexpr double kLateMs = 160.0;
    static constexpr double kRefractoryMs = 50.0;
    static constexpr double kSettleMs     = 150.0;
    static constexpr double kPreGapMs     = 4.0;   // pre-onset window ends this long before the onset
    static constexpr double kBackoffMs    = 8.0;   // onset position = flux frame end - this
    static constexpr double kExpectedSlackMs = 15.0;
    static constexpr int    kStageMinPitch[3] = { 50, 40, 21 }; // lowest expected key judged at each stage

    // ---------------------------------------------------------------------------------------------
    double fs = 48000.0;
    int    maxBlock = 512;
    bool   prepared = false;
    float  sensitivity = 0.6f;

    int hop = 256, N1 = 1024;
    int ringSize = 0, ringMask = 0;
    int stageN[kNumStages] {};
    int stageLen[kNumStages] {};   // samples actually windowed (zero-padded to stageN)
    int64_t stageDelay[kNumStages] {};
    int64_t settle = 0, refractory = 0, preGap = 0, backoff = 0, expectedSlack = 0;

    std::unique_ptr<juce::dsp::FFT> fft1;
    std::unique_ptr<juce::dsp::FFT> fftS[kNumStages];
    std::vector<float> win1, winS[kNumStages], winD[kNumStages]; // winD: time derivative of winS (reassignment)
    std::vector<float> fftBuf, fftBufD;

    // audio history
    std::vector<float> ring;
    int64_t pos = 0;
    int64_t nextHop = 0;
    float levelMs = 0.0f;
    std::atomic<float> levelDb { -100.0f };
    float levelAlpha = 0.0f;

    // short-STFT features
    int kLo = 0, kHi = 0, nb = 0;
    std::vector<float> nf;          // per-bin noise floor
    std::vector<float> magSmooth;
    std::vector<float> Lhist;       // kHistFrames x nb log-compressed magnitudes
    std::vector<float> gHist, cHist;
    std::vector<float> statScratch, diffScratch, diffSorted;
    int64_t frameIdx = 0;
    float zG1 = 0, zG2 = 0, zC1 = 0, zC2 = 0;   // last two frames' z-scores (for peak picking)
    float oPrev1 = 0, oPrev2 = 0, oPrev3 = 0;
    int64_t lastOnsetPos = -(int64_t) 1 << 40;

    // models
    Calibration::Impl cal;
    bool haveCal = false;
    std::array<KeyPartials, kNumKeys> models {};

    // expected / watch
    PitchSet expected, watch;
    int64_t expectedStamp = -(int64_t) 1 << 40;
    bool    hitSinceSet = false;
    std::vector<uint16_t> maskBins;     // comb flux bins (index into the nb range)
    std::vector<uint8_t>  maskFlag;

    // pending onset evaluations
    struct Pending
    {
        bool     active = false;
        int64_t  pos = 0;
        int64_t  limit = kNever;
        float    strength = 0;
        int      stage = 0;
        int      lastStage = 2;
        bool     capture = false;
        PitchSet hitExp, hitWatch, reported;
    };
    Pending pend[kMaxPending];

    // output sink for the current process() call
    DetectedOnset* out = nullptr;
    int outMax = 0, outCount = 0;

    // evaluation scratch
    std::vector<float> magP, magR, evid, evidExp, lnCum, evidWork, ampBuf, medScratch, peakPos, kHat;
    std::vector<uint8_t> explainMask;

    // Auto-tuner: like a tuner app, but over every clear tonal peak of every note: a histogram of the
    // peaks' offsets from the equal-tempered A440 grid (cents, folded into -50..50). Low partials of
    // any note sit on that grid (h = 1, 2, 4, 8 exactly, 3 and 6 at +2 cents), so the histogram's mode
    // is the piano's tuning offset. Uncalibrated key models are shifted by it.
    static constexpr int kTuneBins = 100;
    float tuneHist[kTuneBins] {};
    float tuneMass = 0.0f;
    float modelTune = 0.0f;                 // offset the models are currently built with
    std::atomic<float> tuneEstimate { 0.0f };
    std::atomic<bool>  tuneConfident { false };

    // calibration capture
    bool    captureArmed = false;
    int     capturePitch = 0;
    bool    captureDone = false;
    Calibration::Impl::Key captured;
    int     capturedPitch = 0;

    // ---------------------------------------------------------------------------------------------
    Impl() { applySensitivity(); }

    void prepare (double sampleRate, int maxBlockSize)
    {
        fs = juce::jlimit (8000.0, 192000.0, sampleRate);
        maxBlock = std::max (1, maxBlockSize);
        N1 = nextPow2 (fs * 0.020);
        hop = N1 / 4;
        for (int s = 0; s < kNumStages; ++s)
        {
            stageN[s]     = nextPow2 (fs * kStageWinMs[s] / 1000.0);
            stageDelay[s] = (int64_t) std::llround (fs * kStageDelayMs[s] / 1000.0);
        }
        refractory    = (int64_t) std::llround (fs * kRefractoryMs / 1000.0);
        settle        = (int64_t) std::llround (fs * kSettleMs / 1000.0);
        preGap        = (int64_t) std::llround (fs * kPreGapMs / 1000.0);
        backoff       = (int64_t) std::llround (fs * kBackoffMs / 1000.0);
        expectedSlack = (int64_t) std::llround (fs * kExpectedSlackMs / 1000.0);

        ringSize = nextPow2 ((double) (stageN[kNumStages - 1] * 2 + stageDelay[kNumStages - 1] + 4096));
        ringMask = ringSize - 1;
        ring.assign ((size_t) ringSize, 0.0f);

        fft1 = std::make_unique<juce::dsp::FFT> (log2Int (N1));
        win1.resize ((size_t) N1);
        for (int i = 0; i < N1; ++i)
            win1[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * (float) i / (float) N1);
        int maxN = N1;
        for (int s = 0; s < kNumStages; ++s)
        {
            fftS[s] = std::make_unique<juce::dsp::FFT> (log2Int (stageN[s]));
            stageLen[s] = s == 3 ? std::min (stageN[s], (int) std::llround (fs * kLateMs / 1000.0)) : stageN[s];
            winS[s].resize ((size_t) stageLen[s]);
            winD[s].resize ((size_t) stageLen[s]);
            const float L = (float) stageLen[s], twoPi = 2.0f * juce::MathConstants<float>::pi;
            for (int i = 0; i < stageLen[s]; ++i)
            {
                winS[s][(size_t) i] = 0.5f - 0.5f * std::cos (twoPi * (float) i / L);
                winD[s][(size_t) i] = 0.5f * twoPi / L * std::sin (twoPi * (float) i / L);
            }
            maxN = std::max (maxN, stageN[s]);
        }
        fftBuf.assign ((size_t) maxN * 2, 0.0f);
        fftBufD.assign ((size_t) maxN * 2, 0.0f);
        kHat.assign ((size_t) maxN / 2 + 16, 0.0f);
        explainMask.assign ((size_t) maxN / 2 + 16, 0);
        magP.assign ((size_t) maxN / 2 + 8, 0.0f);
        magR.assign ((size_t) maxN / 2 + 8, 0.0f);
        evid.assign ((size_t) maxN / 2 + 8, 0.0f);
        evidExp.assign ((size_t) maxN / 2 + 8, 0.0f);
        evidWork.assign ((size_t) maxN / 2 + 8, 0.0f);
        lnCum.assign ((size_t) maxN / 2 + 16, 0.0f);
        ampBuf.assign ((size_t) maxN / 2 + 16, 0.0f);
        peakPos.assign ((size_t) maxN / 2 + 16, 0.0f);
        medScratch.assign ((size_t) std::max (4096, maxN), 0.0f);

        const double binHz1 = fs / (double) N1;
        kLo = std::max (2, (int) std::ceil (140.0 / binHz1));
        kHi = std::min (N1 / 2 - 2, (int) std::floor (std::min (8500.0, 0.45 * fs) / binHz1));
        nb  = kHi - kLo + 1;
        nf.assign ((size_t) nb, 0.0f);
        magSmooth.assign ((size_t) N1 / 2 + 8, 0.0f);
        Lhist.assign ((size_t) kHistFrames * (size_t) nb, 0.0f);
        gHist.assign ((size_t) kHistFrames, 0.0f);
        cHist.assign ((size_t) kHistFrames, 0.0f);
        statScratch.assign ((size_t) kStatFrames, 0.0f);
        diffScratch.assign ((size_t) nb, 0.0f);
        diffSorted.assign ((size_t) nb, 0.0f);
        maskFlag.assign ((size_t) nb, 0);
        maskBins.assign ((size_t) nb, 0);
        maskBins.clear();

        levelAlpha = 1.0f - std::exp (-1.0f / (float) (fs * 0.3));
        std::fill (std::begin (tuneHist), std::end (tuneHist), 0.0f);
        tuneMass = 0.0f;
        fitCount = 0;
        prepared = true;
        rebuildModels();
        reset();
    }

    void reset()
    {
        std::fill (ring.begin(), ring.end(), 0.0f);
        std::fill (nf.begin(), nf.end(), 0.0f);
        std::fill (Lhist.begin(), Lhist.end(), 0.0f);
        std::fill (gHist.begin(), gHist.end(), 0.0f);
        std::fill (cHist.begin(), cHist.end(), 0.0f);
        pos = 0;
        nextHop = hop;
        frameIdx = 0;
        zG1 = zG2 = zC1 = zC2 = 0;
        oPrev1 = oPrev2 = oPrev3 = 0;
        lastOnsetPos = -(int64_t) 1 << 40;
        expectedStamp = -(int64_t) 1 << 40;
        hitSinceSet = false;
        for (auto& p : pend)
            p = Pending();
        levelMs = 0;
        levelDb.store (-100.0f, std::memory_order_relaxed);
        captureArmed = false;
        captureDone = false;
    }

    void applySensitivity()
    {
        const float s = juce::jlimit (0.0f, 1.0f, sensitivity);
        tune.zOnset      = 8.5f - 6.0f * s;
        tune.zComb       = 7.0f - 5.5f * s;
        tune.promLo      = 7.0f - 3.3f * s;
        tune.promHi      = tune.promLo + 10.0f;
        tune.newLo       = 8.0f - 3.3f * s;
        tune.newHi       = tune.newLo + 12.0f;
        tune.expectedMin = 0.50f - 0.25f * s;
        tune.expectedFitMin = 0.32f - 0.20f * s;
        tune.energyMin   = 0.68f - 0.12f * s; // hardly loosened: it is what keeps snaps and claps out
        tune.watchMin    = 0.74f - 0.20f * s;
        tune.displayMin  = 0.52f - 0.20f * s;
        for (int i = 0; i < kNumOverrides; ++i)
            if (overrideSet[i])
                *overridePtr (i) = overrideValue[i];
    }

    // Name -> tuning field table (also used by the eval tool for sweeps).
    static constexpr int kNumOverrides = 38;
    static const char* overrideName (int i)
    {
        static const char* names[kNumOverrides] = { "zOnset", "zComb", "promLo", "promHi", "newLo", "newHi", "expectedMin",
                                                    "watchMin", "displayMin", "neighbourMargin", "watchContrast", "settleStrength", "expectedNewRelief", "weakOnsetPenalty", "contrastMin", "watchStrictDb", "chanceCoef", "watchSel", "subharmonicMin", "displaySel", "stage1Bump", "stage2Bump", "watchIndependent", "watchPrefilter", "measuredScale", "tonalTol", "tonalSoft", "explainMin", "explainFloor", "fitTolCents", "fitTolBins", "useFit", "useReassign", "energyMin", "energyLobe", "fitMinPitch", "expectedFitMin", "autoTune" };
        return names[i];
    }
    float* overridePtr (int i)
    {
        float* p[kNumOverrides] = { &tune.zOnset, &tune.zComb, &tune.promLo, &tune.promHi, &tune.newLo, &tune.newHi, &tune.expectedMin,
                                    &tune.watchMin, &tune.displayMin, &tune.neighbourMargin, &tune.watchContrast, &tune.settleStrength, &tune.expectedNewRelief, &tune.weakOnsetPenalty, &tune.contrastMin, &tune.watchStrictDb, &tune.chanceCoef, &tune.watchSel, &tune.subharmonicMin, &tune.displaySel, &tune.stage1Bump, &tune.stage2Bump, &tune.watchIndependent, &tune.watchPrefilter, &tune.measuredScale, &tune.tonalTol, &tune.tonalSoft, &tune.explainMin, &tune.explainFloor, &tune.fitTolCents, &tune.fitTolBins, &tune.useFit, &tune.useReassign, &tune.energyMin, &tune.energyLobe, &tune.fitMinPitch, &tune.expectedFitMin, &tune.autoTune };
        return p[i];
    }
    bool  overrideSet[kNumOverrides] {};
    float overrideValue[kNumOverrides] {};

    void rebuildModels()
    {
        if (! prepared)
            return;
        buildKeyModels (fs, haveCal ? &cal : nullptr, models, haveCal ? 0.0 : (double) modelTune);
        rebuildMask();
    }

    // ---------------------------------------------------------------------------------------------
    // Comb mask for the expected keys (short-STFT bins where their partials live)
    void rebuildMask()
    {
        if (! prepared)
            return;
        std::fill (maskFlag.begin(), maskFlag.end(), (uint8_t) 0);
        maskBins.clear();
        const double binHz1 = fs / (double) N1;
        for (int p = kLowestPianoKey; p <= kHighestPianoKey; ++p)
        {
            if (! expected.test ((size_t) p))
                continue;
            auto& kp = models[(size_t) keyIndex (p)];
            for (int i = 0; i < kp.n; ++i)
            {
                if (kp.h[i] > 16)
                    break;
                const int c = (int) std::lround ((double) kp.hz[i] / binHz1);
                for (int k = c - 1; k <= c + 1; ++k)
                {
                    const int j = k - kLo;
                    if (j >= 0 && j < nb && maskFlag[(size_t) j] == 0)
                    {
                        maskFlag[(size_t) j] = 1;
                        maskBins.push_back ((uint16_t) j);
                    }
                }
            }
        }
        // re-derive the comb flux history for the new mask
        const int64_t n = std::min<int64_t> (frameIdx, kHistFrames);
        for (int64_t f = frameIdx - n; f < frameIdx; ++f)
            cHist[(size_t) (f % kHistFrames)] = combFlux (f);
    }

    float combFlux (int64_t f) const
    {
        if (maskBins.empty() || f < kFluxLag)
            return 0.0f;
        const float* a = &Lhist[(size_t) (f % kHistFrames) * (size_t) nb];
        const float* b = &Lhist[(size_t) ((f - kFluxLag) % kHistFrames) * (size_t) nb];
        float sum = 0;
        for (auto j : maskBins)
            sum += std::max (0.0f, a[j] - b[j]);
        return sum / (float) maskBins.size();
    }

    // ---------------------------------------------------------------------------------------------
    inline float ringAt (int64_t i) const noexcept { return ring[(size_t) (i & ringMask)]; }

    // Windowed copy of the `len` samples ending at endPos, zero-padded to an FFT of size n.
    void windowedCopy (int64_t endPos, int len, int n, const float* win)
    {
        const int64_t start = endPos - len;
        for (int i = 0; i < len; ++i)
            fftBuf[(size_t) i] = ringAt (start + i) * win[i];
        std::fill (fftBuf.begin() + len, fftBuf.begin() + 2 * (size_t) n, 0.0f);
    }

    // median and MAD of the last kStatFrames values before `f` in `h`
    void stats (const std::vector<float>& h, int64_t f, float& med, float& mad)
    {
        const int n = (int) std::min<int64_t> (kStatFrames, f);
        for (int i = 0; i < n; ++i)
            statScratch[(size_t) i] = h[(size_t) ((f - 1 - i) % kHistFrames)];
        auto mid = statScratch.begin() + n / 2;
        std::nth_element (statScratch.begin(), mid, statScratch.begin() + n);
        med = *mid;
        for (int i = 0; i < n; ++i)
            statScratch[(size_t) i] = std::abs (statScratch[(size_t) i] - med);
        std::nth_element (statScratch.begin(), mid, statScratch.begin() + n);
        mad = *mid;
    }

    // ---------------------------------------------------------------------------------------------
    // One short-STFT frame: flux features and onset peak picking.
    void hopStep()
    {
        windowedCopy (pos, N1, N1, win1.data());
        fft1->performRealOnlyForwardTransform (fftBuf.data(), true);

        const int slot = (int) (frameIdx % kHistFrames);
        float* L = &Lhist[(size_t) slot * (size_t) nb];
        const int nBins = N1 / 2 + 1;
        for (int k = 0; k < nBins; ++k)
        {
            const float re = fftBuf[(size_t) 2 * (size_t) k], im = fftBuf[(size_t) 2 * (size_t) k + 1];
            magSmooth[(size_t) k] = std::sqrt (re * re + im * im);
        }
        for (int j = 0; j < nb; ++j)
        {
            const int k = kLo + j;
            const float m = magSmooth[(size_t) k];
            const float s = (magSmooth[(size_t) k - 2] + magSmooth[(size_t) k - 1] + m + magSmooth[(size_t) k + 1]
                             + magSmooth[(size_t) k + 2]) * 0.2f;
            float& floorV = nf[(size_t) j];
            if (frameIdx == 0 || floorV <= 0.0f)
                floorV = s;
            else if (s < floorV)
                floorV += 0.3f * (s - floorV);
            else
                floorV = std::min (s, floorV * 1.0015f);
            L[j] = std::log (1.0f + m / (2.0f * floorV + 1e-6f));
        }

        float g = 0.0f;
        if (frameIdx >= kFluxLag)
        {
            const float* prev = &Lhist[(size_t) ((frameIdx - kFluxLag) % kHistFrames) * (size_t) nb];
            // A gain change (AGC recovering after a loud note) lifts ALL bins; a note lifts a few.
            // Remove the lower-quartile rise so only the excess counts.
            for (int j = 0; j < nb; ++j)
                diffScratch[(size_t) j] = L[j] - prev[j];
            std::copy (diffScratch.begin(), diffScratch.begin() + nb, diffSorted.begin());
            auto q = diffSorted.begin() + nb / 4;
            std::nth_element (diffSorted.begin(), q, diffSorted.begin() + nb);
            const float shift = std::max (0.0f, *q);
            for (int j = 0; j < nb; ++j)
                g += std::max (0.0f, diffScratch[(size_t) j] - shift);
            g /= (float) nb;
        }
        const float c = combFlux (frameIdx);
        gHist[(size_t) slot] = g;
        cHist[(size_t) slot] = c;

        float zG = 0.0f, zC = 0.0f;
        if (frameIdx > 48)
        {
            float med, mad;
            stats (gHist, frameIdx, med, mad);
            const float sigma = std::max (1.4826f * mad, 0.25f * med + 0.004f);
            zG = (g - med) / sigma;
            if (! maskBins.empty())
            {
                stats (cHist, frameIdx, med, mad);
                const float sigmaC = std::max (1.4826f * mad, 0.25f * med + 0.006f);
                zC = (c - med) / sigmaC;
            }
        }
       #ifdef FALL_DETECT_TRACE
        {
            static const double t0 = std::getenv ("FALL_T0") ? std::atof (std::getenv ("FALL_T0")) : -1.0;
            static const double t1 = std::getenv ("FALL_T1") ? std::atof (std::getenv ("FALL_T1")) : -1.0;
            const double now = (double) pos / fs;
            if (now >= t0 && now <= t1)
                std::fprintf (stderr, "[flux] t=%.3f g=%.4f zG=%.2f c=%.4f zC=%.2f mask=%d\n", now, (double) g, (double) zG, (double) c, (double) zC, (int) maskBins.size());
        }
       #endif
        // combined onset function: comb flux is allowed to trigger at a lower z
        const float oNow = std::max (zG / tune.zOnset, zC / tune.zComb);

        // peak picking with one frame of look-ahead: o1 is the candidate
        if (oPrev1 >= 1.0f && oPrev1 >= oPrev2 && oPrev1 > oNow)
        {
            const int64_t frameEnd = pos - hop; // end of the candidate frame
            // walk back to where the rise started
            int back = 0;
            if (oPrev2 >= 0.4f) { back = 1; if (oPrev3 >= 0.4f) back = 2; }
            const int64_t onsetPos = frameEnd - (int64_t) back * hop - backoff;
            // right after an onset the flux has secondary bumps (reverb build-up, compressor release):
            // a second onset inside the "settling" time must be clearly stronger than the threshold
            const int64_t gap = onsetPos - lastOnsetPos;
            if (gap >= refractory && (gap >= settle || oPrev1 >= tune.settleStrength))
                registerOnset (onsetPos, oPrev1);
        }
        oPrev3 = oPrev2;
        oPrev2 = oPrev1;
        oPrev1 = oNow;
        ++frameIdx;
    }

    void registerOnset (int64_t onsetPos, float strengthRel)
    {
        // limit the previous pending onset's windows so they don't swallow this one
        int freeSlot = -1;
        for (int i = 0; i < kMaxPending; ++i)
        {
            auto& p = pend[i];
            if (! p.active)
            {
                if (freeSlot < 0) freeSlot = i;
                continue;
            }
            p.limit = std::min (p.limit, onsetPos - preGap);
        }
        if (freeSlot < 0) // overloaded: drop the oldest
        {
            int64_t oldest = kNever;
            for (int i = 0; i < kMaxPending; ++i)
                if (pend[i].pos < oldest) { oldest = pend[i].pos; freeSlot = i; }
        }
        auto& p = pend[freeSlot];
        p = Pending();
        p.active = true;
        p.pos = onsetPos;
        p.strength = strengthRel;
        p.lastStage = watch.any() ? 3 : 2;
        if (captureArmed)
        {
            p.capture = true;
            p.lastStage = 3;
            captureArmed = false; // this onset is the capture onset
        }
        lastOnsetPos = onsetPos;
       #ifdef FALL_DETECT_TRACE
        std::fprintf (stderr, "[trace] onset registered at %.3f strength %.2f\n", (double) onsetPos / fs, (double) strengthRel);
       #endif
    }

    int64_t stageDue (const Pending& p, int stage) const noexcept
    {
        return std::min (p.pos + stageDelay[stage], p.limit);
    }

    int64_t earliestDue() const noexcept
    {
        int64_t d = kNever;
        for (auto& p : pend)
            if (p.active)
                d = std::min (d, stageDue (p, p.stage));
        return d;
    }

    // ---------------------------------------------------------------------------------------------
    // Evaluation
    void computeSpectrum (int64_t endPos, int stage, std::vector<float>& mag)
    {
        const int n = stageN[stage];
        windowedCopy (endPos, stageLen[stage], n, winS[stage].data());
        fftS[stage]->performRealOnlyForwardTransform (fftBuf.data(), true);
        for (int k = 0; k <= n / 2; ++k)
        {
            const float re = fftBuf[(size_t) 2 * (size_t) k], im = fftBuf[(size_t) 2 * (size_t) k + 1];
            mag[(size_t) k] = std::sqrt (re * re + im * im);
        }
    }

    // Spectrum of the window ending at endPos plus, per bin, the reassigned frequency (in bins):
    // k^ = k - Im (X_h' / X_h) * n / 2pi, with h' the window's time derivative. For a sinusoid every
    // bin of its main lobe reports the sinusoid's exact frequency.
    void computeReassigned (int64_t endPos, int stage)
    {
        const int n = stageN[stage], len = stageLen[stage];
        const int64_t start = endPos - len;
        const float* w = winS[stage].data();
        const float* wd = winD[stage].data();
        for (int i = 0; i < len; ++i)
        {
            const float x = ringAt (start + i);
            fftBuf[(size_t) i] = x * w[i];
            fftBufD[(size_t) i] = x * wd[i];
        }
        std::fill (fftBuf.begin() + len, fftBuf.begin() + 2 * (size_t) n, 0.0f);
        std::fill (fftBufD.begin() + len, fftBufD.begin() + 2 * (size_t) n, 0.0f);
        fftS[stage]->performRealOnlyForwardTransform (fftBuf.data(), true);
        fftS[stage]->performRealOnlyForwardTransform (fftBufD.data(), true);
        const float scale = (float) n / (2.0f * juce::MathConstants<float>::pi);
        for (int k = 0; k <= n / 2; ++k)
        {
            const float re = fftBuf[(size_t) 2 * (size_t) k], im = fftBuf[(size_t) 2 * (size_t) k + 1];
            const float rd = fftBufD[(size_t) 2 * (size_t) k], id = fftBufD[(size_t) 2 * (size_t) k + 1];
            const float p = re * re + im * im;
            magP[(size_t) k] = std::sqrt (p);
            kHat[(size_t) k] = p > 1e-24f ? (float) k - scale * (id * re - rd * im) / p : (float) k;
        }
    }

    struct EvBand { int kLo, kHi; double binHz; };

    // Fills `evid` with per-bin evidence of NEW energy (0..1) and returns the bin range.
    EvBand computeEvidence (int stage, int64_t postEnd, int64_t preEnd, float strictBoostDb = 0.0f)
    {
        const int n = stageN[stage];
        computeReassigned (postEnd, stage);
        computeSpectrum (preEnd, stage, magR);
        EvBand b;
        b.binHz = fs / (double) n;
        b.kLo = std::max (3, (int) std::ceil (110.0 / b.binHz));
        b.kHi = std::min (n / 2 - 3, (int) std::floor (std::min (9800.0, 0.45 * fs) / b.binHz));

        const int nh = n / 2;

        // Baseline = lower quartile of the post spectrum around each bin, evaluated at log-spaced anchors
        // and interpolated (a median ignores the note's own sharp partials, a mean would not).
        int prevK = -1;
        float prevLn = 0.0f;
        for (int k = b.kLo; ; )
        {
            const int w = std::max (8, (int) (k * 0.06));
            const int a = std::max (0, k - w), c = std::min (nh, k + w);
            const int cnt = c - a + 1;
            std::copy (magP.begin() + a, magP.begin() + c + 1, medScratch.begin());
            auto mid = medScratch.begin() + cnt / 4; // lower quartile: dense bass combs fill >50% of the bins
            std::nth_element (medScratch.begin(), mid, medScratch.begin() + cnt);
            const float ln = std::log (*mid + 1e-9f);
            if (prevK >= 0)
                for (int j = prevK; j <= k; ++j)
                    lnCum[(size_t) j] = prevLn + (ln - prevLn) * (float) (j - prevK) / (float) std::max (1, k - prevK);
            prevK = k;
            prevLn = ln;
            if (k >= b.kHi)
                break;
            k = std::min (b.kHi, k + std::max (3, (int) (k * 0.03)));
        }

        // A bin counts as evidence when it is a spectral peak that stands out from its neighbourhood
        // ("prominence") AND is much stronger than just before the onset ("newness"). The skirts of a
        // strong partial are not peaks, and a sustained partial of an old note is not new.
        // Common level rise between the two windows (AGC / compressor release) is not a new note:
        // estimate it as a low percentile of the per-bin log ratio and take it off the newness.
        const int nBand = b.kHi - b.kLo + 1;
        for (int k = b.kLo; k <= b.kHi; ++k)
            medScratch[(size_t) (k - b.kLo)] = std::log ((magP[(size_t) k] + 1e-9f) / (magR[(size_t) k] + 1e-9f));
        auto qIt = medScratch.begin() + nBand / 10;
        std::nth_element (medScratch.begin(), qIt, medScratch.begin() + nBand);
        const float gainRiseDb = std::max (0.0f, 8.6859f * *qIt);

        const float promLo = tune.promLo + strictBoostDb, promInv = 1.0f / (tune.promHi - tune.promLo);
        const float newLo  = tune.newLo + strictBoostDb,  newInv  = 1.0f / (tune.newHi - tune.newLo);
        const float newLoExp = tune.newLo - tune.expectedNewRelief;
        for (int k = b.kLo; k <= b.kHi; ++k)
        {
            const float pk = magP[(size_t) k];
            if (pk < magP[(size_t) k - 1] || pk < magP[(size_t) k + 1])
            {
                evid[(size_t) k] = 0.0f;
                evidExp[(size_t) k] = 0.0f;
                continue;
            }
            const float lnP = std::log (pk + 1e-9f);
            // Tonality: the peak's own reassigned frequency must sit near the peak, and its two
            // neighbours (both inside a sinusoid's main lobe) must point to the same frequency.
            const float kh = kHat[(size_t) k];
            const float dev = std::max ({ std::abs (kHat[(size_t) k - 1] - kh), std::abs (kHat[(size_t) k + 1] - kh),
                                          std::max (0.0f, std::abs (kh - (float) k) - 0.5f) });
            const float tonal = clamp01 (1.0f - (dev - tune.tonalTol) / tune.tonalSoft);
            if (tonal <= 0.0f)
            {
                evid[(size_t) k] = 0.0f;
                evidExp[(size_t) k] = 0.0f;
                continue;
            }
            // sub-bin peak position (the reassigned frequency): lets the key windows be narrower than
            // the FFT resolution, which is what separates adjacent semitones
            if (tune.useReassign > 0.5f)
                peakPos[(size_t) k] = juce::jlimit ((float) k - 0.5f, (float) k + 0.5f, kh);
            else
            {
                const float l = std::log (magP[(size_t) k - 1] + 1e-9f), r = std::log (magP[(size_t) k + 1] + 1e-9f);
                const float den = l - 2.0f * lnP + r;
                peakPos[(size_t) k] = (float) k + (den < -1e-6f ? juce::jlimit (-0.5f, 0.5f, 0.5f * (l - r) / den) : 0.0f);
            }
            const float prom = 8.6859f * (lnP - lnCum[(size_t) k]);
            const float fresh = 8.6859f * (lnP - std::log (1.14f * magR[(size_t) k] + 1e-9f)) - gainRiseDb;
            const float pe = clamp01 ((prom - promLo) * promInv);
            evid[(size_t) k] = tonal * std::min (pe, clamp01 ((fresh - newLo) * newInv));
            // expected-key checks may be more forgiving about newness (a repeated note adds only a
            // few dB on top of its own ringing partial)
            evidExp[(size_t) k] = tonal * std::min (pe, clamp01 ((fresh - newLoExp) * newInv));
        }
        return b;
    }

    float score (const KeyPartials& kp, const float* e, const EvBand& b) const noexcept
    {
        float f = 0.0f;
        for (int i = 0; i < kp.n; ++i)
        {
            const float lo = (float) ((double) kp.lo[i] / b.binHz) - 0.12f, hi = (float) ((double) kp.hi[i] / b.binHz) + 0.12f;
            const int k0 = std::max ((int) std::floor (lo) - 1, b.kLo);
            const int k1 = std::min ((int) std::ceil (hi) + 1, b.kHi);
            float m = 0.0f;
            for (int k = k0; k <= k1; ++k)
                if (e[k] > m && peakPos[(size_t) k] >= lo && peakPos[(size_t) k] <= hi)
                    m = e[k];
            f += kp.w[i] * m;
        }
        return f;
    }

    // Like score(), but only partials that agree with ONE string, the way a tuner locks on: a small grid
    // search over the tuning offset c (cents) and the inharmonicity scale (B x (1 + beta)) predicts every
    // partial; each partial earns credit only for a peak within the tolerance of its prediction (full
    // credit within tol, none beyond 2 x tol). Random peaks that merely fall into the wide model windows
    // (a voice, a clap, another note's partials) don't line up with any single (c, beta).
    static constexpr int kMaxCand = 4;
    static constexpr int kNumBeta = 12;
    static constexpr float kBetas[kNumBeta] = { -1.0f, -0.8f, -0.6f, -0.4f, -0.2f, 0.0f, 0.3f, 0.6f, 1.0f, 1.6f, 2.5f, 4.0f };

    struct Fit { float c = 0.0f; float beta = 0.0f; };
    bool useFitFor (int pitch) const noexcept { return tune.useFit > 0.5f && (float) pitch >= tune.fitMinPitch; }

    float scoreFit (int pitch, const float* e, const EvBand& b, Fit* fitOut = nullptr) const noexcept
    {
        const auto& kp = models[(size_t) keyIndex (pitch)];
        float cd[kMaxPartials][kMaxCand], ce[kMaxPartials][kMaxCand], tol[kMaxPartials], w[kMaxPartials];
        float shift[kMaxPartials][kNumBeta];
        int nc[kMaxPartials];
        const double B = inharmonicityB (pitch);
        int n = 0;
        for (int i = 0; i < kp.n; ++i)
        {
            const float lo = (float) ((double) kp.lo[i] / b.binHz) - 0.12f, hi = (float) ((double) kp.hi[i] / b.binHz) + 0.12f;
            const int k0 = std::max ((int) std::floor (lo) - 1, b.kLo);
            const int k1 = std::min ((int) std::ceil (hi) + 1, b.kHi);
            int m = 0;
            for (int k = k0; k <= k1; ++k)
            {
                if (e[k] <= 0.0f || peakPos[(size_t) k] < lo || peakPos[(size_t) k] > hi)
                    continue;
                // keep the kMaxCand strongest-evidence peaks (ties: louder first)
                int slot = m < kMaxCand ? m++ : -1;
                if (slot < 0)
                {
                    int weakest = 0;
                    for (int j = 1; j < kMaxCand; ++j)
                        if (ce[n][j] < ce[n][weakest]) weakest = j;
                    if (ce[n][weakest] >= e[k])
                        continue;
                    slot = weakest;
                }
                ce[n][slot] = e[k];
                cd[n][slot] = (float) (1200.0 * std::log2 ((double) peakPos[(size_t) k] * b.binHz / (double) kp.hz[i]));
            }
            if (m == 0)
                continue;
            nc[n] = m;
            w[n] = kp.w[i];
            tol[n] = std::max (tune.fitTolCents, (float) (1200.0 * std::log2 (1.0 + tune.fitTolBins * b.binHz / (double) kp.hz[i])));
            const double h2 = (double) kp.h[i] * kp.h[i];
            for (int bi = 0; bi < kNumBeta; ++bi)
                shift[n][bi] = kp.measured ? 0.0f
                                           : (float) (600.0 * std::log2 ((1.0 + B * (1.0 + kBetas[bi]) * h2) / (1.0 + B * h2)));
            ++n;
        }
        if (n == 0)
            return 0.0f;
        if (fitOut != nullptr) *fitOut = Fit();

        float best = 0.0f;
        const int nb = kp.measured ? 1 : kNumBeta;
        for (int bj = 0; bj < nb; ++bj)
        {
            const int bi = kp.measured ? 5 : bj; // beta = 0 for measured partials
            for (float c = -30.0f; c <= 30.0f; c += 1.5f)
            {
                float f = 0.0f;
                for (int i = 0; i < n; ++i)
                {
                    const float pred = c + shift[i][bi];
                    float credit = 0.0f;
                    for (int j = 0; j < nc[i]; ++j)
                        credit = std::max (credit, ce[i][j] * clamp01 (2.0f - std::abs (cd[i][j] - pred) / tol[i]));
                    f += w[i] * credit;
                }
                if (f > best)
                {
                    best = f;
                    if (fitOut != nullptr) { fitOut->c = c; fitOut->beta = kBetas[bi]; }
                }
            }
        }
        return best;
    }

    // Where did the new energy go? Fraction of the new power (post - pre) in the key's frequency range
    // that lies on the FITTED partials of `pitch` (+- the main lobe and the fit tolerance) or on the model
    // partials of the other expected keys (a chord). A piano note puts most of its new energy into its
    // partials; a snap or clap spreads it everywhere, a voice or a wrong key puts it elsewhere.
    float energyRatio (int pitch, const Fit& fit, const EvBand& b) noexcept
    {
        const auto& kp = models[(size_t) keyIndex (pitch)];
        if (kp.n == 0)
            return 1.0f;
        float hiHz = kp.hz[0];
        for (int i = 0; i < kp.n && kp.hz[i] < 5000.0f; ++i)
            hiHz = kp.hz[i];
        const int k0 = std::max (b.kLo, (int) std::floor (kp.lo[0] * 0.94 / b.binHz));
        const int k1 = std::min (b.kHi, (int) std::ceil (hiHz * 1.06 / b.binHz));
        if (k1 <= k0)
            return 1.0f;
        std::fill (explainMask.begin() + k0, explainMask.begin() + k1 + 1, (uint8_t) 0);
        const double B = inharmonicityB (pitch);
        for (int i = 0; i < kp.n; ++i)
        {
            const double h2 = (double) kp.h[i] * kp.h[i];
            const double cents = fit.c + (kp.measured ? 0.0 : 600.0 * std::log2 ((1.0 + B * (1.0 + fit.beta) * h2) / (1.0 + B * h2)));
            const double hz = kp.hz[i] * std::pow (2.0, cents / 1200.0);
            const double tolHz = hz * (std::pow (2.0, tune.fitTolCents / 1200.0) - 1.0);
            const double half = tolHz / b.binHz + tune.energyLobe;
            const int a = std::max (k0, (int) std::floor (hz / b.binHz - half)), c = std::min (k1, (int) std::ceil (hz / b.binHz + half));
            for (int k = a; k <= c; ++k)
                explainMask[(size_t) k] = 1;
        }
        for (int p = kLowestPianoKey; p <= kHighestPianoKey; ++p)
        {
            if (p == pitch || ! expected.test ((size_t) p))
                continue;
            const auto& q = models[(size_t) keyIndex (p)];
            for (int i = 0; i < q.n; ++i)
            {
                const int a = std::max (k0, (int) std::floor ((double) q.lo[i] / b.binHz - tune.energyLobe));
                const int c = std::min (k1, (int) std::ceil ((double) q.hi[i] / b.binHz + tune.energyLobe));
                for (int k = a; k <= c; ++k)
                    explainMask[(size_t) k] = 1;
            }
        }
        double all = 0.0, inside = 0.0;
        for (int k = k0; k <= k1; ++k)
        {
            const double post = (double) magP[(size_t) k] * magP[(size_t) k];
            const double pre = 1.3 * (double) magR[(size_t) k] * magR[(size_t) k];
            const double d = post > pre ? post - pre : 0.0;
            all += d;
            if (explainMask[(size_t) k] != 0)
                inside += d;
        }
        return all <= 0.0 ? 1.0f : (float) (inside / all);
    }

    // Score over only those partials that no octave (h even) or twelfth (h multiple of 3) of a
    // lower note can produce: h = 1, 5, 7, 11, 13 ... Needs at least three of them, else -1.
    float scoreIndependent (const KeyPartials& kp, const float* e, const EvBand& b) const noexcept
    {
        float f = 0.0f, wsum = 0.0f;
        int cnt = 0;
        for (int i = 0; i < kp.n; ++i)
        {
            if (kp.h[i] % 2 == 0 || kp.h[i] % 3 == 0)
                continue;
            const float lo = (float) ((double) kp.lo[i] / b.binHz) - 0.12f, hi = (float) ((double) kp.hi[i] / b.binHz) + 0.12f;
            const int k0 = std::max ((int) std::floor (lo) - 1, b.kLo);
            const int k1 = std::min ((int) std::ceil (hi) + 1, b.kHi);
            float m = 0.0f;
            for (int k = k0; k <= k1; ++k)
                if (e[k] > m && peakPos[(size_t) k] >= lo && peakPos[(size_t) k] <= hi)
                    m = e[k];
            f += kp.w[i] * m;
            wsum += kp.w[i];
            ++cnt;
        }
        return cnt >= 3 && wsum > 0.0f ? f / wsum : -1.0f;
    }

    void damp (const KeyPartials& kp, float* e, const EvBand& b, float factor) const noexcept
    {
        for (int i = 0; i < kp.n; ++i)
        {
            const float lo = (float) ((double) kp.lo[i] / b.binHz) - 0.12f, hi = (float) ((double) kp.hi[i] / b.binHz) + 0.12f;
            const int k0 = std::max ((int) std::floor (lo) - 1, b.kLo);
            const int k1 = std::min ((int) std::ceil (hi) + 1, b.kHi);
            for (int k = k0; k <= k1; ++k)
                if (peakPos[(size_t) k] >= lo && peakPos[(size_t) k] <= hi)
                    e[k] *= factor;
        }
    }

    // Median score of the neighbouring keys (+-1..3 semitones, not expected): what "nothing special
    // at this pitch" looks like in the current spectrum (noise peaks, other notes' coincident partials).
    float nullLevel (int pitch, const float* e, const EvBand& b) const noexcept
    {
        float v[6];
        int n = 0;
        for (int d : { -3, -2, -1, 1, 2, 3 })
        {
            const int q = pitch + d;
            if (isPianoKey (q) && ! expected.test ((size_t) q))
            {
                float x = score (models[(size_t) keyIndex (q)], e, b);
                int i = n++;
                for (; i > 0 && v[i - 1] > x; --i)
                    v[i] = v[i - 1];
                v[i] = x;
            }
        }
        return n == 0 ? 0.0f : v[n / 2];
    }

    // Marks the bins of every expected key's partial windows (for explainedRatio()).
    void buildExplainMask (const EvBand& b)
    {
        std::fill (explainMask.begin() + b.kLo, explainMask.begin() + b.kHi + 1, (uint8_t) 0);
        for (int p = kLowestPianoKey; p <= kHighestPianoKey; ++p)
        {
            if (! expected.test ((size_t) p))
                continue;
            const auto& kp = models[(size_t) keyIndex (p)];
            for (int i = 0; i < kp.n; ++i)
            {
                const int k0 = std::max (b.kLo, (int) std::floor ((double) kp.lo[i] / b.binHz - 0.12));
                const int k1 = std::min (b.kHi, (int) std::ceil ((double) kp.hi[i] / b.binHz + 0.12));
                for (int k = k0; k <= k1; ++k)
                    explainMask[(size_t) k] = 1;
            }
        }
    }

    // Of the clearly new tonal peaks in the frequency range of `pitch`'s partials, the
    // (evidence-weighted) fraction that the expected chord explains. 1 when there is too little
    // new evidence to judge.
    float explainedRatio (int pitch, const EvBand& b) const noexcept
    {
        const auto& kp = models[(size_t) keyIndex (pitch)];
        if (kp.n == 0)
            return 1.0f;
        float hiHz = kp.hz[0];
        for (int i = 0; i < kp.n && kp.hz[i] < 5000.0f; ++i)
            hiHz = kp.hz[i];
        const int k0 = std::max (b.kLo, (int) std::floor (kp.lo[0] * 0.94 / b.binHz));
        const int k1 = std::min (b.kHi, (int) std::ceil (hiHz * 1.06 / b.binHz));
        float all = 0.0f, inside = 0.0f;
        for (int k = k0; k <= k1; ++k)
        {
            const float e = evid[(size_t) k];
            all += e;
            if (explainMask[(size_t) k] != 0)
                inside += e;
        }
        return all < tune.explainFloor ? 1.0f : inside / all;
    }

    // True when a semitone/tone neighbour that is NOT expected explains the evidence clearly better,
    // i.e. the user most likely hit the wrong (adjacent) key.
    bool betterNeighbour (int pitch, float f, const EvBand& b) const noexcept
    {
        for (int d : { -2, -1, 1, 2 })
        {
            const int q = pitch + d;
            if (! isPianoKey (q) || expected.test ((size_t) q))
                continue;
            const float fq = useFitFor (pitch) ? scoreFit (q, evidExp.data(), b) : score (models[(size_t) keyIndex (q)], evidExp.data(), b);
            if (fq > f + tune.neighbourMargin)
                return true;
        }
        return false;
    }

    struct Picks { int n = 0; int pitch[kMaxPicks] {}; float f[kMaxPicks] {}; };

    // Greedy explain-and-subtract: strongest key first, its partials are damped, repeat.
    Picks greedy (const EvBand& b, float minScore, float minSel)
    {
        std::copy (evid.begin(), evid.end(), evidWork.begin());
        Picks pk;
        float sc[kNumKeys];
        for (int it = 0; it < kMaxPicks; ++it)
        {
            int best = -1;
            float bestF = 0.0f, bestSel = 0.0f;
            for (int i = 0; i < kNumKeys; ++i)
            {
                sc[i] = models[(size_t) i].n > 0 ? score (models[(size_t) i], evidWork.data(), b) : 0.0f;
                // keys with only one or two usable partials reach a high score by chance far more
                // easily than a 20-partial series: compare them after subtracting a chance level
                const float sel = sc[i] - tune.chanceCoef * models[(size_t) i].chance;
                if (sel > bestSel) { bestSel = sel; bestF = sc[i]; best = i; }
            }
            if (best < 0 || bestF < minScore || bestSel < minSel)
                break;
            // octave / twelfth ambiguity: a lower key whose series also fits explains the note
            for (int d : { 12, 19, 24 })
            {
                const int q = best - d;
                if (q >= 0 && sc[q] >= 0.9f * bestF && sc[q] >= minScore)
                {
                    best = q;
                    bestF = sc[q];
                }
            }
            pk.pitch[pk.n] = kLowestPianoKey + best;
            pk.f[pk.n] = bestF;
            ++pk.n;
            damp (models[(size_t) best], evidWork.data(), b, 0.3f);
        }
        return pk;
    }

    // A lower key (octave, twelfth, double octave ...) that is itself clearly present explains
    // this key's partials; a jump key must not fire on the overtones of ordinary notes.
    bool explainedFromBelow (int pitch, const EvBand& b) const noexcept
    {
        for (int d : { 12, 19, 24, 28, 31, 34, 36, 38 })
        {
            const int q = pitch - d;
            if (isPianoKey (q) && ! watch.test ((size_t) q)
                && score (models[(size_t) keyIndex (q)], evid.data(), b) >= tune.subharmonicMin)
                return true;
        }
        return false;
    }

    float contrast (int pitch, float f, const EvBand& b) const noexcept
    {
        float worst = 0.0f;
        for (int d : { -2, -1, 1, 2 })
            if (isPianoKey (pitch + d))
                worst = std::max (worst, score (models[(size_t) keyIndex (pitch + d)], evid.data(), b));
        return f - worst;
    }

    void accumulateTuning (const EvBand& b)
    {
        for (auto& v : tuneHist)
            v *= 0.985f;
        tuneMass *= 0.985f;
        const int k0 = std::max (b.kLo + 1, (int) (100.0 / b.binHz)), k1 = std::min (b.kHi - 1, (int) (2000.0 / b.binHz));
        for (int k = k0; k <= k1; ++k)
        {
            const float e = evid[(size_t) k];
            if (e < 0.7f)
                continue;
            const double cents = 1200.0 * std::log2 ((double) peakPos[(size_t) k] * b.binHz / 440.0);
            double frac = cents - 100.0 * std::floor (cents / 100.0 + 0.5); // -50 .. 50
            const int bin = juce::jlimit (0, kTuneBins - 1, (int) std::floor (frac + 50.0));
            tuneHist[bin] += e;
            tuneMass += e;
        }
        if (tuneMass < 25.0f)
            return;
        // circular smoothing (+-4 cents, triangular) and mode
        float best = 0.0f, mean = tuneMass / (float) kTuneBins;
        int bestBin = 0;
        for (int i = 0; i < kTuneBins; ++i)
        {
            float v = 0.0f;
            for (int d = -4; d <= 4; ++d)
                v += (5.0f - (float) std::abs (d)) * tuneHist[(i + d + kTuneBins) % kTuneBins];
            v /= 25.0f;
            if (v > best) { best = v; bestBin = i; }
        }
        // Coarse lock only: higher partials are stretched (inharmonicity), which biases this estimate a
        // few cents sharp. The fine estimate comes from the fitted offsets of accepted notes (noteFitted).
        const bool confident = best > 3.0f * mean;
        const float est = (float) bestBin - 50.0f + 0.5f - 5.0f;
        if (fitsSeen < kFitRing)
        {
            tuneEstimate.store (est, std::memory_order_relaxed);
            tuneConfident.store (confident, std::memory_order_relaxed);
        }
        if (confident && ! haveCal && tune.autoTune > 0.5f && std::abs (est - modelTune) >= 12.0f)
        {
            modelTune = est;
            fitCount = 0;
            rebuildModels();
        }
    }

    // An accepted expected key's fitted offset from its model (cents): the fine tuning estimate.
    static constexpr int kFitRing = 9;
    float fitRing[kFitRing] {};
    int   fitCount = 0, fitsSeen = 0;

    void noteFitted (float c)
    {
        fitRing[fitCount++] = c;
        if (fitCount < kFitRing)
            return;
        float tmp[kFitRing];
        std::copy (fitRing, fitRing + kFitRing, tmp);
        std::nth_element (tmp, tmp + kFitRing / 2, tmp + kFitRing);
        const float med = tmp[kFitRing / 2];
        fitCount = 0;
        fitsSeen += kFitRing;
        tuneEstimate.store (modelTune + med, std::memory_order_relaxed);
        tuneConfident.store (true, std::memory_order_relaxed);
        if (! haveCal && tune.autoTune > 0.5f && std::abs (med) >= 2.5f)
        {
            modelTune = juce::jlimit (-50.0f, 50.0f, modelTune + med);
            rebuildModels();
        }
    }

    void runStage (Pending& p, int stage)
    {
        const bool clamped = stageDue (p, stage) >= p.limit;
        const int64_t postEnd = stageDue (p, stage);
        const int64_t preEnd  = p.pos - preGap;
        if (postEnd - p.pos < (int64_t) (fs * 0.012) || (stage == 3 && postEnd - p.pos < stageDelay[3]))
        {
            // swallowed by the following onset / late window cut short: nothing meaningful to judge
            if (p.capture)
                captureArmed = true;
            return;
        }

        if (p.capture && stage < 3)
            return; // a capture onset only needs the long late window

        // jump keys are judged on much clearer evidence than ordinary notes
        const EvBand b = computeEvidence (stage, postEnd, preEnd, (stage == 3 && ! p.capture) ? tune.watchStrictDb : 0.0f);

        if (p.capture)
        {
            finishCapture (b);
            return;
        }

        PitchSet newExp, newWatch, shown;
        if (stage == 2 || (clamped && stage == 1))
            accumulateTuning (b);

        if (stage < 3)
        {
            // expected keys: permissive threshold, only for onsets that began after the set changed
            const bool eligible = p.pos >= expectedStamp - expectedSlack;
            // a barely-over-threshold onset (secondary bump after a real note, room noise) must
            // bring more evidence than a clear strike
            const float neededExtra = (stage == 1 ? tune.stage1Bump : stage == 2 ? tune.stage2Bump : 0.0f)
                                 + tune.weakOnsetPenalty * clamp01 (2.0f - p.strength);
            if (eligible && expected.any())
            {
                if (tune.explainMin > 0.0f)
                    buildExplainMask (b);
                for (int pitch = std::max (kLowestPianoKey, kStageMinPitch[stage]); pitch <= kHighestPianoKey; ++pitch)
                {
                    if (! expected.test ((size_t) pitch) || p.hitExp.test ((size_t) pitch))
                        continue;
                    Fit fit;
                    const float f = useFitFor (pitch) ? scoreFit (pitch, evidExp.data(), b, &fit)
                                                       : score (models[(size_t) keyIndex (pitch)], evidExp.data(), b);
                   #ifdef FALL_DETECT_TRACE
                    std::fprintf (stderr, "[trace] onset %.3f stage %d expected %d F=%.2f null=%.2f\n", (double) p.pos / fs, stage, pitch, (double) f, (double) nullLevel (pitch, evidExp.data(), b));
                   #endif
                    const float needed = (useFitFor (pitch) ? tune.expectedFitMin : tune.expectedMin) + neededExtra;
                    if (f >= needed * (models[(size_t) keyIndex (pitch)].measured ? tune.measuredScale : 1.0f) && f - nullLevel (pitch, evidExp.data(), b) >= tune.contrastMin && ! betterNeighbour (pitch, f, b)
                        && (tune.explainMin <= 0.0f || explainedRatio (pitch, b) >= tune.explainMin)
                        && (tune.energyMin <= 0.0f || energyRatio (pitch, fit, b) >= tune.energyMin))
                    {
                        newExp.set ((size_t) pitch);
                        p.hitExp.set ((size_t) pitch);
                        if (useFitFor (pitch) && ! models[(size_t) keyIndex (pitch)].measured)
                            noteFitted (fit.c);
                    }
                }
            }

            // the expensive late jump-key stage only runs when a jump key looks plausible so far
            if (stage == 2 && p.lastStage == 3 && ! p.capture)
            {
                bool plausible = false;
                for (int w = kLowestPianoKey; w <= kHighestPianoKey && ! plausible; ++w)
                    plausible = watch.test ((size_t) w) && score (models[(size_t) keyIndex (w)], evid.data(), b) >= tune.watchPrefilter;
                if (! plausible)
                    p.lastStage = 2;
            }

            if (stage == 2 || (clamped && stage >= 1))
            {
                const Picks pk = greedy (b, tune.displayMin, tune.displaySel);
                for (int i = 0; i < pk.n; ++i)
                    shown.set ((size_t) pk.pitch[i]);
            }
        }
        else if (watch.any())
        {
            const Picks pk = greedy (b, tune.displayMin, 0.0f);
           #ifdef FALL_DETECT_TRACE
            for (int w = kLowestPianoKey; w <= kHighestPianoKey; ++w)
                if (watch.test ((size_t) w))
                {
                    const float f = score (models[(size_t) keyIndex (w)], evid.data(), b);
                    bool picked = false;
                    for (int i = 0; i < pk.n; ++i) picked |= pk.pitch[i] == w;
                    std::fprintf (stderr, "[trace] onset %.3f stage 3 watch %d F=%.2f contrast=%.2f picked=%d (picks:", (double) p.pos / fs, w, (double) f, (double) contrast (w, f, b), picked ? 1 : 0);
                    for (int i = 0; i < pk.n; ++i) std::fprintf (stderr, " %d", pk.pitch[i]);
                    std::fprintf (stderr, ")\n");
                }
           #endif
            for (int i = 0; i < pk.n; ++i)
            {
                const int pitch = pk.pitch[i];
                if (! watch.test ((size_t) pitch) || p.hitWatch.test ((size_t) pitch) || pk.f[i] < tune.watchMin)
                    continue;
                const auto& model = models[(size_t) keyIndex (pitch)];
                const float f = score (model, evid.data(), b);
               #ifdef FALL_DETECT_TRACE
                std::fprintf (stderr, "[trace] watch candidate %d f=%.2f explained=%d picks=%d\n", pitch, (double) f, explainedFromBelow (pitch, b) ? 1 : 0, pk.n);
               #endif
                if (f < tune.watchMin || explainedFromBelow (pitch, b))
                    continue;
                // A key with one or two partials (C8) is only believable as the sole explanation of the
                // new energy; a rich series (A0) must also stand out from its neighbours' series.
                const bool few = model.chance >= 0.3f;
                const bool ok = few ? (pk.n == 1 || (f - tune.chanceCoef * model.chance >= tune.watchSel && contrast (pitch, f, b) >= tune.watchContrast))
                                    : (f - tune.chanceCoef * model.chance >= tune.watchSel
                                       && scoreIndependent (model, evid.data(), b) >= tune.watchIndependent);
                if (ok)
                {
                    newWatch.set ((size_t) pitch);
                    p.hitWatch.set ((size_t) pitch);
                }
            }
        }

        // hits are reported once per key; display pitches only when not shown for this onset before
        const PitchSet freshShown = shown & ~p.reported;
        const PitchSet fresh = freshShown | newExp | newWatch;
        if (fresh.any())
        {
            p.reported |= fresh;
            if (newExp.any())
                hitSinceSet = true;
            if (out != nullptr && outCount < outMax)
            {
                DetectedOnset& o = out[outCount++];
                o = DetectedOnset();
                o.samplePosition = p.pos;
                o.reportedAt = pos;
                o.strength = std::min (2.0f, p.strength * 0.5f);
                o.expectedHits = newExp;
                o.watchHits = newWatch;
                o.pitches = fresh | newExp | newWatch;
            }
        }
    }

    void runDue()
    {
        for (int guard = 0; guard < 4 * kMaxPending; ++guard)
        {
            bool ran = false;
            for (auto& p : pend)
            {
                if (! p.active || stageDue (p, p.stage) > pos)
                    continue;
                const int stage = p.stage;
                const bool clamped = stageDue (p, stage) >= p.limit;
                runStage (p, stage);
                if (p.capture && clamped && stage < 3)
                    captureArmed = true; // the capture note was cut short by another onset: try again
                ran = true;
                if (clamped || p.stage >= kNumStages || stage >= p.lastStage)
                    p.active = false;
                else
                    p.stage = stage + 1;
                break;
            }
            if (! ran)
                break;
        }
    }

    // ---------------------------------------------------------------------------------------------
    void finishCapture (const EvBand& b)
    {
        // Fit tuning offset and inharmonicity: grid search maximising partial evidence.
        auto windowMax = [&] (double hz, double tolCents, int& kBest)
        {
            const double lo = hz * std::pow (2.0, -tolCents / 1200.0), hi = hz * std::pow (2.0, tolCents / 1200.0);
            int k0 = std::max ((int) std::floor (lo / b.binHz - 0.5), b.kLo);
            int k1 = std::min ((int) std::ceil (hi / b.binHz + 0.5), b.kHi);
            float m = 0.0f;
            kBest = -1;
            for (int k = k0; k <= k1; ++k)
                if (evid[(size_t) k] > m) { m = evid[(size_t) k]; kBest = k; }
            return m;
        };

        double bestScore = -1, bestTune = 0, bestB = 1e-4;
        const double bModel = inharmonicityB (capturePitch);
        for (double tuneC = -40; tuneC <= 40; tuneC += 2.5)
            for (int bi = 0; bi < 13; ++bi)
            {
                const double B = bModel * 0.25 * std::pow (1.3, bi); // 0.25x .. 5.8x the typical value
                double s = 0;
                for (int h = 1; h <= 40; ++h)
                {
                    const double f = partialHz (pitchToHz (capturePitch, tuneC), B, h);
                    if (f > 0.45 * fs || f > 9500.0)
                        break;
                    int kb;
                    s += windowMax (f, 14.0, kb);
                }
                if (s > bestScore) { bestScore = s; bestTune = tuneC; bestB = B; }
            }

        Calibration::Impl::Key key;
        key.tuneCents = (float) bestTune;
        key.B = (float) bestB;
        int found = 0;
        double sum = 0;
        for (int h = 1; h <= kMaxPartials; ++h)
        {
            const double f = partialHz (pitchToHz (capturePitch, bestTune), bestB, h);
            if (f > 0.45 * fs || f > 9500.0)
                break;
            int kb;
            const float s = windowMax (f, 30.0, kb);
            if (kb < 0 || s < 0.35f)
                continue;
            // parabolic refinement on the new-energy amplitude
            double kk = kb;
            const float a = magP[(size_t) kb - 1], c0 = magP[(size_t) kb], d = magP[(size_t) kb + 1];
            const float den = a - 2.0f * c0 + d;
            if (den < 0.0f)
                kk += juce::jlimit (-0.5, 0.5, (double) (0.5f * (a - d) / den));
            if (key.n < kMaxPartials)
            {
                key.h[key.n]  = (uint8_t) h;
                key.hz[key.n] = (float) (kk * b.binHz);
                key.w[key.n]  = c0; // amplitude for now, turned into a weight below
                ++key.n;
            }
            ++found;
            sum += s;
        }
        if (found >= 3 && sum >= 2.0)
        {
            // weight = how loud the partial was relative to the loudest one (-36 dB and below: 0.15)
            float aMax = 1e-12f;
            for (int j = 0; j < key.n; ++j)
                aMax = std::max (aMax, key.w[j]);
            for (int j = 0; j < key.n; ++j)
                key.w[j] = 0.15f + 0.85f * clamp01 ((20.0f * std::log10 (key.w[j] / aMax + 1e-9f) + 36.0f) / 36.0f);
            key.valid = true;
            captured = key;
            capturedPitch = capturePitch;
            captureDone = true;
        }
        else
        {
            captureArmed = true; // not a clear note: keep listening
        }
    }

    // ---------------------------------------------------------------------------------------------
    int process (const float* in, int n, DetectedOnset* o, int maxO)
    {
        out = o;
        outMax = maxO;
        outCount = 0;
        if (! prepared || n <= 0)
            return 0;

        int i = 0;
        while (i < n)
        {
            int64_t target = std::min (nextHop, earliestDue());
            if (target <= pos)
                target = pos + 1;
            const int chunk = (int) std::min<int64_t> ((int64_t) (n - i), target - pos);
            float ms = levelMs;
            for (int j = 0; j < chunk; ++j)
            {
                const float s = in[i + j];
                ring[(size_t) ((pos + j) & ringMask)] = s;
                ms += (s * s - ms) * levelAlpha;
            }
            levelMs = ms;
            pos += chunk;
            i += chunk;
            if (pos >= nextHop)
            {
                hopStep();
                nextHop += hop;
            }
            runDue();
        }
        levelDb.store (10.0f * std::log10 (levelMs + 1e-12f), std::memory_order_relaxed);
        out = nullptr;
        return outCount;
    }
};

// ---------------------------------------------------------------------------------------------
NoteDetector::NoteDetector() : impl (std::make_unique<Impl>()) {}
NoteDetector::~NoteDetector() = default;

void NoteDetector::prepare (double sampleRate, int maxBlockSize) { impl->prepare (sampleRate, maxBlockSize); }
void NoteDetector::reset() { if (impl->prepared) impl->reset(); }

void NoteDetector::setExpected (const PitchSet& pitches)
{
    auto& d = *impl;
    // A new target (or the same target again after a hit, i.e. a repeated chord) invalidates onsets
    // that began before now: their remaining evaluation stages must not satisfy the new chord.
    if (pitches != d.expected || d.hitSinceSet)
    {
        d.expectedStamp = d.pos;
        d.hitSinceSet = false;
    }
    d.expected = pitches;
    d.rebuildMask();
}

bool NoteDetector::setTuningOverride (const juce::String& name, float value)
{
    for (int i = 0; i < Impl::kNumOverrides; ++i)
        if (name == Impl::overrideName (i))
        {
            impl->overrideSet[i] = true;
            impl->overrideValue[i] = value;
            impl->applySensitivity();
            return true;
        }
    return false;
}

void NoteDetector::setWatchKeys (const PitchSet& pitches) { impl->watch = pitches; }

void NoteDetector::setSensitivity (float s)
{
    impl->sensitivity = s;
    impl->applySensitivity();
}

void NoteDetector::setCalibration (const Calibration& c)
{
    impl->cal = *c.impl;
    impl->haveCal = c.numKeys() > 0;
    impl->rebuildModels();
}

int NoteDetector::process (const float* mono, int numSamples, DetectedOnset* out, int maxOut)
{
    return impl->process (mono, numSamples, out, maxOut);
}

float NoteDetector::getInputLevelDb() const noexcept { return impl->levelDb.load (std::memory_order_relaxed); }

float NoteDetector::getTuningCents (bool* confident) const noexcept
{
    if (confident != nullptr)
        *confident = impl->tuneConfident.load (std::memory_order_relaxed);
    return impl->tuneEstimate.load (std::memory_order_relaxed);
}

void NoteDetector::beginCapture (int pitch)
{
    auto& d = *impl;
    d.capturePitch = juce::jlimit (kLowestPianoKey, kHighestPianoKey, pitch);
    d.captureArmed = true;
    d.captureDone = false;
}

bool NoteDetector::captureFinished() const noexcept { return impl->captureDone; }

void NoteDetector::storeCapture (Calibration& into) const
{
    if (impl->captureDone)
        into.impl->keys[(size_t) keyIndex (impl->capturedPitch)] = impl->captured;
}

} // namespace fall
