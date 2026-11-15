# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

# Twin Crowns – a dual overdrive LV2 plugin for pi-Stomp v3
 
## Goal
An LV2 plugin for the pi-Stomp v3 (Raspberry Pi 5) that uses the circuit DSP of
**Monarch of Tone (MoT)**, a circuit model of a famous boutique two-in-one Bluesbreaker-style
overdrive. Open source under the AGPLv3 (`LICENSE`, `README.md`), by IgnatzMcJockel. Keep
real names, private hosts and addresses out of the repo: it may be published as it is.
**The priority is staying as close to the original as possible:** when fidelity and
simplicity conflict, choose fidelity.
**Don't name the original pedal or its maker** anywhere in the repo or in commit messages, only
paraphrase it as above, the way MoT's README intro does. The repo may become public.
 
## Current state
- Done and tested on x86_64: the half-band port (`src/HalfBandOversampler.h`), the JUCE- and
  LV2-free signal path (`src/CrownsEngine.h`) and the LV2 layer (`src/crowns.cpp`, `ttl/`, `Makefile`).
  The output is bit-identical to MoT, also while controls change (`make nulltest`).
- The Pi builds the plugin and passes `make test`. It runs there in a pedalboard, and
  footswitches and encoders are assigned and work. The pedal GUI is deployed and its controls
  work in MOD-UI.
- The dev machine is Windows, so the x86_64 `.so` is built and tested in WSL (Ubuntu 22.04).
  The toolchain is installed there.

## Basis: Monarch of Tone
- Repo: https://github.com/tehguitarist/Monarch-of-Tone (AGPLv3), as a Git submodule under
  `third_party/MoT`, pinned to `d7c52933fa3422166d17537efbcbf9fa41915ee9` (2026-08-21).
- The circuit DSP lives in `third_party/MoT/src/dsp/` (`MonarchChannel.h` plus the
  stages). It depends **only on chowdsp_wdf and the standard library, not on JUCE**.
- chowdsp_wdf is our own submodule, `third_party/chowdsp_wdf`, on exactly the commit MoT pins
  for its `libs/chowdsp_wdf` (`tests/check_pins.sh`, run by `make test`, checks that). MoT's own
  submodules (`libs/chowdsp_wdf`, `libs/xsimd`, `libs/JUCE`) stay unchecked-out: `libs/xsimd` is
  linked by MoT's CMake, but nothing in `src/` includes it, and chowdsp_wdf only uses it when
  `XSIMD_HPP` is defined (xsimd included first). **Never use `--recursive`.** The null test
  under Local testing is the one exception, in a separate clone.
- The reference for signal chain, oversampling, bypass and levels is
  `third_party/MoT/src/PluginProcessor.cpp` (`processPedalChannel`, `processBlock`,
  `updateOversampling`, `pushParams`, parameter layout with ranges and defaults).
  **Port this logic 1:1, don't guess.**
- Don't modify code in the submodule. All adaptations go into our own wrapper.
- MoT ships its own `CLAUDE.md` for its JUCE development. For this project,
  this file takes precedence: its build commands, `--recursive` and validation agents don't
  apply here. Its `.claude/rules/` (`dsp.md`, `architecture.md`, `circuit.md`) are still the
  reference for why the DSP works the way it does.
- MoT's prose lags its code. Comments claiming the *whole* channel runs oversampled
  (`PluginProcessor.cpp:245`, `PluginProcessor.h:174`, `MonarchChannel.h:741`) and the sections
  `dsp.md` marks as historical predate v1.5. MoT's README also still says Yellow → Red. Where
  they disagree, follow the executed code.

