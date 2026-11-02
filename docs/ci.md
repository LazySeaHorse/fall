# Continuous integration

Workflow: `.github/workflows/build.yml`. It runs on pushes to `main`, pull
requests, `v*` tags and manual dispatch (Actions tab, "Run workflow"). A newer
push to the same branch or PR cancels the older run; tag builds are never
cancelled.

## Jobs

### `windows` (windows-latest, MSVC x64, Release)

This job produces the real product.

1. Restores cached JUCE and Catch2 sources (`deps-cache/`, keyed on the tags
   in the workflow) and passes them to CMake through
   `FETCHCONTENT_SOURCE_DIR_*`. The tags in the workflow's `env:` block must
   match `CMakeLists.txt`; the first step fails if they drift.
2. Configures with Ninja under `ilammy/msvc-dev-cmd` (x64), Release, a static
   MSVC runtime (so `Fall.exe` needs no VC++ redistributable) and
   `-DFALL_BUILD_TESTSYNTH=ON`.
3. Builds everything: `Fall.exe`, `fall_tests`, the `FallTestSynth.vst3` test
   plugin and `fall_plugin_host_test`.
4. Runs `ctest --output-on-failure`: the Catch2 unit tests plus `plugin_host`,
   which loads `FallTestSynth.vst3` through JUCE's VST3 hosting code.
5. Smoke test: `Fall.exe --screenshot shot.png --midi tests/data/ode_to_joy.mid --seek 6`
   with a 60 s timeout. The job fails if the process hangs, exits non-zero, or
   `shot.png` is missing or empty.
6. Uploads artifacts `Fall-windows-x64` (a zip with `Fall.exe` and `LICENSE`)
   and `screenshots-windows` (`shot.png`).

### `linux` (ubuntu-24.04, GCC, Release)

Installs the JUCE system packages (ALSA, X11, FreeType, Fontconfig, GL, Xvfb),
builds with ccache, runs `ctest` and the same screenshot smoke test under
`xvfb-run`, and uploads `screenshots-linux`. It exists to catch portable bugs
quickly and to give a second screenshot; nothing from it is released.

### `release` (tags `v*` only)

Runs after `windows` and `linux` succeed. It downloads `Fall-windows-x64` and
attaches `Fall-windows-x64.zip` to the GitHub Release for the tag using `gh`
(creating the release with generated notes, or uploading to an existing one).
Tags containing a `-` (for example `v0.2.0-rc1`) are marked as pre-releases.

## Getting the `.exe`

- Any run: open the run in the Actions tab, scroll to "Artifacts" and download
  `Fall-windows-x64`. Unzip it (the download is a zip containing
  `Fall-windows-x64.zip`) to get `Fall.exe`.
- Releases: download `Fall-windows-x64.zip` from the Releases page.

Artifacts are kept for the repository's default retention (90 days).

## Cutting a release

1. Bump `project(Fall VERSION ...)` in the top-level `CMakeLists.txt` and merge
   to `main`; wait for CI to go green.
2. Tag and push:

   ```
   git tag v0.2.0
   git push origin v0.2.0
   ```

3. The `release` job publishes the release when both build jobs pass. Edit the
   generated notes on GitHub if needed. To redo a failed release, re-run the
   failed jobs; re-running `release` replaces the existing asset.

## Running the same steps locally

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DFALL_BUILD_TESTSYNTH=ON
cmake --build build
xvfb-run -a ctest --test-dir build --output-on-failure   # drop xvfb-run on Windows/macOS
```

`FETCHCONTENT_SOURCE_DIR_JUCE` / `FETCHCONTENT_SOURCE_DIR_CATCH2` point CMake at
existing checkouts instead of downloading. Add `-DFALL_BUILD_APP=OFF` to skip
the app.

## Test plugin

`tools/testsynth/` builds `FallTestSynth.vst3`, a polyphonic sine instrument
with one `gain` parameter and state save/restore. It is only built with
`-DFALL_BUILD_TESTSYNTH=ON` and is never copied into system plugin folders.
`tests/plugin_host/` builds `fall_plugin_host_test`, which scans the bundle,
instantiates it at 48 kHz / 256 samples, plays a note, checks the audio is
non-silent and then decays to silence, and round-trips the state.
