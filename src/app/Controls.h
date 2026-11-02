#pragma once

#include "Theme.h"

#include <functional>

namespace fall
{

// Components that animate register here; MainComponent ticks them all once per frame.
class Animated
{
public:
    Animated() { registry().push_back (this); }
    virtual ~Animated() { auto& r = registry(); r.erase (std::remove (r.begin(), r.end(), this), r.end()); }
    virtual void tick (float dt) = 0;

    static void tickAll (float dt)
    {
        auto copy = registry();
        for (auto* a : copy) a->tick (dt);
    }

private:
    static std::vector<Animated*>& registry()
    {
        static std::vector<Animated*> r;
        return r;
    }
};

// ---------------------------------------------------------------------------------------------
// Icons, drawn as paths so they stay crisp at any scale.
// ---------------------------------------------------------------------------------------------
namespace icons
{
    inline juce::Path folder (juce::Rectangle<float> r)
    {
        juce::Path p;
        const float x = r.getX(), y = r.getY() + r.getHeight() * 0.12f, w = r.getWidth(), h = r.getHeight() * 0.76f;
        p.startNewSubPath (x, y + h * 0.15f);
        p.lineTo (x, y + h);
        p.lineTo (x + w, y + h);
        p.lineTo (x + w, y + h * 0.28f);
        p.lineTo (x + w * 0.48f, y + h * 0.28f);
        p.lineTo (x + w * 0.38f, y);
        p.lineTo (x + w * 0.04f, y);
        p.closeSubPath();
        return p;
    }

    inline juce::Path sliders (juce::Rectangle<float> r)
    {
        juce::Path p;
        for (int i = 0; i < 3; ++i)
        {
            const float y = r.getY() + r.getHeight() * (0.2f + 0.3f * (float) i);
            p.addRoundedRectangle (r.getX(), y - 0.9f, r.getWidth(), 1.8f, 0.9f);
            const float kx = r.getX() + r.getWidth() * (i == 1 ? 0.68f : (i == 0 ? 0.3f : 0.45f));
            p.addEllipse (kx - 3.2f, y - 3.2f, 6.4f, 6.4f);
        }
        return p;
    }

    inline juce::Path loop (juce::Rectangle<float> r)
    {
        juce::Path p;
        const auto c = r.getCentre();
        const float rx = r.getWidth() * 0.42f, ry = r.getHeight() * 0.32f;
        p.addCentredArc (c.x, c.y, rx, ry, 0.0f, 0.35f, juce::MathConstants<float>::pi - 0.35f, true);
        p.addCentredArc (c.x, c.y, rx, ry, 0.0f, juce::MathConstants<float>::pi + 0.35f, juce::MathConstants<float>::twoPi - 0.35f, true);
        juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded).createStrokedPath (p, p);
        // arrow heads
        juce::Path a;
        const float s = r.getWidth() * 0.16f;
        a.addTriangle (c.x + rx * 0.35f, c.y - ry - s, c.x + rx * 0.35f + s * 1.3f, c.y - ry, c.x + rx * 0.35f, c.y - ry + s);
        a.addTriangle (c.x - rx * 0.35f, c.y + ry - s, c.x - rx * 0.35f - s * 1.3f, c.y + ry, c.x - rx * 0.35f, c.y + ry + s);
        p.addPath (a);
        return p;
    }

