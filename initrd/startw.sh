#!/bin/sh
# Minimal startw.sh for AscentOS

uid=$(id -u)
: "${XDG_RUNTIME_DIR:=/tmp/ascent-runtime-$uid}"
export XDG_RUNTIME_DIR
if [ ! -d "$XDG_RUNTIME_DIR" ]; then
    mkdir "$XDG_RUNTIME_DIR" || exit 1
fi
chmod 0700 "$XDG_RUNTIME_DIR" || exit 1
rm -f "$XDG_RUNTIME_DIR"/wayland-0*

SEATD_PID=
if [ -S /run/seatd.sock ]; then
    export SEATD_SOCK="/run/seatd.sock"
elif command -v seatd >/dev/null 2>&1 && [ "$uid" -eq 0 ]; then
    # This seatd build has a fixed socket path (/run/seatd.sock).
    # Root can create that socket as a fallback when AscentD did not start it.
    export SEATD_SOCK="/run/seatd.sock"
    rm -f "$SEATD_SOCK"
    seatd -u "${USER:-root}" &
    SEATD_PID=$!
    sleep 1
else
    echo "[startw] /run/seatd.sock is missing; start the AscentD seatd service first" >&2
    exit 1
fi

cleanup() {
    [ -z "$SEATD_PID" ] || kill "$SEATD_PID" 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM

# Native DRM cursor path is enabled; do not force userspace cursor.
# export WLR_NO_HARDWARE_CURSORS=1
export XCURSOR_THEME=Adwaita
export XCURSOR_SIZE=24
export XCURSOR_PATH=/usr/share/icons/

# Set ASCENT_GRAPHICS_DEBUG=1 to restore verbose protocol/compositor logging.
if [ "${ASCENT_GRAPHICS_DEBUG:-0}" = "1" ]; then
    export WAYLAND_DEBUG=1
    export WESTON_DEBUG_COMPOSITOR=1
    export WLR_LOG_LEVEL=debug
else
    unset WAYLAND_DEBUG WESTON_DEBUG_COMPOSITOR WLR_LOG_LEVEL
fi
export WLR_RENDERER_ALLOW_SOFTWARE=1
export WLR_DRM_NO_ATOMIC=1

# xrootcursor workaround disabled. Cursor should come from native DRM
# MODE_CURSOR/CURSOR2 or the KMS cursor plane path now.
# (
#     export DISPLAY=:0
#     export HOME=/
#     if [ -x /bin/xrootcursor ]; then
#         /bin/xrootcursor 80 >>/tmp/xrootcursor.log 2>&1
#     fi
# ) &

# Redirect all output to a log file so it survives a crash
LOG="/tmp/weston-$uid.log"
echo "[startw] starting weston at $(date)" > $LOG

renderer=pixman
case "${ASCENT_RENDERER:-pixman}" in
    llvmpipe|gl)
        renderer=gl
        # Force GBM through its software-device creation path. Do not use
        # MESA_LOADER_DRIVER_OVERRIDE=kms_swrast: that takes GBM's hardware
        # path and calls a callback which software KMS does not provide.
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
        echo "[startw] unknown ASCENT_RENDERER='$ASCENT_RENDERER'" >> $LOG
        echo "[startw] expected 'llvmpipe' or 'pixman'" >> $LOG
        exit 2
        ;;
esac

echo "[startw] renderer: ${ASCENT_RENDERER:-pixman}" >> $LOG

weston \
    --backend=drm-backend.so \
    --renderer="$renderer" \
    -c /etc/weston.ini \
    --log=$LOG
status=$?
if [ "$status" -ne 0 ]; then
    echo "[startw] weston exited with status $status"
    echo "[startw] --- $LOG ---"
    cat "$LOG" 2>/dev/null || true
    echo "[startw] --- end $LOG ---"
fi
exit "$status"
