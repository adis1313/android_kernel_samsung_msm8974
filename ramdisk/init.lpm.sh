#!/res/busybox sh

export PATH=/res/asset:$PATH

source /init.mount.sh

echo "0" > /sys/module/intelli_plug/parameters/intelli_plug_active
echo "0" > /sys/module/msm_thermal/parameters/enabled
echo "0" > /sys/module/msm_thermal/core_control/enabled
echo "0" > /sys/devices/system/cpu/cpu1/online
echo "0" > /sys/devices/system/cpu/cpu2/online
echo "0" > /sys/devices/system/cpu/cpu3/online

source /sbin/cpu.sh
source /sbin/arteractive.sh

touch /dev/lpm_prepared
