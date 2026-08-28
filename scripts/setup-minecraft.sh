#!/usr/bin/env bash
# scripts/setup-minecraft.sh -- Stage Minecraft Alpha 1.0 into AvoryOS
#
# Prerequisites:
#   1. Run ./scripts/setup-alpine.sh first  (provides Alpine rootfs, disk.img, OpenJDK 17)
#   2. assets/minecraft.jar  must be the Alpha 1.0 client JAR
#
# Usage:
#   ./scripts/setup-minecraft.sh [--expand-disk] [--username NAME]
#
# Options:
#   --expand-disk   Grow disk.img to 8 GiB
#   --username NAME Offline username written into the launcher (default: Player)

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DISK_IMG="${ROOT_DIR}/disk.img"
BUILD_DIR="${ROOT_DIR}/build/alpine"
ROOTFS_DIR="${BUILD_DIR}/rootfs"
POPULATE_SCRIPT="${ROOT_DIR}/scripts/populate-ext2-dir.sh"
MC_JAR_SRC="${ROOT_DIR}/assets/minecraft.jar"

EXPAND_DISK=0
MC_USERNAME="Player"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --expand-disk) EXPAND_DISK=1; shift ;;
        --username)    MC_USERNAME="${2:-Player}"; shift 2 ;;
        *) echo "[!] Unknown argument: $1" >&2; exit 1 ;;
    esac
done

# ─────────────────────────────────────────────────────────────────────────────
# Sanity checks
# ─────────────────────────────────────────────────────────────────────────────
[ ! -d "${ROOTFS_DIR}/etc" ] && {
    echo "[!] Alpine rootfs not found. Run ./scripts/setup-alpine.sh first."
    exit 1
}
[ ! -f "${MC_JAR_SRC}" ] && {
    echo "[!] assets/minecraft.jar not found."
    exit 1
}
[ ! -f "${DISK_IMG}" ] && {
    echo "[!] disk.img not found. Run 'make disk.img' first."
    exit 1
}

# ─────────────────────────────────────────────────────────────────────────────
# Locate or install OpenJDK 17 in rootfs
# ─────────────────────────────────────────────────────────────────────────────
ALPINE_BRANCH="v3.21"
APK_CACHE="${BUILD_DIR}"

install_apk_mc() {
    local PKG_NAME="$1"
    local REPO="$2"
    local BRANCH="${3:-${ALPINE_BRANCH}}"
    local PKG_MARKER="${ROOTFS_DIR}/etc/avoryos-pkg/${BRANCH}-${REPO}-${PKG_NAME}"

    if [ -f "${PKG_MARKER}" ]; then
        return 0
    fi

    echo "[*] Installing ${PKG_NAME} from ${REPO} (${BRANCH})..."

    local ESCAPED
    ESCAPED=$(printf '%s' "${PKG_NAME}" | sed 's/\./\\./g;s/+/\\+/g')
    local APK_FILENAME
    APK_FILENAME=$(curl -sL "https://dl-cdn.alpinelinux.org/alpine/${BRANCH}/${REPO}/x86_64/" \
        | grep -oP ">${ESCAPED}-[0-9][^<]*\.apk<" | sed 's/>//;s/<//' | sort -V | tail -n 1)

    [ -z "${APK_FILENAME}" ] && {
        echo "[!] Could not find package ${PKG_NAME} in ${REPO} (${BRANCH})" >&2
        return 1
    }

    local APK_URL="https://dl-cdn.alpinelinux.org/alpine/${BRANCH}/${REPO}/x86_64/${APK_FILENAME}"
    if [ ! -f "${APK_CACHE}/${APK_FILENAME}" ]; then
        echo "    Downloading ${APK_URL}..."
        curl -L "${APK_URL}" -o "${APK_CACHE}/${APK_FILENAME}"
    fi

    tar --ignore-zeros -xzf "${APK_CACHE}/${APK_FILENAME}" -C "${ROOTFS_DIR}" \
        --warning=no-unknown-keyword 2>/dev/null || true

    mkdir -p "${ROOTFS_DIR}/etc/avoryos-pkg"
    touch "${PKG_MARKER}"
}

find_java() {
    JAVA_HOME_ROOTFS=""
    for cand in \
        "${ROOTFS_DIR}/usr/lib/jvm/java-17-openjdk" \
        "${ROOTFS_DIR}/usr/lib/jvm/java-17" \
        "${ROOTFS_DIR}/usr/lib/jvm/default-jvm"; do
        if [ -d "${cand}" ]; then
            JAVA_HOME_ROOTFS="${cand}"
            return 0
        fi
    done
    if [ -z "${JAVA_HOME_ROOTFS}" ]; then
        JAVA_HOME_ROOTFS=$(ls -d "${ROOTFS_DIR}/usr/lib/jvm"/java-17* 2>/dev/null | head -1 || true)
    fi
    return 0
}

