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

# Audio is powered by the Linux-compatible Intel HDA / DSP driver.
# If SDL_AUDIODRIVER is not explicitly set, SDL2 automatically chooses the best available backend (alsa/dsp).
if [ -z "${SDL_AUDIODRIVER:-}" ]; then
    if [ -c /dev/dsp ] || [ -c /dev/snd/pcmC0D0p ]; then
        export SDL_AUDIODRIVER=dsp
    fi
fi

# Yamagi falls back to the current directory when /proc/self/exe is unavailable.
cd /opt/quake2 || exit 1
export SDL_RENDER_DRIVER=software
export SDL_RENDER_VSYNC=0
export MESA_SHADER_CACHE_DISABLE=true
exec ./quake2 -portable "$@"
