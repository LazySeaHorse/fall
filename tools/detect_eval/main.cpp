// fall_detect_eval: scores NoteDetector on a recording.
//
//   fall_detect_eval <audio.wav> <truth.mid|truth.csv> [more pairs...] [options]
//   fall_detect_eval --dir data/generated/medium --truth data/generated/midi [options]
//
// The truth is what was played: a MIDI file (tempo map honoured) or a CSV with the header
// "onset,pitch,duration,velocity". Use --offset if the recording starts later/earlier than the MIDI.
//
// Reports (a) a learn-mode simulation (setExpected = next ground-truth chord, advanced the way the
// Player does), (b) wrong-note rejection (expected = a distractor chord while the real one is
// played), (c) jump-key false fires, (d) free "display" detection precision/recall/F1 with +-50 ms
// tolerance, per-octave breakdown, and CPU time per second of audio.
#include "core/Song.h"
#include "detect/NoteDetector.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <tuple>
#if JUCE_LINUX || JUCE_MAC || JUCE_BSD
 #include <time.h>
#endif

using namespace fall;

namespace
{
// CPU time of the calling thread (immune to the machine being busy); wall clock where unavailable.
double threadSeconds()
{
   #if JUCE_LINUX || JUCE_MAC || JUCE_BSD
    timespec ts;
    clock_gettime (CLOCK_THREAD_CPUTIME_ID, &ts);
    return (double) ts.tv_sec + 1e-9 * (double) ts.tv_nsec;
   #else
    return std::chrono::duration<double> (std::chrono::steady_clock::now().time_since_epoch()).count();
   #endif
}

struct Options
{
    std::vector<std::pair<juce::File, juce::File>> pairs;
    double offset = 0.0;
    int    block = 480;       // samples per process() call (0 = pseudo-random 128..1024)
    float  sens = 0.6f;
    double timeout = 1.0;     // learn mode: give up on a chord this long after its last note
    juce::File csvOut;
    bool   learn = true, wrong = true, display = true;
    bool   perFile = true;
    double resample = 0.0;    // pretend the recording has this sample rate (for testing only)
    bool   verbose = false;
};

struct Truth
{
    Song song;
    std::vector<NoteGroup> groups;
};

bool loadTruth (const juce::File& f, double offset, Truth& t, juce::String& err)
{
    if (f.hasFileExtension ("csv"))
    {
        juce::StringArray lines;
        f.readLines (lines);
        for (int i = 1; i < lines.size(); ++i)
        {
            auto tok = juce::StringArray::fromTokens (lines[i], ",", "");
            if (tok.size() < 2)
                continue;
            Note n;
            n.start = tok[0].getDoubleValue();
            n.pitch = tok[1].getIntValue();
            n.end   = n.start + (tok.size() > 2 ? tok[2].getDoubleValue() : 0.2);
            t.song.notes.push_back (n);
        }
        std::stable_sort (t.song.notes.begin(), t.song.notes.end(),
                          [] (const Note& a, const Note& b) { return std::tie (a.start, a.pitch) < std::tie (b.start, b.pitch); });
    }
    else if (! loadMidiFile (f, t.song, err))
        return false;

    for (auto& n : t.song.notes)
    {
        n.start += offset;
        n.end += offset;
        n.hand = Hand::Right;
    }
    t.groups = buildGroups (t.song, HandBoth, 0.05);
    return ! t.song.notes.empty();
}

bool loadAudio (const juce::File& f, std::vector<float>& mono, double& sampleRate)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
    if (r == nullptr)
        return false;
    sampleRate = r->sampleRate;
    const int n = (int) r->lengthInSamples;
    juce::AudioBuffer<float> buf ((int) r->numChannels, n);
    r->read (&buf, 0, n, 0, true, true);
    mono.assign ((size_t) n, 0.0f);
    for (int c = 0; c < buf.getNumChannels(); ++c)
        for (int i = 0; i < n; ++i)
            mono[(size_t) i] += buf.getSample (c, i) / (float) buf.getNumChannels();
    return true;
}

// --- statistics ------------------------------------------------------------------------------
double percentile (std::vector<double> v, double p)
{
    if (v.empty())
        return -1;
    std::sort (v.begin(), v.end());
    const double idx = p * (double) (v.size() - 1);
    const size_t lo = (size_t) idx;
    const size_t hi = std::min (v.size() - 1, lo + 1);
    return v[lo] + (v[hi] - v[lo]) * (idx - (double) lo);
}

int registerOf (int pitch) { return pitch < 48 ? 0 : (pitch < 72 ? 1 : 2); }
const char* registerName (int r) { return r == 0 ? "bass  A0-B2" : (r == 1 ? "mid   C3-B4" : "treble C5-C8"); }

