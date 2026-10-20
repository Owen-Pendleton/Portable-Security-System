# Design Document: Portable Security (Owen Pendleton, Bryan Rosas)

**Milestone:** M2. **Status:** draft for review. M5's version will carry a
changelog of what this one got wrong.

## 1. Summary

Macy travels about three nights a month and leaves a laptop, cash, and bags in
a hotel room all day. Our device sits in the room while she is out and, when
she returns, answers one question with a short timeline: *did anyone come in,
when, and for how long?* It has no internet connection, no cloud, and no
account; every byte stays on its SD card. Macy reads the timeline from her
phone over the device's own Wi-Fi hotspot.

**The narrowed claim:** in a test room, the device separates *door-entry
events* (motion plus a door/voice sound signature) from *motion-only* and
*sound-only* events with a measured confusion matrix, and keeps a
crash-consistent record of them through power cuts and sensor faults.

**Mechanisms:** **B** interrupt-driven GPIO input (PIR and arm button) vs.
polling; **D** a crash-consistent storage layer; **E** multi-process
supervision.

**Hardware:** Raspberry Pi 5 with RTC backup battery · HC-SR501 PIR (GPIO,
keyed connector) · Blue Snowball USB microphone (ALSA) · Arducam OV5647 (CSI,
Pi 5 22-pin cable) · arm button + LED (GPIO). PIR and microphone are the two
*cooperating* sensors; the camera only collects evidence for confirmed entries.

## 2. Architecture

```
 PIR + button (GPIO)    Blue Snowball (USB)        OV5647 (CSI)
     | GPIO edges           | USB isoc audio           | CSI-2
=====|======================|==========================|===================== kernel
 RP1 gpiochip            snd-usb-audio             libcamera
 (kernel timestamps)     (44.1 kHz s16 = 88.2 KB/s)
=====|======================|==========================|===================== user
     v                      v                          v
 +--------+           +-----------+             +-----------+
 |  pird  |           |   micd    |             |   camd    |
 | PIR +  |           | 20 ms FFT |             | burst on  |
 | button |           | features; |             | trigger,  |
 | + LED  |           | 15 s PCM  |             | ~2 fps    |
 +--------+           | ring      |             +-----------+
     | edge/button    +-----------+   trigger (<1/min) ^  | frame-ready
     | msgs <1 Hz,          | features 50 Hz x 64 B    |  | 2 Hz, path only
     | ~40 B                | = 3.2 KB/s               |  v
 +--------------------------------------------------------------+
 |                     hub (single writer)                      |
 |  analysis/: background model -> detectors -> fusion FSM      |
 |  store/: event log + stats log (append, CRC) + clip store    |
 |  arm state machine; query socket                             |
 +--------------------------------------------------------------+
     | events ~0.1-20/hr (fsync on open and close); clips ~6-9 MB
     | per confirmed entry; stats 1/min batched
     v
 SD card: /var/lib/pss/{events/, stats/, clips/}

 supervisor (E): fork/execs pird, micd, camd, hub, ui; 5 s heartbeats from
 each over AF_UNIX; hourly soak heartbeat (liveness, RSS, fds, counts).

 Macy's phone --Wi-Fi (hotspot, no uplink)--> ui (read-only page) --AF_UNIX--> hub
 pss (CLI, over SSH on the hotspot) --AF_UNIX query socket (~1/min)--> hub
```

All internal IPC is AF_UNIX `SOCK_SEQPACKET`: message boundaries are kept, and
a dead peer shows up as EOF/EPIPE, which is the fault signal E needs.