    inline juce::Path mic (juce::Rectangle<float> r)
    {
        juce::Path p;
        const auto c = r.getCentre();
        const float w = r.getWidth() * 0.36f, h = r.getHeight() * 0.56f;
        p.addRoundedRectangle (c.x - w * 0.5f, r.getY(), w, h, w * 0.5f);
        juce::Path cup;
        cup.addCentredArc (c.x, r.getY() + h * 0.55f, w * 0.95f, h * 0.62f, 0.0f,
                           juce::MathConstants<float>::halfPi, juce::MathConstants<float>::halfPi * 3.0f, true);
        cup.startNewSubPath (c.x, r.getY() + h * 1.17f);
        cup.lineTo (c.x, r.getBottom() - 1.0f);
        cup.startNewSubPath (c.x - w * 0.6f, r.getBottom() - 1.0f);
        cup.lineTo (c.x + w * 0.6f, r.getBottom() - 1.0f);
        juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded).createStrokedPath (cup, cup);
        p.addPath (cup);
        return p;
    }

    inline juce::Path restart (juce::Rectangle<float> r)
    {
        juce::Path p;
        const float x = r.getX(), y = r.getY(), w = r.getWidth(), h = r.getHeight();
        p.addRoundedRectangle (x + w * 0.08f, y + h * 0.15f, w * 0.14f, h * 0.7f, w * 0.05f);
        p.addTriangle (x + w * 0.92f, y + h * 0.15f, x + w * 0.92f, y + h * 0.85f, x + w * 0.28f, y + h * 0.5f);
        return p;
    }

    inline juce::Path piano (juce::Rectangle<float> r)
    {
        juce::Path p;
        juce::Path outline;
        outline.addRoundedRectangle (r.reduced (0.8f), 2.0f);
        juce::PathStrokeType (1.5f).createStrokedPath (outline, outline);
        p.addPath (outline);
        for (int i = 0; i < 3; ++i)
            p.addRectangle (r.getX() + r.getWidth() * (0.24f + 0.26f * (float) i) - 1.4f, r.getY() + 1.0f, 2.8f, r.getHeight() * 0.55f);
        return p;
    }

    inline juce::Path flag (juce::Rectangle<float> r)
    {
        juce::Path p;
        p.addRectangle (r.getX() + r.getWidth() * 0.2f, r.getY(), 1.6f, r.getHeight());
        p.addTriangle (r.getX() + r.getWidth() * 0.2f, r.getY(), r.getRight(), r.getY() + r.getHeight() * 0.25f,
                       r.getX() + r.getWidth() * 0.2f, r.getY() + r.getHeight() * 0.5f);
        return p;
    }
} // namespace icons

// ---------------------------------------------------------------------------------------------
// Base for anything clickable: hover glow + press squish.
// ---------------------------------------------------------------------------------------------
class Pressable : public juce::Component, public juce::SettableTooltipClient, protected Animated
{
public:
    std::function<void()> onClick;

    void setToggled (bool t) { toggled = t; toggle.target = t ? 1.0f : 0.0f; }
    bool isToggled() const { return toggled; }

    void tick (float dt) override
    {
        hover.target = isMouseOver (true) && isEnabled() ? 1.0f : 0.0f;
        press.target = isMouseButtonDown() && isMouseOver (true) ? 1.0f : 0.0f;
        bool moving = hover.step (dt);
        moving |= press.step (dt);
        moving |= toggle.step (dt);
        if (moving || needsFrame) repaint();
        needsFrame = moving;
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (isEnabled() && getLocalBounds().contains (e.getPosition()) && onClick) onClick();
    }
    void mouseEnter (const juce::MouseEvent&) override { setMouseCursor (juce::MouseCursor::PointingHandCursor); }

protected:
    // Scale content around the centre while pressed: a 6% squish feels physical without being cute.
    juce::AffineTransform pressTransform() const
    {
        const float s = 1.0f - 0.06f * press.value;
        const auto c = getLocalBounds().toFloat().getCentre();
        return juce::AffineTransform::scale (s, s, c.x, c.y);
    }

    Spring hover { 0.0f, 420.0f, 38.0f }, press { 0.0f, 700.0f, 34.0f }, toggle { 0.0f, 380.0f, 32.0f };
    bool toggled = false, needsFrame = true;
};

class IconButton : public Pressable
{
public:
    using IconFn = std::function<juce::Path (juce::Rectangle<float>)>;
    IconButton (IconFn fn, const juce::String& tip) : icon (std::move (fn)) { setTooltip (tip); }

    void setBadge (juce::String b) { badge = std::move (b); repaint(); }

    void paint (juce::Graphics& g) override
    {
        const auto& pal = theme::palette();
        auto b = getLocalBounds().toFloat().reduced (1.0f);
        g.addTransform (pressTransform());
        const float r = std::min (b.getWidth(), b.getHeight()) * 0.5f;
        g.setColour (pal.raised.withAlpha (0.0f).interpolatedWith (pal.raisedHover, std::max (hover.value, 0.0f))
                         .interpolatedWith (pal.accent.withAlpha (0.18f), toggle.value));
        g.fillRoundedRectangle (b, std::min (r, 9.0f));
        const auto ic = b.withSizeKeepingCentre (17.0f, 17.0f);
        g.setColour (pal.textDim.interpolatedWith (pal.text, hover.value).interpolatedWith (pal.accent, toggle.value));
        g.fillPath (icon (ic));
        if (badge.isNotEmpty())
        {
            g.setFont (theme::font (10.0f, theme::Weight::Semibold));
            g.drawText (badge, b.withTrimmedTop (b.getHeight() * 0.62f), juce::Justification::centred);
        }
    }

private:
    IconFn icon;
    juce::String badge;
};