struct LearnStats
{
    std::vector<float> tuning; // estimated piano tuning per file (999 = not confident)
    int noteTotal = 0, noteHit = 0, groupTotal = 0, groupHit = 0, premature = 0;
    std::vector<double> latency[3];
    std::vector<double> latencyAll;
    std::vector<double> groupLatency;
    int missByRegister[3] {}, totalByRegister[3] {};
    double audioSeconds = 0;

    void add (const LearnStats& o)
    {
        noteTotal += o.noteTotal; noteHit += o.noteHit; groupTotal += o.groupTotal; groupHit += o.groupHit;
        premature += o.premature; audioSeconds += o.audioSeconds;
        tuning.insert (tuning.end(), o.tuning.begin(), o.tuning.end());
        for (int r = 0; r < 3; ++r)
        {
            latency[r].insert (latency[r].end(), o.latency[r].begin(), o.latency[r].end());
            missByRegister[r] += o.missByRegister[r];
            totalByRegister[r] += o.totalByRegister[r];
        }
        latencyAll.insert (latencyAll.end(), o.latencyAll.begin(), o.latencyAll.end());
        groupLatency.insert (groupLatency.end(), o.groupLatency.begin(), o.groupLatency.end());
    }
};

constexpr int kNumShifts = 6;
struct WrongStats
{
    // [0] = nothing but the played chord is sounding, [1] = other notes are still ringing
    int total[2][kNumShifts] {}, accepted[2][kNumShifts] {};
    void add (const WrongStats& o)
    {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < kNumShifts; ++i) { total[c][i] += o.total[c][i]; accepted[c][i] += o.accepted[c][i]; }
    }
};

struct WatchStats
{
    int trueFires = 0, falseFires = 0, trueNotes = 0;
    double seconds = 0;
    void add (const WatchStats& o) { trueFires += o.trueFires; falseFires += o.falseFires; trueNotes += o.trueNotes; seconds += o.seconds; }
};

struct DisplayStats
{
    int tp = 0, fp = 0, fn = 0, tpOct = 0, fpOct = 0;
    int octTp[10] {}, octFn[10] {}, octFp[10] {};
    std::vector<double> timing; // predicted - true onset (s)
    void add (const DisplayStats& o)
    {
        tp += o.tp; fp += o.fp; fn += o.fn; tpOct += o.tpOct; fpOct += o.fpOct;
        for (int i = 0; i < 10; ++i) { octTp[i] += o.octTp[i]; octFn[i] += o.octFn[i]; octFp[i] += o.octFp[i]; }
        timing.insert (timing.end(), o.timing.begin(), o.timing.end());
    }
};

struct CpuStats
{
    double seconds = 0, audio = 0, worstBlockMs = 0;
    std::vector<double> blockMs; // every process() call, for percentiles
};

// --- running the detector ---------------------------------------------------------------------
struct Event { double t, reported; float strength; PitchSet exp, watch, pitches; double blockEnd; };

class Runner
{
public:
    Runner (const std::vector<float>& a, double sr, const Options& o) : audio (a), rate (sr), opt (o) {}

    // Calls `onBlock(events, blockEndSeconds)` after every process() call.
    template <typename F>
    void run (NoteDetector& det, F&& onBlock, CpuStats& cpu)
    {
        std::vector<DetectedOnset> buf (64);
        uint32_t rng = 12345;
        size_t i = 0;
        std::vector<Event> evs;
        while (i < audio.size())
        {
            int bs = opt.block;
            if (bs <= 0)
            {
                rng = rng * 1664525u + 1013904223u;
                bs = 128 + (int) ((rng >> 16) % 897u);
            }
            const int n = (int) std::min<size_t> ((size_t) bs, audio.size() - i);
            const double t0 = threadSeconds();
            const int cnt = det.process (audio.data() + i, n, buf.data(), (int) buf.size());
            const double dt = threadSeconds() - t0;
            cpu.seconds += dt;
            cpu.worstBlockMs = std::max (cpu.worstBlockMs, dt * 1000.0);
            cpu.blockMs.push_back (dt * 1000.0 * 480.0 / (double) n); // normalised to a 10 ms block
            i += (size_t) n;
            const double endT = (double) i / rate;
            evs.clear();
            for (int k = 0; k < cnt; ++k)
                evs.push_back ({ (double) buf[(size_t) k].samplePosition / rate, (double) buf[(size_t) k].reportedAt / rate,
                                 buf[(size_t) k].strength, buf[(size_t) k].expectedHits, buf[(size_t) k].watchHits,
                                 buf[(size_t) k].pitches, endT });
            onBlock (evs, endT);
        }
        cpu.audio += (double) audio.size() / rate;
    }

private:
    const std::vector<float>& audio;
    double rate;
    const Options& opt;
};

void setup (NoteDetector& det, double rate, const Options& opt)
{
    det.prepare (rate, 2048);
    det.setSensitivity (opt.sens);
    PitchSet w;
    w.set (21); w.set (23); w.set (108);
    det.setWatchKeys (w);
}

