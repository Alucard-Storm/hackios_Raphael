#!/bin/bash
# Redmi K20 Pro (raphael): set everything up and boot iOS. Run it again at any time: a step whose
# result is already there is skipped, so with everything built it only boots.
#
#   devices/raphael/setup.sh              set up what is missing, then boot iOS (about 5 minutes)
#   devices/raphael/setup.sh --check      only say what is there and what is missing; changes nothing
#
#   --secs N        how long iOS runs before the phone goes back to the loader (default: device.conf)
#   --size GiB      size of the image file made inside Android's /data, once (default 16)
#   --no-persist    boot from RAM only: nothing is kept between boots, and Android's /data is not touched
#   --rebuild       build the kernel, loader and system image again even if they exist
#   --yes           do not ask before the big downloads and before making the image file
#
# What it needs: a Linux PC; the phone with USB debugging, an unlocked bootloader and root (KernelSU)
# on its Android, on a USB cable. Nothing is flashed. The image file (--size) is one fully allocated
# file, /data/local/hakios/ios.img, pinned so that Android never moves it; iOS keeps its data (the
# home screen, settings, apps' data) in it between boots, and Android goes on working as it was.
# Delete the file to take the space back (adb shell su -c 'rm /data/local/hakios/ios.img').
set -u
cd "$(dirname "$0")/../.." || exit 1
export DEVICE=raphael

SIZE_GIB=16 SECS= PERSIST=1 CHECK=0 REBUILD=0 YES=0
while [ $# -gt 0 ]; do
	case "$1" in
	--check) CHECK=1 ;;
	--secs) SECS=${2:?--secs N}; shift ;;
	--size) SIZE_GIB=${2:?--size GiB}; shift ;;
	--no-persist) PERSIST=0 ;;
	--rebuild) REBUILD=1 ;;
	--yes|-y) YES=1 ;;
	-h|--help) sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) echo "setup.sh: unknown option $1 (--help)" >&2; exit 1 ;;
	esac
	shift
done

IMG_PATH=/data/local/hakios/ios.img
KERNEL=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
conf() { sed -n "s/^$1=//p" devices/raphael/device.conf; }
say() { printf '\n== %s\n' "$*"; }
ok() { printf '   [ok]      %s\n' "$*"; }
todo() { printf '   [missing] %s\n' "$*"; }
die() { printf '\nsetup.sh: %s\n' "$*" >&2; exit 1; }
ask() { # ask <question>: yes without asking under --yes
	[ $YES = 1 ] && return 0
	read -r -p "   $1 [y/N] " a; [ "$a" = y ] || [ "$a" = Y ]
}
# do <what> <command...>: run a step that changes things (not under --check)
do_step() {
	local what=$1; shift
	[ $CHECK = 1 ] && { todo "$what"; return 1; }
	say "$what"
	"$@" || die "$what failed"
}

# ---------------------------------------------------------------------------------------------
say "the PC"
missing=()
for t in adb fastboot python3 make clang 7z lzfse flex bison m4 gawk autoconf automake; do
	command -v "$t" >/dev/null || missing+=("$t")
done
# Ubuntu names its LLVM tools llvm-ar-18 and so on; the build wants the plain names
TOOLBIN=$PWD/devices/raphael/.bin
mkdir -p "$TOOLBIN"
for t in llvm-objcopy llvm-ar llvm-ranlib llvm-strip; do
	if ! command -v "$t" >/dev/null && [ ! -e "$TOOLBIN/$t" ]; then
		v=$(compgen -c "$t-" | grep -E "^$t-[0-9]+$" | sort -t- -k3 -n | tail -1)
		[ -n "$v" ] && ln -sf "$(command -v "$v")" "$TOOLBIN/$t" || missing+=("$t")
	fi
