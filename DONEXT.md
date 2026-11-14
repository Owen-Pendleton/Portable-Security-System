# DONEXT: where we stopped (2026-11-13, end of day)

Read CLAUDE.md, M4-Status.md and this file before starting. main was at
the commit that added this file; `git pull` first.

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

## NEXT: repeat bursts during a long ENTRY (plan only, NOT approved yet)
**Request (Owen and Bryan):** fire a burst at the ENTRY, then every
minute while the event stays open, and stop taking pictures 5 minutes
after the first burst. The cap exists so that long events (e.g. 20 min)
can't use up storage.

**Why this isn't just a timer:** a burst is written to
`clips/<id>.dir.tmp` and committed by renaming that folder to
`clips/<id>.dir` (`clip_commit`, `src/store/clip.c`). A second burst for
the same event would rename onto a folder that already exists and has
files, which fails. Its files would also clash by name, since every
burst writes `img0000.jpg` onward.

### Plan
1. **`src/store/clip.{c,h}` (Bryan's area):** new `clip_append()`.
   - If `clips/<id>.dir` doesn't exist yet, do what `clip_commit` does
     today.
   - Otherwise, fsync each new file, rename it into the existing folder
     with a burst prefix (`b2-img0000.jpg`, ...), fsync the folder, remove
     the empty `.tmp` folder and fsync `clips/`.
   - A crash midway gives at most a partial burst. Startup already deletes
     leftover `.tmp` folders. There are never torn files, and earlier
     bursts are untouched.
   - Tests in `tests/test_store.c`: append without name clashes and with
     the right total, and recovery from an interrupted append.
2. **`src/hub/hub.c` (shared):** add `first_burst_ns` and `bursts` to
   `struct event`.
   - In the tick, call `trigger_burst()` again when all of these hold:
     the event is open and is an ENTRY, no burst is in progress, at least
     60 s have passed since the last burst, less than 5 min has passed
     since the first burst, and the device is watching (ARMED or GRACE).
   - `on_cam_done` uses `clip_append` and keeps a running image total.
   - Set `PSS_EVF_NO_IMAGES` at close only if the total is 0. A failed
     later burst is logged and counted, but doesn't mark the event
     no-images.
   - Each burst still runs the quota check first.
   - New options: `--reburst-ms=60000` and `--reburst-cap-ms=300000`.
     A cap of 0 gives today's one-burst behaviour.
3. **`tests/test_e2e.c`:** fake camera with short times (200 ms apart,
   1 s cap). Check:
   - the expected burst count and summed image count
   - it stops at the cap
   - it stops after the owner disarms in GRACE
   - a short event still gets one burst
4. **Not changed:** camd, `src/common/ipc.h`, `pss events`, anything in
   `src/capture`.

**Storage:** DESIGN.md Section 5 estimates 6-9 MB per burst, so at most
about 45 MB per 5-burst event. The 2 GB clips quota already deletes the
oldest clips first, so the disk can't fill; at worst old pictures go
sooner. About 45 long events fit before anything is deleted.

### Decisions needed before coding
1. **After the owner disarms during GRACE:** stop the repeat bursts?
   (Proposed: yes; the first burst is kept.)
2. **Cap:** 5 bursts (at 0, 1, 2, 3, 4 min; proposed), or a 6th at 5:00?
3. **Who builds it:** most of this is Bryan's code (store) plus the
   shared hub. Either Bryan OKs Owen's session making the change, or
   Bryan's session builds it from this plan.

Then follow CLAUDE.md: plan approved first, done = make, make test,
make asan, make netcheck pass (shown), then verify on the Pi with a long
event (stay in view more than 5 min) and check
`ls --full-time /var/lib/pss/clips/<id>.dir`.

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
