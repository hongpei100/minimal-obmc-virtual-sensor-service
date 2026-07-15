#!/bin/bash
# demo.sh - End-to-end demo on the Ubuntu VM (system bus, real vtemp.ko).
#
# Prerequisites:
#   1. kernel/vtemp:  make && (module built)
#   2. sensord:       make  (needs sdbusplus installed, see README)
#   3. sudo cp dbus/xyz.openbmc_project.VirtualSensor.conf /usr/share/dbus-1/system.d/
set -e
cd "$(dirname "$0")"

SENSOR_PATH=/xyz/openbmc_project/sensors/temperature/vtemp_temp1
DEST=xyz.openbmc_project.VirtualSensor

step() { echo; echo "==== $1 ===="; }

step "1. load vtemp.ko"
lsmod | grep -q '^vtemp' || sudo insmod kernel/vtemp/vtemp.ko
H=""
for d in /sys/class/hwmon/hwmon*; do
    [ "$(cat "$d/name")" = "vtemp" ] && H="$d" && break
done
[ -n "$H" ] || { echo "vtemp hwmon node not found"; exit 1; }
echo "hwmon node: $H"

step "2. start sensord on the system bus"
sudo pkill -x sensord 2>/dev/null || true
sudo ./sensord/sensord &
SPID=$!
sleep 1

step "3. introspect the sensor object"
busctl introspect "$DEST" "$SENSOR_PATH" --no-pager

step "4. read initial Value (expect 45)"
busctl get-property "$DEST" "$SENSOR_PATH" xyz.openbmc_project.Sensor.Value Value

step "5. watch PropertiesChanged in the background"
busctl monitor "$DEST" > /tmp/dbus-monitor.log 2>&1 &
MPID=$!
sleep 0.5

step "6. inject 80 degC -> Warning(65) and Critical(75) assert"
echo 80000 | sudo tee "$H/temp_inject" > /dev/null
sleep 2
busctl get-property "$DEST" "$SENSOR_PATH" xyz.openbmc_project.Sensor.Threshold.Warning WarningAlarmHigh
busctl get-property "$DEST" "$SENSOR_PATH" xyz.openbmc_project.Sensor.Threshold.Critical CriticalAlarmHigh

step "7. drop to 74 degC -> Critical STAYS asserted (hysteresis: deassert below 73)"
echo 74000 | sudo tee "$H/temp_inject" > /dev/null
sleep 2
busctl get-property "$DEST" "$SENSOR_PATH" xyz.openbmc_project.Sensor.Threshold.Critical CriticalAlarmHigh

step "8. back to 45 degC -> everything deasserts"
echo 45000 | sudo tee "$H/temp_inject" > /dev/null
sleep 2
busctl get-property "$DEST" "$SENSOR_PATH" xyz.openbmc_project.Sensor.Threshold.Warning WarningAlarmHigh
busctl get-property "$DEST" "$SENSOR_PATH" xyz.openbmc_project.Sensor.Threshold.Critical CriticalAlarmHigh

step "9. PropertiesChanged traffic captured during the demo"
kill $MPID 2>/dev/null || true
grep -c PropertiesChanged /tmp/dbus-monitor.log || true
echo "(full log: /tmp/dbus-monitor.log)"

sudo kill $SPID 2>/dev/null || true
echo; echo "demo complete"
