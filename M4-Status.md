# M4 Status (in progress): Owen Pendleton, Bryan Rosas

## Hardware wired and checked on the Pi (2026-11-13)
Wired with the Pi powered off, using female-to-female jumpers (no breadboard).

| Part | Wire | Header pin | Signal |
|---|---|---|---|
| HC-SR501 PIR | brown, VCC | pin 2 | 5 V |
| | orange, GND | pin 6 | GND |
| | red, OUT | pin 11 | GPIO17 (pird's default `--line=17`) |
| Arm button module (`-`, middle, `S`) | yellow, S | pin 13 | GPIO27, Pi pull-up, **pressed = low** |
| | blue, - | pin 14 | GND |
| | middle pin | not connected | the Pi's internal pull-up replaces it |
| RTC cell | JST plug | J5 `BAT` | n/a |
| LED | not bought yet | pin 15 (GPIO22) kept free | n/a |

The PIR's knobs were left at factory settings (they're unlabelled on this
board). The RTC cell has no printed type, so trickle charging is **off**
(`rtc_bbat_vchg` not set). Enable it only after confirming an ML
(rechargeable) cell.

**Bring-up checks (stock tools, no project code):**
- PIR, `gpiomon GPIO17`: `rising` on a wave, `falling` a few seconds
  later, quiet for 20 s while still.
- Button, `gpiomon --bias=pull-up GPIO27`: `falling` on press, `rising`
  on release.
- RTC, `timedatectl`: RTC time 22:59 UTC matched local time (MST). With
  Wi-Fi and Ethernet disconnected, `sudo poweroff`, unplugged for 3 min,
  then booted: local time matched a phone, with `System clock
  synchronized: no`, so the time came from the RTC. (`hwclock` isn't
  installed on this image; use `timedatectl`.)

## PIR through the real pipeline (M3 code, before fusion)
`deploy/run-m3.sh` (PIR, camera; no mic), first real-PIR run of `pird`:
```
pird: start pid=1696 chip=/dev/gpiochip0 line=17 mode=irq level=0; warm-up, edges ignored until T+60s
16:18:44.719 pird: warm-up over, armed (line level=0)
16:19:14.931 pird: edge rise lat=22us
16:19:14.931 hub: event 4 opened (PIR rise)
16:19:14.934 camd: burst 4 started: 20 frames every 500 ms (rpicam-still pid 1704)
16:19:25.641 camd: burst 4 done: 18 frames
16:19:25.858 hub: event 4: committed 18 images to clips/4.dir
16:20:16.821 hub: event 4 closed: MOTION_ONLY dur=61.9s edges=9 images=18 audio-offline
```
- Edge-to-userspace latency (mechanism B, IRQ mode) was 17-33 us across
  both runs, with one 97 us edge during a camera burst.
- PIR pulses were ~0.45 s high, at least ~2.5 s apart while someone was
  in view. The event stayed open because the PIR kept re-firing, and it
  closed at Ctrl-C (`hub: exit 0`).
- `build/pss events` listed event 4.

## Fusion: camera only on a confirmed ENTRY (commit 44ff06a)
- **Rule** (`src/analysis/fusion.c`, called from the hub): a PIR rise
  and a sound onset within **T_co = 5 s** of each other, in either order,
  make an ENTRY. The ENTRY is made durable at once (EVENT_OPEN), and only
  then is the camera fired.
- **Motion alone or sound alone** is logged (MOTION_ONLY / SOUND_ONLY)
  with **no images**.
- T_co was changed from 3 s to 5 s at the team's request (DESIGN.md
  Section 4 updated); `--t-co-ms` overrides it.
- **Lab machine:** `make`, `make test`, `make asan`, `make memcheck` and
  `make netcheck` pass. The new `test_fusion` covers the window
  boundaries, both orders and out-of-order arrival. The new
  `test_entry_to_storage` covers sound->motion and motion->sound (ENTRY
  with images), too far apart (no images), and camera failure (ENTRY
  kept, flagged no-images).
- **On the Pi** (`deploy/run-m3.sh --mic --mic-device=plughw:CARD=Snowball`,
  PIR wired), results as observed by Bryan (no log captured):
  1. Motion only, silent: event closed MOTION_ONLY, images=0.
  2. Clap, sensor covered: no images. Labelled MOTION_ONLY because the
     PIR fired during the event, more than 5 s from the clap. An event's
     label is MOTION_ONLY whenever it saw any PIR edge and wasn't an
     ENTRY. The team decided that's acceptable, since only ENTRY drives
     the camera.
  3. Clap and motion together: ENTRY, images taken and saved.

## Open items
- Arm button code: nothing reads GPIO27 yet (pird, Owen's area). The
  button is wired and verified with `gpiomon`.
- Arm state machine (DISARMED/EXIT_DELAY/ARMED/GRACE) in the hub; LED
  hardware.
- Impulse and voice detectors (DESIGN.md Section 4) still to replace the
  M3 sound onset as fusion's audio input.
- Capture a full Pi log of the three fusion cases for the record.
- RTC cell type, to decide on charging.
- Known from M3: bursts give 18 of 20 frames; rpicam-still prints a
  screenful of libcamera INFO per frame (camd).
