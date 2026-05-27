#!/usr/bin/env bash
# scripts/setup-alpine.sh - Downloads and installs Alpine Linux rootfs into AscentOS disk image
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DISK_IMG="${ROOT_DIR}/disk.img"
BUILD_DIR="${ROOT_DIR}/build/alpine"
POPULATE_SCRIPT="${ROOT_DIR}/scripts/populate-ext2-dir.sh"

ALPINE_VERSION="3.21.0"
ALPINE_TARBALL="alpine-minirootfs-${ALPINE_VERSION}-x86_64.tar.gz"
ALPINE_URL="https://dl-cdn.alpinelinux.org/alpine/v3.21/releases/x86_64/${ALPINE_TARBALL}"

mkdir -p "${BUILD_DIR}"

# 1. Download Alpine rootfs if not present
if [ ! -f "${BUILD_DIR}/${ALPINE_TARBALL}" ]; then
    echo "[*] Downloading Alpine ${ALPINE_VERSION}..."
    curl -L "${ALPINE_URL}" -o "${BUILD_DIR}/${ALPINE_TARBALL}"
fi

# 2. Extract rootfs to a temporary location if not already present
ROOTFS_DIR="${BUILD_DIR}/rootfs"
if [ ! -d "${ROOTFS_DIR}/etc" ]; then
    echo "[*] extracting Alpine rootfs to ${ROOTFS_DIR}..."
    mkdir -p "${ROOTFS_DIR}"
    tar -xzf "${BUILD_DIR}/${ALPINE_TARBALL}" -C "${ROOTFS_DIR}"
fi

# 3. Helper to download and install Alpine packages manually
install_apk() {
    local PKG_NAME=$1
    local REPO=$2
    local BRANCH=${3:-"v3.21"}
    local PKG_MARKER="${ROOTFS_DIR}/etc/ascentos-pkg/${BRANCH}-${REPO}-${PKG_NAME}"
    
    if [ -f "${PKG_MARKER}" ]; then
        echo "[*] Package ${PKG_NAME} already installed, skipping."
        return 0
    fi

    echo "[*] Installing package: ${PKG_NAME} from ${REPO} (branch: ${BRANCH})..."
    
    # Escape dots and pluses in PKG_NAME for grep
    local ESCAPED_PKG_NAME=$(echo "${PKG_NAME}" | sed 's/\./\\./g;s/+/\\+/g')
    local APK_FILENAME=$(curl -sL "https://dl-cdn.alpinelinux.org/alpine/${BRANCH}/${REPO}/x86_64/" | grep -oP ">${ESCAPED_PKG_NAME}-[0-9][^<]*\.apk<" | sed 's/>//;s/<//' | sort -V | tail -n 1)
    
    # Fallback to a known version if the search fails
    if [ -z "${APK_FILENAME}" ] && [ "${PKG_NAME}" == "st" ]; then
        APK_FILENAME="st-0.9.2-r0.apk"
    fi
    
    if [ -z "${APK_FILENAME}" ]; then
        echo "[!] Could not find package ${PKG_NAME} in ${REPO} (${BRANCH})"
        return 1
    fi
    
    local APK_URL="https://dl-cdn.alpinelinux.org/alpine/${BRANCH}/${REPO}/x86_64/${APK_FILENAME}"
    
    if [ ! -f "${BUILD_DIR}/${APK_FILENAME}" ]; then
        echo "[*] Downloading ${APK_URL}..."
        curl -L "${APK_URL}" -o "${BUILD_DIR}/${APK_FILENAME}"
    fi
    
    # APK files are 3 concatenated gzip streams (signature + control + data).
    # tar --ignore-zeros -xz processes all streams in the concatenation.
    tar --ignore-zeros -xzf "${BUILD_DIR}/${APK_FILENAME}" -C "${ROOTFS_DIR}" --warning=no-unknown-keyword 2>/dev/null || true
    
    # Mark as installed
    mkdir -p "${ROOTFS_DIR}/etc/ascentos-pkg"
    touch "${PKG_MARKER}"
}

# Install st terminal and X11 utilities and their dependencies
install_apk "st" "community"
install_apk "feh" "community"
install_apk "xclock" "community" "edge"
install_apk "libxft" "main"
install_apk "fontconfig" "main"
install_apk "libxrender" "main"
install_apk "libexpat" "main"
install_apk "libuuid" "main"
install_apk "libpng" "main"
install_apk "freetype" "main"
install_apk "libx11" "main"
install_apk "libxcb" "main"
install_apk "libxau" "main"
install_apk "libxdmcp" "main"
install_apk "libbsd" "main"
install_apk "libmd" "main"
install_apk "ncurses-terminfo-base" "main"
install_apk "font-liberation" "main"
install_apk "libbz2" "main"
install_apk "brotli-libs" "main"
install_apk "zlib" "main"
install_apk "libxcursor" "main"
install_apk "libxfixes" "main"
install_apk "libxrender" "main"
install_apk "libxft" "main"
install_apk "fastfetch" "community"
install_apk "hwdata-pci" "main"

