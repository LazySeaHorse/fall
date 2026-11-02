#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace fall
{

// A small, soft electric-piano-ish synth so the app makes sound before a VST is chosen.
class FallbackSynth
{
public:
    FallbackSynth()
    {
        for (int i = 0; i < 24; ++i)
            synth.addVoice (new Voice());
        synth.addSound (new Sound());
    }

    void prepare (double sampleRate) { synth.setCurrentPlaybackSampleRate (sampleRate); }

    void render (juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi, int numSamples)
    {
        buffer.clear (0, numSamples);
        synth.renderNextBlock (buffer, midi, 0, numSamples);
    }

    void allNotesOff() { synth.allNotesOff (0, false); }

private:
    struct Sound : juce::SynthesiserSound
    {
        bool appliesToNote (int) override { return true; }
        bool appliesToChannel (int) override { return true; }
    };

    struct Voice : juce::SynthesiserVoice
    {
        bool canPlaySound (juce::SynthesiserSound* s) override { return dynamic_cast<Sound*> (s) != nullptr; }

        void startNote (int note, float velocity, juce::SynthesiserSound*, int) override
        {
            const double freq = juce::MidiMessage::getMidiNoteInHertz (note);
            phaseInc = juce::MathConstants<double>::twoPi * freq / getSampleRate();
            phase = 0.0;
            level = 0.18f * velocity * velocity + 0.04f;
            // Lower notes ring longer, like a real piano.
            const double decaySeconds = juce::jmap ((double) note, 21.0, 108.0, 9.0, 1.2);
            decay = (float) std::exp (-1.0 / (decaySeconds * getSampleRate()));
            env = 0.0f;
            attackPos = 0.0f;
            attack = 1.0f / (float) (0.004 * getSampleRate());
            releasing = false;
            brightness = juce::jlimit (0.15f, 0.6f, velocity * 0.6f);
        }

        void stopNote (float, bool allowTailOff) override
        {
            if (! allowTailOff) { clearCurrentNote(); env = 0; return; }
            releasing = true;
            release = (float) std::exp (-1.0 / (0.18 * getSampleRate()));
        }

        void pitchWheelMoved (int) override {}
        void controllerMoved (int, int) override {}

        void renderNextBlock (juce::AudioBuffer<float>& out, int start, int num) override
        {
            if (! isVoiceActive()) return;

            for (int i = start; i < start + num; ++i)
            {
                if (attackPos < 1.0f) { attackPos = std::min (1.0f, attackPos + attack); env = attackPos; }
                else env *= releasing ? release : decay;

                const double p = phase;
                const float s = (float) (std::sin (p) + brightness * 0.5 * std::sin (2 * p)
                                         + brightness * 0.25 * std::sin (3 * p) + 0.08 * std::sin (4 * p));
                const float v = s * env * level;
                for (int ch = 0; ch < out.getNumChannels(); ++ch)
                    out.addSample (ch, i, v);
                phase += phaseInc;
                if (phase > juce::MathConstants<double>::twoPi * 64) phase -= juce::MathConstants<double>::twoPi * 64;

                if (env < 0.0005f && attackPos >= 1.0f)
                {
                    clearCurrentNote();
                    attackPos = 0.0f;
                    break;
                }
            }
        }

        double phase = 0, phaseInc = 0;
        float level = 0, env = 0, decay = 0.9999f, release = 0.999f, attack = 0.01f, attackPos = 0, brightness = 0.3f;
        bool releasing = false;
    };

    juce::Synthesiser synth;
};

} // namespace fall
