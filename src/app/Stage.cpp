#include "Stage.h"

namespace fall
{

Stage::Stage()
{
    setOpaque (true);
    particles.reserve (512);
}

void Stage::setSong (std::shared_ptr<const Song> s)
{
    song = std::move (s);
    maxNoteLength = 1.0;
    if (song)
        for (const auto& n : song->notes)
            maxNoteLength = std::max (maxNoteLength, n.end - n.start);
    particles.clear();
    ripples.clear();
    lastGroupsCompleted = 0;
}

void Stage::resized()
{
    const auto b = getLocalBounds().toFloat();
    keyboardH = juce::jlimit (90.0f, 190.0f, b.getWidth() / 52.0f * 5.2f);
    keyboardH = std::min (keyboardH, b.getHeight() * 0.32f);
    keylineY  = b.getBottom() - keyboardH;
    layout.setBounds (b.getX(), b.getWidth());
}

void Stage::frame (float dt, const EngineSnapshot& s, double t, const HeardEvent* heard, int numHeard)
{
    snap = s;
    now = t;
    clock += dt;
    pixelsPerSecond = keylineY / lookahead;

    // Which song notes are sounding right now (and in which hand)?
    prevSounding = sounding;
    sounding.reset();
    if (song)
    {
        const size_t from = song->firstNoteAtOrAfter (t - maxNoteLength);
        for (size_t i = from; i < song->notes.size(); ++i)
        {
            const auto& n = song->notes[i];
            if (n.start > t) break;
            const bool pending = s.waiting && n.start >= t - 1e-6; // learn mode: not played yet
            if (n.end > t && ! pending)
            {
                sounding.set ((size_t) n.pitch);
                soundingRight[(size_t) n.pitch] = n.hand == Hand::Right;
            }
        }
    }

    const bool learn = s.mode == Mode::Learn;
    for (int p = 0; p < 128; ++p)
    {
        auto& k = keys[(size_t) p];
        const bool on = sounding.test ((size_t) p);
        if (on) k.right = soundingRight[(size_t) p];
        k.press    = approach (k.press, on ? 1.0f : 0.0f, dt, 0.025f, 0.11f);
        k.heard    = approach (k.heard, 0.0f, dt, 0.0f, 0.45f);
        k.heardPop = approach (k.heardPop, 0.0f, dt, 0.0f, 0.12f);
        const bool expect = learn && s.expected.test ((size_t) p) && ! s.satisfied.test ((size_t) p)
                            && s.nextGate >= 0 && s.nextGate - t < 0.6 * std::max (0.25, s.speed);
        k.expected = approach (k.expected, expect ? 1.0f : 0.0f, dt, 0.08f, 0.15f);
        k.good     = approach (k.good, (learn && s.satisfied.test ((size_t) p)) ? 1.0f : 0.0f, dt, 0.03f, 0.35f);
        k.missed   = approach (k.missed, 0.0f, dt, 0.0f, 0.5f);

        // New note crossing the key line: a small spray of light.
        if (on && ! prevSounding.test ((size_t) p) && layout.contains (p) && particles.size() < 480)
        {
            const auto r = keyRect (p);
            const auto c = theme::handColour (k.right);
            for (int i = 0; i < 6; ++i)
                particles.push_back ({ r.getCentreX() + (rng.nextFloat() - 0.5f) * r.getWidth() * 0.6f, keylineY - 1.0f,
                                       (rng.nextFloat() - 0.5f) * 60.0f, -40.0f - rng.nextFloat() * 120.0f,
                                       0.0f, 0.35f + rng.nextFloat() * 0.35f, 1.2f + rng.nextFloat() * 1.8f, c });
        }
    }

    for (int i = 0; i < numHeard; ++i)
    {
        for (int p = 0; p < 128; ++p)
        {
            if (! heard[i].pitches.test ((size_t) p)) continue;
            auto& k = keys[(size_t) p];
            k.heard = 1.0f;
            k.heardPop = 1.0f;
        }
    }

    // A chord was completed in learn mode: green ripples on the keys that were played.
    if (s.groupsCompleted > lastGroupsCompleted && lastExpected.any())
        for (int p = 0; p < 128; ++p)
            if (lastExpected.test ((size_t) p) && layout.contains (p))
            {
                const auto r = keyRect (p);
                ripples.push_back ({ r.getCentreX(), keylineY, 0.0f, theme::good });
            }
    lastGroupsCompleted = s.groupsCompleted;

    // A chord was skipped ("keep going if mostly right"): its keys flash red, with red ripples.
    if (s.groupsSkipped > lastGroupsSkipped)
        for (int p = 0; p < 128; ++p)
            if (s.lastSkipped.test ((size_t) p) && layout.contains (p))
            {
                keys[(size_t) p].missed = 1.0f;
                const auto r = keyRect (p);
                ripples.push_back ({ r.getCentreX(), keylineY, 0.0f, theme::missed });
            }
    lastGroupsSkipped = s.groupsSkipped;
    lastExpected = s.expected;

    for (auto& pt : particles)
    {
        pt.life += dt;
        pt.x += pt.vx * dt;
        pt.y += pt.vy * dt;
        pt.vy += 90.0f * dt; // a little gravity
        pt.vx *= std::exp (-2.0f * dt);
    }
    particles.erase (std::remove_if (particles.begin(), particles.end(), [] (const Particle& p) { return p.life >= p.maxLife; }),
                     particles.end());
    for (auto& r : ripples) r.age += dt;
    ripples.erase (std::remove_if (ripples.begin(), ripples.end(), [] (const Ripple& r) { return r.age > 0.55f; }), ripples.end());

    waitingFor = s.waiting ? waitingFor + dt : 0.0f;
    chipAlpha.target = waitingFor > 0.7f ? 1.0f : 0.0f;
    chipAlpha.step (dt);

    repaint();
}

juce::Rectangle<float> Stage::keyRect (int p) const
{
    const float x = layout.keyX (p), w = layout.keyW (p);
    const float h = isBlackKey (p) ? keyboardH * 0.62f : keyboardH;
    return { x, keylineY, w, h };
}

void Stage::paint (juce::Graphics& g)
{
    paintBackground (g);
    if (song)
    {
        paintNotes (g);
        paintKeylineGlow (g);
    }
    paintKeyboard (g);
    paintEffects (g);
    paintWaitingChip (g);
}

void Stage::paintBackground (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    g.setGradientFill (juce::ColourGradient (theme::stageTop, 0, 0, theme::stageBottom, 0, keylineY, false));
    g.fillRect (b.withBottom (keylineY));

    // Faint grooves each key's notes fall down: a line at every white-key boundary (stronger at
    // each C) and a slightly lighter lane, edged by two lines, behind every black key.
    for (int p = layout.lowest(); p <= layout.highest(); ++p)
    {
        if (isBlackKey (p))
        {
            g.setColour (theme::laneBlackFill);
            g.fillRect (layout.keyX (p), 0.0f, layout.keyW (p), keylineY);
        }
    }
    for (int p = layout.lowest(); p <= layout.highest(); ++p)
    {
        const float x = layout.keyX (p), w = layout.keyW (p);
        if (isBlackKey (p))
        {
            g.setColour (theme::laneLine);
            g.fillRect (x - 0.5f, 0.0f, 1.0f, keylineY);
            g.fillRect (x + w - 0.5f, 0.0f, 1.0f, keylineY);
        }
        else
        {
            // A black key just left of this white one already draws its own edge over this boundary.
            if (p > layout.lowest() && isBlackKey (p - 1)) continue;
            g.setColour (p % 12 == 0 ? theme::laneLineC : theme::laneLine);
            g.fillRect (x - 0.5f, 0.0f, 1.0f, keylineY);
        }
    }

    // Loop region: brackets drawn as the band it occupies in the waterfall.
    if (snap.loop.enabled)
    {
        const float ya = yForTime (snap.loop.a), yb = yForTime (snap.loop.b);
        const float top = std::max (0.0f, yb), bottom = std::min (keylineY, ya);
        if (bottom > top)
        {
            g.setColour (theme::loopTint.withAlpha (0.035f));
            g.fillRect (0.0f, top, b.getWidth(), bottom - top);
        }
        g.setColour (theme::loopTint.withAlpha (0.35f));
        if (yb > 0 && yb < keylineY) g.fillRect (0.0f, yb, b.getWidth(), 1.0f);
        if (ya > 0 && ya < keylineY) g.fillRect (0.0f, ya - 1.0f, b.getWidth(), 1.0f);
    }
}

void Stage::paintNotes (juce::Graphics& g)
{
    const bool learn = snap.mode == Mode::Learn;
    const double visibleUntil = now + keylineY / pixelsPerSecond + 0.1;
    const float pulse = 0.5f + 0.5f * std::sin (clock * juce::MathConstants<float>::twoPi * 1.3f);

    const size_t from = song->firstNoteAtOrAfter (now - maxNoteLength);
    const auto nameFont = theme::font (11.0f, theme::Weight::Semibold);
    g.setFont (nameFont);

    for (size_t i = from; i < song->notes.size(); ++i)
    {
        const auto& n = song->notes[i];
        if (n.start > visibleUntil) break;
        if (n.end <= now || ! layout.contains (n.pitch)) continue;

        const bool black = isBlackKey (n.pitch);
        const float inset = black ? 1.0f : 1.6f;
        const float x = layout.keyX (n.pitch) + inset;
        const float w = layout.keyW (n.pitch) - inset * 2.0f;
        const float yTop = yForTime (n.end);
        const float yBottom = std::min (keylineY, yForTime (n.start));
        if (yBottom - yTop < 1.0f) continue;

        const bool right = n.hand == Hand::Right;
        auto base = theme::handColour (right);
        if (black) base = base.darker (0.35f);

        // The hand that isn't your part fades back.
        const float alpha = handIn (n.hand, snap.practiceHands) ? 1.0f : 0.42f;

        const bool active = n.start <= now && ! (snap.waiting && n.start >= now - 1e-6);
        const bool isExpected = learn && snap.waiting && std::abs (n.start - snap.nextGate) < 0.06
                                && snap.expected.test ((size_t) n.pitch);
        const bool isDone = isExpected && snap.satisfied.test ((size_t) n.pitch);

        auto r = juce::Rectangle<float> (x, yTop, w, yBottom - yTop);
        const float radius = std::min ({ w * 0.32f, 6.0f, r.getHeight() * 0.5f });

        if (active || isExpected)
        {
            // Soft halo behind notes that are sounding (or waiting for you).
            const float glowA = isExpected ? 0.18f + 0.22f * pulse : 0.28f;
            g.setColour ((isDone ? theme::good : base).withAlpha (glowA * 0.5f));
            g.fillRoundedRectangle (r.expanded (4.0f), radius + 4.0f);
            g.setColour ((isDone ? theme::good : base).withAlpha (glowA));
            g.fillRoundedRectangle (r.expanded (2.0f), radius + 2.0f);
        }

        auto fill = isDone ? theme::good : base;
        if (active) fill = fill.brighter (0.25f);
        g.setGradientFill (juce::ColourGradient (fill.brighter (0.18f).withAlpha (alpha), x, yTop,
                                                 fill.withAlpha (alpha), x, yBottom, false));
        g.fillRoundedRectangle (r, radius);

        // A crisp top highlight gives the note a bit of glassy depth.
        g.setColour (juce::Colours::white.withAlpha (0.22f * alpha));
        g.drawRoundedRectangle (r.reduced (0.5f), radius, 1.0f);

        if (isExpected && ! isDone)
        {
            g.setColour (juce::Colours::white.withAlpha (0.35f + 0.45f * pulse));
            g.drawRoundedRectangle (r.expanded (1.0f), radius + 1.0f, 1.5f);
        }

        if (showNames && r.getHeight() >= 16.0f && w >= 11.0f)
        {
            const auto label = w >= 22.0f ? noteName (n.pitch) : noteName (n.pitch, false).substring (0, 1);
            g.setColour (juce::Colours::black.withAlpha (0.55f * alpha));
            g.drawText (label, r.withTrimmedBottom (3.0f).toNearestInt(), juce::Justification::centredBottom, false);
        }
    }
}

void Stage::paintKeylineGlow (juce::Graphics& g)
{
    const float w = (float) getWidth();
    g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.0f), 0, keylineY - 50.0f,
                                             juce::Colours::white.withAlpha (0.05f), 0, keylineY, false));
    g.fillRect (0.0f, keylineY - 50.0f, w, 50.0f);

    for (int p = layout.lowest(); p <= layout.highest(); ++p)
    {
        const auto& k = keys[(size_t) p];
        const float a = std::max (k.press, k.expected * 0.6f);
        if (a < 0.02f) continue;
        const auto c = k.expected > k.press ? theme::handColour (k.right).withMultipliedSaturation (0.6f)
                                            : theme::handColour (k.right);
        const auto r = keyRect (p);
        const float h = 70.0f * a;
        g.setGradientFill (juce::ColourGradient (c.withAlpha (0.0f), 0, keylineY - h, c.withAlpha (0.42f * a), 0, keylineY, false));
        g.fillRect (r.getX() - 2.0f, keylineY - h, r.getWidth() + 4.0f, h);
    }

    g.setColour (juce::Colours::white.withAlpha (0.28f));
    g.fillRect (0.0f, keylineY - 1.0f, w, 1.0f);
}

