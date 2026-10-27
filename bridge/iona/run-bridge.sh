#!/bin/sh
# Bluetooth->AirPlay bridge on iona: audio pulled from the Pi (Bluetooth sink), AirPlay sent from here.
# capture_tee.py also feeds an AirPlay 1 receiver through the C speaker-send tool (pyatv's AirPlay 2 path cannot).
D="$HOME/speaker-sender"
exec "$HOME/airplay-venv/bin/python" -u "$D/bt_airplay_bridge.py" \
  --target "office speaker=30" --target "Kitchen=40" --target "Bedroom=30" \
  --capture-cmd "$D/capture_tee.py" "Living Room=30"
