#include "MainComponent.h"

#include <juce_audio_utils/juce_audio_utils.h>

namespace fall
{

namespace
{
    constexpr int kTopBarH = 56;
    constexpr int kTimelineH = 34;
    constexpr int kSettingsW = 400;

    PitchSet single (int p) { PitchSet s; s.set ((size_t) p); return s; }
    const juce::String kDot = juce::String::fromUTF8 ("  \xc2\xb7  ");
}

// ---------------------------------------------------------------------------------------------
LaunchOptions LaunchOptions::parse (const juce::StringArray& rawArgs)
{
    // Windows hands us the raw command line, quotes included ("C:\path with spaces\x.mid").
    juce::StringArray args;
    for (const auto& a : rawArgs)
        args.add (a.trim().unquoted());

    LaunchOptions o;
    auto next = [&] (int& i) { return i + 1 < args.size() ? args[++i] : juce::String(); };

    for (int i = 0; i < args.size(); ++i)
    {
        const auto a = args[i];
        if      (a == "--screenshot") o.screenshot = next (i);
        else if (a == "--frames-dir") o.framesDir = juce::File::getCurrentWorkingDirectory().getChildFile (next (i));
        else if (a == "--midi")       o.midi = juce::File::getCurrentWorkingDirectory().getChildFile (next (i));
        else if (a == "--seek")       o.seek = next (i).getDoubleValue();
        else if (a == "--wait")       o.wait = next (i).getDoubleValue();
        else if (a == "--speed")      o.speed = next (i).getDoubleValue();
        else if (a == "--play")       o.play = true;
        else if (a == "--no-audio")   o.noAudio = true;
        else if (a == "--auto-learn") o.autoLearn = true;
        else if (a == "--settings")   o.openSettings = true;
        else if (a == "--calibrate")  o.calibrate = true;
        else if (a == "--mode")       o.mode = next (i);
        else if (a == "--hands")      o.hands = next (i);
        else if (a == "--loop")
        {
            const auto v = next (i);
            o.loopA = v.upToFirstOccurrenceOf (":", false, false).getDoubleValue();
            o.loopB = v.fromFirstOccurrenceOf (":", false, false).getDoubleValue();
        }
        else if (a == "--marker") // time:pitch[:name]
        {
            juce::StringArray parts;
            parts.addTokens (next (i), ":", "");
            Marker m;
            m.time = parts[0].getDoubleValue();
            m.triggerPitch = parts.size() > 1 ? parts[1].getIntValue() : -1;
            m.name = parts.size() > 2 ? parts[2] : juce::String ("Marker");
            o.markers.push_back (m);
        }
        else if (a == "--size")
        {
            const auto v = next (i);
            o.width = v.upToFirstOccurrenceOf ("x", false, true).getIntValue();
            o.height = v.fromFirstOccurrenceOf ("x", false, true).getIntValue();
        }
        else if (a.endsWithIgnoreCase (".mid") || a.endsWithIgnoreCase (".midi"))
            o.midi = juce::File::getCurrentWorkingDirectory().getChildFile (a);
    }
    if (o.scripted()) o.noAudio = true;
    return o;
}

// ---------------------------------------------------------------------------------------------
MainComponent::MainComponent (const LaunchOptions& options) : opts (options), scripted (options.scripted())
{
    lnf.refresh();
    juce::LookAndFeel::setDefaultLookAndFeel (&lnf);
    setOpaque (true);
    setWantsKeyboardFocus (true);

    for (auto* c : std::initializer_list<juce::Component*> { &stage, &timeline, &openButton, &restartButton, &playButton,
                                                             &speedControl, &modeSeg, &handsSeg, &loopButton, &micButton,
                                                             &instrumentButton, &settingsButton, &minimiseButton,
                                                             &maximiseButton, &closeButton })
        addAndMakeVisible (c);
    addChildComponent (calibration);
    addChildComponent (settingsPanel);
    addChildComponent (toast);

    for (auto* c : std::initializer_list<juce::Component*> { &openButton, &restartButton, &playButton, &loopButton, &micButton,
                                                             &instrumentButton, &settingsButton, &modeSeg, &handsSeg, &speedControl, &timeline })
        c->setWantsKeyboardFocus (false);

    openButton.onClick       = [this] { openFileChooser(); };
    minimiseButton.onClick   = [this] { if (auto* w = window()) w->setMinimised (true); };
    maximiseButton.onClick   = [this] { if (auto* w = window()) w->setFullScreen (! w->isFullScreen()); };
    closeButton.onClick      = [] { juce::JUCEApplication::getInstance()->systemRequestedQuit(); };
    restartButton.onClick    = [this] { engine.withPlayer ([] (Player& p) { p.seek (p.getLoop().enabled ? p.getLoop().a : std::max (Player::kMinTime, (p.getSong() ? p.getSong()->firstNoteTime() : 0.0) - 1.5)); }); };
    playButton.onClick       = [this] { togglePlay(); };
    loopButton.onClick       = [this] { toggleLoop(); };
    settingsButton.onClick   = [this] { showSettings (! settingsOpen); };
    instrumentButton.onClick = [this] { showInstrumentWindow(); };
    micButton.onClick        = [this] {
        micOn = ! micOn;
        engine.setMicEnabled (micOn);
        micButton.setToggled (micOn);
        settingsPanel.setValues (volume, playMyPart, micOn, sensitivity, earlyWindow, jumpLeadIn, splitPitch, showNames, lookahead);
        toast.show (micOn ? "Listening to your piano" : "Not listening", micOn ? theme::heard : juce::Colour());
        saveSettings();
    };
    modeSeg.onChange      = [this] (int i) { setMode (i == 1 ? Mode::Learn : Mode::Freeplay); };
    modeSeg.setTooltip ("Learn waits for you to play each chord (Tab)");
    handsSeg.onChange     = [this] (int i) { setHands (i); };
    handsSeg.setTooltip ("Your part: the hand you play (1 / 2 / 3)");
    speedControl.onChange = [this] (double s) { setSpeed (s); };

    stage.onKey = [this] (int pitch, bool down) {
        engine.previewNote (pitch, down);
        if (down) heardOnScreenKey (pitch);
    };
    stage.onScrub = [this] (double secs) {
        if (! engine.snapshot().playing) engine.withPlayer ([secs] (Player& p) { p.seek (p.getTime() + secs); });
    };
    stage.onEmptyClick = [this] { openFileChooser(); };
    stage.onZoom = [this] (double f) { lookahead = juce::jlimit (1.5, 8.0, lookahead * f); stage.setLookahead (lookahead); };

    timeline.onSeek        = [this] (double t) { engine.withPlayer ([t] (Player& p) { p.seek (t); }); };
    timeline.onLoopChanged = [this] (double a, double b) { setLoopRegion (a, b); };
    timeline.onClearLoop   = [this] { clearLoop(); };
    timeline.onProgressive = [this] (bool on) {
        const double current = engine.snapshot().speed;
        loop.progressive = on;
        loop.startSpeed  = current < 0.95 ? current : 0.6;
        loop.step        = 0.05;
        loop.targetSpeed = std::max (1.0, current);
        pushLoop();
        toast.show (on ? "Each pass " + juce::String (juce::roundToInt (loop.step * 100)) + "% faster, up to "
                             + juce::String (juce::roundToInt (loop.targetSpeed * 100)) + "%"
                       : "Fixed speed loop", theme::loopTint);
    };
    timeline.onAddMarker   = [this] (double t) { addMarker (t); };
    timeline.onMarkerClicked = [this] (int i, juce::Rectangle<int> r) { editMarker (i, r); };

    // Settings sheet
    settingsPanel.onClose = [this] { showSettings (false); };
    settingsPanel.onInstrumentChosen = [this] (int i) { chooseInstrument (i); };
    settingsPanel.onRescan = [this] {
        plugins.scan ([this] {
            refreshInstrumentList();
            const auto wrong = plugins.getWrongArchitecture();
            toast.show (juce::String (instruments.size()) + " instruments found"
                        + (wrong.isEmpty() ? juce::String() : kDot + juce::String (wrong.size()) + " need the other Fall build"));
            saveSettings();
        });
        refreshInstrumentList();
    };
    settingsPanel.onAddPluginFile = [this] {
        auto chooser = std::make_shared<juce::FileChooser> ("Choose an instrument plugin (.vst3 or .dll)",
            juce::File::getSpecialLocation (juce::File::globalApplicationsDirectory), "*.vst3;*.dll");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::canSelectDirectories,
            [this, chooser] (const juce::FileChooser& fc) {
                if (fc.getResult() == juce::File()) return;
                const auto result = plugins.addFile (fc.getResult());
                refreshInstrumentList();
                saveSettings();
                if (result.instruments.isEmpty())
                {
                    toast.show (result.problem, juce::Colour (0xffff453a));
                    return;
                }
                // Picking a file by hand means "use this one".
                for (int i = 0; i < instruments.size(); ++i)
                    if (instruments[i].isDuplicateOf (result.instruments.getFirst()))
                        chooseInstrument (i + 1);
            });
    };
    settingsPanel.onShowInstrumentWindow = [this] { showInstrumentWindow(); };
    settingsPanel.onAudioSettings = [this] { openAudioSettings(); };
    settingsPanel.onCalibrate = [this] { showSettings (false); startCalibration(); };
    settingsPanel.onMicEnabled = [this] (bool b) { micOn = b; engine.setMicEnabled (b); micButton.setToggled (b); saveSettings(); };
    settingsPanel.onShowNames = [this] (bool b) { showNames = b; stage.setShowNoteNames (b); saveSettings(); };
    settingsPanel.onPlayMyPart = [this] (bool b) { playMyPart = b; applyHands(); saveSettings(); };
    settingsPanel.onSensitivity = [this] (double v) { sensitivity = v; engine.setSensitivity ((float) v); saveSettings(); };
    settingsPanel.onEarlyWindow = [this] (double v) { earlyWindow = v; engine.withPlayer ([v] (Player& p) { p.setEarlyWindow (v); }); saveSettings(); };
    settingsPanel.onJumpLeadIn = [this] (double v) { jumpLeadIn = v; engine.withPlayer ([v] (Player& p) { p.setJumpLeadIn (v); }); saveSettings(); };
    settingsPanel.onLookahead = [this] (double v) { lookahead = v; stage.setLookahead (v); saveSettings(); };
    settingsPanel.onVolume = [this] (double v) { volume = v; engine.setMasterGain ((float) v); saveSettings(); };
    settingsPanel.onSplitPitch = [this] (int p) {
        splitPitch = p;
        if (song != nullptr && ! song->handsFromTracks)
        {
            auto copy = std::make_shared<Song> (*song);
            ImportOptions io; io.splitPitch = p; io.preferTracks = false;
            assignHands (*copy, io);
            const double t = engine.snapshot().time;
            song = copy;
            engine.setSong (song);
            stage.setSong (song);
            applyHands();
            pushLoop();
            pushMarkers();
            engine.withPlayer ([t] (Player& pl) { pl.seek (t); });
        }
        saveSettings();
    };
    settingsPanel.getInputLevelDb = [this] { return engine.getInputLevelDb(); };
    settingsPanel.bindMeter();

