#!/bin/bash
# Pull the Redmi K20 Pro's own firmware out of its Android into the fw-*/ directories at the top of
# the repository (never committed: it is the vendor's). Needs adb and root (su): /vendor/firmware and
# /vendor/firmware_mnt are not readable by the adb shell. The files are copied through su to
# /data/local/tmp, pulled, and removed.
set -e
cd "$(dirname "$0")/../.."
PATH=$PWD/tools/macos-bin/platform-tools:$PATH
adb get-state >/dev/null
stage=/data/local/tmp/hakios-fw
adb shell "su -c 'rm -rf $stage; mkdir -p $stage; chmod 777 $stage'"
trap 'adb shell "su -c \"rm -rf $stage\"" >/dev/null 2>&1' EXIT
pull() { # DIR SOURCE...  (missing files are told, not fatal)
	local dir=$1; shift
	mkdir -p "$dir"
	for f in "$@"; do
		adb shell "su -c 'cp $f $stage/ && chmod 644 $stage/$(basename "$f")'" >/dev/null 2>&1 \
			&& adb pull "$stage/$(basename "$f")" "$dir/" >/dev/null 2>&1 \
			|| echo "pull-firmware.sh: $f: not on the phone"
		adb shell "su -c 'rm -f $stage/$(basename "$f")'" >/dev/null 2>&1
	done
	echo "pull-firmware.sh: $dir: $(ls -1 "$dir" | wc -l | tr -d ' ') files"
}
# Adreno 640 (SM8150): the GMU, the SQE (a630_sqe.fw is the one a640 uses) and the zap shader
pull fw-gpu /vendor/firmware/a640_gmu.bin /vendor/firmware/a630_sqe.fw /vendor/firmware/a640_zap.mdt /vendor/firmware/a640_zap.b00 /vendor/firmware/a640_zap.b01 /vendor/firmware/a640_zap.b02
# the WCN3998 is not in the device tree as a separate DSP (no wpss): the Wi-Fi board data and wlanmdsp
pull fw-wifi /vendor/firmware_mnt/image/wlanmdsp.mbn /vendor/firmware_mnt/image/bdwlan.bin $(for i in 102 104 105 106 107 108 109; do echo /vendor/firmware_mnt/image/bdwlan.$i; done)
pull fw-audio /vendor/firmware_mnt/image/adsp.mdt $(for i in $(seq -w 0 13); do echo /vendor/firmware_mnt/image/adsp.b$i; done)
pull fw-modem /vendor/firmware_mnt/image/modem.mdt $(for i in $(seq -w 0 31); do echo /vendor/firmware_mnt/image/modem.b$i; done)
adb shell "su -c 'cp -r /vendor/firmware_mnt/image $stage/image && chmod -R a+rX $stage/image'" && mkdir -p fw-modem && adb pull $stage/image fw-modem/ >/dev/null 2>&1 && echo "pull-firmware.sh: fw-modem/image: the modem's own files (TFTP)"
pull fw-camera $(adb shell 'su -c "ls /vendor/lib64/camera/com.qti.sensormodule.raphael_*.bin /vendor/lib64/camera/com.qti.tuned.raphael_*.bin"' 2>/dev/null | tr -d '\r')
echo "pull-firmware.sh: done. Touch is a Goodix GT9886 (/vendor/firmware/goodix_gt9886_*); no haptic firmware file is pulled yet (not identified on this phone)"
