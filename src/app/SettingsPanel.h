#pragma once

#include "Controls.h"

namespace fall
{

// A sheet that slides in from the right. Each row is a label + a control; MainComponent wires the
// callbacks so this file stays free of app logic.
class SettingsPanel : public juce::Component, private Animated
{
public:
    // ---- wired by MainComponent ----
    std::function<void (int index)> onInstrumentChosen; // 0 = built-in, 1.. = instruments list
    std::function<void()> onRescan, onAddPluginFile, onShowInstrumentWindow, onAudioSettings, onClose, onSaveRecording;
    std::function<void (bool)> onMicEnabled, onShowNames, onPlayMyPart, onRawMic;
    std::function<juce::String()> getTuningText;
    std::function<void (double)> onSensitivity, onEarlyWindow, onLookahead, onVolume, onJumpLeadIn;
    std::function<void (int)> onSplitPitch;
    std::function<float()> getInputLevelDb;

    SettingsPanel()
    {
        addAndMakeVisible (viewport);
        viewport.setViewedComponent (&content, false);
        viewport.setScrollBarsShown (true, false);
        viewport.setScrollBarThickness (6);

        auto& p = content;
        header (p, "Instrument");
        addRow (p, "Plays the song", instrument);
        instrument.onChange = [this] { if (! updating && onInstrumentChosen) onInstrumentChosen (instrument.getSelectedItemIndex()); };
        addButtons (p, { { &rescan, "Find instruments" }, { &addFile, "Add plugin file..." } });
        hint (p, instrumentHint);
        addButtons (p, { { &showWindow, "Open instrument window" } });
        rescan.onClick     = [this] { if (onRescan) onRescan(); };
        addFile.onClick    = [this] { if (onAddPluginFile) onAddPluginFile(); };
        showWindow.onClick = [this] { if (onShowInstrumentWindow) onShowInstrumentWindow(); };
        addRow (p, "Volume", volume);
        setupSlider (volume, 0.0, 1.5, 0.01, [this] (double v) { if (onVolume) onVolume (v); });
        addRow (p, "Play my part too", playMyPart);
        playMyPart.onToggle = [this] (bool b) { if (onPlayMyPart) onPlayMyPart (b); };
        hint (p, "When off, only the other hand plays in your headphones.");

        header (p, "Audio");
        addButtons (p, { { &audio, "Headphones & microphone..." } });
        audio.onClick = [this] { if (onAudioSettings) onAudioSettings(); };

        header (p, "Listening");
        addRow (p, "Listen to my piano", micEnabled);
        micEnabled.onToggle = [this] (bool b) { if (onMicEnabled) onMicEnabled (b); };
        addRow (p, "Input level", meter);
        addRow (p, "Sensitivity", sensitivity);
        setupSlider (sensitivity, 0.0, 1.0, 0.01, [this] (double v) { if (onSensitivity) onSensitivity (v); });
        hint (p, "Higher catches quiet notes but may accept a wrong one now and then.");
        addRow (p, "Piano tuning", tuning);
        tuning.setFont (theme::font (14.0f));
        tuning.setColour (juce::Label::textColourId, theme::palette().textDim);
        hint (p, "Measured from what you play, like a tuner. Fall adjusts to it by itself.");
       #if JUCE_WINDOWS
        addRow (p, "Raw microphone", rawMic);
        rawMic.onToggle = [this] (bool b) { if (onRawMic) onRawMic (b); };
        hint (p, "Skips Windows' voice effects, which treat piano tones as noise. Turn off only if the input level stays empty.");
       #endif
        addButtons (p, { { &saveRecording, "Save what Fall heard" } });
        saveRecording.onClick = [this] { if (onSaveRecording) onSaveRecording(); };
        hint (p, "Saves the last 90 seconds from the microphone, with what Fall expected and heard, for tracking down missed notes.");

        header (p, "Learn mode");
        addRow (p, "Early tolerance", earlyWindow);
        setupSlider (earlyWindow, 0.0, 0.6, 0.01, [this] (double v) { if (onEarlyWindow) onEarlyWindow (v); });
        earlyWindow.textFromValueFunction = [] (double v) { return juce::String (juce::roundToInt (v * 1000)) + " ms"; };
        addRow (p, "Jump lead-in", jumpLeadIn);
        setupSlider (jumpLeadIn, 0.0, 4.0, 0.1, [this] (double v) { if (onJumpLeadIn) onJumpLeadIn (v); });
        jumpLeadIn.textFromValueFunction = [] (double v) { return juce::String (v, 1) + " s"; };
        addRow (p, "Hand split", splitPitch);
        setupSlider (splitPitch, 36, 84, 1, [this] (double v) { if (onSplitPitch) onSplitPitch ((int) v); });
        splitPitch.textFromValueFunction = [] (double v) { return "below " + noteName ((int) v); };
        hint (p, "Used when a MIDI file doesn't keep the hands on separate tracks.");

        header (p, "View");
        addRow (p, "Note names", showNames);
        showNames.onToggle = [this] (bool b) { if (onShowNames) onShowNames (b); };
        addRow (p, "Look ahead", lookahead);
        setupSlider (lookahead, 1.5, 8.0, 0.1, [this] (double v) { if (onLookahead) onLookahead (v); });
        lookahead.textFromValueFunction = [] (double v) { return juce::String (v, 1) + " s"; };

        addAndMakeVisible (closeButton);
        closeButton.onClick = [this] { if (onClose) onClose(); };
    }

