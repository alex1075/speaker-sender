#!/bin/sh
# Bluetooth -> AirPlay with the C tool for every speaker (AirPlay 1 and 2, one clock, so rooms stay in sync).
# Audio is pulled from the Pi (Bluetooth sink); restarts itself if the capture or the sender ends.
D="$HOME/airplay-sender"
while :; do
  "$D/capture-from-pi.sh" | "$D/airplay-send" --scan 6000 \
    -t "office speaker=30" -t "Kitchen=40" -t "Bedroom=30" -t "Living Room=30"
  sleep 3
done
