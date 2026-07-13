#!/bin/sh
# Forkit launcher — cd into the assets directory first so that Forkit's
# relative paths (assets/test.html, assets/fonts/*.ttf, …) resolve correctly.

# Do not let SDL silently fall back to KMSDRM while a desktop session is
# running. KMSDRM takes over the scanout and opens evdev directly, which
# makes Forkit cover IceWM/Weston and steals the desktop cursor. Respect an
# explicit override so Forkit can still be run directly from a console with
# SDL_VIDEODRIVER=kmsdrm.
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
        if [ -n "${DISPLAY:-}" ]; then
            SDL_VIDEODRIVER=x11
        elif [ -n "${WAYLAND_DISPLAY:-}" ]; then
            SDL_VIDEODRIVER=wayland
        elif [ -S /tmp/.X11-unix/X0 ]; then
            # IceWM is always started on :0 by startx.sh. Recover from a
            # missing DISPLAY rather than allowing SDL to choose KMSDRM.
            DISPLAY=:0
            SDL_VIDEODRIVER=x11
        else
            echo "forkit: no X11 or Wayland desktop display found" >&2
            echo "forkit: set SDL_VIDEODRIVER=kmsdrm explicitly for console use" >&2
            exit 1
        fi
    fi

    export DISPLAY SDL_VIDEODRIVER
fi

cd /usr/share/forkit || exit 1
exec /bin/forkit.elf "$@"
