#!/bin/sh
# Check that every devices/<name>/ with a run.conf has what the build and run scripts read from it, and that the Makefile
# finds the same settings (so `make phone DEVICE=<name>` runs that phone's arguments, not another's).
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
bad=0
for dir in "$root"/devices/*/; do
	name=$(basename "$dir")
	conf="$dir/run.conf"
	[ -f "$conf" ] || continue
	for key in CODENAME BASE_ARGS PHONE_ARGS PHONE_SECS; do
		sed -n "s/^$key=//p" "$conf" | grep -q . || { echo "device-conf-test: FAIL -- devices/$name/run.conf has no $key"; bad=1; }
	done
	want=$(sed -n 's/^PHONE_ARGS=//p' "$conf")
	got=$(make -s -n -C "$root" phone DEVICE="$name" | sed -n 's/.*modem-cycle.sh "\(.*\)" [0-9]*$/\1/p')
	[ "$want" = "$got" ] || { echo "device-conf-test: FAIL -- make phone DEVICE=$name runs '$got', run.conf says '$want'"; bad=1; }
done
[ "$bad" = 0 ] && echo "device-conf-test: every run.conf is complete and the Makefile reads it"
exit "$bad"
