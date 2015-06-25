#!/res/busybox sh

export PATH=/res/asset:$PATH
export ext4=1

mount -t ext4 -o ro,noatime,nodiratime,data=ordered,barrier=0,nodiscard /dev/block/platform/msm_sdcc.1/by-name/system /system
mount -t f2fs -o ro,noatime,nodiratime,background_gc=off,discard /dev/block/platform/msm_sdcc.1/by-name/system /system

if [ ! -f /system/framework/multiwindow.jar ] ; then
	export ext4=0
	umount -f /system
	mount -t ext4 -o noatime,nodiratime,data=ordered,barrier=0,nodiscard,nosuid,nodev,noauto_da_alloc,errors=panic /dev/block/platform/msm_sdcc.1/by-name/userdata /arter97/data
	mount -t f2fs -o noatime,nodiratime,background_gc=on,discard,nosuid,nodev /dev/block/platform/msm_sdcc.1/by-name/userdata /arter97/data
	if [ -e /arter97/data/arter97_secondrom/loadfromext ] ; then
		umount /arter97/data
		echo "on" > /sys/devices/msm_sdcc.3/mmc_host/mmc2/power/control
		mount -t ext4 -o noatime,nodiratime,data=ordered,barrier=0,nodiscard,nosuid,nodev,noauto_da_alloc,errors=panic /dev/block/mmcblk1p1 /arter97/data
		mount -t f2fs -o noatime,nodiratime,background_gc=on,discard,nosuid,nodev /dev/block/mmcblk1p1 /arter97/data
	fi
	chmod 755 /arter97/data/arter97_secondrom/system
	chmod 771 /arter97/data/arter97_secondrom/data
	chmod 771 /arter97/data/arter97_secondrom/cache
	mount --bind /arter97/data/arter97_secondrom/system /system
	mount --bind /arter97/data/arter97_secondrom/data /data
	mount --bind /arter97/data/arter97_secondrom/cache /cache
	mkdir /arter97/data/.arter97
	mkdir /data/.arter97
	rm -rf /data/.arter97/*
	rm -rf /data/.arter97/.*
	chmod 777 /arter97/data/.arter97
	chmod 777 /data/.arter97
	mount --bind /arter97/data/.arter97 /data/.arter97
	mount --bind -o remount,suid,dev /system
	if [ -f /arter97/data/media/0/.arter97/shared ]; then
		rm -rf /arter97/data/arter97_secondrom/data/media/0/.arter97
		cp -rp /arter97/data/arter97_secondrom/data/media/* /arter97/data/media/
		rm -rf /data/media/*
		mount --bind /arter97/data/media /data/media
	fi
	CUR_PATH=$PATH
	export PATH=/sbin:/system/sbin:/system/bin:/system/xbin
	export LD_LIBRARY_PATH=/vendor/lib:/system/lib
	run-parts /arter97/data/arter97_secondrom/init.d
	export PATH=$CUR_PATH
else
	rm -rf /arter97
	mount -t ext4 -o noatime,nodiratime,data=ordered,barrier=0,nodiscard,nosuid,nodev,noauto_da_alloc,errors=panic /dev/block/platform/msm_sdcc.1/by-name/userdata /data || export ext4=0
	mount -t f2fs -o noatime,nodiratime,background_gc=on,discard,nosuid,nodev /dev/block/platform/msm_sdcc.1/by-name/userdata /data
	mount -t ext4 -o noatime,nodiratime,data=ordered,barrier=0,nodiscard,nosuid,nodev,noauto_da_alloc,errors=panic /dev/block/platform/msm_sdcc.1/by-name/cache /cache
	mount -t f2fs -o noatime,nodiratime,background_gc=on,discard,nosuid,nodev /dev/block/platform/msm_sdcc.1/by-name/cache /cache
fi

if [[ $ext4 == "1" ]]; then
	sed -i -e 's/# VOLD/\/dev\/block\/platform\/msm_sdcc.1\/by-name\/userdata       \/data            ext4    nosuid,nodev,noatime,noauto_da_alloc,nodiscard,errors=panic      wait,check,encryptable=footer\n\n# VOLD/g' /fstab.qcom
	umount -f /data
fi

touch /dev/block/mounted
