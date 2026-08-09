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

# D-Bus is required by VLC/Qt and many GTK apps. Bootstrap it once for every
# desktop session (IceWM and XFCE), not only the XFCE code path.
if [ ! -s /etc/machine-id ]; then
    if command -v dbus-uuidgen >/dev/null 2>&1; then
        dbus-uuidgen --ensure=/etc/machine-id 2>/dev/null || true
    else
        echo "10000000000000000000000000000001" > /etc/machine-id
    fi
fi
mkdir -p /var/lib/dbus /run/dbus
[ -L /var/run ] || [ -d /var/run ] || mkdir -p /var/run
[ -e /var/run/dbus ] || ln -sf /run/dbus /var/run/dbus 2>/dev/null || true
[ -f /var/lib/dbus/machine-id ] || cp -f /etc/machine-id /var/lib/dbus/machine-id 2>/dev/null || true
if command -v dbus-daemon >/dev/null 2>&1 && [ ! -S /run/dbus/system_bus_socket ]; then
    dbus-daemon --system --fork 2>/dev/null || true
fi
DBUS_SYSTEM_BUS_ADDRESS=unix:path=/var/run/dbus/system_bus_socket
export DBUS_SYSTEM_BUS_ADDRESS

if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ] &&
   command -v dbus-daemon >/dev/null 2>&1; then
    DBUS_SESSION_BUS_SOCKET=/tmp/ascent-session-bus
    rm -f "$DBUS_SESSION_BUS_SOCKET"
    DBUS_SESSION_BUS_ADDRESS=unix:path=$DBUS_SESSION_BUS_SOCKET
    dbus-daemon --session --nofork \
        --address="$DBUS_SESSION_BUS_ADDRESS" >/tmp/ascent-dbus.log 2>&1 &
    DBUS_SESSION_BUS_PID=$!
    sleep 0.1
    if ! kill -0 "$DBUS_SESSION_BUS_PID" 2>/dev/null; then
        echo "[startx] session D-Bus daemon failed to initialize"
        exit 1
    fi
    export DBUS_SESSION_BUS_ADDRESS
fi
if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]; then
    echo "[startx] unable to start D-Bus session bus"
    exit 1
fi

# ── XFCE4 session ────────────────────────────────────────────────────────
if [ "${ASCENT_SESSION:-}" = "xfce4" ]; then
    echo "[startx] Starting XFCE4 component session..."
    export XDG_SESSION_TYPE=x11
    export XDG_CURRENT_DESKTOP=XFCE
    export XCURSOR_THEME=Adwaita
    export XCURSOR_SIZE=24
    export XDG_CONFIG_HOME="${HOME}/.config"
    export XDG_DATA_HOME="${HOME}/.local/share"
    export XDG_CACHE_HOME="${HOME}/.cache"
    export GDK_GL=disable
    export LIBGL_DRI3_DISABLE=1
    # AscentOS does not run the optional AT-SPI accessibility bus. GTK waits
    # for its D-Bus reply during startup unless the bridge is disabled.
    export NO_AT_BRIDGE=1
    export GTK_A11Y=none
    # Keep desktop file access local; remote GVfs backends are unnecessary for
    # the base desktop and otherwise activate more session-bus services.
    export GIO_USE_VFS=local
    export GIO_USE_VOLUME_MONITOR=unix
    export GTK_USE_PORTAL=0
    unset SESSION_MANAGER
    export PATH=/opt/coreutils/bin:/opt/bash/bin:/bin:/usr/local/bin:/usr/bin:/opt/tcc/bin:$PATH
    export LD_LIBRARY_PATH=/usr/lib:/lib:/usr/local/lib:${LD_LIBRARY_PATH:-}
    mkdir -p "$XDG_CONFIG_HOME/xfce4/xfconf/xfce-perchannel-xml" \
             "$XDG_DATA_HOME" "$XDG_CACHE_HOME" "$HOME/Desktop" \
             "$HOME/Templates" "$HOME/Downloads" "$HOME/Documents" \
             "$HOME/Pictures" "$HOME/Music" "$HOME/Videos" \
             "$XDG_CONFIG_HOME/gtk-3.0"

    cat > "$XDG_CONFIG_HOME/user-dirs.dirs" << 'USER_DIRS_EOF'