double groupLastStart (const Song& s, const NoteGroup& g)
{
    double t = g.time;
    for (int idx : g.notes)
        t = std::max (t, s.notes[(size_t) idx].start);
    return t;
}

// Learn-mode simulation, the way Player drives the detector.
void runLearn (const std::vector<float>& audio, double rate, const Truth& tr, const Options& opt,
               LearnStats& ls, WatchStats& ws, CpuStats& cpu, std::vector<Event>* allEvents)
{
    NoteDetector det;
    setup (det, rate, opt);
    Runner runner (audio, rate, opt);
    const auto& S = tr.song;
    const auto& G = tr.groups;
    size_t g = 0;
    PitchSet satisfied;
    std::vector<bool> noteDone (S.notes.size(), false);
    if (! G.empty())
        det.setExpected (G[0].pitches);

    ls.audioSeconds += (double) audio.size() / rate;
    ws.seconds += (double) audio.size() / rate;

    auto finishGroup = [&] (bool completed, double now)
    {
        const auto& grp = G[g];
        ++ls.groupTotal;
        if (completed)
        {
            ++ls.groupHit;
            ls.groupLatency.push_back ((now - groupLastStart (S, grp)) * 1000.0);
        }
        for (int idx : grp.notes)
        {
            const auto& n = S.notes[(size_t) idx];
            ++ls.noteTotal;
            ++ls.totalByRegister[registerOf (n.pitch)];
            if (noteDone[(size_t) idx])
                ++ls.noteHit;
            else
            {
                ++ls.missByRegister[registerOf (n.pitch)];
                if (opt.verbose)
                    std::printf ("    [miss] note %d at %.3f\n", n.pitch, n.start);
            }
        }
        ++g;
        satisfied.reset();
        if (g < G.size())
            det.setExpected (G[g].pitches);
        else
            det.setExpected ({});
    };

    runner.run (det, [&] (const std::vector<Event>& evs, double blockEnd)
    {
        for (auto& e : evs)
        {
            if (allEvents != nullptr)
                allEvents->push_back (e);

            // jump keys
            for (int p : { 21, 23, 108 })
                if (e.watch.test ((size_t) p))
                {
                    bool real = false;
                    for (auto& n : S.notes)
                        if (n.pitch == p && e.t > n.start - 0.1 && e.t < n.start + 0.4)
                            real = true;
                    ++(real ? ws.trueFires : ws.falseFires);
                }

            if (g >= G.size())
                continue;
            const auto& grp = G[g];
            for (int idx : grp.notes)
            {
                const auto& n = S.notes[(size_t) idx];
                // like the app: one heard note of a chord completes the chord
                if (noteDone[(size_t) idx] || ! e.exp.any())
                    continue;
                noteDone[(size_t) idx] = true;
                satisfied.set ((size_t) n.pitch);
                if (e.t < n.start - 0.08)
                    ++ls.premature;
                const double lat = (blockEnd - n.start) * 1000.0;
                if (opt.verbose && (lat > 150.0 || e.t < n.start - 0.08))
                    std::printf ("    [odd] note %d at %.3f: accepted by onset at %.3f, latency %.0f ms\n", n.pitch, n.start, e.t, lat);
                ls.latency[registerOf (n.pitch)].push_back (lat);
                ls.latencyAll.push_back (lat);
            }
            if (satisfied == grp.pitches)
                finishGroup (true, blockEnd);
        }
        // time out chords that were never heard
        // (a real Player would stall; here we move on shortly before the next chord so one miss
        // doesn't desynchronise the rest of the run)
        while (g < G.size())
        {
            const double last = groupLastStart (S, G[g]);
            double deadline = last + opt.timeout;
            if (g + 1 < G.size())
                deadline = std::min (deadline, G[g + 1].time - 0.15);
            if (blockEnd <= std::max (deadline, last + 0.3))
                break;
            finishGroup (false, blockEnd);
        }
    }, cpu);
    while (g < G.size())
        finishGroup (false, 0);
    bool tuneOk = false;
    const float tuneC = det.getTuningCents (&tuneOk);
    ls.tuning.push_back (tuneOk ? tuneC : 999.0f);

    for (auto& n : S.notes)
        if (n.pitch == 21 || n.pitch == 23 || n.pitch == 108)
            ++ws.trueNotes;
}

