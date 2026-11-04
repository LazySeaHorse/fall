#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Eop.h"

#include <cmath>
#include <cstring>
#include <limits>

using namespace fall;
using Catch::Matchers::WithinAbs;

// Builds small .eop files with the layout the loader reads (see src/core/Eop.cpp).
namespace
{
using Bytes = std::vector<uint8_t>;

const uint8_t kMask[32] = {
    0x71, 0x72, 0x73, 0x74, 0x72, 0x73, 0x74, 0x75, 0x73, 0x74, 0x75, 0x76, 0x74, 0x75, 0x76, 0x77,
    0x75, 0x76, 0x77, 0x78, 0x76, 0x77, 0x78, 0x79, 0x77, 0x78, 0x79, 0x7A, 0x78, 0x79, 0x7A, 0x7B,
};

void putU16 (Bytes& b, size_t at, uint16_t v) { b[at] = (uint8_t) v; b[at + 1] = (uint8_t) (v >> 8); }
void putU32 (Bytes& b, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) b[at + (size_t) i] = (uint8_t) (v >> (8 * i)); }
void putF64 (Bytes& b, size_t at, double v)
{
    uint64_t bits; std::memcpy (&bits, &v, 8);
    for (int i = 0; i < 8; ++i) b[at + (size_t) i] = (uint8_t) (bits >> (8 * i));
}

struct Ev { double ms; uint8_t status; uint8_t scan; uint8_t velocity = 80; };
struct Row { uint8_t scan; uint32_t action; uint16_t pitch; };
Row note (uint8_t scan, uint16_t pitch) { return { scan, 0x0090, pitch }; }

Bytes header (uint32_t version, const char* title = "Test song")
{
    Bytes b (0x1B8, 0);
    std::memcpy (b.data(), "EveryonePiano", 13);
    putU32 (b, 0x10, version);
    putU32 (b, 0x14, 4);
    putU32 (b, 0x18, 4);
    putU32 (b, 0x1C, 120);
    std::memcpy (b.data() + 0x134, title, std::strlen (title));
    return b;
}

void appendEvents (Bytes& b, const std::vector<Ev>& events)
{
    for (const auto& e : events)
    {
        Bytes rec (16, 0);
        putF64 (rec, 0, e.ms);
        rec[8] = e.status; rec[9] = e.scan; rec[10] = e.velocity;
        b.insert (b.end(), rec.begin(), rec.end());
    }
    b.insert (b.end(), 16, 0); // trailer (v2.01 / v3.01) or terminator (v2.00)
}

Bytes scramble (Bytes b)
{
    for (size_t i = 0; i < b.size(); ++i) b[i] ^= kMask[i % 32];
    return b;
}

// v2.01 (42-byte rows) or v3.01 (52-byte rows), with length-prefixed key-map sections.
Bytes makeV3 (const std::vector<std::vector<Row>>& sections, const std::vector<Ev>& events, uint32_t version = 301)
{
    const size_t rowSize = version == 201 ? 42 : 52;
    Bytes b = header (version);
    Bytes container;
    for (const auto& rows : sections)
    {
        Bytes sec (28 + rows.size() * rowSize, 0);
        for (size_t i = 0; i < rows.size(); ++i)
        {
            const size_t at = 28 + i * rowSize;
            sec[at + 4] = rows[i].scan;
            putU16 (sec, at + 32, (uint16_t) rows[i].action);
            putU16 (sec, at + 34, rows[i].pitch);
        }
        Bytes len (4, 0); putU32 (len, 0, (uint32_t) sec.size());
        container.insert (container.end(), len.begin(), len.end());
        container.insert (container.end(), sec.begin(), sec.end());
    }
    putU32 (b, 0x28, (uint32_t) container.size());
    putU32 (b, 0x2C, (uint32_t) ((events.size() + 1) * 16));
    b.insert (b.end(), container.begin(), container.end());
    appendEvents (b, events);
    return scramble (b);
}

