#!/bin/sh
# M3 demo on the Pi: starts hub, camd and pird (and micd with --mic) in the
# foreground. Ctrl-C stops all of them cleanly. Other arguments go to pird,
# e.g. --line=17 --chip=/dev/gpiochip0 --warmup=60 --poll=10.
#   --mic                    also start micd (sound onsets open SOUND_ONLY events)
#   --mic-device=<alsa pcm>  e.g. plughw:CARD=Snowball (default: ALSA "default")
#   --no-pir                 don't start pird (PIR not wired: a floating line
#                            would make bogus edges)
# M3 mic + camera demo:  deploy/run-m3.sh --mic --mic-device=plughw:CARD=<name> --no-pir
#
# Data goes to $PSS_DATA_DIR (default /var/lib/pss); read it back with
#   build/pss events
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
bin=$root/build
export PSS_DATA_DIR="${PSS_DATA_DIR:-/var/lib/pss}"
export PSS_RUN_DIR="${PSS_RUN_DIR:-/run/pss}"

mic=0
pir=1
mic_dev=default
pird_args=
for a in "$@"; do
	case "$a" in
		--mic) mic=1 ;;
		--mic-device=*) mic_dev=${a#--mic-device=} ;;
		--no-pir) pir=0 ;;
		*) pird_args="$pird_args $a" ;;
	esac
done

for d in "$PSS_DATA_DIR" "$PSS_RUN_DIR"; do
	if [ ! -d "$d" ] || [ ! -w "$d" ]; then
		sudo install -d -o "$(id -u)" -g "$(id -g)" "$d"
	fi
done

pids=
cleanup() {
	trap - INT TERM EXIT
	for p in $pids; do
		kill -TERM "$p" 2>/dev/null || true
	done
	wait
}
trap cleanup INT TERM EXIT

"$bin/hub" & pids="$pids $!"
"$bin/camd" & pids="$pids $!"
if [ "$pir" = 1 ]; then
	# shellcheck disable=SC2086  # word-splitting pird_args is intended
	"$bin/pird" $pird_args & pids="$pids $!"
fi
if [ "$mic" = 1 ]; then
	"$bin/micd" --device="$mic_dev" & pids="$pids $!"
fi
wait