    calibration.onSkip = [this] { nextCalibrationKey (false); };
    calibration.onDone = [this] { finishCalibration(); };

    restoreSettings();

    if (! opts.noAudio)
    {
        auto state = settings.get().getXmlValue ("audioDevice");
        const auto err = engine.openDevices (state.get());
        if (err.isNotEmpty())
            juce::MessageManager::callAsync ([this, err] { toast.show ("Audio: " + err); });
    }

    // Instruments: restore the last one, and scan once on first run.
    if (! scripted)
    {
        if (auto xml = settings.get().getXmlValue ("instrument"))
        {
            juce::PluginDescription d;
            if (d.loadFromXml (*xml))
                loadInstrument (d, settings.get().getValue ("instrumentState"));
        }
        // Always rescan in the background: already-known files are skipped, so it's quick,
        // and newly installed instruments just show up.
        plugins.scan ([this] { refreshInstrumentList(); saveSettings(); });
    }

    if (opts.midi.existsAsFile())
        loadFile (opts.midi);
    else if (! scripted)
    {
        const juce::File last (settings.get().getValue ("lastFile"));
        if (last.existsAsFile()) loadFile (last);
    }

    // Scripted state for screenshots
    if (opts.mode == "learn") setMode (Mode::Learn);
    if (opts.hands == "left") setHands (0);
    if (opts.hands == "right") setHands (2);
    if (opts.speed > 0) setSpeed (opts.speed);
    if (! opts.markers.empty()) { markers = opts.markers; pushMarkers(); }
    if (opts.loopA >= 0 && opts.loopB > opts.loopA) setLoopRegion (opts.loopA, opts.loopB);
    if (opts.seek > -1e8) engine.withPlayer ([this] (Player& p) { p.seek (opts.seek); });
    if (opts.play) engine.withPlayer ([] (Player& p) { p.setPlaying (true); });
    if (opts.openSettings) showSettings (true);
    if (opts.calibrate) startCalibration();

