#pragma once

#include "Controls.h"

namespace fall
{

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
