#!/bin/sh
# startw.sh — start Weston (Wayland compositor) for AscentOS
#
# Session launched inside XWayland is selected by ASCENT_SESSION (default: none):
#   none    — just Weston with its built-in panel (default)
#   xfce4   — auto-launch XFCE4 inside XWayland once Weston is ready
#
# Renderer is selected by ASCENT_RENDERER (default: pixman):
#   pixman  — CPU software rasteriser, safe on all QEMU configs
#   llvmpipe / gl — Mesa llvmpipe OpenGL path
#
# Examples:
#   /bin/startw.sh
#   ASCENT_SESSION=xfce4 /bin/startw.sh
#   ASCENT_RENDERER=llvmpipe /bin/startw.sh

uid=$(id -u)

# ── XDG_RUNTIME_DIR ──────────────────────────────────────────────────────
: "${XDG_RUNTIME_DIR:=/tmp/ascent-runtime-$uid}"
export XDG_RUNTIME_DIR
mkdir -p "$XDG_RUNTIME_DIR" || exit 1
chmod 0700 "$XDG_RUNTIME_DIR" || exit 1
rm -f "$XDG_RUNTIME_DIR"/wayland-0*

# ── seatd ────────────────────────────────────────────────────────────────
SEATD_PID=
if [ -S /run/seatd.sock ]; then
    export SEATD_SOCK="/run/seatd.sock"
elif command -v seatd >/dev/null 2>&1 && [ "$uid" -eq 0 ]; then
    export SEATD_SOCK="/run/seatd.sock"
    rm -f "$SEATD_SOCK"
    seatd -u "${USER:-root}" &
    SEATD_PID=$!
    sleep 1
else
    echo "[startw] /run/seatd.sock missing; start the AscentD seatd service first" >&2
    exit 1
fi

cleanup() {
    [ -z "$SEATD_PID" ] || kill "$SEATD_PID" 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM

# ── Cursor / theme ────────────────────────────────────────────────────────
export XCURSOR_THEME=Adwaita
export XCURSOR_SIZE=24
export XCURSOR_PATH=/usr/share/icons/

# ── Debug logging ─────────────────────────────────────────────────────────
if [ "${ASCENT_GRAPHICS_DEBUG:-0}" = "1" ]; then
    export WAYLAND_DEBUG=1
    export WESTON_DEBUG_COMPOSITOR=1
    export WLR_LOG_LEVEL=debug
else
    unset WAYLAND_DEBUG WESTON_DEBUG_COMPOSITOR WLR_LOG_LEVEL
fi

# ── Renderer selection ───────────────────────────────────────────────────
export WLR_RENDERER_ALLOW_SOFTWARE=1

renderer=pixman
case "${ASCENT_RENDERER:-pixman}" in
    llvmpipe|gl)
        renderer=gl
        export GBM_ALWAYS_SOFTWARE=1
        export LIBGL_ALWAYS_SOFTWARE=1
        export GALLIUM_DRIVER=llvmpipe
        ;;
    pixman)
        renderer=pixman
        unset GBM_ALWAYS_SOFTWARE
        export LIBGL_ALWAYS_SOFTWARE=1
        export GALLIUM_DRIVER=llvmpipe
        ;;
    *)
        echo "[startw] Unknown ASCENT_RENDERER='${ASCENT_RENDERER}' (expected: pixman, llvmpipe)" >&2
        exit 2
        ;;
esac

LOG="/tmp/weston-$uid.log"
echo "[startw] starting weston at $(date) (renderer: ${renderer})" > "$LOG"
echo "[startw] renderer: ${renderer}"

# ── XFCE4 auto-launch inside XWayland ────────────────────────────────────
# When ASCENT_SESSION=xfce4 we wait for Weston to export its XWayland
# display socket, then launch start-xfce4-wayland in the background.
launch_xfce4_when_ready() {
    echo "[startw] waiting for XWayland display socket..." >> "$LOG"
    for i in $(seq 1 30); do
        for d in 10 0 1 2 3 4 5; do
            if [ -S "/tmp/.X11-unix/X${d}" ]; then
                echo "[startw] XWayland found on :${d}, launching XFCE4..." >> "$LOG"
                echo "[startw] XWayland ready on :${d} — starting XFCE4"
                DISPLAY=":${d}" \
                XDG_SESSION_TYPE=x11 \
                XDG_CURRENT_DESKTOP=XFCE \
                HOME="${HOME:-/}" \
                    /usr/bin/start-xfce4-wayland >> "$LOG" 2>&1 &
                return 0
            fi
        done
        sleep 1
    done
    echo "[startw] Warning: XWayland socket not found after 30s" >> "$LOG"
    echo "[startw] Warning: XWayland socket not found — XFCE4 not launched"
}

case "${ASCENT_SESSION:-none}" in
    xfce4)
        launch_xfce4_when_ready &
        ;;
    none|*)
        # nothing — user can open weston-terminal and run start-xfce4-wayland manually
        ;;
esac

# ── Start Weston ──────────────────────────────────────────────────────────
weston \
    --backend=drm-backend.so \
    --renderer="$renderer" \
    -c /etc/weston.ini \
    --log="$LOG"

status=$?
if [ "$status" -ne 0 ]; then
    echo "[startw] weston exited with status $status"
    echo "[startw] --- $LOG ---"
    cat "$LOG" 2>/dev/null || true
    echo "[startw] --- end $LOG ---"
fi
exit "$status"