| Process | Threads | Inputs → outputs |
|---|---|---|
| `supervisor` | 1 (epoll: signalfd, child sockets, timerfd) | heartbeats → restarts, soak log |
| `pird` | 1 (blocks on GPIO edge events, or `timerfd` in poll mode) | PIR/button edges → msgs; LED |
| `micd` | 2: ALSA capture + feature thread, joined by an SPSC ring | PCM → features; audio clip on request |
| `camd` | 1 + `rpicam-still` child per burst | trigger → JPEGs in `clips/*.tmp` |
| `hub` | 1 (epoll over child + query sockets, timerfd) | features + edges → events, storage |
| `ui` | 1 (Python, `ui/`, display only) | HTTP on hotspot address → hub queries |

All run at default priority; mechanism C is not claimed.

## 3. Mechanism mapping

### B: Interrupt-driven GPIO input (`pird`, `src/capture/pir`)
**Implementation.** `pird` requests the PIR and button lines through libgpiod
v2 with both edges and kernel timestamps (`CLOCK_MONOTONIC`), and blocks until
the RP1 GPIO interrupt delivers an edge. A `--poll=<ms>` mode reads line values
on a `timerfd` instead; that is the comparison arm.
**Why Macy needs it.** The device sits idle for hours and must catch the few
edges that matter. An interrupt reports every edge with the kernel's
timestamp; a poll only knows a change happened somewhere in the last interval,
and can miss a button press shorter than that interval entirely. A 10 ms poll
also wakes the CPU 100 times a second for a sensor that changes a few times a
day. We are honest about scale: the PIR itself takes hundreds of milliseconds
to react to motion, and on a Pi 5 the CPU difference may be small. We will
report whatever we measure.
**Measured.** Edge-to-userspace latency distribution, missed short pulses, and
`pird` CPU, IRQ vs. poll at 1, 10, 100 ms, idle and under load (Section 9).

### D: Crash-consistent storage (`src/store`, linked into `hub`)
**Implementation.** Append-only CRC32-framed logs in segment files (events and
stats kept separately); atomic clip commits (tmp → fsync → rename → fsync dir);
recovery at startup; byte quota with oldest-first clip eviction (Section 5).
**Why Macy needs it.** The device runs from a hotel wall outlet, and outlets
get used: housekeeping unplugs things to vacuum, Macy unplugs it to pack, and
an intruder may pull the plug on purpose. The record exists to answer "did
someone come in?" after the fact, so it must survive exactly that.
**Measured.** Power-pull recovery (records lost, corrupt records accepted,
dangling clip references), bytes written per day, fsync count and latency.

### E: Multi-process supervision (`src/supervisor`)
**Implementation.** One process per sensor, plus hub, ui, and supervisor.
Children heartbeat every 5 s. The supervisor restarts any child that exits or
misses heartbeats for 15 s (SIGKILL first), with backoff 1 s → 60 s that resets
after 10 min healthy. Exit code `3` means "device gone": the supervisor retries
every 30 s and the hub marks that sensor DEGRADED instead of storming restarts.
**Why Macy needs it.** A security monitor that silently dies is worse than
none, because Macy believes it is on duty. If the camera hangs or the mic drops
off USB, the PIR record must keep flowing and the timeline must say which
sensors were offline and when.
**Measured.** `kill -9` each child and unplug the mic: detection time, recovery
time, events lost during the gap, log excerpts.

## 4. Analysis engine (`src/analysis`, all ours)

**Audio features (in `micd`).** 20 ms hops (882 samples) over a 1024-point Hann
window. Per frame: RMS, energy in 8 log-spaced bands (100 Hz – 8 kHz), and
spectral flux. FFT is a primitive (small library or our own radix-2).

**Background model.** Per band, an exponentially weighted mean and variance
(time constant ~60 s), updated only outside active events so a conversation
doesn't become "background". Frames are scored as per-band z-scores, so the
same device works next to a loud HVAC unit and in a silent room.

**Detectors.**
- *Impulse (latch/door):* spectral-flux z-score above `K_imp`, broadband, decay
  under 150 ms.
- *Voice:* z-score in the 300–3400 Hz bands above `K_voice` for ≥ 400 ms within
  2 s, with sub-200 Hz energy not dominant (rules out HVAC).
