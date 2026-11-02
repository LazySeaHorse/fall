// FallTestSynth: minimal polyphonic sine instrument with one "gain" parameter.
// Each note-on starts a sine voice with an exponential decay while held and a
// short release after note-off, so a host test can assert on both.

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <cmath>
#include <cstdint>

namespace
{
    constexpr int numVoices = 16;
    constexpr double releaseSeconds = 0.03;
    constexpr double decayPerSecond = 0.5; // level multiplier per second while held

    struct Voice
    {
        bool active = false;
        bool released = false;
        int note = -1;
        double phase = 0.0;
        double phaseDelta = 0.0;
        double level = 0.0;
        float velocity = 0.0f;
        int64_t age = 0; // for voice stealing
    };
}

class FallTestSynthProcessor final : public juce::AudioProcessor
{
public:
    FallTestSynthProcessor()
        : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          parameters (*this, nullptr, "STATE",
                      { std::make_unique<juce::AudioParameterFloat> (
                          juce::ParameterID { "gain", 1 }, "gain",
                          juce::NormalisableRange<float> (0.0f, 1.0f), 0.5f) })
    {
        gainParam = parameters.getRawParameterValue ("gain");
    }

    const juce::String getName() const override { return "FallTestSynth"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    bool hasEditor() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override
    {
        const auto out = layouts.getMainOutputChannelSet();
        return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
    }

    void prepareToPlay (double newSampleRate, int) override
    {
        sampleRate = newSampleRate;
        previousGain = gainParam->load();

        for (auto& v : voices)
            v = {};
    }

    void releaseResources() override {}

    using AudioProcessor::processBlock;

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        juce::ScopedNoDenormals noDenormals;
        const auto numSamples = buffer.getNumSamples();
        const auto numChannels = buffer.getNumChannels();
        buffer.clear();

        const auto releaseMul = std::exp (std::log (0.001) / (releaseSeconds * sampleRate));
        const auto decayMul = std::exp (std::log (decayPerSecond) / sampleRate);

        int pos = 0;
        auto renderTo = [&] (int end)
        {
            for (auto& v : voices)
            {
                if (! v.active)
                    continue;

                for (int i = pos; i < end; ++i)
                {
                    const auto s = (float) (std::sin (v.phase) * v.level * (double) v.velocity * 0.25);
                    v.phase += v.phaseDelta;
                    if (v.phase > juce::MathConstants<double>::twoPi)
                        v.phase -= juce::MathConstants<double>::twoPi;
                    v.level *= v.released ? releaseMul : decayMul;

                    for (int ch = 0; ch < numChannels; ++ch)
                        buffer.getWritePointer (ch)[i] += s;
                }

                if (v.level < 1.0e-5)
                    v.active = false;
            }
            pos = end;
        };

        for (const auto metadata : midi)
        {
            renderTo (juce::jlimit (pos, numSamples, metadata.samplePosition));
            handleMessage (metadata.getMessage());
        }
        renderTo (numSamples);

        const auto gain = gainParam->load();
        buffer.applyGainRamp (0, numSamples, previousGain, gain);
        previousGain = gain;
    }

    void getStateInformation (juce::MemoryBlock& destData) override
    {
        if (auto xml = parameters.copyState().createXml())
            copyXmlToBinary (*xml, destData);
    }

    void setStateInformation (const void* data, int sizeInBytes) override
    {
        if (auto xml = getXmlFromBinary (data, sizeInBytes))
            if (xml->hasTagName (parameters.state.getType()))
                parameters.replaceState (juce::ValueTree::fromXml (*xml));
    }

private:
    void handleMessage (const juce::MidiMessage& m)
    {
        if (m.isNoteOn())
        {
            auto* target = &voices[0];

            for (auto& v : voices)
            {
                if (! v.active)
                {
                    target = &v;
                    break;
                }

                if (v.age < target->age)
                    target = &v;
            }

            target->active = true;
            target->released = false;
            target->note = m.getNoteNumber();
            target->phase = 0.0;
            target->phaseDelta = juce::MathConstants<double>::twoPi
                                 * juce::MidiMessage::getMidiNoteInHertz (m.getNoteNumber()) / sampleRate;
            target->level = 1.0;
            target->velocity = m.getFloatVelocity();
            target->age = ++noteCounter;
        }
        else if (m.isNoteOff())
        {
            for (auto& v : voices)
                if (v.active && ! v.released && v.note == m.getNoteNumber())
                    v.released = true;
        }
        else if (m.isAllNotesOff() || m.isAllSoundOff())
        {
            for (auto& v : voices)
                v.released = v.active;
        }
    }

    juce::AudioProcessorValueTreeState parameters;
    std::atomic<float>* gainParam = nullptr;
    float previousGain = 0.5f;
    double sampleRate = 44100.0;
    int64_t noteCounter = 0;
    Voice voices[numVoices];

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FallTestSynthProcessor)
};

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new FallTestSynthProcessor();
}
