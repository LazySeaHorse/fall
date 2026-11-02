#!/usr/bin/env python3
"""Non-piano sounds for the note detector's false-accept test.

Synthesises finger snaps, claps, knocks on a table, taps on the laptop, typing, a spoon on a
mug (ringing, so genuinely tonal) and a sung/spoken vowel, one event every 1.6 s, and runs them
through the same "laptop mic" chains as gen_dataset.py.

  data/generated/<level>/noises.wav
  data/generated/midi/noises_events.csv   (time,kind)

Evaluate with:  fall_detect_eval --transients data/generated/easy/noises.wav data/generated/midi/noises_events.csv

Usage: python3 tools/detect/gen_noises.py [--out data/generated] [--levels clean,easy,medium] [--per-kind 24]
Needs numpy and sox.
"""
import argparse
import os
import random
import wave

import numpy as np

import gen_dataset as gd

RATE = 48000


def bandnoise(rng, n, lo, hi):
    x = rng.standard_normal(n)
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(n, 1.0 / RATE)
    X[(f < lo) | (f > hi)] = 0
    y = np.fft.irfft(X, n)
    return y / (np.max(np.abs(y)) + 1e-12)


def env(n, attack_ms, tau_ms):
    t = np.arange(n) / RATE
    a = np.clip(t / max(attack_ms / 1000.0, 1e-5), 0, 1)
    return a * np.exp(-t / (tau_ms / 1000.0))


def damped(freqs, taus_ms, amps, n, rng):
    t = np.arange(n) / RATE
    y = np.zeros(n)
    for f, tau, a in zip(freqs, taus_ms, amps):
        y += a * np.sin(2 * np.pi * f * t + rng.uniform(0, 2 * np.pi)) * np.exp(-t / (tau / 1000.0))
    return y


def snap(rng):
    n = int(0.12 * RATE)
    y = bandnoise(rng, n, 1500, 6000) * env(n, 0.3, rng.uniform(2.5, 5))
    y += 0.5 * damped([rng.uniform(1800, 3200)], [rng.uniform(5, 12)], [1], n, rng)
    return y


def clap(rng):
    n = int(0.2 * RATE)
    y = np.zeros(n)
    for k in range(rng.integers(2, 4)):
        off = int(rng.uniform(0, 0.012) * RATE) + k * int(0.004 * RATE)
        m = n - off
        y[off:] += rng.uniform(0.5, 1) * bandnoise(rng, m, 500, 3500) * env(m, 0.5, rng.uniform(5, 12))
    return y


def knock(rng):
    n = int(0.3 * RATE)
    k = rng.integers(3, 7)
    y = damped(rng.uniform(120, 900, k), rng.uniform(15, 60, k), rng.uniform(0.3, 1, k), n, rng)
    y += 0.6 * bandnoise(rng, n, 800, 5000) * env(n, 0.2, 1.5)
    return y


def tap(rng):
    n = int(0.15 * RATE)
    y = damped([rng.uniform(70, 200)], [rng.uniform(8, 20)], [1], n, rng)
    y += 0.5 * bandnoise(rng, n, 1000, 7000) * env(n, 0.2, 1.0)
    return y


def typing(rng):
    n = int(0.9 * RATE)
    y = np.zeros(n)
    t = 0.0
    while t < 0.75:
        off = int(t * RATE)
        m = int(0.05 * RATE)
        c = bandnoise(rng, m, 2000, 9000) * env(m, 0.1, rng.uniform(1, 2.5))
        c += 0.4 * damped([rng.uniform(250, 450)], [6], [1], m, rng)
        y[off:off + m] += rng.uniform(0.4, 1) * c
        t += rng.uniform(0.07, 0.16)
    return y


def clink(rng):
    n = int(0.8 * RATE)
    base = rng.uniform(1800, 3500)
    ratios = np.array([1.0, 2.32, 4.25, 6.63])  # bell-like, inharmonic
    y = damped(base * ratios * rng.uniform(0.97, 1.03, 4), rng.uniform(120, 400, 4), [1, 0.6, 0.35, 0.2], n, rng)
    y += 0.5 * bandnoise(rng, n, 2000, 9000) * env(n, 0.1, 1.0)
    return y


def vowel(rng):
    """A short sung/spoken 'ah' with jitter and vibrato: harmonic, like a note, by nature."""
    n = int(0.35 * RATE)
    t = np.arange(n) / RATE
    f0 = rng.uniform(100, 260) * (1 + 0.012 * np.sin(2 * np.pi * 5.5 * t)) * (1 + 0.004 * rng.standard_normal(n).cumsum() / np.sqrt(n))
    ph = 2 * np.pi * np.cumsum(f0) / RATE
    formants = [(800, 80), (1200, 90), (2600, 120)]
    y = np.zeros(n)
    for h in range(1, 40):
        fh = h * f0.mean()
        if fh > 6000:
            break
        g = sum(np.exp(-0.5 * ((fh - fc) / bw) ** 2) for fc, bw in formants) + 0.05
        y += g / h ** 0.5 * np.sin(h * ph)
    a = np.clip(t / 0.03, 0, 1) * np.clip((t[-1] - t) / 0.05, 0, 1)
    return y * a


KINDS = {"snap": snap, "clap": clap, "knock": knock, "tap": tap, "typing": typing, "clink": clink, "vowel": vowel}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(gd.ROOT, "data", "generated"))
    ap.add_argument("--levels", default="clean,easy,medium")
    ap.add_argument("--per-kind", type=int, default=24)
    ap.add_argument("--seed", type=int, default=7)
    a = ap.parse_args()
    rng = np.random.default_rng(a.seed)
    order = [k for k in KINDS for _ in range(a.per_kind)]
    random.Random(a.seed).shuffle(order)

    spacing = 1.6
    total = gd.LEAD_IN + spacing * len(order) + 1.0
    x = np.zeros(int(total * RATE))
    events = []
    for i, kind in enumerate(order):
        t = gd.LEAD_IN + i * spacing + rng.uniform(0, 0.2)
        y = KINDS[kind](rng)
        y = y / (np.max(np.abs(y)) + 1e-12) * 10 ** (rng.uniform(-14, 0) / 20.0) * 0.7
        off = int(t * RATE)
        x[off:off + len(y)] += y[: len(x) - off]
        events.append((t, kind))

    os.makedirs(os.path.join(a.out, "midi"), exist_ok=True)
    os.makedirs(os.path.join(a.out, "dry"), exist_ok=True)
    dry = os.path.join(a.out, "dry", "noises.wav")
    pcm = (np.clip(np.repeat(x[:, None], 2, axis=1), -1, 1) * 32767).astype("<i2")
    with wave.open(dry, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(pcm.tobytes())
    with open(os.path.join(a.out, "midi", "noises_events.csv"), "w") as f:
        f.write("time,kind\n")
        for t, k in events:
            f.write(f"{t:.4f},{k}\n")
    for lv in [l for l in a.levels.split(",") if l]:
        d = os.path.join(a.out, lv)
        os.makedirs(d, exist_ok=True)
        gd.mic_sim(dry, os.path.join(d, "noises.wav"), gd.LEVELS[lv], total, RATE)
    print(f"{len(events)} events, {total:.0f} s -> {a.out}")


if __name__ == "__main__":
    main()
