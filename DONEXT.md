# DONEXT: where we stopped (2026-11-13, end of day)

Read CLAUDE.md, M4-Status.md and this file before starting. main was at
the commit that added this file; `git pull` first.

## PLAN TO THE END (agreed 2026-11-18; M5 due Wed Dec 9, demo day after)
We resume **Tue Dec 1** and work each day as much as we can until done.
Work in this order; each step needs the ones above it.

**Before code freeze (Pi + code):**
1. Tuning: Owen runs the tuning session below, Bryan sets `--k-imp` /
   `--k-voice` from `tune.txt`.
2. `pss status` (honest sensor states) + `pss timeline`. Agree the hub
   query message in `ipc.h` first (both of us). Owen does Pi steps 1, 6,
   7, 8 meanwhile.
3. Accuracy trials together (~30 x 5 cases): one acts, one logs on paper.
4. Sensor health (PIR SUSPECT, audio DEGRADED/RECOVERED) + OFFLINE spans;
   `make memcheck`.
5. Pi measurements: IRQ vs poll (idle + loaded), power pulls, kill -9,
   unplugs.
6. README.md: a clean Pi to a running system, no help needed.
7. **Code freeze and start the 48 h soak: Wed Dec 2 preferred, Thu Dec 3
   evening at the latest** (so a failed run can be redone).

**After freeze (no code changes):**
8. Unplug the mic for 10 min during the soak (the required injected fault).
9. EVALUATION.md; DESIGN.md changelog (including the cuts below).
10. PROMPTLOG.md and REFLECTION.md, each of us our own.
11. Copy the raw soak logs and transcripts into the repo.
12. Rehearse the demo: someone unplugs a sensor, `pss status` shows it;
    each of us practises a small live change in our own code.

If behind on Wed evening: start the soak anyway and record the rest as cut.

**Cut, per the course spec** (record each in the DESIGN.md changelog):
phone page and hotspot (spec: a CLI is a valid interface), audio WAV
clips (not in the spec; photos cover mechanism D), routine/UNEXPECTED
times (`pss expect`), probably the LED.

**Owen, before the break (5 min, optional):** `git pull && make`, then
`sudo systemctl restart pss`, and leave the Pi plugged in. It soaks the
current code until Dec 1; check `journalctl -u pss` then for leaks or
crashes.

## Done today
- **Arm button in pird** (Owen): GPIO27 debounced at 20 ms (kernel +
  software), one `PSS_MSG_BUTTON` per press. Verified on the Pi: 20/20
  presses in IRQ and `--poll=10` modes. Details in M4-Status.md.
- **Arm state machine** (Bryan, `src/analysis/arm.c`) and **supervisor +
  systemd service** (Bryan, `src/supervisor`, `deploy/`).
- **camd quiet output** (Owen): `--verbose=0` and
  `LIBCAMERA_LOG_LEVELS=*:WARN`. Verified on the Pi: an armed ENTRY gave
  no INFO text, and its images were saved.
- **Service installed on the Pi** with `deploy/install-service.sh`. It
  starts at boot. Logs: `journalctl -u pss -f`. Stop it with
  `sudo systemctl stop pss` before using `deploy/run-m3.sh`.

## Pi test (Bryan's 10 steps): what's confirmed, what's left
Confirmed from the `build/pss events` output on the Pi:

| Step | Result |
|---|---|
| 2: press, 60 s exit delay | EXIT_DELAY -> ARMED after 60 s |
| 3: ENTRY fires camera | ENTRY #8, #9, #10 got 18-19 images |
| 4: press in GRACE | `DISARMED #9 entry owner` |
| 5: no press | `ARMED #8 entry reported` after 30 s |
| 9: power cut | Cut during GRACE; came back ARMED with `#10 entry reported`; #10 `interrupted (power lost)` but kept 18 images |
| 10: `pss events` | Works (run it from `~/pss`) |

**Still to do:**
- **Steps 1, 6, 7, 8** (boot start, mic unplug and replug, `kill -9 camd`)
  are not confirmed. These are the mechanism E evidence. Re-run them with
  `journalctl -u pss -f 2>&1 | tee ~/pi-test.txt` and look for:
  - supervisor starting all four daemons
  - `micd exited 3 (device gone), retry in 30000ms`
  - micd coming back after the replug
  - `camd killed by signal 9, restart #1 in 1000ms`
- **Optional:** a power cut while plainly ARMED (no entry). Expect it to
  come back ARMED with no press.
- **Send the log text files to Bryan, not photos.** His session records
  the results in M4-Status.md. Copy them off the Pi with `scp`:
  - `~/pi-test.txt`
  - the previous boot's log: `journalctl -u pss -b -1 --no-pager > ~/pi-test-before-cut.txt`
- **Things to remember on the Pi:**
  - After every boot the PIR ignores motion for 60 s.
  - Presses while the hub is down do nothing, and there is no LED yet.
  - An event stays open (one burst) until the room has been quiet for
    30 s.

