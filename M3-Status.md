# M3 Status: Checkpoint Demo (Owen Pendleton, Bryan Rosas)

**Milestone requirement:** both sensors flowing through the real pipeline into
real storage; at least one menu mechanism working; transcripts copied out.

## Sensor substitution for this checkpoint
The PIR (HC-SR501) is **not wired** yet (no jumper wires), so the M3 demo
uses the **microphone (Blue Snowball) and the camera (Arducam OV5647)** as its
two sensors. The final design is still PIR + mic (DESIGN.md Section 4); this
swap is for the M3 demo only. The PIR path (`pird`) is still built and its
end-to-end test still runs.

**M3 sound trigger (in `hub`, stand-in for the M4 detectors).** `micd` sends
features every 20 ms. The hub keeps an EWMA background model (60 s time
constant) of spectral flux and log RMS, learned only while no event is open.
A frame is a sound onset when **both** its flux z-score is >= `--onset-z`
(default 6) **and** it is >= `--onset-db` (default 10 dB) louder than the
learned background, after `--warmup-frames` (default 250 = 5 s). Steady
noise (fans, AC) does not trigger: its flux stays near its own learned mean
and its level *is* the background; slow changes are absorbed by the EWMA.
An onset with no event open opens a `SOUND_ONLY` event and fires a camera
burst; later onsets extend it; it closes after `--quiet-ms` with no onsets.
**The thresholds are placeholders** until tuned on Pi recordings: the hub's
heartbeat line logs `mic_zmax` / `mic_dbmax` (largest scores per minute
outside events) for that. Fusion with the PIR is M4.

## What the demo shows
```
clap        -> micd (ALSA, our FFT features) -> hub background model -> onset -> SOUND_ONLY event opened
wave at PIR -> pird (GPIO edge interrupt, B) -> hub -> MOTION_ONLY event opened (when wired)
                                              -> camd -> rpicam-still burst -> clips/<id>.dir.tmp
            hub commits the burst atomically (fsync, rename, fsync dir; D) -> clips/<id>.dir
            hub writes a CRC-framed event record + fdatasync (D) -> events/seg-NNNNNN.pel
            (per-minute stats go to a separate log, stats/seg-NNNNNN.pel)
pss events  -> reads the record back from disk
```
- **Mechanism B (interrupt-driven input):** `pird` blocks until the kernel
  delivers a GPIO edge with a CLOCK_MONOTONIC timestamp, and logs the
  edge-to-userspace latency per edge. `--poll=<ms>` is the polling arm for
  the head-to-head comparison.
- **Mechanism D (storage layer):** separate append-only CRC32-framed event
  and stats logs with torn-tail recovery at startup; ENTRY records made
  durable when the entry opens, and any entry left open by a power cut
  closed as `interrupted (power lost)`; stats segments deleted whole after
  7 days; atomic clip commit; deletion of `*.tmp` left by interrupted
  writes; oldest-first clip quota with a free-space floor. Power-pull demo:
  pull the plug mid-burst, boot, and restart. The hub logs
  `store: recovered events seg=N valid=R truncated_bytes=B` and removes the
  interrupted clip. (ENTRY events themselves need fusion, M4; the store
  side is done and tested.)

## Running it on the Pi
```
make deploy PI=pi@<host>          # copy the tree and build natively (needs libgpiod >= 2, ALSA)
ssh pi@<host>
arecord -l                        # find the Snowball's card name
cd pss && deploy/run-m3.sh --mic --mic-device=plughw:CARD=<name> --no-pir   # M3: mic + camera
# (PIR wired: deploy/run-m3.sh, add --line=N if the PIR is not on GPIO17)
build/pss events                  # in another terminal: what has been stored
```
Check the GPIO chip and line first with `gpioinfo`. Depending on the kernel,
the Pi 5 header is `gpiochip0` or `gpiochip4`; pass `--chip=` to override.
The PIR ignores edges for its first 60 s (sensor warm-up); `--warmup=0` skips
this for a quick check.

## Not in M3 (next milestones)
- Fusion FSM and the DESIGN.md Section 4 detectors (impulse, voice) in
  `src/analysis` (M4); the M3 sound trigger in `hub` is replaced then.
- Supervisor (mechanism E) and `make soakcheck`.
- Hub query socket; `pss timeline / status / health / expect / settime`.

## Verification on the lab machine
`make`, `make test`, `make asan` (clang ASan+UBSan), `make memcheck`
(valgrind) and `make netcheck` all pass. `pird` cannot link on the lab
machines (no libgpiod), so it builds on the Pi. It was compile-checked
against the libgpiod v2.1 header. `tests/test_e2e` runs the real hub and camd
with a stand-in camera script, with the test itself sending PIR edges in
place of `pird`, and (`test_sound_to_storage`) synthetic mic frames in place
of `micd`: 10 s of steady fan noise plus a slow 9.5 dB fan ramp store no
event, then one clap frame stores exactly one `SOUND_ONLY` event with its
images. **Not yet verified on the Pi:** `pird` with the real PIR,
`rpicam-still` burst arguments, `micd` on the Blue Snowball, and the live
clap -> `pss events` demo. (Pending; this section is updated with the real
Pi output once run.)
