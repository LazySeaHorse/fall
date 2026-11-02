#include "MainComponent.h"

namespace fall
{

class MainWindow : public juce::DocumentWindow
{
public:
    MainWindow (const LaunchOptions& opts)
        : juce::DocumentWindow ("Fall", theme::stageTop, juce::DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        auto* content = new MainComponent (opts);
        setContentOwned (content, true);
        setResizable (true, false);
        setResizeLimits (760, 480, 10000, 10000);
        centreWithSize (opts.width, opts.height);
        setVisible (true);
        content->grabKeyboardFocus();
    }

    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
};

class FallApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String& commandLine) override
    {
        window = std::make_unique<MainWindow> (LaunchOptions::parse (juce::StringArray::fromTokens (commandLine, true)));
    }

    void shutdown() override { window.reset(); }

private:
    std::unique_ptr<MainWindow> window;
};

} // namespace fall

START_JUCE_APPLICATION (fall::FallApplication)