// Play/pause with the icon morphing between the two shapes.
class PlayButton : public Pressable
{
public:
    PlayButton() { setTooltip ("Play / pause (Space)"); }
    void setPlaying (bool p) { morph.target = p ? 1.0f : 0.0f; }

    void tick (float dt) override
    {
        const bool m = morph.step (dt);
        Pressable::tick (dt);
        if (m) repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto& pal = theme::palette();
        auto b = getLocalBounds().toFloat().reduced (2.0f);
        const float d = std::min (b.getWidth(), b.getHeight());
        auto circle = b.withSizeKeepingCentre (d, d);
        g.addTransform (pressTransform());

        g.setColour (pal.accent.withAlpha (0.25f * hover.value));
        g.fillEllipse (circle.expanded (3.0f * hover.value));
        g.setColour (pal.accent.brighter (0.08f * hover.value));
        g.fillEllipse (circle);

        // Two quads: halves of the triangle <-> the two pause bars.
        const auto c = circle.getCentre();
        const float s = d * 0.36f;
        const float m = morph.value;
        auto lerp = [m] (juce::Point<float> from, juce::Point<float> to) { return from + (to - from) * m; };
        const float tx = c.x - s * 0.38f; // optical centre of the triangle
        const juce::Point<float> play[2][4] = {
            { { tx, c.y - s * 0.62f }, { tx + s * 0.58f, c.y - s * 0.31f }, { tx + s * 0.58f, c.y + s * 0.31f }, { tx, c.y + s * 0.62f } },
            { { tx + s * 0.58f, c.y - s * 0.31f }, { tx + s * 1.16f, c.y }, { tx + s * 1.16f, c.y }, { tx + s * 0.58f, c.y + s * 0.31f } } };
        const juce::Point<float> pause[2][4] = {
            { { c.x - s * 0.5f, c.y - s * 0.55f }, { c.x - s * 0.14f, c.y - s * 0.55f }, { c.x - s * 0.14f, c.y + s * 0.55f }, { c.x - s * 0.5f, c.y + s * 0.55f } },
            { { c.x + s * 0.14f, c.y - s * 0.55f }, { c.x + s * 0.5f, c.y - s * 0.55f }, { c.x + s * 0.5f, c.y + s * 0.55f }, { c.x + s * 0.14f, c.y + s * 0.55f } } };

        juce::Path p;
        for (int q = 0; q < 2; ++q)
        {
            p.startNewSubPath (lerp (play[q][0], pause[q][0]));
            for (int i = 1; i < 4; ++i) p.lineTo (lerp (play[q][i], pause[q][i]));
            p.closeSubPath();
        }
        g.setColour (juce::Colours::white);
        g.fillPath (p.createPathWithRoundedCorners (1.2f));
    }

private:
    Spring morph { 0.0f, 520.0f, 36.0f };
};

// iOS-style segmented control with a sliding selection pill.
class Segmented : public juce::Component, public juce::SettableTooltipClient, private Animated
{
public:
    std::function<void (int)> onChange;

    explicit Segmented (juce::StringArray items) : labels (std::move (items)) {}

    void setSelected (int i, bool animate = true)
    {
        selected = juce::jlimit (0, labels.size() - 1, i);
        if (! animate || ! hasLaidOut) { pos.snap ((float) selected); }
        else pos.target = (float) selected;
        repaint();
    }
    int getSelected() const { return selected; }

    void tick (float dt) override
    {
        hoverIdx = isMouseOver() ? indexAt (getMouseXYRelative().x) : -1;
        hoverA.target = hoverIdx >= 0 ? 1.0f : 0.0f;
        bool m = pos.step (dt);
        m |= hoverA.step (dt);
        if (m) repaint();
    }

