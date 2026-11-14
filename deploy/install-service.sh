#!/bin/sh
# One-time setup on the Pi, after `make deploy`: installs deploy/pss.service
# so the supervisor (and every daemon) starts at boot. Then plugging the Pi
# in is enough; the arm button does the rest.
#   deploy/install-service.sh            install, enable, start now
#   deploy/install-service.sh --remove   stop, disable, uninstall
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
unit=/etc/systemd/system/pss.service

if [ "${1:-}" = "--remove" ]; then
	sudo systemctl disable --now pss || true
	sudo rm -f "$unit"
	sudo systemctl daemon-reload
	echo "pss.service removed"
	exit 0
fi

if [ ! -x "$root/build/supervisor" ]; then
	echo "build/supervisor missing: run make first" >&2
	exit 1
fi
for b in hub camd pird micd; do
	[ -x "$root/build/$b" ] || echo "warning: build/$b missing; the supervisor will keep retrying it" >&2
done

tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
sed -e "s|@USER@|$(id -un)|g" -e "s|@ROOT@|$root|g" "$root/deploy/pss.service" > "$tmp"
sudo install -m 0644 "$tmp" "$unit"
sudo systemctl daemon-reload
sudo systemctl enable --now pss
systemctl --no-pager status pss | head -n 12
echo "installed: starts at every boot. Logs: journalctl -u pss -f"
