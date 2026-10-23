#!/bin/sh
# Open a pairing window (default 120 s), then close it again and trust whatever paired.
# Order matters for a transmitter-only device like the Sony PS-LX310BT: open the window first, THEN hold
# the turntable's BLUETOOTH/PAIRING button for ~2 s (LED flashes blue). The turntable starts the pairing.
T="${1:-120}"
bluetoothctl pairable on >/dev/null
bluetoothctl discoverable on >/dev/null
echo "Pairing window open for ${T}s - start pairing on the turntable now."
(
  sleep "$T"
  bluetoothctl discoverable off >/dev/null
  bluetoothctl pairable off >/dev/null
  # trust every paired device so it can reconnect by itself later
  for mac in $(bluetoothctl devices | awk '{print $2}'); do
    bluetoothctl info "$mac" | grep -q "Paired: yes" && bluetoothctl trust "$mac" >/dev/null
  done
) >/dev/null 2>&1 &
