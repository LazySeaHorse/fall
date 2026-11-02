#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>

namespace fall
{

// Finds installed VST3 instruments and creates instances of them.
class PluginManager : private juce::Thread
{
public:
    PluginManager();
    ~PluginManager() override;

    void restoreList (const juce::XmlElement* xml);
    std::unique_ptr<juce::XmlElement> saveList() const { return knownList.createXml(); }

    // Instruments only, sorted by name.
    juce::Array<juce::PluginDescription> getInstruments() const;

    // Scan the standard VST3 folders in the background. `onDone` runs on the message thread.
    void scan (std::function<void()> onDone);
    bool isScanning() const { return isThreadRunning(); }
    juce::String getCurrentlyScanning() const;

    // Add a single .vst3 the user picked by hand. Returns the instruments found in it.
    juce::Array<juce::PluginDescription> addFile (const juce::File& file);

    using LoadCallback = std::function<void (std::unique_ptr<juce::AudioPluginInstance>, const juce::String& error)>;
    void load (const juce::PluginDescription& d, double sampleRate, int blockSize, LoadCallback cb);

    static std::unique_ptr<juce::XmlElement> describe (const juce::PluginDescription& d) { return d.createXml(); }

private:
    void run() override;

    juce::AudioPluginFormatManager formats;
    juce::KnownPluginList knownList;
    juce::File deadMansPedal;
    std::function<void()> scanDone;
    mutable juce::CriticalSection nameLock;
    juce::String scanningName;
};

// A floating window holding a plugin's own editor (e.g. Pianoteq's UI).
class PluginWindow : public juce::DocumentWindow
{
public:
    PluginWindow (juce::AudioPluginInstance& p, std::function<void()> onClose);
    void closeButtonPressed() override { if (closed) closed(); }

private:
    std::function<void()> closed;
};

} // namespace fall
