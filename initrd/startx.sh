#!/bin/sh
# Xorg + Window Manager startup script for AvoryOS
# Supports IceWM (default) and XFCE4 (ASCENT_SESSION=xfce4)

rm -f /tmp/.X0-lock /tmp/.X11-unix/X0

export DISPLAY=:0
: "${HOME:=/}"
export HOME
export XAUTHORITY="${HOME}/.Xauthority"

export PATH="/opt/coreutils/bin:/opt/bash/bin:/bin:/usr/local/bin:/usr/bin:/opt/tcc/bin:${PATH}"
export LD_LIBRARY_PATH="/usr/lib:/lib:/usr/local/lib:${LD_LIBRARY_PATH:-}"

XORG_LOG="/var/log/Xorg.0.log"
mkdir -p /var/log

XORG=/usr/libexec/Xorg
[ ! -x "$XORG" ] && XORG=/usr/bin/Xorg

if [ ! -x "$XORG" ]; then
    echo "[startx] Error: Xorg binary not found."
    exit 1
fi

"$XORG" "$DISPLAY" -noreset -nolisten tcp -logfile "$XORG_LOG" &
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

# ── XFCE4 Session ─────────────────────────────────────────────────────────
if [ "${ASCENT_SESSION:-}" = "xfce4" ]; then
    echo "[startx] Starting XFCE4 session..."

    # ── D-Bus Initialization for XFCE ─────────────────────────────────────
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
        rm -f /tmp/ascent-system-dbus.log
        dbus-daemon --system --nofork >/tmp/ascent-system-dbus.log 2>&1 &
        SYSTEM_DBUS_PID=$!
        sleep 0.1
        if [ ! -S /run/dbus/system_bus_socket ] ||
           ! kill -0 "$SYSTEM_DBUS_PID" 2>/dev/null; then
            echo "[startx] Warning: system D-Bus did not stay running."
            [ -s /tmp/ascent-system-dbus.log ] && cat /tmp/ascent-system-dbus.log
        fi
    fi
    export DBUS_SYSTEM_BUS_ADDRESS=unix:path=/var/run/dbus/system_bus_socket

    if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ] && command -v dbus-daemon >/dev/null 2>&1; then
        DBUS_SESSION_BUS_SOCKET=/tmp/ascent-session-bus
        rm -f "$DBUS_SESSION_BUS_SOCKET"
        DBUS_SESSION_BUS_ADDRESS="unix:path=${DBUS_SESSION_BUS_SOCKET}"
        dbus-daemon --session --nofork --address="$DBUS_SESSION_BUS_ADDRESS" >/tmp/ascent-dbus.log 2>&1 &
        export DBUS_SESSION_BUS_ADDRESS

        for _ in 1 2 3 4 5 6 7 8 9 10; do
            [ -S "$DBUS_SESSION_BUS_SOCKET" ] && break
            sleep 0.1
        done
        if [ ! -S "$DBUS_SESSION_BUS_SOCKET" ]; then
            echo "[startx] Error: session D-Bus failed to create its socket."
            [ -s /tmp/ascent-dbus.log ] && cat /tmp/ascent-dbus.log
            exit 1
        fi
    fi
    export XDG_SESSION_TYPE=x11
    export XDG_CURRENT_DESKTOP=XFCE
    export XDG_SESSION_DESKTOP=xfce
    export DESKTOP_SESSION=xfce
    # Never inherit a partial XDG search path from the boot shell.  XFCE
    # resolves its failsafe session and xfconf defaults from /etc/xdg.
    export XDG_CONFIG_DIRS=/etc/xdg
    export XDG_DATA_DIRS=/usr/local/share:/usr/share
    export XDG_CONFIG_HOME="${HOME}/.config"
    export XDG_DATA_HOME="${HOME}/.local/share"
    export XDG_CACHE_HOME="${HOME}/.cache"
    unset SESSION_MANAGER

    mkdir -p "${XDG_CONFIG_HOME}/xfce4/xfconf/xfce-perchannel-xml" \
             "${XDG_DATA_HOME}" "${XDG_CACHE_HOME}" "${HOME}/Desktop" \
             "${HOME}/Templates" "${HOME}/Downloads" "${HOME}/Documents" \
             "${HOME}/Pictures" "${HOME}/Music" "${HOME}/Videos"

    # Run the known-good component session directly.  startxfce4 delegates to
    # xfce4-session, whose failsafe-session discovery depends on service
    # activation that is not yet reliable on AvoryOS.
    XFCONFD=/usr/lib/xfce4/xfconf/xfconfd
    if [ -x "$XFCONFD" ]; then
        "$XFCONFD" &
        XFCONFD_PID=$!
        sleep 0.1
    fi

    xfwm4 --replace >/tmp/xfwm4.log 2>&1 &
    XFWM_PID=$!
    sleep 0.2
    xfsettingsd &
    XFSETTINGS_PID=$!
    sleep 0.5
    xfdesktop &
    XFDESKTOP_PID=$!
    xfce4-panel &
    XFPANEL_PID=$!

    wait "$XFWM_PID"
    status=$?
    kill "$XFSETTINGS_PID" "$XFDESKTOP_PID" "$XFPANEL_PID" \
         "${XFCONFD_PID:-}" 2>/dev/null || true
    exit "$status"
fi

# ── Default: IceWM Session ────────────────────────────────────────────────
echo "[startx] Starting IceWM session..."
export XDG_SESSION_TYPE=x11
export XDG_CURRENT_DESKTOP=IceWM
export XDG_SESSION_DESKTOP=icewm
export DESKTOP_SESSION=icewm
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
