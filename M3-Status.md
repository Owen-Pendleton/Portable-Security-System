# M3 Status: Checkpoint Demo (Owen Pendleton, Bryan Rosas)

**Milestone requirement:** both sensors flowing through the real pipeline into
real storage; at least one menu mechanism working; transcripts copied out.

## Sensor substitution for this checkpoint
The Blue Snowball microphone has **not arrived yet**. The M3 demo therefore
uses the **PIR (HC-SR501) and the camera (Arducam OV5647)** as its two
sensors. `micd` is written and its feature extraction is unit-tested on
synthetic signals, but it has **not been run on real microphone hardware**.
Fusion of PIR and audio (Design.md §4) needs the mic and data we collect, so
it is planned for M4. Until then, every PIR-triggered event is recorded as
`MOTION_ONLY` and tagged `audio-offline`, and every successful camera burst is
kept. (Once fusion exists, Design.md §5 deletes unconfirmed bursts.)

## What the demo shows
```
wave at PIR -> pird (GPIO edge interrupt, B) -> hub -> event opened
                                              -> camd -> rpicam-still burst -> clips/<id>.dir.tmp
            hub commits the burst atomically (fsync, rename, fsync dir; D) -> clips/<id>.dir
            hub writes a CRC-framed event record + fdatasync (D) -> log/seg-000001.pel
pss events  -> reads the record back from disk
```
- **Mechanism B (interrupt-driven input):** `pird` blocks until the kernel
  delivers a GPIO edge with a CLOCK_MONOTONIC timestamp, and logs the
  edge-to-userspace latency per edge. `--poll=<ms>` is the polling arm for
  the head-to-head comparison.
- **Mechanism D (storage layer):** append-only CRC32-framed log with
  torn-tail recovery at startup; atomic clip commit; deletion of `*.tmp`
  left by interrupted writes; oldest-first quota with a free-space floor.
  Power-pull demo: pull the plug mid-burst, boot, and restart. The hub logs
  `store: recovered seg=N valid=R truncated_bytes=B` and removes the
  interrupted clip.

## Running it on the Pi
```
make deploy PI=pi@<host>          # copy the tree and build natively (needs libgpiod >= 2, ALSA)
ssh pi@<host>
cd pss && deploy/run-m3.sh        # hub + camd + pird; add --line=N if the PIR is not on GPIO17
build/pss events                  # in another terminal: what has been stored
```
Check the GPIO chip and line first with `gpioinfo`. Depending on the kernel,
the Pi 5 header is `gpiochip0` or `gpiochip4`; pass `--chip=` to override.
The PIR ignores edges for its first 60 s (sensor warm-up); `--warmup=0` skips
this for a quick check.

## Not in M3 (next milestones)
- Fusion FSM, detectors, background model (M4, needs the mic).
- Supervisor (mechanism E) and `make soakcheck`.
- Hub query socket; `pss timeline / status / health / expect / settime`.

## Verification on the lab machine
`make`, `make test`, `make asan` (clang ASan+UBSan), `make memcheck`
(valgrind) and `make netcheck` all pass. `pird` cannot link on the lab
machines (no libgpiod), so it builds on the Pi. It was compile-checked
against the libgpiod v2.1 header. `tests/test_e2e` runs the real hub and camd
with a stand-in camera script, with the test itself sending PIR edges in
place of `pird`. **Not yet verified on the Pi:** `pird` with the real PIR,
`rpicam-still` burst arguments, and `micd` (no microphone yet).
