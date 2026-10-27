#!/bin/bash
# tools/logdump.sh [MB]: the last run's kernel log, from the phone's own `logdump` partition. The loader writes the log there
# at the end of a run (and its end every couple of seconds while a kernel runs on another core, bootcpu=N), so after a run that
# did not come back, or a phone restarted by hand, this is where it is. Needs the phone in Android with adb and root (su).
#   tools/logdump.sh            3 MB into logs/kernel-<time>.log, with a summary
cd "$(dirname "$0")/.." || exit 1
PATH=$PWD/tools/macos-bin/platform-tools:$PATH
mb=${1:-3}
mkdir -p logs
for i in $(seq 1 60); do adb get-state 2>/dev/null | grep -q device && break; sleep 4; done
adb get-state 2>/dev/null | grep -q device || { echo "logdump.sh: the phone is not in Android on adb"; exit 1; }
out=logs/kernel-$(date +%H%M%S).log
adb shell "su -c 'dd if=/dev/block/by-name/logdump bs=1M count=$mb 2>/dev/null'" > "$out" || { echo "logdump.sh: cannot read the partition (root?)"; exit 1; }
echo "logdump.sh: $out ($(wc -c < "$out") bytes)"
grep -a -m1 "^cmdline:" "$out" | cut -c1-200
echo "panics: $(grep -a -c 'panic(' "$out"); the kernel hung: $(grep -a -c 'THE KERNEL HUNG' "$out")"
grep -a "panic(" "$out" | head -3 | cut -c1-200