find_java
if [ -z "${JAVA_HOME_ROOTFS}" ]; then
    echo "[*] OpenJDK 17 not found in rootfs. Installing now..."
    install_apk_mc "mesa-gl" "main"
    install_apk_mc "mesa-glapi" "main"
    install_apk_mc "mesa-gles" "main"
    install_apk_mc "mesa-egl" "main"
    install_apk_mc "freeglut" "community"
    install_apk_mc "jemalloc" "main"
    install_apk_mc "java-common"            "community"
    install_apk_mc "openjdk17-jre-headless" "community"
    install_apk_mc "openjdk17-jre"          "community"
    install_apk_mc "java-cacerts"           "community"
    find_java
fi

[ -z "${JAVA_HOME_ROOTFS}" ] && {
    echo "[!] Failed to find or install OpenJDK 17. Aborting." >&2
    exit 1
}

JAVA_HOME_GUEST="${JAVA_HOME_ROOTFS#"${ROOTFS_DIR}"}"
echo "[+] Using JDK: ${JAVA_HOME_GUEST}"

# All JVM internal .so files use RPATH=$ORIGIN/../lib which AvoryOS's musl
# doesn't reliably resolve. Copy every .so from the JVM lib dir into /usr/lib/
# so musl's hardcoded search path always finds them.
echo "[*] Copying JVM internal libraries to /usr/lib/..."
find "${JAVA_HOME_ROOTFS}/lib" -name '*.so' -exec cp -f {} "${ROOTFS_DIR}/usr/lib/" \;
JVM_LIBS_COUNT=$(find "${JAVA_HOME_ROOTFS}/lib" -name '*.so' | wc -l)
echo "[+] Copied ${JVM_LIBS_COUNT} JVM .so files → /usr/lib/"

# libjli.so looks for jvm.cfg relative to its own location.
# Since it's now in /usr/lib/, copy jvm.cfg there too.
cp -f "${JAVA_HOME_ROOTFS}/lib/jvm.cfg" "${ROOTFS_DIR}/usr/lib/jvm.cfg"
echo "[+] Copied jvm.cfg → /usr/lib/"

# jvm.cfg references '-server KNOWN', so libjvm.so must also be in /usr/lib/.
cp -f "${JAVA_HOME_ROOTFS}/lib/server/libjvm.so" "${ROOTFS_DIR}/usr/lib/libjvm.so"
echo "[+] Copied server/libjvm.so → /usr/lib/"

# libjli.so resolves the server JVM at $libdir/server/libjvm.so relative to
# its own location (/usr/lib/), so create /usr/lib/server/ as well.
mkdir -p "${ROOTFS_DIR}/usr/lib/server"
cp -f "${JAVA_HOME_ROOTFS}/lib/server/libjvm.so" "${ROOTFS_DIR}/usr/lib/server/libjvm.so"
echo "[+] Copied server/libjvm.so → /usr/lib/server/"

# ─────────────────────────────────────────────────────────────────────────────
# 1. Download LWJGL 2.9.4 Linux x86_64 natives
#    MIT licensed — https://github.com/LWJGL/lwjgl/releases/tag/lwjgl-2.9.4
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "=== [1/4] Fetching LWJGL 2.9.1 Linux natives (Maven Central) ==="

# Maven Central hosts the LWJGL 2 natives JAR reliably.
# The JAR is a plain ZIP with .so files at its root.
LWJGL_VERSION="2.9.1"
LWJGL_ZIP="${BUILD_DIR}/lwjgl-platform-${LWJGL_VERSION}-natives-linux.jar"
LWJGL_NATIVES_DIR="${BUILD_DIR}/lwjgl-natives"
LWJGL_ZIP_URL="https://repo1.maven.org/maven2/org/lwjgl/lwjgl/lwjgl-platform/${LWJGL_VERSION}/lwjgl-platform-${LWJGL_VERSION}-natives-linux.jar"

if [ ! -f "${LWJGL_ZIP}" ]; then
    echo "[*] Downloading LWJGL ${LWJGL_VERSION}..."
    curl -L "${LWJGL_ZIP_URL}" -o "${LWJGL_ZIP}"
fi

