# Device: Redmi K20 Pro (raphael)

**Status: iOS 16.5 boots from RAM to the home screen on this phone, with touch and battery readout.** Nothing is
flashed; the phone goes back to Android after a restart. The first phone, `devices/poco-x5-pro/`, is the
worked example. The sections below are the notes in the order they were found.

| What | State |
|---|---|
| UEFI (Mu-Silicium, `uefi/`) | works: USB and display |
| XNU loader (`platform_raphael`) | works, also the UFS and charger polls |
| Kernel | boots to user space; the SM8150 virtual timer is INTID 19 (`vtimer-intid` in the device tree) |
| iOS 16.5 | home screen, software drawing |
| Touch | works (`touchscreen=2`, Goodix GT9886 on I2C) |
| Buttons | power, volume up and down read and delivered (on with `touchscreen=`, or `buttons=1`); polled over SPMI, no interrupts |
| Vibration | works (`haptic=5`): the PM8150B's haptics block, an ERM motor, driven by iOS's actuator waveforms as taps and vibrations |
| Sound (`adsp=1 sound=4`) | works: iOS's sounds play through the bottom speaker (the audio DSP, MI2S, a TFA9874 amplifier); earpiece, microphones, headphones not done |
| Battery | SOC, voltage and charging state read (`battery=1`); temperature and current words not confirmed |
| GPU (Adreno 640) | steps 1 to 16 pass; step 17 (the GMU's clock request) does not yet, see the end |
| Wi-Fi, cellular, sound, haptics, cameras | not started (the Wi-Fi chip lives inside the modem) |

## Quick start

On Linux, with the phone on adb (KernelSU `su`) and the bootloader unlocked:

```sh
devices/raphael/pull-soc.sh                     # device tree and soc.args (kept in the repository already)
# system image: userland/build.sh with the IPSW, then `lzfse` (apt) is needed for the loader's identity
export DEVICE=raphael
export PATH=<dir with version-less llvm-objcopy, llvm-ar, llvm-strip links>:$PATH   # Ubuntu has only the -18 names
make phone DEVICE=raphael                       # enters the loader, uploads the image, runs 180 s, returns to the loader
tools/logdump.sh                                # after rebooting to Android: the loader's log from the `logdump` partition
```

## Traps

- No `-v`: the screen shows the Silicium logo for the whole run; that is normal. Do not hold Power.
- Keep runs short (`PHONE_SECS=180`): there is no thermal control yet.
- A watched run needs `hv_wdt_pet=1 bootcpu=7 cpus=8` (`run.conf` has them); with `cpus=1` the loader sees no heartbeat.
- The system image is lost when the phone restarts: upload it again (`make phone` does).
- An interrupted `xl run` leaves a process that blocks `xl`: `pgrep -f '^/bin/bash \./xl '`, kill it (not `pkill -f "xl run"`, that kills your own shell).
- `xl` failing with "loader did not come up on USB" on Linux: `llvm-objcopy` missing from `PATH` (BootShim build), or the macOS shims in `tools/macos-bin` shadowing `stat`/`timeout` (`modem-cycle.sh` now adds them only on a Mac).
- Use the addresses of the phone's own device tree, not Android source's: wrong GPU/PDC addresses stalled the whole system.
- Kernel build: needs theos with the iPhoneOS16.5 SDK, `gawk` (mawk breaks `makesyscalls.sh`), flex, bison, m4, autoconf, automake; the `config/` symbolset errors are harmless.

## Getting the data

The stock Android does not let the adb shell read the device tree or `/vendor/firmware*`, so both
scripts go through `su` (KernelSU worked here):

```sh
devices/raphael/pull-soc.sh        # -> prebuilt/device-tree.dtb, soc.args
devices/raphael/pull-firmware.sh   # -> fw-gpu fw-wifi fw-audio fw-modem fw-camera
```

## soc.args against the first phone

Generated: `soc_ipa_base`, `soc_gsi_base`, `soc_aoss`, `soc_rsc`, `soc_apps_smmu`, `soc_smem`,
`soc_imem`, `soc_mpss_mem`, `modem_ipa_fw_at`, `soc_ipa_stream(_uc)`, `soc_pas_mpss`, `soc_pas_ipa`,
`soc_ipa_fw` (`ipa_fws`), and `soc_cmd_db` (0x85f20000: the command DB is not a reserved-memory node
here; the kernel registers it from the AOP message RAM, so the address is read from `/proc/iomem`).

Not generated, because the SM8150 does not have it: **`soc_ipcc`**. The IPCC block arrived with later
SoCs; the SM8150 signals the modem through APCS mailboxes (`qcom,sm8150-apcs-hmss-global`), SMP2P and
GLINK. The kernel's modem and IPA code assumes IPCC and the IPA 4.11 layout; the SM8150's IPA is an
older version. Cellular is therefore a driver port, not an argument change.

## What differs, driver by driver

| Area | POCO X5 Pro | Redmi K20 Pro | Effort |
|---|---|---|---|
| GPU | Adreno 642L | Adreno 640; firmware `a640_gmu.bin`, `a630_sqe.fw`, `a640_zap.*` | new GMU/RPMh bring-up for a6xx gen1 |
| Touch | Goodix | Goodix GT9886 (`goodix_gt9886_*` in `/vendor/firmware`) | probably close to `hakios.goodix.c`; check bus and addresses |
| Display | its own DSI panel | SM8150 `mdss` (DPU 4.0), panel from the device tree | new |
| Storage | UFS 3.1 | UFS 3.0 on SM8150 | check `hakios.ufshc.c` |
| Wi-Fi | WCN6750 (`ath11k`, wpss) | WCN3998 (`wlanmdsp.mbn`, `bdwlan.*`, no wpss) | different chip and driver |
| Audio / battery | adsp, 36 parts | adsp, 14 parts (`adsp.b00-b13`) | firmware path and PAS differ |
| Modem / IPA | IPA 4.11, IPCC | older IPA, no IPCC | larger port |
| Haptics | Awinic, firmware file | not yet identified | |
| Cameras | `hakios_*` modules | `raphael_*` modules (imx586, s5k3l6, s5k3t2, ov8856) | new sensor tables |
| UEFI | Mu-Silicium, `KodiakPkg` | needs the SM8150 package and the phone's `xbl` | new |

## xbl / UEFI findings (`fw-xbl/`, pulled with `dd` through `su`, not in git)

- `xbl.img` is an ELF aarch64 image with 16 segments. Segment 11 (PA `0x9fc00000`, 3 MB, RWX) is where
  the stock UEFI's firmware volume lives; in the file it is an empty (all 0xff) `_FVH` volume. So the
  DXE drivers the POCO port copies out of its `xbl.img` are **not** in this one: `xbl.img` has no
  compressed volume after the ELF (the tail is zero padding), `abl.img` and `imagefv.img` hold small
  volumes with one file each. Where the SM8150's UEFI drivers come from is still open (UEFIReader
  was not run; running downloaded tools was not authorised here).