    void setInstruments (const juce::StringArray& names, int selectedIndex, bool scanning, const juce::String& note = {})
    {
        if (instrumentHint != nullptr)
            instrumentHint->setText (note.isNotEmpty() ? note : "VST3 and older .dll instruments are supported.", juce::dontSendNotification);
        updating = true;
        instrument.clear (juce::dontSendNotification);
        instrument.addItem ("Built-in piano", 1);
        for (int i = 0; i < names.size(); ++i) instrument.addItem (names[i], i + 2);
        instrument.setSelectedItemIndex (selectedIndex, juce::dontSendNotification);
        rescan.setButtonText (scanning ? "Searching..." : "Find instruments");
        rescan.setEnabled (! scanning);
        updating = false;
    }

    void setValues (double vol, bool playMine, bool mic, double sens, double early, double lead, int split, bool names, double look)
    {
        volume.setValue (vol, juce::dontSendNotification);
        playMyPart.setToggled (playMine);
        micEnabled.setToggled (mic);
        sensitivity.setValue (sens, juce::dontSendNotification);
        earlyWindow.setValue (early, juce::dontSendNotification);
        jumpLeadIn.setValue (lead, juce::dontSendNotification);
        splitPitch.setValue (split, juce::dontSendNotification);
        showNames.setToggled (names);
        lookahead.setValue (look, juce::dontSendNotification);
    }

    void setRawMic (bool b) { rawMic.setToggled (b); }

    void tick (float) override
    {
        if (! isShowing())
            return;
        meter.repaint();
        if (getTuningText && ++tuningFrames % 15 == 0)
            tuning.setText (getTuningText(), juce::dontSendNotification);
    }

    void paint (juce::Graphics& g) override
    {
        const auto& pal = theme::palette();
        g.setColour (pal.bar);
        g.fillRect (getLocalBounds());
        g.setColour (pal.separator);
        g.fillRect (0, 0, 1, getHeight());
        g.setColour (pal.text);
        g.setFont (theme::font (20.0f, theme::Weight::Semibold));
        g.drawText ("Settings", 24, 18, 200, 30, juce::Justification::centredLeft);
    }

    void resized() override
    {
        closeButton.setBounds (getWidth() - 52, 16, 34, 34);
        viewport.setBounds (getLocalBounds().withTrimmedTop (60));
        layoutContent();
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::escapeKey && onClose) { onClose(); return true; }
        return false;
    }

private:
    struct Meter : juce::Component
    {
        std::function<float()> level;
        float shown = -60.0f;
        void paint (juce::Graphics& g) override
        {
            const float db = level ? level() : -100.0f;
            shown = db > shown ? db : shown - 1.2f;
            const float norm = juce::jlimit (0.0f, 1.0f, (shown + 60.0f) / 60.0f);
            auto r = getLocalBounds().toFloat().withSizeKeepingCentre ((float) getWidth(), 6.0f);
            g.setColour (theme::palette().raised);
            g.fillRoundedRectangle (r, 3.0f);
            g.setColour (norm > 0.92f ? juce::Colour (0xffff453a) : theme::good);
            g.fillRoundedRectangle (r.withWidth (std::max (6.0f, r.getWidth() * norm)), 3.0f);
        }
    };

    struct Item { juce::Component* label; juce::Component* control; int height; bool fullWidth; };

