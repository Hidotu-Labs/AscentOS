#!/bin/sh
# Xorg + Window Manager startup script for AvoryOS
# Supports IceWM (default) and XFCE4 (ASCENT_SESSION=xfce4)

rm -f /tmp/.X0-lock /tmp/.X11-unix/X0

export DISPLAY=:0
: "${HOME:=/root}"
export HOME
export XAUTHORITY="${HOME}/.Xauthority"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/runtime-root}"
mkdir -p "${XDG_RUNTIME_DIR}"
chmod 700 "${XDG_RUNTIME_DIR}"

export PATH="/usr/bin:/bin:/usr/local/bin:/opt/coreutils/bin:/opt/bash/bin:${PATH}"
export LD_LIBRARY_PATH="/usr/lib:/lib:/usr/local/lib:${LD_LIBRARY_PATH:-}"

XORG_LOG="/var/log/Xorg.0.log"
mkdir -p /var/log

XORG=/usr/libexec/Xorg
[ ! -x "$XORG" ] && XORG=/usr/bin/Xorg

if [ ! -x "$XORG" ]; then
    echo "[startx] Error: Xorg binary not found."
    exit 1
fi

"$XORG" "$DISPLAY" -noreset -nolisten tcp \
    -configdir /etc/X11/xorg.conf.d \
    -logfile "$XORG_LOG" &
XORG_PID=$!

ready=0
for _ in 1 2 3 4 5 6 7 8 9 10; do
    if [ -S /tmp/.X11-unix/X0 ]; then
        ready=1
        break
    fi
    if ! kill -0 "$XORG_PID" 2>/dev/null; then
        echo "[startx] Error: Xorg failed to start. See $XORG_LOG."
        exit 1
    fi
    sleep 0.2
done

if [ "$ready" != 1 ]; then
    echo "[startx] Error: Xorg startup timed out."
    kill "$XORG_PID" 2>/dev/null
    exit 1
fi

# ── D-Bus Initialization ──────────────────────────────────────────────────
if [ ! -s /etc/machine-id ]; then
    if command -v dbus-uuidgen >/dev/null 2>&1; then
        dbus-uuidgen --ensure=/etc/machine-id 2>/dev/null || true
    else
        echo "10000000000000000000000000000001" > /etc/machine-id
    fi
fi
mkdir -p /var/lib/dbus /run/dbus /var/run
[ -e /var/run/dbus ] || ln -sf /run/dbus /var/run/dbus 2>/dev/null || true
[ -f /var/lib/dbus/machine-id ] || cp -f /etc/machine-id /var/lib/dbus/machine-id 2>/dev/null || true

if command -v dbus-daemon >/dev/null 2>&1 && [ ! -S /run/dbus/system_bus_socket ]; then
    dbus-daemon --system --fork 2>/dev/null || true
fi
export DBUS_SYSTEM_BUS_ADDRESS=unix:path=/var/run/dbus/system_bus_socket

if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ] && command -v dbus-daemon >/dev/null 2>&1; then
    DBUS_SESSION_BUS_SOCKET=/tmp/ascent-session-bus
    rm -f "$DBUS_SESSION_BUS_SOCKET"
    DBUS_SESSION_BUS_ADDRESS="unix:path=${DBUS_SESSION_BUS_SOCKET}"
    dbus-daemon --session --nofork --address="$DBUS_SESSION_BUS_ADDRESS" >/tmp/ascent-dbus.log 2>&1 &
    export DBUS_SESSION_BUS_ADDRESS
fi

# ── XFCE4 Session ─────────────────────────────────────────────────────────
if [ "${ASCENT_SESSION:-}" = "xfce4" ]; then
    echo "[startx] Starting XFCE4 session..."
    export XDG_SESSION_TYPE=x11
    export XDG_CURRENT_DESKTOP=XFCE
    export XDG_CONFIG_HOME="${HOME}/.config"
    export XDG_DATA_HOME="${HOME}/.local/share"
    export XDG_CACHE_HOME="${HOME}/.cache"
    export GDK_GL=disable
    export LIBGL_DRI3_DISABLE=1
    export NO_AT_BRIDGE=1
    export GTK_A11Y=none
    export GIO_USE_VFS=local
    export GTK_USE_PORTAL=0
    unset SESSION_MANAGER

    mkdir -p "${XDG_CONFIG_HOME}" "${XDG_DATA_HOME}" "${XDG_CACHE_HOME}" "${HOME}/Desktop"

    if [ -x /usr/bin/startxfce4 ]; then
        exec /usr/bin/startxfce4
    fi

    # Fallback manual XFCE component startup
    [ -x /usr/lib/xfce4/xfconf/xfconfd ] && /usr/lib/xfce4/xfconf/xfconfd &
    xfwm4 --replace &
    XFWM_PID=$!
    xfsettingsd &
    xfdesktop &
    xfce4-panel &

    wait "$XFWM_PID"
    exit 0
fi

# ── Default: IceWM Session ────────────────────────────────────────────────
echo "[startx] Starting IceWM session..."
export NO_AT_BRIDGE=1
mkdir -p "${HOME}/.icewm"
if [ -d /etc/icewm ]; then
    cp -n /etc/icewm/* "${HOME}/.icewm/" 2>/dev/null || true
fi
export ICEWM_PRIVCFG="${HOME}/.icewm"

# Start IceWM Window Manager
if command -v icewm >/dev/null 2>&1; then
    icewm &
fi

# Set wallpaper
if [ -f /assets/room.png ] && command -v feh >/dev/null 2>&1; then
    feh --bg-fill /assets/room.png &
elif command -v xsetroot >/dev/null 2>&1; then
    xsetroot -solid "#1e1e2e" &
fi

# Start xclock
if command -v xclock >/dev/null 2>&1; then
    xclock &
fi

# Start Terminal (st)
if command -v st >/dev/null 2>&1; then
    st -T "st" -e /bin/bash &
fi

echo "[startx] IceWM desktop ready."
wait
