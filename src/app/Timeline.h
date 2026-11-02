#pragma once

#include "Controls.h"
#include "core/Player.h"

namespace fall
{

// Song progress, loop region and markers. Drag to seek, shift-drag to make a loop,
// drag the loop's edges to adjust, right-click for markers.
class Timeline : public juce::Component, private Animated
{
public:
    std::function<void (double)> onSeek;
    std::function<void (double a, double b)> onLoopChanged;
    std::function<void (double t)> onAddMarker;
    std::function<void (int index, juce::Rectangle<int> screenBounds)> onMarkerClicked;
    std::function<void()> onClearLoop;
    std::function<void (bool)> onProgressive;

    void setSongLength (double startTime, double endTime) { t0 = startTime; t1 = std::max (startTime + 0.1, endTime); }
    void setTime (double t) { now = t; repaint(); }
    void setLoop (const LoopSettings& l) { loop = l; repaint(); }
    void setMarkers (const std::vector<Marker>& m) { markers = m; repaint(); }

    void tick (float dt) override
    {
        hover.target = isMouseOver() || dragging != Drag::None ? 1.0f : 0.0f;
        if (hover.step (dt)) repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto& pal = theme::palette();
        auto b = getLocalBounds().toFloat();
        g.setColour (pal.bar);
        g.fillRect (b);
        g.setColour (pal.separator);
        g.fillRect (b.removeFromBottom (1.0f));

        g.setFont (theme::mono (11.5f));
        g.setColour (pal.textDim);
        g.drawText (formatTime (now), labelArea (true), juce::Justification::centredRight, false);
        g.drawText (formatTime (t1), labelArea (false), juce::Justification::centredLeft, false);

        const auto tr = track();
        const float h = 4.0f + 2.0f * hover.value;
        auto line = tr.withSizeKeepingCentre (tr.getWidth(), h);
        g.setColour (pal.raised);
        g.fillRoundedRectangle (line, h * 0.5f);

        if (loop.enabled)
        {
            const float xa = xFor (loop.a), xb = xFor (loop.b);
            auto band = juce::Rectangle<float> (xa, tr.getY() + 3.0f, xb - xa, tr.getHeight() - 6.0f);
            g.setColour (theme::loopTint.withAlpha (0.16f));
            g.fillRoundedRectangle (band, 4.0f);
            g.setColour (theme::loopTint.withAlpha (0.85f));
            g.fillRoundedRectangle (band.withWidth (3.0f), 1.5f);
            g.fillRoundedRectangle (band.withX (band.getRight() - 3.0f).withWidth (3.0f), 1.5f);
        }

        const float xp = xFor (now);
        g.setColour (pal.accent);
        g.fillRoundedRectangle (line.withRight (std::max (line.getX() + h, xp)), h * 0.5f);

        for (size_t i = 0; i < markers.size(); ++i)
        {
            const auto r = markerRect ((int) i);
            g.setColour (pal.text.withAlpha (0.85f));
            juce::Path d;
            d.addTriangle (r.getCentreX() - 5.0f, r.getY(), r.getCentreX() + 5.0f, r.getY(), r.getCentreX(), r.getY() + 7.0f);
            g.fillPath (d);
            if (markers[i].triggerPitch >= 0)
            {
                g.setFont (theme::font (9.5f, theme::Weight::Semibold));
                g.drawText (noteName (markers[i].triggerPitch), r.withY (r.getY() + 7.0f).withHeight (10.0f).expanded (8.0f, 0),
                            juce::Justification::centred, false);
            }
        }

        const float knob = 6.0f + 3.0f * hover.value;
        g.setColour (juce::Colours::black.withAlpha (0.25f));
        g.fillEllipse (xp - knob, tr.getCentreY() - knob + 1.0f, knob * 2.0f, knob * 2.0f);
        g.setColour (juce::Colours::white);
        g.fillEllipse (xp - knob, tr.getCentreY() - knob, knob * 2.0f, knob * 2.0f);

        if (hover.value > 0.01f && isMouseOver())
        {
            const float mx = (float) getMouseXYRelative().x;
            if (mx >= tr.getX() && mx <= tr.getRight())
            {
                const auto label = formatTime (timeFor (mx));
                auto bubble = juce::Rectangle<float> (44.0f, 18.0f).withCentre ({ mx, tr.getY() - 2.0f });
                bubble = bubble.withX (juce::jlimit (0.0f, b.getRight() - bubble.getWidth(), bubble.getX()));
                g.setColour (pal.text.withAlpha (0.9f * hover.value));
                g.fillRoundedRectangle (bubble, 9.0f);
                g.setColour (pal.bar.withAlpha (hover.value));
                g.setFont (theme::mono (10.5f));
                g.drawText (label, bubble, juce::Justification::centred, false);
            }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const float x = e.position.x;
        for (int i = 0; i < (int) markers.size(); ++i)
            if (markerRect (i).expanded (3.0f).contains (e.position))
            {
                if (onMarkerClicked) onMarkerClicked (i, localAreaToGlobal (markerRect (i).toNearestInt()));
                dragging = Drag::None;
                return;
            }

        if (e.mods.isPopupMenu())
        {
            showMenu (timeFor (x));
            return;
        }

        if (loop.enabled && std::abs (x - xFor (loop.a)) < 6.0f) { dragging = Drag::LoopA; return; }
        if (loop.enabled && std::abs (x - xFor (loop.b)) < 6.0f) { dragging = Drag::LoopB; return; }

        if (e.mods.isShiftDown())
        {
            dragging = Drag::NewLoop;
            loopAnchor = timeFor (x);
            return;
        }

        dragging = Drag::Seek;
        if (onSeek) onSeek (timeFor (x));
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        const double t = timeFor (e.position.x);
        switch (dragging)
        {
            case Drag::Seek:    if (onSeek) onSeek (t); break;
            case Drag::NewLoop: if (onLoopChanged && std::abs (t - loopAnchor) > 0.2) onLoopChanged (std::min (t, loopAnchor), std::max (t, loopAnchor)); break;
            case Drag::LoopA:   if (onLoopChanged) onLoopChanged (std::min (t, loop.b - 0.2), loop.b); break;
            case Drag::LoopB:   if (onLoopChanged) onLoopChanged (loop.a, std::max (t, loop.a + 0.2)); break;
            case Drag::None:    break;
        }
    }

    void mouseUp (const juce::MouseEvent&) override { dragging = Drag::None; }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const bool edge = loop.enabled && (std::abs (e.position.x - xFor (loop.a)) < 6.0f || std::abs (e.position.x - xFor (loop.b)) < 6.0f);
        setMouseCursor (edge ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::PointingHandCursor);
        repaint();
    }

private:
    enum class Drag { None, Seek, NewLoop, LoopA, LoopB };

