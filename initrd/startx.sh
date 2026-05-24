#!/bin/sh
# Xfbdev ile X server başlat ve IceWM + xclock + st çalıştır
# Alpine package integration via setup-alpine.sh provides additional userland tools

echo "X server başlatılıyor..."


rm -f "/tmp/.X0-lock"

# X server'ı arka planda başlat
Xfbdev -retro -xkbdir /share/X11/xkb \
    -mouse evdev,,device=/dev/input/event1 \
    -keybd evdev,,device=/dev/input/event0 &

# X server'ın tam olarak başlaması için bekle
sleep 1

echo "X server hazır, uygulamalar başlatılıyor..."

# DISPLAY ortam değişkenini ayarla
export DISPLAY=:0

# IceWM configuration setup
export HOME=/root
mkdir -p /root/.icewm
if [ -d /etc/icewm ]; then
    cp -f /etc/icewm/icewmrc /root/.icewm/icewmrc 2>/dev/null || true
    cp -f /etc/icewm/winoptions /root/.icewm/winoptions 2>/dev/null || true
fi
export ICEWM_PRIVCFG=/root/.icewm

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

# IceWM'in tam olarak başlaması için daha uzun süre bekle
sleep 3

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
export PATH=/usr/bin:/usr/local/bin:/bin:$PATH
export LD_LIBRARY_PATH=/usr/lib:/lib:/usr/local/lib:$LD_LIBRARY_PATH

# st (suckless terminal from Alpine)
if [ -x /usr/bin/st ]; then
    echo "st (Alpine) başlatılıyor..."
    /usr/bin/st &
elif [ -x /bin/st ]; then
    echo "st başlatılıyor..."
    /bin/st &
elif command -v st >/dev/null 2>&1; then
    echo "st (sistem yolu ile) başlatılıyor..."
    st &
else
    echo "Uyarı: st (Alpine) bulunamadı!"
fi

echo "Tamamlandı. IceWM, xclock ve st çalıştırıldı."
echo "Not: Alpine paketleri kullanmak için scripts/setup-alpine.sh çalıştırın."