    setSize (opts.width, opts.height);
    vblank = juce::VBlankAttachment (this, [this] { frame(); });
}

MainComponent::~MainComponent()
{
    saveSettings();
    saveSongState();
    pluginWindow.reset();
    vblank = {};
    engine.withPlayer ([] (Player& p) { p.setPlaying (false); });
    engine.getDeviceManager().closeAudioDevice();
    engine.setInstrument (nullptr);
    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
}

// ---------------------------------------------------------------------------------------------
// Frame loop
// ---------------------------------------------------------------------------------------------
void MainComponent::frame()
{
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    float dt = lastFrameMs > 0.0 ? (float) ((nowMs - lastFrameMs) / 1000.0) : 1.0f / 60.0f;
    lastFrameMs = nowMs;
    dt = juce::jlimit (0.0f, 0.1f, dt);
    if (scripted) dt = 1.0f / 60.0f; // deterministic captures

    if (! engine.isDeviceRunning())
        engine.tickWithoutDevice (dt);

    auto s = engine.snapshot();

    // The audio thread moves time in block-sized steps; extrapolate so motion is smooth.
    double t = s.time;
    if (s.playing && ! s.waiting && engine.isDeviceRunning())
    {
        t += (nowMs - s.wallMs) / 1000.0 * s.speed;
        if (s.mode == Mode::Learn && s.nextGate >= s.time) t = std::min (t, s.nextGate);
        if (s.loop.enabled && s.time < s.loop.b) t = std::min (t, s.loop.b);
        if (t < displayTime && displayTime - t < 0.05) t = displayTime; // never step backwards by jitter
    }
    displayTime = t;

    std::array<HeardEvent, 64> heard;
    int numHeard = engine.popHeard (heard.data(), (int) heard.size());

    waitingFor = s.waiting ? waitingFor + dt : 0.0f;
    if (opts.autoLearn && s.waiting && waitingFor > 0.35f && numHeard < (int) heard.size())
    {
        // Simulated perfect player, for demos and screenshots.
        HeardEvent e;
        e.pitches = e.accepted = s.expected;
        engine.withPlayer ([&] (Player& p) { p.notePlayed (s.expected); });
        heard[(size_t) numHeard++] = e;
        s = engine.snapshot();
    }

    for (int i = 0; i < numHeard; ++i)
    {
        const auto& e = heard[(size_t) i];
        if (e.marker >= 0 && e.marker < (int) markers.size())
            toast.show ("Back to " + (markers[(size_t) e.marker].name.isNotEmpty() ? markers[(size_t) e.marker].name : juce::String ("marker")),
                        theme::loopTint);

        if (listeningMarker >= 0 && listeningMarker < (int) markers.size() && e.pitches.any())
        {
            for (int p = 0; p < 128; ++p)
                if (e.pitches.test ((size_t) p))
                {
                    markers[(size_t) listeningMarker].triggerPitch = p;
                    if (markerEditor != nullptr) markerEditor->setPitch (p);
                    pushMarkers();
                    toast.show ("Jump key: " + noteName (p), theme::heard);
                    break;
                }
            listeningMarker = -1;
        }
    }

    if (s.loopPass != lastLoopPass && s.loopPass > 0)
        toast.show ("Loop " + juce::String (s.loopPass + 1) + kDot + juce::String (juce::roundToInt (s.speed * 100)) + "%", theme::loopTint);
    lastLoopPass = s.loopPass;

    // Calibration: wait for the detector to capture the spotlighted key.
    if (calibIndex >= 0)
    {
        calibCooldown -= dt;
        if (calibCooldown <= 0.0f && calibCooldown > -1000.0f && engine.captureFinished())
        {
            engine.storeCapture (calib);
            calibration.flashSuccess();
            calibCooldown = -2000.0f; // advance after the success animation
            juce::Timer::callAfterDelay (550, [safe = juce::Component::SafePointer<MainComponent> (this)] {
                if (safe != nullptr) safe->nextCalibrationKey (true);
            });
        }
    }

    Animated::tickAll (dt);
    stage.frame (dt, s, t, heard.data(), numHeard);
    timeline.setTime (t);
    playButton.setPlaying (s.playing);
    speedControl.setSpeed (s.speed);
    micButton.setToggled (micOn && engine.hasInput());
    if (auto* w = window()) maximiseButton.setMaximised (w->isFullScreen());

    // Settings sheet slides over the stage.
    settingsSlide.target = settingsOpen ? 1.0f : 0.0f;
    if (settingsSlide.step (dt) || settingsPanel.isVisible())
    {
        const int x = getWidth() - juce::roundToInt ((float) kSettingsW * settingsSlide.value);
        settingsPanel.setBounds (x, kTopBarH, kSettingsW, getHeight() - kTopBarH);
        settingsPanel.setVisible (settingsSlide.value > 0.001f);
        repaint (getLocalBounds().withTrimmedTop (kTopBarH)); // dimming
    }

    dropHover.target = dragOver ? 1.0f : 0.0f;
    if (dropHover.step (dt) || song == nullptr) repaint();

    if (opts.scripted()) scriptedFrame (dt);
}

