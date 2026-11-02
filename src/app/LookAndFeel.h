#pragma once

#include "Theme.h"

namespace fall
{

// Restyles stock JUCE widgets (combo boxes, sliders, menus, dialogs) to match the custom controls.
class FallLookAndFeel : public juce::LookAndFeel_V4
{
public:
    FallLookAndFeel() { refresh(); }

    void refresh()
    {
        const auto& p = theme::palette();
        auto scheme = getDarkColourScheme();
        scheme.setUIColour (ColourScheme::windowBackground, p.window);
        scheme.setUIColour (ColourScheme::widgetBackground, p.raised);
        scheme.setUIColour (ColourScheme::menuBackground, p.bar);
        scheme.setUIColour (ColourScheme::outline, p.separator);
        scheme.setUIColour (ColourScheme::defaultText, p.text);
        scheme.setUIColour (ColourScheme::defaultFill, p.accent);
        scheme.setUIColour (ColourScheme::highlightedText, juce::Colours::white);
        scheme.setUIColour (ColourScheme::highlightedFill, p.accent);
        scheme.setUIColour (ColourScheme::menuText, p.text);
        setColourScheme (scheme);

        setColour (juce::ResizableWindow::backgroundColourId, p.window);
        setColour (juce::TextButton::buttonColourId, p.raised);
        setColour (juce::TextButton::textColourOffId, p.text);
        setColour (juce::ComboBox::backgroundColourId, p.raised);
        setColour (juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
        setColour (juce::ComboBox::textColourId, p.text);
        setColour (juce::ComboBox::arrowColourId, p.textDim);
        setColour (juce::PopupMenu::backgroundColourId, p.bar);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, p.accent);
        setColour (juce::Slider::trackColourId, p.accent);
        setColour (juce::Slider::backgroundColourId, p.raised);
        setColour (juce::Slider::thumbColourId, juce::Colours::white);
        setColour (juce::Label::textColourId, p.text);
        setColour (juce::TextEditor::backgroundColourId, p.raised);
        setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        setColour (juce::TextEditor::focusedOutlineColourId, p.accent);
        setColour (juce::TextEditor::textColourId, p.text);
        setColour (juce::TooltipWindow::backgroundColourId, p.text.withAlpha (0.92f));
        setColour (juce::TooltipWindow::textColourId, p.window);
        setColour (juce::ListBox::backgroundColourId, p.bar);
        setColour (juce::CaretComponent::caretColourId, p.accent);

       #if JUCE_WINDOWS
        setDefaultSansSerifTypefaceName ("Segoe UI");
       #endif
    }

    juce::Font getTextButtonFont (juce::TextButton&, int) override { return theme::font (13.5f, theme::Weight::Semibold); }
    juce::Font getComboBoxFont (juce::ComboBox&) override { return theme::font (13.5f); }
    juce::Font getPopupMenuFont() override { return theme::font (14.0f); }
    juce::Font getLabelFont (juce::Label& l) override { return l.getFont().getHeight() > 0 ? l.getFont() : theme::font (13.5f); }

    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& bg, bool over, bool down) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (0.5f);
        auto c = bg;
        if (over) c = c.brighter (theme::palette().dark ? 0.12f : 0.0f).darker (theme::palette().dark ? 0.0f : 0.04f);
        if (down) c = c.darker (0.12f);
        g.setColour (c);
        g.fillRoundedRectangle (r, std::min (9.0f, r.getHeight() * 0.5f));
    }

    void drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box) override
    {
        auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h).reduced (0.5f);
        g.setColour (box.findColour (juce::ComboBox::backgroundColourId).brighter (box.isMouseOver() ? 0.06f : 0.0f));
        g.fillRoundedRectangle (r, 8.0f);
        juce::Path chevron;
        const float cx = (float) w - 16.0f, cy = (float) h * 0.5f;
        chevron.startNewSubPath (cx - 4.0f, cy - 2.0f);
        chevron.lineTo (cx, cy + 2.5f);
        chevron.lineTo (cx + 4.0f, cy - 2.0f);
        g.setColour (box.findColour (juce::ComboBox::arrowColourId));
        g.strokePath (chevron, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds (8, 1, box.getWidth() - 30, box.getHeight() - 2);
        label.setFont (getComboBoxFont (box));
    }

    void drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float, float,
                           juce::Slider::SliderStyle, juce::Slider& s) override
    {
        const float cy = (float) y + (float) h * 0.5f;
        auto track = juce::Rectangle<float> ((float) x, cy - 2.0f, (float) w, 4.0f);
        g.setColour (s.findColour (juce::Slider::backgroundColourId));
        g.fillRoundedRectangle (track, 2.0f);
        g.setColour (s.findColour (juce::Slider::trackColourId));
        g.fillRoundedRectangle (track.withRight (pos), 2.0f);
        const float k = s.isMouseButtonDown() ? 10.0f : 9.0f;
        g.setColour (juce::Colours::black.withAlpha (0.25f));
        g.fillEllipse (pos - k, cy - k + 1.0f, k * 2.0f, k * 2.0f);
        g.setColour (juce::Colours::white);
        g.fillEllipse (pos - k, cy - k, k * 2.0f, k * 2.0f);
    }

    int getSliderThumbRadius (juce::Slider&) override { return 10; }

    void drawPopupMenuBackground (juce::Graphics& g, int w, int h) override
    {
        g.fillAll (findColour (juce::PopupMenu::backgroundColourId));
        g.setColour (theme::palette().separator);
        g.drawRect (0, 0, w, h);
    }

    void drawTooltip (juce::Graphics& g, const juce::String& text, int w, int h) override
    {
        auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h);
        g.setColour (findColour (juce::TooltipWindow::backgroundColourId));
        g.fillRoundedRectangle (r, 6.0f);
        g.setColour (findColour (juce::TooltipWindow::textColourId));
        g.setFont (theme::font (12.5f, theme::Weight::Medium));
        g.drawText (text, r.reduced (8.0f, 0), juce::Justification::centred, true);
    }

    juce::Rectangle<int> getTooltipBounds (const juce::String& text, juce::Point<int> pos, juce::Rectangle<int> parent) override
    {
        const int w = (int) juce::GlyphArrangement::getStringWidth (theme::font (12.5f, theme::Weight::Medium), text) + 20;
        return juce::Rectangle<int> (pos.x - w / 2, pos.y + 18, w, 26).constrainedWithin (parent);
    }
};

// Everything we remember between runs.
class AppSettings
{
public:
    AppSettings()
    {
        juce::PropertiesFile::Options o;
        o.applicationName = "Fall";
        o.filenameSuffix = ".settings";
       #if JUCE_LINUX
        o.folderName = ".config/Fall";
       #else
        o.folderName = "Fall";
       #endif
        o.osxLibrarySubFolder = "Application Support";
        props.setStorageParameters (o);
    }

    juce::PropertiesFile& get() { return *props.getUserSettings(); }

    juce::var songState (const juce::File& f)
    {
        return juce::JSON::parse (get().getValue (songKey (f)));
    }

    void setSongState (const juce::File& f, const juce::var& v)
    {
        get().setValue (songKey (f), juce::JSON::toString (v, true));
        get().saveIfNeeded();
    }

private:
    static juce::String songKey (const juce::File& f)
    {
        return "song_" + juce::String::toHexString (f.getFullPathName().toLowerCase().hashCode64());
    }

    juce::ApplicationProperties props;
};

} // namespace fall