if [ ! -d "${LWJGL_NATIVES_DIR}" ] || [ -z "$(ls -A "${LWJGL_NATIVES_DIR}" 2>/dev/null)" ]; then
    echo "[*] Extracting LWJGL Linux natives..."
    mkdir -p "${LWJGL_NATIVES_DIR}"
    # The natives JAR has .so files directly at the root (no subdirectory)
    unzip -o "${LWJGL_ZIP}" "*.so" -d "${LWJGL_NATIVES_DIR}" 2>/dev/null || true
fi

NATIVES_COUNT=$(find "${LWJGL_NATIVES_DIR}" -name '*.so' | wc -l)
echo "[+] LWJGL natives found: ${NATIVES_COUNT} .so file(s)"
[ "${NATIVES_COUNT}" -eq 0 ] && {
    echo "[!] No LWJGL .so files extracted. Check the zip layout." >&2
    exit 1
}

MC_NATIVES_ROOTFS="${ROOTFS_DIR}/opt/minecraft/natives"
mkdir -p "${MC_NATIVES_ROOTFS}"
cp "${LWJGL_NATIVES_DIR}/"*.so "${MC_NATIVES_ROOTFS}/"
echo "[+] Staged natives → /opt/minecraft/natives/"

# Also download lwjgl.jar (the Java API — contains org.lwjgl.* classes needed at runtime)
LWJGL_JAR="${BUILD_DIR}/lwjgl-${LWJGL_VERSION}.jar"
LWJGL_JAR_URL="https://repo1.maven.org/maven2/org/lwjgl/lwjgl/lwjgl/${LWJGL_VERSION}/lwjgl-${LWJGL_VERSION}.jar"
if [ ! -f "${LWJGL_JAR}" ]; then
    echo "[*] Downloading lwjgl.jar (Java classes)..."
    curl -L "${LWJGL_JAR_URL}" -o "${LWJGL_JAR}"
fi
cp "${LWJGL_JAR}" "${ROOTFS_DIR}/opt/minecraft/lwjgl.jar"
echo "[+] Staged lwjgl.jar → /opt/minecraft/"

# Also download lwjgl_util.jar (applet + utility classes some MC versions need)
LWJGL_UTIL_JAR="${BUILD_DIR}/lwjgl_util-${LWJGL_VERSION}.jar"
LWJGL_UTIL_URL="https://repo1.maven.org/maven2/org/lwjgl/lwjgl/lwjgl_util/${LWJGL_VERSION}/lwjgl_util-${LWJGL_VERSION}.jar"
if [ ! -f "${LWJGL_UTIL_JAR}" ]; then
    echo "[*] Downloading lwjgl_util.jar..."
    curl -L "${LWJGL_UTIL_URL}" -o "${LWJGL_UTIL_JAR}" 2>/dev/null || true
fi
if [ -f "${LWJGL_UTIL_JAR}" ]; then
    cp "${LWJGL_UTIL_JAR}" "${ROOTFS_DIR}/opt/minecraft/lwjgl_util.jar"
    echo "[+] Staged lwjgl_util.jar → /opt/minecraft/"
fi

# ─────────────────────────────────────────────────────────────────────────────
# 2. Stage minecraft.jar
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "=== [2/4] Staging minecraft.jar ==="

mkdir -p "${ROOTFS_DIR}/opt/minecraft"
cp "${MC_JAR_SRC}" "${ROOTFS_DIR}/opt/minecraft/minecraft.jar"
echo "[+] Staged minecraft.jar → /opt/minecraft/"

if [ -f "${ROOT_DIR}/userland/terrain.png" ]; then
    cp "${ROOT_DIR}/userland/terrain.png" "${ROOTFS_DIR}/opt/minecraft/terrain.png"
    echo "[+] Staged terrain.png"
fi

# ─────────────────────────────────────────────────────────────────────────────
# 3. Launcher script + desktop entries
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "=== [3/4] Writing launcher + desktop entries ==="

# /usr/bin/minecraft
mkdir -p "${ROOTFS_DIR}/usr/bin"
cat > "${ROOTFS_DIR}/usr/bin/minecraft" << LAUNCHER_EOF
#!/bin/sh
# Minecraft Alpha 1.0 / Release launcher for AvoryOS

export DISPLAY="\${DISPLAY:-:0}"

