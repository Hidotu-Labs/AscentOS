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
install_apk "libxxf86vm" "main"
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

# Openbox window manager (LXDE base) and dependencies
echo "[*] Installing Openbox window manager..."
install_apk "openbox" "community"
install_apk "openbox-libs" "community"
install_apk "libxml2" "main"
install_apk "startup-notification" "community"
install_apk "libxcomposite" "main"
install_apk "libxdamage" "main"



# X11 utilities and toolkit libraries (xclock, xterm, etc.)
echo "[*] Installing Xorg Server and DRM drivers..."
install_apk "xorg-server" "community"
install_apk "xf86-input-libinput" "community"
install_apk "xf86-input-evdev" "community"
install_apk "libxfont2" "community"
install_apk "libxcvt" "community"
install_apk "libfontenc" "main"
install_apk "font-cursor-misc" "main"
install_apk "font-misc-misc" "main"
install_apk "xkbcomp" "main"
install_apk "mesa-dri-gallium" "main"
install_apk "libdrm" "main"
install_apk "mesa-gbm" "main"
install_apk "mesa-egl" "main"
install_apk "nettle" "main"
install_apk "libmagic" "main"

echo "[*] Installing X11 utilities and toolkit libraries..."
install_apk "libxt" "main"
install_apk "libxmu" "main"
install_apk "libxaw" "main"
install_apk "libxext" "main"
install_apk "libxkbfile" "main"

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
install_apk "libxfixes" "main"
install_apk "fontconfig" "main"
install_apk "freetype" "main"
install_apk "libpng" "main"
install_apk "libexpat" "main"
install_apk "libuuid" "main"
install_apk "shared-mime-info" "main"
install_apk "pcre2" "main"
install_apk "libffi" "main"
install_apk "libxinerama" "main"
install_apk "util-linux" "main"
install_apk "libbz2" "main"
install_apk "brotli-libs" "main"
install_apk "zlib" "main"
install_apk "lua5.4" "main"
install_apk "lua5.4-libs" "main"
install_apk "readline" "main"
install_apk "libncursesw" "main"
install_apk "htop" "main"

# GTK 3.0 and its core dependencies
echo "[*] Installing GTK 3.0 and dependencies..."
install_apk "gtk+3.0" "main"
install_apk "libatk-bridge-2.0" "main"
install_apk "at-spi2-core" "main"
install_apk "dbus-libs" "main"
install_apk "cairo-gobject" "main"
install_apk "libepoxy" "main"
install_apk "adwaita-icon-theme" "community"
install_apk "hicolor-icon-theme" "main"
install_apk "iso-codes" "main"
install_apk "wayland" "main"
install_apk "wayland-libs-client" "main"
install_apk "wayland-libs-server" "main"
install_apk "wayland-libs-cursor" "main"
install_apk "wayland-libs-egl" "main"
install_apk "libxkbcommon" "main"
install_apk "wlroots" "community"
install_apk "wlroots-dev" "community"
install_apk "libweston" "community"
install_apk "weston" "community"
install_apk "weston-shell-desktop" "community"
install_apk "weston-backend-drm" "community"
install_apk "weston-terminal" "community"

install_apk "wayland-protocols" "main"
install_apk "wayland-dev" "main"
install_apk "vulkan-loader" "main"
install_apk "mesa-gl" "main"
install_apk "mesa-gles" "main"
install_apk "mesa-vulkan-swrast" "main"
install_apk "mesa-demos" "community"
install_apk "freeglut" "community"
# Alpine names the classic GLX demo "gears"; preserve the conventional command.
rm -f "${ROOTFS_DIR}/usr/bin/glxgears"
cp "${ROOTFS_DIR}/usr/bin/gears" "${ROOTFS_DIR}/usr/bin/glxgears"
install_apk "libliftoff" "community"
install_apk "libinput" "community"
install_apk "libinput-libs" "community"
install_apk "libwacom" "community"
install_apk "libevdev" "community"
install_apk "mtdev" "community"
install_apk "libxml2" "main"
install_apk "libdisplay-info" "community"
install_apk "eudev-libs" "main"
install_apk "libgudev" "community"
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
install_apk "libseat" "community"
install_apk "libelogind" "community"
install_apk "libcap2" "main"
install_apk "libpciaccess" "main"
install_apk "gcompat" "main"
install_apk "libucontext" "main"
install_apk "libucontext-dev" "main"
install_apk "jansson" "main"

