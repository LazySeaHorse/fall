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

juce::FileSearchPath PluginManager::searchPathFor (juce::AudioPluginFormat& format)
{
    auto path = format.getDefaultLocationsToSearch();

   #if JUCE_WINDOWS
    if (format.getName() == "VST")
    {
        // Older instruments install in all sorts of places; these are the common ones, in both
        // Program Files folders. Ones with the wrong bitness are skipped and reported separately.
        const juce::File roots[] = { juce::File::getSpecialLocation (juce::File::globalApplicationsDirectory),
                                     juce::File::getSpecialLocation (juce::File::globalApplicationsDirectoryX86) };
        const char* subs[] = { "Steinberg\\VstPlugins", "Steinberg\\VSTPlugins", "VstPlugins", "VSTPlugins",
                               "Common Files\\VST2", "Common Files\\Steinberg\\VST2", "Vstplugins" };
        for (const auto& root : roots)
            for (auto* sub : subs)
                if (auto dir = root.getChildFile (sub); dir.isDirectory())
                    path.addIfNotAlreadyThere (dir);
    }
   #endif
    path.removeNonExistentPaths();
    return path;
}

int PluginManager::dllBits (const juce::File& file)
{
    juce::FileInputStream in (file);
    if (! in.openedOk() || in.readShort() != 0x5a4d) // "MZ"
        return 0;
    in.setPosition (0x3c);
    const auto peOffset = (juce::int64) (juce::uint32) in.readInt();
    in.setPosition (peOffset);
    if (in.readInt() != 0x00004550) // "PE\0\0"
        return 0;
    switch ((juce::uint16) in.readShort())
    {
        case 0x014c: return 32; // x86
        case 0x8664: return 64; // x64
        case 0xaa64: return 64; // arm64
        default:     return 0;
    }
}

juce::StringArray PluginManager::getWrongArchitecture() const
{
    const juce::ScopedLock sl (nameLock);
    return wrongArch;
}

void PluginManager::run()
{
    juce::StringArray mismatched;

    for (auto* format : formats.getFormats())
    {
        const auto path = searchPathFor (*format);

        if (format->getName() == "VST")
            for (int i = 0; i < path.getNumPaths(); ++i)
                for (const auto& f : path[i].findChildFiles (juce::File::findFiles, true, "*.dll"))
                    if (const int bits = dllBits (f); bits != 0 && bits != processBits())
                        mismatched.addIfNotAlreadyThere (f.getFileNameWithoutExtension() + " (" + juce::String (bits) + "-bit)");

        // A plugin that crashed a previous scan is listed in the dead man's pedal file and skipped.
        juce::PluginDirectoryScanner scanner (knownList, *format, path, true, deadMansPedal, false);
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
        wrongArch = mismatched;
    }

    juce::MessageManager::callAsync ([this] { if (scanDone) scanDone(); });
}

PluginManager::AddResult PluginManager::addFile (const juce::File& file)
{
    AddResult r;

    if (file.hasFileExtension (".dll"))
    {
        const int bits = dllBits (file);
        if (bits != 0 && bits != processBits())
        {
            r.problem = file.getFileNameWithoutExtension() + " is a " + juce::String (bits) + "-bit plugin. Use the "
                        + juce::String (bits) + "-bit version of Fall (Fall-windows-" + (bits == 32 ? "x86" : "x64") + ") to load it.";
            return r;
        }
    }

    bool anyFound = false;
    for (auto* format : formats.getFormats())
    {
        if (! format->fileMightContainThisPluginType (file.getFullPathName()))
            continue;
        juce::OwnedArray<juce::PluginDescription> types;
        knownList.scanAndAddFile (file.getFullPathName(), true, types, *format);
        anyFound |= ! types.isEmpty();
        for (auto* t : types)
            if (t->isInstrument)
                r.instruments.add (*t);
    }

    if (r.instruments.isEmpty())
        r.problem = anyFound ? file.getFileName() + " is an effect, not an instrument"
                             : "Couldn't load " + file.getFileName() + " as a plugin";
    return r;
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
