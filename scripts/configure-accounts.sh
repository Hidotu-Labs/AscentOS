#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(dirname "$SCRIPT_DIR")
ROOTFS_DIR=${1:?usage: configure-accounts.sh ROOTFS_DIR}

DEFAULT_SHELL="/bin/sh"
if [ -f "${ROOTFS_DIR}/opt/bash/bin/bash" ] || [ -f "${ROOT_DIR}/toolchain/glibc-sysroot/opt/bash/bin/bash" ]; then
    DEFAULT_SHELL="/bin/bash"
fi

mkdir -p "${ROOTFS_DIR}/etc" "${ROOTFS_DIR}/home" "${ROOTFS_DIR}/etc/skel"
sed -E "s/^([^:]+):x:/\1:!:/" "${ROOTFS_DIR}/etc/passwd" | \
    sed -E "s#^root:[^:]*:0:0:([^:]*):[^:]*:.*#root::0:0:\1:/:${DEFAULT_SHELL}#" > \
    "${ROOTFS_DIR}/etc/passwd.avory-new"
mv "${ROOTFS_DIR}/etc/passwd.avory-new" "${ROOTFS_DIR}/etc/passwd"
rm -f "${ROOTFS_DIR}/etc/shadow" "${ROOTFS_DIR}/etc/shadow-" \
      "${ROOTFS_DIR}/etc/gshadow" "${ROOTFS_DIR}/etc/gshadow-"
chmod 0644 "${ROOTFS_DIR}/etc/passwd" "${ROOTFS_DIR}/etc/group"
chmod 0755 "${ROOTFS_DIR}/home"
rm -rf "${ROOTFS_DIR}/root"
mkdir -p "${ROOTFS_DIR}/root/.config/alacritty" "${ROOTFS_DIR}/root/.cache"

if ! grep -q "^avory:" "${ROOTFS_DIR}/etc/group"; then
    echo "avory:x:1000:" >> "${ROOTFS_DIR}/etc/group"
fi
if ! grep -q "^avory:" "${ROOTFS_DIR}/etc/passwd"; then
    echo "avory::1000:1000:AvoryOS User:/home/avory:${DEFAULT_SHELL}" >> "${ROOTFS_DIR}/etc/passwd"
fi
if ! grep -q "^messagebus:" "${ROOTFS_DIR}/etc/group"; then
    echo "messagebus:x:86:" >> "${ROOTFS_DIR}/etc/group"
fi
if ! grep -q "^messagebus:" "${ROOTFS_DIR}/etc/passwd"; then
    echo "messagebus:!:86:86:D-Bus Message Bus User:/var/run/dbus:/bin/false" >> "${ROOTFS_DIR}/etc/passwd"
fi
if ! grep -q "^lightdm:" "${ROOTFS_DIR}/etc/group"; then
    echo "lightdm:x:620:" >> "${ROOTFS_DIR}/etc/group"
fi
if ! grep -q "^lightdm:" "${ROOTFS_DIR}/etc/passwd"; then
    echo "lightdm:!:620:620:LightDM daemon:/var/lib/lightdm:/sbin/nologin" >> "${ROOTFS_DIR}/etc/passwd"
fi
mkdir -p "${ROOTFS_DIR}/home/avory" "${ROOTFS_DIR}/var/lib/lightdm" "${ROOTFS_DIR}/var/log/lightdm" "${ROOTFS_DIR}/run/lightdm"
chown -R 620:620 "${ROOTFS_DIR}/var/lib/lightdm" "${ROOTFS_DIR}/var/log/lightdm" "${ROOTFS_DIR}/run/lightdm" 2>/dev/null || true
cp -a "${ROOTFS_DIR}/etc/skel/." "${ROOTFS_DIR}/home/avory/" 2>/dev/null || true
chmod 0700 "${ROOTFS_DIR}/home/avory"

mkdir -p "${ROOTFS_DIR}/usr/sbin" "${ROOTFS_DIR}/bin"
cp "${ROOT_DIR}/userland/avory-account" "${ROOTFS_DIR}/usr/sbin/avory-account"
chmod 0755 "${ROOTFS_DIR}/usr/sbin/avory-account"
for command in useradd userdel usermod groupadd groupdel groupmod; do
    ln -sf /usr/sbin/avory-account "${ROOTFS_DIR}/usr/sbin/$command"
    ln -sf /usr/sbin/avory-account "${ROOTFS_DIR}/bin/$command"
done
cp "${ROOT_DIR}/userland/test_accounts.sh" "${ROOTFS_DIR}/bin/test_accounts"
chmod 0755 "${ROOTFS_DIR}/bin/test_accounts"
cp "${ROOT_DIR}/userland/avory-login.elf" "${ROOTFS_DIR}/bin/avory-login"
chmod 0755 "${ROOTFS_DIR}/bin/avory-login"

cat > "${ROOTFS_DIR}/etc/login.defs" <<EOF
UID_MIN 1000
UID_MAX 60000
GID_MIN 1000
GID_MAX 60000
CREATE_HOME yes
HOME_MODE 0700
UMASK 077
EOF
