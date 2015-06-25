#!/res/busybox sh

export PATH=/res/asset:$PATH

echo "1" > /sys/module/intelli_plug/parameters/intelli_plug_active
echo "1" > /sys/module/msm_thermal/parameters/enabled
echo "0" > /sys/module/msm_thermal/core_control/enabled
echo "1" > /sys/devices/system/cpu/cpu1/online
echo "1" > /sys/devices/system/cpu/cpu2/online
echo "1" > /sys/devices/system/cpu/cpu3/online

source /sbin/cpu.sh
source /sbin/arteractive.sh

DEFAULTPOLLMS=$(cat /sys/module/msm_thermal/parameters/poll_ms)
echo "50" > /sys/module/msm_thermal/parameters/poll_ms

while ! pgrep com.android ; do
	sleep 1
done

sleep 1

while pgrep bootanimation ; do
	sleep 1
done

sleep 1

while pgrep dexopt ; do
	sleep 1
done

echo "1" > /sys/module/msm_thermal/core_control/enabled
echo "$DEFAULTPOLLMS" > /sys/module/msm_thermal/parameters/poll_ms
source /sbin/arteractive.sh

if [ -e /arter97 ] ; then
	fstrim -v /arter97/data
else
	fstrim -v /system
	fstrim -v /cache
	fstrim -v /data
fi

sync