# Keep musl's runtime linker search path explicit inside AscentOS.
# Some early userspace paths only reliably resolve shared objects from /lib.
mkdir -p "${ROOTFS_DIR}/etc" "${ROOTFS_DIR}/lib"
cat > "${ROOTFS_DIR}/etc/ld-musl-x86_64.path" <<'EOF'
/lib
/usr/local/lib
/usr/lib
EOF
if [ -f "${ROOTFS_DIR}/usr/lib/libucontext.so.1" ]; then
    cp "${ROOTFS_DIR}/usr/lib/libucontext.so.1" "${ROOTFS_DIR}/lib/libucontext.so.1"
fi
if [ -f "${ROOTFS_DIR}/usr/lib/libucontext_posix.so.1" ]; then
    cp "${ROOTFS_DIR}/usr/lib/libucontext_posix.so.1" "${ROOTFS_DIR}/lib/libucontext_posix.so.1"
fi

# glibc-linked tools such as /opt/coreutils/bin/ls search lib64 paths.
mkdir -p "${ROOTFS_DIR}/lib64" "${ROOTFS_DIR}/usr/lib64"
if [ -f "${ROOTFS_DIR}/lib/libc.musl-x86_64.so.1" ]; then
    cp "${ROOTFS_DIR}/lib/libc.musl-x86_64.so.1" "${ROOTFS_DIR}/lib64/libc.musl-x86_64.so.1"
    cp "${ROOTFS_DIR}/lib/libc.musl-x86_64.so.1" "${ROOTFS_DIR}/usr/lib64/libc.musl-x86_64.so.1"
fi
if [ -f "${ROOTFS_DIR}/usr/lib/libucontext.so.1" ]; then
    cp "${ROOTFS_DIR}/usr/lib/libucontext.so.1" "${ROOTFS_DIR}/lib64/libucontext.so.1"
    cp "${ROOTFS_DIR}/usr/lib/libucontext.so.1" "${ROOTFS_DIR}/usr/lib64/libucontext.so.1"
fi
if [ -f "${ROOTFS_DIR}/usr/lib/libucontext_posix.so.1" ]; then
    cp "${ROOTFS_DIR}/usr/lib/libucontext_posix.so.1" "${ROOTFS_DIR}/lib64/libucontext_posix.so.1"
    cp "${ROOTFS_DIR}/usr/lib/libucontext_posix.so.1" "${ROOTFS_DIR}/usr/lib64/libucontext_posix.so.1"
fi

install_apk "mesa-glapi" "main"
install_apk "llvm19-libs" "main"
install_apk "libelf" "main"
install_apk "zstd-libs" "main"
install_apk "musl-obstack" "main"

# glibc-linked coreutils also need musl-obstack in lib64 paths.
if [ -f "${ROOTFS_DIR}/usr/lib/libobstack.so.1" ]; then
    mkdir -p "${ROOTFS_DIR}/lib64" "${ROOTFS_DIR}/usr/lib64"
    cp "${ROOTFS_DIR}/usr/lib/libobstack.so.1" "${ROOTFS_DIR}/lib64/libobstack.so.1"
    cp "${ROOTFS_DIR}/usr/lib/libobstack.so.1" "${ROOTFS_DIR}/usr/lib64/libobstack.so.1"
fi
install_apk "libunwind" "main"
install_apk "libva" "main"
install_apk "xcb-util-wm" "community"
install_apk "xcb-util-image" "community"
install_apk "xcb-util-renderutil" "community"
install_apk "xcb-util" "main"
install_apk "seatd" "community"
install_apk "libxshmfence" "main"
install_apk "xcalc" "community"
install_apk "galculator" "community"
install_apk "gnome-calculator" "community"
install_apk "libadwaita" "community"
install_apk "appstream" "community"
install_apk "libxmlb" "community"
install_apk "yaml" "main"
install_apk "graphene" "main"
install_apk "gtksourceview5" "community"
install_apk "gtk4.0" "community"
install_apk "harfbuzz" "main"
install_apk "harfbuzz-subset" "main"
install_apk "gsettings-desktop-schemas" "community"
install_apk "libgee" "community"
install_apk "mpfr4" "main"
install_apk "mpc1" "main"
install_apk "figlet" "community"
install_apk "nyancat" "community"
install_apk "micro-tetris" "community" "edge"
install_apk "cmus" "community"

