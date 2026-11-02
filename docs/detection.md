# Note detection from a laptop microphone

`src/detect/NoteDetector.*` hears the user's acoustic piano through a (laptop) microphone and tells
the player engine which notes were struck. Plain DSP, no ML, no audio files needed at runtime.

The job is mostly **verification**: in learn mode we know which chord the song is waiting for, so
the detector answers "did something new start, and does it contain the expected pitches?". False
accepts are tolerated, misses and latency are not. A looser "what was pressed" estimate is produced
for the on-screen keyboard, and jump keys (A0/B0/C8) use much stricter rules.

## Algorithm

```
mic -> ring buffer (all sample rates, any block size)
    -> every 256 samples (5 ms): 1024-pt FFT -> log(1 + |X| / noise floor)   (level invariant)
       global spectral flux (minus the common level rise = AGC recovery)
       "comb flux": the same, restricted to bins where the EXPECTED keys' partials live
       adaptive threshold: z-score against median / MAD of the last 1.4 s    -> onset (peak pick, 1 hop lookahead)
    -> at onset + 30 / 65 / 90 ms (and +240 ms for jump keys, calibration):
       FFT of the audio ENDING there (2048 / 4096 / 8192 pts) and of the audio ending 4 ms BEFORE the onset
       per bin: is it a spectral peak (prominence over the local lower quartile)
                AND clearly stronger than before the onset (newness)?  -> evidence 0..1
       per key: weighted fraction of its inharmonic partial series with evidence = score F
         expected keys   G3 and up: "tuner" fit - grid search over tuning offset c and inharmonicity
                         scale; only partials within ~6 cents of the fitted string count (score F')
                         F' >= 0.20 / bass: F >= 0.35 (+ weak-onset penalty), not beaten by a
                         non-expected neighbour key, AND >= ~60% of the new power in the key's range
                         lies on its fitted partials (or other expected keys' partials)
         display pitches greedy "explain and subtract" with chance-corrected scores
         jump keys       late window only (hammer click over), stricter evidence, sole/clean explanation
```

Details that matter:

