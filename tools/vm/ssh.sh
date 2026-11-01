#!/bin/sh
# Runs a command in the VM as root: tools/vm/ssh.sh 'uname -a'
set -eu
. "$(dirname "$0")/common.sh"
vm_ssh "$@"
