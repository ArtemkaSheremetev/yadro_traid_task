#!/bin/bash

if [ "$(id -u)" -ne 0 ]; then
	echo "Run this script with sudo"
	exit 1
fi

echo "Checking available device-mapper targets"
dmsetup targets | grep racewarn >/dev/null || {
	echo "racewarn target is not loaded. Run ./scripts/load-racewarn.sh first"
	exit 1
}

echo "Preparing backend device"
truncate -s 1G /tmp/racewarn.img
losetup -f /tmp/racewarn.img

LOOP_DEV=$(losetup -a | grep racewarn.img | cut -d: -f1)

echo "Creating dm-racewarn device"
echo "0 2097152 racewarn ${LOOP_DEV}" | dmsetup create my0

echo "Running overlapping writes"
dd oflag=direct if=/dev/urandom of=/dev/mapper/my0 bs=32k count=1 seek=4 status=none &
dd oflag=direct if=/dev/urandom of=/dev/mapper/my0 bs=8k count=1 seek=17 status=none
wait

echo "Checking target status"
dmsetup status my0

echo "Recent kernel messages"
dmesg | tail -n 20

echo "Cleaning up"
dmsetup remove my0
losetup -d "${LOOP_DEV}"
rm -f /tmp/racewarn.img
