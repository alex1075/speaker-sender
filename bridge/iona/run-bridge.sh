#!/bin/sh
# Bluetooth->AirPlay bridge on iona: audio pulled from the Pi (Bluetooth sink), AirPlay sent from here.
D="$HOME/airplay-sender"
exec "$HOME/airplay-venv/bin/python" -u "$D/bt_airplay_bridge.py" \
  --target "office speaker=30" --target "Kitchen=40" --target "Bedroom=30" \
  --capture-cmd "$D/capture-from-pi.sh"