// v2.00: a fixed table of 255 rows (scan codes 1..255), one velocity per hand.
Bytes makeV200 (const std::vector<Row>& rows, uint32_t leftVel, uint32_t rightVel, const std::vector<Ev>& events)
{
    Bytes b = header (200);
    putU32 (b, 0x20, 1); // map block
    Bytes map (28 + 255 * 12 + 28, 0);
    putU32 (map, 12, leftVel);
    putU32 (map, 16, rightVel);
    for (uint32_t i = 0; i < 255; ++i)
        putU32 (map, 28 + i * 12, i + 1);
    for (const auto& r : rows)
    {
        putU32 (map, 28 + (size_t) (r.scan - 1) * 12 + 4, r.action == 0x0090 ? 1u : r.action);
        putU32 (map, 28 + (size_t) (r.scan - 1) * 12 + 8, r.pitch);
    }
    b.insert (b.end(), map.begin(), map.end());
    appendEvents (b, events);
    Bytes out = scramble (b);
    out.push_back (0); // the unscrambled NUL v2.00 files end with
    return out;
}

bool load (const Bytes& file, Song& s, juce::String& err, juce::StringArray* warnings = nullptr, int split = 60)
{
    ImportOptions io; io.splitPitch = split;
    return songFromEop (file.data(), file.size(), s, err, io, warnings);
}
} // namespace

TEST_CASE ("EOP 3.01: notes, times, velocities and title", "[eop]")
{
    const auto file = makeV3 ({ { note (0x1E, 60), note (0x1F, 64) } },
                              { { 0, 0x90, 0x1E }, { 0, 0x90, 0x1F }, { 500, 0x80, 0x1E }, { 500, 0x80, 0x1F },
                                { 1000, 0x90, 0x1E, 90 }, { 1250.5, 0x80, 0x1E } });
    Song s; juce::String err; juce::StringArray warnings;
    REQUIRE (load (file, s, err, &warnings));
    CHECK (warnings.isEmpty());
    CHECK (s.title == "Test song");
    REQUIRE (s.notes.size() == 3);
    CHECK (s.notes[0].pitch == 60);
    CHECK (s.notes[1].pitch == 64);
    CHECK_THAT (s.notes[0].start, WithinAbs (0.0, 1e-6));
    CHECK_THAT (s.notes[0].end, WithinAbs (0.5, 1e-6));
    CHECK_THAT (s.notes[2].start, WithinAbs (1.0, 1e-6));
    CHECK_THAT (s.notes[2].end, WithinAbs (1.2505, 1e-6)); // no rounding to MIDI ticks
    CHECK_THAT (s.notes[2].velocity, WithinAbs (90.0 / 127.0, 1e-3));
    CHECK_THAT (s.length, WithinAbs (1.2505, 1e-6));
    CHECK_FALSE (s.handsFromTracks); // no hand data: split at the split point
    CHECK (s.notes[0].hand == Hand::Right);
}

TEST_CASE ("EOP 3.01 without hand data splits at the split point", "[eop]")
{
    const auto file = makeV3 ({ { note (1, 48), note (2, 72) } },
                              { { 0, 0x90, 1 }, { 0, 0x90, 2 }, { 300, 0x80, 1 }, { 300, 0x80, 2 } });
    Song s; juce::String err;
    REQUIRE (load (file, s, err));
    REQUIRE (s.notes.size() == 2);
    CHECK (s.notes[0].hand == Hand::Left);
    CHECK (s.notes[1].hand == Hand::Right);
}

TEST_CASE ("EOP 2.01 uses 42-byte key-map rows", "[eop]")
{
    const auto file = makeV3 ({ { note (0x10, 67), note (0x11, 69) } },
                              { { 0, 0x90, 0x11 }, { 200, 0x80, 0x11 } }, 201);
    Song s; juce::String err;
    REQUIRE (load (file, s, err));
    REQUIRE (s.notes.size() == 1);
    CHECK (s.notes[0].pitch == 69);
}

