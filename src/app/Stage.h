#pragma once

#include "AudioEngine.h"
#include "Theme.h"

namespace fall
{

// The waterfall and the piano it falls onto. Purely a view: MainComponent feeds it a frame at a time.
class Stage : public juce::Component
{
public:
    Stage();

    void setSong (std::shared_ptr<const Song> s);
    void setLookahead (double songSeconds) { lookahead = juce::jlimit (1.0, 12.0, songSeconds); }
    double getLookahead() const noexcept { return lookahead; }
    void setShowNoteNames (bool b) { showNames = b; }

    // Advance animations. `t` is the (interpolated) song time to draw.
    void frame (float dt, const EngineSnapshot& s, double t, const HeardEvent* heard, int numHeard);

    // A key was clicked on the on-screen keyboard.
    std::function<void (int pitch, bool down)> onKey;
    // Mouse wheel over the waterfall: scrub by this many song seconds.
    std::function<void (double seconds)> onScrub;
    std::function<void (double factor)> onZoom;
    std::function<void()> onEmptyClick; // clicked the waterfall with no song loaded

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    void paintBackground (juce::Graphics&);
    void paintNotes (juce::Graphics&);
    void paintKeylineGlow (juce::Graphics&);
    void paintKeyboard (juce::Graphics&);
    void paintEffects (juce::Graphics&);
    void paintWaitingChip (juce::Graphics&);

    juce::Rectangle<float> keyRect (int pitch) const;
    float yForTime (double songTime) const { return keylineY - (float) ((songTime - now) * pixelsPerSecond); }
    int pitchAt (juce::Point<float> p) const;

    std::shared_ptr<const Song> song;
    double maxNoteLength = 4.0;

    KeyboardLayout layout;
    float keylineY = 0, keyboardH = 0;
    double lookahead = 3.2, pixelsPerSecond = 100, now = 0;
    bool showNames = true;
    EngineSnapshot snap;
    float clock = 0;

    struct KeyState
    {
        float press = 0, heard = 0, heardPop = 0, expected = 0, good = 0;
        bool  right = true;
    };
    std::array<KeyState, 128> keys {};
    PitchSet sounding, prevSounding;
    std::array<bool, 128> soundingRight {};

    struct Particle { float x, y, vx, vy, life, maxLife, size; juce::Colour c; };
    std::vector<Particle> particles;
    struct Ripple { float x, y, age; juce::Colour c; };
    std::vector<Ripple> ripples;
    int lastGroupsCompleted = 0;
    PitchSet lastExpected;

    float waitingFor = 0;  // seconds spent waiting at the current chord
    Spring chipAlpha { 0.0f, 260.0f, 32.0f };

    int mousePitch = -1;
    juce::Random rng;
};

} // namespace fall