void MainComponent::scriptedFrame (float dt)
{
    scriptedTime += dt;
    ++frameCount;

    if (opts.framesDir != juce::File())
    {
        opts.framesDir.createDirectory();
        auto img = createComponentSnapshot (getLocalBounds(), true, 1.0f);
        auto f = opts.framesDir.getChildFile ("frame_" + juce::String (frameCount).paddedLeft ('0', 5) + ".png");
        f.deleteFile();
        juce::FileOutputStream out (f);
        juce::PNGImageFormat().writeImageToStream (img, out);
    }

    if (scriptedTime >= opts.wait)
    {
        int code = 0;
        if (opts.screenshot.isNotEmpty())
        {
            const auto file = juce::File::getCurrentWorkingDirectory().getChildFile (opts.screenshot);
            file.deleteFile();
            auto img = createComponentSnapshot (getLocalBounds(), true, 1.0f);
            juce::FileOutputStream out (file);
            code = out.openedOk() && juce::PNGImageFormat().writeImageToStream (img, out) ? 0 : 1;
            out.flush();
            std::cout << (code == 0 ? "Saved screenshot: " : "FAILED to save screenshot: ") << file.getFullPathName() << std::endl;
        }
        opts.screenshot = {};
        opts.framesDir = juce::File();
        juce::JUCEApplicationBase::getInstance()->setApplicationReturnValue (code);
        juce::JUCEApplicationBase::quit();
    }
}

// ---------------------------------------------------------------------------------------------
// Painting & layout
// ---------------------------------------------------------------------------------------------
juce::String MainComponent::subtitle() const
{
    if (song == nullptr) return "Drop a MIDI file to begin";
    juce::String s;
    int hands = 0;
    bool l = false, r = false;
    for (const auto& n : song->notes) { l |= n.hand == Hand::Left; r |= n.hand == Hand::Right; }
    hands = (int) l + (int) r;
    s << formatTime (song->length) << kDot << (int) song->notes.size() << " notes" + kDot
      << (hands == 2 ? "both hands" : "one hand") << kDot << instrumentName;
    return s;
}

void MainComponent::paint (juce::Graphics& g)
{
    const auto& pal = theme::palette();
    g.fillAll (pal.window);

    auto bar = getLocalBounds().removeFromTop (kTopBarH);
    g.setColour (pal.bar);
    g.fillRect (bar);
    g.setColour (pal.separator);
    g.fillRect (bar.removeFromBottom (1));

    auto titleArea = juce::Rectangle<int> (openButton.getRight() + 10, 0, restartButton.getX() - openButton.getRight() - 20, kTopBarH);
    g.setColour (pal.text);
    g.setFont (theme::font (15.0f, theme::Weight::Semibold));
    g.drawText (song ? song->title : juce::String ("Fall"), titleArea.withTrimmedBottom (kTopBarH / 2 - 3).withTrimmedTop (9),
                juce::Justification::bottomLeft, true);
    g.setColour (pal.textDim);
    g.setFont (theme::font (12.0f));
    g.drawText (subtitle(), titleArea.withTrimmedTop (kTopBarH / 2 + 1).withTrimmedBottom (8), juce::Justification::topLeft, true);
}

void MainComponent::paintOverChildren (juce::Graphics& g)
{
    // Dim the stage behind the settings sheet.
    if (settingsSlide.value > 0.001f)
    {
        g.setColour (juce::Colours::black.withAlpha (0.35f * juce::jlimit (0.0f, 1.0f, settingsSlide.value)));
        g.fillRect (getLocalBounds().withTrimmedTop (kTopBarH).withRight (settingsPanel.getX()));
    }

    if (calibration.isVisible())
        return;

    // Empty state / drop target.
    if (song == nullptr || dropHover.value > 0.01f)
    {
        auto area = stage.getBounds().toFloat();
        area = area.withHeight (area.getHeight() * 0.62f);
        const float s = 1.0f + 0.03f * dropHover.value;
        auto card = juce::Rectangle<float> (420.0f, 190.0f).withCentre (area.getCentre());
        card = card.withSizeKeepingCentre (card.getWidth() * s, card.getHeight() * s);

        if (song != nullptr)
        {
            g.setColour (juce::Colours::black.withAlpha (0.5f * dropHover.value));
            g.fillRect (stage.getBounds());
        }

        const float alpha = song == nullptr ? 1.0f : dropHover.value;
        g.setColour (juce::Colours::white.withAlpha (0.04f * alpha + 0.06f * dropHover.value));
        g.fillRoundedRectangle (card, 22.0f);
        juce::Path border;
        border.addRoundedRectangle (card.reduced (1.0f), 22.0f);
        juce::Path dashed;
        const float dashes[] = { 7.0f, 6.0f };
        juce::PathStrokeType (1.5f).createDashedStroke (dashed, border, dashes, 2);
        g.setColour ((dropHover.value > 0.01f ? theme::rightHand : juce::Colours::white).withAlpha ((0.28f + 0.5f * dropHover.value) * alpha));
        g.fillPath (dashed);

        g.setColour (juce::Colours::white.withAlpha (0.92f * alpha));
        g.setFont (theme::font (21.0f, theme::Weight::Semibold));
        g.drawText (dragOver ? "Drop to open" : "Drop a MIDI file here", card.withTrimmedBottom (card.getHeight() * 0.42f).withTrimmedTop (24.0f),
                    juce::Justification::centredBottom);
        g.setColour (juce::Colours::white.withAlpha (0.5f * alpha));
        g.setFont (theme::font (13.5f));
        g.drawText ("or click to choose one" + kDot + "Ctrl+O", card.withTrimmedTop (card.getHeight() * 0.6f), juce::Justification::centredTop);
    }
}