// Wrong-note test: while the real chord g sounds, the detector is told to expect a different chord.
void runWrong (const std::vector<float>& audio, double rate, const Truth& tr, const Options& opt, WrongStats& st, CpuStats& cpu)
{
    // -12 / -24: the key that sounds is an octave (two) ABOVE the expected one
    const int shifts[kNumShifts] = { 1, -1, 2, 7, -12, -24 };
    const auto& S = tr.song;
    const auto& G = tr.groups;
    static const int onlyShift = std::getenv ("FALL_SHIFT") ? std::atoi (std::getenv ("FALL_SHIFT")) : 0;
    for (int si = 0; si < kNumShifts; ++si)
    {
        if (onlyShift != 0 && shifts[si] != onlyShift)
            continue;
        NoteDetector det;
        setup (det, rate, opt);
        Runner runner (audio, rate, opt);

        // distractor chord per group: shifted pitches that are not part of what actually sounds then
        std::vector<PitchSet> distract (G.size());
        std::vector<int> context (G.size(), 0);
        for (size_t g = 0; g < G.size(); ++g)
        {
            PitchSet sounding;
            for (auto& n : S.notes)
                if (n.start < G[g].time + 0.15 && n.end > G[g].time - 0.6)
                    sounding.set ((size_t) n.pitch);
            context[g] = (sounding & ~G[g].pitches).any() ? 1 : 0;
            for (size_t p = 21; p <= 108; ++p)
                if (G[g].pitches.test (p))
                {
                    const int q = (int) p + shifts[si];
                    if (q >= 21 && q <= 108 && ! sounding.test ((size_t) q))
                        distract[g].set ((size_t) q);
                }
        }

        size_t cur = 0;
        PitchSet accepted;
        auto finish = [&]
        {
            st.total[context[cur]][si] += (int) distract[cur].count();
            st.accepted[context[cur]][si] += (int) accepted.count();
            accepted.reset();
            ++cur;
        };
        // Expected is set 150 ms before the chord, the way a waiting Player would have it.
        det.setExpected (distract.empty() ? PitchSet() : distract[0]);
        runner.run (det, [&] (const std::vector<Event>& evs, double blockEnd)
        {
            for (auto& e : evs)
            {
                if (cur >= G.size())
                    continue;
                const double until = cur + 1 < G.size() ? std::min (G[cur].time + 0.5, G[cur + 1].time - 0.06) : G[cur].time + 0.5;
                if (e.t > G[cur].time - 0.08 && e.t < until)
                    accepted |= e.exp;
            }
            while (cur < G.size() && blockEnd > G[cur].time + 0.55)
            {
                finish();
                if (cur < G.size())
                    det.setExpected (distract[cur]);
            }
        }, cpu);
        while (cur < G.size())
            finish();
    }
}

void evalDisplay (const std::vector<Event>& evs, const Truth& tr, DisplayStats& ds)
{
    struct P { double t; int pitch; bool used; };
    std::vector<P> preds;
    for (auto& e : evs)
        for (size_t p = 21; p <= 108; ++p)
            if (e.pitches.test (p))
            {
                bool dup = false;
                for (auto& q : preds)
                    if (q.pitch == (int) p && std::abs (q.t - e.t) < 0.06)
                        dup = true;
                if (! dup)
                    preds.push_back ({ e.t, (int) p, false });
            }

    const auto& S = tr.song;
    std::vector<bool> matched (S.notes.size(), false);
    for (int pass = 0; pass < 2; ++pass) // pass 1: octave-folded matching of what is left
    {
        for (size_t i = 0; i < S.notes.size(); ++i)
        {
            if (matched[i])
                continue;
            int best = -1;
            double bestDt = 1e9;
            for (size_t k = 0; k < preds.size(); ++k)
            {
                if (preds[k].used)
                    continue;
                const bool same = pass == 0 ? preds[k].pitch == S.notes[i].pitch : (preds[k].pitch % 12 == S.notes[i].pitch % 12);
                const double dt = preds[k].t - S.notes[i].start;
                if (same && std::abs (dt) <= 0.05 && std::abs (dt) < bestDt)
                {
                    bestDt = std::abs (dt);
                    best = (int) k;
                }
            }
            if (best >= 0)
            {
                preds[(size_t) best].used = true;
                matched[i] = true;
                if (pass == 0)
                {
                    ++ds.tp;
                    ++ds.octTp[std::clamp (S.notes[i].pitch / 12 - 1, 0, 9)];
                    ds.timing.push_back (preds[(size_t) best].t - S.notes[i].start);
                }
                ++ds.tpOct;
            }
        }
        if (pass == 0)
        {
            for (size_t i = 0; i < S.notes.size(); ++i)
                if (! matched[i])
                {
                    ++ds.fn;
                    ++ds.octFn[std::clamp (S.notes[i].pitch / 12 - 1, 0, 9)];
                }
            for (auto& p : preds)
                if (! p.used)
                {
                    ++ds.fp;
                    ++ds.octFp[std::clamp (p.pitch / 12 - 1, 0, 9)];
                }
        }
    }
    for (auto& p : preds)
        if (! p.used)
            ++ds.fpOct;
}