# cmus audio dependencies
echo "[*] Installing cmus audio codecs and libraries..."
install_apk "libflac" "main"
install_apk "alsa-lib" "main"
install_apk "faad2-libs" "community"
install_apk "libmad" "community"
install_apk "libvorbis" "main"
install_apk "wavpack-libs" "community"
install_apk "opusfile" "main"
# ffmpeg libs (for additional codec support)
install_apk "ffmpeg-libavcodec" "community"
install_apk "ffmpeg-libavformat" "community"
install_apk "libpulse" "community"

install_apk "xkeyboard-config" "main"
install_apk "font-dejavu" "main"
install_apk "sl" "community"
install_apk "gifsicle" "community"

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
install_apk "dbus" "main"
install_apk "nano" "main"

# Compiler / toolchain tools for AUR package compilation
echo "[*] Installing compilation tools..."
install_apk "make" "main"
install_apk "gcc" "main"
# gcc runtime shared-library dependencies (cc1/lto1 link against these)
install_apk "isl26" "main"
install_apk "mpfr4" "main"
install_apk "mpc1" "main"
install_apk "musl-dev" "main"
install_apk "binutils" "main"

# Wrap the real gcc with a script that locks the sysroot to / so that
# cc1 always resolves #include <...> against the rootfs's musl headers
# rather than any host glibc headers that may bleed through the VFS.
echo "[*] Installing gcc sysroot wrapper..."
GCC_REAL="${ROOTFS_DIR}/usr/bin/gcc"
GCC_WRAPPER="${ROOTFS_DIR}/usr/bin/gcc"
if [ -f "${GCC_REAL}" ]; then
    mv "${GCC_REAL}" "${ROOTFS_DIR}/usr/bin/gcc.real"
    cat > "${GCC_WRAPPER}" << 'GCC_WRAP_EOF'
#!/bin/sh
# gcc wrapper — forces --sysroot=/ so the compiler always uses the musl
# headers and libraries inside the AscentOS rootfs image, not host glibc.
exec /usr/bin/gcc.real \
    --sysroot=/ \
    -isystem /usr/lib/gcc/x86_64-alpine-linux-musl/14.2.0/include \
    -isystem /usr/include \
    "$@"
GCC_WRAP_EOF
    chmod +x "${GCC_WRAPPER}"
fi

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
install_apk "pixman-dev" "main"
install_apk "libdrm-dev" "main"
install_apk "libinput-dev" "community"
install_apk "libseat-dev" "community"
install_apk "vulkan-loader-dev" "main"
install_apk "mesa-dev" "main"
install_apk "mesa" "main"

# ── XFCE4 Desktop Environment ────────────────────────────────────────────
# XFCE4 runs on top of XWayland (Weston provides the Wayland compositor;
# XFCE4 components run as X11 clients on the embedded XWayland display).
echo "[*] Installing XFCE4 desktop environment..."
install_apk "xfce4" "community"
install_apk "xfce4-session" "community"
install_apk "xfwm4" "community"
install_apk "xfdesktop" "community"
install_apk "xfce4-panel" "community"
install_apk "xfce4-settings" "community"
install_apk "lz4-libs" "main"
install_apk "vte3" "community"
install_apk "xfce4-terminal" "community"
install_apk "xfconf" "community"
install_apk "libxklavier" "community"
install_apk "libxfce4util" "community"
install_apk "libxfce4ui" "community"
install_apk "libgtop" "community"
install_apk "garcon" "community"
install_apk "exo" "community"
install_apk "exo-libs" "community"
install_apk "libxfce4panel" "community"
install_apk "thunar" "community"
install_apk "thunar-volman" "community"
install_apk "tumbler" "community"
install_apk "xfce4-appfinder" "community"
install_apk "xfce4-power-manager" "community"
install_apk "xfce4-notifyd" "community"
install_apk "xfce4-screensaver" "community"

# Plugins commonly expected to exist at XFCE4 startup
install_apk "xfce4-panel-dev" "community"
install_apk "xfce4-battery-plugin" "community"
install_apk "xfce4-clipman-plugin" "community" "edge"
install_apk "xfce4-systemload-plugin" "community" "edge"
install_apk "xfce4-whiskermenu-plugin" "community"