void MainComponent::resized()
{
    auto b = getLocalBounds();
    auto bar = b.removeFromTop (kTopBarH);
    timeline.setBounds (b.removeFromTop (kTimelineH));
    stage.setBounds (b);
    calibration.setBounds (getLocalBounds().withTrimmedTop (kTopBarH));
    toast.setBounds (getLocalBounds().withTrimmedTop (kTopBarH + kTimelineH + 18).withHeight (52));

    bar.reduce (12, 0);
    const int cy = bar.getCentreY();
    openButton.setBounds (juce::Rectangle<int> (36, 36).withPosition (bar.getX(), cy - 18));

    auto right = bar;
    const bool narrow = getWidth() < 1180;
    // Caption buttons hug the window's top-right corner, full bar height, like Windows 11.
    {
        auto caption = getLocalBounds().removeFromTop (kTopBarH).removeFromRight (46 * 3);
        closeButton.setBounds (caption.removeFromRight (46));
        maximiseButton.setBounds (caption.removeFromRight (46));
        minimiseButton.setBounds (caption.removeFromRight (46));
        right.removeFromRight (46 * 3 - 12 + 8);
    }
    settingsButton.setBounds (right.removeFromRight (36).withSizeKeepingCentre (36, 36));
    right.removeFromRight (4);
    instrumentButton.setBounds (right.removeFromRight (36).withSizeKeepingCentre (36, 36));
    right.removeFromRight (4);
    micButton.setBounds (right.removeFromRight (36).withSizeKeepingCentre (36, 36));
    right.removeFromRight (4);
    loopButton.setBounds (right.removeFromRight (36).withSizeKeepingCentre (36, 36));
    right.removeFromRight (12);
    handsSeg.setBounds (right.removeFromRight (narrow ? 156 : 180).withSizeKeepingCentre (narrow ? 156 : 180, 32));
    right.removeFromRight (10);
    modeSeg.setBounds (right.removeFromRight (narrow ? 156 : 180).withSizeKeepingCentre (narrow ? 156 : 180, 32));

    // Transport cluster sits in the middle of what's left, or the window if there's room.
    const int clusterW = 36 + 8 + 44 + 10 + 70;
    int cx = getWidth() / 2 - clusterW / 2;
    cx = std::min (cx, modeSeg.getX() - 20 - clusterW);
    cx = std::max (cx, openButton.getRight() + 200);
    restartButton.setBounds (cx, cy - 18, 36, 36);
    playButton.setBounds (restartButton.getRight() + 8, cy - 22, 44, 44);
    speedControl.setBounds (playButton.getRight() + 10, cy - 15, 70, 30);

    settingsPanel.setBounds (getWidth() - juce::roundToInt ((float) kSettingsW * settingsSlide.value), kTopBarH, kSettingsW, getHeight() - kTopBarH);
}

void MainComponent::mouseDown (const juce::MouseEvent& e)
{
    // The top bar is the window's title bar: drag it to move the window.
    draggingWindow = e.y < kTopBarH && window() != nullptr && ! window()->isFullScreen();
    if (draggingWindow)
    {
        windowDragger.startDraggingComponent (window(), e.getEventRelativeTo (window()));
        return;
    }

    if (settingsOpen && ! settingsPanel.getBounds().contains (e.getPosition()))
        showSettings (false);
}

void MainComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (draggingWindow)
        windowDragger.dragComponent (window(), e.getEventRelativeTo (window()), nullptr);
}

void MainComponent::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (e.y < kTopBarH)
        if (auto* w = window()) w->setFullScreen (! w->isFullScreen());
}

// ---------------------------------------------------------------------------------------------
// Keyboard
// ---------------------------------------------------------------------------------------------
bool MainComponent::keyPressed (const juce::KeyPress& k)
{
    const auto code = k.getKeyCode();
    const auto mods = k.getModifiers();

    if (code == juce::KeyPress::spaceKey) { togglePlay(); return true; }
    if (code == juce::KeyPress::leftKey)  { seekBy (mods.isShiftDown() ? -1.0 : -5.0); return true; }
    if (code == juce::KeyPress::rightKey) { seekBy (mods.isShiftDown() ? 1.0 : 5.0); return true; }
    if (code == juce::KeyPress::upKey)    { setSpeed (engine.snapshot().speed + 0.05); return true; }
    if (code == juce::KeyPress::downKey)  { setSpeed (engine.snapshot().speed - 0.05); return true; }
    if (code == juce::KeyPress::homeKey)  { restartButton.onClick(); return true; }
    if (code == juce::KeyPress::tabKey)   { setMode (engine.snapshot().mode == Mode::Learn ? Mode::Freeplay : Mode::Learn); return true; }
    if (code == juce::KeyPress::escapeKey)
    {
        if (calibIndex >= 0) finishCalibration();
        else if (settingsOpen) showSettings (false);
        return true;
    }
    if (mods.isCommandDown() && (code == 'O' || code == 'o')) { openFileChooser(); return true; }
    if (code == 'L' || code == 'l') { toggleLoop(); return true; }
    if (code == 'M' || code == 'm') { addMarker (engine.snapshot().time); return true; }
    if (code == '1') { setHands (0); return true; }
    if (code == '2') { setHands (1); return true; }
    if (code == '3') { setHands (2); return true; }
    if (code == ',') { showSettings (! settingsOpen); return true; }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Song & transport
// ---------------------------------------------------------------------------------------------
bool MainComponent::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (f.endsWithIgnoreCase (".mid") || f.endsWithIgnoreCase (".midi") || f.endsWithIgnoreCase (".kar"))
            return true;
    return false;
}

void MainComponent::filesDropped (const juce::StringArray& files, int, int)
{
    dragOver = false;
    for (auto& f : files)
        if (loadFile (juce::File (f))) return;
}