- *PIR:* HC-SR501 in **single-trigger mode** with the delay pot at minimum, so
  each detection is a pulse of a few seconds followed by a ~2.5 s low block
  time, even under continuous motion. A person moving around produces a train
  of pulses.

**Fusion state machine (in `hub`).**
```
IDLE --PIR rise--> MOTION_WINDOW (±T_co = 3 s around the edge; audio history checked)
MOTION_WINDOW --impulse or voice in window--> ENTRY (confirmed; durable at once)
MOTION_WINDOW --window expires, no audio--> MOTION_ONLY
IDLE --impulse/voice, no PIR within ±T_co--> SOUND_ONLY (hallway)
ENTRY/MOTION_ONLY --no PIR pulse and no voice for T_quiet = 30 s--> close event
```
Opening and closing use different conditions, so ten minutes of someone moving
around is one event, not forty.

**Arm state machine (in `hub`, button handled by `pird`).**
```
DISARMED --button--> EXIT_DELAY (60 s, LED blinks) --> ARMED (LED off)
ARMED --ENTRY confirmed--> GRACE (30 s) --button--> DISARMED, entry labelled `owner`
GRACE --30 s, no button--> ARMED, entry reported (routine / UNEXPECTED)
```
Events are only reported while ARMED. Macy can also use `pss arm` /
`pss disarm`. The LED stays dark while armed so the device doesn't draw
attention.

**Routine vs. unexpected (optional).** Housekeeping times are often unknown,
so the expected window is optional (`pss expect 10:00-14:00`). With no window,
every confirmed entry while armed is reported plainly; with one, entries
inside it are labelled `routine` and others `UNEXPECTED`. The device never
tries to identify who entered.

**Why a threshold cannot do this.** A loudness threshold fires on every
hallway door slam and misses a quiet entry in a noisy room; a PIR alone fires
on curtains moved by HVAC. The decision rests on the *timing* of two different
physical signals, scored against a background learned in *this* room, with
hysteresis on event boundaries. No single `if` reproduces that.

## 5. Storage and data

| Data | Rate | Format | Retention |
|---|---|---|---|
| Event records | ~0.1–20/hr | `[len u32][type u16][ver u16][payload][crc32]` | kept (tiny); never evicted |
| Stats / heartbeat records | 1/min, batched | same framing, separate segments | whole segments deleted after 7 days |
| Audio clips (confirmed entries) | 10 s pre-roll + ≤ 20 s post ≈ 2.6 MB | WAV | clip quota, oldest first |
| Image bursts (confirmed entries) | ~2 fps × 10 s ≈ 3–6 MB | JPEG | clip quota, oldest first |
| Daemon text logs | low | timestamped lines | logrotate, 4 × 5 MB |

**Event log.** Segments `events/seg-NNNNNN.pel`, capped at 4 MB. When an ENTRY
is confirmed, an `event_open` record is appended and `fdatasync`ed
immediately; `event_close` is synced when the event ends. So if someone pulls
the plug mid-entry, the entry is already on disk. Recovery turns an open
without a close into an event marked `interrupted (power lost)`.
MOTION_ONLY and SOUND_ONLY events are written once, at close.

**Stats log.** Separate segments in `stats/`, buffered in memory and flushed
once a minute. A power cut loses at most a minute of stats, never an event.
Because stats live apart from events, old stats are dropped by deleting whole
segments, with no compaction.

**Clips.** Written as `clips/<event-id>.{wav,dir}.tmp`; on completion: fsync
file → rename → fsync directory. The record referencing a clip is written only
after the rename is durable, so the log never points at a missing clip. Bursts
for unconfirmed events are deleted. The `micd` PCM ring holds 15 s, so a full
10 s pre-roll still exists after the ~3 s confirmation delay.