# IceWM window manager and dependencies
echo "[*] Installing IceWM window manager..."
install_apk "icewm" "community"
install_apk "libxinerama" "main"
install_apk "libxrandr" "main"
install_apk "libxpm" "main"
install_apk "libjpeg" "main"
install_apk "libpng" "main"
install_apk "libsm" "main"
install_apk "libice" "main"
install_apk "imlib2" "main"
install_apk "libstdc++" "main"

# X11 utilities and toolkit libraries (xclock, xterm, etc.)
echo "[*] Installing X11 utilities and toolkit libraries..."
install_apk "libxt" "main"
install_apk "libxmu" "main"
install_apk "libxaw" "main"
install_apk "libxext" "main"
install_apk "libxkbfile" "main"
install_apk "xeyes" "community" "edge"

# Minimal GTK (GTK 2.0) and core dependencies
echo "[*] Installing GTK 2.0 and core dependencies..."
install_apk "gtk+2.0" "community"
install_apk "glib" "main"
install_apk "pango" "main"
install_apk "libatk-1.0" "main"
install_apk "gdk-pixbuf" "main"
install_apk "cairo" "main"
install_apk "fribidi" "main"
install_apk "harfbuzz" "main"
install_apk "libx11" "main"
install_apk "libxext" "main"
install_apk "libxrender" "main"
install_apk "libxi" "main"
install_apk "libxcursor" "main"
install_apk "libxfixes" "main"
install_apk "fontconfig" "main"
install_apk "freetype" "main"
install_apk "libpng" "main"
install_apk "libexpat" "main"
install_apk "libuuid" "main"
install_apk "shared-mime-info" "main"
install_apk "pcre2" "main"
install_apk "libffi" "main"
install_apk "pixman" "main"
install_apk "libjpeg-turbo" "main"
install_apk "libmount" "main"
install_apk "libblkid" "main"
install_apk "libeconf" "main"
install_apk "libintl" "main"
install_apk "graphite2" "main"
install_apk "libxcomposite" "main"
install_apk "libxdamage" "main"
install_apk "gettext-libs" "main"
install_apk "libxrandr" "main"
install_apk "libxinerama" "main"
install_apk "util-linux" "main"
install_apk "libbz2" "main"
install_apk "brotli-libs" "main"
install_apk "zlib" "main"
install_apk "lua5.4" "main"
install_apk "lua5.4-libs" "main"
install_apk "readline" "main"
install_apk "libncursesw" "main"

# GTK 3.0 and its core dependencies
echo "[*] Installing GTK 3.0 and dependencies..."
install_apk "gtk+3.0" "main"
install_apk "libatk-bridge-2.0" "main"
install_apk "at-spi2-core" "main"
install_apk "dbus-libs" "main"
install_apk "cairo-gobject" "main"
install_apk "libepoxy" "main"
install_apk "adwaita-icon-theme" "community"
install_apk "iso-codes" "main"
install_apk "wayland" "main"
install_apk "wayland-libs-client" "main"
install_apk "wayland-libs-server" "main"
install_apk "wayland-libs-cursor" "main"
install_apk "wayland-libs-egl" "main"
install_apk "libxkbcommon" "main"
install_apk "wlroots" "community"
install_apk "wlroots-dev" "community"
install_apk "wayland-protocols" "main"
install_apk "wayland-dev" "main"
install_apk "libinput" "community"
install_apk "libinput-libs" "community"
install_apk "libwacom" "community"
install_apk "libevdev" "community"
install_apk "mtdev" "community"
install_apk "libxml2" "main"
install_apk "libdisplay-info" "community"
install_apk "eudev-libs" "main"
install_apk "libdrm" "main"
install_apk "pixman" "main"
install_apk "libseat" "community"
install_apk "libelogind" "community"
install_apk "libcap2" "main"
install_apk "libpciaccess" "main"
install_apk "gcompat" "main"
install_apk "libucontext" "main"
install_apk "musl-obstack" "main"
install_apk "libunwind" "main"
install_apk "libva" "main"
install_apk "xcb-util-wm" "community"
install_apk "xcb-util-image" "community"
install_apk "xcb-util-renderutil" "community"
install_apk "xcb-util" "main"
install_apk "seatd" "community"
install_apk "libxshmfence" "main"

