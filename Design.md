# Design Document: Portable Security (Owen Pendleton, Bryan Rosas)

**Milestone:** M2. **Status:** draft for review. This document will change as we
meet the hardware; M5's version will carry a changelog of what this one got wrong.

## 1. Summary

Macy travels about three nights a month and leaves a laptop, cash, and bags in a
hotel room all day. Our device sits in the room while she is out and, when she
returns, answers one question with a short timeline: *did anyone come in, when,
and for how long?* It runs fully offline (no Wi-Fi, no cloud, no account) and
keeps every byte on its own SD card.

**The narrowed claim we will defend:** on our hardware, in a test room, the
device separates *door-entry events* (motion plus a door/voice sound signature)
from *motion-only* and *sound-only* events with a measured confusion matrix,
labels each entry routine or unexpected against a housekeeping window Macy sets,
and keeps a crash-consistent record of it through power cuts and sensor faults.

**Mechanisms (Section 3.2 menu):** **B** interrupt-driven PIR input, compared
head-to-head with polling; **D** a custom crash-consistent storage layer; **E** a
multi-process architecture with a supervisor that survives any child dying.

**Hardware:** Raspberry Pi 5 · HC-SR501 PIR (GPIO) · Blue Snowball USB
microphone (ALSA) · Arducam 5MP OV5647 camera (CSI, via `rpicam-*` tools).
PIR and microphone are the two *cooperating* sensors; the camera is an evidence
sensor that is triggered by the PIR and kept only when fusion confirms an entry.

## 2. Architecture

```
 HC-SR501 PIR          Blue Snowball (USB)        Arducam OV5647 (CSI)
     | GPIO edge            | USB isoc audio            | CSI-2
=====|======================|===========================|==================== kernel
 RP1 gpiochip            snd-usb-audio             libcamera / rpicam-*
 (line-edge events,      (ALSA PCM, 44.1 kHz       (JPEG stills on demand)
  kernel timestamps)      s16 mono = 88.2 KB/s)
=====|======================|===========================|==================== user
     v                      v                           v
 +--------+           +-----------+              +-----------+
 |  pird  |           |   micd    |              |   camd    |
 | B: IRQ |           | 20 ms FFT |              | burst on  |
 | or poll|           | features; |              | trigger;  |
 +--------+           | 10 s PCM  |              | ~2 fps    |
     |                | ring      |              +-----------+
     | edge msgs      +-----------+                 ^     |
     | (<1 Hz typ,         | feature frames         |     | frame-ready msgs
     |  ~40 B each)        | 50 Hz x ~64 B          |     | (2 Hz, path only)
     |                     | = 3.2 KB/s             |     |
     v                     v                        |     v
 +----------------------------------------------------------------+
 |                         hub (single writer)                    |
 |  analysis/: background model -> detectors -> fusion FSM        |
 |  store/:    event log (append, CRC) + clip store (quota)       |
 |  query socket for cli                                          |
 +----------------------------------------------------------------+
     |  writes: event records ~0.1-20/hr, fsync per closed event
     |          clips ~6-9 MB per confirmed entry
     |          stats/heartbeat records batched, 1/min
     v
 SD card: /var/lib/pss/{log/, clips/, state/}

 supervisor (E): fork/execs pird, micd, camd, hub; reaps with waitpid;
 receives 5 s heartbeats from each child over AF_UNIX; writes the hourly
 soak heartbeat (liveness, RSS, event counts) to its own log.

 cli  --AF_UNIX query socket-->  hub      (timeline, events, status, health)
```

All IPC is AF_UNIX `SOCK_SEQPACKET`: message boundaries are preserved, and a
dead peer shows up as EOF/EPIPE, which is exactly the fault signal E needs.
The product opens no AF_INET socket anywhere (Section 11).

| Process | Threads | Priority | Inputs → outputs |
|---|---|---|---|
| `supervisor` | 1 (epoll loop: signalfd, child sockets, timerfd) | default | heartbeats → restarts, soak log |
| `pird` | 1 (blocks in `gpiod_line_request_read_edge_events` or poll timer) | default (C is not claimed) | GPIO → edge msgs |
| `micd` | 2: ALSA capture thread, feature thread, joined by an SPSC ring | default | PCM → feature frames, audio clip on request |
| `camd` | 1 + `rpicam-still` child per burst | default | trigger → JPEG files in `clips/*.tmp` |
| `hub` | 1 (epoll loop over all child sockets + query socket + timerfd) | default | features + edges → events, storage |
| `cli` | 1, short-lived | n/a | user query → text |

