#!/usr/bin/env python3
"""Piano-through-a-laptop-mic dataset generator for the Fall note detector.

For every test MIDI (scales over the full range, chords, repeated notes, fast passages, pedal
passages, Ode to Joy, a random "piece") this
  1. writes the MIDI (ground truth) to <out>/midi/<name>.mid and <name>.csv (onset,pitch,dur,vel),
  2. renders it with fluidsynth (FluidR3_GM, program 0 = Acoustic Grand) to <out>/dry/<name>.wav,
  3. runs a "laptop mic" chain with sox at several difficulty levels -> <out>/<level>/<name>.wav.

Usage:  python3 tools/detect/gen_dataset.py [--out data/generated] [--levels easy,medium,hard]
                                            [--only scale,chords] [--rate 48000]
Needs: fluidsynth, sox, python3 with mido.  Generated audio is git-ignored.

The MIDI files already contain LEAD_IN seconds of silence, so MIDI time == WAV time.
"""
import argparse
import csv
import os
import random
import shutil
import subprocess
import sys

import mido

TPQ = 480
LEAD_IN = 1.0      # seconds of silence before the first note (noise floor / detector warm-up)
SF2 = "/usr/share/sounds/sf2/FluidR3_GM.sf2"
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

# --- laptop mic simulation levels --------------------------------------------------------------
# hp/lp: mic frequency response; reverb: sox reverb args (reverberance, HF damping, room scale,
# stereo depth, pre-delay, wet gain); noise: pink noise level in dBFS; compand: crude AGC;
# gain: overall dB; tremolo: slow level wobble (speed Hz, depth %) imitating AGC pumping.
LEVELS = {
    "clean":  dict(hp=60,  lp=12000, reverb=None, noise=-90, compand=False, gain=-6, tremolo=None),
    "easy":   dict(hp=150, lp=8000, reverb=(20, 50, 60, 100, 10, -6), noise=-62, compand=False, gain=-3, tremolo=None),
    "medium": dict(hp=150, lp=8000, reverb=(40, 50, 80, 100, 20, -3), noise=-52, compand=True, gain=-6, tremolo=(0.25, 25)),
    "hard":   dict(hp=200, lp=7000, reverb=(60, 40, 100, 100, 30, 0), noise=-38, compand=True, gain=-12, tremolo=(0.4, 45)),
}


# --- MIDI builders -------------------------------------------------------------------------------
class Piece:
    def __init__(self, name, bpm=100):
        self.name, self.bpm = name, bpm
        self.notes = []  # (start_sec, dur_sec, pitch, vel)
        self.pedal = []  # (time_sec, value)

    def add(self, t, dur, pitch, vel):
        self.notes.append((LEAD_IN + t, dur, pitch, vel))

    def ped(self, t, v):
        self.pedal.append((LEAD_IN + t, v))

    def end(self):
        return max(n[0] + n[1] for n in self.notes) + 1.5

    def save(self, path):
        def sec2tick(s):
            return int(round(s * TPQ * self.bpm / 60.0))
        ev = []
        for s, d, p, v in self.notes:
            ev.append((sec2tick(s), 1, mido.Message("note_on", note=p, velocity=v)))
            ev.append((sec2tick(s + d), 0, mido.Message("note_off", note=p, velocity=0)))
        for t, v in self.pedal:
            ev.append((sec2tick(t), 0, mido.Message("control_change", control=64, value=v)))
        ev.sort(key=lambda e: (e[0], e[1]))
        tr = mido.MidiTrack()
        tr.append(mido.MetaMessage("set_tempo", tempo=mido.bpm2tempo(self.bpm), time=0))
        last = 0
        for tick, _, m in ev:
            tr.append(m.copy(time=tick - last))
            last = tick
        mid = mido.MidiFile(ticks_per_beat=TPQ, type=0)
        mid.tracks.append(tr)
        mid.save(path)

    def save_csv(self, path):
        with open(path, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["onset", "pitch", "duration", "velocity"])
            for s, d, p, v in sorted(self.notes):
                w.writerow([f"{s:.4f}", p, f"{d:.4f}", v])


def vel(rng, lo=50, hi=105):
    return rng.randint(lo, hi)


def make_scale(rng):
    p = Piece("scale_full")
    t = 0.0
    for pitch in range(21, 109):
        p.add(t, 0.45, pitch, vel(rng))
        t += 0.5
    for pitch in range(108, 20, -1):  # and back down, staccato and quieter
        p.add(t, 0.15, pitch, vel(rng, 35, 70))
        t += 0.4
    return p