# Additional XFCE4 dependencies
install_apk "libnotify" "community"
install_apk "notification-daemon" "testing" "edge"
install_apk "polkit" "community"
install_apk "polkit-elogind" "community"
install_apk "upower" "community"
install_apk "gcr" "community"
install_apk "gcr4" "community"
install_apk "gcr4-base" "community"
install_apk "libsecret" "main"
install_apk "libgcrypt" "main"
install_apk "libgpg-error" "main"
install_apk "p11-kit" "main"
install_apk "libtasn1" "main"
install_apk "libxres" "community"
install_apk "libxpresent" "community"

# xfce4-session hard dependency — libwnck3
install_apk "libwnck3" "community"
# xrdb is called by startxfce4 / .xinitrc to load X resources
install_apk "xrdb" "community"
# xhost needed by some XFCE4 components when switching displays
install_apk "xhost" "community"

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

# SDL2 and related libraries
echo "[*] Installing SDL2 and related libraries..."
install_apk "sdl2" "community"
install_apk "sdl2-dev" "community"
install_apk "sdl2_image" "community"
install_apk "sdl2_image-dev" "community"
install_apk "sdl2_ttf" "community"
install_apk "sdl2_ttf-dev" "community"
install_apk "sdl2_mixer" "community"
install_apk "sdl2_mixer-dev" "community"
install_apk "sdl2_net" "community"
install_apk "sdl2_net-dev" "community"

# WebKitGTK (GTK 3 / libsoup 3 ABI)
#
# Packages are extracted manually by install_apk(), so apk cannot resolve the
# shared-library providers for us. Keep WebKitGTK on the same v3.21 branch as
# GTK and install its non-core runtime providers explicitly before the engine.
echo "[*] Installing WebKitGTK and dependencies..."
install_apk "bubblewrap" "main"
install_apk "xdg-dbus-proxy" "community"
install_apk "gnome-keyring" "community"
install_apk "libavif" "main"
install_apk "libatomic" "main"
install_apk "aom-libs" "main"
install_apk "enchant2-libs" "community"
install_apk "flite" "main"
install_apk "gstreamer" "main"
install_apk "gst-plugins-base" "main"
install_apk "orc" "main"
# gst-plugins-bad is unpacked without apk dependency resolution. Install the
# codec/scanner libraries used by its voaacenc, voamrwbenc, webrtc and zbar
# plugins explicitly so gst-plugin-scanner can load them.
install_apk "vo-aacenc" "community"
install_apk "vo-amrwbenc" "community"
install_apk "libnice" "community"
install_apk "libzbar" "community"
install_apk "libxv" "main"
install_apk "libusb" "main"
install_apk "libsrtp" "main"
install_apk "tiff" "main"
install_apk "spandsp" "main"
install_apk "gst-plugins-bad" "community"
install_apk "harfbuzz-icu" "main"
install_apk "hyphen" "community"
install_apk "icu-data-en" "main"
install_apk "icu-libs" "main"
install_apk "libjxl" "community"
install_apk "libhwy" "community"
install_apk "libmanette" "community"
install_apk "libseccomp" "main"
install_apk "libsoup3" "community"
# libsoup3 uses GIO's dynamically loaded TLS implementation. Since packages
# are unpacked without apk dependency resolution, install the complete
# glib-networking/GnuTLS chain explicitly; otherwise HTTPS fails with
# "TLS support is not available" even though plain HTTP works.
install_apk "gmp" "main"
install_apk "gnutls" "main"
install_apk "duktape" "community"
install_apk "libproxy" "community"
install_apk "ca-certificates" "main"
install_apk "glib-networking" "community"
install_apk "sqlite-libs" "main"
install_apk "libwoff2common" "community"
install_apk "libwoff2dec" "community"
install_apk "libwoff2enc" "community"
install_apk "libwebpmux" "main"
install_apk "libwebpdemux" "main"
install_apk "webkit2gtk-4.1" "community"
install_apk "badwolf" "community"

# AscentOS does not yet provide the namespaces, seccomp, or pidfd syscalls used
# by WebKitGTK's bubblewrap sandbox. Its DRM stack also lacks the DRI2/DRI3
# authentication needed by WebKit accelerated compositing. Keep the packaged
# binary intact and install a compatibility launcher at the conventional path.
if [ -x "${ROOTFS_DIR}/usr/bin/badwolf" ] &&
   [ ! -e "${ROOTFS_DIR}/usr/libexec/badwolf.bin" ]; then
    mkdir -p "${ROOTFS_DIR}/usr/libexec"
    mv "${ROOTFS_DIR}/usr/bin/badwolf" "${ROOTFS_DIR}/usr/libexec/badwolf.bin"
