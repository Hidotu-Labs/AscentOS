#!/bin/sh
# Xorg modesetting/DRM ile X server başlat ve IceWM + xclock + st çalıştır
# Alpine package integration via setup-alpine.sh provides additional userland tools
#
# Set ASCENT_SESSION=xfce4 to launch XFCE4 instead of IceWM.

rm -f "/tmp/.X0-lock"
rm -f "/tmp/.X11-unix/X0"


# DISPLAY ortam değişkenini ayarla
export DISPLAY=:0
: "${HOME:=/}"
export HOME
export XAUTHORITY="$HOME/.Xauthority"
uid=$(id -u)
XORG_LOG="$HOME/Xorg.log"
# The modesetting DDX uses the AscentOS KMS device at /dev/dri/card0.
XORG="${XORG:-/usr/libexec/Xorg}"
if [ ! -x "$XORG" ]; then
    XORG=/usr/bin/Xorg
fi
if [ ! -x "$XORG" ]; then
    echo "Hata: Xorg bulunamadı. scripts/setup-alpine.sh çalıştırın."
    exit 1
fi

"$XORG" "$DISPLAY" -retro -noreset -nolisten tcp \
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
        echo "Hata: Xorg başlatılamadı; $XORG_LOG dosyasına bakın."
        exit 1
    fi
    sleep 1
done
if [ "$ready" != 1 ]; then
    echo "Hata: Xorg zaman aşımına uğradı; $XORG_LOG dosyasına bakın."
    kill "$XORG_PID" 2>/dev/null
    exit 1
fi

# ── XFCE4 session ────────────────────────────────────────────────────────
if [ "${ASCENT_SESSION:-}" = "xfce4" ]; then
    echo "[startx] Starting XFCE4 session..."
    export XDG_SESSION_TYPE=x11
    export XDG_CURRENT_DESKTOP=XFCE
    export XDG_CONFIG_HOME="${HOME}/.config"
    export XDG_DATA_HOME="${HOME}/.local/share"
    export XDG_CACHE_HOME="${HOME}/.cache"
    mkdir -p "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_CACHE_HOME"
    export PATH=/opt/coreutils/bin:/opt/bash/bin:/bin:/usr/local/bin:/usr/bin:/opt/tcc/bin:$PATH
    export LD_LIBRARY_PATH=/usr/lib:/lib:/usr/local/lib:$LD_LIBRARY_PATH

    # Copy default xfce4 config on first run
    if [ ! -d "$XDG_CONFIG_HOME/xfce4" ] && [ -d /etc/xdg/xfce4 ]; then
        cp -r /etc/xdg/xfce4 "$XDG_CONFIG_HOME/xfce4"
    fi

    # Load X resources if available
    if command -v xrdb >/dev/null 2>&1; then
        xrdb -merge "$HOME/.Xresources" 2>/dev/null || true
    fi

    # Start D-Bus session daemon then launch xfce4-session directly.
    # We do NOT call startxfce4 — it tries to spawn a second Xorg.
    if command -v dbus-run-session >/dev/null 2>&1; then
        exec dbus-run-session -- xfce4-session
    elif command -v dbus-launch >/dev/null 2>&1; then
        eval "$(dbus-launch --sh-syntax --exit-with-session)"
        exec xfce4-session
    else
        exec xfce4-session
    fi
    # fallback: keep Xorg alive
    wait "$XORG_PID"
    exit $?
fi

# ── Default: IceWM session (original behaviour) ───────────────────────────

# IceWM configuration setup
: "${HOME:=/}"
export HOME
export XAUTHORITY="$HOME/.Xauthority"
mkdir -p "$HOME/.icewm"
if [ -d /etc/icewm ]; then
    cp -f /etc/icewm/icewmrc "$HOME/.icewm/icewmrc" 2>/dev/null || true
    cp -f /etc/icewm/winoptions "$HOME/.icewm/winoptions" 2>/dev/null || true
fi
export ICEWM_PRIVCFG="$HOME/.icewm"

# IceWM (Pencere yöneticisi önce başlatılmalı)
if [ -x /usr/bin/icewm ]; then
    echo "IceWM (Alpine) başlatılıyor..."
    /usr/bin/icewm &
elif command -v icewm >/dev/null 2>&1; then
    echo "IceWM (sistem yolu ile) başlatılıyor..."
    icewm &
else
    echo "Uyarı: IceWM bulunamadı!"
fi

# Set background image
if [ -f /assets/room.png ]; then
    if command -v feh >/dev/null 2>&1; then
        echo "Setting background with feh..."
        feh --bg-fill /assets/room.png &
    elif command -v xsetroot >/dev/null 2>&1; then
        echo "Setting background with xsetroot..."
        xsetroot -solid gray &
    fi
fi

# xclock
if [ -x /usr/bin/xclock ]; then
    echo "xclock başlatılıyor..."
    /usr/bin/xclock &
elif command -v xclock >/dev/null 2>&1; then
    echo "xclock (sistem yolu ile) başlatılıyor..."
    xclock &
fi

sleep 2
export PATH=/opt/coreutils/bin:/opt/bash/bin:/bin:/usr/local/bin:/usr/bin:/opt/tcc/bin:$PATH
export LD_LIBRARY_PATH=/usr/lib:/lib:/usr/local/lib:$LD_LIBRARY_PATH

# st (suckless terminal from Alpine)
# Set PS1/PATH in environment so bash picks them up even without rcfile
export PATH=/opt/coreutils/bin:/opt/bash/bin:/bin:/usr/local/bin:/usr/bin:/opt/tcc/bin
export BASH_ENV="$HOME/.bashrc"
ST_SHELL="/bin/bash"
ST_RCFILE="$HOME/.bashrc"
if [ -x /usr/bin/st ]; then
    echo "st (Alpine) başlatılıyor..."
    /usr/bin/st -T "st" -e "$ST_SHELL" --noprofile --rcfile "$ST_RCFILE" &
elif [ -x /bin/st ]; then
    echo "st başlatılıyor..."
    /bin/st -T "st" -e "$ST_SHELL" --noprofile --rcfile "$ST_RCFILE" &
elif command -v st >/dev/null 2>&1; then
    echo "st (sistem yolu ile) başlatılıyor..."
    st -T "st" -e "$ST_SHELL" --noprofile --rcfile "$ST_RCFILE" &
else
    echo "Uyarı: st (Alpine) bulunamadı!"
fi

echo "Tamamlandı. IceWM, xclock ve st çalıştırıldı."
echo "Not: Alpine paketleri kullanmak için scripts/setup-alpine.sh çalıştırın."