# Software GL (no GPU driver on AvoryOS)
export LIBGL_ALWAYS_SOFTWARE=1
export GALLIUM_DRIVER=llvmpipe
export LP_NUM_THREADS="\${LP_NUM_THREADS:-3}"
export LP_PERF=no_linear
export MESA_GL_VERSION_OVERRIDE=2.1
export MESA_GLSL_VERSION_OVERRIDE=120
export LIBGL_DRI3_DISABLE=1
# Disable Mesa on-disk shader cache to prevent fallocate crash in kernel
export MESA_SHADER_CACHE_DISABLE=true
export MESA_GLSL_CACHE_DISABLE=true
# llvmpipe still honors vblank waits, which caps fps at whatever refresh
# rate Mesa assumes for a headless/virtual output (often 60). Uncap it.
export vblank_mode=0
# Disable glthread on software rasterizers to avoid thread thrashing
export mesa_glthread="\${MESA_GLTHREAD:-false}"
# Mesa LLVMpipe performance tweaks
export MESA_NO_DITHER=1

JAVA_HOME="${JAVA_HOME_GUEST}"
export PATH="\${JAVA_HOME}/bin:\${PATH}"
# libjli.so and other JVM internals live in JAVA_HOME/lib — must be on LD_LIBRARY_PATH
export LD_LIBRARY_PATH="\${JAVA_HOME}/lib:\${JAVA_HOME}/lib/server:/usr/lib:/lib:\${LD_LIBRARY_PATH:-}"

MC_PRELOAD=""
[ -f /usr/lib/libjemalloc.so.2 ] && MC_PRELOAD="/usr/lib/libjemalloc.so.2"

MC_USER="\${MC_USER:-${MC_USERNAME}}"
MC_RAM="\${MC_RAM:-512m}"
MC_HOME="\${HOME}/.minecraft"
mkdir -p "\${MC_HOME}/saves" "\${MC_HOME}/texturepacks"
[ -f /opt/minecraft/terrain.png ] && [ ! -f "\${MC_HOME}/terrain.png" ] && cp /opt/minecraft/terrain.png "\${MC_HOME}/terrain.png" 2>/dev/null || true

# Always enforce high-performance options.txt for software rendering stability
cat > "\${MC_HOME}/options.txt" << 'OPT_EOF'
music:0.0
sound:0.0
invertYMouse:false
mouseSensitivity:0.5
fov:0.0
gamma:1.0
viewDistance:3
guiScale:0
particles:2
bobView:false
anaglyph3d:false
advancedOpengl:false
fpsLimit:0
difficulty:1
fancyGraphics:false
ao:0
clouds:false
skin:Default
lastServer:
chatVisibility:0
chatColors:true
chatLinks:false
chatLinksPrompt:false
chatOpacity:1.0
serverTextures:false
snooperEnabled:false
fullscreen:false
enableVsync:false
hideServerAddress:false
advancedItemTooltips:false
pauseOnLostFocus:true
showCape:false
touchscreen:false
overrideWidth:0
overrideHeight:0
heldItemTooltips:true
chatHeightFocused:1.0
chatHeightUnfocused:0.44366196
chatScale:1.0
chatWidth:1.0
OPT_EOF

echo "[minecraft] Starting Minecraft as '\${MC_USER}' (Heap: \${MC_RAM})..."
LD_PRELOAD="\${MC_PRELOAD}\${LD_PRELOAD:+:\$LD_PRELOAD}" \
MALLOC_CONF="background_thread:true,metadata_thp:auto,dirty_decay_ms:5000,muzzy_decay_ms:5000" \
exec "\${JAVA_HOME}/bin/java" \
    -Xms256m -Xmx"\${MC_RAM}" \
    -Xss512k \
    -Djdk.lang.Process.launchMechanism=fork \
    -Dorg.lwjgl.opengl.Display.allowSoftwareOpenGL=true \
    -Dsun.java2d.opengl=false \
    -Dsun.java2d.d3d=false \
    -Dsun.java2d.noddraw=true \
    -Dsun.awt.noerasebackground=true \
    -Dhttp.keepAlive=false \
    -Dsun.net.client.defaultConnectTimeout=2000 \
    -Dsun.net.client.defaultReadTimeout=2000 \
    -XX:+TieredCompilation \
    -XX:CICompilerCount=2 \
    -XX:Tier4InvocationThreshold=500 \
    -XX:Tier4MinInvocationThreshold=100 \
    -XX:Tier4CompileThreshold=1000 \
    -XX:ReservedCodeCacheSize=64m \
    -XX:InitialCodeCacheSize=32m \
    -XX:+DoEscapeAnalysis \
    -XX:+EliminateLocks \
    -XX:+UseSerialGC \
    -XX:+AlwaysPreTouch \
    -XX:+UnlockDiagnosticVMOptions \
    -XX:-ImplicitNullChecks \
    -Xshare:off \
    -XX:-UsePerfData \
    -Dos.name=Linux \
    -Djava.library.path=/opt/minecraft/natives \
    -Dminecraft.applet.TargetDirectory="\${MC_HOME}" \
    -cp /opt/minecraft/minecraft.jar:/opt/minecraft/lwjgl.jar:/opt/minecraft/lwjgl_util.jar \
    net.minecraft.client.Minecraft "\${MC_USER}" ""
