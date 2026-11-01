#!/bin/sh
# Make Shelf the menu Onion boots into, and its game switcher, without changing any of
# Onion's files.
#   boot.sh enable    install the startup hook and switch to Shelf now
#   boot.sh disable   remove the hook and give Onion's menu and switcher back now
#   boot.sh mount     run by the hook at every boot
#   boot.sh switcher  remount the switcher after the Shelf binary changed (make push)
#   boot.sh switcher-off  persistently restore Onion's switcher/keymon, keeping Shelf home
#   boot.sh switcher-on   explicitly enable the experimental overlay for device trials
#
# Onion's runtime starts miyoo/app/MainUI whenever no game is queued. It bind-mounts
# .tmp_update/bin/MainUI-<model>-<mode> there and only remounts when the mounted file's
# name differs, so a stub with that same name is left in place. Mounts don't survive a
# reboot: deleting .tmp_update/startup/shelf.sh (or App/Shelf) restores stock Onion, and
# holding SELECT at power-on skips Shelf for that boot.
#
# The switcher: keymon runs `gameSwitcher --overlay` over a paused RetroArch, and the
# runtime runs `gameSwitcher` from .tmp_update/bin. The Shelf binary itself is mounted
# there, so it runs under that name (keymon looks for a process called gameSwitcher) and
# switches to its switcher mode. Onion's own is copied to boot/gameSwitcher-onion first
# for recovery. An unverifiable overlay exits without taking over the game's display.
sysdir=/mnt/SDCARD/.tmp_update
shelf=/mnt/SDCARD/App/Shelf
target=/mnt/SDCARD/miyoo/app/MainUI
switcher=$sysdir/bin/gameSwitcher
keymon=$sysdir/bin/keymon
hook=$sysdir/startup/shelf.sh
switcher_disabled=$shelf/boot/switcher-disabled

is_mounted() { grep -q "/App/Shelf/boot/[^ ]* $target " /proc/self/mountinfo; }
switcher_mounted() { grep -q " $switcher " /proc/self/mountinfo; }
keymon_mounted() { grep -q "/App/Shelf/keymon-overlay[^ ]* $keymon " /proc/self/mountinfo; }

restart_keymon() {
    killall -TERM keymon 2> /dev/null
    for i in 1 2 3 4 5 6 7 8 9 10; do
        pidof keymon > /dev/null || break
        sleep 0.1
    done
    pidof keymon > /dev/null && return 1 # never start two input monitors
    "$keymon" < /dev/null > /tmp/shelf-keymon.log 2>&1 &
}

mount_keymon() {
    [ -f "$switcher_disabled" ] && return 0
    # New Onion already launches overlays. Never replace its keymon with an older one.
    [ "$(cat "$sysdir/onionVersion/version.txt")" = "v4.3.1-1" ] || return 0
    if [ ! -x "$shelf/keymon-overlay" ]; then
        echo "shelf: legacy Onion needs keymon-overlay for the paused switcher" >&2
        return 0
    fi
    while keymon_mounted && umount -l "$keymon"; do :; done
    keymon_mounted && return 1
    cp "$keymon" "$shelf/boot/keymon-onion" || return 1
    mount -o bind "$shelf/keymon-overlay" "$keymon" || return 1
    restart_keymon
}

mount_switcher() {
    [ -f "$switcher_disabled" ] && return 0
    [ -x "$shelf/shelf" ] || return 0
    while switcher_mounted && umount -l "$switcher" 2> /dev/null; do :; done
    switcher_mounted && return 1 # never copy Shelf over the saved Onion switcher
    mkdir -p "$shelf/boot"
    cmp -s "$switcher" "$shelf/boot/gameSwitcher-onion" || cp "$switcher" "$shelf/boot/gameSwitcher-onion"
    mount -o bind "$shelf/shelf" "$switcher"
}

restore_onion_switcher() {
    while switcher_mounted && umount -l "$switcher"; do :; done
    switcher_mounted && return 1
    if keymon_mounted; then
        while keymon_mounted && umount -l "$keymon"; do :; done
        keymon_mounted && return 1
        restart_keymon || return 1
    fi
}

do_mount() {
    [ -x "$shelf/shelf" ] || return 0
    "$sysdir/bin/detectKey" 97 && return 0 # SELECT (KEY_RIGHTCTRL) held: stock Onion
    switcher_mounted || mount_switcher
    keymon_mounted || mount_keymon
    is_mounted && return 0
    mode=$([ -f "$sysdir/config/.showExpert" ] && echo expert || echo clean)
    stub=$shelf/boot/MainUI-$(cat /tmp/deviceModel)-$mode
    if [ ! -f "$stub" ]; then
        mkdir -p "$shelf/boot"
        # Re-exec as plain sh: keymon treats a process named MainUI as Onion's menu.
        printf '#!/bin/sh\nexec /bin/sh %s/mainui.sh\n' "$shelf" > "$stub"
        chmod +x "$stub"
    fi
    # Lazy, since the stock binary may be running; stacking mounts would confuse the
    # runtime's name check.
    while umount -l "$target" 2> /dev/null; do :; done
    mount -o bind "$stub" "$target"
}

case "$1" in
mount)
    do_mount
    ;;
enable)
    printf '#!/bin/sh\n# Boots into Shelf. Delete this file for stock Onion.\n[ -x %s/boot.sh ] && %s/boot.sh mount\n' \
        "$shelf" "$shelf" > "$hook"
    do_mount
    sync
    # Restart the menu so Shelf shows now, as keymon does for its shortcuts.
    killall -9 MainUI 2> /dev/null
    echo "shelf: boot enabled ($hook)"
    ;;
switcher)
    if [ -f "$hook" ]; then
        mount_switcher && mount_keymon
    fi
    ;;
switcher-on)
    [ -f "$hook" ] || { echo "shelf: enable Shelf home first" >&2; exit 1; }
    rm -f "$switcher_disabled"
    mount_switcher && mount_keymon || exit 1
    sync
    echo "shelf: experimental switcher enabled"
    ;;
switcher-off)
    mkdir -p "$shelf/boot" || exit 1
    touch "$switcher_disabled" || exit 1
    restore_onion_switcher || exit 1
    sync
    echo "shelf: stock Onion switcher restored; Shelf home is unchanged"
    ;;
disable)
    rm -f "$hook"
    # The runtime mounts its own MainUI again the next time the menu opens.
    while is_mounted && umount -l "$target"; do :; done
    restore_onion_switcher || exit 1
    sync
    echo "shelf: boot disabled; Onion's menu returns next time it opens"
    ;;
*)
    echo "usage: $0 enable | disable | mount | switcher | switcher-on | switcher-off" >&2
    exit 2
    ;;
esac