    void header (juce::Component& p, const juce::String& text)
    {
        auto* l = labels.add (new juce::Label ({}, text.toUpperCase()));
        l->setFont (theme::font (11.5f, theme::Weight::Semibold));
        l->setColour (juce::Label::textColourId, theme::palette().textDim);
        p.addAndMakeVisible (l);
        items.push_back ({ l, nullptr, 44, true });
    }

    void hint (juce::Component& p, const juce::String& text)
    {
        auto* l = labels.add (new juce::Label ({}, text));
        l->setFont (theme::font (12.0f));
        l->setColour (juce::Label::textColourId, theme::palette().textFaint);
        p.addAndMakeVisible (l);
        items.push_back ({ l, nullptr, 34, true });
    }

    void hint (juce::Component& p, juce::Label*& keep)
    {
        hint (p, juce::String());
        keep = labels.getLast();
    }

    void addRow (juce::Component& p, const juce::String& text, juce::Component& control)
    {
        auto* l = labels.add (new juce::Label ({}, text));
        l->setFont (theme::font (14.0f));
        p.addAndMakeVisible (l);
        p.addAndMakeVisible (control);
        items.push_back ({ l, &control, 40, false });
    }

    void addButtons (juce::Component& p, std::initializer_list<std::pair<juce::TextButton*, const char*>> buttons)
    {
        auto* row = rows.add (new juce::Component());
        for (auto& [b, text] : buttons)
        {
            b->setButtonText (text);
            row->addAndMakeVisible (b);
        }
        p.addAndMakeVisible (row);
        items.push_back ({ row, nullptr, 42, true });
    }

    void setupSlider (juce::Slider& s, double lo, double hi, double step, std::function<void (double)> cb)
    {
        s.setSliderStyle (juce::Slider::LinearHorizontal);
        s.setTextBoxStyle (juce::Slider::TextBoxRight, true, 72, 22);
        s.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        s.setColour (juce::Slider::textBoxTextColourId, theme::palette().textDim);
        s.setRange (lo, hi, step);
        s.onValueChange = [&s, cb = std::move (cb)] { cb (s.getValue()); };
    }

    void layoutContent()
    {
        const int w = viewport.getWidth() - 10;
        int y = 0;
        for (auto& it : items)
        {
            if (it.fullWidth)
            {
                it.label->setBounds (20, y, w - 34, it.height);
                if (auto* row = dynamic_cast<juce::Component*> (it.label); row != nullptr && rows.contains (row))
                {
                    const int n = row->getNumChildComponents();
                    int x = 0;
                    const int bw = (w - 44 - 8 * (n - 1)) / std::max (1, n);
                    for (auto* c : row->getChildren()) { c->setBounds (x, 6, bw, 30); x += bw + 8; }
                }
            }
            else
            {
                it.label->setBounds (20, y, 150, it.height);
                const bool isSwitch = dynamic_cast<Switch*> (it.control) != nullptr;
                it.control->setBounds (isSwitch ? w - 64 : 170, y + 6, isSwitch ? 44 : w - 190, it.height - 12);
            }
            y += it.height;
        }
        content.setSize (w, y + 24);
    }

    juce::Viewport viewport;
    juce::Component content;
    juce::OwnedArray<juce::Label> labels;
    juce::OwnedArray<juce::Component> rows;
    std::vector<Item> items;
    bool updating = false;

    juce::ComboBox instrument;
    juce::TextButton rescan, addFile, showWindow, audio, saveRecording;
    juce::Label tuning;
    int tuningFrames = 0;
    juce::Slider volume, sensitivity, earlyWindow, lookahead, jumpLeadIn, splitPitch;
    Switch playMyPart, micEnabled, showNames, rawMic;
    Meter meter;
    juce::Label* instrumentHint = nullptr;

    struct CloseButton : Pressable
    {
        void paint (juce::Graphics& g) override
        {
            const auto& pal = theme::palette();
            auto b = getLocalBounds().toFloat().reduced (3.0f);
            g.addTransform (pressTransform());
            g.setColour (pal.raised.interpolatedWith (pal.raisedHover, hover.value));
            g.fillEllipse (b);
            const auto c = b.getCentre();
            g.setColour (pal.textDim);
            g.drawLine (c.x - 5, c.y - 5, c.x + 5, c.y + 5, 1.7f);
            g.drawLine (c.x - 5, c.y + 5, c.x + 5, c.y - 5, 1.7f);
        }
    } closeButton;

public:
    void bindMeter() { meter.level = [this] { return getInputLevelDb ? getInputLevelDb() : -100.0f; }; }
};

} // namespace fall