void printLearn (const char* title, const LearnStats& l, const WatchStats& w, const CpuStats& cpu)
{
    std::printf ("%s\n", title);
    std::printf ("  learn: notes %d/%d heard = recall %.1f%%   chords %d/%d complete = %.1f%%   premature accepts %d (%.2f/min)\n",
                 l.noteHit, l.noteTotal, 100.0 * l.noteHit / std::max (1, l.noteTotal), l.groupHit, l.groupTotal,
                 100.0 * l.groupHit / std::max (1, l.groupTotal), l.premature, l.premature * 60.0 / std::max (1.0, l.audioSeconds));
    std::printf ("  tuning estimate (cents, per file; x = not confident):");
    for (float t : l.tuning)
        t > 900.0f ? std::printf (" x") : std::printf (" %+.0f", (double) t);
    std::printf ("\n");
    std::printf ("  latency (ms, until process() returned it)   n     p50    p90    p99    miss%%\n");
    for (int r = 0; r < 3; ++r)
        std::printf ("    %-14s                        %5zu %6.0f %6.0f %6.0f %6.1f\n", registerName (r), l.latency[r].size(),
                     percentile (l.latency[r], 0.5), percentile (l.latency[r], 0.9), percentile (l.latency[r], 0.99),
                     100.0 * l.missByRegister[r] / std::max (1, l.totalByRegister[r]));
    std::printf ("    %-14s                        %5zu %6.0f %6.0f %6.0f\n", "all notes", l.latencyAll.size(),
                 percentile (l.latencyAll, 0.5), percentile (l.latencyAll, 0.9), percentile (l.latencyAll, 0.99));
    std::printf ("  jump keys (A0/B0/C8): %d correct, %d false fires (%.2f/min), %d such notes in truth\n", w.trueFires,
                 w.falseFires, w.falseFires * 60.0 / std::max (1.0, w.seconds), w.trueNotes);
    (void) cpu;
}

void printWrong (const WrongStats& w)
{
    const char* names[kNumShifts] = { "+1 semitone", "-1 semitone", "+2 semitones", "+7 (fifth)", "played 8va", "played 15ma" };
    const char* ctx[2] = { "clean context", "dense context" };
    for (int c = 0; c < 2; ++c)
    {
        std::printf ("  wrong-note false accepts, %s:", ctx[c]);
        for (int i = 0; i < kNumShifts; ++i)
            std::printf ("  %s %d/%d = %.1f%%", names[i], w.accepted[c][i], w.total[c][i], 100.0 * w.accepted[c][i] / std::max (1, w.total[c][i]));
        std::printf ("\n");
    }
}

void printDisplay (const DisplayStats& d)
{
    const double p = d.tp / double (std::max (1, d.tp + d.fp)), r = d.tp / double (std::max (1, d.tp + d.fn));
    const double f1 = 2 * p * r / std::max (1e-9, p + r);
    const double po = (d.tp + d.tpOct - d.tp) / double (std::max (1, d.tp + d.fp)); // tpOct includes exact
    std::printf ("  display: precision %.1f%%  recall %.1f%%  F1 %.1f%%   (octave-folded recall %.1f%%)   onset offset p50 %.0f ms\n",
                 100 * p, 100 * r, 100 * f1, 100.0 * d.tpOct / double (std::max (1, d.tp + d.fn)), percentile (d.timing, 0.5) * 1000.0);
    (void) po;
    std::printf ("  octave   :");
    for (int o = 0; o < 9; ++o)
        std::printf ("   C%d", o);
    std::printf ("\n  recall %% :");
    for (int o = 0; o < 9; ++o)
        std::printf (" %4.0f", d.octTp[o] + d.octFn[o] > 0 ? 100.0 * d.octTp[o] / (d.octTp[o] + d.octFn[o]) : -1.0);
    std::printf ("\n  precis. %% :");
    for (int o = 0; o < 9; ++o)
        std::printf (" %4.0f", d.octTp[o] + d.octFp[o] > 0 ? 100.0 * d.octTp[o] / (d.octTp[o] + d.octFp[o]) : -1.0);
    std::printf ("\n  n truth  :");
    for (int o = 0; o < 9; ++o)
        std::printf (" %4d", d.octTp[o] + d.octFn[o]);
    std::printf ("\n");
}

void printCpu (const CpuStats& c)
{
    std::printf ("  cpu: %.2f ms per second of audio (%.3f%% of one core); per 10 ms block: p99 %.2f ms, p99.9 %.2f ms; worst single process() call %.2f ms\n",
                 1000.0 * c.seconds / std::max (1e-9, c.audio), 100.0 * c.seconds / std::max (1e-9, c.audio),
                 percentile (c.blockMs, 0.99), percentile (c.blockMs, 0.999), c.worstBlockMs);
}

