#include <juce_gui_extra/juce_gui_extra.h>

class FallApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    void initialise (const juce::String&) override {}
    void shutdown() override {}
};

START_JUCE_APPLICATION (FallApplication)
