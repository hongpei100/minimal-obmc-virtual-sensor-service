#!/bin/sh
# acceptance test for vtemp.ko
set -e

if ! lsmod | grep -q '^vtemp'; then
	sudo insmod vtemp.ko
	echo "[+] vtemp.ko loaded"
fi

# Find our hwmon node by chip name (index is not stable across boots)
H=""
for d in /sys/class/hwmon/hwmon*; do
	if [ "$(cat "$d/name")" = "vtemp" ]; then
		H="$d"
		break
	fi
done
[ -n "$H" ] || { echo "vtemp hwmon device not found"; exit 1; }
echo "[+] hwmon node: $H"

echo "--- default state ---"
echo "temp1_input     = $(cat "$H/temp1_input")"
echo "temp1_max       = $(cat "$H/temp1_max")"
echo "temp1_max_alarm = $(cat "$H/temp1_max_alarm")"

echo "--- inject 85.0 degC (above the 75.0 degC limit) ---"
echo 85000 | sudo tee "$H/temp_inject" > /dev/null
echo "temp1_input     = $(cat "$H/temp1_input")"
echo "temp1_max_alarm = $(cat "$H/temp1_max_alarm")   (expect 1)"

echo "--- inject back to 45.0 degC ---"
echo 45000 | sudo tee "$H/temp_inject" > /dev/null
echo "temp1_max_alarm = $(cat "$H/temp1_max_alarm")   (expect 0)"

echo "--- lower the limit below current temp via standard ABI ---"
echo 40000 | sudo tee "$H/temp1_max" > /dev/null
echo "temp1_max_alarm = $(cat "$H/temp1_max_alarm")   (expect 1)"
echo 75000 | sudo tee "$H/temp1_max" > /dev/null

echo "[+] all checks done"