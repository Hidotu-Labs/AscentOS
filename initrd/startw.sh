#!/bin/sh
# Minimal startw.sh for AscentOS

export XDG_RUNTIME_DIR=/tmp/wayland
mkdir -p $XDG_RUNTIME_DIR
rm -f $XDG_RUNTIME_DIR/wayland-0*

# Xwayland publishes its display sockets here. Weston creates the socket file
# itself, but it expects the parent directory to already exist.
mkdir -p /tmp/.X11-unix
chmod 1777 /tmp /tmp/.X11-unix 2>/dev/null || true
rm -f /tmp/.X11-unix/X0 /tmp/.X0-lock

seatd -u root &
sleep 1

# Native DRM cursor path is enabled; do not force userspace cursor.
# export WLR_NO_HARDWARE_CURSORS=1
export WESTON_FORCE_RENDERER=1
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
#     export HOME=/root
#     if [ -x /bin/xrootcursor ]; then
#         /bin/xrootcursor 80 >>/tmp/xrootcursor.log 2>&1
#     fi
# ) &

# Redirect all output to a log file so it survives a crash
LOG=/tmp/weston-debug.log
echo "[startw] starting weston at $(date)" > $LOG

weston \
    --backend=drm-backend.so \
    --renderer=pixman \
    -c /etc/weston.ini \
    --log=$LOG \
    2>&1 | tee -a $LOG