## Wrapper architecture (ported from MoT)
Signal flow for one mono strip. **Red runs first, Yellow second**: MoT's ids and parameter lists
name Yellow first only for historical reasons. We put Red first everywhere a user sees the order,
because MOD-UI's pedalboard runs left to right: the ports list Red first, and the pedal face is
MoT's mirrored, Red on the left (see Pedal GUI). A real pedal has its input jack on the right,
so its face reads right to left, Red on the right.
```
in → ×dB(input_trim)·0.87 → Red channel → Yellow channel → ×dB(output_trim)/0.87 → out
```
Each channel, as in `processPedalChannel`:
```
dry = x
processStage1             @ fs      Stage 1 is linear, so it stays at the base rate
upsample
processPreOs, processClip @ osRate  only the nonlinear span is oversampled
downsample
processPost               @ fs      Tone/Volume, also linear
out = dry + wet·(processed − dry)
```
- Instances: `MonarchChannel red { true }`, `yellow { false }`. Hi-Gain is that fixed ctor flag
  on Red, not a parameter.
- Prepare with `prepareLinear (osRate, fs, fs)` then `prepareClip (osRate)`, once per channel,
  where `osRate` is `fs` times the factor. Don't use `prepare (fs)`, `processPre` or
  `processSample`: they run everything at one rate.
- Each channel has its **own** up/downsampler, so the latency is the sum of both. MoT doesn't
  delay-compensate `dry`.
- `wet` is a 5 ms ramp, 1 = on, 0 = bypassed. Once it has settled at 0 the channel is skipped
  entirely, including its oversampler. The trim gains use 5 ms ramps too, on the linear gain.
  All are JUCE `SmoothedValue<float>` in linear mode: `floor (0.005·fs)` steps (240 at 48 kHz),
  `step = (target − current) / steps`, and the last step lands exactly on the target.
- Setters (`setDrive`, `setTone`, `setVolume`, `setPresence`, `setClippingMode`,
  `setSupplyVoltage`) run at block rate, as in `pushParams`, and must also run before the first
  sample. `VolumePot` smooths itself; drive, tone and presence aren't smoothed.
- `circuitVoltsPerFS = 0.87` is private to `PluginProcessor.h`, so the wrapper has to copy it.
- The factor is a control, MoT's `oversampling_realtime`, switched as in MoT's
  `updateOversampling`. MoT prepares in `prepareToPlay` before the first `pushParams`, and in
  `processBlock` after each one. Keep that order: preparing resets the drive-keyed shelves until
  the next `setDrive`, so the block after a switch runs with them. A switch resets the
  oversamplers and channel state (a short dropout, as in MoT) but not the bypass and trim ramps.
  1× runs MoT's own branch, `processPost (processClip (processPre (x)))`, without an oversampler.
- Not ported: `TiltShelf` (`kEnabled = false`, and its `shelfMix`), `trim_lock` (UI only),
  meters, MoT's render oversampling (FIR, offline only).
- MoT's factory presets (`src/Presets.h`) are LV2 presets in `ttl/presets.ttl.in`, each also
  listed in `ttl/manifest.ttl.in`, or hosts won't find it. Like MoT's, they set only the knobs
  and modes, not on/off, trims or supply voltage.

## Half-band filter (port of JUCE)
MoT builds `juce::dsp::Oversampling<double> (numCh, log2, filterHalfBandPolyphaseIIR, true)`,
so `maxQuality` is on and integer latency is off. The reference is JUCE at MoT's pinned commit
`3cc224c3e84d6acbabf62358ba1ccbf133cd2095`, not JUCE master: `juce_Oversampling.cpp`,
`juce_FilterDesign.cpp` (`designIIRLowpassHalfBandPolyphaseAllpassMethod`).
- Port the design function and run it in `instantiate`. It depends only on transition width
  and stopband, not on the sample rate.
- Inputs for stage `n` (0 is nearest the base rate; 2× uses stage 0, 4× adds stage 1, 8× stage 2):
  width up `0.10f · (n == 0 ? 0.5f : 1.0f)`, width down `0.12f · (n == 0 ? 0.5f : 1.0f)`,
  stopband up `−90 + 10·n` dB, down `−75 + 10·n` dB. Keep the width as a `float` expression, as
  JUCE does: it's 0.05000000074…, not 0.05, and that changes the coefficients.
