# Fall

A lightweight Windows app for **learning piano from MIDI files on a real piano**. Notes fall
onto an on-screen keyboard. In **Learn** mode the song waits at every chord until you play it on
your acoustic piano. Fall listens through the laptop microphone, so the piano needs no MIDI
connection.

- **Two modes.** *Free play* scrolls in time. *Learn* waits for you to play each chord.
- **Your own instruments.** Fall plays the song through any VST3 instrument you have installed,
  such as Pianoteq, and keeps its preset between sessions. A small built-in piano covers the case
  where you haven't picked one.
- **Hearing your piano.** Fall knows which notes come next, so it only has to check whether they
  were played. That keeps it fast, at a few tens of milliseconds. The keys it hears show on screen
  as purple dots.
- **Calibration.** *Settings → Teach Fall my piano* has you play about 30 keys, so Fall learns how
  your piano sounds through your microphone.
- **Jump keys.** Drop markers on the timeline (`M` or right-click) and bind each one to a piano
  key. Playing that key jumps the song back to the marker. Fall suggests the far-left keys (A0,
  B0, C1…), which songs rarely use.
- **Loops.** Shift-drag on the timeline or press `L`. Right-click the timeline for *Speed up each
  pass*.
- **Hands.** Choose your part (Left, Both or Right). The other hand is dimmed and keeps playing in
  your headphones.

## Keys

| Key | Action |
| --- | --- |
| Space | Play / pause |
| ← / → | Back / forward 5 s (Shift: 1 s) |
| ↑ / ↓ | Speed ±5 % |
| Home | Back to the start (or the loop start) |
| Tab | Switch between Free play and Learn |
| 1 / 2 / 3 | Your part: left / both / right |
| L | Loop on or off |
| M | Add a marker here |
| , | Settings |
| Ctrl+O | Open a MIDI file |

Scroll over the waterfall to scrub (when paused). Ctrl+scroll zooms. You can also click keys on the
on-screen piano: this counts as playing them, which is handy for trying Learn mode away from the
piano.

## Setup tips

- **Use headphones** for Fall's own sound. Then the microphone only hears your real piano.
- In *Windows Settings → Privacy & security → Microphone*, allow desktop apps to use the microphone.
- If notes are missed, turn off *Audio enhancements* for the laptop microphone (Sound settings →
  your microphone → Advanced). Noise suppression and auto-gain dull the start of each note.
  Raising *Sensitivity* in Fall's settings also helps.

## Download

Every push to `main` builds `Fall-windows-x64.zip` on GitHub Actions (look under the run's
*Artifacts*). Tagged versions (`v0.1.0`, …) also appear on the Releases page. See
[docs/ci.md](docs/ci.md).

## Building

You need CMake 3.22+ and a C++20 compiler (MSVC 2022 on Windows). JUCE and Catch2 are fetched
automatically.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

Useful options:
- `-DFALL_LTO=OFF` makes links faster while developing.
- `-DFALL_BUILD_TESTSYNTH=ON` builds the tiny VST3 used by the plugin-hosting test.

The app can take screenshots of itself, for CI and UI checks:

```sh
Fall --screenshot shot.png --midi tests/data/ode_to_joy.mid --mode learn --seek 3 --play --wait 2
```

The flags are `--frames-dir DIR`, `--loop A:B`, `--marker T:PITCH:NAME`, `--hands left|right`,
`--speed X`, `--settings`, `--calibrate` and `--auto-learn`. See `LaunchOptions` in
`src/app/MainComponent.h`.

## Layout

| Path | What |
| --- | --- |
| `src/core` | Song model, MIDI import, the `Player` that plays, loops and waits in Learn mode (no UI, unit-tested) |
| `src/detect` | Real-time note detection from the microphone ([docs/detection.md](docs/detection.md)) |
| `src/app` | The JUCE desktop app: audio engine, VST3 hosting, waterfall, controls |
| `tests` | Catch2 tests, plus a VST3 hosting test |
| `tools` | MIDI fixture generator, detector evaluation tools, the test synth |

## License

AGPL-3.0-or-later. See [LICENSE](LICENSE). Built with [JUCE](https://juce.com) (AGPLv3).
