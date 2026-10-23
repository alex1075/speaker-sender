#!/bin/sh
# Open a pairing window (default 180 s). Put the phono in pairing mode while this is open.
T="${1:-180}"
bluetoothctl pairable-timeout "$T"
bluetoothctl pairable on
bluetoothctl discoverable-timeout "$T"
bluetoothctl discoverable on
echo "Pairing window open for ${T}s. Paired devices:"
bluetoothctl devices Paired
