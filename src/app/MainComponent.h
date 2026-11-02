#pragma once

#include "AudioEngine.h"
#include "Controls.h"
#include "LookAndFeel.h"
#include "Overlays.h"
#include "PluginManager.h"
#include "SettingsPanel.h"
#include "Stage.h"
#include "Timeline.h"

namespace fall
{

// Command-line options, mostly for automated screenshots (CI) and debugging.
struct LaunchOptions
{
    juce::File   midi;
    juce::String screenshot;   // write a PNG of the window here, then quit
    juce::File   framesDir;    // also dump every frame while waiting
    double       wait = 1.5;   // seconds of (simulated) time before the screenshot
    double       seek = -1e9;
    double       speed = -1.0;
    bool         play = false;
    bool         noAudio = false;
    bool         autoLearn = false; // pretend to be a perfect player in learn mode
    bool         openSettings = false;
    bool         calibrate = false;
    juce::String mode, hands;
    double       loopA = -1.0, loopB = -1.0;
    std::vector<Marker> markers;
    int          width = 1280, height = 800;

    bool scripted() const { return screenshot.isNotEmpty() || framesDir != juce::File(); }
    static LaunchOptions parse (const juce::StringArray& args);
};

class MainComponent : public juce::Component,
                      public juce::FileDragAndDropTarget,
                      private Animated
{
public:
    explicit MainComponent (const LaunchOptions& options);
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int, int) override;
    void fileDragEnter (const juce::StringArray&, int, int) override { dragOver = true; }
    void fileDragExit (const juce::StringArray&) override { dragOver = false; }

    void tick (float) override {}

private:
    void frame();
    void scriptedFrame (float dt);

    // song & transport
    bool loadFile (const juce::File& f);
    void openFileChooser();
    void togglePlay();
    void seekBy (double seconds);
    void setSpeed (double s);
    void setMode (Mode m);
    void setHands (int segment); // 0 left, 1 both, 2 right
    void applyHands();

    // loop & markers
    void setLoopRegion (double a, double b);
    void toggleLoop();
    void clearLoop();
    void pushLoop();
    void addMarker (double t);
    void editMarker (int index, juce::Rectangle<int> screenBounds);
    void pushMarkers();
    void saveSongState();
    void restoreSongState();

    // instrument & audio
    void refreshInstrumentList();
    void chooseInstrument (int index);
    void loadInstrument (const juce::PluginDescription& d, const juce::String& base64State);
    void showInstrumentWindow();
    void openAudioSettings();

    // calibration
    void startCalibration();
    void nextCalibrationKey (bool captured);
    void finishCalibration();

    void showSettings (bool show);
    void restoreSettings();
    void saveSettings();
    void heardOnScreenKey (int pitch);
    juce::String subtitle() const;

    LaunchOptions opts;
    const bool scripted; // fixed at launch (opts.screenshot is cleared on the way out)

    // Must run before any widget is constructed: several capture theme colours in their constructors.
    struct ThemeInit
    {
        explicit ThemeInit (const LaunchOptions&) { theme::init(); }
    } themeInit { opts };

    AppSettings settings;
    FallLookAndFeel lnf;
    AudioEngine engine;
    PluginManager plugins;

    std::shared_ptr<const Song> song;
    juce::File songFile;
    std::vector<Marker> markers;
    LoopSettings loop;
    double lastLoopA = -1, lastLoopB = -1;

    Stage stage;
    Timeline timeline;
    Toast toast;
    SettingsPanel settingsPanel;
    CalibrationOverlay calibration;

    IconButton openButton { icons::folder, "Open a MIDI file (Ctrl+O)" };
    IconButton restartButton { icons::restart, "Back to the start (Home)" };
    IconButton loopButton { icons::loop, "Loop (L) - shift-drag the timeline to choose" };
    IconButton micButton { icons::mic, "Listen to my piano" };
    IconButton instrumentButton { icons::piano, "Instrument window" };
    IconButton settingsButton { icons::sliders, "Settings (,)" };
    PlayButton playButton;
    Segmented modeSeg { { "Free play", "Learn" } };
    Segmented handsSeg { { "Left", "Both", "Right" } };
    SpeedControl speedControl;
    WindowButton minimiseButton { WindowButton::Kind::Minimise };
    WindowButton maximiseButton { WindowButton::Kind::Maximise };
    WindowButton closeButton { WindowButton::Kind::Close };
    juce::ComponentDragger windowDragger;
    bool draggingWindow = false;
    juce::ResizableWindow* window() const { return dynamic_cast<juce::ResizableWindow*> (getTopLevelComponent()); }
    juce::TooltipWindow tooltips { this, 700 };

    juce::VBlankAttachment vblank;
    double lastFrameMs = 0.0, displayTime = 0.0, scriptedTime = 0.0;
    int frameCount = 0, lastLoopPass = 0;
    double lastOctaveHintMs = -1e9;
    float waitingFor = 0.0f;

    Spring settingsSlide { 0.0f, 340.0f, 34.0f };
    bool settingsOpen = false;
    Spring dropHover { 0.0f, 300.0f, 24.0f };
    bool dragOver = false;

    // preferences
    int    handsSegment = 1;
    bool   playMyPart = true, showNames = true, micOn = true, rawMic = true;
    double volume = 1.0, sensitivity = 0.6, earlyWindow = 0.25, jumpLeadIn = 1.0, lookahead = 3.2;
    int    splitPitch = 60;

    // instrument
    juce::Array<juce::PluginDescription> instruments;
    int instrumentIndex = 0; // 0 = built-in
    juce::String instrumentName { "Built-in piano" };
    std::unique_ptr<PluginWindow> pluginWindow;

    // calibration
    Calibration calib;
    std::vector<int> calibKeys;
    int calibIndex = -1;
    float calibCooldown = 0.0f;

    // marker key learning
    int listeningMarker = -1;
    juce::Component::SafePointer<MarkerEditor> markerEditor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace fall
