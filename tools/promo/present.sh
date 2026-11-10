#!/bin/bash
# Records presentation clips as real screen recordings: a private headless
# GNOME Shell at 2560x1440 (plus its top bar, cropped away) and 200% scale,
# so a 1280x720 window, saved as 1920x1080; a cursor that moves, clicks and
# drags like a hand on the mouse, and animations on.
# usage: present.sh OUT_DIR [scene ...]   (no scenes = all; see present.py)
# Needs: the build in _build, a demo home (make-home.sh), ffmpeg.
set -euo pipefail
OUT=$(readlink -f "${1:?out dir}"); shift
PROMO=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$PROMO/../.." && pwd)
SRC=${MAKO_DEMO:-/tmp/mako-home}
WORK=$(mktemp -d /tmp/mako-present.XXXXXX)
trap 'cp "$WORK/app.log" "$OUT/app.log" 2>/dev/null; rm -rf "$WORK"' EXIT
mkdir -p "$OUT" "$WORK/shell/.config/glib-2.0/settings" "$WORK/shell/.local/share" "$WORK/shell/.cache"
# The shell runs in an empty home of its own: nothing of yours (online
# accounts, calendars, notifications) can show up in a clip. A bigger
# cursor, and no notification banners.
cat > "$WORK/shell/.config/glib-2.0/settings/keyfile" <<'K'
[org/gnome/desktop/interface]
cursor-size=32

enable-hot-corners=false

[org/gnome/desktop/notifications]
show-banners=false

[org/gnome/desktop/background]
picture-uri=''
picture-uri-dark=''
picture-options='none'
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
  kill $SHELL_PID; wait $SHELL_PID 2>/dev/null; exit $rc' "$PROMO/present.py" "$@"
