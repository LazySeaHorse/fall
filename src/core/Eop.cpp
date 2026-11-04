#include "Eop.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>

// The layout below comes from a clean-room study of Everyone Piano's .eop files (the eop2midi
// converter). The file is XOR-scrambled with a 32-byte mask. After a 0x1B8-byte header comes a key
// map (computer-keyboard scan code -> piano keys) and then 16-byte events: a double timestamp in
// milliseconds, a status byte (0x90 on / 0x80 off), the scan code and a velocity.

namespace fall
{

namespace detail { std::string codePageToUtf8 (const std::string& input, unsigned codePage); }

namespace
{
using Bytes = std::vector<uint8_t>;

constexpr size_t   kHeaderSize    = 0x1B8;
constexpr size_t   kMinimumSize   = 0x1BC;
constexpr size_t   kEventSize     = 16;
constexpr size_t   kV200MapStride = 0x184;
constexpr uint16_t kNoteAction    = 0x0090;
constexpr uint32_t kV200Right     = 1;
constexpr uint32_t kV200Left      = 0x00010001;
constexpr size_t   kMaxFileSize   = 128u * 1024 * 1024;

constexpr std::array<uint8_t, 32> kXorMask = {
    0x71, 0x72, 0x73, 0x74, 0x72, 0x73, 0x74, 0x75,
    0x73, 0x74, 0x75, 0x76, 0x74, 0x75, 0x76, 0x77,
    0x75, 0x76, 0x77, 0x78, 0x76, 0x77, 0x78, 0x79,
    0x77, 0x78, 0x79, 0x7A, 0x78, 0x79, 0x7A, 0x7B,
};

// The layout doesn't make sense: the file can't be read.
struct EopError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

struct Header
{
    uint32_t     version = 0;
    uint32_t     mapBlocks = 0;
    uint32_t     mapContainerSize = 0;
    uint32_t     eventContainerSize = 0;
    juce::String title;
};

struct KeyMapping
{
    bool                 present = false;
    uint32_t             action = 0;
    std::vector<uint8_t> pitches;
};

using KeyMap = std::array<KeyMapping, 256>;

enum class EopHand { None, Left, Right };

struct EopEvent
{
    double  ms = 0;
    bool    on = false;
    uint8_t pitch = 0;
    uint8_t velocity = 0;
    EopHand hand = EopHand::None;
    size_t  serial = 0;
    bool    atEnd = false; // a release we added for a note still held at the end
};

uint16_t readU16 (const Bytes& data, size_t offset)
{
    if (offset > data.size() || data.size() - offset < 2)
        throw EopError ("the file ends in the middle of a 16-bit field");
    return (uint16_t) (data[offset] | (data[offset + 1] << 8));
}

uint32_t readU32 (const Bytes& data, size_t offset)
{
    if (offset > data.size() || data.size() - offset < 4)
        throw EopError ("the file ends in the middle of a 32-bit field");
    return (uint32_t) data[offset] | ((uint32_t) data[offset + 1] << 8)
         | ((uint32_t) data[offset + 2] << 16) | ((uint32_t) data[offset + 3] << 24);
}

double readF64 (const Bytes& data, size_t offset)
{
    if (offset > data.size() || data.size() - offset < 8)
        throw EopError ("the file ends in the middle of an event timestamp");
    uint64_t bits = 0;
    for (unsigned i = 0; i < 8; ++i)
        bits |= (uint64_t) data[offset + i] << (8 * i);
    double value = 0;
    static_assert (sizeof (value) == sizeof (bits));
    std::memcpy (&value, &bits, sizeof (value));
    return value;
}

// UTF-8, or GBK (Everyone Piano is Chinese software) where Windows can decode it.
juce::String decodeText (const Bytes& data, size_t start, size_t length)
{
    if (start > data.size() || data.size() - start < length) return {};
    size_t end = start;
    while (end < start + length && data[end] != 0) ++end;
    if (end == start) return {};

    const auto* raw = reinterpret_cast<const char*> (data.data() + start);
    const int   n = (int) (end - start);
    if (juce::CharPointer_UTF8::isValidString (raw, n))
        return juce::String::fromUTF8 (raw, n).trim();
    return juce::String::fromUTF8 (detail::codePageToUtf8 (std::string (raw, (size_t) n), 936).c_str()).trim();
}

void addMappingPitch (KeyMapping& mapping, uint8_t pitch)
{
    if (std::find (mapping.pitches.begin(), mapping.pitches.end(), pitch) == mapping.pitches.end())
    {
        mapping.pitches.push_back (pitch);
        std::sort (mapping.pitches.begin(), mapping.pitches.end());
    }
}

// One key-map section of v2.01 / v3.01: a 28-byte header, then fixed-size rows. Strict, because a
// bad row here usually means we're reading from the wrong place (v2.01 zero-section maps are found
// by trying every boundary).
void parseMappingSection (const Bytes& data, size_t start, size_t size, size_t recordSize, KeyMap& mappings)
{
    constexpr size_t sectionHeaderSize = 28;
    if (size < sectionHeaderSize) throw EopError ("a key-map section is shorter than its header");
    if (start > data.size() || size > data.size() - start) throw EopError ("a key-map section runs past the end of the file");
    if ((size - sectionHeaderSize) % recordSize != 0) throw EopError ("a key-map section doesn't fit its version's row size");

    const size_t count = (size - sectionHeaderSize) / recordSize;
    std::array<bool, 256> seen {};
    for (size_t i = 0; i < count; ++i)
    {
        const size_t offset = start + sectionHeaderSize + i * recordSize;
        const uint8_t scan = data[offset + 4];
        if (seen[scan]) throw EopError ("a key-map section lists the same key twice");
        seen[scan] = true;

        const uint16_t action = readU16 (data, offset + 32);
        const uint16_t pitch  = readU16 (data, offset + 34);
        auto& mapping = mappings[scan];
        mapping.present = true;
        mapping.action = action;
        if (action == kNoteAction)
        {
            if (pitch > 127) throw EopError ("a key-map row has a pitch outside the MIDI range");
            addMappingPitch (mapping, (uint8_t) pitch);
        }
    }
}

void parseLengthPrefixedSections (const Bytes& data, size_t start, size_t end, size_t recordSize, KeyMap& mappings)
{
    size_t cursor = start;
    int sections = 0;
    while (cursor < end)
    {
        if (end - cursor < 4) throw EopError ("a key-map section length is cut off");
        const size_t size = readU32 (data, cursor);
        if (size < 28 || size > end - cursor - 4) throw EopError ("a key-map section runs past its container");
        parseMappingSection (data, cursor + 4, size, recordSize, mappings);
        cursor += 4 + size;
        ++sections;
    }
    if (sections == 0) throw EopError ("the key map has no sections");
}

// v2.01 files whose first key-map section has no length: the map is whatever lies between the
// header and the event table, and only one way of cutting it into sections may make sense.
void parseV201ZeroSection (const Bytes& data, const Header& header, KeyMap& mappings, size_t& eventStart)
{
    const uint32_t mapSize = header.mapContainerSize;
    const uint32_t eventSize = header.eventContainerSize;
    if (mapSize < 4) throw EopError ("the v2.01 key map is too small");
    if (eventSize < kEventSize || eventSize % kEventSize != 0 || eventSize > data.size())
        throw EopError ("the v2.01 event table has an invalid length");

    eventStart = data.size() - (size_t) eventSize;
    const size_t mapBytes = (size_t) mapSize - 4;
    if (mapBytes > eventStart) throw EopError ("the v2.01 key map is outside the file");
    const size_t mapStart = eventStart - mapBytes;
    if (mapStart < kHeaderSize) throw EopError ("the v2.01 key map overlaps the header");

    constexpr size_t recordSize = 42;
    std::vector<KeyMap> candidates;
    for (size_t firstSize = 28; firstSize <= mapBytes; firstSize += recordSize)
    {
        try
        {
            KeyMap candidate {};
            parseMappingSection (data, mapStart, firstSize, recordSize, candidate);
            if (mapStart + firstSize < eventStart)
                parseLengthPrefixedSections (data, mapStart + firstSize, eventStart, recordSize, candidate);
            candidates.push_back (std::move (candidate));
        }
        catch (const EopError&) {} // not a valid boundary
    }
    if (candidates.empty()) throw EopError ("the v2.01 key map can't be read");
    if (candidates.size() != 1) throw EopError ("the v2.01 key map can be read more than one way");
    mappings = std::move (candidates.front());
}

void parseCommonMapping (const Bytes& data, const Header& header, KeyMap& mappings, size_t& eventStart)
{
    const size_t recordSize = header.version == 201 ? 42 : 52;
    const uint32_t firstSectionSize = readU32 (data, kHeaderSize);
    if (firstSectionSize == 0)
    {
        if (header.version == 201)
        {
            parseV201ZeroSection (data, header, mappings, eventStart);
            return;
        }
        throw EopError ("this v3.01 key-map layout isn't supported");
    }

    uint64_t containerSize = header.mapContainerSize;
    if (containerSize == 0) containerSize = (uint64_t) firstSectionSize + 4;
    if (containerSize < 4 || containerSize > data.size() - kHeaderSize)
        throw EopError ("the key map is outside the file");
    const size_t containerEnd = kHeaderSize + (size_t) containerSize;
    parseLengthPrefixedSections (data, kHeaderSize, containerEnd, recordSize, mappings);
    eventStart = containerEnd;
}

struct Parsed
{
    Header                header;
    std::vector<EopEvent> events;
    juce::StringArray     warnings;
};

Parsed parse (const Bytes& raw)
{
    if (raw.size() < kMinimumSize) throw EopError ("the file is too small to be an EOP score");

    Bytes data (raw.size());
    for (size_t i = 0; i < raw.size(); ++i)
        data[i] = raw[i] ^ kXorMask[i % kXorMask.size()];
    if (std::memcmp (data.data(), "EveryonePiano", 13) != 0)
        throw EopError ("it isn't an Everyone Piano score");

    Parsed result;
    Header& header = result.header;
    header.version            = readU32 (data, 0x10);
    header.mapBlocks          = readU32 (data, 0x20);
    header.mapContainerSize   = readU32 (data, 0x28);
    header.eventContainerSize = readU32 (data, 0x2C);
    header.title              = decodeText (data, 0x134, 0x40);

    if (header.version != 200 && header.version != 201 && header.version != 301)
        throw EopError (("EOP version " + juce::String (header.version / 100) + "."
                        + juce::String (header.version % 100).paddedLeft ('0', 2) + " isn't supported").toStdString());

    // v2.00 stores one unscrambled NUL byte after the scrambled data.
    if (header.version == 200 && raw.back() == 0) data.pop_back();

    KeyMap mappings {};
    size_t eventStart = 0;
    uint8_t leftVelocity = 100, rightVelocity = 100;

    if (header.version == 200)
    {
        if (header.mapBlocks < 1 || header.mapBlocks > 4)
            throw EopError ("the v2.00 key map has an invalid block number");
        const size_t mapStart = kHeaderSize + (header.mapBlocks - 1) * kV200MapStride;
        const size_t rowStart = mapStart + 28;
        eventStart = rowStart + 255 * 12 + 28;
        if (eventStart > data.size()) throw EopError ("the v2.00 key map runs past the end of the file");

        // One velocity per hand; anything out of range plays at a normal strength.
        auto velocity = [] (uint32_t v) { return (uint8_t) (v >= 1 && v <= 127 ? v : 100); };
        leftVelocity  = velocity (readU32 (data, mapStart + 12));
        rightVelocity = velocity (readU32 (data, mapStart + 16));

        // 255 rows, one per scan code 1..255.
        for (size_t i = 0; i < 255; ++i)
        {
            const size_t offset = rowStart + i * 12;
            const uint32_t action = readU32 (data, offset + 4);
            const uint32_t pitch  = readU32 (data, offset + 8);
            auto& mapping = mappings[i + 1];
            mapping.present = true;
            mapping.action = action;
            if ((action == kV200Right || action == kV200Left) && pitch <= 127)
                mapping.pitches.push_back ((uint8_t) pitch);
        }
    }
    else
    {
        parseCommonMapping (data, header, mappings, eventStart);
    }

    if (eventStart > data.size() || data.size() - eventStart < kEventSize)
        throw EopError ("the event table is outside the file");

    size_t count = 0;
    if (header.version == 200)
    {
        // Events run up to a record whose status is zero (or the end of the file).
        const size_t pool = (data.size() - eventStart) / kEventSize;
        count = pool;
        for (size_t i = 0; i < pool; ++i)
            if (data[eventStart + i * kEventSize + 8] == 0) { count = i; break; }
    }
    else
    {
        const size_t remaining = data.size() - eventStart;
        if ((remaining - kEventSize) % kEventSize != 0)
            throw EopError ("the event table isn't made of 16-byte events");
        count = remaining / kEventSize - 1; // the last 16 bytes are a trailer
    }

    // Forgiving from here on: skip what doesn't make sense, and say so.
    int badTime = 0, badStatus = 0, unmapped = 0, strayOffs = 0, released = 0;
    std::map<int, int> held; // (hand, pitch) -> notes on
    double lastMs = 0.0;
    size_t serial = 0;

    for (size_t i = 0; i < count; ++i)
    {
        const size_t offset = eventStart + i * kEventSize;
        const double ms = readF64 (data, offset);
        const uint8_t status = data[offset + 8];
        const uint8_t scan = data[offset + 9];
        const uint8_t rawVelocity = data[offset + 10];

        if (! std::isfinite (ms) || ms < 0 || ms > 24.0 * 3600.0 * 1000.0) { ++badTime; continue; }
        if (status != 0x80 && status != 0x90)                             { ++badStatus; continue; }

        const auto& mapping = mappings[scan];
        if (! mapping.present) { ++unmapped; continue; }
        if (mapping.pitches.empty()) continue; // a key mapped to something other than a note

        const EopHand hand = header.version != 200 ? EopHand::None
                           : mapping.action == kV200Left ? EopHand::Left : EopHand::Right;
        // 0x90 is always a note-on in EOP, so a velocity of 0 (or out of range) plays at a normal strength.
        const uint8_t velocity = header.version == 200 ? (hand == EopHand::Left ? leftVelocity : rightVelocity)
                               : (rawVelocity >= 1 && rawVelocity <= 127 ? rawVelocity : (uint8_t) 100);
        const bool on = status == 0x90;

        for (const uint8_t pitch : mapping.pitches)
        {
            int& n = held[(int) hand * 128 + pitch];
            if (on) ++n;
            else if (n > 0) --n;
            else { ++strayOffs; continue; }

            result.events.push_back ({ ms, on, pitch, on ? velocity : (uint8_t) 0, hand, serial++ });
            lastMs = std::max (lastMs, ms);
        }
    }

    // Notes still held at the end are released with the last event.
    for (const auto& [key, n] : held)
        for (int k = 0; k < n; ++k)
        {
            result.events.push_back ({ lastMs, false, (uint8_t) (key % 128), 0, (EopHand) (key / 128), serial++, true });
            ++released;
        }

    auto& w = result.warnings;
    if (unmapped > 0)  w.add ("Skipped " + juce::String (unmapped) + " events on keys with no note");
    if (badTime > 0)   w.add ("Skipped " + juce::String (badTime) + " events with a broken time");
    if (badStatus > 0) w.add ("Skipped " + juce::String (badStatus) + " events of an unknown kind");
    if (strayOffs > 0) w.add ("Ignored " + juce::String (strayOffs) + " note releases with no note");
    if (released > 0)  w.add ("Released " + juce::String (released) + " notes still held at the end");
    return result;
}

// Events -> a MIDI file at 1 tick per millisecond, so the usual import builds the Song.
juce::MidiFile toMidi (const Parsed& parsed)
{
    auto events = parsed.events;
    std::stable_sort (events.begin(), events.end(), [] (const EopEvent& a, const EopEvent& b) {
        if (a.atEnd != b.atEnd) return b.atEnd; // after everything, even a note struck at the last moment
        if (a.ms < b.ms || b.ms < a.ms) return a.ms < b.ms;
        if (a.on != b.on) return ! a.on; // release before a re-strike at the same moment
        return a.serial < b.serial;
    });

    juce::MidiFile midi;
    midi.setTicksPerQuarterNote (1000);

    juce::MidiMessageSequence conductor;
    conductor.addEvent (juce::MidiMessage::tempoMetaEvent (1000000)); // 1 quarter = 1 s
    midi.addTrack (conductor);

    auto addTrack = [&] (EopHand hand, int channel, const char* name) {
        juce::MidiMessageSequence seq;
        for (const auto& e : events)
        {
            if (e.hand != hand) continue;
            seq.addEvent (e.on ? juce::MidiMessage::noteOn (channel, e.pitch, (juce::uint8) e.velocity)
                               : juce::MidiMessage::noteOff (channel, e.pitch), e.ms);
        }
        if (seq.getNumEvents() == 0) return;
        seq.addEvent (juce::MidiMessage::textMetaEvent (3, name), 0.0);
        seq.sort();
        midi.addTrack (seq);
    };

    // Named tracks let the import tell the hands apart.
    addTrack (EopHand::Left, 1, "Left hand");
    addTrack (EopHand::Right, 2, "Right hand");
    addTrack (EopHand::None, 1, "Piano");
    return midi;
}
} // namespace

bool songFromEop (const void* data, size_t size, Song& out, juce::String& error,
                  const ImportOptions& opts, juce::StringArray* warnings)
{
    Parsed parsed;
    try
    {
        const auto* bytes = static_cast<const uint8_t*> (data);
        parsed = parse (Bytes (bytes, bytes + size));
    }
    catch (const EopError& e)
    {
        error = "Can't read this EOP file: " + juce::String (e.what());
        return false;
    }

    if (std::none_of (parsed.events.begin(), parsed.events.end(), [] (const EopEvent& e) { return e.on; }))
    {
        error = "This EOP file has no notes";
        return false;
    }

    if (! songFromMidi (toMidi (parsed), out, error, opts))
        return false;

    out.title = parsed.header.title;
    if (warnings != nullptr) warnings->addArray (parsed.warnings);
    return true;
}

bool loadEopFile (const juce::File& file, Song& out, juce::String& error,
                  const ImportOptions& opts, juce::StringArray* warnings)
{
    if (file.getSize() > (juce::int64) kMaxFileSize)
    {
        error = file.getFileName() + " is too large to be an EOP score";
        return false;
    }

    juce::MemoryBlock block;
    if (! file.loadFileAsData (block))
    {
        error = "Couldn't open " + file.getFileName();
        return false;
    }

    if (! songFromEop (block.getData(), block.getSize(), out, error, opts, warnings))
        return false;

    if (out.title.isEmpty())
        out.title = file.getFileNameWithoutExtension();
    return true;
}

} // namespace fall
