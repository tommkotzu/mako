#!/bin/bash
# Runs a command against a private headless GNOME Shell (own D-Bus session,
# virtual 1600x1000 monitor), so nothing can cover or throttle the window.
# usage: headless.sh COMMAND [ARGS...]
exec dbus-run-session --config-file="$(dirname "$(readlink -f "$0")")/headless-bus.conf" -- bash -c '
  GSETTINGS_BACKEND=memory gnome-shell --mode=gnome --headless --wayland --no-x11 --wayland-display=mako-headless --virtual-monitor 1600x1000@60 >/tmp/mako-headless.log 2>&1 &
  SHELL_PID=$!
  for i in $(seq 1 60); do [ -S "$XDG_RUNTIME_DIR/mako-headless" ] && break; sleep 0.25; done
  sleep 2
  MAKO_HEADLESS=1 WAYLAND_DISPLAY=mako-headless GDK_BACKEND=wayland "$@"; rc=$?
  kill $SHELL_PID; wait $SHELL_PID 2>/dev/null; exit $rc' headless "$@"
