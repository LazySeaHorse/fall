// Maps the include path JUCE expects onto FST, a GPLv3 reverse-engineered VST2 header.
//
// FST declares two structs with flexible array members, but JUCE relies on the fixed sizes
// from the original SDK:
//  * MidiKeyName is created on the stack (MSVC rejects zero-sized arrays there: C2466).
//  * VstSpeakerArrangement is allocated as sizeof(struct) + extra speakers beyond 8, so a
//    zero-length array would make JUCE write past the end of its buffer.
// We rename FST's versions out of the way and declare the SDK-sized layouts instead.
#pragma once

#define MidiKeyName           FallFstMidiKeyNameFlexible
#define VstSpeakerArrangement FallFstSpeakerArrangementFlexible
#include <fst/aeffect.h>
#undef MidiKeyName
#undef VstSpeakerArrangement

struct MidiKeyName
{
    t_fstInt32 thisProgramIndex;
    t_fstInt32 thisKeyNumber;
    char       keyName[64]; // kVstMaxNameLen
    t_fstInt32 reserved;
    t_fstInt32 flags;
};

struct VstSpeakerArrangement
{
    int                    type;
    int                    numChannels;
    t_fstSpeakerProperties speakers[8];
};