def make_chords(rng):
    p = Piece("chords")
    t = 0.0
    shapes = [(0, 4, 7), (0, 3, 7), (0, 4, 7, 11), (0, 5, 7), (0, 4, 7, 12), (0, 7, 12, 16), (0, 3, 6, 9), (0, 4, 7, 10, 14)]
    for root in range(24, 97, 3):
        sh = rng.choice(shapes)
        pitches = [root + i for i in sh if root + i <= 108]
        for pc in pitches:
            p.add(t + rng.random() * 0.012, 1.1, pc, vel(rng, 55, 100))
        t += 1.4
    # six-note, two-handed chords
    for _ in range(10):
        lh = rng.randint(36, 52)
        rh = rng.randint(60, 84)
        for pc in (lh, lh + 7, lh + 12, rh, rh + 4, rh + 7):
            p.add(t + rng.random() * 0.02, 1.2, pc, vel(rng, 55, 100))
        t += 1.5
    return p


def make_repeated(rng):
    p = Piece("repeated")
    t = 0.0
    for pitch in (24, 33, 40, 48, 55, 60, 64, 67, 72, 79, 84, 91, 96, 103):
        for i in range(8):  # same key struck again while still ringing (0.25 s apart)
            p.add(t, 0.22 if i % 2 else 0.4, pitch, vel(rng, 50, 100))
            t += 0.25
        t += 0.6
    # repeated chords
    for chord in ((48, 55, 64), (36, 43, 52, 67), (60, 64, 67, 72)):
        for _ in range(6):
            for pc in chord:
                p.add(t, 0.4, pc, vel(rng, 60, 95))
            t += 0.3
        t += 0.6
    # soft repetition after a loud strike
    for pitch in (45, 60, 76):
        p.add(t, 1.5, pitch, 110)
        for i in range(1, 4):
            p.add(t + 0.3 * i, 0.25, pitch, 60)
        t += 2.5
    return p


def make_fast(rng):
    p = Piece("fast")
    t = 0.0
    pitch = 60
    for _ in range(300):  # 16th notes at ~140 bpm, wandering
        step = rng.choice([-2, -1, -1, 1, 1, 2, 3, -3, 5, -5])
        pitch = max(33, min(100, pitch + step))
        p.add(t, 0.11, pitch, vel(rng, 55, 100))
        t += 0.105
    # fast two-hand: alternating bass and treble
    for _ in range(80):
        p.add(t, 0.2, rng.choice([36, 43, 48, 52]), vel(rng, 60, 95))
        p.add(t + 0.07, 0.1, rng.randint(72, 96), vel(rng, 55, 95))
        t += 0.14
    return p


def make_pedal(rng):
    p = Piece("pedal")
    t = 0.0
    for _ in range(24):
        root = rng.choice([36, 38, 41, 43, 45, 48])
        p.ped(t, 127)
        arp = [root, root + 7, root + 12, root + 16, root + 19, root + 24, root + 19, root + 16]
        arp += [x + 12 for x in arp[:4]]
        for i, pc in enumerate(arp[:12]):
            p.add(t + i * 0.2, 0.5, pc, vel(rng, 45, 95))
        # melody over the pedalled wash
        for j in range(3):
            p.add(t + 0.4 + j * 0.8, 0.7, rng.randint(72, 91), vel(rng, 60, 100))
        t += 2.6
        p.ped(t - 0.05, 0)
    return p


def make_piece(rng):
    """A random 'real-ish' piece: slow bass + chords on the left, a wandering melody on the right."""
    p = Piece("mixed", bpm=90)
    t = 0.0
    mel = 72
    for _ in range(30):
        root = rng.choice([36, 38, 40, 41, 43, 45, 47, 48])
        p.ped(t, 127)
        p.add(t, 1.2, root, vel(rng, 55, 85))
        for k in (1, 2, 3):
            for pc in (root + 12, root + 19, root + 24 + rng.choice([0, 3, 4])):
                p.add(t + k * 0.5, 0.4, pc, vel(rng, 40, 70))
        for k in range(8):
            if rng.random() < 0.8:
                mel = max(60, min(96, mel + rng.choice([-4, -2, -1, 0, 1, 2, 4])))
                p.add(t + k * 0.25, rng.choice([0.2, 0.4, 0.8]), mel, vel(rng, 60, 105))
        t += 2.0
        p.ped(t - 0.04, 0)
    return p