done
export PATH=$TOOLBIN:$PATH
command -v aarch64-linux-gnu-gcc >/dev/null || [ -x tools/android/hakios-img ] || missing+=(aarch64-linux-gnu-gcc)
if [ ${#missing[@]} -gt 0 ]; then
	todo "tools: ${missing[*]}"
	echo "   On Ubuntu: sudo apt install adb fastboot python3 make clang llvm p7zip-full lzfse flex bison m4 gawk autoconf automake gcc-aarch64-linux-gnu"
	echo "   (the kernel build also needs theos with the iPhoneOS16.5 SDK: see xnu-deps/setup.sh)"
	[ $CHECK = 1 ] || die "install the tools above, then run this again"
else
	ok "tools"
fi

# ---------------------------------------------------------------------------------------------
say "the phone"
STATE=$(./xl state 2>/dev/null)
echo "   state: $STATE"
phone_to_android() {
	[ "$(./xl state)" = android ] && return 0
	[ "$(./xl state)" = loader ] && ./xl reboot >/dev/null 2>&1
	for _ in $(seq 1 60); do
		if [ "$(./xl state)" = android ] && [ "$(adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ]; then
			sleep 5		# the loader is not reachable for a few seconds after Android says it is up
			return 0
		fi
		sleep 3
	done
	return 1
}
has_root() { adb shell 'su -c id' 2>/dev/null | grep -q 'uid=0'; }
if [ "$STATE" = android ]; then
	has_root && ok "adb and root" || { todo "root: adb shell su -c id does not answer uid=0 (grant it in KernelSU)"; [ $CHECK = 1 ] || die "the phone needs root"; }
elif [ "$STATE" = absent ]; then
	todo "no phone on USB (plug it in, USB debugging on)"
	[ $CHECK = 1 ] || die "no phone found"
fi

# ---------------------------------------------------------------------------------------------
say "the files this build is made of"
if [ -d sysimage/rootfs ] && [ -d sysimage/cryptex ] && [ -d "sysimage/20F66__iPhone10,3_6" ]; then
	ok "iOS 16.5 files (sysimage/)"
elif ask "download the iOS 16.5 files from Apple's servers (about 7.4 GB, 8 GB of disk while it runs)?"; then
	do_step "fetching the iOS 16.5 files" sysimage/fetch.sh
else
	todo "iOS 16.5 files (make fetch)"
fi

if [ -d fw-gpu ] && [ -d fw-audio ]; then
	ok "the phone's firmware (fw-*/)"
else
	todo "the phone's firmware (pulled from your own Android)"
	if [ $CHECK = 0 ]; then
		phone_to_android || die "the phone did not come up in Android"
		do_step "pulling the firmware off the phone" devices/raphael/pull-firmware.sh
	fi
fi

if [ -f "$KERNEL" ] && [ $REBUILD = 0 ]; then
	ok "kernel"
else
	# make ends with errors about symbol sets that are harmless: what counts is the kernel file
	build_kernel() { rm -f "$KERNEL"; make kernel DEVICE=raphael JOBS="$(nproc)"; [ -f "$KERNEL" ]; }
	do_step "building the kernel" build_kernel
fi

if [ -f xnuloader/loader.efi ] && [ $REBUILD = 0 ]; then
	ok "loader"
else
	do_step "building the loader" make loader DEVICE=raphael
fi

if [ -f userland/system.apfs ] && [ $REBUILD = 0 ]; then
	ok "system image (userland/system.apfs)"
else
	# exactly this recipe: a different one (PREBUILT=1, your own RC_JOBS) gives an iOS without wallpaper, sound or buttons
	do_step "building the system image (about 20 minutes)" env GL=1 SYSTEM=1 REST=1 userland/build.sh
fi

# ---------------------------------------------------------------------------------------------
if [ $PERSIST = 1 ]; then
	say "the image file in Android's /data (iOS keeps its data in it)"
	if [ $CHECK = 1 ]; then
		if [ "$STATE" = android ] && has_root; then
			adb shell "su -c 'test -f $IMG_PATH'" 2>/dev/null && ok "$IMG_PATH" || todo "$IMG_PATH (made on the first run, $SIZE_GIB GiB)"
		else
			echo "   (the phone is not in Android: not looked at)"
		fi
	else
		phone_to_android || die "the phone did not come up in Android"
		has_root || die "the phone needs root"
		if [ ! -x tools/android/hakios-img ]; then
			aarch64-linux-gnu-gcc -O2 -static -o tools/android/hakios-img tools/android/hakios-img.c || die "could not build tools/android/hakios-img"
		fi
		adb push tools/android/hakios-img /data/local/tmp/hakios-img >/dev/null || die "adb push failed"
		adb shell 'su -c "chmod 755 /data/local/tmp/hakios-img"'
		if adb shell "su -c 'test -f $IMG_PATH'"; then
			ok "$IMG_PATH is there"
		else
			free=$(adb shell "su -c 'df -k /data'" | awk 'END { print int($4 / 1048576) }')
			echo "   /data has $free GiB free; the image file takes $SIZE_GIB GiB for good (until you delete it)"
			[ "$free" -gt $((SIZE_GIB + 8)) ] || die "not enough free space on /data for $SIZE_GIB GiB"
			ask "make $IMG_PATH ($SIZE_GIB GiB) on the phone?" || die "no image file: run with --no-persist to boot from RAM only"
			adb shell "su -c '/data/local/tmp/hakios-img create $IMG_PATH $((SIZE_GIB * 1073741824)) >/dev/null'" || die "could not make the image file"
			inline=$(adb shell "su -c 'dump.f2fs -i \$(stat -c %i $IMG_PATH) /dev/block/dm-45 2>&1 | grep i_inline'" | sed -n 's/.*: *\([0-9][0-9]*\)\].*/\1/p')
			[ -n "$inline" ] && [ $((inline & 64)) -ne 0 ] || echo "   (could not confirm that f2fs pinned the file; do not use it until dump.f2fs -i shows i_inline with bit 0x40)"
			ok "made $IMG_PATH"
		fi
		./xl imgscan "$IMG_PATH" logs/ios.ext || die "could not read where the image file's blocks are"
	fi
fi

[ $CHECK = 1 ] && { say "check done"; exit 0; }

# ---------------------------------------------------------------------------------------------
say "booting iOS"
SECS=${SECS:-$(conf PHONE_SECS)}
ARGS="$(conf BASE_ARGS) $(conf PHONE_ARGS)"
if [ $PERSIST = 1 ]; then
	ARGS="$ARGS ufs_write=1 var_store=1 var_store_dev=hakios"
fi
echo "   $SECS s, arguments: $ARGS"
entered=0
for try in 1 2 3; do ./xl enter && { entered=1; break; }; sleep 5; done
[ $entered = 1 ] || die "the loader did not come up"
loaded=0
if [ $PERSIST = 1 ]; then
	./xl img hakios logs/ios.ext | grep -q OKAY || die "the loader did not take the image file"
	# the system image kept in the image file: loaded from there (a few seconds) when it is the one built here
	want=$(python3 -c 'import sys, zlib
c = 0
with open(sys.argv[1], "rb") as f:
    while True:
        b = f.read(1 << 24)
        if not b: break
        c = zlib.crc32(b, c)
print(c)' userland/system.apfs)
	size=$(stat -c %s userland/system.apfs)
	have=$(./xl rdinfo | tail -1)
	have_size=$(echo "$have" | awk '$2 == "bytes" { print $3 }')
	have_crc=$(echo "$have" | awk '$4 == "crc" { print $5 }')
	if [ "$have_size" = "$size" ] && [ -n "$have_crc" ] && [ $((16#$have_crc)) -eq "$want" ]; then
		echo "   loading the system image from the image file"
		./xl rdload | grep -q 'loaded' && loaded=1 || echo "   (loading failed: uploading instead)"
	else
		echo "   the image file holds no system image, or an older one: it is uploaded now and kept there"
	fi
fi
if [ $loaded = 0 ]; then
	echo "   uploading the system image (about 90 s; the screen shows the Silicium logo until iOS starts)"
	./xl ramdisk userland/system.apfs || die "the upload failed"
	if [ $PERSIST = 1 ]; then
		./xl rdsave | grep -q 'saved' && echo "   kept in the image file: the next boot loads it from there" || echo "   (could not keep it in the image file; the next boot uploads again)"
	fi
fi
XL_CMDLINE="$ARGS" XL_SECS="$SECS" ./xl run
echo
echo "The phone is in the loader with the system image in its RAM. Again: ./xl run (with XL_CMDLINE and XL_SECS as above, or"
echo "run this script again). Back to Android: ./xl reboot. The log of the run is in logs/."
