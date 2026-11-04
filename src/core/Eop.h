#pragma once

#include "Song.h"

namespace fall
{

// Everyone Piano scores (.eop), versions 2.00, 2.01 and 3.01.
//
// The file's layout must make sense (signature, version, key map, event table), but the notes are
// read forgivingly: keys with no note mapped, events of unknown kinds or with broken timestamps and
// note-offs with no note-on are skipped, and notes still held at the end are released at the last
// event. What was skipped is described in `warnings`.
//
// Only version 2.00 records which hand plays each key. Other versions are split at
// ImportOptions::splitPitch like a one-track MIDI file. EOP has no pedal events.
bool loadEopFile (const juce::File& file, Song& out, juce::String& error,
                  const ImportOptions& = {}, juce::StringArray* warnings = nullptr);

bool songFromEop (const void* data, size_t size, Song& out, juce::String& error,
                  const ImportOptions& = {}, juce::StringArray* warnings = nullptr);

} // namespace fall
