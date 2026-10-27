#!/bin/sh
# Raw S16_LE/44.1k/stereo from the Pi. The Pi's authorized_keys entry for this key forces the arecord command and uses
# "restrict" (no shell, tty or forwarding), so the command given here is ignored:
#   restrict,command="arecord -q -D plug:bluealsa -t raw -f S16_LE -r 44100 -c 2" ssh-ed25519 AAAA... iona->piw capture-only
exec ssh -i "$HOME/.ssh/piw_capture" -o BatchMode=yes -o IdentitiesOnly=yes -o StrictHostKeyChecking=accept-new \
  -o ServerAliveInterval=5 root@PI_ADDRESS capture