void MainComponent::openFileChooser()
{
    auto chooser = std::make_shared<juce::FileChooser> ("Open a MIDI file",
        songFile.existsAsFile() ? songFile.getParentDirectory() : juce::File::getSpecialLocation (juce::File::userMusicDirectory),
        "*.mid;*.midi;*.kar");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this, chooser] (const juce::FileChooser& fc) {
                              if (fc.getResult().existsAsFile()) loadFile (fc.getResult());
                          });
}

bool MainComponent::loadFile (const juce::File& f)
{
    auto s = std::make_shared<Song>();
    juce::String error;
    ImportOptions io;
    io.splitPitch = splitPitch;
    if (! loadMidiFile (f, *s, error, io))
    {
        toast.show (error, juce::Colour (0xffff453a));
        return false;
    }

    saveSongState();
    song = s;
    songFile = f;
    engine.setSong (song);
    stage.setSong (song);
    timeline.setSongLength (0.0, song->length);
    markers.clear();
    loop = {};
    lastLoopA = lastLoopB = -1;
    engine.withPlayer ([this] (Player& p) {
        p.setEarlyWindow (earlyWindow);
        p.setJumpLeadIn (jumpLeadIn);
        p.setMode (modeSeg.getSelected() == 1 ? Mode::Learn : Mode::Freeplay);
    });
    restoreSongState();
    applyHands();
    settings.get().setValue ("lastFile", f.getFullPathName());

    if (auto* w = dynamic_cast<juce::DocumentWindow*> (getTopLevelComponent()))
        w->setName ("Fall  -  " + song->title);
    toast.show (song->title, theme::rightHand);
    repaint();
    return true;
}

void MainComponent::togglePlay()
{
    if (song == nullptr) { openFileChooser(); return; }
    engine.withPlayer ([] (Player& p) {
        if (! p.isPlaying() && p.getSong() != nullptr && p.getTime() >= p.getSong()->length + 0.5)
            p.seek (std::max (Player::kMinTime, p.getSong()->firstNoteTime() - 1.5)); // finished: start over
        p.setPlaying (! p.isPlaying());
    });
}

void MainComponent::seekBy (double seconds)
{
    engine.withPlayer ([seconds] (Player& p) { p.seek (p.getTime() + seconds); });
}

void MainComponent::setSpeed (double s)
{
    s = juce::jlimit (0.1, 2.0, std::round (s * 20.0) / 20.0);
    engine.withPlayer ([s] (Player& p) { p.setSpeed (s); });
    speedControl.setSpeed (s);
    if (loop.progressive)
        loop.startSpeed = s; // the ramp continues from wherever you set it
}

void MainComponent::setMode (Mode m)
{
    modeSeg.setSelected (m == Mode::Learn ? 1 : 0);
    engine.withPlayer ([m] (Player& p) { p.setMode (m); });
    toast.show (m == Mode::Learn ? "Learn: the song waits for you" : "Free play", m == Mode::Learn ? theme::good : theme::rightHand);
    saveSettings();
}

void MainComponent::setHands (int segment)
{
    handsSegment = juce::jlimit (0, 2, segment);
    handsSeg.setSelected (handsSegment);
    applyHands();
    saveSettings();
}

void MainComponent::applyHands()
{
    const uint8_t mine = (uint8_t) (handsSegment == 0 ? HandLeft : handsSegment == 2 ? HandRight : HandBoth);
    const auto audible = (uint8_t) (playMyPart ? (int) HandBoth : (HandBoth & ~mine));
    engine.withPlayer ([mine, audible] (Player& p) {
        p.setPracticeHands (mine);
        p.setAudibleHands (audible);
    });
}

// ---------------------------------------------------------------------------------------------
// Loop & markers
// ---------------------------------------------------------------------------------------------
void MainComponent::setLoopRegion (double a, double b)
{
    if (song == nullptr) return;
    loop.enabled = true;
    loop.a = juce::jlimit (0.0, song->length, a);
    loop.b = juce::jlimit (loop.a + 0.2, song->length + 0.5, b);
    lastLoopA = loop.a;
    lastLoopB = loop.b;
    pushLoop();
}

void MainComponent::toggleLoop()
{
    if (song == nullptr) return;
    if (loop.enabled) { loop.enabled = false; pushLoop(); toast.show ("Loop off"); return; }
    if (lastLoopB > lastLoopA) setLoopRegion (lastLoopA, lastLoopB);
    else
    {
        const double t = std::max (0.0, engine.snapshot().time);
        setLoopRegion (t, t + 8.0);
    }
    toast.show ("Looping " + formatTime (loop.a) + " - " + formatTime (loop.b), theme::loopTint);
}

void MainComponent::clearLoop()
{
    loop.enabled = false;
    lastLoopA = lastLoopB = -1;
    pushLoop();
}

void MainComponent::pushLoop()
{
    const auto l = loop;
    engine.withPlayer ([l] (Player& p) { p.setLoop (l); });
    loopButton.setToggled (loop.enabled);
    timeline.setLoop (loop);
    saveSongState();
}

void MainComponent::addMarker (double t)
{
    if (song == nullptr) return;
    Marker m;
    m.time = std::max (0.0, t);
    m.name = "Marker " + juce::String ((int) markers.size() + 1);
    // Default jump keys: the top white keys (C8, B7, A7...). Songs rarely go there, and unlike the
    // lowest keys they're easy to tell apart through a laptop mic (A0/B0 overtones hide in bass chords).
    static const int defaults[] = { 108, 107, 105, 103, 101 };
    for (int d : defaults)
    {
        const bool used = std::any_of (markers.begin(), markers.end(), [d] (const Marker& x) { return x.triggerPitch == d; });
        if (! used && d > song->highestPitch) { m.triggerPitch = d; break; }
    }
    markers.push_back (m);
    std::sort (markers.begin(), markers.end(), [] (const Marker& a, const Marker& b) { return a.time < b.time; });
    pushMarkers();
    toast.show (m.triggerPitch >= 0 ? "Marker added" + kDot + "play " + noteName (m.triggerPitch) + " to jump here" : "Marker added",
                theme::loopTint);
}