fi
cat > "${ROOTFS_DIR}/usr/bin/badwolf" <<'EOF'
#!/bin/sh
export WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1
export WEBKIT_DISABLE_COMPOSITING_MODE=1
export WEBKIT_DISABLE_DMABUF_RENDERER=1
export LIBGL_ALWAYS_SOFTWARE=1
exec /usr/libexec/badwolf.bin "$@"
EOF
chmod +x "${ROOTFS_DIR}/usr/bin/badwolf"

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
install_apk "libtirpc-nokrb" "main"
install_apk "xwayland" "community" 
install_apk "weston-xwayland" "community" 
install_apk "cmatrix" "community" 
install_apk "btop" "community" 

# 4. Finalize GTK environment
echo "[*] Setting up global GTK performance environment variables..."
mkdir -p "${ROOTFS_DIR}/etc/profile.d" "${ROOTFS_DIR}/etc/pulse"
cat > "${ROOTFS_DIR}/etc/profile.d/gtk_ascentos.sh" << 'ENV_EOF'
export NO_AT_BRIDGE=1
export GTK_A11Y=none
export GIO_USE_VFS=local
export GIO_USE_VOLUME_MONITOR=unix
export GTK_USE_PORTAL=0
export GDK_GL=disable
export LIBGL_DRI3_DISABLE=1
export PULSE_SERVER=""
ENV_EOF
chmod +x "${ROOTFS_DIR}/etc/profile.d/gtk_ascentos.sh"

cat > "${ROOTFS_DIR}/etc/environment" << 'ENV_EOF'
NO_AT_BRIDGE=1
GTK_A11Y=none
GIO_USE_VFS=local
GIO_USE_VOLUME_MONITOR=unix
GTK_USE_PORTAL=0
GDK_GL=disable
LIBGL_DRI3_DISABLE=1
PULSE_SERVER=""
ENV_EOF

cat > "${ROOTFS_DIR}/etc/pulse/client.conf" << 'PULSE_EOF'
autospawn = no
disable-shm = yes
PULSE_EOF

# Disable D-Bus activation for GVfs volume monitors to prevent 25s timeouts
rm -f "${ROOTFS_DIR}"/usr/share/dbus-1/services/org.gtk.vfs.*VolumeMonitor.service 2>/dev/null || true

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

# 4a. Configure Xorg for DRM/Modesetting
echo "[*] Configuring Xorg DRM/modesetting..."
XORG_CONF_DIR="${ROOTFS_DIR}/etc/X11/xorg.conf.d"
mkdir -p "${XORG_CONF_DIR}"
rm -f "${ROOTFS_DIR}/usr/share/X11/xorg.conf.d/40-libinput.conf"
cat > "${XORG_CONF_DIR}/10-modesetting.conf" <<EOF
Section "ServerLayout"
    Identifier  "AscentLayout"
    Screen      0 "Screen0" 0 0
    InputDevice "Keyboard0" "CoreKeyboard"
    InputDevice "Mouse0" "CorePointer"
    Option      "AutoAddDevices" "false"
EndSection

Section "Device"
    Identifier  "Card0"
    Driver      "modesetting"
    Option      "SWcursor" "true"
EndSection

Section "Screen"
    Identifier  "Screen0"
    Device      "Card0"
EndSection

Section "InputDevice"
    Identifier  "Keyboard0"
    Driver      "evdev"
    Option      "Device" "/dev/input/event0"
    Option      "CoreKeyboard" "true"
EndSection

Section "InputDevice"
    Identifier  "Mouse0"
    Driver      "evdev"
    Option      "Device" "/dev/input/event1"
    Option      "CorePointer" "true"
EndSection
EOF

# 4b. Inject Openbox / LXDE startup config
echo "[*] Configuring Openbox as default X11 session..."

mkdir -p "${ROOTFS_DIR}/etc/skel"
cat > "${ROOTFS_DIR}/etc/skel/.xinitrc" << 'EOF'
#!/bin/sh
exec openbox-session
EOF
chmod +x "${ROOTFS_DIR}/etc/skel/.xinitrc"

# Root's home is / on AscentOS.
cp "${ROOTFS_DIR}/etc/skel/.xinitrc" "${ROOTFS_DIR}/.xinitrc"