- `run.conf`: `FD_BASE=0x9FC00000`, `FD_SIZE=0x300000` from that segment, `PAYLOAD_BASE=0xF0000000`
  in free RAM (`/proc/iomem`: System RAM `0xc0000000-0x1ffffffff`, 6 GB phone). Derived, not booted.

## A UEFI that is known to boot this phone

The `uefi-images-raphael.zip` of `rexnox/windows_xiaomi_platforms` (release `0e7aa8b`, built from
`SurfaceDuo1Pkg` of edk2-msm, for Windows on ARM) is an Android boot image with the same layout as the
POCO's: `gzip(shim + FD) + DTB`, `ANDROID!` header version 0, kernel at `0x8000`. Its shim is 112 bytes
and loads a **3 MB FD at 0x9fc00000**, which is what `run.conf` says. Its FD holds three compressed
volumes, so it does not need the stock `xbl` drivers. (Not in the repository: it is somebody else's
build. The release notes of an older raphael image say "broken".)

Which tree to port: **Mu-Silicium, from `cepheusPkg`** (Mi 9, also SM8150), the way `lisa` was the
template for `redwood`. Checked: its `Resources/Configs/cepheus.toml` has `[uefi_fd] base = 0x9FC00000,
size = 0x300000`, exactly raphael's segment and the working image's shim; it has no `Binaries/<device>`
(its stock drivers are `Binaries/cepheus`, shipped by Mu-Silicium's `Device-Binaries` submodule, so nothing has to come from this phone's `xbl`); its
framebuffer is 1080x2340 like raphael's; and our hook (`XnuLoader` in `BootManagerLib`, the shim patch)
is already written against Mu-Silicium. edk2-msm has no raphael (its SM8150 Xiaomi devices are cepheus,
nabu, vayu, with `FD_BASE=0xCE000000`), and the working image above comes from a Project Mu
`SurfaceDuo1Pkg` fork whose source I did not find (`WOA-Project/mu_andromeda_platforms` has no raphael).
`uefi/apply.sh` makes `raphaelPkg` that way. Still to do for it: its `MemoryMapLib` (from the live device tree's /memory and
reserved-memory), its SMBIOS names, `Resources/DTBs/raphael.dtb`, and a `Resources/Configs/raphael.toml`.

