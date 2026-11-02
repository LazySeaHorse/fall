#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include "core/Song.h"

#include <atomic>
#include <vector>

namespace fall
{

// Keeps the last ~90 s of what the microphone heard, plus a log of what Fall expected and what
// the detector reported, so "Save what Fall heard" can write a WAV + CSV for diagnosing detection
// on a real piano (fall_detect_eval can replay the WAV).
//
// push()/log*() run on the audio thread (lock-free, no allocation); save() on the message thread.
// Positions are sample indices of the microphone stream as fed to push(), which is the same stream
// the NoteDetector sees, so its samplePosition values can be logged directly.
class MicRecorder
{
public:
    static constexpr double kSeconds = 90.0;

    void prepare (double sampleRate)
    {
        rate = sampleRate;
        ring.assign ((size_t) juce::nextPowerOfTwo ((int) (sampleRate * kSeconds)), 0.0f);
        mask = (int64_t) ring.size() - 1;
        written.store (0);
        logCount.store (0);
    }

    void push (const float* mono, int n) noexcept
    {
        if (ring.empty())
            return;
        int64_t w = written.load (std::memory_order_relaxed);
        for (int i = 0; i < n; ++i)
            ring[(size_t) ((w + i) & mask)] = mono[i];
        written.store (w + n, std::memory_order_release);
    }

    int64_t position() const noexcept { return written.load (std::memory_order_relaxed); }

    void logExpected (const PitchSet& pitches) noexcept { add ({ position(), 'E', pitches, {}, 0.0f }); }

    void logHeard (int64_t onsetPos, const PitchSet& pitches, const PitchSet& accepted, float latencyMs) noexcept
    {
        add ({ onsetPos, 'H', pitches, accepted, latencyMs });
    }

    // Writes <folder>/fall-<date>.wav and .csv. Returns the WAV file, or an empty File + error.
    juce::File save (const juce::File& folder, const juce::String& headerNotes, juce::String& error) const
    {
        const int64_t end = written.load (std::memory_order_acquire);
        if (ring.empty() || end < (int64_t) (rate * 1.0))
        {
            error = "Nothing recorded yet. Is \"Listen to my piano\" on?";
            return {};
        }
        // keep one second clear of the writer, which may be overwriting the oldest samples right now
        const int64_t keep = std::min<int64_t> (end, (int64_t) ring.size() - (int64_t) rate);
        const int64_t start = end - keep;

        if (! folder.createDirectory())
        {
            error = "Couldn't create " + folder.getFullPathName();
            return {};
        }
        const auto stem = "fall-" + juce::Time::getCurrentTime().formatted ("%Y-%m-%d-%H%M%S");
        const auto wav = folder.getChildFile (stem + ".wav");
        const auto csv = folder.getChildFile (stem + ".csv");

        juce::AudioBuffer<float> buf (1, (int) keep);
        for (int64_t i = 0; i < keep; ++i)
            buf.setSample (0, (int) i, ring[(size_t) ((start + i) & mask)]);

        {
            std::unique_ptr<juce::OutputStream> os (wav.createOutputStream());
            if (os == nullptr)
            {
                error = "Couldn't write " + wav.getFullPathName();
                return {};
            }
            juce::WavAudioFormat fmt;
            auto writer = fmt.createWriterFor (os, juce::AudioFormatWriterOptions {}.withSampleRate (rate)
                                                                                    .withNumChannels (1)
                                                                                    .withBitsPerSample (24));
            if (writer == nullptr)
            {
                error = "Couldn't write " + wav.getFullPathName();
                return {};
            }
            writer->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
        }

        juce::String text;
        text << "# Fall microphone log. time = seconds from the start of " << wav.getFileName() << "\n";
        for (auto& line : juce::StringArray::fromLines (headerNotes))
            if (line.isNotEmpty())
                text << "# " << line << "\n";
        text << "time,kind,pitches,accepted,latency_ms\n";
        const int n = std::min (logCount.load (std::memory_order_acquire), kLogSize);
        const int first = logCount.load() - n;
        for (int i = first; i < first + n; ++i)
        {
            const auto& e = log[(size_t) (i % kLogSize)];
            if (e.pos < start)
                continue;
            text << juce::String ((double) (e.pos - start) / rate, 3) << ","
                 << (e.kind == 'E' ? "expected" : "heard") << ","
                 << names (e.a) << "," << names (e.b) << ","
                 << (e.kind == 'H' ? juce::String (e.latencyMs, 0) : juce::String()) << "\n";
        }
        csv.replaceWithText (text);
        return wav;
    }

private:
    struct Entry
    {
        int64_t  pos;
        char     kind;
        PitchSet a, b;
        float    latencyMs;
    };

    void add (const Entry& e) noexcept
    {
        const int i = logCount.load (std::memory_order_relaxed);
        log[(size_t) (i % kLogSize)] = e;
        logCount.store (i + 1, std::memory_order_release);
    }

    static juce::String names (const PitchSet& s)
    {
        juce::StringArray out;
        for (int p = 0; p < 128; ++p)
            if (s.test ((size_t) p))
                out.add (juce::String (p));
        return out.joinIntoString (" ");
    }

    static constexpr int kLogSize = 8192;
    double rate = 48000.0;
    std::vector<float> ring;
    int64_t mask = 0;
    std::atomic<int64_t> written { 0 };
    std::array<Entry, kLogSize> log {};
    std::atomic<int> logCount { 0 };
};

} // namespace fall
