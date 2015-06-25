#!/bin/bash
export KERNELDIR=`readlink -f .`
export RAMFS_SOURCE=`readlink -f $KERNELDIR/ramdisk`
export USE_SEC_FIPS_MODE=true

echo "kerneldir = $KERNELDIR"
echo "ramfs_source = $RAMFS_SOURCE"

RAMFS_TMP="/tmp/arter97-e330-ramdisk"

echo "ramfs_tmp = $RAMFS_TMP"
cd $KERNELDIR

if [ "${1}" = "skip" ] ; then
	echo "Skipping Compilation"
else
	echo "Compiling kernel"
	cp defconfig .config
scripts/configcleaner "
CONFIG_SEC_LOCALE_KOR
CONFIG_MACH_KS01EUR
CONFIG_EXTRA_FIRMWARE
CONFIG_EXTRA_FIRMWARE_DIR
CONFIG_TDMB
CONFIG_SEC_DEVIDE_RINGTONE_GAIN
CONFIG_WLAN_REGION_CODE
"
	echo '
# CONFIG_SEC_LOCALE_KOR is not set
CONFIG_MACH_KS01EUR=y
CONFIG_EXTRA_FIRMWARE="audience-es325-fw-KS01-eur.bin"
CONFIG_EXTRA_FIRMWARE_DIR="firmware"
# CONFIG_TDMB is not set
# CONFIG_SEC_DEVIDE_RINGTONE_GAIN is not set
CONFIG_WLAN_REGION_CODE=100
' >> .config
	make oldconfig
	make "$@" || exit 1
fi

echo "Building new ramdisk"
#remove previous ramfs files
rm -rf '$RAMFS_TMP'*
rm -rf $RAMFS_TMP
rm -rf $RAMFS_TMP.cpio
#copy ramfs files to tmp directory
cp -ax $RAMFS_SOURCE $RAMFS_TMP
cd $RAMFS_TMP

find . -name '*.sh' -exec chmod 755 {} \;

$KERNELDIR/ramdisk_fix_permissions.sh 2>/dev/null

#clear git repositories in ramfs
find . -name .git -exec rm -rf {} \;
find . -name EMPTY_DIRECTORY -exec rm -rf {} \;
cd $KERNELDIR
rm -rf $RAMFS_TMP/tmp/*

cd $RAMFS_TMP
find . | fakeroot cpio -H newc -o | lzop -9 > $RAMFS_TMP.cpio.lzo
ls -lh $RAMFS_TMP.cpio.lzo
cd $KERNELDIR

echo "Making new boot image"
gcc -w -s -pipe -O2 -o tools/dtbtool/dtbtool tools/dtbtool/dtbtool.c
tools/dtbtool/dtbtool -s 2048 -o arch/arm/boot/dt.img -p scripts/dtc/ arch/arm/boot/
gcc -w -s -pipe -O2 -Itools/libmincrypt -o tools/mkbootimg/mkbootimg tools/libmincrypt/*.c tools/mkbootimg/mkbootimg.c
tools/mkbootimg/mkbootimg --kernel $KERNELDIR/arch/arm/boot/zImage --dt $KERNELDIR/arch/arm/boot/dt.img --ramdisk $RAMFS_TMP.cpio.lzo --cmdline 'console=null androidboot.hardware=qcom user_debug=23 msm_rtb.filter=0x37 ehci-hcd.park=3 enforcing=0' --base 0x00000000 --pagesize 2048 --kernel_offset 0x00008000 --ramdisk_offset 0x02000000 --tags_offset 0x01e00000 --second_offset 0x00f00000 -o $KERNELDIR/i9506.img
echo -n "SEANDROIDENFORCE" >> i9506.img
if [ "${1}" = "CC=\$(CROSS_COMPILE)gcc" ] ; then
	dd if=/dev/zero bs=$((20971520-$(stat -c %s i9506.img))) count=1 >> i9506.img
fi

echo "done"
ls -al i9506.img
echo ""