## Kernel on raphael: the timer interrupt

The kernel boots here as far as `hello` as process 1 (`XL_RAMDISK=prebuilt/userland/hello`, boot-args
`-v debug=0x8 rd=md0 cpus=8`; no `ufs=`, `cpufreq=` or other POCO drivers). One thing had to change: Qualcomm's
SM8150 wires its timers as GIC INTIDs 17 (secure physical), 18 (physical) and 19 (**virtual**), not the
architecture's 26-30, so the kernel's "virtual timer on PPI 27" never got a tick: every timeout (thread calls,
`loader_return_after`, sleeps) waited forever and the boot stalled after the network init. Found by reading
this UEFI's own timer numbers (`HanaPkg.dsc.inc`) against the redistributor's pending bits (19 and 28 set
while the virtual timer's condition was met, 27 never).

- Source: `gic_v3_irq.c` reads `vtimer-intid` from `/arm-io/gic` (default 27, so the POCO is unchanged); the
  loader's `platform_raphael` writes 19 there.
- Until the prebuilt kernel is rebuilt from that source (this needs theos and the iOS 16.5 SDK, `xnu-deps/setup.sh`),
  `devices/raphael/tools/patch-kernel.py` makes `mach.raphael` from `prebuilt/mach.development.bcm2837` with the
  five instructions that carry the 27 changed; `xl` uses `mach.$DEVICE` when it exists. Boot-test of that
  patched kernel: returned to the loader after the 20 s of `loader_return_after`, log with `hello`'s output.
- Not checked yet: that the kernel built from the changed source reads the property (the loader side and the
  patched binary were run, the rebuilt kernel was not).
- Also learned: the loader cannot read the kernel's log when it hangs (no log partition on this phone, 0 bytes);
  the screen (`-v`) is the only view of a hang. `ICC_HPPIR*_EL1` reads make this firmware reset the phone.

## iOS 16.5 on raphael (2026-10-26)

With the pieces above, the stock iOS 16.5 user space boots to the home screen (SpringBoard, the stock apps'
icons, the dock), drawn in software, with touch (boot-arg `touchscreen=2`, `bsd/dev/hakios.goodix_i2c.c`; the
POCO's `touchscreen=1` driver is SPI and RPMh and does not apply here): no GPU driver, no sound, no cellular or Wi-Fi. What it took, from the top of the repository with `DEVICE=raphael`:

```sh
sysimage/fetch.sh                                   # the IPSW parts from Apple (needs 7-Zip 23+)
PREBUILT=1 REST=1 userland/build.sh                 # the images, with the prebuilt programs (no iOS SDK here)
xnuloader/mkidentity.sh && xnuloader/build.sh       # the loader with the real identity (needs the `lzfse` tool)
devices/raphael/tools/patch-kernel.py               # mach.raphael: the SM8150's timer interrupt (see above)
./xl enter && ./xl ramdisk userland/system.apfs     # the system image into the phone's RAM (~90 s), once per loader session
XL_CMDLINE="debug=0x8 rd=md0 hv_wdt_pet=1 cpus=8 ufs=2 touchscreen=2" XL_SECS=180 ./xl run
```

Leave `-v` out of the arguments for a clean screen (it makes the kernel's console draw over the UI), and
leave out the POCO's `gpu=`, `touchscreen=`, `wifi=`, `modem=`, `haptic=`, `adsp=`, `sound=` and `cpufreq=`.
The system image is lost whenever the phone restarts into Android: upload it again before the next run.

Two build-environment traps that cost a run each: the images take the files' permission bits, and launchd
refuses a plist that is group-writable (`error = 122: Path had bad ownership/permissions`), so the build now
sets `umask 022` (a Linux user's default umask is 002); and `mkidentity.sh` needs an LZFSE decoder, which
Ubuntu has as the `lzfse` package.

## Touch screen: proven from the loader (2026-10-26)

`xl touch [SECONDS]` (the loader's `oem touch`, `touch_probe()` in `xnuloader/loader.c`) powers the chip, resets it,
talks to it and prints what it reports while the screen is touched. On the Redmi K20 Pro it works first time
after one fix (the slave bus clock has hardware gating and reads "off" while idle, so only the engine's clock
and the master bus clock are waited for): 4054 reports in 40 s, none with a bad checksum, finger paths as
coordinates in the panel's own 1080 x 2340. Everything the kernel driver needs is in that function:

| What | Value |
|---|---|
| Chip | Goodix GT9886 ("normandy"), I2C address `0x5d`, 16-bit big-endian register addresses |
| Bus | QUPv3 wrapper 2, engine at `0xC88000` (GENI, protocol 3 = I2C, FIFO mode, polled), 100 kHz from the 19.2 MHz XO (divider 7, SCL counters 10/11/26) |
| Clocks (GCC at `0x100000`) | engine source RCG `0x1e3a8` (cfg 0: XO, no divider, then update), votes in `0x52014`: bit 6 engine, bit 2 master AHB, bit 1 slave AHB |
| Pins (TLMM south tile `0x3D00000 + 0x1000*n`, input/output at +4) | SDA 57 and SCL 58 function 2 (`qup19`); reset 54 (output, pulled up, 16 mA); interrupt 122 (input, pulled up, active low); 3.3 V enable 98 and 1.8 V enable 100 (outputs) |
| Power-up | 98 high, 10 ms, 100 high, 300 ms, reset low 2 ms then high, 350 ms |
| Report | 12 bytes at register `0x4100`: byte 0 bit 7 = touch event, byte 1 low nibble = contacts, then per contact `id, x lo, x hi, y lo, y hi, w, ...` (8 bytes); with more than one contact read `(n-1)*8` more at `0x410C`; the sum of the first `4 + 8n` bytes is 0 mod 256; write 0 to `0x4100` to acknowledge |
| Product id | register `0x4535`: "9886"; version block at `0x452c`: "NOR_G1" |

The config is already in the chip (nothing is downloaded); the chip's register map comes from
`goodix_gt9886_cfg_f11.bin`, which the Android kernel builds in (`firmware/goodix_gt9886_cfg_f11.bin.ihex`).
Next: the same sequence as a kernel driver (`bsd/dev/`, feeding `/dev/hakios-touch`), which needs the kernel rebuilt
(theos and the iOS 16.5 SDK, `xnu-deps/setup.sh`).

## Building the kernel here (2026-10-26)

`make kernel` works on Linux with theos (`~/theos`, with its iOS toolchain and `sdks/iPhoneOS16.5.sdk`) and these
packages: `flex bison m4 autoconf automake gawk` (not mawk: Ubuntu's default awk keeps the backslash in Apple's
`makesyscalls.sh`, and the generated `init_sysent.c` does not parse) and LLVM's `dsymutil` (`xnu-build.sh` finds it). It
ends with errors in `config/` (the kext symbol sets, `exported name not in import list`): the build runs with `-k`
(keep going) and the kernel is made regardless, as `BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837`
(`xl` takes it from there by itself, but a `mach.raphael` beside it wins: delete that, or use `XL_KERNEL=`). It has no
Wi-Fi: the POCO's `ath11k` is a separate library (`tools/mk-ath11k.sh`) that this phone does not need. With it the
timer interrupt comes from the device tree, so `patch-kernel.py` is no longer needed.

## Battery (2026-10-26)

The POCO's battery path works here as it is, once the loader looks: `charger_start()` in the loader was restricted to the
POCO (`PLAT != &platform_redwood`) and now also takes the Redmi K20 Pro. The loader finds Qualcomm's own UEFI charger
drivers by protocol (`QcomChargerDxe`, `ChargerExDxe`, which `Binaries/cepheus` has for this SoC), polls them on core 0 and
publishes a read-only cache; the kernel's `battery=1` reads it and iOS shows the level, the bolt and the lock-screen ring.
Charger limits stay with the firmware. Checked: `battery: SOC=99% V=4422 mV, battery=1 charger=1 charging=1 (13 reads, 0 failed)`
while Android, a moment later, said 100%, 4.35 V, 36.5 C, charging. State of charge, charging and the voltage agree
(the gauge reads 99 where Android rounds to 100); the temperature (25 C against 36.5 C) and the current (-104 mA against
+385 mA, different loads and perhaps a different sign convention) do not, so the loader's layout for those two on the SM8150
is unconfirmed (the offsets were confirmed on the POCO only). Run with `battery=1` among the arguments.

## GPU (Adreno 640): steps 1-16 proven (2026-10-27)

`bsd/dev/hakios.adreno.c` is now model-aware: a table (`gpu_models[]`) of the blocks and firmware names that differ, chosen by
the loader's `/arm-io` `device_type` (`redwood` or `raphael`), so the POCO's driver and the iokit side (`hakios_gpu_*`) stay
one piece. Run the bring-up by steps with the small image: `XL_RAMDISK=prebuilt/userland/hello XL_CMDLINE="debug=0x8
rd=md0 adreno=N adreno_pause=1500 adreno_watch=15 adreno_return=1 cpus=1" ./xl run` (N = 16 so far: each step names what it
is about to touch, reads come before writes, the kernel log comes back with the run). Found on this phone:

| What | Value |
|---|---|
| Blocks | GPU `0x2C00000`, GPU clock controller `0x2C90000`, GMU `0x2C6A000` (`0x30000`), GMU RSC at GMU `+0x23000` (`0x2C8D000`), SMMU `0x2CA0000` (5 contexts), PDC `0xB280000`; no separate CX-misc block |
| Command database | `0x85F20000` (RPMh: `gfx.lvl`, `cx.lvl`, `mx.lvl` found, with their levels) |
| GCC | the GPU's GPLL0 gates are bits 15 and 16 of the vote register `0x52004` (the POCO's is `0x52000`); `gpu_cfg_ahb 0x71004`, `memnoc_gfx 0x7100c`, `snoc_dvm_gfx 0x71018`, `ddrss_gpu_axi 0x71154` |
| GPU clock controller | no hub or `cb` clocks (nothing at `0x1170`, `0x1178`, `0x117c`, `0x1204`); GMU clock source `0x1120` takes `0x602` (GPLL0/1.5, 200 MHz) as on the POCO |
| Firmware | `a640_gmu.bin`, `a630_sqe.fw`, `a640_zap.mdt` + `a640_zap.b02` (from `fw-gpu/`) |

Step 4 powers the CX switch (`cx_gdscr` becomes `0xf8222000`); step 6 reads the GMU's always-on counter at 19.199 MHz, so the
GPU block is alive and the hypervisor lets the kernel read the GMU and the bus interface. Not done: the GPU's fuses (the POCO's cells
are meaningless here), the clock-rate measurement (its debug-mux numbers are the POCO's), and steps 10 to 22 (GMU firmware, HFI,
the 3D core, the zap shader, a frame). The GMU firmware's message protocol (HFI) is the older generation's: expect differences there.

Steps 10 to 16 (2026-10-27): the GMU's memory takes writes; TrustZone accepts the zap shader's signature from this kernel
(image 13); the SMMU context for the GMU is programmed (streams as on the POCO: 0 and 1 the 3D core, 2 secure, 4 and 5 the GMU,
mask `0x400`); the GMU firmware `a640_gmu.bin` (core `0x20000105`, power `0x20000012`, HFI `0x20000001`) loads and **runs**
(`cm3_fw_init_result = 0xbabeface`); and its message queues come up. What differs from the POCO's (a660-class) driver, all in the
`gpu_models[]` table and `legacy_gmu` branches of `bsd/dev/hakios.adreno.c`: the older GMU has a fixed memory map, not one the
firmware asks for (instruction window at `0x4000`, data at `0x44000`, state at `0x60000000`; its data memory is at GMU address
`0x40000`); it signals success with `0xbabeface`; the chip id it is given is `0x06040100`; and its RSC and PDC programs
(`legacy_rpmh_ucode()`) are the vendor's for this generation and are loaded **before** the GMU starts.

Two lessons that cost runs: use this phone's **own** device tree for addresses, not the Android source's (the GMU's PDC is at
`0xB290000` and `0xB490000` here, the source says `0xB280000` and `0xB480000`: the wrong ones stall the system at the first
access); and a watched run needs `hv_wdt_pet=1 bootcpu=7 cpus=8` among the arguments (the loader's hang detector reads the kernel's
heartbeat, which needs another core to run), without which a healthy run is declared hung after 50 s. Without `bootcpu` nothing
watches and a hang saves no log. Next: step 17 (the 3D core's power through the GMU's messages: the older firmware's protocol
is HFI version 1), then the 3D core's microcode and zap shader, then a frame.

### Step 17, where it stands (2026-10-27)

The GMU takes the three configuration messages of its protocol (the speed table, the bus table, "start the core firmware": the
table layouts are the POCO's, and the speeds are the Adreno 640's 215 to 600 MHz at RPMh rail levels 64 to 384). Asked to power the
3D core (the "OOB" request, bits 30 and 31 as on the POCO), the GMU **brings the GX power switch on** (`gx_gdscr` becomes
`0x8222000`, bit 6 of `sptprac_pwr_clk_status` clears: "GX power domain off") but **the 3D core's clock does not start** (bit 7,
"GX clock off", stays set) and the GMU never grants the request, within 10 s (its own log suggests it keeps working for about
1.5 s). Tried: the vendor's order of the messages (the speed vote only after the core is powered and started), the GPU rail's
RPMh address from the command database (`0x30050`, not the vendor source's `0x30090`), a longer wait. Not yet looked at: what the
GMU needs before its first power request (the vendor sets up the 3D core's SMMU context and the GBIF halt first), the PLL0 of the
GPU clock controller (the Android driver only configures PLL1: the firmware must do the 3D core's), the bus table's votes, and a
decode of the GMU's event log. The step is a faithful port of the vendor's flow up to this point; what stops it is internal to
the GMU firmware's power-up sequence.

## Buttons

`hakios.buttons.c` is the POCO's driver with this phone's peripherals (chosen from the device tree's model). The PMIC
arbiter is the same (version 5, same addresses). Power and volume down are KPDPWR_N (bit 0) and RESIN_N (bit 1) of the
PM8150's power-on block (slave 0, 0x800, type 1 subtype 4, "qcom,qpnp-power-on"); volume up is GPIO6 of the PM8150
(0xc500, input with pull-up, left by the firmware). Read-only, as on the POCO. Checked by hand: every press and release
was logged with a sensible hold time. Confirmed by hand in iOS: the buttons work.

## Vibration

`hakios.haptic_pmic.c`. The motor is not an I2C chip as on the POCO but the PM8150B's haptics block (SPMI slave 3,
0xc000; type 0x15, subtype 0x09; an ERM, 3000 mV max in Android's tree), written through the arbiter's write channel
(ours) the way the flashlight is on the POCO: a list of registers, every write read back, any surprise ends all
writing. The constant-voltage source is used (a tap is a 40 ms drive, a vibration a long one, 1.5 s at most). The
current-limit register (0x52) does not take Android's 0x02 and is left as the firmware has it. `haptic=1` looks,
`haptic=2` buzzes once (`haptic_ms`), `haptic=5` is iOS's. Felt on the phone, and iOS's feedbacks work. One hang was
seen once in a haptic=5 run, about 20 s into the userland start, and did not come back in the next run; not tied to the driver.

## Sound

**Status: iOS's sounds play through the bottom speaker** (`adsp=1 sound=4 sound_below=96`, in `PHONE_ARGS`; checked by ear with the
lock-screen and message sounds; the amplifier stayed at 42 to 46 C with no alarm). Not done: the earpiece and the other speaker paths
(WSA881x behind the WCD9340 over SoundWire/SLIMbus), the microphones, headphones, a measured gain scale.

`hakios.adsp.c` takes a model table. Steps 1 to 11 pass (`adsp_step=11`; firmware: `fw-audio/` from `pull-firmware.sh`, put into
the small image with `ADSP=1 REST=1 PREBUILT=1 userland/build.sh`). What differs from the POCO:

- shared memory 0x86000000 (not 0x80900000), command database 0x85f20000, the DSP's memory 0x8be00000, 34 MB (the image fits
  exactly); the image has a segment of 0x54 bytes (a 4-byte tail is written)
- no IPCC: the doorbells are bits of one register of the APCS mailbox (0x17c0000c): the always-on processor bit 0, GLINK to the
  DSP bit 8, SMP2P bit 10. Nothing waits for the interrupts (SPIs 156, 158): the shared memory is polled
- the DSP makes GLINK's descriptor (item 478) and its own queue (480) and leaves ours (479) to us: step 8 makes it, 16 KB
- result: the DSP says it is up 13 ms after TrustZone starts it, eight QRTR services, the audio services' versions over APR

The speaker path (`hakios.sound.c`, model-aware):

- the DSP's quaternary MI2S interface, AFE port 0x1006, bit clock 0x106 at 1.536 MHz (16 bits x 2 x 48 kHz), the DSP the master,
  one data line (SD0) stereo; the pins GPIO 137 (bit clock), 138 (frame sync), 139, 140, function 1 (`qua_mi2s`), in the TLMM's
  south tile at 0x3d00000
- the audio memory goes through the apps SMMU as stream 0x1b21 (the POCO's is 0x1801), same SMMU base, same low-nibble rule
- the amplifier is an NXP TFA9874 (`hakios.speaker_tfa.c`, behind the speaker interface `hakios_speaker_*`): I2C address 0x34 on QUP
  wrapper 0's engine 1 (0x884000; SDA/SCL GPIO 114/115 function 1; the clocks are GCC 0x17278, vote 0x5200c bits 11, 6, 7), reset
  GPIO 59 (high then low: the chip runs with it low), 8-bit register addresses and 16-bit big-endian values. Identity 0x0c74
  ("V1.14"). The configuration is Android's: the rev-specific writes of `tfa9874_specific()`, the container's (`tfa98xx.cnt`,
  510 bytes) three registers and 33 bit fields, the first items of its "speaker" profile, and 16-bit slots; the chip is powered
  up only while the MI2S clock runs (it locks to it: "clocks stable" is bit 7 of register 0x11), AMPE set, power-down and AMPE
  cleared when iOS is silent for three seconds
- faults: the over-current bit, the alarm-state bit, a lost clock (register 0x10), and the chip's own temperature (register 0x16)
  over 85 C. Bits 0, 1, 2 and 4 of register 0x10 read 0x0016 at idle and while Android plays: they are not faults
- the level: `sound_below` eighths of a dB under Android's; the amplifier's total gain field (TDMSPKG, Android's profile has 4) is
  set from it by a provisional map (20 dB or more under: 0, 12: 1, 6: 2, 2: 3, 0: 4). The step of that field is not measured yet

How it was found: Android's own I2C traffic to the amplifier recorded with ftrace while it played a key tone (register
addresses and the on/off sequence), the container decoded with the structures of its `tfa_container.h`, and the driver's
start-up order. Reading the amplifier's registers from Android's sysfs while it was powered down hung that phone: do not.
Test runs that you interact with need `PHONE_SECS` long enough (the run ends, and the phone leaves iOS, when it expires).
