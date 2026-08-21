#!/bin/sh
# Select the desktop backend explicitly.  This prevents SDL from taking over
# the DRM scanout while Quake II is launched from X11 or Wayland, while still
# allowing it to run directly from a text console.
if [ -z "${SDL_VIDEODRIVER:-}" ]; then
    case "${XDG_SESSION_TYPE:-}" in
        wayland)
            [ -z "${WAYLAND_DISPLAY:-}" ] || SDL_VIDEODRIVER=wayland
            ;;
        x11)
            [ -z "${DISPLAY:-}" ] || SDL_VIDEODRIVER=x11
            ;;
    esac

    if [ -z "${SDL_VIDEODRIVER:-}" ]; then
        if [ -n "${WAYLAND_DISPLAY:-}" ]; then
            SDL_VIDEODRIVER=wayland
        elif [ -n "${DISPLAY:-}" ]; then
            SDL_VIDEODRIVER=x11
        elif [ -S /tmp/.X11-unix/X0 ]; then
            DISPLAY=:0
            SDL_VIDEODRIVER=x11
        else
            SDL_VIDEODRIVER=kmsdrm
        fi
    fi
    export DISPLAY SDL_VIDEODRIVER
fi

# Disable audio completely.  The OSS /dev/dsp backend initialises but then
# blocks in the SDL audio callback thread waiting for the ring buffer, causing
# the game to hang after "SDL audio initialized."  Use the dummy driver so SDL
# never opens any audio device and Yamagi skips all sound processing.
export SDL_AUDIODRIVER=dummy

# Yamagi falls back to the current directory when /proc/self/exe is unavailable.
cd /opt/quake2 || exit 1
export SDL_RENDER_DRIVER=software
export SDL_RENDER_VSYNC=0
export MESA_SHADER_CACHE_DISABLE=true
exec ./quake2 -portable "$@"