**Recovery (hub start).** For the newest event and stats segments: stop at the
first bad length or CRC, `ftruncate` there, and log `store: recovered seg=N
valid=R truncated_bytes=B`. Delete every `clips/*.tmp` and log each. Close any
open entry as `interrupted`. Rebuild the in-memory index.

**Power-loss gaps.** The hub writes a `boot` record at every start. The gap
between the last durable stats record and `boot` is when the device was off,
accurate to about a minute; the timeline shows it as `OFFLINE` so Macy doesn't
read silence as "nobody came in".

**Quota.** Default 2 GiB for `clips/` (~250 entries). Before each clip, evict
the oldest clips until there is room. Below a 5 % free-space floor, clips stop
and event records continue.

**Time.** There is no NTP offline. Wall time comes from the Pi 5's RTC, and the
RTC backup battery is a **requirement**: `CLOCK_MONOTONIC` restarts at every
boot, so after a power cut only the RTC knows how long the device was off.
Records carry both clocks; intervals and ordering use monotonic time within a
boot. If the RTC reports an unset clock at boot, the hub logs it and the
timeline shows the gap as `OFFLINE (duration unknown)` until the clock is set
(`pss settime`, or the "set clock from this phone" button on the UI page).

## 6. Failure-mode table

| Component | Failure | Detected by | System response | Log shows |
|---|---|---|---|---|
| `micd` | mic unplugged (**our offered demo fault**) | ALSA read `-ENODEV`/`-EIO` | exit 3; retry every 30 s; audio DEGRADED; PIR-only events tagged `unconfirmed (audio offline)` | `micd: snd_pcm_readi: No such device; exit 3` … `hub: RECOVERED audio after 612s` |
| `micd` | crash / `kill -9` | `SIGCHLD` + `waitpid` | restart with backoff; gap events tagged DEGRADED | `sup: micd killed by signal 9, restart #3 in 1s` |
| `micd` | ALSA overrun | `-EPIPE` | `snd_pcm_prepare`, count, continue | `micd: xrun #N recovered` |
| `pird` | crash / hang | `SIGCHLD` / 15 s missed heartbeat | SIGKILL + restart; PIR DEGRADED | `sup: pird heartbeat missed 15s, SIGKILL` |
| PIR | unplugged at its connector | pull-up holds line high; in single-trigger mode a real pulse never lasts past `T_pulse + 5 s` (to verify on bench) | PIR SUSPECT within ~10 s; the one false edge becomes MOTION_ONLY (no sound) and is tagged `possible-unplug` | `pird: line high 10s > max pulse, SUSPECT` |
| PIR | warm-up false triggers | uptime < 60 s | ignore edges | `pird: warm-up, edges ignored until T+60s` |
| `camd` | `rpicam-still` fails | non-zero exit, missing files | event kept, tagged `no-images` | `camd: rpicam-still exit 255; burst skipped` |
| `camd` | crash / `kill -9` | `SIGCHLD` | restart; stale `.tmp` cleaned | `sup: camd killed by signal 9` |
| camera | ribbon unplugged | n/a | **not a supported fault**: CSI must never be hot-unplugged | n/a |
| `hub` | crash / `kill -9` | `SIGCHLD` | restart; recovery runs; daemons reconnect with backoff, counting dropped frames | `store: recovered seg=12 valid=341 truncated_bytes=58` |
| `ui` | crash | `SIGCHLD` | restart; detection unaffected | `sup: ui exited 1, restart in 1s` |
| storage | power cut mid-write | CRC/length at startup | truncate torn tail; delete `.tmp`; close open entry as `interrupted` | as above |
| power | unplugged (anyone) | gap before next `boot` record | timeline `OFFLINE` span | `hub: boot after power loss, offline 10:02-10:40 (38m)` |
| clock | RTC battery missing/dead | RTC unset at boot | gap shown as duration unknown | `hub: RTC unset, offline duration unknown` |
| storage | disk near full | `statvfs` before each clip | stop clips, keep events, warn | `store: free 4.8% < floor, clips disabled` |
| storage | `write`/`fsync` EIO | syscall return | log, retry once, exit non-zero | `store: fdatasync: Input/output error` |
| `supervisor` | killed | systemd `Restart=always` | kill stale children (process group), relaunch | `sup: start, cleaned 3 stale children` |
| any child | fd leak | hourly fd count per process | soak evidence; regression test | `hb: ... fds=pird:5 micd:7 ...` |