- Each stage: direct path gets `a[0], a[2], …`, delayed path `a[1], a[3], …`.
  Allpass: `y = a·x + s; s = x − a·y`, with separate state for up and down.
- Upsample: `x` through the direct path gives `out[2i]`, `x` through the delayed path gives
  `out[2i + 1]`. There is no gain factor.
- Downsample: `in[2i]` through the direct path, `in[2i + 1]` through the delayed path, then
  `out[i] = 0.5·(d + direct); d = delayed`.
- With more stages: up runs stage 0 first, down runs it last. Per sample this goes depth first,
  which gives JUCE's numbers, because each stage sees the previous one's output in order.
- Expected for stage 0: up has 7 coefficients with `a[0] ≈ 0.0457281476`, down has 6 with
  `a[0] ≈ 0.0542175265`.
- Latency: `Σ (up + down) / 2^(n+1)` per oversampler, where `up` and `down` are in samples of
  that stage's own rate (3.3005 and 2.9741 for stage 0, 2.8013 and 2.3801 for stage 1, 2.2944
  and 1.8553 for stage 2). Two oversamplers in series, rounded as in MoT: 0 samples at 1×, 6 at
  2× (6.27), 9 at 4× (8.87), 10 at 8× (9.90). MoT reports this even when channels are bypassed.
- Don't port `JUCE_SNAP_TO_ZERO`: it's a no-op outside Intel, so the Pi has none in MoT either.

## Why not JUCE's LV2 export
JUCE exports parameters as `lv2:Parameter` (patch parameters), not as control ports.
MOD-UI has no controller/MIDI assignment for those. The pi-Stomp's footswitches and
encoders send MIDI CC to mod-host, so **every control must be a classic
`lv2:ControlPort` input.**
 
## Target platform (pi-Stomp v3)
- Raspberry Pi 5 (Cortex-A76), Debian Trixie arm64, RT kernel
- Host: mod-host (LV2 on JACK), runs with SCHED_FIFO priority
- JACK: 48 kHz, period 128 frames (default on Pi 5), 2 periods
- SSH: `pistomp@pistomp.local`. `.local` names don't resolve in WSL, so use an `~/.ssh/config`
  entry `pistomp` with the Pi's IP
- Plugin directory: `/home/pistomp/.lv2`. mod-host uses
  `LV2_PATH=/home/pistomp/.lv2:/usr/lib/lv2`, so `~/.lv2` is scanned **first**.
  The plugin therefore needs its own unique URI.
- Services: `jack`, `mod-host`, `mod-ui`
- Logs: `journalctl -u mod-ui -u mod-host --since "5 minutes ago"`
- Check what's installed: MOD-UI HTTP `http://pistomp.local/effect/list`,
  port symbols via `/effect/get?uri=<URL-encoded URI>`
## Plugin requirements
- Plain LV2 C API (`lv2/core/lv2.h` from `lv2-dev`), no UI, no framework needed
- One plugin, **Twin Crowns** by McJockel, `urn:ignatzmcjockel:twin-crowns`, in
  `twin-crowns.lv2/` (`manifest.ttl`, `twin-crowns.ttl`, `presets.ttl`, `modgui.ttl`,
  `twin-crowns.so`, `modgui/`). `NAME` and `URI` in the Makefile set both; the TTL templates get
  them as `@NAME@` and `@URI@`. Internal names use `crowns` too: `src/crowns.cpp`,
  `CrownsEngine`, namespace `crowns`, the `crowns-` CSS classes and `modgui/*-crowns.*`.
- Until 2026-11 the plugin had another name, brand and URI, in `kot.lv2/`.
  It was renamed while no pedalboard used it, and `make install` removes the old `kot.lv2`.
