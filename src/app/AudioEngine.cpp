#include "AudioEngine.h"

#if JUCE_WINDOWS
namespace juce { extern bool fallWasapiRawInput; } // cmake/PatchJuceWasapiRaw.cmake
#endif


namespace fall
{

AudioEngine::AudioEngine()
{
    mono.allocate (8192, true);
    renderBuffer.setSize (2, 8192);
    midi.ensureSize (4096);
    previewCollector.reset (sampleRate);
}

AudioEngine::~AudioEngine()
{
    deviceManager.removeAudioCallback (this);
    deviceManager.closeAudioDevice();
}

void AudioEngine::setRawMicrophone (bool raw)
{
   #if JUCE_WINDOWS
    if (juce::fallWasapiRawInput == raw)
        return;
    juce::fallWasapiRawInput = raw;
    if (deviceManager.getCurrentAudioDevice() != nullptr)
    {
        deviceManager.closeAudioDevice();
        deviceManager.restartLastAudioDevice();
    }
   #else
    juce::ignoreUnused (raw);
   #endif
}

juce::String AudioEngine::openDevices (const juce::XmlElement* savedState)
{
    // Prefer the low-latency shared WASAPI mode on Windows; it works with any headphones + laptop mic.
    juce::AudioDeviceManager::AudioDeviceSetup preferred;
    preferred.bufferSize = 256;

    const auto error = deviceManager.initialise (2, 2, savedState, true, {}, savedState == nullptr ? &preferred : nullptr);
    deviceManager.addAudioCallback (this);
    return error;
}

void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    const juce::SpinLock::ScopedLockType sl (lock);
    sampleRate = device->getCurrentSampleRate();
    blockSize  = device->getCurrentBufferSizeSamples();
    numInputs.store (device->getActiveInputChannels().countNumberOfSetBits());

    const int capacity = std::max (blockSize, 64) * 2;
    mono.allocate ((size_t) capacity, true);
    detector.prepare (sampleRate, capacity);
    recorder.prepare (sampleRate);
    expectedGroups = -1; // re-send the expected chord to the fresh detector
    previewCollector.reset (sampleRate);
    fallback.prepare (sampleRate);

    {
        const juce::ScopedLock il (instrumentLock);
        const int channels = instrument ? std::max ({ 2, instrument->getTotalNumOutputChannels(), instrument->getTotalNumInputChannels() }) : 2;
        renderBuffer.setSize (channels, capacity, false, true, false);
        if (instrument)
            instrument->prepareToPlay (sampleRate, capacity);
    }

    deviceRunning.store (true);
}

void AudioEngine::audioDeviceStopped()
{
    deviceRunning.store (false);
    const juce::ScopedLock il (instrumentLock);
    if (instrument)
        instrument->releaseResources();
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* inputs, int numIn, float* const* outputs,
                                                    int numOut, int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    // Some drivers hand us blocks bigger than they announced: process in chunks we've allocated for.
    const int chunk = renderBuffer.getNumSamples();
    for (int done = 0; done < numSamples; done += chunk)
    {
        const int n = std::min (chunk, numSamples - done);
        const float* in[8] {};
        float* out[8] {};
        const int ni = std::min (numIn, 8), no = std::min (numOut, 8);
        for (int c = 0; c < ni; ++c) in[c] = inputs[c] != nullptr ? inputs[c] + done : nullptr;
        for (int c = 0; c < no; ++c) out[c] = outputs[c] != nullptr ? outputs[c] + done : nullptr;
        processBlock (in, ni, out, no, n);
    }
}

void AudioEngine::processBlock (const float* const* inputs, int numIn, float* const* outputs, int numOut, int numSamples)
{
    // 1. Microphone -> mono.
    int activeIn = 0;
    juce::FloatVectorOperations::clear (mono.get(), numSamples);
    for (int c = 0; c < numIn; ++c)
        if (inputs[c] != nullptr) { juce::FloatVectorOperations::add (mono.get(), inputs[c], numSamples); ++activeIn; }
    if (activeIn > 1)
        juce::FloatVectorOperations::multiply (mono.get(), 1.0f / (float) activeIn, numSamples);

    midi.clear();

    {
        const juce::SpinLock::ScopedLockType sl (lock);

        // 2. Detector -> player (learn mode / jump keys).
        if (activeIn > 0 && micEnabled.load())
        {
            recorder.push (mono.get(), numSamples);
            const int found = detector.process (mono.get(), numSamples, onsets.data(), (int) onsets.size());
            for (int i = 0; i < found; ++i)
            {
                const auto& o = onsets[(size_t) i];
                HeardEvent e;
                e.pitches  = o.pitches | o.expectedHits | o.watchHits;
                e.accepted = o.expectedHits | o.watchHits;
                if (e.accepted.any())
                    e.marker = player.notePlayed (e.accepted);
                e.wallMs    = juce::Time::getMillisecondCounterHiRes();
                e.latencyMs = (float) (1000.0 * (double) (o.reportedAt - o.samplePosition) / sampleRate);
                pushHeard (e);
                recorder.logHeard (o.samplePosition, e.pitches, e.accepted, e.latencyMs);
            }
            if (player.getGroupsCompleted() != expectedGroups || player.nextGroupTime() != expectedGate)
            {
                expectedGroups = player.getGroupsCompleted();
                expectedGate = player.nextGroupTime();
                detector.setExpected (player.expectedPitches());
                recorder.logExpected (player.expectedPitches());
            }
            detector.setWatchKeys (player.jumpKeys());
            inputDb.store (detector.getInputLevelDb());
        }

        // 3. Song -> MIDI.
        player.render (midi, numSamples, sampleRate);
        refreshSnapshotLocked();
    }
    streamPos += numSamples;

    previewCollector.removeNextBlockOfMessages (midi, numSamples);

    // 4. MIDI -> instrument -> output.
    const juce::ScopedTryLock il (instrumentLock);
    if (! il.isLocked())
    {
        for (int c = 0; c < numOut; ++c)
            if (outputs[c] != nullptr) juce::FloatVectorOperations::clear (outputs[c], numSamples);
        return;
    }

    if (instrument)
    {
        juce::AudioBuffer<float> view (renderBuffer.getArrayOfWritePointers(), renderBuffer.getNumChannels(), numSamples);
        view.clear();
        instrument->processBlock (view, midi);
    }
    else
    {
        juce::AudioBuffer<float> view (renderBuffer.getArrayOfWritePointers(), 2, numSamples);
        fallback.render (view, midi, numSamples);
    }

    const float gain = masterGain.load();
    for (int c = 0; c < numOut; ++c)
    {
        if (outputs[c] == nullptr) continue;
        const int src = std::min (c, renderBuffer.getNumChannels() - 1);
        juce::FloatVectorOperations::copyWithMultiply (outputs[c], renderBuffer.getReadPointer (src), gain, numSamples);
    }
}