LAUNCHER_EOF

chmod +x "${ROOTFS_DIR}/usr/bin/minecraft"
echo "[+] Wrote /usr/bin/minecraft"

# .desktop file
mkdir -p "${ROOTFS_DIR}/usr/share/applications"
cat > "${ROOTFS_DIR}/usr/share/applications/minecraft-alpha.desktop" << DESKTOP_EOF
[Desktop Entry]
Type=Application
Name=Minecraft Alpha 1.0
GenericName=Block Game
Comment=Minecraft Alpha 1.0 Java Edition
Exec=minecraft
Icon=minecraft-alpha
Terminal=false
Categories=Game;
Keywords=minecraft;alpha;blocks;survival;
DESKTOP_EOF
echo "[+] Wrote minecraft-alpha.desktop"

# Openbox menu entry
OPENBOX_MENU="${ROOTFS_DIR}/etc/xdg/openbox/menu.xml"
if [ -f "${OPENBOX_MENU}" ] && ! grep -q 'minecraft' "${OPENBOX_MENU}"; then
    sed -i 's|<separator/>|<item label="Minecraft Alpha 1.0">\n      <action name="Execute"><execute>st -e minecraft</execute></action>\n    </item>\n    <separator/>|' \
        "${OPENBOX_MENU}"
    echo "[+] Patched Openbox menu.xml"
fi

# Java PATH profile
cat > "${ROOTFS_DIR}/etc/profile.d/java_avoryos.sh" << ENV_EOF
export JAVA_HOME="${JAVA_HOME_GUEST}"
export PATH="\${JAVA_HOME}/bin:\${PATH}"
ENV_EOF
chmod +x "${ROOTFS_DIR}/etc/profile.d/java_avoryos.sh"
echo "[+] Wrote /etc/profile.d/java_avoryos.sh"

# ─────────────────────────────────────────────────────────────────────────────
# Optional disk expansion
# ─────────────────────────────────────────────────────────────────────────────
if [ "${EXPAND_DISK}" = "1" ]; then
    CURRENT_SIZE=$(stat -c%s "${DISK_IMG}")
    TARGET_SIZE=$((8 * 1024 * 1024 * 1024))
    if [ "${CURRENT_SIZE}" -ge "${TARGET_SIZE}" ]; then
        echo "[*] disk.img already >= 8 GiB. Skipping."
    else
        echo "[*] Expanding disk.img to 8 GiB..."
        truncate -s 8G "${DISK_IMG}"
        PART_IMG="${BUILD_DIR}/part_expand.img"
        dd if="${DISK_IMG}" of="${PART_IMG}" bs=1M skip=1 status=none
        e2fsck -f -y "${PART_IMG}" || true
        resize2fs "${PART_IMG}"
        dd if="${PART_IMG}" of="${DISK_IMG}" bs=1M seek=1 conv=notrunc status=none
        rm -f "${PART_IMG}"
        echo "[+] disk.img expanded."
    fi
fi

# ─────────────────────────────────────────────────────────────────────────────
# 4. Inject into disk.img
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "=== [4/4] Injecting into disk.img ==="

PART_IMG="${BUILD_DIR}/part_mc.img"
dd if="${DISK_IMG}" of="${PART_IMG}" bs=1M skip=1 status=none
"${POPULATE_SCRIPT}" "${PART_IMG}" "${ROOTFS_DIR}" "/"
dd if="${PART_IMG}" of="${DISK_IMG}" bs=1M seek=1 conv=notrunc status=none
rm -f "${PART_IMG}"

echo ""
echo "╔══════════════════════════════════════════════════════════════════╗"
echo "║  [SUCCESS] Minecraft Release 1.0 is ready in AvoryOS!             ║"
echo "╠══════════════════════════════════════════════════════════════════╣"
echo "║  Boot AvoryOS → open a terminal → type:  minecraft              ║"
echo "║  Custom username:  MC_USER=YourName minecraft                    ║"
echo "║  Custom RAM:       MC_RAM=1536m minecraft                        ║"
echo "╚══════════════════════════════════════════════════════════════════╝"