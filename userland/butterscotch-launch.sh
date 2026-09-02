#!/bin/sh
# Launch Butterscotch from its install directory so it finds game.unx.
cd /opt/butterscotch
exec /opt/butterscotch/butterscotch game.unx "$@"
