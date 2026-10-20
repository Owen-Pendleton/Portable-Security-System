# Portable Security System: team rules

## Commands
- Build: `make`    Cross/deploy: `make deploy PI=pi@<host>`
- Tests: `make test`    Sanitizers: `make asan`    Valgrind: `make memcheck`
- Network boundary check: `make netcheck` (fails if network code appears
  outside the hotspot UI server; see DESIGN.md Section 11)
- Soak sanity: `make soakcheck` (1-hour miniature of the 48h run)
- A change is DONE only when build, test, asan, and netcheck pass. Show output.

## Hard constraints
- The PRODUCT makes no network calls. Its only network code is the
  local UI server, bound to the Pi's own hotspot address (no internet
  uplink, no outbound connections). No LLM APIs, no cloud inference,
  no pretrained models. The intelligence in src/analysis is ours.
  This is a graded boundary.
- Every daemon must be supervisable: clean exit codes, no orphaned
  fds across restart, heartbeat within 60s of start.
- Every allocation checked; every syscall's error path handled and
  logged. The 48-hour soak is the test suite of last resort.
- NEVER weaken, skip, or delete a test to make the suite pass.

## Project facts (see DESIGN.md)
- Pi 5, wall powered (no battery). RTC backup battery is required: wall
  time across power cuts depends on it.
- Sensors: HC-SR501 PIR (GPIO), Blue Snowball USB mic (ALSA), OV5647
  camera (CSI, evidence only), arm button + LED (GPIO).
- Safe to hot-unplug for fault injection: the USB mic. The PIR only via
  its keyed connector. NEVER hot-unplug the CSI camera ribbon.

## Ownership
- Owen: capture daemons and arm button (src/capture/{pir,mic,cam})
- Bryan: storage layer, analysis engine (src/store, src/analysis)
- Shared: hub, supervisor, CLI, UI (src/hub, src/supervisor, src/cli, ui/)
- Each partner works in their own clone and their own Claude Code
  sessions; never prompt jointly in one session. Copy transcripts to
  CLAUDE-TRANSCRIPTS/<name>/ at every milestone.
- The agent edits outside the session owner's area only when told
  explicitly to.

## Style
- Systems core: C17, -Wall -Wextra -Werror, no VLAs.
  goto-cleanup for multi-resource functions.
- Python permitted only under tools/ and ui/. No graded mechanism
  may live there.
- Smallest diff that passes. Do not refactor unrelated code.

## Workflow
- Multi-file or algorithmic change: plan first, wait for approval.
- Hardware bugs: paste real evidence (dmesg, timing capture,
  /proc/interrupts). No fixes proposed from a verbal description.
- Commit only from a green state; message format "M<n>: <what>".