- Mono: 1 audio in, 1 audio out
- Control input ports (take ranges and defaults from `PluginProcessor.cpp`):
  - per channel (`red_*` at indices 2–7, then `yellow_*` at 8–13, in signal order Red → Yellow,
    so MOD-UI's settings and lists show Red first):
    `on` (toggled, default 1; the inverse of MoT's `bypass_*`), `volume`, `drive`, `tone`
    (0–1, default 0.5), `presence` (0–1, default 0),
    `mode` (`lv2:enumeration`: 0 Boost / 1 Overdrive / 2 Distortion, default 1)
  - global: `input_trim`, `output_trim` (−18…+18 dB, default 0), `supply_voltage`
    (enumeration 0/1/2 → 9/12/18 V, default 0), `oversampling` (enumeration 0–3 → 1×/2×/4×/8×,
    default 2, where MoT defaults to 2×). It comes after the `latency` output.
  - Pedalboards, presets and MIDI bindings refer to ports by symbol, so indices could move once
    (Yellow used to come first). Symbols must never change.
- **MOD metadata in the TTL:** `lv2:DistortionPlugin` for the browser category, `mod:brand` and
  `mod:label` for the short LCD name, `units:db` on the trims, `lv2:integer` with
  `lv2:enumeration` on `mode` and `supply_voltage`. `rdfs:comment` is the description in MOD-UI's
  info dialog, which collapses line breaks, so keep it one paragraph.
- **Keep the URIs.** Pedalboards store plugins by URI, so changing one breaks every saved
  pedalboard that uses the plugin.
- **Real-time safety:** no allocation, no locks, no I/O in `run()`. Everything is
  prepared in `instantiate`/`activate`.
- **Arbitrary block sizes:** `n_samples` varies. Don't assume a fixed block buffer;
  process per sample or in chunks up to a fixed internal maximum size.
- **Oversampling:** 1×, 2×, 4× or 8× at run time, MoT's *live* IIR path, not its FIR render
  path. Every oversampler is designed in `instantiate`, so switching allocates nothing. Replace
  `juce::dsp::Oversampling` with an exact port (see Half-band filter). Only the clip span is
  oversampled (see Wrapper architecture). Report latency via a latency port, as MoT does; it
  changes with the factor.
- **Per-channel bypass:** click-free, port MoT's behavior (see Wrapper architecture).
- **Calibration:** 0 dBFS maps to 0.87 circuit volts (`circuitVoltsPerFS`, fitted so the rails
  start clipping at a −18 dBFS 1 kHz input). MoT's −12 dBu figure is only the VU meter's
  nominal. The pi-Stomp's absolute input level isn't documented (input gain is set on the
  device via Tweak2), so `input_trim` is for matching by ear.
- Keep denormals in mind: MoT's `ScopedNoDenormals` has to be replaced in `run()`
  (FPCR flush-to-zero on aarch64, MXCSR on x86_64, so it has to compile for both).
## Pedal GUI in MOD-UI (`modgui/`, `ttl/modgui.ttl.in`)
- The face is our own "Twin Crowns" design: raw cast aluminium (`modgui/metal-crowns.svg`) with
  marker writing (Permanent Marker and Rock Salt, Apache 2.0, in `modgui/`). The layout follows
  MoT's pedal face, mirrored: Red (A) sits on the left because it runs first, so the face reads
  left to right like MOD-UI. CSS classes name positions (`-left`, `-right`), not channels. Every
  control sits on the pedal. Knobs, switches, LEDs and footswitches are MoT's art; it stays in
  `third_party/MoT/assets/ui`, and the Makefile copies it into the bundle.
- `input-control-port` elements can share a port: a footswitch and its LED are two `switch`
  widgets on `yellow_on`, which only set the classes `on` and `off`. Knobs use
  `mod-widget-rotation="270"` with a single image. The three-way switches are `custom-select`
  with `enumeration-option` elements, which get the class `selected`.
- The middle footswitch and the LED under the crowns are MOD-UI's plugin-wide bypass
  (`mod-role="bypass"`, `bypass-light`), not MoT's channel bypass: MOD-UI and `mod-host` do that
  themselves, without our 5 ms ramps.
- CSS needs `{{{cns}}}` in selectors and `{{{ns}}}` on `/resources/` URLs, so it doesn't collide
  with other plugins' GUIs.
- `oversampling` has no control on the pedal: it's set in MOD-UI's settings.
- Controls need a `z-index` above 20, or MOD-UI's invisible drag handle swallows their clicks.
- After changing the layout, run `python3 modgui/make_screenshots.py` and
  `python3 modgui/check_clicks.py` in WSL. They use Chrome on Windows: the first renders
  `modgui/screenshot-crowns.png` and `thumbnail-crowns.png`, the second checks that every control is
  clickable.
- Changes to `modgui/*.html` or `*.css` only need `make install` on the Pi and Ctrl+F5 in the
  browser, with no restart and no audio interruption.

## Build
- From the repo root in WSL: `make -j8` builds the bundle into `build/lv2/`, `make test` runs
  the tests in `tests/`.
- Two targets: x86_64 locally for testing and aarch64 **natively on the Pi** (build via SSH).
- One-time setup on the Pi, already done: `sudo apt update`, then
  `sudo apt install --no-install-recommends build-essential` (`lv2-dev` and `git` were already
  there). To undo it, run `bash ~/crowns-restore/restore.sh` on the Pi and reboot. It removes the 34
  new packages and restores the 5 upgraded ones. WSL has a copy in `~/crowns-pi-restore`.
- The dependencies have copies next to this repo, under the same owner: `MoT`, `chowdsp_wdf`,
  `JUCE`, `xsimd`, each with a tag `twin-crowns-pin-<commit>` on the pinned commit. They are
  plain repos or forks, not pull mirrors, so upstream deletions or rewritten history never
  reach them. To update one, fetch from upstream and push additively (no `--mirror`, no
  `--prune`), or sync the fork, then tag the new pin. Very large pushes may hit an upload
  limit; import such repos on the server side instead.
  `.gitmodules` uses relative URLs (`../MoT`, `../chowdsp_wdf`), so every clone gets the copies
  next to the repo it came from. No `url.insteadOf` is needed anywhere.
  `tests/nulltest/setup_reference.sh` uses the copy next to `origin` where it can reach one,
  and upstream otherwise.
- After a fresh clone, fetch the submodules, non-recursively: `git submodule update --init`
- Flags: `-std=c++17 -O3 -fPIC -shared -fvisibility=hidden`, plus
  `-mcpu=cortex-a76` on the Pi
- Include paths: `-Ithird_party/MoT/src -Ithird_party/chowdsp_wdf/include`
  (the wrapper includes `dsp/MonarchChannel.h`)
- Makefile targets: `all`, `test`, `nulltest`, `install` (to `~/.lv2/twin-crowns.lv2`), `clean`,
  `deploy` (see below)
## Local testing
- Validate the TTL with `sord_validate` (package `sordi`; `lv2lint` isn't in Ubuntu's repositories):
  `sord_validate /usr/lib/lv2/schemas.lv2/*.ttl /usr/lib/lv2/core.lv2/*.ttl /usr/lib/lv2/units.lv2/*.ttl build/lv2/*/*.ttl`.
  Errors about `mod:` and `modgui:` properties are expected, since MOD's schemas aren't installed.
- Offline-render a WAV file with `lv2apply` (package `lilv-utils`) or `jalv`:
  `LV2_PATH=$PWD/build/lv2:/usr/lib/lv2 lv2apply -i in.wav -o out.wav -c yellow_drive 0.8 urn:ignatzmcjockel:twin-crowns`.
  The input WAV has to be mono.
- Null test against real MoT: run `bash tests/nulltest/setup_reference.sh` once, then
  `make nulltest`. The setup clones MoT with JUCE into `~/mot-ref`, patches `PedalRender` there
  and needs `cmake` and JUCE's Linux dependencies.
  - The expected result is **bit-identical** output, so any difference is a defect to explain.
    Deliberate mismatches run first, to prove the comparison can see one.
  - It covers all four factors with fixed controls through `lv2apply`, and controls changing
    during the render, the factor included. For those, `nulltest.py` loads `twin-crowns.so` itself and
    acts as the host.
  - The reference is built without JUCE's snap-to-zero, matching MoT on ARM. With it, MoT's own
    test signal differs by up to 3e-8 (about −150 dB) in 1–3 % of the samples.
  - Compare x86 only with x86: results built for the Pi can differ slightly.
  - Rerun it after every MoT update.
- Optional: pi-Stomp emulator with MOD Desktop
## Deploying to the pi-Stomp
1. From WSL, `make deploy` copies the needed sources to `~/twin-crowns` on the Pi, runs `make` and
   `make install` there, and restarts the services. It needs the `pistomp` SSH host entry.
2. The restart is `sudo systemctl restart mod-host mod-ui`, which is enough for MOD-UI to pick up
   a changed plugin or GUI. It interrupts audio. Then reload the browser page.
3. Check the logs, look for the plugin in `/effect/list`
4. In MOD-UI: assign `yellow_on`/`red_on` to footswitches, `drive`/`tone` to encoders
5. Watch CPU and XRUNs in the MOD-UI bar (above 75 % CPU or frequent XRUNs = too much)
## CI (Gitea Actions, `.gitea/workflows/`)
- All jobs run on a host runner with the label `host` on an arm64 machine with Docker. They build
  through `ci/build.sh`: the pin check on the host, then `make` and `make test` in a Debian
  Trixie container (`ci/Dockerfile`), the same compiler and glibc as on the pi-Stomp.
- Checkout uses `submodules: true`, never `recursive`, with the secret `CROSS_ORG_TOKEN`, which
  can read the copies of MoT and chowdsp_wdf.
- `build.yml`: every push; uploads `build/lv2/` (the bundle) as an artifact.
- `deploy.yml`: manual. Builds the chosen ref and installs it on the pi-Stomp like
  `make install` (copy next to the old bundle, then swap), restarts mod-host and mod-ui and
  waits for MOD-UI to list the plugin. Needs the variable `PISTOMP_HOST` as `user@host` (without
  the user, ssh connects as the runner's own user) and an SSH key of the
  runner's user on the pi-Stomp.
- `release.yml`: manual, for an existing release tag. Publishes it to the public GitHub repo
  through `ci/publish-github.sh`: one new commit per release with the tag's files (without
  `.gitea/` and `ci/publish-github.sh`), the tag's annotation as notes and the public author,
  plus a GitHub release with the zipped bundle. The private history is never pushed. It aborts
  if anything published matches the secret `PUBLISH_DENYLIST`, so **write tag annotations as
  public release notes**. Needs the variables `PUBLISH_GITHUB_REPO`, `PUBLIC_AUTHOR_NAME`,
  `PUBLIC_AUTHOR_EMAIL` and the secret `PUBLISH_GITHUB_TOKEN` (Gitea reserves names starting
  with `GITHUB_` or `GITEA_`).
- Release tags (`v1.0.0`, …) are the public version and don't match the LV2 version in
  `ttl/twin-crowns.ttl.in` (`minorVersion`.`microVersion`: 2.2 at `v1.0.0`, 2.4 at `v1.0.1`).
  LV2 has no major version (that's the URI), and MOD-UI treats minor 0 or any odd number as
  unstable, so the TTL can't say 1.0. Raise `microVersion` by 2 for every release, so hosts see
  it as newer.
- Every workflow mails its result to the variable `CI_NOTIFY_EMAIL` with `msmtp`.

## CPU load on the Pi 5
Total load from the MOD-UI bar, so it includes the rest of the pedalboard; above 75 % is too much.
- 2× 5 % idle, 7 % played; 4× 8 % idle, 10 % played (pedalboard with an amp sim, `ampx`)
- 1× 2.4 % idle, 3 % played; 8× 11 % idle, 12.5 % played (measured 2026-11-15)
- Every factor stays far below the limit, so the default 4× can stay, and 8× is affordable.

## Open points / assumptions
- Absolute input level of the pi-Stomp unknown
 
