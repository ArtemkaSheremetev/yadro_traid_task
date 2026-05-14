#!/usr/bin/env bash

if [ "$(id -u)" -ne 0 ]; then
	echo "Run this script with sudo"
	exit 1
fi

if lsmod | grep -q '^dm_racewarn'; then
	echo "Unloading dm_racewarn"
	rmmod dm_racewarn
else
	echo "dm_racewarn is not loaded"
fi
