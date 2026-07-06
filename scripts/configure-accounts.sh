#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(dirname "$SCRIPT_DIR")
ROOTFS_DIR=${1:?usage: configure-accounts.sh ROOTFS_DIR}

mkdir -p "${ROOTFS_DIR}/etc" "${ROOTFS_DIR}/home" "${ROOTFS_DIR}/etc/skel"
sed -E "s/^([^:]+):x:/\1:!:/" "${ROOTFS_DIR}/etc/passwd" | \
    sed -E "s#^root:[^:]*:0:0:([^:]*):[^:]*:.*#root::0:0:\1:/:/bin/bash#" > \
    "${ROOTFS_DIR}/etc/passwd.ascent-new"
mv "${ROOTFS_DIR}/etc/passwd.ascent-new" "${ROOTFS_DIR}/etc/passwd"
rm -f "${ROOTFS_DIR}/etc/shadow" "${ROOTFS_DIR}/etc/shadow-" \
      "${ROOTFS_DIR}/etc/gshadow" "${ROOTFS_DIR}/etc/gshadow-"
chmod 0644 "${ROOTFS_DIR}/etc/passwd" "${ROOTFS_DIR}/etc/group"
chmod 0755 "${ROOTFS_DIR}/home"
rm -rf "${ROOTFS_DIR}/root"

if ! grep -q "^ascent:" "${ROOTFS_DIR}/etc/group"; then
    echo "ascent:x:1000:" >> "${ROOTFS_DIR}/etc/group"
fi
if ! grep -q "^ascent:" "${ROOTFS_DIR}/etc/passwd"; then
    echo "ascent::1000:1000:AscentOS User:/home/ascent:/bin/bash" >> "${ROOTFS_DIR}/etc/passwd"
fi
mkdir -p "${ROOTFS_DIR}/home/ascent"
cp -a "${ROOTFS_DIR}/etc/skel/." "${ROOTFS_DIR}/home/ascent/" 2>/dev/null || true
chmod 0700 "${ROOTFS_DIR}/home/ascent"

mkdir -p "${ROOTFS_DIR}/usr/sbin" "${ROOTFS_DIR}/bin"
cp "${ROOT_DIR}/userland/ascent-account" "${ROOTFS_DIR}/usr/sbin/ascent-account"
chmod 0755 "${ROOTFS_DIR}/usr/sbin/ascent-account"
for command in useradd userdel usermod groupadd groupdel groupmod; do
    ln -sf /usr/sbin/ascent-account "${ROOTFS_DIR}/usr/sbin/$command"
    ln -sf /usr/sbin/ascent-account "${ROOTFS_DIR}/bin/$command"
done
cp "${ROOT_DIR}/userland/test_accounts.sh" "${ROOTFS_DIR}/bin/test_accounts"
chmod 0755 "${ROOTFS_DIR}/bin/test_accounts"
cp "${ROOT_DIR}/userland/ascent-login.elf" "${ROOTFS_DIR}/bin/ascent-login"
chmod 0755 "${ROOTFS_DIR}/bin/ascent-login"

cat > "${ROOTFS_DIR}/etc/login.defs" <<EOF
UID_MIN 1000
UID_MAX 60000
GID_MIN 1000
GID_MAX 60000
CREATE_HOME yes
HOME_MODE 0700
UMASK 077
EOF
