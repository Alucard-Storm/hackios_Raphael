# UEFI for raphael

A port of [Project Silicium / Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium) to the
Redmi K20 Pro. **Built and booted** (2026-10-26): it shows the Project Silicium logo, and the XNU loader
started from it comes up on USB with the panel working (needs `llvm-objcopy` on `PATH`: Ubuntu has only
`llvm-objcopy-18`, so make version-less symlinks). It is modelled on `cepheus` (Mi 9), the same SM8150 with the
same UEFI FD (`0x9FC00000`, 3 MB, which is also what raphael's own `xbl.img` and the community's
working raphael UEFI use; see `../README.md`).

What is here:

- `BASE`: the Mu-Silicium commit (the same as the POCO port's; `apply.sh` refuses if they differ);
- `apply.sh`: clones Mu-Silicium at `BASE`, applies the POCO port's hook patch (`BootShim.S` payload copy,
  `BootManagerLib` starting the XNU loader: nothing in it is phone-specific) and the loader's files, makes
  `Platforms/Xiaomi/raphaelPkg` as a copy of `cepheusPkg` with the names changed (SMBIOS "Redmi K20 Pro", a
  new platform GUID), and puts in `files/` and the phone's device tree as `Resources/DTBs/raphael.dtb`;
- `files/Resources/Configs/raphael.toml`: FD base `0x9FC00000`, size `0x300000`.

`raphaelPkg` keeps pointing at `Binaries/cepheus` (the stock SM8150 UEFI drivers that Mu-Silicium's
`Device-Binaries` submodule already ships for the Mi 9) and at `cepheus/AcpiTables.inf`. Nothing is
extracted from this phone's `xbl.img`: its UEFI volume is empty (`../README.md`).

## Open points before a first boot

- `MemoryMapLib` is cepheus's, unchanged. Compare it with this phone's reserved-memory nodes and
  `/proc/iomem` (`soc.args` has the key ones): raphael's System RAM starts at `0x9a500000` where
  cepheus reserves `0x8b700000-0x9ab00000` as one PIL region, so cepheus's map is the more
  conservative of the two. If a boot hangs early, start here.
- Panel: this phone's kernel command line says `dsi_ss_fhd_ea8076_cmd_display` (Samsung EA8076, FHD
  command mode); `Binaries/cepheus/RawFiles` has `Panel_ss_ea8076_f11_fhd_cmd.xml` and the
  `Panel_samsung_ea8076_*` set, which `raphaelPkg/Include/RAW.inc` already lists. Not checked that the
  right one is chosen.
- `PcdStorageIsUfs3|FALSE` (as cepheus): this phone's UFS links at HS-Gear3 x2.
- Build as in `../../poco-x5-pro/uefi/README.md`, with `-d raphael`; `device.conf` one level up has the
  memory map `mkxnuimg.py` uses.
