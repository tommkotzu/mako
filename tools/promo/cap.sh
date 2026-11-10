#!/bin/bash
# Source me. C "command" -> win.flier-capture; W ACTION [param]; START [path]
PROMO=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BUS="-d org.gnome.Nautilus.Devel -o /org/gnome/Nautilus/Devel/window/1"
C(){ gdbus call --session $BUS -m org.gtk.Actions.Activate flier-capture "[<\"$1\">]" '{}' >/dev/null; }
W(){ gdbus call --session $BUS -m org.gtk.Actions.Activate "$1" "[${2:-}]" '{}' >/dev/null; }
# In headless.sh's private session, only the Mako on that session's bus is
# quit; your own keeps running.
STOPAPP(){
  if [ -n "${MAKO_HEADLESS:-}" ]; then
    gdbus call --session -d org.gnome.Nautilus.Devel -o /org/gnome/Nautilus/Devel \
      -m org.gtk.Actions.Activate quit '[]' '{}' >/dev/null 2>&1; sleep 0.5; return 0
  fi
  pkill -f "^$HOME/.local/nautilus-ff/bin/nautilus"; pkill -f "^$(cd "$PROMO/../.." && pwd)/_build/src/nautilus"; }
START(){ STOPAPP; sleep 1
  ("$PROMO/env.sh" "$@" > "${CAPLOG:-/dev/null}" 2>&1 &)
  for i in $(seq 1 40); do gdbus call --session $BUS -m org.gtk.Actions.List 2>/dev/null | grep -q flier-capture && { sleep 1; return 0; }; sleep 0.5; done
  echo "demo did not start"; return 1; }
