#!/bin/sh
# Minimal startw.sh for AscentOS

export XDG_RUNTIME_DIR=/tmp/wayland
mkdir -p $XDG_RUNTIME_DIR
rm -f $XDG_RUNTIME_DIR/wayland-0*

seatd -u root &
sleep 1

# Launch TinyWL with Pixman renderer for stability
WLR_RENDERER=pixman LD_PRELOAD=/lib/libgcompat.so.0 tinywl