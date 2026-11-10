# Promo recordings and speed tests

Scripts that made the README media and the promo clips. They run the build
from `_build` in a throwaway demo home with in-memory settings, driven over
D-Bus through the `FLIER_CAPTURE` hook in `src/nautilus-window.c`.

| Script | What it does |
|---|---|
| `make-home.sh SRC [HOME]` | Builds the demo home (default `/tmp/mako-home`) from a folder of images and one video: photo folders, ratings, tags, docs, and 10k/50k/20k-tree bench folders. |
| `record.sh OUT [clip…]` | Records raw clips (no captions or fades) and encodes each to a 30 fps MP4 plus `.markers.json` (when each action happens). |
| `present.sh OUT [scene…]` | Presentation clips as real screen recordings with the cursor: `present.py` moves, clicks, right-clicks and drags like a hand on the mouse and mixes in shortcuts (tabs, columns, density, sidebar, files, quicklook, inspector, marks, tiles). The window floats with rounded corners in your icon theme, in a shell with an empty home. 1920x1080 at 30 fps from a 200% screen, with `.markers.json` (steps, plus the cursor ten times a second for zooming in). |
| `encode.py DIR OUT.mp4` | Frame folder to MP4; frames are named by ms, so timing is real. |
| `bench-suite.sh OUT.jsonl [runs]` | Times folder loads, view switches, filter and flat view, command to frame painted. |
| `bench-startup.py OUT.json [runs]` | Launch-to-window, Mako vs the system's GNOME Files, plugins off for both. |
| `summarize.py SUITE STARTUP OUT` | Medians; drops runs throttled by the compositor. |
| `headless.sh CMD…` | Runs CMD against a private headless GNOME Shell, so no other window can cover or throttle the test window. |

Example: `./headless.sh ./bench-suite.sh /tmp/bench.jsonl 7`

`record.sh` and `bench-suite.sh` stop any running Mako first. `present.sh`
runs its own private shell with an empty home, so it leaves a running Mako
alone and nothing of yours (accounts, notifications) can get into a clip.