void Stage::paintKeyboard (juce::Graphics& g)
{
    const float bottom = keylineY + keyboardH;
    g.setColour (juce::Colour (0xff050507));
    g.fillRect (0.0f, keylineY, (float) getWidth(), keyboardH);

    const float pulse = 0.5f + 0.5f * std::sin (clock * juce::MathConstants<float>::twoPi * 1.3f);

    auto keyFill = [&] (int p, juce::Colour rest) {
        const auto& k = keys[(size_t) p];
        auto c = rest;
        c = c.interpolatedWith (theme::handColour (k.right).withMultipliedSaturation (0.75f), k.expected * (0.35f + 0.25f * pulse));
        c = c.interpolatedWith (theme::handColour (k.right), k.press * 0.85f);
        c = c.interpolatedWith (theme::good, k.good * 0.8f);
        c = c.interpolatedWith (theme::missed, k.missed * 0.85f);
        return c;
    };

    // White keys
    const auto labelFont = theme::font (std::min (11.0f, layout.whiteWidth() * 0.45f), theme::Weight::Medium);
    for (int p = layout.lowest(); p <= layout.highest(); ++p)
    {
        if (isBlackKey (p)) continue;
        const auto& k = keys[(size_t) p];
        auto r = keyRect (p).reduced (0.6f, 0.0f).withTrimmedTop (k.press * 2.0f);
        const auto top = keyFill (p, juce::Colour (0xfff7f7f9));
        const auto bot = keyFill (p, juce::Colour (0xffd9d9df));
        g.setGradientFill (juce::ColourGradient (top, 0, r.getY(), bot, 0, r.getBottom(), false));
        juce::Path path;
        path.addRoundedRectangle (r.getX(), r.getY(), r.getWidth(), r.getHeight(), 3.5f, 3.5f, false, false, true, true);
        g.fillPath (path);

        if (p % 12 == 0 && labelFont.getHeight() >= 7.0f)
        {
            g.setColour (juce::Colours::black.withAlpha (0.32f));
            g.setFont (labelFont);
            g.drawText (noteName (p), r.withTrimmedBottom (6.0f).toNearestInt(), juce::Justification::centredBottom, false);
        }
    }

    // Black keys
    for (int p = layout.lowest(); p <= layout.highest(); ++p)
    {
        if (! isBlackKey (p)) continue;
        const auto& k = keys[(size_t) p];
        auto r = keyRect (p).withTrimmedTop (k.press * 1.5f);
        const auto top = keyFill (p, juce::Colour (0xff2c2c33));
        const auto bot = keyFill (p, juce::Colour (0xff0e0e12));
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRoundedRectangle (r.translated (1.0f, 1.5f), 2.5f);
        g.setGradientFill (juce::ColourGradient (top, 0, r.getY(), bot, 0, r.getBottom(), false));
        g.fillRoundedRectangle (r, 2.5f);
        g.setColour (juce::Colours::white.withAlpha (0.10f + 0.1f * k.press));
        g.fillRoundedRectangle (r.reduced (r.getWidth() * 0.18f, 0).withTrimmedTop (r.getHeight() - 7.0f).withHeight (3.0f), 1.5f);
    }

    // Lip shadow under the key line, so keys look like they slide under it.
    g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.45f), 0, keylineY,
                                             juce::Colours::transparentBlack, 0, keylineY + 10.0f, false));
    g.fillRect (0.0f, keylineY, (float) getWidth(), 10.0f);
    g.setColour (juce::Colours::black);
    g.fillRect (0.0f, bottom - 1.0f, (float) getWidth(), 1.0f);

    // What the microphone heard: a dot on each key.
    for (int p = layout.lowest(); p <= layout.highest(); ++p)
    {
        const auto& k = keys[(size_t) p];
        if (k.heard < 0.02f) continue;
        const auto r = keyRect (p);
        const float radius = std::max (3.0f, r.getWidth() * 0.22f) * (1.0f + 0.5f * k.heardPop);
        const float cy = r.getBottom() - std::max (10.0f, r.getWidth() * 0.55f);
        g.setColour (theme::heard.withAlpha (0.25f * k.heard));
        g.fillEllipse (r.getCentreX() - radius * 2.0f, cy - radius * 2.0f, radius * 4.0f, radius * 4.0f);
        g.setColour (theme::heard.withAlpha (k.heard));
        g.fillEllipse (r.getCentreX() - radius, cy - radius, radius * 2.0f, radius * 2.0f);
    }
}

