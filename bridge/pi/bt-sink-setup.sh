#!/bin/sh
# Make the Pi look like a Bluetooth speaker, but NOT discoverable/pairable until bt-pair-mode is run.
# The device class (audio/video, loudspeaker) comes from "Class = 0x240414" in /etc/bluetooth/main.conf (set by
# setup_pi.sh). hciconfig/btmgmt tricks get overwritten by bluetoothd or hang without a tty.
bluetoothctl power on
bluetoothctl system-alias "Phono Bridge"
bluetoothctl discoverable off
bluetoothctl pairable off
