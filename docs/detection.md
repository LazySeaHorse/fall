# Note detection from a laptop microphone

Fall listens to an acoustic piano through one microphone and decides, one note at a time, which key
was struck. The code is `src/detect/NoteDetector.cpp` (about 300 lines). Pitch tracking uses
[aubio](https://aubio.org) (GPLv3), built from source by `cmake/Aubio.cmake`.

## Why it was rewritten (November 2026)

The first detector tried to transcribe chords. It compared every short moment with the previous
second and called anything "unusually different" a new note, then scored all 88 keys against the
new sound. Nothing in it asked whether there was any sound at all. In a quiet room through a laptop
mic (room noise around -33 dBFS) it registered about five note starts per second of silence. The
bar for accepting the expected key was at chance level: at 40.6 s of one recording, C5 was
accepted in dead silence with the same score a random key got.

The rewrite follows three rules:
- silence must never trigger anything, by construction;
- one note at a time;
- use a standard, well-tested pitch tracker.

## How it works

Every hop (256 samples at 48 kHz, about 5 ms):

1. **Room level.** The level of the 60 Hz high-passed input, averaged over about 40 ms, is tracked
   as the room's noise floor. The estimate drops quickly in quiet moments and rises slowly (1 dB/s)
   during notes.
2. **Strike.** A strike needs both of these:
   - The 10 ms level is at least *gate* dB above the room. Silence never passes this.
   - Something jumps, either of:
     - the level rose at least *rise* dB above its minimum 15–65 ms earlier;
     - the 2 kHz+ band (hammer noise) rose at least 2×*rise*. This catches a softer strike of a
       key that is still ringing.

   A new strike needs 90 ms after the last one.
3. **Pitch.** aubio's YIN-FFT tracker runs on an 85 ms window. Only hops whose confidence is at
   least *minConf* count:
   - the finger thump and clicks score about 0.2;
   - a struck string scores 0.5 to 0.98.

   After a strike, the first pitch that stays within half a semitone for *need* hops (about 30 ms)
   is the note.
   - It is reported once: `DetectedOnset::pitches`.
   - It counts for learn mode if the song is waiting for it: `expectedHits`.
   - Jump keys must hold twice as long: `watchHits`.
   - If nothing steady shows up within 500 ms, nothing is reported. Bass notes can take 150–250 ms.
4. **Details:**
   - **Note that was already sounding.** It is ignored for the first ~40 ms after a strike, because
     it is still inside the pitch window.
   - **Same key within 160 ms.** Counts as the same strike (an attack that rises in steps).
   - **Legato.** A different key that holds steady for twice as long, while the previous note is
     still loud, counts as a strike even without a level jump. Octave flips of a decaying note
     don't count.
   - **Tuning.** It is learned from the notes C3–C6 heard so far. A pitch within 0.65 semitones of
     an expected key's neighbour snaps to the expected key, because high notes read sharp.
   - **96 kHz.** Input above 60 kHz is averaged down for the tracker. aubio finds no pitch at 96 kHz.
   - **aubio patch.** `cmake/Aubio.cmake` patches aubio 0.4.9's YIN-FFT, which otherwise reports
     confidence 0 for every pitch below ~1300 Hz.

Learn mode: the app (`AudioEngine`) treats one heard note of a chord as playing the whole chord.

### Sensitivity (Settings, 0..1, default 0.6)

| s | gate (dB over room) | rise (dB) | minConf | steady for |
|---|---|---|---|---|
| 0 | 16 | 8 | 0.85 | 45 ms |
| 0.6 | 10 | 5.6 | 0.55 | ~30 ms |
| 1 | 6 | 4 | 0.35 | 20 ms |

## Results

**User recording 1.** A grand piano, laptop mic, raw input, 60 s. The user played each C, G and B
up the keyboard, with long silences in between.
- 0 detections in silence. The old detector registered 308 note starts here, and accepted the
  expected note in silence.
- Every key from B1 to C8 was reported correctly, at 37–90 ms after the strike for C3 and up and
  75–170 ms for C2 and below.
- C1, A0 and G1 were not found.

**User recording 2.** *River Flows in You* in learn mode. The user mostly played the melody an
octave too high.
- Those notes are reported as the keys actually played, and not accepted.
- The correct notes are accepted.

**Rendered dataset.** FluidR3 grand through `tools/detect/gen_dataset.py` mic chains. Learn-mode
recall:

| | scale | Ode to Joy | repeated | mixed | chords | pedal | fast |
|---|---|---|---|---|---|---|---|
| clean | 84% | 81% | 61% | 49% | 26% | 10% | 2% |
| medium | 81% | 58% | 55% | 40% | 37% | 6% | 2% |
| hard | 55% | 44% | 45% | 25% | 37% | 1% | 2% |

- On the scale, A0–A1 are always missed and B1–C2 sometimes; the rest is ~100%.
- These files are two-handed, pedalled or fast (about 10 notes/s), played at the recording's tempo.
  A single-note tracker is not built for that.
- CPU: about 1% of one core.

## Known weaknesses

- **Bass.** Below ~C2, notes are slow (150–250 ms). Below ~B1 they are often not found: the
  fundamental is weak through a laptop mic and the 85 ms window is short.
- **Two hands, pedal, fast runs.** The tracker reports one pitch, usually the loudest or newest note.
  Learn mode therefore lets one note of a chord count. Use *Your part: right* for melodies.
- **Soft repeats.** A soft repeat of a key that is still ringing loudly can be missed.
- **Octave errors at the top.** Some top-octave notes can read an octave or a twelfth low in
  synthetic tests (G#6, G#7), and a C7 in a dense synthetic passage read as C8. Real recordings so
  far read the top correctly.

## Tools

- **`fall_detect_eval --replay rec.wav rec.csv`.** Replays a *Save what Fall heard* pair and prints
  what the detector reports now. Pass a CSV with only a header for free play.
- **`fall_detect_eval rec.wav played.mid`** (or `--dir data/generated/<level> --truth
  data/generated/midi`). Runs a learn-mode simulation, wrong-note rejection, display
  precision/recall and CPU. Options: `--only learn|wrong|display`, `--sens`, `--block`, `-v`.
- **Trace.** Configure with `-DFALL_DETECT_TRACE=ON`, then set `FALL_T0` / `FALL_T1` (seconds) to
  print every hop: level, floor, pitch, confidence and attack.
- **`tools/detect/gen_dataset.py`, `gen_noises.py`.** Render the dataset (git-ignored, in
  `data/generated/`).

## Integration notes

- Call `prepare()` with the device rate (tested at 44.1, 48 and 96 kHz). Any block size works.
- Call `setExpected()` with the notes the song waits for, and `setWatchKeys()` with the jump keys.
- Each report is one strike. Pass `expectedHits | watchHits` to the player.