void MainComponent::editMarker (int index, juce::Rectangle<int> screenBounds)
{
    if (index < 0 || index >= (int) markers.size()) return;
    auto editor = std::make_unique<MarkerEditor> (markers[(size_t) index].name, markers[(size_t) index].triggerPitch);
    editor->onChange = [this, index] (const juce::String& name, int pitch) {
        if (index >= (int) markers.size()) return;
        markers[(size_t) index].name = name;
        markers[(size_t) index].triggerPitch = pitch;
        pushMarkers();
    };
    editor->onListen = [this, index] { listeningMarker = index; };
    editor->onDelete = [this, index] {
        if (index < (int) markers.size()) markers.erase (markers.begin() + index);
        pushMarkers();
        if (markerEditor != nullptr)
            if (auto* box = markerEditor->findParentComponentOfClass<juce::CallOutBox>()) box->dismiss();
    };
    markerEditor = editor.get();
    juce::CallOutBox::launchAsynchronously (std::move (editor), screenBounds, nullptr);
}

void MainComponent::pushMarkers()
{
    const auto m = markers;
    engine.withPlayer ([m] (Player& p) { p.setMarkers (m); });
    timeline.setMarkers (markers);
    saveSongState();
}

void MainComponent::saveSongState()
{
    if (song == nullptr || ! songFile.existsAsFile() || scripted) return;
    auto* o = new juce::DynamicObject();
    juce::Array<juce::var> ms;
    for (const auto& m : markers)
    {
        auto* mo = new juce::DynamicObject();
        mo->setProperty ("time", m.time);
        mo->setProperty ("pitch", m.triggerPitch);
        mo->setProperty ("name", m.name);
        ms.add (juce::var (mo));
    }
    o->setProperty ("markers", ms);
    auto* lo = new juce::DynamicObject();
    lo->setProperty ("enabled", loop.enabled);
    lo->setProperty ("a", lastLoopA);
    lo->setProperty ("b", lastLoopB);
    lo->setProperty ("progressive", loop.progressive);
    o->setProperty ("loop", juce::var (lo));
    settings.setSongState (songFile, juce::var (o));
}

void MainComponent::restoreSongState()
{
    const auto v = settings.songState (songFile);
    if (! v.isObject()) { pushMarkers(); pushLoop(); return; }

    markers.clear();
    if (auto* arr = v["markers"].getArray())
        for (const auto& mv : *arr)
        {
            Marker m;
            m.time = mv["time"];
            m.triggerPitch = mv["pitch"];
            m.name = mv["name"].toString();
            markers.push_back (m);
        }

    const auto lv = v["loop"];
    lastLoopA = lv.hasProperty ("a") ? (double) lv["a"] : -1.0;
    lastLoopB = lv.hasProperty ("b") ? (double) lv["b"] : -1.0;
    loop.progressive = lv["progressive"];
    loop.enabled = (bool) lv["enabled"] && lastLoopB > lastLoopA;
    loop.a = std::max (0.0, lastLoopA);
    loop.b = std::max (0.0, lastLoopB);
    pushMarkers();
    pushLoop();
}

// ---------------------------------------------------------------------------------------------
// Instruments & audio
// ---------------------------------------------------------------------------------------------
void MainComponent::refreshInstrumentList()
{
    instruments = plugins.getInstruments();
    juce::StringArray names;
    for (const auto& d : instruments) names.add (d.name + (d.manufacturerName.isNotEmpty() ? "  (" + d.manufacturerName + ")" : juce::String()));
    juce::String note;
    if (const auto wrong = plugins.getWrongArchitecture(); ! wrong.isEmpty())
        note = "Found but not loadable in this " + juce::String (PluginManager::processBits()) + "-bit build: "
               + wrong.joinIntoString (", ") + ". Use Fall-windows-" + (PluginManager::processBits() == 64 ? "x86" : "x64") + " for those.";
    settingsPanel.setInstruments (names, instrumentIndex, plugins.isScanning(), note);
}

void MainComponent::chooseInstrument (int index)
{
    if (index <= 0)
    {
        pluginWindow.reset();
        engine.setInstrument (nullptr);
        instrumentIndex = 0;
        instrumentName = "Built-in piano";
        settings.get().removeValue ("instrument");
        toast.show ("Built-in piano");
        repaint();
        return;
    }
    if (index - 1 < instruments.size())
        loadInstrument (instruments[index - 1], {});
}

void MainComponent::loadInstrument (const juce::PluginDescription& d, const juce::String& base64State)
{
    toast.show ("Loading " + d.name + "...");
    plugins.load (d, engine.getSampleRate(), 512,
        [this, d, base64State, safe = juce::Component::SafePointer<MainComponent> (this)] (std::unique_ptr<juce::AudioPluginInstance> inst, const juce::String& err) {
            if (safe == nullptr) return;
            if (inst == nullptr)
            {
                toast.show ("Couldn't load " + d.name + (err.isNotEmpty() ? ": " + err : juce::String()), juce::Colour (0xffff453a));
                return;
            }
            if (base64State.isNotEmpty())
            {
                juce::MemoryBlock mb;
                if (mb.fromBase64Encoding (base64State))
                    inst->setStateInformation (mb.getData(), (int) mb.getSize());
            }
            pluginWindow.reset();
            engine.setInstrument (std::move (inst));
            instrumentName = d.name;
            instrumentIndex = 0;
            for (int i = 0; i < instruments.size(); ++i)
                if (instruments[i].isDuplicateOf (d)) instrumentIndex = i + 1;
            if (auto xml = d.createXml()) settings.get().setValue ("instrument", xml.get());
            refreshInstrumentList();
            toast.show (d.name + " ready", theme::good);
            repaint();
        });
}

void MainComponent::showInstrumentWindow()
{
    auto* inst = engine.getInstrument();
    if (inst == nullptr)
    {
        showSettings (true);
        toast.show ("Choose an instrument in Settings");
        return;
    }
    if (pluginWindow != nullptr) { pluginWindow->toFront (true); return; }
    pluginWindow = std::make_unique<PluginWindow> (*inst, [this] {
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this)] {
            if (safe != nullptr) safe->pluginWindow.reset();
        });
    });
}

