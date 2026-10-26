#!/bin/bash
# Make ../Mu-Silicium: a clone of Project-Silicium/Mu-Silicium at the commit BASE, with the raphael
# port put into it. raphaelPkg is generated from the repository's own cepheusPkg (Mi 9, the same SM8150,
# the same FD at 0x9FC00000) with the names changed; the XNU loader hook is the POCO port's (same BASE,
# nothing in it is phone-specific). See README.md here.
set -e
cd "$(dirname "$(readlink -f "$0")")"
MU=../../../Mu-Silicium
POCO=../../poco-x5-pro/uefi
[ "$(cat BASE)" = "$(cat $POCO/BASE)" ] || { echo "uefi/apply.sh: BASE differs from the POCO port's; the hook patch is made on the other one"; exit 1; }
[ -e $MU ] && { echo "uefi/apply.sh: $MU already exists"; exit 1; }
git clone https://github.com/Project-Silicium/Mu-Silicium.git $MU
git -C $MU checkout -q "$(cat BASE)"
git -C $MU submodule update --init --recursive
# the hook: BootShim payload copy + BootManagerLib starting the loader, and the loader's own files
git -C $MU apply --whitespace=nowarn "$PWD/$POCO/mu-silicium.patch"
cp -r $POCO/files/Silicon/. $MU/Silicon/
# raphaelPkg from cepheusPkg: the package's names change; the stock drivers (Binaries/cepheus) and the
# ACPI tables (cepheus/AcpiTables.inf) are the Mi 9's, shared by the same SoC, and keep their names
P=$MU/Platforms/Xiaomi
rm -rf $P/raphaelPkg && cp -r $P/cepheusPkg $P/raphaelPkg
cd $P/raphaelPkg
for f in cepheus.dec cepheus.dsc cepheus.fdf; do mv $f ${f/cepheus/raphael}; done
sed -i -e 's/cepheusPkg/raphaelPkg/g' -e 's/Binaries\/cepheus/Binaries\/CEPHEUS_KEEP/g' -e 's/cepheus\/AcpiTables/CEPHEUS_ACPI_KEEP/g' \
	-e 's/cepheus/raphael/g' -e 's/CEPHEUS_KEEP/cepheus/g' -e 's/CEPHEUS_ACPI_KEEP/cepheus\/AcpiTables/g' \
	raphael.dsc raphael.fdf DeviceBuild.py Include/*.inc Library/*/*.inf
sed -i -e 's/^\(  PLATFORM_GUID *= \).*/\14E0C8A3B-5D1F-4B6A-9C72-AF1D3E6B8C05/' \
	-e 's/"Mi 9"/"Redmi K20 Pro"/' -e 's/"Mi_9_raphael"/"Redmi_K20_Pro_raphael"/' raphael.dsc
cd - >/dev/null
# the files of the port itself: the configuration (FD base and size) and the phone's device tree
cp -r files/. $MU/
cp ../prebuilt/device-tree.dtb $MU/Resources/DTBs/raphael.dtb
echo "uefi/apply.sh: done; next: uefi/README.md"
