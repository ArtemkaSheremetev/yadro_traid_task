#!/bin/bash

if [ "$(id -u)" -ne 0 ]; then
	echo "Run this script with sudo"
	exit 1
fi

echo "Loading dm-mod"
modprobe dm-mod

echo "Loading dm-racewarn"
if lsmod | grep -q '^dm_racewarn'; then
	echo "dm_racewarn is already loaded"
else
	insmod ./dm-racewarn.ko
fi

echo "Checking available device-mapper targets:"
TARGET_INFO=$(dmsetup targets | grep '^racewarn')

echo
if [ -n "$TARGET_INFO" ]; then
	echo "racewarn target in dmsetup is available"
	echo "$TARGET_INFO"
else
	echo "racewarn target did not load"
	exit 1
fi
