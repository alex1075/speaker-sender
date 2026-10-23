#!/bin/sh
# Run as root on the Pi, from the directory that holds bt_airplay_bridge.py and pi/.
# Needs: apt install bluez-alsa-utils alsa-utils bluez-tools ; pyatv in /root/airplay-venv
# The bridge service is installed but NOT enabled; enable it once pairing has been tested:
#   systemctl enable --now bt-airplay-bridge
set -e
D="$(cd "$(dirname "$0")" && pwd)"
systemctl disable --now bluealsa-aplay.service 2>/dev/null || true   # we capture the audio, we don't play it locally
mkdir -p /etc/systemd/system/bluealsa.service.d
install -m 644 "$D/pi/bluealsa-override.conf" /etc/systemd/system/bluealsa.service.d/override.conf
install -m 755 "$D/pi/bt-sink-setup.sh" /usr/local/bin/bt-sink-setup.sh
install -m 755 "$D/pi/bt-pair-mode.sh" /usr/local/bin/bt-pair-mode
install -m 644 "$D/pi/bt-agent.service" /etc/systemd/system/bt-agent.service
install -m 644 "$D/pi/bt-sink-setup.service" /etc/systemd/system/bt-sink-setup.service
install -m 644 "$D/pi/bt-airplay-bridge.service" /etc/systemd/system/bt-airplay-bridge.service
[ -f /etc/default/bt-airplay-bridge ] || install -m 644 "$D/pi/bt-airplay-bridge.default" /etc/default/bt-airplay-bridge
systemctl daemon-reload
systemctl enable --now bluealsa.service bt-agent.service bt-sink-setup.service
systemctl restart bluealsa.service
echo "done. Open a pairing window with:  bt-pair-mode 180"