void dumpCsv (const juce::File& f, const std::vector<Event>& evs)
{
    std::ostringstream os;
    os << "onset_s,reported_s,strength,expected_hits,watch_hits,pitches\n";
    auto list = [] (const PitchSet& s)
    {
        std::string o;
        for (size_t p = 0; p < 128; ++p)
            if (s.test (p))
                o += (o.empty() ? "" : " ") + std::to_string (p);
        return o;
    };
    for (auto& e : evs)
        os << e.t << "," << e.reported << "," << e.strength << "," << list (e.exp) << "," << list (e.watch) << "," << list (e.pitches) << "\n";
    f.replaceWithText (os.str());
}

// False-accept test on non-piano sounds (snaps, claps, knocks ...): before every event the
// detector is told to expect a random chord; any expected hit near the event is a false accept.
void runTransients (const std::vector<float>& audio, double rate, const juce::File& eventsCsv, const Options& opt)
{
    struct Ev { double t; juce::String kind; };
    std::vector<Ev> evs;
    juce::StringArray lines;
    eventsCsv.readLines (lines);
    for (int i = 1; i < lines.size(); ++i)
    {
        auto tok = juce::StringArray::fromTokens (lines[i], ",", "");
        if (tok.size() >= 2)
            evs.push_back ({ tok[0].getDoubleValue(), tok[1].trim() });
    }
    std::map<juce::String, std::array<int, 4>> perKind; // events, expected accepted, display shown, jump fired
    const int passes = 4;
    for (int pass = 0; pass < passes; ++pass)
    {
        NoteDetector det;
        setup (det, rate, opt);
        Runner runner (audio, rate, opt);
        uint32_t rng = 777u + (uint32_t) pass * 1000003u;
        auto rnd = [&] (int n) { rng = rng * 1664525u + 1013904223u; return (int) ((rng >> 8) % (uint32_t) n); };
        std::vector<PitchSet> chords (evs.size());
        for (auto& c : chords)
        {
            const int root = 36 + rnd (61);
            c.set ((size_t) root);
            const int kind = rnd (3); // single note, dyad, triad
            if (kind >= 1) c.set ((size_t) std::min (108, root + 3 + rnd (2)));
            if (kind >= 2) c.set ((size_t) std::min (108, root + 7));
        }
        std::vector<std::array<bool, 3>> hit (evs.size(), { false, false, false });
        size_t next = 0;      // next event whose chord must be set
        CpuStats cpu;
        runner.run (det, [&] (const std::vector<Event>& es, double blockEnd)
        {
            while (next < evs.size() && blockEnd >= evs[next].t - 0.6)
                det.setExpected (chords[next++]);
            for (auto& e : es)
                for (size_t k = 0; k < evs.size(); ++k)
                    if (e.t > evs[k].t - 0.05 && e.t < evs[k].t + 0.6)
                    {
                        if (opt.verbose && pass == 0 && (e.exp & chords[k]).any())
                        {
                            std::printf ("    [accept] %s at %.2f (onset %.3f, +%.0f ms): expected", evs[k].kind.toRawUTF8(), evs[k].t, e.t, (e.reported - e.t) * 1000.0);
                            for (int p = 21; p <= 108; ++p) if (chords[k].test ((size_t) p)) std::printf (" %d", p);
                            std::printf (" hit");
                            for (int p = 21; p <= 108; ++p) if (e.exp.test ((size_t) p)) std::printf (" %d", p);
                            std::printf ("\n");
                        }
                        hit[k][0] = hit[k][0] || (e.exp & chords[k]).any();
                        hit[k][1] = hit[k][1] || e.pitches.any();
                        hit[k][2] = hit[k][2] || e.watch.any();
                    }
        }, cpu);
        for (size_t k = 0; k < evs.size(); ++k)
        {
            auto& a = perKind[evs[k].kind];
            ++a[0];
            a[1] += hit[k][0];
            a[2] += hit[k][1];
            a[3] += hit[k][2];
        }
    }
    std::printf ("  false accepts on non-piano sounds (%d passes, random expected chord per event)\n", passes);
    std::printf ("    %-8s %6s %10s %10s %8s\n", "kind", "events", "accepted", "display", "jumpkey");
    std::array<int, 4> tot {};
    for (auto& [k, a] : perKind)
    {
        std::printf ("    %-8s %6d %9.1f%% %9.1f%% %7.1f%%\n", k.toRawUTF8(), a[0], 100.0 * a[1] / a[0], 100.0 * a[2] / a[0], 100.0 * a[3] / a[0]);
        for (int i = 0; i < 4; ++i) tot[(size_t) i] += a[(size_t) i];
    }
    std::printf ("    %-8s %6d %9.1f%% %9.1f%% %7.1f%%\n", "ALL", tot[0], 100.0 * tot[1] / std::max (1, tot[0]), 100.0 * tot[2] / std::max (1, tot[0]), 100.0 * tot[3] / std::max (1, tot[0]));
}


