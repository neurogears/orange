#!/usr/bin/env bash
# orange launcher, installed as <prefix>/bin/orange by the Nix flake and by
# install.sh; run.sh uses it for a development build.
#
#  - Logs every run to ~/orange_data/logs/orange_<date>_<time>.log: a header
#    (time, user, version, command line, driver), each line orange prints with
#    a timestamp, and the exit status. orange_latest.log links to the newest.
#    The log is owned by the invoking user, also when orange runs as root.
#  - Elevates with sudo (camera capture needs raw NIC access), passing through
#    only the variables the GUI and the eSDK need. Without a terminal (started
#    from the desktop entry) sudo asks for the password in a dialog.
#  - Runs <prefix>/opt/orange/orange from that directory. ORANGE_APPDIR
#    overrides the directory (run.sh points it at release/).
set -uo pipefail

SELF="$(readlink -f "${BASH_SOURCE[0]}")"
APPDIR="${ORANGE_APPDIR:-$(cd "$(dirname "$SELF")/../opt/orange" 2>/dev/null && pwd)}"
# Dialogs for desktop launches; the Nix package substitutes its own zenity.
ZENITY="@zenity@"
[[ "$ZENITY" == @*@ ]] && ZENITY=zenity

# ---- Elevated stage: just run orange (stdout already goes to the logger) ----
if [[ "${ORANGE_LAUNCHER_STAGE:-}" == run ]]; then
    cd "$APPDIR" || exit 1
    exec "$APPDIR/orange" "$@"
fi

if [[ ! -x "$APPDIR/orange" ]]; then
    echo "orange: no executable at $APPDIR/orange" >&2
    exit 1
fi

# ---- Per-run log, in the invoking user's orange_data ----------------------
if [[ $EUID -eq 0 && -n "${SUDO_USER:-}" ]]; then
    USER_NAME="$SUDO_USER"
    USER_HOME="$(getent passwd "$SUDO_USER" | cut -d: -f6)"
else
    USER_NAME="$(id -un)"
    USER_HOME="$HOME"
fi
LOG_DIR="$USER_HOME/orange_data/logs"
mkdir -p "$LOG_DIR"
LOG_FILE="$LOG_DIR/orange_$(date +%Y-%m-%d_%H-%M-%S).log"
: >"$LOG_FILE"
ln -sfn "$(basename "$LOG_FILE")" "$LOG_DIR/orange_latest.log"
if [[ $EUID -eq 0 && -n "${SUDO_UID:-}" && -n "${SUDO_GID:-}" ]]; then
    chown -h "$SUDO_UID:$SUDO_GID" "$USER_HOME/orange_data" "$LOG_DIR" \
        "$LOG_FILE" "$LOG_DIR/orange_latest.log" 2>/dev/null
fi

version="$(cat "$APPDIR/VERSION" 2>/dev/null ||
    git -C "$APPDIR" describe --always --dirty 2>/dev/null || echo unknown)"
{
    echo "# orange run log"
    echo "# started:  $(date -Iseconds)"
    echo "# user:     $USER_NAME@$(hostname)"
    echo "# version:  $version"
    echo "# binary:   $APPDIR/orange"
    echo "# command:  orange$(printf ' %q' "$@")"
    echo "# kernel:   $(uname -r)"
    echo "# driver:   $(head -n1 /proc/driver/nvidia/version 2>/dev/null || echo none)"
    echo
} >>"$LOG_FILE"

# A terminal to answer prompts on? (false when started from the desktop entry)
interactive=0
[[ -t 0 ]] && interactive=1

run_orange() {
    if [[ $EUID -eq 0 ]]; then
        ORANGE_LAUNCHER_STAGE=run ORANGE_APPDIR="$APPDIR" "$SELF" "$@"
        return
    fi
    local sudo_opts=()
    if [[ $interactive -eq 0 ]] && ! sudo -n true 2>/dev/null; then
        export SUDO_ASKPASS="$APPDIR/orange-askpass" ORANGE_ZENITY="$ZENITY"
        sudo_opts=(-A)
    fi
    sudo "${sudo_opts[@]}" \
        ${DISPLAY:+DISPLAY="$DISPLAY"} \
        ${XAUTHORITY:+XAUTHORITY="$XAUTHORITY"} \
        ${XDG_RUNTIME_DIR:+XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR"} \
        ${WAYLAND_DISPLAY:+WAYLAND_DISPLAY="$WAYLAND_DISPLAY"} \
        ${LD_LIBRARY_PATH:+LD_LIBRARY_PATH="$LD_LIBRARY_PATH"} \
        ${__EGL_VENDOR_LIBRARY_FILENAMES:+__EGL_VENDOR_LIBRARY_FILENAMES="$__EGL_VENDOR_LIBRARY_FILENAMES"} \
        ${__GLX_VENDOR_LIBRARY_NAME:+__GLX_VENDOR_LIBRARY_NAME="$__GLX_VENDOR_LIBRARY_NAME"} \
        ${EVT_DEBUG_LOG:+EVT_DEBUG_LOG="$EVT_DEBUG_LOG"} \
        ORANGE_LAUNCHER_STAGE=run ORANGE_APPDIR="$APPDIR" \
        "$SELF" "$@"
}

# Copy orange's output to the console and, timestamped, to the log. Ctrl-C and
# a closed terminal stop orange, not the logger: it drains to EOF so the end
# of the run (and the footer below) still reaches the log.
log_lines() {
    trap '' INT HUP
    local line echo_out=1
    exec 3>>"$LOG_FILE"
    while IFS= read -r line || [[ -n "$line" ]]; do
        if [[ $echo_out -eq 1 ]]; then
            printf '%s\n' "$line" 2>/dev/null || echo_out=0
        fi
        printf '%(%Y-%m-%d %H:%M:%S)T %s\n' -1 "$line" >&3
    done
    exec 3>&-
}

trap '' INT HUP
echo "orange: logging to $LOG_FILE"
run_orange "$@" 2>&1 | log_lines
status=${PIPESTATUS[0]}

{
    echo
    echo "# exited:   $(date -Iseconds) (status $status)"
} >>"$LOG_FILE"

if [[ $status -ne 0 && $interactive -eq 0 ]]; then
    "$ZENITY" --error --title="Orange" --width=420 \
        --text="orange exited with status $status.\n\nLog: $LOG_FILE" \
        >/dev/null 2>&1 || true
fi
exit "$status"
