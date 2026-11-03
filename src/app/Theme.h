#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <cmath>

namespace fall
{

// ---------------------------------------------------------------------------------------------
// Colours & type
// ---------------------------------------------------------------------------------------------
struct Palette
{
    bool dark = true;
    juce::Colour window, bar, raised, raisedHover, text, textDim, textFaint, separator, accent;
};

namespace theme
{
    inline Palette& palette()
    {
        static Palette p;
        return p;
    }

    // Fall is dark-only: it's a stage, and dark keeps the falling notes vivid.
    inline void init()
    {
        auto& p = palette();
        p.dark        = true;
        p.window      = juce::Colour (0xff0c0c10);
        p.bar         = juce::Colour (0xff17171c);
        p.raised      = juce::Colour (0xff26262d);
        p.raisedHover = juce::Colour (0xff31313a);
        p.text        = juce::Colour (0xfff5f5f7);
        p.textDim     = juce::Colour (0xffa1a1aa);
        p.textFaint   = juce::Colour (0xff6b6b75);
        p.separator   = juce::Colour (0x1affffff);
        p.accent      = juce::Colour (0xff0a84ff);
    }

    // The stage (waterfall) is always dark: it's a light show, and it keeps contrast high.
    inline const juce::Colour stageTop    { 0xff07070b };
    inline const juce::Colour stageBottom { 0xff111119 };
    // Key lanes in the waterfall: white-key boundaries, C boundaries, and the black-key lane fill.
    inline const juce::Colour laneLine      { 0x0affffff };
    inline const juce::Colour laneLineC     { 0x14ffffff };
    inline const juce::Colour laneBlackFill { 0x06ffffff };
    // Pastel teal (right hand) and pastel purple (left hand), matching the app icon. "Heard" and
    // "accepted" stay clear of both: soft pink and soft lime.
    inline const juce::Colour rightHand   { 0xff7ee0d2 };
    inline const juce::Colour leftHand    { 0xffb9a5ff };
    inline const juce::Colour heard       { 0xffff9cc8 };
    inline const juce::Colour good        { 0xffa8e86c };
    inline const juce::Colour missed      { 0xffff453a }; // a chord that was skipped
    inline const juce::Colour loopTint    { 0xffffd60a };

    inline juce::Colour handColour (bool right) { return right ? rightHand : leftHand; }

    enum class Weight { Regular, Medium, Semibold, Bold };

    inline juce::Font font (float height, Weight w = Weight::Regular)
    {
       #if JUCE_WINDOWS
        const char* name = w == Weight::Semibold ? "Segoe UI Semibold"
                         : w == Weight::Medium   ? "Segoe UI Semibold"
                                                 : "Segoe UI";
        auto opts = juce::FontOptions (name, height, w == Weight::Bold ? juce::Font::bold : juce::Font::plain);
       #else
        auto opts = juce::FontOptions (height, w >= Weight::Semibold ? juce::Font::bold : juce::Font::plain);
       #endif
        return juce::Font (opts);
    }

    inline juce::Font mono (float height)
    {
        return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), height, juce::Font::plain));
    }
} // namespace theme

// ---------------------------------------------------------------------------------------------
// Motion
// ---------------------------------------------------------------------------------------------

// A damped spring: the workhorse of every micro-interaction. Slightly under-damped by default
// so things settle with a hint of life rather than a mechanical ease.
struct Spring
{
    float value = 0.0f, target = 0.0f, velocity = 0.0f;
    float stiffness = 380.0f, damping = 30.0f;

    Spring() = default;
    Spring (float v, float k = 380.0f, float d = 30.0f) : value (v), target (v), stiffness (k), damping (d) {}

    void snap (float v) { value = target = v; velocity = 0.0f; }

    // Returns true while still moving.
    bool step (float dt)
    {
        dt = juce::jlimit (0.0f, 0.05f, dt);
        constexpr int sub = 4;
        const float h = dt / sub;
        for (int i = 0; i < sub; ++i)
        {
            const float a = stiffness * (target - value) - damping * velocity;
            velocity += a * h;
            value += velocity * h;
        }
        if (std::abs (target - value) < 0.0005f && std::abs (velocity) < 0.005f)
        {
            value = target;
            velocity = 0.0f;
            return false;
        }
        return true;
    }
};

// Exponential approach with separate rise/fall times (seconds): good for glows and key presses.
inline float approach (float current, float target, float dt, float riseTime, float fallTime)
{
    const float tau = target > current ? riseTime : fallTime;
    if (tau <= 0.0f) return target;
    return target + (current - target) * std::exp (-dt / tau);
}

// ---------------------------------------------------------------------------------------------
// Piano geometry
// ---------------------------------------------------------------------------------------------
inline bool isBlackKey (int pitch)
{
    const int pc = pitch % 12;
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}

inline juce::String noteName (int pitch, bool withOctave = true)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    juce::String s (names[pitch % 12]);
    if (withOctave) s << (pitch / 12 - 1);
    return s;
}

class KeyboardLayout
{
public:
    void setBounds (float x, float width, int lowest = 21, int highest = 108)
    {
        x0 = x; lo = lowest; hi = highest;
        int whites = 0;
        for (int p = lo; p <= hi; ++p) whites += ! isBlackKey (p);
        whiteW = width / (float) juce::jmax (1, whites);
        blackW = whiteW * 0.58f;

        int w = 0;
        for (int p = 0; p < 128; ++p)
        {
            if (p < lo || p > hi) { xs[(size_t) p] = -1; continue; }
            if (! isBlackKey (p)) { xs[(size_t) p] = x0 + (float) w * whiteW; ++w; }
            else
            {
                // Black keys sit on the boundary between white keys, nudged like a real keyboard.
                static const float nudge[12] = { 0, -0.12f, 0, 0.12f, 0, 0, -0.14f, 0, 0, 0, 0.14f, 0 };
                const float boundary = x0 + (float) w * whiteW;
                xs[(size_t) p] = boundary - blackW * 0.5f + nudge[p % 12] * whiteW;
            }
        }
    }

    float keyX (int p) const { return xs[(size_t) p]; }
    float keyW (int p) const { return isBlackKey (p) ? blackW : whiteW; }
    float whiteWidth() const { return whiteW; }
    int lowest() const { return lo; }
    int highest() const { return hi; }
    bool contains (int p) const { return p >= lo && p <= hi; }

    // Black keys win when the point is in their (shorter) area.
    int pitchAt (float x, float yFraction) const
    {
        if (yFraction < 0.62f)
            for (int p = lo; p <= hi; ++p)
                if (isBlackKey (p) && x >= xs[(size_t) p] && x < xs[(size_t) p] + blackW)
                    return p;
        for (int p = lo; p <= hi; ++p)
            if (! isBlackKey (p) && x >= xs[(size_t) p] && x < xs[(size_t) p] + whiteW)
                return p;
        return -1;
    }

private:
    std::array<float, 128> xs {};
    float x0 = 0, whiteW = 10, blackW = 6;
    int lo = 21, hi = 108;
};

inline juce::String formatTime (double seconds)
{
    if (seconds < 0) seconds = 0;
    const int s = (int) seconds;
    return juce::String (s / 60) + ":" + juce::String (s % 60).paddedLeft ('0', 2);
}

} // namespace fall
