#!/bin/sh
# Test stand-in for rpicam-still, used only by tests/test_e2e.c (via
# $PSS_RPICAM). Writes -t / --timelapse dummy JPEGs to the -o pattern.
# FAKE_RPICAM_FAIL=1 makes it fail the way a missing camera does.
t=0; tl=1; out=
while [ $# -gt 0 ]; do
	case "$1" in
		-t) t=$2; shift ;;
		--timelapse) tl=$2; shift ;;
		-o) out=$2; shift ;;
	esac
	shift
done
if [ -n "$FAKE_RPICAM_FAIL" ]; then
	echo "fake rpicam-still: no cameras available" >&2
	exit 255
fi
n=$((t / tl)); i=0
while [ "$i" -lt "$n" ]; do
	# shellcheck disable=SC2059  # $out is the printf pattern, by design
	printf '\377\330fake\377\331' > "$(printf "$out" "$i")" || exit 1
	i=$((i + 1))
done