XDG_DESKTOP_DIR="$HOME/Desktop"
XDG_DOWNLOAD_DIR="$HOME/Downloads"
XDG_TEMPLATES_DIR="$HOME/Templates"
XDG_PUBLICSHARE_DIR="$HOME/Public"
XDG_DOCUMENTS_DIR="$HOME/Documents"
XDG_MUSIC_DIR="$HOME/Music"
XDG_PICTURES_DIR="$HOME/Pictures"
XDG_VIDEOS_DIR="$HOME/Videos"
USER_DIRS_EOF

    # Clear stale icon/thumbnail caches and single-instance IPC sockets from previous boots.
    rm -rf "$XDG_CACHE_HOME/icon-*" \
           "$XDG_CACHE_HOME/icons" \
           "$XDG_CACHE_HOME/xfce4" \
           "$XDG_CACHE_HOME/thumbnails" \
           "$XDG_CACHE_HOME"/*-socket* \
           "$XDG_CACHE_HOME"/pcmanfm* \
           "$XDG_CACHE_HOME"/Thunar* \
           /tmp/.*-lock /tmp/*-socket*
    if [ -f /etc/xdg/gtk-3.0/gtk.css ]; then
        cp -f /etc/xdg/gtk-3.0/gtk.css \
            "$XDG_CONFIG_HOME/gtk-3.0/gtk.css"
    fi

    # Refresh the AscentOS desktop defaults. The home directory is persistent,
    # so otherwise an older one-panel/no-backdrop configuration wins.
    # Wipe the entire xfce4 config dir to prevent stale xfconfd state/launcher
    # caches from corrupting icon resolution on subsequent boots.
    rm -rf "$XDG_CONFIG_HOME/xfce4"
    mkdir -p "$XDG_CONFIG_HOME/xfce4/xfconf/xfce-perchannel-xml"
    for channel in xfwm4 xfce4-panel xfce4-desktop xsettings; do
        src="/etc/xdg/xfce4/xfconf/xfce-perchannel-xml/$channel.xml"
        if [ -f "$src" ]; then
            cp -f "$src" \
                "$XDG_CONFIG_HOME/xfce4/xfconf/xfce-perchannel-xml/$channel.xml"
        fi
    done
    # Minimalist XFCE setup configuration: single clean top bar, dark theme, disabled desktop icons
    cat > "$XDG_CONFIG_HOME/xfce4/xfconf/xfce-perchannel-xml/xfwm4.xml" << 'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<channel name="xfwm4" version="1.0">
  <property name="general" type="empty">
    <property name="theme" type="string" value="Default-dark"/>
    <property name="use_compositing" type="bool" value="true"/>
    <property name="unredirect_overlays" type="bool" value="false"/>
    <property name="box_move" type="bool" value="false"/>
    <property name="box_resize" type="bool" value="false"/>
    <property name="vblank_mode" type="string" value="off"/>
    <property name="sync_to_vblank" type="bool" value="false"/>
  </property>
</channel>
EOF

    cat > "$XDG_CONFIG_HOME/xfce4/xfconf/xfce-perchannel-xml/xfce4-panel.xml" << 'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<channel name="xfce4-panel" version="1.0">
  <property name="panels" type="array">
    <value type="int" value="1"/>
    <property name="panel-1" type="empty">
      <property name="position" type="string" value="p=6;x=0;y=0"/>
      <property name="length" type="uint" value="100"/>
      <property name="position-locked" type="bool" value="true"/>
      <property name="size" type="uint" value="26"/>
      <property name="plugin-ids" type="array">
        <value type="int" value="1"/>
        <value type="int" value="2"/>
        <value type="int" value="3"/>
        <value type="int" value="4"/>
        <value type="int" value="5"/>
      </property>
    </property>
  </property>
  <property name="plugins" type="empty">
    <property name="plugin-1" type="string" value="applicationsmenu"/>
    <property name="plugin-2" type="string" value="tasklist">
      <property name="flat-buttons" type="bool" value="true"/>
      <property name="show-labels" type="bool" value="true"/>
    </property>
    <property name="plugin-3" type="string" value="separator">
      <property name="expand" type="bool" value="true"/>
      <property name="style" type="uint" value="0"/>
    </property>
    <property name="plugin-4" type="string" value="systray"/>
    <property name="plugin-5" type="string" value="clock">
      <property name="digital-format" type="string" value="%H:%M"/>
    </property>
  </property>
</channel>
EOF

    cat > "$XDG_CONFIG_HOME/xfce4/xfconf/xfce-perchannel-xml/xfce4-desktop.xml" << 'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<channel name="xfce4-desktop" version="1.0">
  <property name="desktop-icons" type="empty">
    <property name="style" type="int" value="0"/>
  </property>
  <property name="backdrop" type="empty">
    <property name="screen0" type="empty">
      <property name="monitor0" type="empty">
        <property name="workspace0" type="empty">
          <property name="color-style" type="int" value="0"/>
          <property name="rgba1" type="array">
            <value type="double" value="0.12"/>
            <value type="double" value="0.13"/>
            <value type="double" value="0.15"/>
            <value type="double" value="1.0"/>
          </property>
          <property name="image-style" type="int" value="0"/>
        </property>
      </property>
    </property>
  </property>
</channel>
EOF

    cat > "$XDG_CONFIG_HOME/xfce4/xfconf/xfce-perchannel-xml/xsettings.xml" << 'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<channel name="xsettings" version="1.0">
  <property name="Net" type="empty">
    <property name="ThemeName" type="string" value="Adwaita-dark"/>
    <property name="IconThemeName" type="string" value="Adwaita"/>
  </property>
  <property name="Gtk" type="empty">
    <property name="CursorThemeName" type="string" value="Adwaita"/>
    <property name="CursorThemeSize" type="int" value="24"/>
  </property>
</channel>
EOF

    # Prevent PulseAudio client library from autospawning pulseaudio and blocking cmus/GTK apps
    mkdir -p /etc/pulse "$HOME/.config/pulse"
    cat > /etc/pulse/client.conf << 'PULSE_EOF'
autospawn = no
disable-shm = yes
PULSE_EOF

    # Configure cmus default audio plugin to oss
    mkdir -p "$HOME/.config/cmus"
    if [ ! -f "$HOME/.config/cmus/rc" ]; then
        cat > "$HOME/.config/cmus/rc" << 'CMUS_EOF'
set output_plugin=oss
set dsp.device=/dev/dsp
CMUS_EOF
    else
        sed -i 's/set output_plugin=dummy/set output_plugin=oss/g;s/set output_plugin=alsa/set output_plugin=oss/g' "$HOME/.config/cmus/rc" 2>/dev/null || true
        grep -q "dsp.device" "$HOME/.config/cmus/rc" || echo "set dsp.device=/dev/dsp" >> "$HOME/.config/cmus/rc"
    fi
    if [ ! -f "$HOME/.config/cmus/autosave" ]; then
        cat > "$HOME/.config/cmus/autosave" << 'CMUS_EOF'
set output_plugin=oss
set dsp.device=/dev/dsp
CMUS_EOF
    else
        sed -i 's/set output_plugin=dummy/set output_plugin=oss/g;s/set output_plugin=alsa/set output_plugin=oss/g' "$HOME/.config/cmus/autosave" 2>/dev/null || true
        grep -q "dsp.device" "$HOME/.config/cmus/autosave" || echo "set dsp.device=/dev/dsp" >> "$HOME/.config/cmus/autosave"
    fi

    if command -v xrdb >/dev/null 2>&1; then
        xrdb -merge "$HOME/.Xresources" 2>/dev/null || true
    fi

    XFCONFD=/usr/lib/xfce4/xfconf/xfconfd
    if [ -x "$XFCONFD" ]; then
        # Ensure xfconf channel files are writable by xfconfd to prevent D-Bus timeouts
        chmod -R u+w "$XDG_CONFIG_HOME/xfce4/xfconf/xfce-perchannel-xml" \
            2>/dev/null || true
        "$XFCONFD" &
        XFCONFD_PID=$!
        sleep 0.1
        if ! kill -0 "$XFCONFD_PID" 2>/dev/null; then
            echo "[startx] xfconfd failed to initialize"
            exit 1
        fi
    fi

    XFWM_LOG=/tmp/xfwm4.log
    rm -f "$XFWM_LOG"
    xfwm4 --replace >"$XFWM_LOG" 2>&1 &
    XFWM_PID=$!
    sleep 0.2
    if ! kill -0 "$XFWM_PID" 2>/dev/null; then
        echo "[startx] xfwm4 failed to initialize"
        wait "$XFWM_PID"
        status=$?
        echo "[startx] --- xfwm4 output ---"
        cat "$XFWM_LOG" 2>/dev/null || true
        exit "$status"
    fi

    xfsettingsd &
    XFSETTINGS_PID=$!
    # Give xfsettingsd time to apply IconThemeName=Adwaita via XSETTINGS before
    # xfce4-panel starts resolving Icon= names. Without this delay the panel
    # reads icons before the theme is active and caches a broken lookup.
    sleep 0.5
    xfdesktop &
    XFDESKTOP_PID=$!
    xfce4-panel &
    XFPANEL_PID=$!

    wait "$XFWM_PID"
    status=$?
    if [ "$status" -ne 0 ]; then
        echo "[startx] xfwm4 exited with status $status"
        echo "[startx] --- xfwm4 output ---"
        cat "$XFWM_LOG" 2>/dev/null || true
    fi
    kill "$XFSETTINGS_PID" "$XFDESKTOP_PID" "$XFPANEL_PID" \
         "${XFCONFD_PID:-}" 2>/dev/null || true
    exit "$status"
fi

# ── Default: IceWM session (original behaviour) ───────────────────────────

export NO_AT_BRIDGE=1
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/runtime-ascent}"
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"

# IceWM configuration setup
: "${HOME:=/}"
export HOME
export XAUTHORITY="$HOME/.Xauthority"
mkdir -p "$HOME/.icewm"
if [ -d /etc/icewm ]; then
    cp -f /etc/icewm/icewmrc "$HOME/.icewm/icewmrc" 2>/dev/null || true
    cp -f /etc/icewm/winoptions "$HOME/.icewm/winoptions" 2>/dev/null || true
    cp -f /etc/icewm/menu "$HOME/.icewm/menu" 2>/dev/null || true
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
