#pragma once

#include "Controls.h"

namespace fall
{

// Guided calibration: the stage spotlights a key, you play it, it turns green, next key.
class CalibrationOverlay : public juce::Component, private Animated
{
public:
    std::function<void()> onSkip, onDone;

    CalibrationOverlay()
    {
        addAndMakeVisible (skip);
        addAndMakeVisible (done);
        skip.setButtonText ("Skip key");
        done.setButtonText ("Done");
        done.setColour (juce::TextButton::buttonColourId, theme::palette().accent);
        done.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        skip.onClick = [this] { if (onSkip) onSkip(); };
        done.onClick = [this] { if (onDone) onDone(); };
    }

    void setStep (int pitch, int index, int total)
    {
        current = pitch;
        stepIndex = index;
        steps = total;
        slide.value = 1.0f;
        slide.target = 0.0f;
        success.snap (0.0f);
        repaint();
    }

    void flashSuccess() { success.target = 1.0f; }

    void tick (float dt) override
    {
        if (! isVisible()) return;
        bool m = slide.step (dt);
        m |= success.step (dt);
        listen += dt;
        juce::ignoreUnused (m);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto& pal = theme::palette();
        auto c = card();
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRoundedRectangle (c.translated (0, 6).expanded (4), 22.0f);
        g.setColour (pal.bar.withAlpha (0.97f));
        g.fillRoundedRectangle (c, 20.0f);

        auto inner = c.reduced (28.0f, 22.0f);
        g.setColour (pal.text);
        g.setFont (theme::font (19.0f, theme::Weight::Semibold));
        g.drawText ("Teach Fall your piano", inner.removeFromTop (28.0f), juce::Justification::centred);
        g.setColour (pal.textDim);
        g.setFont (theme::font (13.5f));
        g.drawText ("Play the highlighted key once, firmly, and let it ring.", inner.removeFromTop (22.0f), juce::Justification::centred);

        auto big = inner.removeFromTop (92.0f).translated (slide.value * 40.0f, 0);
        const float s = 1.0f + 0.08f * success.value;
        g.setColour (pal.text.interpolatedWith (theme::good, success.value).withAlpha (1.0f - slide.value * 0.8f));
        g.setFont (theme::font (56.0f * s, theme::Weight::Semibold));
        g.drawText (current >= 0 ? noteName (current) : juce::String ("-"), big, juce::Justification::centred);

        // Listening pulse
        const float pulse = 0.5f + 0.5f * std::sin (listen * 4.0f);
        auto dot = juce::Rectangle<float> (8.0f, 8.0f).withCentre ({ c.getCentreX(), big.getBottom() + 6.0f });
        g.setColour (theme::heard.withAlpha (0.3f + 0.5f * pulse * (1.0f - success.value)));
        g.fillEllipse (dot.expanded (3.0f * pulse));

        auto bar = juce::Rectangle<float> (c.getX() + 40.0f, big.getBottom() + 24.0f, c.getWidth() - 80.0f, 4.0f);
        g.setColour (pal.raised);
        g.fillRoundedRectangle (bar, 2.0f);
        g.setColour (pal.accent);
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * (float) stepIndex / (float) std::max (1, steps)), 2.0f);
        g.setColour (pal.textFaint);
        g.setFont (theme::font (12.0f));
        g.drawText (juce::String (stepIndex) + " of " + juce::String (steps), bar.translated (0, 8).withHeight (18.0f),
                    juce::Justification::centred);
    }

    void resized() override
    {
        auto c = card().reduced (28.0f, 22.0f).toNearestInt();
        auto row = c.removeFromBottom (34);
        done.setBounds (row.removeFromRight (110));
        row.removeFromRight (10);
        skip.setBounds (row.removeFromRight (110));
    }

private:
    juce::Rectangle<float> card() const
    {
        return juce::Rectangle<float> (420.0f, 300.0f).withCentre ({ getWidth() * 0.5f, std::max (170.0f, getHeight() * 0.32f) });
    }

    juce::TextButton skip, done;
    int current = -1, stepIndex = 0, steps = 1;
    float listen = 0.0f;
    Spring slide { 0.0f, 300.0f, 28.0f }, success { 0.0f, 420.0f, 22.0f };
};

// Edits one marker: its name and the piano key that jumps to it.
class MarkerEditor : public juce::Component
{
public:
    std::function<void (const juce::String& name, int pitch)> onChange;
    std::function<void()> onDelete, onListen;

    MarkerEditor (const juce::String& name, int pitch)
    {
        addAndMakeVisible (nameEditor);
        nameEditor.setText (name, false);
        nameEditor.setTextToShowWhenEmpty ("Marker name", theme::palette().textFaint);
        nameEditor.setFont (theme::font (14.0f));
        nameEditor.setIndents (8, 6);
        nameEditor.onTextChange = [this] { changed(); };

        addAndMakeVisible (keyBox);
        keyBox.addItem ("No key", 1);
        // Top keys first: rare in music and the most reliably heard; the lowest keys come last
        // because their overtones blend into ordinary bass notes.
        const int suggested[] = { 108, 107, 106, 105, 104, 21, 22, 23 };
        keyBox.addSectionHeading ("Suggested");
        for (int p : suggested) keyBox.addItem (noteName (p), p + 2);
        keyBox.addSectionHeading ("All keys");
        for (int p = 24; p <= 103; ++p) keyBox.addItem (noteName (p), p + 2);
        keyBox.setSelectedId (pitch >= 0 ? pitch + 2 : 1, juce::dontSendNotification);
        keyBox.onChange = [this] { changed(); };

        addAndMakeVisible (listen);
        listen.setButtonText ("Listen for a key");
        listen.onClick = [this] { listen.setButtonText ("Play the key now..."); if (onListen) onListen(); };

        addAndMakeVisible (remove);
        remove.setButtonText ("Delete marker");
        remove.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffff453a));
        remove.onClick = [this] { if (onDelete) onDelete(); };

        setSize (280, 176);
    }

    void setPitch (int p)
    {
        keyBox.setSelectedId (p + 2, juce::dontSendNotification);
        listen.setButtonText ("Listen for a key");
        changed();
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced (12);
        nameEditor.setBounds (b.removeFromTop (32));
        b.removeFromTop (8);
        keyBox.setBounds (b.removeFromTop (32));
        b.removeFromTop (8);
        listen.setBounds (b.removeFromTop (30));
        b.removeFromTop (8);
        remove.setBounds (b.removeFromTop (30));
    }

private:
    void changed()
    {
        const int id = keyBox.getSelectedId();
        if (onChange) onChange (nameEditor.getText(), id > 1 ? id - 2 : -1);
    }

    juce::TextEditor nameEditor;
    juce::ComboBox keyBox;
    juce::TextButton listen, remove;
};

} // namespace fall
