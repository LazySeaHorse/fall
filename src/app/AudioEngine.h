#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "FallbackSynth.h"
#include "MicRecorder.h"
#include "core/Player.h"
#include "detect/NoteDetector.h"

#include <array>
#include <atomic>

namespace fall
{

// What the UI needs each frame. Captured by the audio thread after every block.
struct EngineSnapshot
{
    double       time = 0.0;
    double       speed = 1.0;
    bool         playing = false;
    bool         waiting = false;
    Mode         mode = Mode::Freeplay;
    uint8_t      practiceHands = HandBoth;
    PitchSet     expected, satisfied;
    double       nextGate = -1.0; // song time the player will stop at next (learn mode)
    LoopSettings loop;
    int          loopPass = 0;
    int          groupsCompleted = 0;
    double       wallMs = 0.0;    // juce::Time::getMillisecondCounterHiRes() at capture
};

// Something the microphone heard.
struct HeardEvent
{
    PitchSet pitches;  // best guess of what was played (display)
    PitchSet accepted; // notes that counted for learn mode / jump keys
    int      marker = -1;
    double   wallMs = 0.0;
    float    latencyMs = 0.0f;
};

class AudioEngine : private juce::AudioIODeviceCallback
{
public:
    AudioEngine();
    ~AudioEngine() override;

    juce::AudioDeviceManager& getDeviceManager() noexcept { return deviceManager; }
    juce::String openDevices (const juce::XmlElement* savedState);
    bool isDeviceRunning() const noexcept { return deviceRunning.load(); }

    // Run the clock without an audio device (headless runs, or when the device failed).
    void tickWithoutDevice (double seconds);

    // ---- Player commands (message thread) ----
    template <typename Fn>
    void withPlayer (Fn&& fn)
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        fn (player);
        refreshSnapshotLocked();
    }

    EngineSnapshot snapshot() const;
    std::shared_ptr<const Song> getSong() const { return song; }
    void setSong (std::shared_ptr<const Song> s);

    // ---- Instrument ----
    // nullptr = built-in synth. Takes ownership; the old instance is returned for the caller to destroy.
    std::unique_ptr<juce::AudioPluginInstance> setInstrument (std::unique_ptr<juce::AudioPluginInstance> plugin);
    juce::AudioPluginInstance* getInstrument() const noexcept { return instrument.get(); }
    void setMasterGain (float g) noexcept { masterGain.store (g); }

    // Notes clicked on the on-screen keyboard.
    void previewNote (int pitch, bool down);

    // ---- Microphone / detector ----
    void  setMicEnabled (bool shouldListen) noexcept { micEnabled.store (shouldListen); }
    bool  isMicEnabled() const noexcept { return micEnabled.load(); }
    void  setSensitivity (float s);
    void  setCalibration (const Calibration& c);
    void  beginCapture (int pitch);
    bool  captureFinished();
    void  storeCapture (Calibration& into);
    float getInputLevelDb() const noexcept { return inputDb.load(); }
    bool  hasInput() const noexcept { return numInputs.load() > 0; }

    // The piano's tuning in cents from A440 (see NoteDetector::getTuningCents).
    float getTuningCents (bool& confident) const noexcept { return detector.getTuningCents (&confident); }

    // Windows: ask for the raw microphone stream, skipping Windows' voice processing (noise
    // suppression, auto gain). Reopens the audio device when the setting changes.
    void setRawMicrophone (bool raw);

    // Writes the last ~90 s of microphone audio + what was expected/heard (see MicRecorder).
    juce::File saveRecording (const juce::File& folder, const juce::String& notes, juce::String& error) const
    {
        return recorder.save (folder, notes, error);
    }

    // Things the mic heard since the last call (message thread).
    int popHeard (HeardEvent* out, int maxEvents);

    double getSampleRate() const noexcept { return sampleRate; }

private:
    void audioDeviceIOCallbackWithContext (const float* const* inputs, int numIn, float* const* outputs, int numOut,
                                           int numSamples, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;

    void processBlock (const float* const* inputs, int numIn, float* const* outputs, int numOut, int numSamples);
    void refreshSnapshotLocked();
    void pushHeard (const HeardEvent& e);

    juce::AudioDeviceManager deviceManager;

    mutable juce::SpinLock lock; // guards player, detector, snapshot
    Player player;
    NoteDetector detector;
    EngineSnapshot snap;
    std::shared_ptr<const Song> song;

    juce::CriticalSection instrumentLock;
    std::unique_ptr<juce::AudioPluginInstance> instrument;
    FallbackSynth fallback;
    MicRecorder recorder;
    juce::MidiMessageCollector previewCollector;

    juce::AudioBuffer<float> renderBuffer;
    juce::HeapBlock<float> mono;
    juce::MidiBuffer midi;
    std::array<DetectedOnset, 16> onsets;

    static constexpr int kHeardCapacity = 128;
    juce::AbstractFifo heardFifo { kHeardCapacity };
    std::array<HeardEvent, kHeardCapacity> heard;

    double sampleRate = 48000.0;
    int    blockSize = 512;
    int64_t streamPos = 0;
    // The detector treats every setExpected() call after a hit as a new chord, so only call it when
    // the chord the player waits for actually changes.
    int    expectedGroups = -1;
    double expectedGate = -1e9;
    std::atomic<bool>  deviceRunning { false };
    std::atomic<bool>  micEnabled { true };
    std::atomic<int>   numInputs { 0 };
    std::atomic<float> inputDb { -100.0f };
    std::atomic<float> masterGain { 1.0f };
};

} // namespace fall