    juce::Rectangle<float> track() const
    {
        return getLocalBounds().toFloat().withTrimmedBottom (1.0f).reduced (64.0f, 0.0f);
    }
    juce::Rectangle<float> labelArea (bool left) const
    {
        auto b = getLocalBounds().toFloat();
        return left ? b.withWidth (54.0f) : b.withLeft (b.getRight() - 54.0f);
    }
    float xFor (double t) const
    {
        const auto tr = track();
        return tr.getX() + (float) juce::jlimit (0.0, 1.0, (t - t0) / (t1 - t0)) * tr.getWidth();
    }
    double timeFor (float x) const
    {
        const auto tr = track();
        return t0 + juce::jlimit (0.0, 1.0, (double) ((x - tr.getX()) / tr.getWidth())) * (t1 - t0);
    }
    juce::Rectangle<float> markerRect (int i) const
    {
        const float x = xFor (markers[(size_t) i].time);
        return { x - 6.0f, 2.0f, 12.0f, 8.0f };
    }

    void showMenu (double t)
    {
        juce::PopupMenu m;
        m.addItem ("Add marker at " + formatTime (t), [this, t] { if (onAddMarker) onAddMarker (t); });
        m.addSeparator();
        m.addItem ("Loop from here (8 s)", [this, t] { if (onLoopChanged) onLoopChanged (t, std::min (t1, t + 8.0)); });
        m.addItem ("Speed up each pass", loop.enabled, loop.progressive, [this] { if (onProgressive) onProgressive (! loop.progressive); });
        m.addItem ("Clear loop", loop.enabled, false, [this] { if (onClearLoop) onClearLoop(); });
        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
    }

    double t0 = 0, t1 = 1, now = 0, loopAnchor = 0;
    LoopSettings loop;
    std::vector<Marker> markers;
    Drag dragging = Drag::None;
    Spring hover { 0.0f, 400.0f, 36.0f };
};

} // namespace fall
