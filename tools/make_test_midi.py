#!/usr/bin/env python3
"""Generates the small MIDI fixtures in tests/data. Run: python3 tools/make_test_midi.py"""
import os
import mido

OUT = os.path.join(os.path.dirname(__file__), "..", "tests", "data")
TPQ = 480


def track(name, events):
    """events: list of (abs_tick, msg). Returns a MidiTrack with delta times."""
    t = mido.MidiTrack()
    t.append(mido.MetaMessage("track_name", name=name, time=0))
    last = 0
    for tick, msg in sorted(events, key=lambda e: (e[0], 0 if e[1].type == "note_off" else 1)):
        t.append(msg.copy(time=tick - last))
        last = tick
    return t


def notes(seq, channel=0):
    """seq: list of (start_beat, length_beats, pitch[, velocity])."""
    ev = []
    for item in seq:
        b, l, p = item[:3]
        v = item[3] if len(item) > 3 else 80
        ev.append((int(b * TPQ), mido.Message("note_on", note=p, velocity=v, channel=channel)))
        ev.append((int((b + l) * TPQ), mido.Message("note_off", note=p, velocity=0, channel=channel)))
    return ev


def two_tracks():
    # 120 bpm for 4 beats (2 s), then 60 bpm.
    meta = mido.MidiTrack()
    meta.append(mido.MetaMessage("set_tempo", tempo=500000, time=0))
    meta.append(mido.MetaMessage("set_tempo", tempo=1000000, time=4 * TPQ))
    rh = notes([(0, 1, 72), (1, 1, 74), (2, 1, 76), (3, 1, 77), (4, 1, 79), (5, 1, 77)])
    lh = notes([(0, 2, 48), (0, 2, 55), (2, 2, 43), (4, 2, 48), (4, 2, 52)])
    lh += [(0, mido.Message("control_change", control=64, value=127)),
           (int(3.9 * TPQ), mido.Message("control_change", control=64, value=0))]
    mid = mido.MidiFile(ticks_per_beat=TPQ, type=1)
    mid.tracks += [meta, track("Piano Right", rh), track("Piano Left", lh)]
    mid.save(os.path.join(OUT, "two_tracks.mid"))


def single_track():
    mid = mido.MidiFile(ticks_per_beat=TPQ, type=0)
    ev = notes([(0, 1, 60), (0, 1, 64), (0, 1, 67), (0, 1, 36), (1, 1, 62), (1, 1, 41), (2, 2, 59), (2, 2, 43)])
    t = track("Piano", ev)
    t.insert(1, mido.MetaMessage("set_tempo", tempo=500000, time=0))
    mid.tracks.append(t)
    mid.save(os.path.join(OUT, "single_track.mid"))


def demo():
    """Ode to Joy (Beethoven, public domain), simple two-hand arrangement, ~40 s."""
    E, F, G, A, B, C5, D5 = 76, 77, 79, 81, 83, 84, 86
    D, C = 74, 72
    phrase_a = [E, E, F, G, G, F, E, D, C, C, D, E]
    mel = []
    beat = 0.0

    def add(pitches_lengths):
        nonlocal beat
        for p, l in pitches_lengths:
            mel.append((beat, l * 0.95, p, 84))
            beat += l

    line1 = [(p, 1) for p in phrase_a] + [(E, 1.5), (D, 0.5), (D, 2)]
    line2 = [(p, 1) for p in phrase_a] + [(D, 1.5), (C, 0.5), (C, 2)]
    line3 = [(D, 1), (D, 1), (E, 1), (C, 1), (D, 1), (E, .5), (F, .5), (E, 1), (C, 1),
             (D, 1), (E, .5), (F, .5), (E, 1), (D, 1), (C, 1), (D, 1), (55 + 12, 2)]
    add(line1); add(line2); add(line3); add(line2)
    # Left hand: one chord per bar following the harmony.
    bars = ["C", "G", "C", "G", "C", "G", "C", "C", "G", "C", "G", "C", "C", "G", "C", "C"]
    chords = {"C": [48, 55, 64], "G": [43, 55, 62]}
    lh = []
    for i, name in enumerate(bars):
        root, fifth, third = chords[name]
        lh += [(i * 4, 1.9, root, 64), (i * 4 + 2, 1.9, fifth, 58), (i * 4 + 2, 1.9, third - 12, 58)]
    meta = mido.MidiTrack()
    meta.append(mido.MetaMessage("set_tempo", tempo=mido.bpm2tempo(100), time=0))
    mid = mido.MidiFile(ticks_per_beat=TPQ, type=1)
    mid.tracks += [meta, track("Right Hand", notes(mel)), track("Left Hand", notes(lh))]
    mid.save(os.path.join(OUT, "ode_to_joy.mid"))


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    two_tracks()
    single_track()
    demo()
    print("wrote", sorted(os.listdir(OUT)))
