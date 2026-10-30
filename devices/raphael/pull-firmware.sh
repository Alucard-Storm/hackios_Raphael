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
# The Wi-Fi (an ath10k WCN3998 inside the modem: bsd/dev/hakios.wifi.c, MODEM=1 MPSS_WIFI=1 userland/build.sh). Besides
# wlanmdsp.mbn above it takes the board data for this phone's variant (F11: CN for China, GL for everyone else; the phone's
# own Android picks by ro.boot.hwc, so does this), and the modem's own non-volatile files, which it asks for at start: raw
# copies of the partitions modemst1, modemst2, fsg and fsc. Those hold this phone's identity (IMEI, calibration): they stay
# on this PC (and in the image built from them), never in git or in an image for anybody else. Kept as they are if already
# pulled, so that one snapshot is used until you delete it.
pull fw-wifi /vendor/firmware_mnt/image/bd_f11cn.bin /vendor/firmware_mnt/image/bd_f11gl.bin
hwc=$(adb shell 'getprop ro.boot.hwc' | tr -d '\r')
if [ "$hwc" = CN ]; then echo bd_f11cn.bin > fw-wifi/bdf; else echo bd_f11gl.bin > fw-wifi/bdf; fi
echo "pull-firmware.sh: this phone is $hwc: Wi-Fi board data $(cat fw-wifi/bdf)"
for part in modemst1 modemst2 fsg fsc; do
	if [ -s "fw-wifi/nv-$part" ]; then
		echo "pull-firmware.sh: fw-wifi/nv-$part is there already (delete it to take a new snapshot)"
	else
		adb exec-out "su -c 'dd if=/dev/block/by-name/$part bs=1048576 2>/dev/null'" > "fw-wifi/nv-$part"
		chmod 600 "fw-wifi/nv-$part"
		echo "pull-firmware.sh: fw-wifi/nv-$part: $(stat -c %s "fw-wifi/nv-$part" 2>/dev/null || stat -f %z "fw-wifi/nv-$part") bytes from the partition"
	fi
done
# not from the phone: the 3 MB the modem's file service is given (it starts zeroed), and the ath10k firmware file the host driver
# insists on (the firmware itself runs in the modem): QCA-ATH10K, version, features and the like, 60 bytes
head -c 3211264 /dev/zero > fw-wifi/0-rmtfs-ram
printf '\x51\x43\x41\x2d\x41\x54\x48\x31\x30\x4b\x00\x77\x01\x00\x00\x00\x04\x00\x00\x00\xa4\xe4\xbe\x5b\x02\x00\x00\x00\x03\x00\x00\x00\x40\x00\x0c\x77\x05\x00\x00\x00\x04\x00\x00\x00\x04\x00\x00\x00\x06\x00\x00\x00\x04\x00\x00\x00\x03\x00\x00\x00' > fw-wifi/firmware-5.bin
pull fw-audio /vendor/firmware_mnt/image/adsp.mdt $(for i in $(seq -w 0 13); do echo /vendor/firmware_mnt/image/adsp.b$i; done)
pull fw-modem /vendor/firmware_mnt/image/modem.mdt $(for i in $(seq -w 0 31); do echo /vendor/firmware_mnt/image/modem.b$i; done)
adb shell "su -c 'cp -r /vendor/firmware_mnt/image $stage/image && chmod -R a+rX $stage/image'" && mkdir -p fw-modem && adb pull $stage/image fw-modem/ >/dev/null 2>&1 && echo "pull-firmware.sh: fw-modem/image: the modem's own files (TFTP)"
pull fw-camera $(adb shell 'su -c "ls /vendor/lib64/camera/com.qti.sensormodule.raphael_*.bin /vendor/lib64/camera/com.qti.tuned.raphael_*.bin"' 2>/dev/null | tr -d '\r')
echo "pull-firmware.sh: done. Touch is a Goodix GT9886 (/vendor/firmware/goodix_gt9886_*); no haptic firmware file is pulled yet (not identified on this phone)"