install_apk "xkeyboard-config" "main"
install_apk "font-dejavu" "main"


# GTK2 Development headers (for host compilation)
echo "[*] Installing GTK 2.0 development packages..."
install_apk "gtk+2.0-dev" "community"
install_apk "glib-dev" "main"
install_apk "pango-dev" "main"
install_apk "harfbuzz-dev" "main"
install_apk "graphite2-dev" "main"
install_apk "libxcomposite-dev" "main"
install_apk "libxdamage-dev" "main"
install_apk "at-spi2-core-dev" "main"
install_apk "gdk-pixbuf-dev" "main"
install_apk "libjpeg-turbo-dev" "main"
install_apk "util-linux-dev" "main"
install_apk "libeconf-dev" "main"
install_apk "gettext-dev" "main"
install_apk "libxrandr-dev" "main"
install_apk "libxinerama-dev" "main"
install_apk "cairo-dev" "main"
install_apk "libx11-dev" "main"
install_apk "libxrender-dev" "main"
install_apk "fontconfig-dev" "main"
install_apk "freetype-dev" "main"
install_apk "libpng-dev" "main"
install_apk "zlib-dev" "main"
install_apk "libxext-dev" "main"
install_apk "libxi-dev" "main"
install_apk "libxcursor-dev" "main"
install_apk "libxfixes-dev" "main"
install_apk "xorgproto" "main"
install_apk "python3" "main"
install_apk "dbus-dev" "main"

# GTK 3.0 Development headers
echo "[*] Installing GTK 3.0 development packages..."
install_apk "gtk+3.0-dev" "main"
install_apk "at-spi2-core-dev" "main"
install_apk "libepoxy-dev" "main"
install_apk "wayland-dev" "main"
install_apk "libxkbcommon-dev" "main"
install_apk "libxcb-dev" "main"
install_apk "xcb-util-wm-dev" "community"
install_apk "xcb-util-image-dev" "community"
install_apk "xcb-util-renderutil-dev" "community"
install_apk "mesa-dev" "main"
install_apk "mesa" "main"

# Mousepad (Text Editor) + GTK Plumbing
install_apk "libpng" "main"
install_apk "librsvg" "community"
install_apk "shared-mime-info" "main"
install_apk "adwaita-icon-theme" "community"
install_apk "gsettings-desktop-schemas" "community"
install_apk "gettext-libs" "main"
install_apk "gtksourceview" "community"
install_apk "mousepad" "community"
install_apk "gspell" "community"
install_apk "libxfce4ui" "community"

# PCManFM (Lightweight File Manager)
echo "[*] Installing PCManFM file manager and dependencies..."
install_apk "menu-cache" "community"
install_apk "pcmanfm" "community"
install_apk "libfm" "community"
install_apk "libfm-extra" "community"
install_apk "libexif" "community"
install_apk "tumbler" "community"
install_apk "gvfs" "community"
install_apk "gvfs-archive" "community"
install_apk "gvfs-fuse" "community"
install_apk "file" "main"

# NetSurf Web Browser
echo "[*] Installing NetSurf and dependencies..."
install_apk "netsurf" "community" "edge"
install_apk "duktape" "community" "edge"
install_apk "zstd-libs" "main" "edge"
install_apk "xz-libs" "main" "edge"
install_apk "curl" "main" "edge"
install_apk "libcurl" "main" "edge"
install_apk "libsharpyuv" "main" "edge"
install_apk "libwebp" "main" "edge"
install_apk "lcms2" "main" "edge"
install_apk "libgcc" "main" "edge"
install_apk "libdav1d" "main" "edge"
install_apk "dav1d" "main" "edge"
install_apk "libxml2" "main" "edge"
install_apk "libxslt" "main" "edge"
install_apk "librsvg" "community" "edge"
install_apk "openssl" "main" "edge"
install_apk "libssl3" "main" "edge"
install_apk "libcrypto3" "main" "edge"
install_apk "nghttp2-libs" "main" "edge"
install_apk "libidn2" "main" "edge"
install_apk "libunistring" "main" "edge"
install_apk "libpsl" "main" "edge"
install_apk "c-ares" "main" "edge"
install_apk "brotli-libs" "main" "edge"
install_apk "ca-certificates" "main"
install_apk "libbz2" "main"
install_apk "zlib" "main"


