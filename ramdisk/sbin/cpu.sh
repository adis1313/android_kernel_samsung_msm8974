#!/res/busybox sh

export PATH=/res/asset:$PATH

chmod 666 /sys/module/lpm_levels/enable_low_power/l2
chmod 666 /sys/module/msm_pm/modes/cpu0/power_collapse/suspend_enabled
chmod 666 /sys/module/msm_pm/modes/cpu1/power_collapse/suspend_enabled
chmod 666 /sys/module/msm_pm/modes/cpu2/power_collapse/suspend_enabled
chmod 666 /sys/module/msm_pm/modes/cpu3/power_collapse/suspend_enabled
chmod 666 /sys/module/msm_pm/modes/cpu0/power_collapse/idle_enabled
chmod 666 /sys/module/msm_pm/modes/cpu1/power_collapse/idle_enabled
chmod 666 /sys/module/msm_pm/modes/cpu2/power_collapse/idle_enabled
chmod 666 /sys/module/msm_pm/modes/cpu3/power_collapse/idle_enabled
chmod 666 /sys/module/msm_pm/modes/cpu0/standalone_power_collapse/suspend_enabled
chmod 666 /sys/module/msm_pm/modes/cpu1/standalone_power_collapse/suspend_enabled
chmod 666 /sys/module/msm_pm/modes/cpu2/standalone_power_collapse/suspend_enabled
chmod 666 /sys/module/msm_pm/modes/cpu3/standalone_power_collapse/suspend_enabled
chmod 666 /sys/module/msm_pm/modes/cpu0/standalone_power_collapse/idle_enabled
chmod 666 /sys/module/msm_pm/modes/cpu1/standalone_power_collapse/idle_enabled
chmod 666 /sys/module/msm_pm/modes/cpu2/standalone_power_collapse/idle_enabled
chmod 666 /sys/module/msm_pm/modes/cpu3/standalone_power_collapse/idle_enabled
chmod 666 /sys/module/msm_pm/modes/cpu0/retention/idle_enabled
chmod 666 /sys/module/msm_pm/modes/cpu1/retention/idle_enabled
chmod 666 /sys/module/msm_pm/modes/cpu2/retention/idle_enabled
chmod 666 /sys/module/msm_pm/modes/cpu3/retention/idle_enabled
chmod 666 /proc/sys/kernel/sched_wake_to_idle
echo "4" > /sys/module/lpm_levels/enable_low_power/l2
echo "1" > /sys/module/msm_pm/modes/cpu0/power_collapse/suspend_enabled
echo "1" > /sys/module/msm_pm/modes/cpu1/power_collapse/suspend_enabled
echo "1" > /sys/module/msm_pm/modes/cpu2/power_collapse/suspend_enabled
echo "1" > /sys/module/msm_pm/modes/cpu3/power_collapse/suspend_enabled
echo "1" > /sys/module/msm_pm/modes/cpu0/power_collapse/idle_enabled
echo "1" > /sys/module/msm_pm/modes/cpu1/power_collapse/idle_enabled
echo "1" > /sys/module/msm_pm/modes/cpu2/power_collapse/idle_enabled
echo "1" > /sys/module/msm_pm/modes/cpu3/power_collapse/idle_enabled
echo "1" > /sys/module/msm_pm/modes/cpu0/standalone_power_collapse/suspend_enabled
echo "1" > /sys/module/msm_pm/modes/cpu1/standalone_power_collapse/suspend_enabled
echo "1" > /sys/module/msm_pm/modes/cpu2/standalone_power_collapse/suspend_enabled
echo "1" > /sys/module/msm_pm/modes/cpu3/standalone_power_collapse/suspend_enabled
echo "1" > /sys/module/msm_pm/modes/cpu0/standalone_power_collapse/idle_enabled
echo "1" > /sys/module/msm_pm/modes/cpu1/standalone_power_collapse/idle_enabled
echo "1" > /sys/module/msm_pm/modes/cpu2/standalone_power_collapse/idle_enabled
echo "1" > /sys/module/msm_pm/modes/cpu3/standalone_power_collapse/idle_enabled
echo "1" > /sys/module/msm_pm/modes/cpu0/retention/idle_enabled
echo "1" > /sys/module/msm_pm/modes/cpu1/retention/idle_enabled
echo "1" > /sys/module/msm_pm/modes/cpu2/retention/idle_enabled
echo "1" > /sys/module/msm_pm/modes/cpu3/retention/idle_enabled
echo "0" > /proc/sys/kernel/sched_wake_to_idle
chmod 444 /sys/module/lpm_levels/enable_low_power/l2
chmod 444 /sys/module/msm_pm/modes/cpu0/power_collapse/suspend_enabled
chmod 444 /sys/module/msm_pm/modes/cpu1/power_collapse/suspend_enabled
chmod 444 /sys/module/msm_pm/modes/cpu2/power_collapse/suspend_enabled
chmod 444 /sys/module/msm_pm/modes/cpu3/power_collapse/suspend_enabled
chmod 444 /sys/module/msm_pm/modes/cpu0/power_collapse/idle_enabled
chmod 444 /sys/module/msm_pm/modes/cpu1/power_collapse/idle_enabled
chmod 444 /sys/module/msm_pm/modes/cpu2/power_collapse/idle_enabled
chmod 444 /sys/module/msm_pm/modes/cpu3/power_collapse/idle_enabled
chmod 444 /sys/module/msm_pm/modes/cpu0/standalone_power_collapse/suspend_enabled
chmod 444 /sys/module/msm_pm/modes/cpu1/standalone_power_collapse/suspend_enabled
chmod 444 /sys/module/msm_pm/modes/cpu2/standalone_power_collapse/suspend_enabled
chmod 444 /sys/module/msm_pm/modes/cpu3/standalone_power_collapse/suspend_enabled
chmod 444 /sys/module/msm_pm/modes/cpu0/standalone_power_collapse/idle_enabled
chmod 444 /sys/module/msm_pm/modes/cpu1/standalone_power_collapse/idle_enabled
chmod 444 /sys/module/msm_pm/modes/cpu2/standalone_power_collapse/idle_enabled
chmod 444 /sys/module/msm_pm/modes/cpu3/standalone_power_collapse/idle_enabled
chmod 444 /sys/module/msm_pm/modes/cpu0/retention/idle_enabled
chmod 444 /sys/module/msm_pm/modes/cpu1/retention/idle_enabled
chmod 444 /sys/module/msm_pm/modes/cpu2/retention/idle_enabled
chmod 444 /sys/module/msm_pm/modes/cpu3/retention/idle_enabled
chmod 444 /proc/sys/kernel/sched_wake_to_idle
