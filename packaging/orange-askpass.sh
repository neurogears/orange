#!/usr/bin/env bash
# SUDO_ASKPASS helper: the launcher uses it when orange is started without a
# terminal (desktop entry), so the sudo password is asked in a dialog.
exec "${ORANGE_ZENITY:-zenity}" --password --title="Orange: administrator password" \
    2>/dev/null