# Minimal Openbox rc.xml (no dbus dependency, clean keybinds)
mkdir -p "${ROOTFS_DIR}/etc/xdg/openbox"
cat > "${ROOTFS_DIR}/etc/xdg/openbox/rc.xml" << 'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc"
                xmlns:xi="http://www.w3.org/2001/XInclude">
  <resistance><strength>10</strength><screen_edge_strength>20</screen_edge_strength></resistance>
  <focus><focusNew>yes</focusNew><followMouse>no</followMouse><focusLast>yes</focusLast></focus>
  <placement><policy>Smart</policy></placement>
  <theme>
    <name>Clearlooks</name>
    <titleLayout>NLC</titleLayout>
    <keepBorder>yes</keepBorder>
  </theme>
  <desktops><number>2</number><firstdesk>1</firstdesk><names><name>Main</name><name>Extra</name></names></desktops>
  <resize><drawContents>yes</drawContents></resize>
  <mouse>
    <dragThreshold>8</dragThreshold>
    <doubleClickTime>200</doubleClickTime>
    <context name="Frame">
      <mousebind button="A-Left" action="Press"><action name="Focus"/><action name="Raise"/></mousebind>
      <mousebind button="A-Left" action="Drag"><action name="Move"/></mousebind>
      <mousebind button="A-Right" action="Drag"><action name="Resize"/></mousebind>
    </context>
    <context name="Titlebar">
      <mousebind button="Left" action="Drag"><action name="Move"/></mousebind>
      <mousebind button="Left" action="DoubleClick"><action name="ToggleMaximizeFull"/></mousebind>
    </context>
    <context name="Desktop">
      <mousebind button="Right" action="Press"><action name="ShowMenu"><menu>root-menu</menu></action></mousebind>
    </context>
  </mouse>
  <keyboard>
    <keybind key="A-F4"><action name="Close"/></keybind>
    <keybind key="A-Tab"><action name="NextWindow"/></keybind>
    <keybind key="A-space"><action name="ShowMenu"><menu>client-menu</menu></action></keybind>
    <keybind key="Super_L"><action name="ShowMenu"><menu>root-menu</menu></action></keybind>
  </keyboard>
  <applications/>
</openbox_config>
EOF

# Minimal right-click desktop menu
cat > "${ROOTFS_DIR}/etc/xdg/openbox/menu.xml" << 'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<openbox_menu xmlns="http://openbox.org/3.4/menu">
  <menu id="root-menu" label="AscentOS">
    <item label="Terminal (st)">
      <action name="Execute"><execute>st</execute></action>
    </item>
    <item label="Forkit Browser">
      <action name="Execute"><execute>forkit</execute></action>
    </item>
    <item label="NetSurf Browser">
      <action name="Execute"><execute>netsurf-gtk</execute></action>
    </item>
    <item label="File Manager">
      <action name="Execute"><execute>pcmanfm</execute></action>
    </item>
    <item label="Text Editor">
      <action name="Execute"><execute>mousepad</execute></action>
    </item>
    <item label="Music Player (cmus)">
      <action name="Execute"><execute>st -e cmus</execute></action>
    </item>
    <separator/>
    <item label="Reconfigure Openbox">
      <action name="Reconfigure"/>
    </item>
    <item label="Exit">
      <action name="Exit"/>
    </item>
  </menu>
</openbox_menu>
EOF



# 4c-xfce. Configure XFCE4 session to launch inside XWayland
# Weston starts XWayland automatically (xwayland=true in weston.ini).
# We provide a startxfce4 wrapper that points at the XWayland display
# Weston exports (typically :10), and a D-Bus session so XFCE4 can talk
# to its own daemons.
echo "[*] Configuring XFCE4 session for XWayland..."
mkdir -p "${ROOTFS_DIR}/usr/bin"
cat > "${ROOTFS_DIR}/usr/bin/start-xfce4-wayland" << 'XFCE_EOF'
#!/bin/sh
# start-xfce4-wayland — launch XFCE4 on the XWayland display that Weston
# exports.  Run this from a weston-terminal or from the Weston launcher.
#
# Weston exports XWayland as DISPLAY=:10 by default; try :10 first,
# then scan :0..:9 as fallback.
find_xwayland_display() {
    for d in 10 0 1 2 3 4 5; do
        if [ -S "/tmp/.X11-unix/X${d}" ]; then
            echo ":${d}"
            return 0
        fi
    done
    echo ":10"   # best guess even if socket not yet visible
}