void Stage::paintEffects (juce::Graphics& g)
{
    for (const auto& p : particles)
    {
        const float a = 1.0f - p.life / p.maxLife;
        g.setColour (p.c.brighter (0.4f).withAlpha (a * 0.9f));
        g.fillEllipse (p.x - p.size, p.y - p.size, p.size * 2.0f, p.size * 2.0f);
    }
    for (const auto& r : ripples)
    {
        const float t = r.age / 0.55f;
        const float rad = 6.0f + 26.0f * (1.0f - std::pow (1.0f - t, 3.0f));
        g.setColour (r.c.withAlpha (0.8f * (1.0f - t)));
        g.drawEllipse (r.x - rad, r.y - rad, rad * 2.0f, rad * 2.0f, 2.0f * (1.0f - t) + 0.5f);
    }
}

void Stage::paintWaitingChip (juce::Graphics& g)
{
    const float a = juce::jlimit (0.0f, 1.0f, chipAlpha.value);
    if (a < 0.01f || ! snap.expected.any()) return;

    juce::String text ("Play ");
    for (int p = 0; p < 128; ++p)
        if (snap.expected.test ((size_t) p) && ! snap.satisfied.test ((size_t) p))
            text << noteName (p) << "  ";
    text = text.trimEnd();

    const auto font = theme::font (15.0f, theme::Weight::Semibold);
    const float tw = juce::GlyphArrangement::getStringWidth (font, text);
    auto chip = juce::Rectangle<float> (tw + 36.0f, 34.0f)
                    .withCentre ({ (float) getWidth() * 0.5f, keylineY - 58.0f + (1.0f - a) * 10.0f });
    g.setColour (juce::Colour (0xff1c1c22).withAlpha (0.92f * a));
    g.fillRoundedRectangle (chip, chip.getHeight() * 0.5f);
    g.setColour (juce::Colours::white.withAlpha (0.12f * a));
    g.drawRoundedRectangle (chip.reduced (0.5f), chip.getHeight() * 0.5f, 1.0f);
    g.setColour (juce::Colours::white.withAlpha (a));
    g.setFont (font);
    g.drawText (text, chip, juce::Justification::centred, false);
}