## Observation (Owen's area, no action yet)
At 19:32:09-12 pird logged six PIR falling edges in a row with no rise
between them. The filter dropped them as duplicates, so there was no
false event. They are probably glitches from noise on the loose jumper
wires. If it keeps happening, capture `gpiomon GPIO17` with the service
stopped before proposing any fix.

## DONE: repeat bursts during a long ENTRY (2026-11-18, commit c567880)
Built from the plan that was here, with the three decisions settled:
stop after a disarm in GRACE (first burst kept); cap at 5 bursts (0-4
min); Bryan OK'd Owen's session doing it.

- **Store** (`clip_append()`, `src/store/clip.c`): later bursts go into
  the existing `clips/<id>.dir` as `b<n>-img*.jpg`; the first keeps its
  plain names. A crash midway leaves a partial burst; earlier bursts are
  untouched.
- **Hub:** repeat burst while the ENTRY is open and the device is ARMED or
  GRACE. Options `--reburst-ms=60000`, `--reburst-cap-ms=300000` (0 = one
  burst). `no-images` is set at close only if the event's total is 0.
- **Changed from the plan:** each burst is due at a fixed offset from the
  first (first + n x 60 s), not "60 s after the last one", so the count
  is always 5 whatever the 1 s tick phase. On the Pi a burst may be up to
  1 s late.
- **Changed from the plan:** the e2e test uses 1 s bursts and a 3.5 s cap
  (4 bursts), not 200 ms / 1 s, because the hub ticks once a second.
- **Tests:** `test_store.c` (append, interrupted append), `test_e2e.c`
  `test_repeat_bursts` (cap, GRACE disarm, short event). make, make test,
  make asan, make netcheck all pass.
- **Known edge case (not handled):** if camd hangs past the burst timeout
  and reports late while the next burst is running, that report is taken
  as the new burst's result. Only possible if camd stalls for over ~30 s.

**Still to do: verify on the Pi.** Pull, rebuild, stay in view for more
than 5 min, then check `ls --full-time /var/lib/pss/clips/<id>.dir`.
Expect 5 bursts about a minute apart (`img*`, `b2-` to `b5-`) and
`pss events` showing the summed image count.

## NEXT: tune the sound detectors on the Pi (Owen, then Bryan)
The impulse (door) and voice detectors are in (`src/analysis/audio.c`,
DESIGN.md Section 4). Their thresholds `--k-imp=6` and `--k-voice=3` are
guesses. `hub --tune` logs a line for every sound that comes close, with
its scores and why it did or didn't count, for example:
```
tune: impulse? flux z=9.1 (k_imp 6), 6/8 bands, fell 14 dB in 60 ms -> YES
tune: impulse? flux z=7.4 (k_imp 6), 3/8 bands -> no (not broadband, needs 5)
tune: voice? last 1 s: speech z max=4.2 (k_voice 3), low band z=1.0, 31 frames counted, 31/100 in 2 s (needs 20)
```
Steps:
1. `git pull && make`. In `deploy/pss.conf` change the hub line to
   `hub    hub --tune`, then `sudo systemctl restart pss`.
2. Leave it **disarmed** (don't press the button): every sound is still
   scored and logged, but nothing is recorded and the camera never fires.
3. Wait 2 minutes in a quiet room so it learns the room.
4. `journalctl -u pss -f 2>&1 | tee ~/tune.txt`
5. Make each sound about 5 times, a few seconds apart, and write down the
   clock time you start each kind:
   - should count: open and close the room door; normal talking ~10 s,
     near and across the room; quiet talking
   - should not: heating/AC switching on (most important: it can also
     fool the PIR); a fan; footsteps; a phone beep; music; a cough
6. Send Bryan `~/tune.txt` and the list of times, as text files.
7. Remove `--tune` from `pss.conf` and restart.

Bryan's session then picks the thresholds from the scores, sets them in
`pss.conf`, and asks for a short armed check (walk in through the door:
ENTRY with photos; AC switching on: nothing).

## Other open items (from M4-Status.md)
- **LED** (GPIO22, pin 15, not bought). Proposed: the hub sends an LED
  message (off / on / blink) and pird does the blinking. Pattern
  suggestion: steady = ready, blink = exit delay, off = armed. Needs a
  new message in `ipc.h`; agree it with Bryan first.
- **`pss arm` / `pss disarm`:** needs a new message or role in `ipc.h`;
  agree it with Bryan first.
- **18 of 20 frames per burst:** deferred (about 9 s of pictures per
  burst).
- Impulse and voice detectors, the RTC cell type, and the full Pi log of
  the fusion cases (see M4-Status.md).

## Housekeeping
- Photos `IMG_4171.JPG` and `IMG_4172.JPG` are untracked in the repo
  folder. Delete them; don't commit them.
- Copy session transcripts to `CLAUDE-TRANSCRIPTS/Owen-Pendleton/` and
  `CLAUDE-TRANSCRIPTS/bryan-rosas/`.
