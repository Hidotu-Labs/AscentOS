# opengd-openal-oss.cmake
# CMake toolchain/cache-init fragment injected by build-opengd.sh via
# -C <file>.  It forces openal-soft to use only the OSS backend (/dev/dsp)
# and disables every other backend so no ALSA/PipeWire/PulseAudio headers
# are required at configure time.
#
# Usage (from build-opengd.sh):
#   cmake -C /path/to/opengd-openal-oss.cmake [rest of flags] <src>

# ── Disable all non-OSS backends ────────────────────────────────────────────
set(ALSOFT_BACKEND_ALSA       OFF CACHE BOOL "" FORCE)
set(ALSOFT_BACKEND_PIPEWIRE   OFF CACHE BOOL "" FORCE)
set(ALSOFT_BACKEND_PULSEAUDIO OFF CACHE BOOL "" FORCE)
set(ALSOFT_BACKEND_SNDIO      OFF CACHE BOOL "" FORCE)
set(ALSOFT_BACKEND_SOLARIS    OFF CACHE BOOL "" FORCE)
set(ALSOFT_BACKEND_JACK       OFF CACHE BOOL "" FORCE)
set(ALSOFT_BACKEND_COREAUDIO  OFF CACHE BOOL "" FORCE)
set(ALSOFT_BACKEND_WASAPI     OFF CACHE BOOL "" FORCE)
set(ALSOFT_BACKEND_DSOUND     OFF CACHE BOOL "" FORCE)
set(ALSOFT_BACKEND_WINMM      OFF CACHE BOOL "" FORCE)
set(ALSOFT_BACKEND_OPENSL     OFF CACHE BOOL "" FORCE)
set(ALSOFT_BACKEND_OBOE       OFF CACHE BOOL "" FORCE)
set(ALSOFT_BACKEND_PORTAUDIO  OFF CACHE BOOL "" FORCE)
set(ALSOFT_BACKEND_SDL2       OFF CACHE BOOL "" FORCE)
set(ALSOFT_BACKEND_SDL3       OFF CACHE BOOL "" FORCE)
set(ALSOFT_BACKEND_WAVE       OFF CACHE BOOL "" FORCE)

# ── Require the OSS backend ──────────────────────────────────────────────────
# openal-soft uses its own cmake/FindOSS.cmake which checks for <sys/soundcard.h>
# or <soundcard.h>.  Alpine's musl sysroot ships sys/soundcard.h so this will
# auto-detect; the REQUIRE flag turns a missing detection into a hard error so
# we know immediately if something is wrong.
set(ALSOFT_BACKEND_OSS        ON  CACHE BOOL "" FORCE)
set(ALSOFT_REQUIRE_OSS        ON  CACHE BOOL "" FORCE)

# ── Reduce build footprint ────────────────────────────────────────────────────
set(ALSOFT_UTILS              OFF CACHE BOOL "" FORCE)
set(ALSOFT_NO_CONFIG_UTIL     ON  CACHE BOOL "" FORCE)
set(ALSOFT_EXAMPLES           OFF CACHE BOOL "" FORCE)
set(ALSOFT_TESTS              OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL            OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_CONFIG     OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_HRTF_DATA  OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_AMBDEC_PRESETS OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_EXAMPLES   OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_UTILS      OFF CACHE BOOL "" FORCE)
set(ALSOFT_UPDATE_BUILD_VERSION OFF CACHE BOOL "" FORCE)

# ── EAX is Windows-only; make sure it's off ───────────────────────────────────
set(ALSOFT_EAX                OFF CACHE BOOL "" FORCE)

# ── RTKit/D-Bus not present on AscentOS ──────────────────────────────────────
set(ALSOFT_RTKIT              OFF CACHE BOOL "" FORCE)

# ── axslcc shader compiler location ──────────────────────────────────────────
# AXSLCC.cmake uses axslcc_option() (set...CACHE STATIC "" FORCE) which makes
# the variable immutable to any -D flag that arrives after cmake modules run.
# The only way to override it is via a -C cache-init file loaded before cmake
# processes any CMakeLists.  build-opengd.sh generates a patched copy of this
# file with the real path substituted for the @AXSLCC_BIN_DIR@ placeholder.
set(AXSLCC_FIND_PROG_ROOT "@AXSLCC_BIN_DIR@" CACHE STRING "" FORCE)
