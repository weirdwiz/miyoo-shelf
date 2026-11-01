// SPDX-License-Identifier: GPL-3.0-only
// Backport only the GameSwitcher handoff to Onion 4.3.1-1 keymon.
// Shelf negotiates pause/save commands with the installed RetroArch itself.
void action_RA_gameSwitcher(void)
{
    if (exists("/mnt/SDCARD/.tmp_update/.runGameSwitcher"))
        return;
    set_gameSwitcher();
    system("(gameSwitcher --overlay; touch /tmp/state_changed) &");
    system_state_update();
}