export DISPLAY="${DISPLAY:-$(find_xwayland_display)}"
export XDG_SESSION_TYPE=x11
export XDG_CURRENT_DESKTOP=XFCE
export XCURSOR_THEME=Adwaita
export XCURSOR_SIZE=24
export GDK_GL=disable
export LIBGL_DRI3_DISABLE=1
export NO_AT_BRIDGE=1
export GTK_A11Y=none
export GIO_USE_VFS=local
export GIO_USE_VOLUME_MONITOR=unix
export GTK_USE_PORTAL=0

# Ensure XDG dirs exist
export XDG_CONFIG_HOME="${XDG_CONFIG_HOME:-${HOME}/.config}"
export XDG_DATA_HOME="${XDG_DATA_HOME:-${HOME}/.local/share}"
export XDG_CACHE_HOME="${XDG_CACHE_HOME:-${HOME}/.cache}"
mkdir -p "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_CACHE_HOME" \
         "$HOME/Desktop" "$HOME/Templates" "$HOME/Downloads" "$HOME/Documents" \
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

rm -rf "$XDG_CACHE_HOME"/*-socket* "$XDG_CACHE_HOME"/pcmanfm* "$XDG_CACHE_HOME"/Thunar* /tmp/.*-lock /tmp/*-socket*
if [ -f /etc/xdg/gtk-3.0/gtk.css ]; then
    cp -f /etc/xdg/gtk-3.0/gtk.css "$XDG_CONFIG_HOME/gtk-3.0/gtk.css"
fi

# Bootstrap D-Bus session if not already present
if [ -z "$DBUS_SESSION_BUS_ADDRESS" ]; then
    eval "$(dbus-launch --sh-syntax --exit-with-session)" 2>/dev/null || true
fi

exec startxfce4
XFCE_EOF
chmod +x "${ROOTFS_DIR}/usr/bin/start-xfce4-wayland"

# Drop a minimal xfce4-session.xml so first-run doesn't open the wizard
XFCE_SESSION_DIR="${ROOTFS_DIR}/etc/xdg/xfce4/xfconf/xfce-perchannel-xml"
mkdir -p "${XFCE_SESSION_DIR}"
cat > "${XFCE_SESSION_DIR}/xfce4-session.xml" << 'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<channel name="xfce4-session" version="1.0">
  <property name="general" type="empty">
    <property name="SaveOnExit" type="bool" value="false"/>
    <property name="SessionName" type="string" value="Default"/>
  </property>
  <property name="startup" type="empty">
    <property name="screensaver-delay" type="uint" value="0"/>
  </property>
  <property name="splash-screen" type="empty">
    <property name="engine" type="string" value=""/>
  </property>
</channel>
EOF

# xfwm4 — minimal dark window decorations
cat > "${XFCE_SESSION_DIR}/xfwm4.xml" << 'EOF'
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

# xfce4-panel — minimalist single top bar: app menu | tasklist | [spacer] | systray | clock
cat > "${XFCE_SESSION_DIR}/xfce4-panel.xml" << 'EOF'
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

# xfce4-desktop — no desktop icons, solid near-black background
cat > "${XFCE_SESSION_DIR}/xfce4-desktop.xml" << 'EOF'
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

# Pin GTK and cursor settings — dark theme for all GTK apps
cat > "${XFCE_SESSION_DIR}/xsettings.xml" << 'EOF'
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

# Xfdesktop normally uses a translucent rubber band. Keep an opaque fallback
# for AscentOS&apos;s non-composited X11 path, where alpha fills may disappear.
mkdir -p "${ROOTFS_DIR}/etc/xdg/gtk-3.0"
cat > "${ROOTFS_DIR}/etc/xdg/gtk-3.0/gtk.css" << 'EOF'
XfdesktopIconView .rubberband,
XfdesktopIconView rubberband {
    background-color: #3584e4;
    border: 1px solid #1c71d8;
    border-radius: 0;
}
EOF

# 3c. Alpine compiles its vendor name into xfce4-about rather than reading it
# from os-release. Replace only that NUL-terminated distributor field; keep
# Alpine/Xfce copyright and license text intact.
XFCE_ABOUT="${ROOTFS_DIR}/usr/bin/xfce4-about"
if [ -f "${XFCE_ABOUT}" ]; then
    perl -0pi -e "s/Alpine Linux\x00/AscentOS\x00\x00\x00\x00\x00/g" "${XFCE_ABOUT}"
fi

# 3d. Brand the assembled system while retaining accurate userland attribution.
echo "[*] Writing AscentOS operating-system identity..."
mkdir -p "${ROOTFS_DIR}/etc" "${ROOTFS_DIR}/usr/lib" "${ROOTFS_DIR}/usr/share/doc/ascentos"
rm -f "${ROOTFS_DIR}/etc/os-release" "${ROOTFS_DIR}/usr/lib/os-release"
cat > "${ROOTFS_DIR}/usr/lib/os-release" <<'EOF'
NAME="AscentOS"
ID=ascentos
VERSION="2.0.0 Beta"
VERSION_ID="2.0.0-beta"
PRETTY_NAME="AscentOS 2.0.0 Beta x86_64"
HOME_URL="https://github.com/AscentOS"
SUPPORT_URL="https://github.com/AscentOS"
BUG_REPORT_URL="https://github.com/AscentOS"
EOF
ln -s ../usr/lib/os-release "${ROOTFS_DIR}/etc/os-release"
cat > "${ROOTFS_DIR}/usr/share/doc/ascentos/ALPINE-USERLAND" <<'EOF'
AscentOS includes a userland assembled from Alpine Linux packages.
Alpine Linux is an independent project and does not produce or endorse AscentOS.
The copyright notices and license terms shipped with each package continue to apply.
See /usr/share/licenses and the corresponding package metadata where available.
EOF

# 4. Create weston.ini
echo "[*] Creating /etc/weston.ini..."
mkdir -p "${ROOTFS_DIR}/etc"
mkdir -p "${ROOTFS_DIR}/usr/share/icons/default"
cat > "${ROOTFS_DIR}/usr/share/icons/default/index.theme" <<'EOF'
[Icon Theme]
Name=Default
Inherits=Adwaita
EOF
rm -rf "${ROOTFS_DIR}/usr/share/icons/default/cursors"
ln -s ../Adwaita/cursors "${ROOTFS_DIR}/usr/share/icons/default/cursors"

cat > "${ROOTFS_DIR}/etc/weston.ini" <<EOF
[core]
backend=drm-backend.so
shell=desktop-shell.so
xwayland=true

[shell]
panel-position=top
locking=false
background-image=/assets/room.png
background-type=scale
cursor-theme=Adwaita
cursor-size=24

[launcher]
icon=/usr/share/weston/icon_terminal.png
path=/usr/bin/weston-terminal

[launcher]
icon=/usr/share/pixmaps/xfce4-session.png
path=/usr/bin/start-xfce4-wayland

[launcher]
icon=/usr/share/pixmaps/netsurf.xpm
path=/usr/bin/netsurf

[output]
name=HDMI-A-1
mode=preferred
EOF

# 4c. Setup helper symlinks in /usr/bin for path resolutions (e.g. tar child execs)
echo "[*] Creating helper symlinks in /usr/bin..."
mkdir -p "${ROOTFS_DIR}/usr/bin"
ln -sf /bin/busybox "${ROOTFS_DIR}/usr/bin/gzip"
ln -sf /bin/busybox "${ROOTFS_DIR}/usr/bin/tar"
ln -sf /bin/busybox "${ROOTFS_DIR}/usr/bin/xz"
ln -sf /bin/busybox "${ROOTFS_DIR}/usr/bin/unzip"
ln -sf /bin/busybox "${ROOTFS_DIR}/usr/bin/zip"

# 4d. Configure local users and groups without shadow databases
echo "[*] Configuring passwd/group databases (no shadow)..."
"${ROOT_DIR}/scripts/configure-accounts.sh" "${ROOTFS_DIR}"

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

# Generate caches last: no subsequent rootfs customization may make their
# configuration or source-directory metadata stale before image population.
echo "[*] Building final Fontconfig caches for the target rootfs..."
TARGET_LOADER="${ROOTFS_DIR}/lib/ld-musl-x86_64.so.1"
TARGET_FC_CACHE="${ROOTFS_DIR}/usr/bin/fc-cache"
if [ -x "${TARGET_LOADER}" ] && [ -x "${TARGET_FC_CACHE}" ]; then
    mkdir -p "${ROOTFS_DIR}/var/cache/fontconfig"
    "${TARGET_LOADER}" \
        --library-path "${ROOTFS_DIR}/lib:${ROOTFS_DIR}/usr/lib" \
        "${TARGET_FC_CACHE}" --sysroot="${ROOTFS_DIR}" --really-force \
        --system-only
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
