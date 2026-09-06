#!/bin/sh
# LightDM Display Manager startup script for AvoryOS
# Spawns system D-Bus, elogind (for seat management), and LightDM (autologin, no greeter).

export PATH="/usr/bin:/bin:/usr/local/bin:/opt/coreutils/bin:/opt/bash/bin:${PATH}"
export LD_LIBRARY_PATH="/usr/lib:/lib:/usr/local/lib:${LD_LIBRARY_PATH:-}"
export NO_AT_BRIDGE=1
export GTK_A11Y=none

rm -f /tmp/.X0-lock /tmp/.X11-unix/X0

mkdir -p /var/log /var/lib/dbus /run/dbus /var/run \
         /run/lightdm /var/lib/lightdm /var/log/lightdm /var/cache/lightdm \
         /run/systemd /run/user /var/lib/lightdm-data
[ -e /var/run/dbus ] || ln -sf /run/dbus /var/run/dbus 2>/dev/null || true
touch /var/run/utmp /var/log/wtmp /var/log/lastlog /run/utmp 2>/dev/null || true
chmod 0664 /var/run/utmp /var/log/wtmp /var/log/lastlog /run/utmp 2>/dev/null || true

# ── Dynamic account provisioning ──────────────────────────────────────────
if ! grep -q "^lightdm:" /etc/group 2>/dev/null; then
    echo "lightdm:x:620:" >> /etc/group
fi
if ! grep -q "^lightdm:" /etc/passwd 2>/dev/null; then
    echo "lightdm:!:620:620:LightDM daemon:/var/lib/lightdm:/sbin/nologin" >> /etc/passwd
fi
chown -R 620:620 /var/lib/lightdm /run/lightdm /var/log/lightdm /var/cache/lightdm /var/lib/lightdm-data 2>/dev/null || true
chmod 0777 /var/lib/lightdm /var/cache/lightdm /var/lib/lightdm-data 2>/dev/null || true

# ── D-Bus Initialization ──────────────────────────────────────────────────
if [ ! -s /etc/machine-id ]; then
    if command -v dbus-uuidgen >/dev/null 2>&1; then
        dbus-uuidgen --ensure=/etc/machine-id 2>/dev/null || true
    else
        echo "10000000000000000000000000000001" > /etc/machine-id
    fi
fi
[ -f /var/lib/dbus/machine-id ] || cp -f /etc/machine-id /var/lib/dbus/machine-id 2>/dev/null || true

if command -v dbus-daemon >/dev/null 2>&1 && [ ! -S /run/dbus/system_bus_socket ]; then
    rm -f /tmp/ascent-system-dbus.log
    dbus-daemon --system --nofork >/tmp/ascent-system-dbus.log 2>&1 &
    SYSTEM_DBUS_PID=$!
    sleep 0.2
    if [ ! -S /run/dbus/system_bus_socket ] || ! kill -0 "$SYSTEM_DBUS_PID" 2>/dev/null; then
        echo "[startx] Warning: system D-Bus did not stay running."
        [ -s /tmp/ascent-system-dbus.log ] && cat /tmp/ascent-system-dbus.log
    fi
fi
export DBUS_SYSTEM_BUS_ADDRESS=unix:path=/var/run/dbus/system_bus_socket

if command -v dbus-daemon >/dev/null 2>&1; then
    rm -f /tmp/avory-session-bus
    dbus-daemon --session --fork --address=unix:path=/tmp/avory-session-bus 2>/dev/null || true
    export DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/avory-session-bus
fi

chmod 1777 /tmp /tmp/.X11-unix 2>/dev/null || true
chmod 0777 /run/lightdm /var/lib/lightdm /var/log/lightdm /var/cache/lightdm /var/lib/lightdm-data 2>/dev/null || true

# ── Launch LightDM ────────────────────────────────────────────────────────
if command -v lightdm >/dev/null 2>&1; then
    echo "[startx] Launching LightDM display manager..."

    # Configure LightDM with GTK greeter and disable logind seat probing
    cat > /etc/lightdm/lightdm.conf << 'LDMCFG'
[LightDM]
run-directory=/run/lightdm
start-default-seat=true
logind-load-seats=false
logind-check-graphical=false

[Seat:*]
type=local
greeter-session=lightdm-gtk-greeter
greeter-hide-users=false
user-session=xfce
xserver-command=/usr/libexec/Xorg -noreset -nolisten tcp -ac
session-wrapper=/etc/X11/xinit/Xsession
LDMCFG

    cat > /etc/lightdm/lightdm-gtk-greeter.conf << 'GREETERCFG'
[greeter]
at-spi-enabled=false
indicators=~host;~spacer;~clock;~power
theme-name=Adwaita
icon-theme-name=Adwaita
GREETERCFG

    # Ensure PAM services for LightDM greeter, login, and autologin are clean and working
    mkdir -p /etc/pam.d
    cat > /etc/pam.d/lightdm-greeter << 'PAMCFG'
#%PAM-1.0
auth      required  pam_permit.so
account   required  pam_permit.so
password  required  pam_deny.so
session   required  pam_env.so
session   required  pam_unix.so
-session  optional  pam_limits.so
PAMCFG

    cat > /etc/pam.d/lightdm << 'PAMCFG'
#%PAM-1.0
auth      required  pam_permit.so
account   required  pam_permit.so
password  required  pam_deny.so
session   required  pam_env.so
session   required  pam_unix.so
-session  optional  pam_limits.so
PAMCFG

    cat > /etc/pam.d/lightdm-autologin << 'PAMCFG'
#%PAM-1.0
auth      required  pam_permit.so
account   required  pam_permit.so
password  required  pam_deny.so
session   required  pam_env.so
session   required  pam_unix.so
-session  optional  pam_limits.so
PAMCFG

    lightdm --debug
    echo "=== /.xsession-errors ==="
    [ -f /.xsession-errors ] && cat /.xsession-errors
    [ -f /root/.xsession-errors ] && cat /root/.xsession-errors
    echo "=== /var/log/lightdm/lightdm.log ==="
    [ -f /var/log/lightdm/lightdm.log ] && cat /var/log/lightdm/lightdm.log
    echo "=== /var/log/lightdm/seat0-greeter.log ==="
    [ -f /var/log/lightdm/seat0-greeter.log ] && cat /var/log/lightdm/seat0-greeter.log
    echo "=== /var/log/lightdm/x-0.log ==="
    [ -f /var/log/lightdm/x-0.log ] && cat /var/log/lightdm/x-0.log
    exec /bin/sh
fi

echo "[startx] Error: lightdm binary not found."
exec /bin/sh