#pragma once

#include "Song.h"

#include <memory>

namespace fall
{

enum class Mode { Freeplay, Learn };

// What learn mode does when the player doesn't play the chord the song is waiting at.
enum class LearnStyle
{
    WaitEveryNote, // hold at every chord until it is played
    MostlyRight,   // skip a chord (as missed) when most of the last ones were played
    WhilePlaying   // play along: keep going while the player plays anything, stop when they stop
};

struct LoopSettings
{
    bool   enabled     = false;
    double a           = 0.0;  // song seconds
    double b           = 0.0;
    bool   progressive = false; // speed up a little after every pass
    double startSpeed  = 0.6;
    double step        = 0.05;
    double targetSpeed = 1.0;
};

// A place in the song that a key on the real piano jumps back to.
struct Marker
{
    double       time         = 0.0;
    int          triggerPitch = -1; // MIDI note on the real piano, -1 = unbound
    juce::String name;
};

// Plays a Song as MIDI and, in learn mode, holds the song at each chord until the player
// has played it. Single-threaded: the owner serialises calls (the audio engine calls
// notePlayed() then render() from the audio callback, and applies UI commands under a lock).
class Player
{
public:
    static constexpr double kMinTime = -4.0; // pre-roll so the first notes can fall in

    void setSong (std::shared_ptr<const Song> song);
    const Song* getSong() const noexcept { return song.get(); }

    void setMode (Mode m);
    Mode getMode() const noexcept { return mode; }

    void setPracticeHands (uint8_t mask); // which hands learn mode waits for
    void setAudibleHands (uint8_t mask);  // which hands are sent to the instrument
    uint8_t getPracticeHands() const noexcept { return practiceHands; }
    uint8_t getAudibleHands() const noexcept { return audibleHands; }

    void   setSpeed (double s);
    double getSpeed() const noexcept { return speed; }

    void setPlaying (bool shouldPlay);
    bool isPlaying() const noexcept { return playing; }

    void   seek (double songTime);
    double getTime() const noexcept { return time; }

    void setLoop (const LoopSettings& l);
    const LoopSettings& getLoop() const noexcept { return loop; }
    int getLoopPass() const noexcept { return loopPass; }

    void setMarkers (std::vector<Marker> m);
    const std::vector<Marker>& getMarkers() const noexcept { return markers; }
    void setJumpLeadIn (double seconds) { jumpLeadIn = seconds; }

    // How early (real seconds) a correct note may be played before its chord reaches the key line.
    void setEarlyWindow (double realSeconds) { earlyWindow = realSeconds; }

    // MostlyRight: a chord you haven't played is skipped (and counted as missed) once the song has
    // waited 0.5 s at it, if 8 of the last 10 chords were played and the one before this was too.
    // WhilePlaying: the song passes a chord (not counted as missed) if something was played within
    // 1.5 s, or since just before the previous chord, otherwise it waits for any key.
    void setLearnStyle (LearnStyle s);
    LearnStyle getLearnStyle() const noexcept { return style; }

    // Something (anything but a jump key) was struck on the real piano. WhilePlaying: resumes a
    // song that is waiting at a chord.
    void heardSomething();

    // Advance by numSamples of real time, writing instrument MIDI at sample offsets.
    void render (juce::MidiBuffer& out, int numSamples, double sampleRate);

    // Notes heard from the real piano at one onset.
    // Returns the marker index if this was a jump-key press (and jumps), otherwise -1.
    int notePlayed (const PitchSet& pitches);

    // Learn mode state.
    bool     isWaiting() const noexcept;
    PitchSet expectedPitches() const noexcept;  // current chord (empty outside learn mode)
    PitchSet satisfiedPitches() const noexcept { return satisfied; }
    double   nextGroupTime() const noexcept;
    int      getGroupsCompleted() const noexcept { return groupsCompleted; }
    int      getGroupsSkipped() const noexcept { return groupsSkipped; }
    int      getGroupsPassed() const noexcept { return groupsPassed; }
    PitchSet lastSkippedPitches() const noexcept { return lastSkipped; }

    // Pitches reserved as jump keys (never count as playing).
    PitchSet jumpKeys() const noexcept;

private:
    void resync();                 // recompute indices after a seek / song / hand change
    void silenceAll (juce::MidiBuffer& out, int sampleOffset);
    void sendControllerStateAt (juce::MidiBuffer& out, int sampleOffset, double t);
    void emitRange (juce::MidiBuffer& out, double from, double to, double startSample, double samplesPerSongSecond);
    bool gateOpen() const noexcept;
    void decided (bool played);    // record a chord as played or skipped, and move past it
    void skipIfMostlyRight();
    bool mayPassWhilePlaying() const noexcept;
    double endTime() const noexcept;

    std::shared_ptr<const Song> song;
    std::vector<NoteGroup> groups;
    std::vector<Marker> markers;

    Mode    mode          = Mode::Freeplay;
    uint8_t practiceHands = HandBoth;
    uint8_t audibleHands  = HandBoth;
    double  speed         = 1.0;
    bool    playing       = false;
    double  time          = 0.0;
    double  earlyWindow   = 0.25;
    double  jumpLeadIn    = 1.0;

    LoopSettings loop;
    int          loopPass = 0;

    size_t nextNote    = 0; // next note-on to emit
    size_t nextControl = 0;
    size_t nextGroup   = 0; // next chord learn mode waits for
    PitchSet satisfied;
    int groupsCompleted = 0;

    LearnStyle style = LearnStyle::WaitEveryNote;
    unsigned history   = 0; // last 10 decided chords, newest in bit 0, 1 = played
    double   waited    = 0.0; // real seconds spent at the current chord
    int      groupsSkipped = 0;
    PitchSet lastSkipped;

    // WhilePlaying: real-time clock and when we last heard a key / passed a chord on it.
    static constexpr double kNever = -1e9;
    double clock      = 0.0;
    double lastHeard  = kNever;
    double lastPassed = 0.0;
    int    groupsPassed = 0; // chords passed without being played

    std::vector<int> sounding; // indices of notes we've sent note-on for
    int blockSamples = 0;
    bool needsResync = true;   // send all-notes-off + pedal state on next render
};

} // namespace fall