void AudioEngine::tickWithoutDevice (double seconds)
{
    if (deviceRunning.load() || seconds <= 0.0)
        return;

    const juce::SpinLock::ScopedLockType sl (lock);
    juce::MidiBuffer discard;
    const double rate = 48000.0;
    player.render (discard, std::max (1, (int) std::lround (seconds * rate)), rate);
    refreshSnapshotLocked();
}

void AudioEngine::refreshSnapshotLocked()
{
    snap.time            = player.getTime();
    snap.speed           = player.getSpeed();
    snap.playing         = player.isPlaying();
    snap.waiting         = player.isWaiting();
    snap.mode            = player.getMode();
    snap.practiceHands   = player.getPracticeHands();
    snap.expected        = player.expectedPitches();
    snap.satisfied       = player.satisfiedPitches();
    snap.nextGate        = player.nextGroupTime();
    snap.loop            = player.getLoop();
    snap.loopPass        = player.getLoopPass();
    snap.groupsCompleted = player.getGroupsCompleted();
    snap.wallMs          = juce::Time::getMillisecondCounterHiRes();
}

EngineSnapshot AudioEngine::snapshot() const
{
    const juce::SpinLock::ScopedLockType sl (lock);
    return snap;
}

void AudioEngine::setSong (std::shared_ptr<const Song> s)
{
    song = s;
    withPlayer ([&] (Player& p) { p.setSong (s); });
}

std::unique_ptr<juce::AudioPluginInstance> AudioEngine::setInstrument (std::unique_ptr<juce::AudioPluginInstance> plugin)
{
    if (plugin != nullptr)
    {
        plugin->enableAllBuses();
        plugin->prepareToPlay (sampleRate, std::max (renderBuffer.getNumSamples(), 64));
    }

    const int channels = plugin ? std::max ({ 2, plugin->getTotalNumOutputChannels(), plugin->getTotalNumInputChannels() }) : 2;

    {
        const juce::ScopedLock il (instrumentLock);
        renderBuffer.setSize (channels, renderBuffer.getNumSamples(), false, true, false);
        std::swap (instrument, plugin);
        fallback.allNotesOff();
    }

    // Make the song resend held notes / pedal state to the new instrument.
    withPlayer ([] (Player& p) { p.seek (p.getTime()); });

    if (plugin != nullptr)
        plugin->releaseResources();
    return plugin;
}

void AudioEngine::previewNote (int pitch, bool down)
{
    auto m = down ? juce::MidiMessage::noteOn (1, pitch, 0.7f) : juce::MidiMessage::noteOff (1, pitch);
    m.setTimeStamp (juce::Time::getMillisecondCounterHiRes() * 0.001);
    previewCollector.addMessageToQueue (m);
}

void AudioEngine::setSensitivity (float s)
{
    const juce::SpinLock::ScopedLockType sl (lock);
    detector.setSensitivity (s);
}

void AudioEngine::setCalibration (const Calibration& c)
{
    const juce::SpinLock::ScopedLockType sl (lock);
    detector.setCalibration (c);
}

void AudioEngine::beginCapture (int pitch)
{
    const juce::SpinLock::ScopedLockType sl (lock);
    detector.beginCapture (pitch);
}

bool AudioEngine::captureFinished()
{
    const juce::SpinLock::ScopedLockType sl (lock);
    return detector.captureFinished();
}

void AudioEngine::storeCapture (Calibration& into)
{
    const juce::SpinLock::ScopedLockType sl (lock);
    detector.storeCapture (into);
}

void AudioEngine::pushHeard (const HeardEvent& e)
{
    const auto scope = heardFifo.write (1);
    if (scope.blockSize1 > 0) heard[(size_t) scope.startIndex1] = e;
}

int AudioEngine::popHeard (HeardEvent* out, int maxEvents)
{
    const auto scope = heardFifo.read (std::min (maxEvents, heardFifo.getNumReady()));
    int n = 0;
    for (int i = 0; i < scope.blockSize1; ++i) out[n++] = heard[(size_t) (scope.startIndex1 + i)];
    for (int i = 0; i < scope.blockSize2; ++i) out[n++] = heard[(size_t) (scope.startIndex2 + i)];
    return n;
}

} // namespace fall