TEST_CASE ("EOP 2.00 keeps the hands, even when the left plays higher", "[eop]")
{
    // Left hand above the right: only the stored hands (not the pitch) can get this right.
    const auto file = makeV200 ({ { 1, 0x00010001, 64 }, { 2, 0x0090, 62 } }, 70, 110,
                                { { 0, 0x90, 1 }, { 0, 0x90, 2 }, { 400, 0x80, 1 }, { 400, 0x80, 2 } });
    Song s; juce::String err;
    REQUIRE (load (file, s, err));
    REQUIRE (s.notes.size() == 2);
    CHECK (s.handsFromTracks);
    CHECK (s.notes[0].pitch == 62);
    CHECK (s.notes[0].hand == Hand::Right);
    CHECK_THAT (s.notes[0].velocity, WithinAbs (110.0 / 127.0, 1e-3));
    CHECK (s.notes[1].pitch == 64);
    CHECK (s.notes[1].hand == Hand::Left);
    CHECK_THAT (s.notes[1].velocity, WithinAbs (70.0 / 127.0, 1e-3));
    CHECK_THAT (s.notes[1].end, WithinAbs (0.4, 1e-6));
}

TEST_CASE ("One EOP key can play several notes", "[eop]")
{
    // The same key in two key-map sections adds a note each.
    const auto file = makeV3 ({ { note (5, 60) }, { note (5, 67) } },
                              { { 0, 0x90, 5 }, { 250, 0x80, 5 } });
    Song s; juce::String err;
    REQUIRE (load (file, s, err));
    REQUIRE (s.notes.size() == 2);
    CHECK (s.notes[0].pitch == 60);
    CHECK (s.notes[1].pitch == 67);
}

TEST_CASE ("EOP notes are read forgivingly", "[eop]")
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const auto file = makeV3 ({ { note (1, 60), note (2, 62), { 3, 0x0001, 0 } } },
                              { { 0, 0x90, 1 },
                                { 10, 0x90, 9 },    // key with nothing mapped
                                { 20, 0x80, 2 },    // release of a note that never started
                                { 30, 0xB0, 1 },    // unknown kind of event
                                { nan, 0x90, 2 },   // broken time
                                { 40, 0x90, 3 },    // key mapped to something other than a note: silent
                                { 100, 0x80, 1 },
                                { 200, 0x90, 2, 0 }, // velocity 0 is still a note
                                { 900, 0x90, 1 } }); // never released
    Song s; juce::String err; juce::StringArray warnings;
    REQUIRE (load (file, s, err, &warnings));
    REQUIRE (s.notes.size() == 3);
    CHECK (s.notes[1].pitch == 62);
    CHECK (s.notes[1].velocity > 0.5f);
    CHECK_THAT (s.notes[1].end, WithinAbs (0.9, 1e-6)); // held to the end too
    CHECK (s.notes[2].pitch == 60);
    CHECK_THAT (s.notes[2].start, WithinAbs (0.9, 1e-6));
    CHECK_THAT (s.notes[2].end, WithinAbs (0.93, 1e-6)); // released at the last event (+ the minimum length)
    CHECK (warnings.size() == 5);
}

TEST_CASE ("Files that aren't EOP scores are refused", "[eop]")
{
    Song s; juce::String err;

    Bytes tiny (100, 0);
    CHECK_FALSE (load (tiny, s, err));

    Bytes junk (2000, 0x42);
    CHECK_FALSE (load (junk, s, err));
    CHECK (err.contains ("Everyone Piano"));

    auto plain = makeV3 ({ { note (1, 60) } }, { { 0, 0x90, 1 }, { 100, 0x80, 1 } });
    for (size_t i = 0; i < plain.size(); ++i) plain[i] ^= kMask[i % 32]; // not scrambled
    CHECK_FALSE (load (plain, s, err));

    auto future = makeV3 ({ { note (1, 60) } }, { { 0, 0x90, 1 }, { 100, 0x80, 1 } }, 401);
    CHECK_FALSE (load (future, s, err));
    CHECK (err.contains ("4.01"));

    auto cut = makeV3 ({ { note (1, 60) } }, { { 0, 0x90, 1 }, { 100, 0x80, 1 } });
    cut.resize (cut.size() - 5);
    CHECK_FALSE (load (cut, s, err));

    const auto silent = makeV3 ({ { note (1, 60) } }, { { 0, 0x90, 7 } });
    CHECK_FALSE (load (silent, s, err));
    CHECK (err.contains ("no notes"));
}
