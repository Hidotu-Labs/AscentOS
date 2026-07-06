#!/bin/sh
# Forkit launcher — cd into the assets directory first so that Forkit's
# relative paths (assets/test.html, assets/fonts/*.ttf, …) resolve correctly.
cd /usr/share/forkit || exit 1
exec /bin/forkit.elf "$@"
