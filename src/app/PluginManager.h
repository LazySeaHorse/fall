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

    // Add a single .vst3 / .dll the user picked by hand.
    struct AddResult
    {
        juce::Array<juce::PluginDescription> instruments;
        juce::String problem; // set when nothing usable was found, in words a person can act on
    };
    AddResult addFile (const juce::File& file);

    // Plugins found during the last scan that this build can't load because of 32/64-bit mismatch.
    juce::StringArray getWrongArchitecture() const;

    // 32 or 64 for a Windows DLL, 0 if unknown / not a PE file.
    static int dllBits (const juce::File& file);
    static constexpr int processBits() { return (int) sizeof (void*) * 8; }

    using LoadCallback = std::function<void (std::unique_ptr<juce::AudioPluginInstance>, const juce::String& error)>;
    void load (const juce::PluginDescription& d, double sampleRate, int blockSize, LoadCallback cb);

    static std::unique_ptr<juce::XmlElement> describe (const juce::PluginDescription& d) { return d.createXml(); }

private:
    void run() override;
    static juce::FileSearchPath searchPathFor (juce::AudioPluginFormat& format);

    juce::AudioPluginFormatManager formats;
    juce::KnownPluginList knownList;
    juce::File deadMansPedal;
    std::function<void()> scanDone;
    mutable juce::CriticalSection nameLock;
    juce::String scanningName;
    juce::StringArray wrongArch;
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