* **Precision, not presence (Nov 2026).** The first version asked "do >= 30% of this key's partials
  show a new peak inside their +-30-cent windows?". A finger snap, a clap or a voice fills such
  windows by chance (a snap's reverb tail is full of random spectral peaks), so they satisfied the
  expected chord 15-20% (voice: ~90%) of the time. Now (1) the key's partials must agree with ONE
  string: a grid search over the tuning offset (-30..30 cents) and the inharmonicity scale
  (B x 0..5) predicts every partial, and each partial earns credit only for a peak within
  max(6 cents, 0.3 bins) of its prediction; and (2) most of the NEW power (post - pre) in the key's
  frequency range must lie on those fitted partials (+-1.5 bins) or on other expected keys'
  partials. Broadband noise puts its power everywhere, a voice or a wrong key puts it elsewhere.
  Bass keys (< G3) keep the window score: their partials are too messy for a tight fit, and the
  energy check alone keeps noise out there.
* **Auto-tuner.** Home pianos are rarely at A440, and the precise fit needs the right reference. A
  histogram of all clear tonal peaks (100-2000 Hz) folded onto the equal-tempered grid gives a coarse
  lock (used when off by more than 12 cents); after that, the median fitted offset of every 9
  accepted notes refines it. Uncalibrated key models are rebuilt at the new tuning (cheap, rare).
  `NoteDetector::getTuningCents()`; shown in Settings -> Listening.
* **Tried and rejected: reassignment tonality.** Requiring the reassigned frequency of a peak bin
  and its neighbours to agree (a sinusoid's main lobe points at one frequency, noise doesn't)
  separates noise poorly in onset windows: the note starts inside the window, which breaks the
  sinusoid model. Still in the code (`tonalTol`, off by default) for experiments.

* **Only new energy counts.** Evidence needs `P_post / P_pre` to be high, so a ringing (pedalled)
  note is not reported again, while a repeated key is (its strike adds energy on top of the
  ringing partial; expected keys accept a smaller rise than display/jump keys).
* **Incremental reports.** One onset can be reported several times (stage 1, 2, 3): the host gets
  the early hits at +30 ms and bass notes at +80..90 ms. `DetectedOnset::samplePosition` is the same
  for all reports of an onset; union them. A later onset cuts the windows of the previous one.
* **Stale-stage protection.** `setExpected()` stamps the time; onsets that began before the stamp
  never satisfy the new chord. Calling `setExpected()` with an identical set after a hit counts as a
  new chord (repeated notes). Calling it every callback with an unchanged set is harmless.
* **Partial model.** `f_h = h f0 sqrt(1 + B h^2)` with a typical `B(pitch)`, +-28 cents tolerance
  plus the B range, weights `h^-0.6 x mic response (120 Hz..9 kHz)`. Bass keys therefore live on
  partials 6..40 (150 Hz..1.5 kHz). Peak positions are interpolated to sub-bin accuracy so key
  windows can be narrower than the FFT resolution (this is what separates adjacent semitones).
* **Level independence.** Everything is a ratio (log-magnitude differences, peak vs. local floor,
  z-scores). A slow rise of all bins (compressor release) is estimated as a low percentile and
  removed. Measured: no change between -30 dB and +10 dB input gain.
* **Calibration.** `beginCapture(pitch)` waits for the next clear onset, analyses the sustained part
  (160 ms window starting 80 ms after the strike), fits the tuning offset and `B`, snaps each
  partial to the nearest peak and stores `(h, Hz, weight = relative loudness)` per key
  (versioned binary, `Calibration::toBinary/fromBinary`). Calibrated keys are scored with their
  measured partials (+-22 cents, weights from the real mic/room) and a 25% lower threshold;
  uncalibrated keys are bent towards the nearest calibrated neighbours (tuning, B, weights).
* **Octave check.** The key an octave above (C7 for C6) has a partial wherever the expected key has
  an even one, so it fits the expected key's model about as well as the key itself; before this
  check a clean C7 satisfied an expected C6 almost every time (23 of 23 in the unit test). For
  expected keys from C4 up, `oddShare()` measures the new power on the fitted partials 1..6 above
  the local spectral floor and takes the share on the ODD ones (1, 3, 5), which only the expected key
  produces: a real note puts 0.2..0.9 there, the octave above 0.00..0.08 (real piano, laptop mic).
  Below `oddMin` (0.10) the key is rejected. Partials that another expected key also produces (an
  octave or a fifth in the chord) are left out. Below C4 the check is off: the synthetic piano's
  low partials 3 and 5 are too weak to judge. The display picker uses the same test before it
  prefers the lower key of an octave pair, and shows a key one or two octaves above a not-yet-heard
  expected key at a lower threshold (`octaveHintSel`), so the app can say "you played C7, the song
  wants C6".
* **Jump keys.** Late analysis window (the click is gone), +6 dB stricter prominence/newness, the
  key must be a pick of the explain-and-subtract pass, must not be explainable by a lower key
  (octave/twelfth...), rich keys (A0, B0) additionally need their *independent* partials
  (h = 1,5,7,11...) that no lower note can produce, and keys with one partial (C8) are only accepted
  as the sole explanation of the new energy.

CPU: ~0.9% of one core (9 ms per second of audio) on the ARM64 dev box, p99 of 0.9 ms per 10 ms
block, worst block 2 ms (an onset's second-stage FFTs). `process()` allocates nothing, takes no lock.

## Tuning knobs

All in `NoteDetector::Impl::Tune`, derived from `setSensitivity()` (0..1, default 0.6) in
`applySensitivity()`. Any of them can be overridden by name with `setTuningOverride()` or
`fall_detect_eval --param name=value` (for sweeps): `zOnset`, `zComb` (onset thresholds), `promLo/Hi`,
`newLo/Hi` (evidence ramps, dB), `expectedMin`, `expectedFitMin`, `fitTolCents`, `fitTolBins`,
`fitMinPitch`, `energyMin`, `energyLobe`, `autoTune`, `useFit`, `expectedNewRelief`, `weakOnsetPenalty`,
`settleStrength`, `neighbourMargin`, `measuredScale`, `displayMin`, `displaySel`, `chanceCoef`,
`watchMin`, `watchSel`, `watchContrast`, `watchIndependent`, `watchStrictDb`, `subharmonicMin`,
`watchPrefilter`. Timing constants (stage delays 30/65/90/240 ms, refractory 50 ms, settle 150 ms)
are `constexpr` at the top of the class.

Sensitivity (medium material, 6 non-fast pieces): 0.3 -> recall 85.5%, wrong-note accept (+-1
semitone, clean) 1-2%; 0.6 -> 95.9% / 5-5.5%; 0.9 -> 98.8% / 7%, but 56 premature accepts/min
(ambient bumps start satisfying the chord). **Keep 0.6.**

## Tools and data

* `tools/detect/gen_dataset.py` renders scales over the whole range, chords, repeated notes, fast
  passages, pedal passages, a random piece and Ode to Joy with fluidsynth (FluidR3 acoustic grand)
  and runs sox "laptop mic" chains (`clean`, `easy`, `medium`, `hard`: high-pass 150-200 Hz,
  low-pass 7-8 kHz, reverb, pink noise down to ~25 dB SNR, compressor/AGC, level wobble) into
  `data/generated/<level>/` (git-ignored) with MIDI + CSV ground truth in `data/generated/midi/`.
* `tools/detect/gen_noises.py` synthesises non-piano sounds (finger snaps, claps, knocks, taps,
  typing, a spoon on a mug, a sung vowel) through the same mic chains:
  `data/generated/<level>/noises.wav` + `data/generated/midi/noises_events.csv`.
  `fall_detect_eval --transients <wav> <csv>` sets a random expected chord before every event
  and reports how often it is (wrongly) satisfied, per kind.
* `fall_detect_eval` (target in `tools/detect_eval`):
  `fall_detect_eval --dir data/generated/medium --truth data/generated/midi [--quiet]` or
  `fall_detect_eval rec.wav played.mid [--offset s]` for real recordings (MIDI of what was played,
  or the CSV format `onset,pitch,duration,velocity`). Reports the learn-mode simulation, wrong-note
  rejection (expected = real chord shifted by +-1, +2, +7 semitones, or one/two octaves below
  what sounds: the "played 8va / 15ma" columns), jump-key fires, display
  precision/recall/F1 (+-50 ms), per-octave breakdown, CPU per 10 ms block. `--csv f` dumps all
  reports, `--block 0` uses random block sizes, `--sens`, `--param k=v`, `--calibration f`,
  `--make-calibration out.cal` (builds a calibration from a recording in which every key is
  played once, e.g. `scale.wav`), `-v` lists misses and odd accepts.

## Measured results

**Current detector (fitted partials + energy check + auto-tuner), sensitivity 0.6.**

| level  | learn note recall | chords complete | wrong-note accept, clean ctx (+-1 st) | display P / R / F1 |
|--------|------|------|---------|----------------------|
| clean  | 94.6% | 92.1% | 5.5% / 9.0% | 97.4 / 93.7 / 95.5 |
| medium | 93.0% | 90.1% | 5.0% / 4.5% | 97.6 / 91.6 / 94.5 |
| hard   | 84.5% | 79.7% | 4.5% / 4.5% | 97.8 / 83.0 / 89.8 |

False accepts on non-piano sounds (random expected chord per event, 96 events per kind):

| sound | old (easy / medium) | now (easy / medium) |
|-------|------|------|
| clap | 19.8% / 18.8% | 5.2% / 3.1% |
| snap | 2.1% / 6.2% | 1.0% / 4.2% |
| typing | 3.1% / 16.7% | 1.0% / 6.2% |
| knock | 0.0% / 3.1% | 3.1% / 4.2% |
| sung vowel | 88.5% / 86.5% | 49% / 51% |

The vowel stays high when its formant harmonics coincide with the expected notes, which is
physically close to singing the note. Cost: about 3 points of recall on synthetic material (the
old detector: 95.7% / 88.5% medium / hard). Detuned pianos (sox pitch shift of the medium set):
30 cents flat 89%, 20 cents sharp 89%, 45 cents flat 81%; the tuner reads -27, +21, -42.
Fitting all keys including the bass (`fitMinPitch=21`) cuts semitone wrong-note accepts to 0.5%
but costs 6 more points of recall; misses hurt more than false accepts, so the bass keeps the
window score.

**First version (for reference):**

Synthetic data (FluidR3 piano through the sox mic chains, 48 kHz, 10 ms blocks), six pieces
(chords, ode_to_joy, mixed, pedal, repeated, scale; 1480 notes, 915 chords). Learn mode: the
expected chord advances like `Player` does, a chord that is not heard within 0.3 s of the next one
is counted as a miss.

| level  | learn note recall | chords complete | wrong-note accept, clean ctx (+-1 st) | dense ctx (+-1 st) | display P / R / F1 |
|--------|------|------|---------|--------|----------------------|
| clean  | 96.6% | 95.0% | 5.5% / 5.0% | 7.6% / 6.7% | 96.1 / 94.2 / 95.1 |
| easy   | 95.9% | 94.5% | 5.5% / 5.0% | 6.9% / 6.1% | 95.0 / 92.8 / 93.9 |
| medium | 95.9% | 94.1% | 5.5% / 5.0% | 7.0% / 6.0% | 95.5 / 92.4 / 93.9 |
| hard   | 87.2% | 83.6% | 5.0% / 4.5% | 6.5% / 5.6% | 93.9 / 82.5 / 87.8 |

Wrong-note accepts: +2 semitones 7-8%, a fifth up 16% (clean) / 13% (dense): consonant
intervals share partials. "Clean context" = nothing else ringing, "dense" = other notes sounding.

Latency, medium level (ms from the real strike until `process()` returned the hit; includes the up
to 10 ms block):

| register | p50 | p90 | p99 | notes missed |
|----------|-----|-----|-----|-----|
| bass A0-B2 | 80 | 100 | 110 | 9.7% |
| mid C3-B4 | 40 | 70 | 246 | 3.9% |
| treble C5-C8 | 40 | 40 | 240 | 2.3% |

(p99 values near 240 ms are notes whose second chance came from the next onset's stage.)
The reported onset time is within +-5 ms of the MIDI strike (median offset 3 ms).

Other measurements:

* 44.1 kHz (medium, scale + ode + mixed): recall 99.0%, display F1 98.6%; random block sizes give
  the same recall (hit positions are identical across block sizes, see the unit test).
* Calibration (built from `scale.wav`, evaluated on the other five pieces, medium): recall
  95.1% (generic: 95.7%), wrong-note accept clean 1.5-2.5% (generic 5%), dense 3.4-5% (7-9%).
  The synthetic piano is close to the generic model, so a real piano should gain more.
* Jump keys (A0/B0/C8) over ~7 minutes of adversarial material (full-range scales, random fast
  lines, pedalled arpeggios): 1 false fire (0.14/min) at medium, 0.14/min easy and hard; but only
  **1 of 6 deliberate presses recognised** (the C8 in the isolated test, plus the unit test with
  synthetic tones; A0/B0 via the FluidR3 samples never).
* Fast passages (16ths at 140 bpm, 105 ms apart): learn recall only 32-38%, display F1 17%.

## Known weaknesses

* **Wrong notes that share partials** (fifth, octave, third) are accepted a lot, by design (false
  positives are allowed). With a dense, pedalled texture even +-1 semitone is accepted 6-8%.
* **Octave below**: an expected key one octave ABOVE the played one is accepted (its whole series
  is contained in the lower note's). The opposite mistake (playing the octave above) is rejected
  from C4 up, but still accepted ~15% of the time in the bass, where the check is off.
* **A0/B0 jump keys** are barely audible on a laptop mic and every one of their partials coincides
  with an ordinary bass note of a chord; they fire rarely. C8 has one usable partial. Prefer
  calibrated jump keys and consider keys such as B0/C8 only if a miss is acceptable.
* **Bass** (A0-B2): ~10% missed, 80-100 ms latency; the three lowest octaves lean on partials 6+.
* **Notes < ~150 ms after another onset** (fast trills, 16ths at >120 bpm) are mostly missed: onsets
  closer than 150 ms must be 2.2x the threshold (secondary bumps of reverb/compressors otherwise
  trigger false hits). Fine for learn mode (the song waits), not for free transcription.
* A very soft repeat of a loud ringing note (more than ~6 dB below the ringing partials) is missed.
* Hard level (SNR ~20 dB in reverberant room with strong compressor): 87% recall.
* **First real recording** (grand piano, laptop mic, raw mode, River Flows in You): the misses and
  false accepts were nearly all octave mistakes (the melody played one or two octaves above the
  song); the true notes in it are accepted. `fall_detect_eval --replay rec.wav rec.csv` replays a
  recording with the expected notes Fall had logged and prints what the detector reports now.
  *Settings -> Save what Fall heard* writes the last 90 s of microphone audio
  (`Documents/Fall recordings/fall-<date>.wav`) plus a CSV of what was expected and heard; replay
  the WAV with `fall_detect_eval` and sweep `--param`.
* **Windows voice processing.** Laptop microphones on Windows go through noise suppression /
  "Voice Clarity" / AGC, which treat a sustained piano tone as noise. Fall asks WASAPI for the raw
  stream (`AUDCLNT_STREAMOPTIONS_RAW`, patched into JUCE by `cmake/PatchJuceWasapiRaw.cmake`;
  Settings -> Raw microphone). Devices without raw support ignore the request.

## Integration notes

* Expected sample rates: anything >= 16 kHz works; tested at 44.1, 48, 96 kHz. Call `prepare()` with
  the device rate and the largest block you will pass (any size is accepted).
* Call `setExpected()` with the chord the song waits for (not further ahead); call it again,
  even with an identical set, when a chord is completed and the next one is a repeat.
  `setExpected({})` outside learn mode.
* Treat reports as incremental: feed `expectedHits | watchHits` straight to `Player::notePlayed`.
  Use `onset.samplePosition` to deduplicate for the display.
* Jump keys: `setWatchKeys()` only while jump keys are bound; they cost an extra analysis stage
  only when a jump key already looks plausible. Their latency is ~250 ms.