void MainComponent::openAudioSettings()
{
    auto* selector = new juce::AudioDeviceSelectorComponent (engine.getDeviceManager(), 1, 2, 2, 2, false, false, false, false);
    selector->setSize (540, 420);
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (selector);
    o.dialogTitle = "Headphones & microphone";
    o.dialogBackgroundColour = theme::palette().window;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.componentToCentreAround = this;
    if (auto* w = o.launchAsync())
        w->enterModalState (true, juce::ModalCallbackFunction::create ([this] (int) { saveSettings(); }), false);
}

// ---------------------------------------------------------------------------------------------
// Calibration
// ---------------------------------------------------------------------------------------------
void MainComponent::startCalibration()
{
    engine.withPlayer ([] (Player& p) { p.setPlaying (false); });
    calibKeys.clear();
    for (int p = 21; p <= 108; p += 3) calibKeys.push_back (p);
    calibIndex = -1;
    calibration.setVisible (true);
    calibration.toFront (false);
    toast.toFront (false);
    nextCalibrationKey (false);
}

void MainComponent::nextCalibrationKey (bool)
{
    ++calibIndex;
    if (calibIndex >= (int) calibKeys.size()) { finishCalibration(); return; }
    const int p = calibKeys[(size_t) calibIndex];
    calibration.setStep (p, calibIndex, (int) calibKeys.size());
    stage.setSpotlight (p);
    engine.beginCapture (p);
    calibCooldown = 0.25f;
}

void MainComponent::finishCalibration()
{
    calibIndex = -1;
    calibration.setVisible (false);
    stage.setSpotlight (-1);
    engine.setCalibration (calib);
    settings.get().setValue ("calibration", calib.toBinary().toBase64Encoding());
    settingsPanel.setCalibrationStatus (calib.numKeys());
    if (calib.numKeys() > 0) toast.show ("Learned " + juce::String (calib.numKeys()) + " keys", theme::good);
    saveSettings();
}

// ---------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------
void MainComponent::showSettings (bool show)
{
    settingsOpen = show;
    if (show)
    {
        refreshInstrumentList();
        settingsPanel.setValues (volume, playMyPart, micOn, sensitivity, earlyWindow, jumpLeadIn, splitPitch, showNames, lookahead);
        settingsPanel.setCalibrationStatus (calib.numKeys());
        settingsPanel.setVisible (true);
        settingsPanel.toFront (false);
        toast.toFront (false);
    }
    settingsButton.setToggled (show);
    if (! show) grabKeyboardFocus();
}

void MainComponent::restoreSettings()
{
    auto& s = settings.get();
    if (! scripted)
    {
        handsSegment = s.getIntValue ("hands", 1);
        playMyPart   = s.getBoolValue ("playMyPart", true);
        showNames    = s.getBoolValue ("showNames", true);
        micOn        = s.getBoolValue ("micEnabled", true);
        volume       = s.getDoubleValue ("volume", 1.0);
        sensitivity  = s.getDoubleValue ("sensitivity", 0.6);
        earlyWindow  = s.getDoubleValue ("earlyWindow", 0.25);
        jumpLeadIn   = s.getDoubleValue ("jumpLeadIn", 1.0);
        lookahead    = s.getDoubleValue ("lookahead", 3.2);
        splitPitch   = s.getIntValue ("splitPitch", 60);
        modeSeg.setSelected (s.getIntValue ("mode", 0), false);
        engine.withPlayer ([sp = s.getDoubleValue ("speed", 1.0)] (Player& p) { p.setSpeed (sp); });

        if (auto xml = s.getXmlValue ("pluginList")) plugins.restoreList (xml.get());
        juce::MemoryBlock mb;
        if (mb.fromBase64Encoding (s.getValue ("calibration")) && calib.fromBinary (mb))
            engine.setCalibration (calib);
    }

    handsSeg.setSelected (handsSegment, false);
    stage.setShowNoteNames (showNames);
    stage.setLookahead (lookahead);
    engine.setMicEnabled (micOn);
    engine.setMasterGain ((float) volume);
    engine.setSensitivity ((float) sensitivity);
    micButton.setToggled (micOn);
    applyHands();
    refreshInstrumentList();
}

void MainComponent::saveSettings()
{
    if (scripted) return;
    auto& s = settings.get();
    s.setValue ("hands", handsSegment);
    s.setValue ("playMyPart", playMyPart);
    s.setValue ("showNames", showNames);
    s.setValue ("micEnabled", micOn);
    s.setValue ("volume", volume);
    s.setValue ("sensitivity", sensitivity);
    s.setValue ("earlyWindow", earlyWindow);
    s.setValue ("jumpLeadIn", jumpLeadIn);
    s.setValue ("lookahead", lookahead);
    s.setValue ("splitPitch", splitPitch);
    s.setValue ("mode", modeSeg.getSelected());
    s.setValue ("speed", engine.snapshot().speed);
    if (auto xml = plugins.saveList()) s.setValue ("pluginList", xml.get());
    if (auto xml = engine.getDeviceManager().createStateXml()) s.setValue ("audioDevice", xml.get());
    if (auto* inst = engine.getInstrument())
    {
        juce::MemoryBlock mb;
        inst->getStateInformation (mb);
        s.setValue ("instrumentState", mb.toBase64Encoding());
    }
    s.saveIfNeeded();
}

void MainComponent::heardOnScreenKey (int pitch)
{
    // Clicking a key counts as playing it: handy for trying learn mode without a piano nearby.
    engine.withPlayer ([pitch] (Player& p) { p.notePlayed (single (pitch)); });
    if (listeningMarker >= 0 && listeningMarker < (int) markers.size())
    {
        markers[(size_t) listeningMarker].triggerPitch = pitch;
        if (markerEditor != nullptr) markerEditor->setPitch (pitch);
        listeningMarker = -1;
        pushMarkers();
    }
}

} // namespace fall