## 7. Interface

The Pi runs its own Wi-Fi hotspot (WPA2, password set by Macy) with **no
internet uplink**. Macy joins it from her phone and opens the timeline page,
served by `ui` on the hotspot address only. `ui` is a thin display in Python
under `ui/`: it forwards the same queries as the CLI over the hub's query
socket and renders the result. All query logic lives in the C hub. The CLI
`pss` runs on the Pi, over SSH on the hotspot or a local console:

```
$ pss timeline [--since 08:00] [--until now]
09:41:07  ENTRY       routine      4m12s  latch+voice  [12 imgs, 30s audio]
12:03:55  SOUND_ONLY  -              6s   voice        hallway?
13:10:02  OFFLINE     -             38m   -            power lost
15:22:40  ENTRY       UNEXPECTED     38s  latch        [20 imgs, 30s audio]

$ pss events [--type entry|motion|sound] [--json]   # full records, scores, sensor flags
$ pss status    # arm state, expected window, sensors OK/DEGRADED/SUSPECT
$ pss health    # per-process uptime, restarts, RSS, fds, disk free, xruns, drops
```
Setters: `pss arm`, `pss disarm`, `pss expect HH:MM-HH:MM`, `pss settime`.

## 8. Constraints and substitutions

| Ideal | What we use | What it costs us |
|---|---|---|
| Small I2S MEMS mic | Blue Snowball USB mic (owned) | Bulky; USB adds latency and xruns; but unplugging it is a clean, safe fault to inject |
| Door reed switch for ground truth | None (it is a hotel door) | Ground truth from scripted trials and a paper log |
| Video with pre-roll | JPEG bursts on PIR trigger | Pi 5 has no hardware H.264 encoder; we miss the first ~1 s (measured) and get frames, not motion |
| Camera in the fusion | Evidence only | No vision models (none allowed, none we could write in time) |
| Battery, place anywhere | Wall outlet, 5 V / 5 A USB-C | Needs an outlet; anyone can unplug it, hence D and `OFFLINE` records |
| Ironclad PIR-disconnect detection | Single-trigger timing heuristic | Depends on HC-SR501 timing we must verify; a bad sensor stuck high looks the same |
| Arm button only Macy can use | Plain button | Anyone who finds it can disarm during the grace period; the `owner` label is a claim, not proof |
| Real hotel rooms | One test room with a door | Acoustics differ from a hotel's; claim narrowed accordingly |
| Recording with everyone's consent | Audio + images of whoever enters | See privacy note below |