def ode_to_joy():
    mid = mido.MidiFile(os.path.join(ROOT, "tests", "data", "ode_to_joy.mid"))
    p = Piece("ode_to_joy")
    t = 0.0
    on = {}
    for msg in mid:  # iterating a MidiFile yields delta times in seconds
        t += msg.time
        if msg.type == "note_on" and msg.velocity > 0:
            on[msg.note] = (t, msg.velocity)
        elif msg.type in ("note_off", "note_on") and msg.note in on:
            s, v = on.pop(msg.note)
            p.notes.append((LEAD_IN + s, max(0.05, t - s), msg.note, v))
    return p


BUILDERS = {"scale": make_scale, "chords": make_chords, "repeated": make_repeated, "fast": make_fast,
            "pedal": make_pedal, "mixed": make_piece}


# --- rendering ---------------------------------------------------------------------------------
def run(cmd):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        sys.exit(f"command failed: {' '.join(cmd)}\n{r.stdout}")


def render_dry(mid_path, wav_path, rate):
    # -R 0 -C 0: no built-in reverb/chorus, the room is simulated afterwards.
    run(["fluidsynth", "-ni", "-g", "2.0", "-R", "0", "-C", "0", "-r", str(rate), "-F", wav_path, SF2, mid_path])


def mic_sim(dry, out, lv, duration, rate):
    # Several sox invocations instead of one long effects chain: sox truncates the output when
    # reverb, pad, compand and tremolo are combined in one chain.
    t1, t2 = out + ".1.wav", out + ".2.wav"
    fx = ["remix", "1,2", "highpass", str(lv["hp"]), "lowpass", str(lv["lp"])]
    if lv["reverb"]:
        fx += ["reverb"] + [str(x) for x in lv["reverb"]] + ["remix", "1,2"]
    run(["sox", dry, "-c", "1", "-r", str(rate), "-b", "16", t1] + fx + ["pad", "0", "1"])
    fx = []
    if lv["compand"]:
        # soft-knee compressor + make-up gain, roughly what Windows AGC / "loudness equalisation" does
        # (quiet sounds are lifted by ~10 dB, loud ones squeezed, with 20 ms attack / 300 ms release)
        fx += ["compand", "0.02,0.3", "6:-90,-90,-50,-40,-20,-17,0,-8", "0", "-90"]
    if lv["tremolo"]:
        fx += ["tremolo", str(lv["tremolo"][0]), str(lv["tremolo"][1])]
    fx += ["gain", str(lv["gain"])]
    run(["sox", t1, "-b", "16", t2] + fx)
    os.remove(t1)
    if lv["noise"] > -89:
        noise = out + ".noise.wav"
        run(["sox", "-n", "-r", str(rate), "-c", "1", "-b", "16", noise, "synth", f"{duration + 3:.2f}", "pinknoise",
             "vol", f"{10 ** (lv['noise'] / 20.0):.6f}", "highpass", "80"])
        run(["sox", "-m", t2, noise, "-b", "16", out])
        os.remove(noise)
        os.remove(t2)
    else:
        os.replace(t2, out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(ROOT, "data", "generated"))
    ap.add_argument("--levels", default="clean,easy,medium,hard")
    ap.add_argument("--only", default="", help="comma separated subset of: " + ",".join(list(BUILDERS) + ["ode_to_joy"]))
    ap.add_argument("--rate", type=int, default=48000)
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    for tool in ("fluidsynth", "sox"):
        if not shutil.which(tool):
            sys.exit(f"{tool} not found")
    levels = [x for x in a.levels.split(",") if x]
    only = set(x for x in a.only.split(",") if x)
    os.makedirs(os.path.join(a.out, "midi"), exist_ok=True)
    os.makedirs(os.path.join(a.out, "dry"), exist_ok=True)
    pieces = list(BUILDERS.items()) + [("ode_to_joy", None)]
    for name, builder in pieces:
        if only and name not in only:
            continue
        rng = random.Random(f"{a.seed}-{name}")  # per-piece seed: --only doesn't change content
        piece = builder(rng) if builder else ode_to_joy()
        piece.name = name
        mid_path = os.path.join(a.out, "midi", name + ".mid")
        piece.save(mid_path)
        piece.save_csv(os.path.join(a.out, "midi", name + ".csv"))
        dry = os.path.join(a.out, "dry", f"{name}.wav")
        render_dry(mid_path, dry, a.rate)
        for lvname in levels:
            d = os.path.join(a.out, lvname)
            os.makedirs(d, exist_ok=True)
            mic_sim(dry, os.path.join(d, f"{name}.wav"), LEVELS[lvname], piece.end(), a.rate)
        print(f"{name}: {len(piece.notes)} notes, {piece.end():.1f} s")
    print("done ->", a.out)


if __name__ == "__main__":
    main()
