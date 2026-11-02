#pragma once

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>

#include <bitset>
#include <cstdint>
#include <vector>

namespace fall
{

using PitchSet = std::bitset<128>;

constexpr int kLowestPianoKey  = 21;  // A0
constexpr int kHighestPianoKey = 108; // C8

enum class Hand : uint8_t { Left = 0, Right = 1 };

enum HandMask : uint8_t { HandNone = 0, HandLeft = 1, HandRight = 2, HandBoth = 3 };

inline bool handIn (Hand h, uint8_t mask) noexcept
{
    return (mask & (h == Hand::Left ? HandLeft : HandRight)) != 0;
}

struct Note
{
    int    pitch    = 60;
    double start    = 0.0;  // seconds of song time
    double end      = 0.0;
    float  velocity = 0.8f; // 0..1
    Hand   hand     = Hand::Right;
    int    track    = 0;
    int    channel  = 1;    // 1..16
};

// Pedals and other controllers forwarded to the instrument.
struct ControlEvent
{
    double time       = 0.0;
    int    channel    = 1;
    int    controller = 64;
    int    value      = 0;
};

struct Song
{
    juce::String              title;
    std::vector<Note>         notes;    // sorted by start, then pitch
    std::vector<ControlEvent> controls; // sorted by time
    std::vector<juce::String> trackNames;
    double length         = 0.0;        // end of the last note
    int    numNoteTracks  = 0;
    bool   handsFromTracks = false;     // true when hands came from track/channel layout
    int    lowestPitch    = kLowestPianoKey;
    int    highestPitch   = kHighestPianoKey;

    double firstNoteTime() const noexcept { return notes.empty() ? 0.0 : notes.front().start; }

    // Index of the first note whose start is >= t (notes.size() if none).
    size_t firstNoteAtOrAfter (double t) const noexcept;
};

struct ImportOptions
{
    int  splitPitch  = 60;   // used when the file doesn't separate hands
    bool preferTracks = true;
};

bool loadMidiFile (const juce::File& file, Song& out, juce::String& error, const ImportOptions& = {});
bool songFromMidi (const juce::MidiFile& midi, Song& out, juce::String& error, const ImportOptions& = {});

// (Re)assign hands. Uses track names / two-track layout when possible, otherwise a split point.
void assignHands (Song& song, const ImportOptions& opts);

// A chord the player must play in learn mode: notes of the practised hands starting together.
struct NoteGroup
{
    double           time = 0.0; // start of the earliest note in the group
    std::vector<int> notes;      // indices into Song::notes
    PitchSet         pitches;
};

std::vector<NoteGroup> buildGroups (const Song& song, uint8_t handMask, double chordWindow = 0.05);

} // namespace fall
