#include "Song.h"

#include <algorithm>
#include <map>
#include <numeric>
#include <set>

namespace fall
{

size_t Song::firstNoteAtOrAfter (double t) const noexcept
{
    auto it = std::lower_bound (notes.begin(), notes.end(), t,
                                [] (const Note& n, double v) { return n.start < v; });
    return (size_t) std::distance (notes.begin(), it);
}

bool loadMidiFile (const juce::File& file, Song& out, juce::String& error, const ImportOptions& opts)
{
    juce::FileInputStream in (file);
    if (! in.openedOk())
    {
        error = "Couldn't open " + file.getFileName();
        return false;
    }

    juce::MidiFile midi;
    if (! midi.readFrom (in, true))
    {
        error = file.getFileName() + " isn't a valid MIDI file";
        return false;
    }

    if (! songFromMidi (midi, out, error, opts))
        return false;

    out.title = file.getFileNameWithoutExtension();
    return true;
}

bool songFromMidi (const juce::MidiFile& source, Song& out, juce::String& error, const ImportOptions& opts)
{
    juce::MidiFile midi (source);
    if (midi.getTimeFormat() <= 0)
    {
        error = "SMPTE-timed MIDI files aren't supported yet";
        return false;
    }
    midi.convertTimestampTicksToSeconds();

    Song song;
    std::vector<int> notesPerTrack ((size_t) midi.getNumTracks(), 0);
    song.trackNames.resize ((size_t) midi.getNumTracks());

    for (int t = 0; t < midi.getNumTracks(); ++t)
    {
        juce::MidiMessageSequence seq (*midi.getTrack (t));
        seq.updateMatchedPairs();

        for (auto* ev : seq)
        {
            const auto& msg = ev->message;

            if (msg.isTrackNameEvent() && song.trackNames[(size_t) t].isEmpty())
                song.trackNames[(size_t) t] = msg.getTextFromTextMetaEvent().trim();

            if (msg.isNoteOn() && msg.getChannel() != 10) // skip GM drums
            {
                Note n;
                n.pitch    = msg.getNoteNumber();
                n.start    = msg.getTimeStamp();
                n.end      = ev->noteOffObject != nullptr ? ev->noteOffObject->message.getTimeStamp()
                                                          : n.start + 0.5;
                n.end      = std::max (n.end, n.start + 0.03);
                n.velocity = msg.getFloatVelocity();
                n.track    = t;
                n.channel  = msg.getChannel();
                song.notes.push_back (n);
                ++notesPerTrack[(size_t) t];
            }
            else if (msg.isController())
            {
                const int cc = msg.getControllerNumber();
                if (cc == 64 || cc == 66 || cc == 67) // sustain, sostenuto, soft
                    song.controls.push_back ({ msg.getTimeStamp(), msg.getChannel(), cc, msg.getControllerValue() });
            }
        }
    }

    if (song.notes.empty())
    {
        error = "This MIDI file has no notes";
        return false;
    }

    std::stable_sort (song.notes.begin(), song.notes.end(), [] (const Note& a, const Note& b) {
        return a.start != b.start ? a.start < b.start : a.pitch < b.pitch;
    });
    std::stable_sort (song.controls.begin(), song.controls.end(),
                      [] (const ControlEvent& a, const ControlEvent& b) { return a.time < b.time; });

    song.numNoteTracks = (int) std::count_if (notesPerTrack.begin(), notesPerTrack.end(), [] (int c) { return c > 0; });
    song.lowestPitch   = 127;
    song.highestPitch  = 0;
    for (const auto& n : song.notes)
    {
        song.length       = std::max (song.length, n.end);
        song.lowestPitch  = std::min (song.lowestPitch, n.pitch);
        song.highestPitch = std::max (song.highestPitch, n.pitch);
    }

    assignHands (song, opts);
    out = std::move (song);
    return true;
}

namespace
{
    enum class NameHint { None, Left, Right };

    NameHint hintFromName (const juce::String& raw)
    {
        const auto name = raw.toLowerCase();
        auto hasWord = [&] (const char* w) {
            juce::StringArray tokens;
            tokens.addTokens (name, " _-.:()[]/", "");
            return tokens.contains (w);
        };

        if (name.contains ("left") || name.contains ("l.h") || hasWord ("lh") || name.contains ("bass")
            || name.contains ("links") || name.contains ("gauche") || name.contains ("izq"))
            return NameHint::Left;
        if (name.contains ("right") || name.contains ("r.h") || hasWord ("rh") || name.contains ("treble")
            || name.contains ("melody") || name.contains ("rechts") || name.contains ("droite") || hasWord ("der"))
            return NameHint::Right;
        return NameHint::None;
    }
} // namespace

void assignHands (Song& song, const ImportOptions& opts)
{
    song.handsFromTracks = false;

    if (opts.preferTracks)
    {
        // Group notes into "parts": by track, or by channel when everything lives on one track.
        std::map<int, std::vector<size_t>> parts;
        std::set<int> tracks, channels;
        for (const auto& n : song.notes) { tracks.insert (n.track); channels.insert (n.channel); }
        const bool byChannel = tracks.size() == 1 && channels.size() == 2;

        for (size_t i = 0; i < song.notes.size(); ++i)
            parts[byChannel ? song.notes[i].channel : song.notes[i].track].push_back (i);

        if (parts.size() == 2)
        {
            auto it = parts.begin();
            const int keyA = it->first; const auto& a = it->second; ++it;
            const int keyB = it->first; const auto& b = it->second;

            auto meanPitch = [&] (const std::vector<size_t>& idx) {
                double s = 0; for (auto i : idx) s += song.notes[i].pitch; return s / (double) idx.size();
            };

            NameHint ha = NameHint::None, hb = NameHint::None;
            if (! byChannel)
            {
                if ((size_t) keyA < song.trackNames.size()) ha = hintFromName (song.trackNames[(size_t) keyA]);
                if ((size_t) keyB < song.trackNames.size()) hb = hintFromName (song.trackNames[(size_t) keyB]);
            }

            bool aIsRight = meanPitch (a) >= meanPitch (b);
            if (ha == NameHint::Right || hb == NameHint::Left)  aIsRight = true;
            if (ha == NameHint::Left  || hb == NameHint::Right) aIsRight = false;

            for (auto i : a) song.notes[i].hand = aIsRight ? Hand::Right : Hand::Left;
            for (auto i : b) song.notes[i].hand = aIsRight ? Hand::Left  : Hand::Right;
            song.handsFromTracks = true;
            return;
        }
    }

    for (auto& n : song.notes)
        n.hand = n.pitch < opts.splitPitch ? Hand::Left : Hand::Right;
}

std::vector<NoteGroup> buildGroups (const Song& song, uint8_t handMask, double chordWindow)
{
    std::vector<NoteGroup> groups;
    for (int i = 0; i < (int) song.notes.size(); ++i)
    {
        const auto& n = song.notes[(size_t) i];
        if (! handIn (n.hand, handMask))
            continue;

        if (groups.empty() || n.start - groups.back().time > chordWindow)
            groups.push_back ({ n.start, {}, {} });

        groups.back().notes.push_back (i);
        groups.back().pitches.set ((size_t) n.pitch);
    }
    return groups;
}

} // namespace fall
