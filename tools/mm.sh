#!/bin/sh
# Run a command on the Miyoo over SSH: MM_HOST=<device-ip> tools/mm.sh 'uname -a'
# Uses Onion's default SSH login (onion/onion); override the password with MM_PASS.
# Enable SSH in Onion: Apps > Tweaks > Network > SSH.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
host=${MM_HOST:?set MM_HOST to the device IP (Onion: Settings > Network)}
mkdir -p "$here/build"
askpass="$here/build/askpass.sh"
printf '#!/bin/sh\necho "%s"\n' "${MM_PASS:-onion}" > "$askpass"
chmod 700 "$askpass"
export SSH_ASKPASS="$askpass" SSH_ASKPASS_REQUIRE=force DISPLAY=${DISPLAY:-:0}
exec ssh -o ConnectTimeout=8 -o StrictHostKeyChecking=accept-new \
    -o UserKnownHostsFile="$here/build/known_hosts" \
    -o ControlMaster=auto -o ControlPath="$here/build/ssh-%r@%h" -o ControlPersist=10m \
    "onion@$host" "$@"