// Replays a "Save what Fall heard" recording: the expected notes change at the times Fall logged
// them, and every onset the detector reports is printed next to what the app reported.
void runReplay (const std::vector<float>& audio, double rate, const juce::File& logCsv, const Options& opt)
{
    struct Line { double t; bool expected; PitchSet a, b; };
    std::vector<Line> log;
    juce::StringArray lines;
    logCsv.readLines (lines);
    for (auto& l : lines)
    {
        if (l.startsWith ("#") || l.startsWith ("time") || l.trim().isEmpty())
            continue;
        auto tok = juce::StringArray::fromTokens (l, ",", "");
        while (tok.size() < 4) tok.add ({});
        Line e { tok[0].getDoubleValue(), tok[1] == "expected", {}, {} };
        for (auto& p : juce::StringArray::fromTokens (tok[2], " ", "")) if (p.isNotEmpty()) e.a.set ((size_t) p.getIntValue());
        for (auto& p : juce::StringArray::fromTokens (tok[3], " ", "")) if (p.isNotEmpty()) e.b.set ((size_t) p.getIntValue());
        log.push_back (e);
    }
    auto str = [] (const PitchSet& s)
    {
        juce::String r;
        for (int p = 0; p < 128; ++p) if (s.test ((size_t) p)) r << (r.isEmpty() ? "" : " ") << p;
        return r.isEmpty() ? juce::String ("-") : r;
    };
    NoteDetector det;
    setup (det, rate, opt);
    det.setWatchKeys ({});
    Runner runner (audio, rate, opt);
    size_t next = 0;
    PitchSet cur;
    CpuStats cpu;
    runner.run (det, [&] (const std::vector<Event>& es, double blockEnd)
    {
        while (next < log.size() && blockEnd >= log[next].t)
        {
            const auto& l = log[next++];
            if (l.expected) { cur = l.a; det.setExpected (cur); std::printf ("%8.3f  ---- expected %s\n", l.t, str (cur).toRawUTF8()); }
            else std::printf ("%8.3f  app  heard %s accepted %s\n", l.t, str (l.a).toRawUTF8(), str (l.b).toRawUTF8());
        }
        for (auto& e : es)
            std::printf ("%8.3f  now  +%3.0fms str %.2f  shown %s  accepted %s\n", e.t, (e.reported - e.t) * 1000.0, e.strength,
                         str (e.pitches).toRawUTF8(), str (e.exp).toRawUTF8());
    }, cpu);
    bool conf = false;
    const float c = det.getTuningCents (&conf);
    std::printf ("tuning %.1f cents%s\n", c, conf ? "" : " (not confident)");
}

void usage()
{
    std::puts ("usage: fall_detect_eval <audio.wav> <truth.mid|csv> [<audio2> <truth2> ...] [options]\n"
               "       fall_detect_eval --dir <wavdir> --truth <truthdir> [options]   (pairs by file name; .mid or .csv)\n"
               "options: --offset <s>  --block <n|0=random>  --sens <0..1>  --timeout <s>  --csv <file>\n"
               "         --replay <recording.wav> <recording.csv> (a \"Save what Fall heard\" pair: what the detector reports now)\n"
               "         --transients <noises.wav> <events.csv> (false accepts on snaps/claps/knocks; see tools/detect/gen_noises.py)\n"
               "         --only learn|wrong|display (default all)  --quiet (totals only)  --names a,b,c (with --dir)");
}
} // namespace