    void paint (juce::Graphics& g) override
    {
        hasLaidOut = true;
        const auto& pal = theme::palette();
        auto b = getLocalBounds().toFloat();
        const float r = b.getHeight() * 0.5f;
        g.setColour (pal.raised);
        g.fillRoundedRectangle (b, r);

        const float segW = b.getWidth() / (float) labels.size();
        auto pill = juce::Rectangle<float> (b.getX() + pos.value * segW, b.getY(), segW, b.getHeight()).reduced (2.5f);
        g.setColour (juce::Colours::black.withAlpha (pal.dark ? 0.35f : 0.10f));
        g.fillRoundedRectangle (pill.translated (0, 1.0f), pill.getHeight() * 0.5f);
        g.setColour (pal.dark ? juce::Colour (0xff4a4a54) : juce::Colours::white);
        g.fillRoundedRectangle (pill, pill.getHeight() * 0.5f);

        g.setFont (theme::font (13.0f, theme::Weight::Semibold));
        for (int i = 0; i < labels.size(); ++i)
        {
            const float closeness = 1.0f - juce::jlimit (0.0f, 1.0f, std::abs (pos.value - (float) i));
            auto c = pal.textDim.interpolatedWith (pal.text, std::max (closeness, i == hoverIdx ? 0.5f * hoverA.value : 0.0f));
            g.setColour (c);
            g.drawText (labels[i], juce::Rectangle<float> (b.getX() + (float) i * segW, b.getY(), segW, b.getHeight()),
                        juce::Justification::centred, false);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int i = indexAt (e.x);
        if (i >= 0 && i != selected)
        {
            setSelected (i);
            if (onChange) onChange (i);
        }
    }

private:
    int indexAt (int x) const
    {
        if (labels.isEmpty() || getWidth() <= 0) return -1;
        return juce::jlimit (0, labels.size() - 1, x * labels.size() / getWidth());
    }

    juce::StringArray labels;
    int selected = 0, hoverIdx = -1;
    bool hasLaidOut = false;
    Spring pos { 0.0f, 420.0f, 34.0f }, hoverA { 0.0f };
};

// Speed: drag sideways or scroll; double-click for 100%.
class SpeedControl : public juce::Component, public juce::SettableTooltipClient, private Animated
{
public:
    std::function<void (double)> onChange;

    SpeedControl() { setTooltip ("Speed: drag or scroll, double-click for 100%"); setMouseCursor (juce::MouseCursor::LeftRightResizeCursor); }

    void setSpeed (double s)
    {
        if (std::abs (s - speed) < 1e-6) return;
        speed = s;
        fill.target = (float) normalised (s);
        repaint();
    }

    void tick (float dt) override
    {
        hover.target = isMouseOver() || isMouseButtonDown() ? 1.0f : 0.0f;
        bool m = fill.step (dt);
        m |= hover.step (dt);
        m |= pop.step (dt);
        if (m) repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto& pal = theme::palette();
        auto b = getLocalBounds().toFloat();
        const float r = b.getHeight() * 0.5f;
        g.setColour (pal.raised.interpolatedWith (pal.raisedHover, hover.value));
        g.fillRoundedRectangle (b, r);

        // Fill shows speed between 25% and 150%.
        g.saveState();
        juce::Path clip;
        clip.addRoundedRectangle (b, r);
        g.reduceClipRegion (clip);
        g.setColour (pal.accent.withAlpha (0.22f + 0.1f * hover.value));
        g.fillRect (b.withWidth (b.getWidth() * juce::jlimit (0.0f, 1.0f, fill.value)));
        g.restoreState();

        const float s = 1.0f + 0.12f * pop.value;
        g.setColour (pal.text);
        g.setFont (theme::font (13.0f * s, theme::Weight::Semibold));
        g.drawText (juce::String (juce::roundToInt (speed * 100.0)) + "%", b, juce::Justification::centred, false);
    }

    void mouseDown (const juce::MouseEvent&) override { dragStart = speed; }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        set (dragStart + e.getDistanceFromDragStartX() * 0.004);
    }
    void mouseDoubleClick (const juce::MouseEvent&) override { set (1.0); }
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        set (speed + (w.deltaY > 0 ? 0.05 : -0.05));
    }

