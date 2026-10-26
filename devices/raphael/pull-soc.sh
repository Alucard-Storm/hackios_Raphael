#!/bin/bash
# Pull the phone's own device tree over adb and turn it into the SoC boot-args HakiOS needs on this
# device (tools/soc-args.py). The Redmi K20 Pro's stock Android does not let the adb shell read
# /sys/firmware/fdt, so this needs root (su). Two things the device tree does not carry are read
# from the running kernel here: the command DB's address (/proc/iomem, "cmd-db").
set -e
cd "$(dirname "$0")/../.."
PATH=$PWD/tools/macos-bin/platform-tools:$PATH
out=$(dirname "$0")/soc.args
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
adb exec-out 'su -c "cat /sys/firmware/fdt"' > "$tmp/fdt"
[ "$(head -c4 "$tmp/fdt" | xxd -p)" = d00dfeed ] || { echo "pull-soc.sh: cannot read /sys/firmware/fdt through su (is the phone rooted, adb allowed?)"; exit 1; }
cp "$tmp/fdt" "$(dirname "$0")/prebuilt/device-tree.dtb"
tools/soc-args.py "$tmp/fdt" > "$out"
cmd_db=$(adb shell 'su -c "grep cmd-db /proc/iomem"' | tr -d '\r' | sed -n 's/^\([0-9a-f]*\)-.*/0x\1/p' | head -1)
[ -n "$cmd_db" ] && echo "soc_cmd_db=$cmd_db" >> "$out"
cat "$out"
echo "pull-soc.sh: wrote $out"