## 3. Mechanism mapping

### B: Interrupt-driven PIR input (`pird`, `src/capture/pir`)
**Implementation.** `pird` requests the PIR line through libgpiod v2 with both
edges, pull-up bias, and kernel event timestamps (`CLOCK_MONOTONIC`). It blocks
in the kernel until an edge arrives; the GPIO IRQ handler in the RP1 driver
timestamps the edge and wakes us. A `--poll=<ms>` mode reads the line value on a
`timerfd` instead and is the comparison arm.
**Why Macy needs it.** The PIR changes state a few times a day, while the same
four cores run audio FFTs every 20 ms and camera bursts. Spinning on a pin for
an input that rarely changes takes CPU from the work that does. And the fusion window (Section 4) correlates a PIR edge with an audio
transient within a second or two, so edge timestamps must be accurate to well
under the 20 ms audio frame; a poll interval long enough to be cheap blurs that.
**Measured.** Edge-to-userspace latency distribution and `pird` CPU utilization,
IRQ vs. poll at 1, 10, 100 ms, idle and under `stress-ng` load (Section 9).

### D: Custom storage layer (`src/store`, linked into `hub`)
**Implementation.** Append-only, CRC32-framed event log in segment files;
atomic clip commits (tmp → fsync → rename → fsync dir); startup recovery; a
byte quota with oldest-first eviction. Details in Section 5.
**Why Macy needs it.** The device runs from the room's wall outlet, and hotel
outlets get used: housekeeping unplugs things to run a vacuum, Macy unplugs it
to pack, a power strip gets kicked.
The record exists to settle "did someone come in?" after the fact; a log that
is corrupt or silently truncated after a power loss fails at the only moment it
matters. The SD card is also small and wears, and clips are the bulk of writes
(the memo's named risk).
**Measured.** Power-pull recovery (records lost, records corrupt), write
amplification and bytes/day, fsync count and latency.

### E: Multi-process architecture with supervision (`src/supervisor`, `src/hub`)
**Implementation.** One process per sensor, one hub, one supervisor. Children
send a heartbeat every 5 s. The supervisor restarts any child that exits or
misses heartbeats for 15 s (SIGKILL first), with exponential backoff 1 s → 60 s
that resets after 10 min of health. Exit code `3` (device gone) is distinguished
from a crash: the supervisor retries slowly and the hub enters a DEGRADED mode
for that sensor instead of storming restarts.
**Why Macy needs it.** A security monitor that silently dies is worse than none,
because Macy believes it is on duty. If the camera stack hangs or the USB mic
drops off the bus, the PIR record must keep flowing and the timeline must say
plainly which sensors were offline and when.
**Measured.** `kill -9` each child and unplug the mic during runs: detection
time, recovery time, events lost during the gap, and the log excerpt.

## 4. Analysis engine (`src/analysis`, all ours)

**Audio features (in `micd`).** 20 ms hops (882 samples) over a 1024-point Hann
window. Per frame: RMS, energy in 8 log-spaced bands (100 Hz – 8 kHz), and
spectral flux (sum of positive band-energy increases). FFT via a small library
or our own radix-2; either way it is a primitive we can explain.

**Background model.** For each band, an exponentially weighted mean and
variance (time constant ~60 s) updated only on frames not inside an active
event, so a long conversation does not teach the model that voices are
background. Each frame is scored as a per-band z-score. This is what lets the
same device work next to a loud HVAC unit and in a silent room.

**Detectors.**
- *Impulse (latch/door):* spectral-flux z-score spike above `K_imp` with a short
  decay (< 150 ms) and broadband energy, the shape of a latch or door thud.
- *Voice:* z-score in the 300–3400 Hz bands above `K_voice` sustained for ≥ 400 ms
  within a 2 s window, with low-band (< 200 Hz) energy not dominant (rules out HVAC).
- *PIR:* rising edge debounced; HC-SR501's hardware block time (~2.5 s) and
  retrigger mode noted in config.

**Fusion state machine (in `hub`).**
```
IDLE --PIR rise--> MOTION_WINDOW (open ±T_co = 3 s around the edge, look back in audio history)
MOTION_WINDOW --impulse or voice in window--> ENTRY (confirmed)
MOTION_WINDOW --window expires, no audio--> MOTION_ONLY
IDLE --impulse/voice with no PIR in ±T_co--> SOUND_ONLY (hallway)
ENTRY/MOTION_ONLY --no PIR high and no voice for T_quiet = 30 s--> close event
```
An event records start, end, duration, which detectors fired, peak scores,
sensors available (DEGRADED flags), and clip references. Opening and closing use
different conditions (hysteresis), so a person moving around the room for ten
minutes is one event, not forty.

**Routine vs. unexpected.** Macy sets an expected window (`cli expect
10:00-14:00`). Confirmed entries inside it are labelled `routine`; outside it,
`UNEXPECTED`. The device does not try to identify who entered.

**Why a threshold cannot do this.** A fixed loudness threshold fires on every
slammed door in the hallway and misses a quiet entry in a noisy room; a PIR
alone fires on curtains moved by HVAC and sunlight patches. The decision depends
on the *temporal coincidence* of two different physical signals, scored against
a background that the device learned in *this* room, with hysteresis for event
boundaries. Each piece is simple; no single `if` reproduces the combination.

## 5. Storage and data

| Data | Rate | Format | Retention |
|---|---|---|---|
| Event records | ~0.1–20 /hr | binary, `[len u32][type u16][ver u16][payload][crc32]` | quota-managed, never evicted before clips |
| Stats / heartbeat records | 1 /min, batched | same framing | 7 days |
| Audio clips (confirmed entries only) | 10 s pre-roll + ≤ 20 s post, 88.2 KB/s ≈ 2.6 MB | WAV | quota (oldest first) |
| Image bursts (confirmed entries only) | ~2 fps × 10 s, ~150–300 KB each ≈ 3–6 MB | JPEG | quota (oldest first) |
| Daemon text logs | low | line-oriented, timestamped | logrotate by size, 4 × 5 MB |

**Event log.** Segment files `log/seg-NNNNNN.pel` capped at 4 MB. Records are
appended with `write()` and the segment is `fdatasync`ed when an event *closes*
(low rate, so per-event durability is affordable). Stats records are buffered
in memory and flushed once a minute, so a power cut loses at most a minute of
stats but never a closed event.

**Clips.** Written as `clips/<event-id>.{wav,dir}.tmp`; on completion: `fsync`
file → `rename` to final name → `fsync` the directory. The event record that
references a clip is written only after the rename is durable, so the log never
points at a missing clip. Unconfirmed bursts (MOTION_ONLY / SOUND_ONLY) are
deleted, not kept. That is the main defence against the memo's storage risk.

**Recovery (at hub start).** Scan the newest segment; stop at the first record
with a bad length or CRC; `ftruncate` there; log `store: recovered seg=N
valid=R truncated_bytes=B`. Delete every `*.tmp` in `clips/` and log each one.
Rebuild the in-memory index (event id → offset) from the segments.

**Power-loss gaps.** The hub writes a `boot` record at every start. Stats
records land once a minute, so the span between the last durable record and the
`boot` record is when the device was off, accurate to about a minute. `timeline`
shows that span as an `OFFLINE` line, so Macy knows the device was not watching
then, instead of reading silence as "nobody came in".

**Quota.** Default 2 GiB for `clips/`. Before a new clip, evict the oldest clips
until there is room. At ~6–9 MB per confirmed entry, that is ~250 entries,
several trips' worth. A free-space floor (5 % of the filesystem) stops all clip
writes before the card fills, while event records still go through.

**Time.** There is no NTP offline. Wall-clock timestamps come from the Pi 5's
RTC (with its backup battery fitted) or `cli settime`; every interval and
ordering decision uses `CLOCK_MONOTONIC`. Records carry both, so a wrong wall
clock can be corrected after the fact without reordering events.

## 6. Failure-mode table

| Component | Failure | Detected by | System response | Log shows |
|---|---|---|---|---|
| `micd` | Snowball unplugged | ALSA read returns `-ENODEV`/`-EIO` | exit 3; supervisor retries every 30 s; hub marks audio DEGRADED; fusion falls back to PIR-only, events tagged `unconfirmed (audio offline)` | `micd: snd_pcm_readi: No such device; exit 3` / `sup: micd device-gone, retry 30s` / `hub: DEGRADED audio` … `hub: RECOVERED audio after 612s` |
| `micd` | crash / `kill -9` | `SIGCHLD` + `waitpid` status | restart with backoff; events during gap tagged DEGRADED | `sup: micd killed by signal 9, restart #3 in 1s` |
| `micd` | ALSA overrun (xrun) | `-EPIPE` from read | `snd_pcm_prepare`, count it, continue | `micd: xrun #N recovered` (counted in heartbeat) |
| `pird` | crash / hang | `SIGCHLD` / missed heartbeat 15 s | SIGKILL + restart; hub marks PIR DEGRADED | `sup: pird heartbeat missed 15s, SIGKILL` |
| PIR sensor | unplugged | line held high by pull-up for > 300 s with no voice activity (**heuristic**: a person in constant motion could also do this) | flag PIR `SUSPECT`, keep running, surface in `status` | `pird: line stuck high 300s, SUSPECT` |
| PIR sensor | warm-up false triggers (first ~60 s after power) | uptime < 60 s | ignore edges during warm-up | `pird: warm-up, edges ignored until T+60s` |
| `camd` | `rpicam-still` fails / camera absent | non-zero child exit, missing files | event kept without images, tagged `no-images`; camd backoff | `camd: rpicam-still exit 255; burst skipped` |
| `camd` | crash / `kill -9` | `SIGCHLD` | restart; in-flight `.tmp` burst cleaned on next start | `sup: camd killed by signal 9` |
| `hub` | crash / `kill -9` | `SIGCHLD` | restart; recovery scan runs; sensor daemons reconnect (they retry connect with backoff and drop frames while disconnected, counted) | `store: recovered seg=12 valid=341 truncated_bytes=58` |
| storage | power cut mid-write | CRC/length check at startup | truncate torn tail, delete `.tmp` clips | as above |
| power | device unplugged (housekeeping, Macy, or an intruder) | at next boot: gap between last durable record and new `boot` record | timeline shows an `OFFLINE` span; nothing is recorded while off | `hub: boot after power loss, offline 10:02-10:40 (38m)` |
| storage | disk near full | `statvfs` before each clip | stop clips, keep events, raise `health` warning | `store: free 4.8% < floor, clips disabled` |
| storage | `write`/`fsync` error (EIO) | syscall return | log, retry once, then exit non-zero so supervisor restarts hub | `store: fdatasync: Input/output error` |
| `supervisor` | itself killed | systemd (`Restart=always`) | systemd restarts it; it kills stale children via the pidfile/process group before relaunching | `sup: start, cleaned 3 stale children` |
| any child | fd leak across restarts | hourly heartbeat records `/proc/<pid>/fd` count | soak evidence; regression test | `hb: ... fds=pird:5 micd:7 ...` |

## 7. Interface: CLI (`src/cli`)

Offline only: the CLI runs on the Pi (local console or USB-serial) and talks to
the hub over `/run/pss/query.sock`. Four verbs, each with exact semantics:

```
$ pss timeline [--since 08:00] [--until now]     # human-readable, one line per event
09:41:07  ENTRY       routine     4m12s  latch+voice  [12 imgs, 30s audio]
12:03:55  SOUND_ONLY  -             6s   voice        hallway?
13:10:02  OFFLINE     -           38m   -            power lost
15:22:40  ENTRY       UNEXPECTED   38s  latch         [20 imgs, 30s audio]

$ pss events [--type entry|motion|sound] [--json]   # full records incl. scores, sensor flags
$ pss status                                        # armed?, expected window, sensors OK/DEGRADED/SUSPECT
$ pss health                                        # per-process uptime, restarts, RSS, fds, disk free, xruns, drops
```
Plus two setters: `pss expect HH:MM-HH:MM` and `pss settime`.

## 8. Constraints and substitutions

| Ideal | What we use | What it costs us |
|---|---|---|
| Small embedded I2S MEMS mic | Blue Snowball USB mic (owned) | Bulky; USB audio adds latency and xruns are possible; but unplugging it is a clean, safe fault to inject and demo |
| Door reed switch for automatic ground truth | None installed (it is Macy's hotel door, not ours) | Ground truth comes from scripted trials and a paper log with timestamps; fewer labels and more effort |
| Continuous video with pre-roll | JPEG still bursts on PIR trigger | Pi 5 has no hardware H.264 encoder; software video would take a large share of the CPU that audio analysis needs. We miss the first ~1 s (camera start latency, measured) and get frames, not motion |
| Camera in the fusion | Camera as evidence only | We run no vision models (no pretrained models allowed and no budget to write one); images are for Macy to look at |
| Battery power, so it can sit anywhere in the room | Wall outlet via a Pi 5 USB-C supply (5 V / 5 A) | Must sit within cable reach of an outlet; anyone, housekeeping included, can unplug it. That is a power cut, hence D and the `OFFLINE` gap record (Section 5) |
| Reliable PIR-disconnect detection | Pull-up "stuck high" heuristic | Can be fooled; stated as a limitation |
| Real hotel rooms | One test room (dorm/apartment) with a door | Acoustics and hallway noise are not a hotel's; claim narrowed accordingly |

Replayed data: we will record labelled audio/PIR traces from scripted trials and
replay them through `micd`'s feature path and the hub (`tools/replay`) to tune
`K_imp`, `K_voice`, and `T_co`. Replayed data is labelled as such everywhere it
appears; the soak and the demo run on live sensors.

## 9. Evaluation plan

| Measurement | Method | Target |
|---|---|---|
| **B: PIR latency, IRQ vs. poll** | Drive a spare GPIO output into the PIR input line (PIR disconnected); toggle with a timestamp from the same monotonic clock; compute toggle → `pird` receipt. 1,000 edges per arm; arms: IRQ, poll 1/10/100 ms; idle and under `stress-ng --cpu 4` | IRQ p99 < 1 ms idle, < 5 ms loaded; report full CDFs |
| **B: CPU cost** | `pird` CPU from `/proc/<pid>/stat` over 10 min each arm | IRQ ≈ 0 %; report poll cost |
| **Detection accuracy (domain metric)** | ≥ 30 scripted trials each: real entry (door + walk in), housekeeping-style long entry, hallway noise only, non-human motion (fan-blown curtain), empty room. Paper log for ground truth | Confusion matrix; entry recall ≥ 90 %, false ENTRY on non-entry trials ≤ 5 % |
| **Event-to-record latency** | PIR edge timestamp → event record durable | p95 < 5 s after window closes |
| **D: power-cut recovery** | 20 power pulls at random times during clip and log writes | 0 corrupt records accepted; 0 dangling clip refs; lost data ≤ last open event |
| **D: write volume** | bytes written per day from `/proc/<pid>/io` and `/sys/block/mmcblk0/stat` | report; < 1 GB/day at 20 entries/day |
| **E: fault recovery** | `kill -9` each child 10×; unplug mic 10× for 10 min | detection < 1 s (crash) / < 15 s (hang); restart < 5 s; no fd growth |
| **Camera start latency** | PIR edge → first JPEG mtime | report distribution (expected ~1 s) |
| **Resource footprint** | hourly heartbeats over the 48 h soak: RSS, CPU, fds per process | flat RSS (< 1 MB drift after warm-up); plotted |

## 10. Ownership map

Owners are **to be decided by the team**; this table is the list to assign.
Ownership means first authorship and answerability at the defense.

| Subsystem | Path | Owner |
|---|---|---|
| PIR capture + IRQ/poll comparison (B) | `src/capture/pir` | TBD |
| Microphone capture + feature extraction | `src/capture/mic` | TBD |
| Camera burst capture | `src/capture/cam` | TBD |
| Storage layer (D) | `src/store` | TBD |
| Analysis: background model, detectors, fusion FSM | `src/analysis` | TBD |
| Hub (event loop, IPC server, query handling) | `src/hub` | Shared |
| Supervisor (E) | `src/supervisor` | Shared |
| CLI | `src/cli` | Shared |
| Replay harness, plotting | `tools/` | TBD |

Once decided, the same split goes into CLAUDE.md's Ownership section.

## 11. AI-use plan and the no-network boundary

**We use Claude Code for:** planning (plan mode first for every multi-file
change), explaining APIs (libgpiod v2, ALSA, epoll, signalfd), drafting
boilerplate, writing tests and fixtures, adversarial review of diffs, and
debugging with pasted evidence (dmesg, `/proc/interrupts`, timing captures).
**We do not use it for:** choosing detector thresholds or fusion rules without
data we collected, or anything that runs on the device at runtime.

**Boundary, legible in one minute.** The product links no network library and
opens only AF_UNIX sockets. `make netcheck` greps `src/` for `AF_INET`,
`AF_INET6`, `getaddrinfo`, `connect(` outside `AF_UNIX` helpers, `curl`, and
`http`, and fails the build if any appear. The device runs with Wi-Fi and
Ethernet disabled during the soak, which is the strongest possible check.

## 12. Proposed repository layout

```
src/capture/{pir,mic,cam}/   sensor daemons
src/analysis/                features, background model, detectors, fusion
src/store/                   event log, clip store, recovery, quota
src/hub/                     event loop, IPC, query server
src/supervisor/              process supervision, soak heartbeat
src/cli/                     pss command
src/common/                  IPC message structs, logging, heartbeat client
tests/                       unit tests (store recovery, FSM, ring), run by `make test`
tools/                       Python: replay harness, plotting (no graded mechanism)
deploy/                      systemd unit for supervisor
```