private:
    static double normalised (double s) { return (s - 0.25) / 1.25; }

    void set (double s)
    {
        s = juce::jlimit (0.1, 2.0, std::round (s * 20.0) / 20.0); // 5% steps
        if (std::abs (s - speed) < 1e-6) return;
        setSpeed (s);
        pop.value = 1.0f;
        pop.target = 0.0f;
        if (onChange) onChange (s);
    }

    double speed = 1.0, dragStart = 1.0;
    Spring fill { (float) normalised (1.0), 300.0f, 30.0f }, hover { 0.0f }, pop { 0.0f, 500.0f, 22.0f };
};

// iOS-like switch.
class Switch : public Pressable
{
public:
    Switch() { onClick = [this] { setToggled (! toggled); if (onToggle) onToggle (toggled); }; }
    std::function<void (bool)> onToggle;

    void paint (juce::Graphics& g) override
    {
        const auto& pal = theme::palette();
        auto b = getLocalBounds().toFloat().withSizeKeepingCentre (40.0f, 24.0f);
        g.setColour (pal.raisedHover.interpolatedWith (theme::good, toggle.value));
        g.fillRoundedRectangle (b, 12.0f);
        const float knobW = 20.0f + 4.0f * press.value;
        const float x = b.getX() + 2.0f + (b.getWidth() - 4.0f - knobW) * toggle.value;
        auto knob = juce::Rectangle<float> (x, b.getY() + 2.0f, knobW, 20.0f);
        g.setColour (juce::Colours::black.withAlpha (0.18f));
        g.fillRoundedRectangle (knob.translated (0, 1.0f), 10.0f);
        g.setColour (juce::Colours::white);
        g.fillRoundedRectangle (knob, 10.0f);
    }
};

// A HUD message that springs in, waits, and fades away.
class Toast : public juce::Component, private Animated
{
public:
    Toast() { setInterceptsMouseClicks (false, false); }

    void show (const juce::String& message, juce::Colour dot = {})
    {
        text = message;
        dotColour = dot;
        age = 0.0f;
        appear.target = 1.0f;
        if (! wasVisible) appear.value = 0.0f;
        wasVisible = true;
        setVisible (true);
        repaint();
    }

    void tick (float dt) override
    {
        if (! wasVisible) return;
        age += dt;
        if (age > 2.2f) appear.target = 0.0f;
        appear.step (dt);
        if (appear.target < 0.5f && appear.value < 0.01f) { wasVisible = false; setVisible (false); }
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const float a = juce::jlimit (0.0f, 1.0f, appear.value);
        const auto font = theme::font (14.0f, theme::Weight::Semibold);
        const float tw = juce::GlyphArrangement::getStringWidth (font, text) + (dotColour.isTransparent() ? 0.0f : 18.0f);
        auto pill = juce::Rectangle<float> (tw + 36.0f, 38.0f).withCentre (getLocalBounds().toFloat().getCentre())
                        .translated (0, (1.0f - a) * -14.0f);
        const float s = 0.92f + 0.08f * a;
        g.addTransform (juce::AffineTransform::scale (s, s, pill.getCentreX(), pill.getCentreY()));
        g.setColour (juce::Colours::black.withAlpha (0.25f * a));
        g.fillRoundedRectangle (pill.translated (0, 3.0f).expanded (2.0f), 21.0f);
        g.setColour (juce::Colour (0xf0222228).withMultipliedAlpha (a));
        g.fillRoundedRectangle (pill, 19.0f);
        g.setColour (juce::Colours::white.withAlpha (0.1f * a));
        g.drawRoundedRectangle (pill.reduced (0.5f), 19.0f, 1.0f);

        auto content = pill.reduced (18.0f, 0);
        if (! dotColour.isTransparent())
        {
            g.setColour (dotColour.withMultipliedAlpha (a));
            g.fillEllipse (content.removeFromLeft (10.0f).withSizeKeepingCentre (8.0f, 8.0f));
            content.removeFromLeft (8.0f);
        }
        g.setColour (juce::Colours::white.withAlpha (a));
        g.setFont (font);
        g.drawText (text, content, juce::Justification::centredLeft, false);
    }

private:
    juce::String text;
    juce::Colour dotColour;
    float age = 0.0f;
    bool wasVisible = false;
    Spring appear { 0.0f, 360.0f, 26.0f };
};

} // namespace fall
