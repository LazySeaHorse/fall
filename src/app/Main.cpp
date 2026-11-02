#include "MainComponent.h"

#if JUCE_WINDOWS
 #include <dwmapi.h>
 #pragma comment (lib, "dwmapi.lib")
#endif

namespace fall
{

class MainWindow : public juce::DocumentWindow
{
public:
    MainWindow (const LaunchOptions& opts)
        : juce::DocumentWindow ("Fall", theme::stageTop, juce::DocumentWindow::allButtons)
    {
        // Fall draws its own title bar (the top bar), so the window has no caption of its own.
        setUsingNativeTitleBar (false);
        setTitleBarHeight (0);
        auto* content = new MainComponent (opts);
        setContentOwned (content, true);
        setResizable (true, false);
        setResizeLimits (860, 520, 10000, 10000);
        centreWithSize (opts.width, opts.height);
        setVisible (true);
        roundCorners();
        content->grabKeyboardFocus();
    }

    // Thin, near-invisible resize border.
    juce::BorderSize<int> getBorderThickness() const override
    {
        return isFullScreen() ? juce::BorderSize<int>() : juce::BorderSize<int> (1);
    }

    void paint (juce::Graphics& g) override { g.fillAll (juce::Colour (0xff2a2a31)); }

private:
    void roundCorners()
    {
       #if JUCE_WINDOWS
        // Windows 11 rounds native windows; ask DWM to do the same for ours (ignored on Windows 10).
        if (auto* peer = getPeer())
        {
            const DWORD preference = 2; // DWMWCP_ROUND
            DwmSetWindowAttribute ((HWND) peer->getNativeHandle(), 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */,
                                   &preference, sizeof (preference));
        }
       #endif
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