int Stage::pitchAt (juce::Point<float> p) const
{
    if (p.y < keylineY) return -1;
    return layout.pitchAt (p.x, (p.y - keylineY) / keyboardH);
}

void Stage::mouseDown (const juce::MouseEvent& e)
{
    if (song == nullptr && e.position.y < keylineY && onEmptyClick) { onEmptyClick(); return; }
    mousePitch = pitchAt (e.position);
    if (mousePitch >= 0 && onKey) onKey (mousePitch, true);
}

void Stage::mouseDrag (const juce::MouseEvent& e)
{
    const int p = pitchAt (e.position);
    if (p == mousePitch) return;
    if (mousePitch >= 0 && onKey) onKey (mousePitch, false);
    mousePitch = p;
    if (mousePitch >= 0 && onKey) onKey (mousePitch, true);
}

void Stage::mouseUp (const juce::MouseEvent&)
{
    if (mousePitch >= 0 && onKey) onKey (mousePitch, false);
    mousePitch = -1;
}

void Stage::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (e.mods.isCtrlDown() || e.mods.isCommandDown())
    {
        if (onZoom) onZoom (w.deltaY > 0 ? 0.9 : 1.1);
        return;
    }
    if (onScrub) onScrub (w.deltaY * lookahead * 0.5);
}

} // namespace fall
