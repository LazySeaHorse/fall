// Loads FallTestSynth.vst3 through JUCE's VST3 hosting code, plays a note and
// checks the audio and state round trip. Exits non-zero on any failure.
//
// Usage: fall_plugin_host_test <path/to/FallTestSynth.vst3>

#include <juce_audio_processors/juce_audio_processors.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 256;

    int failures = 0;

    void check (bool condition, const juce::String& message)
    {
        std::printf ("[%s] %s\n", condition ? " ok " : "FAIL", message.toRawUTF8());

        if (! condition)
            ++failures;
    }

    float peakOf (const juce::AudioBuffer<float>& b)
    {
        return b.getMagnitude (0, b.getNumSamples());
    }

    // Renders `numBlocks` blocks, with `midi` applied to the first one, and
    // returns the peak of each block.
    struct Renderer
    {
        explicit Renderer (juce::AudioPluginInstance& p)
            : plugin (p), buffer (juce::jmax (p.getTotalNumInputChannels(), p.getTotalNumOutputChannels()), blockSize) {}

        float renderBlock (juce::MidiBuffer& midi)
        {
            buffer.clear();
            plugin.processBlock (buffer, midi);
            midi.clear();
            return peakOf (buffer);
        }

        float renderBlock()
        {
            juce::MidiBuffer empty;
            return renderBlock (empty);
        }

        juce::AudioPluginInstance& plugin;
        juce::AudioBuffer<float> buffer;
    };

    std::unique_ptr<juce::AudioPluginInstance> createInstance (juce::AudioPluginFormat& format,
                                                               const juce::PluginDescription& desc)
    {
        juce::String error;
        auto instance = format.createInstanceFromDescription (desc, sampleRate, blockSize, error);

        if (instance == nullptr)
            std::printf ("createInstanceFromDescription failed: %s\n", error.toRawUTF8());

        return instance;
    }

    juce::AudioProcessorParameter* findGain (juce::AudioPluginInstance& plugin)
    {
        for (auto* p : plugin.getParameters())
            if (p->getName (64).equalsIgnoreCase ("gain"))
                return p;

        return nullptr;
    }

    void prepare (juce::AudioPluginInstance& plugin)
    {
        plugin.setPlayConfigDetails (plugin.getTotalNumInputChannels(), plugin.getTotalNumOutputChannels(),
                                     sampleRate, blockSize);
        plugin.prepareToPlay (sampleRate, blockSize);
    }

    int runTests (const juce::String& bundlePath)
    {
        juce::AudioPluginFormatManager formatManager;
        formatManager.addFormat (std::make_unique<juce::VST3PluginFormat>());

        juce::AudioPluginFormat* vst3 = nullptr;

        for (auto* f : formatManager.getFormats())
            if (f->getName() == "VST3")
                vst3 = f;

        check (vst3 != nullptr, "VST3 format available");

        if (vst3 == nullptr)
            return 1;

        check (juce::File (bundlePath).exists(), "bundle exists: " + bundlePath);

        // --- Scan ---------------------------------------------------------
        juce::OwnedArray<juce::PluginDescription> found;
        vst3->findAllTypesForFile (found, bundlePath);
        check (found.size() == 1, "scan finds exactly one plugin (found " + juce::String (found.size()) + ")");

        if (found.isEmpty())
            return 1;

        const auto desc = *found[0];
        check (desc.name == "FallTestSynth", "plugin name is FallTestSynth (got '" + desc.name + "')");
        check (desc.isInstrument, "plugin is flagged as an instrument");

        // --- Instantiate ----------------------------------------------------
        auto plugin = createInstance (*vst3, desc);
        check (plugin != nullptr, "instantiate plugin");

        if (plugin == nullptr)
            return 1;

        check (plugin->acceptsMidi(), "plugin accepts MIDI");
        check (plugin->getTotalNumOutputChannels() == 2, "plugin has 2 output channels (got "
                                                             + juce::String (plugin->getTotalNumOutputChannels()) + ")");
        prepare (*plugin);

        auto* gain = findGain (*plugin);
        check (gain != nullptr, "plugin exposes a 'gain' parameter");

        if (gain == nullptr)
            return 1;

        gain->setValue (0.5f);

        // --- Audio ----------------------------------------------------------
        Renderer r (*plugin);

        check (r.renderBlock() < 1.0e-6f, "silent before any note");

        const int noteOffBlock = (int) (0.25 * sampleRate / blockSize);
        const int totalBlocks = (int) (0.5 * sampleRate / blockSize);
        float peakWhileHeld = 0.0f, peakAtEnd = 0.0f;

        for (int block = 0; block < totalBlocks; ++block)
        {
            juce::MidiBuffer midi;

            if (block == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, 69, (juce::uint8) 100), 0);
            if (block == noteOffBlock)
                midi.addEvent (juce::MidiMessage::noteOff (1, 69), 0);

            const auto peak = r.renderBlock (midi);

            if (block < noteOffBlock)
                peakWhileHeld = juce::jmax (peakWhileHeld, peak);
            if (block >= totalBlocks - 8) // last ~40 ms, well after the release
                peakAtEnd = juce::jmax (peakAtEnd, peak);

            if (! std::isfinite (peak))
            {
                check (false, "output contains NaN/inf");
                return 1;
            }
        }

        std::printf ("peak while held = %f, peak at end = %f\n", (double) peakWhileHeld, (double) peakAtEnd);
        check (peakWhileHeld > 0.01f, "non-silent output while note held");
        check (peakWhileHeld <= 1.0f, "output not clipping");
        check (peakAtEnd < 1.0e-3f, "output decays to silence after note-off");

        // --- State round trip -----------------------------------------------
        gain->setValue (0.8f);
        r.renderBlock(); // let the host push the parameter change to the processor

        juce::MemoryBlock state;
        plugin->getStateInformation (state);
        check (state.getSize() > 0, "getStateInformation returns data");

        gain->setValue (0.1f);
        r.renderBlock();
        check (std::abs (gain->getValue() - 0.1f) < 0.01f, "gain changed before restore");

        plugin->setStateInformation (state.getData(), (int) state.getSize());
        check (std::abs (gain->getValue() - 0.8f) < 0.01f,
               "state restore brings gain back (got " + juce::String (gain->getValue()) + ")");

        // A second, fresh instance must pick the state up as well.
        auto second = createInstance (*vst3, desc);
        check (second != nullptr, "instantiate second instance");

        if (second != nullptr)
        {
            prepare (*second);
            second->setStateInformation (state.getData(), (int) state.getSize());

            if (auto* gain2 = findGain (*second))
                check (std::abs (gain2->getValue() - 0.8f) < 0.01f,
                       "second instance restores gain (got " + juce::String (gain2->getValue()) + ")");
            else
                check (false, "second instance exposes gain");

            second->releaseResources();
        }

        // --- Gain parameter actually affects the audio ----------------------
        gain->setValue (0.0f);
        r.renderBlock(); // settle the gain ramp
        {
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), 0);
            r.renderBlock (midi);
        }
        check (r.renderBlock() < 1.0e-6f, "gain = 0 gives silence");

        plugin->releaseResources();
        return failures;
    }
}

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf (stderr, "usage: %s <path/to/FallTestSynth.vst3>\n", argv[0]);
        return 2;
    }

    const juce::ScopedJuceInitialiser_GUI juceInit;

    const auto failed = runTests (juce::String (juce::CharPointer_UTF8 (argv[1])));
    std::printf (failed == 0 ? "ALL PLUGIN HOST CHECKS PASSED\n" : "%d PLUGIN HOST CHECK(S) FAILED\n", failed);

    return failed == 0 ? 0 : 1;
}
