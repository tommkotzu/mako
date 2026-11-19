#!/bin/bash
# Stills for the website, one per section: the window alone (rounded
# corners transparent), PNG at 2x plus a 1600 px WebP, light or dark.
# usage: PRESENT_SCHEME=dark|light site-shots.sh OUT_DIR [scene ...]
# (scenes are the functions in site-shots.py: hero split commander ...)
# Needs: the build in _build, a demo home (make-home.sh SRC /tmp/mako-home,
# Bench folder not needed). Runs in a private headless shell.
set -euo pipefail
OUT=$(readlink -f "${1:?out dir}"); shift
export SITE_OUT=${SITE_OUT:-$OUT}
PROMO=/home/tommboo/Documents/GitHub/nautilus/tools/promo
REPO=$(cd "$PROMO/../.." && pwd)
SRC=${MAKO_DEMO:-/tmp/mako-home}
WORK=$(mktemp -d /tmp/mako-present.XXXXXX)
trap 'cp "$WORK/app.log" "$OUT/app.log" 2>/dev/null; rm -rf "$WORK"' EXIT
mkdir -p "$OUT" "$WORK/shell/.config/glib-2.0/settings" "$WORK/shell/.local/share" "$WORK/shell/.cache"
# The shell runs in an empty home of its own: nothing of yours (online
# accounts, calendars, notifications) can show up in a clip. A bigger
# cursor, and no notification banners.
WALL="$WORK/wall.jpg"; cp "$SRC/Pictures/Deep Sea/Reef/coral.jpg" "$WALL"
cat > "$WORK/shell/.config/glib-2.0/settings/keyfile" <<K
[org/gnome/desktop/interface]
cursor-size=32
accent-color='purple'

enable-hot-corners=false

[org/gnome/desktop/notifications]
show-banners=false

[org/gnome/desktop/background]
picture-uri='file://$WALL'
picture-uri-dark='file://$WALL'
picture-options='zoom'
color-shading-type='vertical'
primary-color='#2b2640'
secondary-color='#15131c'
K
if [ -n "${PRESENT_SCALE:-}" ]; then
  printf '\n[org/gnome/mutter]\nexperimental-features=["scale-monitor-framebuffer"]\n' \
    >> "$WORK/shell/.config/glib-2.0/settings/keyfile"
fi
# The icons you use (orange Yaru folders otherwise, on Ubuntu).
export PRESENT_ICONS=${PRESENT_ICONS:-$(gsettings get org.gnome.desktop.interface icon-theme 2>/dev/null | tr -d "'")}
export PRESENT_OUT=$OUT PRESENT_WORK=$WORK PRESENT_SRC=$SRC PRESENT_REPO=$REPO
export HOME=$WORK/shell XDG_CONFIG_HOME=$WORK/shell/.config XDG_DATA_HOME=$WORK/shell/.local/share \
       XDG_CACHE_HOME=$WORK/shell/.cache
dbus-run-session --config-file="$PROMO/headless-bus.conf" -- bash -c '
  GSETTINGS_BACKEND=keyfile \
    gnome-shell --mode=gnome --headless --unsafe-mode --wayland --no-x11 \
    --wayland-display=mako-present --virtual-monitor 2560x1504@60 >"$PRESENT_WORK/shell.log" 2>&1 &
  SHELL_PID=$!
  for i in $(seq 1 60); do [ -S "$XDG_RUNTIME_DIR/mako-present" ] && break; sleep 0.25; done
  sleep 2
  WAYLAND_DISPLAY=mako-present GDK_BACKEND=wayland XCURSOR_SIZE=32 python3 -I "$0" "$@"; rc=$?
  kill $SHELL_PID; wait $SHELL_PID 2>/dev/null; exit $rc' "$PROMO/site-shots.py" "$@"