**Privacy and consent.** The device records audio and images of whoever
enters, including hotel staff. We keep this as designed because the evidence
is the point for Macy, and limit it: nothing leaves the device, clips exist
only for confirmed entries, and the quota deletes old ones. Laws on recording
audio differ by place (some require every party's consent), so this is a real
limitation of the product as built, stated in the evaluation report.

**Replayed data.** Labelled audio/PIR traces from scripted trials are replayed
through `micd`'s feature path and the hub (`tools/replay`) to tune `K_imp`,
`K_voice`, and `T_co`. Replayed data is labelled as such everywhere; the soak
and the demo run on live sensors.

## 9. Evaluation plan

| Measurement | Method | Target |
|---|---|---|
| **B: latency, IRQ vs. poll** | Spare GPIO output wired to the input line (sensor detached); toggle with a monotonic timestamp; 1,000 edges per arm (IRQ, poll 1/10/100 ms), idle and under `stress-ng --cpu 4` | IRQ p99 < 1 ms idle, < 5 ms loaded; full CDFs |
| **B: missed pulses** | 1,000 pulses of 20–200 ms per arm | IRQ: 0 missed; report poll misses |
| **B: CPU cost** | `pird` CPU from `/proc/<pid>/stat`, 10 min per arm | report all arms, however small |
| **Detection accuracy** | ≥ 30 scripted trials each: entry, long housekeeping-style entry, hallway noise, fan-blown curtain, empty room; paper log | Confusion matrix; entry recall ≥ 90 %, false ENTRY ≤ 5 % |
| **Entry durability** | 20 power pulls 5–60 s after a confirmed ENTRY | 20/20 entries present after reboot, marked `interrupted` |
| **D: power-cut recovery** | 20 power pulls at random during clip and log writes | 0 corrupt records accepted; 0 dangling clip refs |
| **D: write volume** | `/proc/<pid>/io`, `/sys/block/mmcblk0/stat` | < 1 GB/day at 20 entries/day |
| **E: fault recovery** | `kill -9` each child 10×; unplug mic 10× for 10 min | detection < 1 s (crash) / < 15 s (hang); restart < 5 s; no fd growth |
| **PIR unplug detection** | Unplug PIR 10× at its connector | SUSPECT within 15 s; 0 false ENTRY |
| **Camera start latency** | PIR edge → first JPEG mtime | report distribution (~1 s expected) |
| **Resource footprint** | hourly soak heartbeats | flat RSS (< 1 MB drift after warm-up); plotted |

## 10. Ownership map

Ownership means first authorship and answerability at the defense. Each
partner works in their own clone and Claude Code sessions.

| Subsystem | Path | Owner |
|---|---|---|
| PIR + button capture, IRQ/poll comparison (B) | `src/capture/pir` | Owen |
| Microphone capture + feature extraction | `src/capture/mic` | Owen |
| Camera bursts | `src/capture/cam` | Owen |
| Storage layer (D) | `src/store` | Bryan |
| Background model, detectors, fusion and arm FSMs | `src/analysis` | Bryan |
| Hub, supervisor (E), CLI, UI | `src/hub`, `src/supervisor`, `src/cli`, `ui/` | Shared |
| Replay harness, plotting | `tools/` | Shared |

## 11. AI-use plan and the network boundary

**We use Claude Code for:** planning (plan mode for every multi-file change),
explaining APIs (libgpiod v2, ALSA, epoll, signalfd), boilerplate, tests and
fixtures, adversarial review of diffs, and debugging with pasted evidence.
**We do not use it for:** choosing thresholds or fusion rules without data we
collected, or anything that runs on the device.

**Boundary, legible in one minute.** The only network code in the product is
`ui/server.py`, which binds to the hotspot address and never opens an outbound
connection. Everything in `src/` uses AF_UNIX only. `make netcheck` fails the
build if `src/` contains `AF_INET`, `AF_INET6`, `getaddrinfo`, or `curl`, or if
anything under `ui/` other than `server.py` imports `socket`, `http`, or
`urllib`, or if `server.py` imports an HTTP client or calls `connect`. During
the soak, Ethernet is unplugged and `wlan0` runs only as the hotspot, so there
is no route to the internet.

## 12. Repository layout

```
src/capture/{pir,mic,cam}/   sensor daemons (pir also handles button + LED)
src/analysis/                features, background model, detectors, FSMs
src/store/                   event + stats logs, clip store, recovery, quota
src/hub/                     event loop, IPC, query server
src/supervisor/              supervision, soak heartbeat
src/cli/                     pss command
src/common/                  IPC structs, logging, heartbeat client
ui/                          hotspot timeline page (Python, display only)
tests/                       unit tests, run by `make test`
tools/                       replay harness, plotting (Python)
deploy/                      systemd unit, hotspot config
```
