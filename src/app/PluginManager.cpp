#include "PluginManager.h"

namespace fall
{

PluginManager::PluginManager() : juce::Thread ("Plugin scan")
{
    juce::addDefaultFormatsToManager (formats);
    deadMansPedal = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                        .getChildFile ("Fall").getChildFile ("scan-in-progress.txt");
    deadMansPedal.getParentDirectory().createDirectory();
}

PluginManager::~PluginManager() { stopThread (10000); }

void PluginManager::restoreList (const juce::XmlElement* xml)
{
    if (xml != nullptr)
        knownList.recreateFromXml (*xml);
}

juce::Array<juce::PluginDescription> PluginManager::getInstruments() const
{
    juce::Array<juce::PluginDescription> result;
    for (const auto& d : knownList.getTypes())
        if (d.isInstrument)
            result.add (d);

    std::sort (result.begin(), result.end(), [] (const auto& a, const auto& b) {
        return a.name.compareNatural (b.name) < 0;
    });
    return result;
}

void PluginManager::scan (std::function<void()> onDone)
{
    if (isThreadRunning())
        return;
    scanDone = std::move (onDone);
    startThread();
}

juce::String PluginManager::getCurrentlyScanning() const
{
    const juce::ScopedLock sl (nameLock);
    return scanningName;
}

void PluginManager::run()
{
    for (auto* format : formats.getFormats())
    {
        // A plugin that crashed a previous scan is listed in the dead man's pedal file and skipped.
        juce::PluginDirectoryScanner scanner (knownList, *format, format->getDefaultLocationsToSearch(),
                                              true, deadMansPedal, false);
        juce::String name;
        while (! threadShouldExit())
        {
            {
                const juce::ScopedLock sl (nameLock);
                scanningName = scanner.getNextPluginFileThatWillBeScanned();
            }
            if (! scanner.scanNextFile (true, name))
                break;
        }
    }

    {
        const juce::ScopedLock sl (nameLock);
        scanningName = {};
    }

    juce::MessageManager::callAsync ([this] { if (scanDone) scanDone(); });
}

juce::Array<juce::PluginDescription> PluginManager::addFile (const juce::File& file)
{
    juce::Array<juce::PluginDescription> found;
    for (auto* format : formats.getFormats())
    {
        juce::OwnedArray<juce::PluginDescription> types;
        knownList.scanAndAddFile (file.getFullPathName(), true, types, *format);
        for (auto* t : types)
            if (t->isInstrument)
                found.add (*t);
    }
    return found;
}

void PluginManager::load (const juce::PluginDescription& d, double sampleRate, int blockSize, LoadCallback cb)
{
    formats.createPluginInstanceAsync (d, sampleRate, blockSize,
        [cb = std::move (cb)] (std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& error) {
            cb (std::move (instance), error);
        });
}

// ---------------------------------------------------------------------------------------------

PluginWindow::PluginWindow (juce::AudioPluginInstance& p, std::function<void()> onClose)
    : juce::DocumentWindow (p.getName(), juce::Colours::black, juce::DocumentWindow::closeButton),
      closed (std::move (onClose))
{
    setUsingNativeTitleBar (true);
    if (auto* editor = p.createEditorIfNeeded())
        setContentOwned (editor, true);
    else
        setContentOwned (new juce::GenericAudioProcessorEditor (p), true);

    if (auto* ed = dynamic_cast<juce::AudioProcessorEditor*> (getContentComponent()))
        setResizable (ed->isResizable(), false);
    centreWithSize (getWidth(), getHeight());
    setVisible (true);
}

} // namespace fall