int main (int argc, char** argv)
{
    Options opt;
    std::vector<juce::String> positional;
    juce::File dir, truthDir;
    juce::StringArray names;
    std::vector<std::pair<juce::File, juce::File>> transients, replays;
    for (int i = 1; i < argc; ++i)
    {
        const juce::String a (argv[i]);
        auto next = [&] { return i + 1 < argc ? juce::String (argv[++i]) : juce::String(); };
        if (a == "--offset") opt.offset = next().getDoubleValue();
        else if (a == "--block") opt.block = next().getIntValue();
        else if (a == "--sens") opt.sens = next().getFloatValue();
        else if (a == "--timeout") opt.timeout = next().getDoubleValue();
        else if (a == "--csv") opt.csvOut = juce::File::getCurrentWorkingDirectory().getChildFile (next());
        else if (a == "--dir") dir = juce::File::getCurrentWorkingDirectory().getChildFile (next());
        else if (a == "--truth") truthDir = juce::File::getCurrentWorkingDirectory().getChildFile (next());
        else if (a == "--names") names = juce::StringArray::fromTokens (next(), ",", "");
        else if (a == "--quiet") opt.perFile = false;
        else if (a == "--verbose" || a == "-v") opt.verbose = true;
        else if (a == "--only")
        {
            const auto m = next();
            opt.learn = m == "learn";
            opt.wrong = m == "wrong";
            opt.display = m == "display";
        }
        else if (a == "--transients")
        {
            const auto w = next();
            const auto c = next();
            transients.push_back ({ juce::File::getCurrentWorkingDirectory().getChildFile (w), juce::File::getCurrentWorkingDirectory().getChildFile (c) });
        }
        else if (a == "--replay")
        {
            const auto w = next();
            const auto c = next();
            replays.push_back ({ juce::File::getCurrentWorkingDirectory().getChildFile (w), juce::File::getCurrentWorkingDirectory().getChildFile (c) });
        }
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else positional.push_back (a);
    }

    if (dir != juce::File())
    {
        for (auto& f : dir.findChildFiles (juce::File::findFiles, false, "*.wav"))
        {
            if (! names.isEmpty() && ! names.contains (f.getFileNameWithoutExtension()))
                continue;
            juce::File t = truthDir.getChildFile (f.getFileNameWithoutExtension() + ".mid");
            opt.pairs.push_back ({ f, t });
        }
        std::sort (opt.pairs.begin(), opt.pairs.end(), [] (auto& a, auto& b) { return a.first.getFileName() < b.first.getFileName(); });
    }
    for (size_t i = 0; i + 1 < positional.size(); i += 2)
        opt.pairs.push_back ({ juce::File::getCurrentWorkingDirectory().getChildFile (positional[i]),
                               juce::File::getCurrentWorkingDirectory().getChildFile (positional[i + 1]) });
    for (auto& [w, c] : transients)
    {
        std::vector<float> audio;
        double rate = 48000;
        if (! loadAudio (w, audio, rate))
        {
            std::fprintf (stderr, "cannot read %s\n", w.getFullPathName().toRawUTF8());
            return 2;
        }
        std::printf ("%s\n", w.getFullPathName().toRawUTF8());
        runTransients (audio, rate, c, opt);
    }
    for (auto& [w, c] : replays)
    {
        std::vector<float> audio;
        double rate = 48000;
        if (! loadAudio (w, audio, rate))
        {
            std::fprintf (stderr, "cannot read %s\n", w.getFullPathName().toRawUTF8());
            return 2;
        }
        runReplay (audio, rate, c, opt);
    }
    if (opt.pairs.empty() && ! (transients.empty() && replays.empty()))
        return 0;
    if (opt.pairs.empty())
    {
        usage();
        return 1;
    }

    LearnStats learnTotal;
    WrongStats wrongTotal;
    WatchStats watchTotal;
    DisplayStats displayTotal;
    CpuStats cpuTotal;
    std::vector<Event> csvEvents;

    for (auto& [wav, truthFile] : opt.pairs)
    {
        std::vector<float> audio;
        double rate = 48000;
        if (! loadAudio (wav, audio, rate))
        {
            std::fprintf (stderr, "cannot read %s\n", wav.getFullPathName().toRawUTF8());
            return 2;
        }
        Truth truth;
        juce::String err;
        if (! loadTruth (truthFile, opt.offset, truth, err))
        {
            std::fprintf (stderr, "cannot read truth %s %s\n", truthFile.getFullPathName().toRawUTF8(), err.toRawUTF8());
            return 2;
        }

        LearnStats l;
        WatchStats w;
        WrongStats wr;
        DisplayStats d;
        CpuStats cpu;
        std::vector<Event> events;
        if (opt.learn || opt.display)
            runLearn (audio, rate, truth, opt, l, w, cpu, &events);
        if (opt.wrong)
            runWrong (audio, rate, truth, opt, wr, cpu);
        if (opt.display)
            evalDisplay (events, truth, d);
        if (! opt.csvOut.getFullPathName().isEmpty())
            dumpCsv (opt.csvOut, events);

        if (opt.perFile)
        {
            printLearn ((wav.getFileName() + "  (" + juce::String (rate, 0) + " Hz, " + juce::String ((double) audio.size() / rate, 1) + " s)").toRawUTF8(), l, w, cpu);
            if (opt.wrong) printWrong (wr);
            if (opt.display) printDisplay (d);
            printCpu (cpu);
        }
        learnTotal.add (l);
        wrongTotal.add (wr);
        watchTotal.add (w);
        displayTotal.add (d);
        cpuTotal.seconds += cpu.seconds;
        cpuTotal.audio += cpu.audio;
        cpuTotal.worstBlockMs = std::max (cpuTotal.worstBlockMs, cpu.worstBlockMs);
        cpuTotal.blockMs.insert (cpuTotal.blockMs.end(), cpu.blockMs.begin(), cpu.blockMs.end());
    }

    if (opt.pairs.size() > 1 || ! opt.perFile)
    {
        std::printf ("\n==== TOTAL over %zu files ====\n", opt.pairs.size());
        printLearn ("", learnTotal, watchTotal, cpuTotal);
        if (opt.wrong) printWrong (wrongTotal);
        if (opt.display) printDisplay (displayTotal);
        printCpu (cpuTotal);
    }
    return 0;
}
