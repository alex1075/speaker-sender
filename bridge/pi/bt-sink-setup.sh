#!/bin/sh
# Make the Pi look like a Bluetooth speaker, but NOT discoverable/pairable until bt-pair-mode is run.
bluetoothctl power on
bluetoothctl system-alias "Phono Bridge"
timeout 5 btmgmt class 4 20 </dev/null >/dev/null 2>&1 || true   # major 0x04 audio/video, minor 0x14 loudspeaker -> Class 0x440414 (timeout: btmgmt hangs without a tty)
bluetoothctl discoverable off
bluetoothctl pairable off