# 4. Finalize GTK environment
echo "[*] Compiling GSettings schemas..."
if [ -d "${ROOTFS_DIR}/usr/share/glib-2.0/schemas" ]; then
    if command -v glib-compile-schemas >/dev/null 2>&1; then
        glib-compile-schemas "${ROOTFS_DIR}/usr/share/glib-2.0/schemas"
    fi
fi

echo "[*] Updating MIME database..."
if [ -d "${ROOTFS_DIR}/usr/share/mime" ]; then
    if command -v update-mime-database >/dev/null 2>&1; then
        update-mime-database "${ROOTFS_DIR}/usr/share/mime"
    fi
fi

echo "[*] Injecting gdk-pixbuf loaders cache..."
# Manually register PNG, JPEG and SVG loaders since we can't run the query tool
LOADERS_DIR="/usr/lib/gdk-pixbuf-2.0/2.10.0"
mkdir -p "${ROOTFS_DIR}${LOADERS_DIR}"
cat > "${ROOTFS_DIR}${LOADERS_DIR}/loaders.cache" <<EOF
# GdkPixbuf Image Loader Modules file
# Automatically generated file, do not edit

"/usr/lib/libgdk_pixbuf-2.0.so.0"
"png" 5 "gdk-pixbuf" "PNG" "LGPL"
"image/png" ""
"png" ""
"\211PNG\r\n\032\n" "" 100

"/usr/lib/libgdk_pixbuf-2.0.so.0"
"jpeg" 5 "gdk-pixbuf" "JPEG" "LGPL"
"image/jpeg" ""
"jpeg" "jpe" "jpg" ""
"\377\330" "" 100

"${LOADERS_DIR}/loaders/libpixbufloader-gif.so"
"gif" 4 "gdk-pixbuf" "GIF" "LGPL"
"image/gif" ""
"gif" ""
"GIF8" "" 100

"${LOADERS_DIR}/loaders/libpixbufloader-bmp.so"
"bmp" 5 "gdk-pixbuf" "BMP" "LGPL"
"image/bmp" "image/x-bmp" "image/x-MS-bmp" ""
"bmp" ""
"BM" "" 100

"${LOADERS_DIR}/loaders/libpixbufloader_svg.so"
"svg" 6 "gdk-pixbuf" "Scalable Vector Graphics" "LGPL"
"image/svg+xml" "image/svg" "image/svg-xml" "image/vnd.adobe.svg+xml" "text/xml-svg" "image/svg+xml-compressed" ""
"svg" "svgz" "svg.gz" ""
" <svg" "* " 100
" <!DOCTYPE svg" "* " 100

EOF

# 4. Create weston.ini
echo "[*] Creating /etc/weston.ini..."
mkdir -p "${ROOTFS_DIR}/etc"
cat > "${ROOTFS_DIR}/etc/weston.ini" <<EOF
[core]
backend=drm-backend.so
xwayland=true

[shell]
panel-position=top
locking=false

[launcher]
icon=/usr/share/icons/Adwaita/24x24/apps/utilities-terminal.png
path=/usr/bin/weston-terminal

[output]
name=LVDS1
mode=preferred
EOF

# 5. Inject custom binaries
echo "[*] Injecting custom binaries into rootfs..."
mkdir -p "${ROOTFS_DIR}/bin"
if [ -f "${ROOT_DIR}/userland/gtk_test.elf" ]; then
    cp "${ROOT_DIR}/userland/gtk_test.elf" "${ROOTFS_DIR}/bin/gtk_test"
    chmod +x "${ROOTFS_DIR}/bin/gtk_test"
fi
if [ -f "${ROOT_DIR}/userland/gtk3_test.elf" ]; then
    cp "${ROOT_DIR}/userland/gtk3_test.elf" "${ROOTFS_DIR}/bin/gtk3_test"
    chmod +x "${ROOTFS_DIR}/bin/gtk3_test"
fi

# 5. Inject into disk.img
if [ ! -f "${DISK_IMG}" ]; then
    echo "[!] disk.img not found. Please run 'make disk.img' first."
    exit 1
fi

echo "[*] Extracting partition 1 from disk.img..."
PART_IMG="${BUILD_DIR}/part1.img"
dd if="${DISK_IMG}" of="${PART_IMG}" bs=1M skip=1 status=none

echo "[*] Populating partition with Alpine rootfs (using debugfs)..."
"${POPULATE_SCRIPT}" "${PART_IMG}" "${ROOTFS_DIR}" "/"

echo "[*] Re-injecting partition 1 into disk.img..."
dd if="${PART_IMG}" of="${DISK_IMG}" bs=1M seek=1 conv=notrunc status=none

echo "[SUCCESS] Alpine rootfs with GTK 2.0 and 'st' installed into ${DISK_IMG}"
