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

## Arm button in pird (Owen, branch `m4-arm-button`)
- `pird` requests GPIO27 together with the PIR line (pull-up, both
  edges, kernel timestamps). A fall counts as a press only if the line
  was high for at least the debounce period before it
  (`src/capture/pir/button_logic.c`, tested by `tests/test_button_logic.c`).
  It is debounced by the kernel and in software in IRQ mode, and in
  software only with `--poll`. The PIR warm-up does not apply to it. Each
  press goes to the hub as `PSS_MSG_BUTTON`. The hub only logs it
  (`arm button press #N`, approved by Bryan); the arm state machine is
  still to come.
- **Lab machine:** `make`, `make test`, `make asan`, `make memcheck`,
  `make netcheck` pass. pird doesn't build there (no libgpiod v2).
- **Raw bounce on the Pi** (debounce off, `--button-debounce-ms=0`):
  - 10 presses gave 12 counted. The 2 extras were release bounces,
    12 µs and 38 µs after a release. Each bouncing release settled
    within 0.64 ms.
  - Presses never bounced.
  - `gpiomon`, 11 presses: one release bounced, for about 0.23 ms.
  - With debounce off the kernel once reported two falls in a row
    10 µs apart: edge direction is unreliable during a fast bounce. So
    the kernel debounce is kept on in IRQ mode, not only the software
    filter.
- **Chosen debounce: 20 ms** (the default). That is about 30x the
  longest bounce, and far below the gap between real presses (at least
  1 s here).
- **20-press checks** (`deploy/run-m3.sh`, PIR wired, no mic):

  | Mode | pird `press #` | hub `arm button press` | bounces | dropped |
  |---|---|---|---|---|
  | IRQ, kernel+software 20 ms | 20 | 20 | 0 | 0 |
  | `--poll=10`, software 20 ms | 20 | 20 | 0 | 0 |

  - Presses #1-13 of the IRQ run came during the PIR's 60 s warm-up and
    were all forwarded: no warm-up on the button.
  - IRQ latency was 7-33 µs.
  - With kernel debounce on, button timestamps fall on about a 4 ms grid.
    The kernel stamps the event when its debounce timer fires, so for
    the button `lat=` is measured from the end of the debounce, not from
    the contact. Press-to-hub delay is about 20-24 ms. Mechanism B
    latency numbers should therefore come from the PIR line, or from
    the button with debounce off.

## Arm state machine in the hub (Bryan, commit 32478e1)
- **Rule** (`src/analysis/arm.c`, called from the hub; DESIGN.md
  Section 4): DISARMED --button--> EXIT_DELAY (60 s) --> ARMED
  --ENTRY--> GRACE (30 s). A press in GRACE disarms and labels the entry
  `owner`. If GRACE runs out, the entry is labelled `reported`.
  `--exit-delay-ms` and `--grace-ms` override the times.
- **Decisions DESIGN.md left open** (Bryan, now in DESIGN.md Section 4):
  1. A press while ARMED (no entry yet, e.g. a quiet entry with no
     sound) disarms.
  2. A press during EXIT_DELAY cancels back to DISARMED, so a double
     press cancels arming.
  3. The state survives a restart or power cut. A restart during
     EXIT_DELAY starts the full delay over. A restart during GRACE comes
     back ARMED with the entry `reported`, so pulling the plug can't hide
     an entry.
  4. While DISARMED or in EXIT_DELAY, no new events are recorded and the
     camera never fires. An event already open when the device is
     disarmed finishes normally, so an `owner` entry keeps its images.
- **Storage:** every arm change is a new `PSS_REC_ARM` record in the
  events log, fsynced at once. The hub restores the last one at start.
  `pss events` prints these records, with the owner/reported label.
- **Hub:** button presses are accepted from pird only. The LED state is
  logged (`arm: LED blink/off (not wired yet)`) until it is wired.
- **Lab machine:** `make`, `make test`, `make asan`, `make memcheck` and
  `make netcheck` pass.
  - The new `test_arm` covers every transition and both timeouts at
    their exact boundaries.
  - The existing e2e tests now arm the hub first, since a fresh hub
    records nothing. Their checks are unchanged.
  - The new `test_arm_decisions` runs a real hub. It covers each decision
    above, including SIGKILL during GRACE and during EXIT_DELAY. Hub log
    from that run:
    ```
    hub: arm: EXIT_DELAY -> DISARMED (button)
    hub: arm: ARMED -> DISARMED (button)
    hub: arm: GRACE -> DISARMED (button), event 1 labelled owner
    hub: arm: GRACE -> ARMED (grace over), event 2 labelled reported
    hub: store: event 3 was open when power was lost; closed as interrupted (0 images kept)
    hub: arm: GRACE -> ARMED (restart), event 3 labelled reported
    hub: arm: EXIT_DELAY -> EXIT_DELAY (restart)
    ```
- **Not yet checked on the Pi** with the real button.
- **Side effect of decision 4:** while disarmed no event is open, so the
  mic background model keeps learning, including people talking. It
  adjusts back within about a minute (its time constant), and the 60 s
  exit delay mostly covers that.

## Open items
- Arm state machine on the Pi: press, wait out the 60 s, walk in, press
  within 30 s; check the `arm:` lines in the hub log and in `pss events`.
- LED hardware, plus a hub -> pird message to drive it (Owen to decide
  who times the blink).
- `pss arm` / `pss disarm`: needs a new message or role in the shared
  `src/common/ipc.h` (agree with Owen first).
- Done (Bryan, 2026-11-18): impulse and voice detectors
  (`src/analysis/audio.c`, DESIGN.md Section 4) replace the M3 sound onset
  as fusion's audio input. Hub flags `--onset-z`/`--onset-db` became
  `--k-imp=6`/`--k-voice=3`. Unit tests in `tests/test_audio.c` (door
  click, beep, slow thump, swell, speech, cough, HVAC rumble, learning);
  each rule was broken on purpose to confirm a test catches it. The e2e
  helpers now send 200 ms of room sound after each clap, as real micd
  does, so the impulse can see its decay. Still to do: tune `K_imp` and
  `K_voice` on the Pi (door, talking, TV, HVAC) from the heartbeat's
  `imp_zmax` / `voice_zmax`.
- Background keeps learning during SOUND_ONLY events, freezing only once
  there is motion (Bryan, 2026-11-18; DESIGN.md Section 4). e2e
  `test_tv_fades_into_background` fails without it (the event never
  closes). A steady TV fades in about 7.5 s; so does continuous talking
  with nobody in view.
- Repeat bursts during a long ENTRY (Owen, 2026-11-18, commit c567880):
  see DONEXT.md; not yet checked on the Pi.
- Capture a full Pi log of the three fusion cases for the record.
- RTC cell type, to decide on charging.
- Known from M3: bursts give 18 of 20 frames (camd). Deferred: the
  pictures still cover about 9 s of an entry.
- Done (Owen, 2026-11-13): camd no longer prints a screenful of
  libcamera INFO per frame. rpicam-still gets `--verbose=0`, and camd
  sets `LIBCAMERA_LOG_LEVELS=*:WARN` unless it is already set
  (`LIBCAMERA_LOG_LEVELS=*:INFO` brings the detail back). Checked on the
  Pi: after arming, a triggered ENTRY burst printed no INFO text, and its
  images were taken and committed.
