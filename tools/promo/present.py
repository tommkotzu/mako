#!/usr/bin/env python3
# Presentation clips for HyperFrames: real screen recordings with the cursor,
# driven like a person with a mouse (clicks, drags, drops, right-clicks).
# Run through present.sh, which starts the private shell this talks to.
#
# Each scene writes OUT/NAME.mp4 (1920x1080, 30 fps) and OUT/NAME.markers.json:
#   {"clip", "duration", "markers": [{"t", "label"}], "cursor": [[t, x, y], ...]}
# markers are when each step happens (for captions and keycaps), cursor is
# where the pointer is ten times a second (to zoom in on the action).
import json, math, os, shutil, subprocess, sys, time
from gi.repository import Gio, GLib

OUT = os.environ['PRESENT_OUT']
WORK = os.environ['PRESENT_WORK']
SRC = os.environ['PRESENT_SRC']
REPO = os.environ['PRESENT_REPO']
HOME = os.path.join(WORK, 'home')
TOP = 32                      # the shell's top bar (logical px), cropped from the clips
SCALE = float(os.environ.get('PRESENT_SCALE', 2))  # the monitor's scale: 2 gives 2560x1440 px, 1280x720 logical
APP = 'org.gnome.Nautilus.Devel'

bus = Gio.bus_get_sync(Gio.BusType.SESSION)


def call(dest, path, iface, method, sig=None, args=(), timeout=-1):
    return bus.call_sync(dest, path, iface, method, GLib.Variant(sig, args) if sig else None,
                         None, 0, timeout, None)


def set_scale():
    """The virtual monitor at SCALE (a headless shell starts it at 1)."""
    D = ('org.gnome.Mutter.DisplayConfig', '/org/gnome/Mutter/DisplayConfig', 'org.gnome.Mutter.DisplayConfig')
    serial, monitors, logical, props = call(*D, 'GetCurrentState').unpack()
    (connector, *_), modes = monitors[0][0], monitors[0][1]
    mode = next(m for m in modes if m[6].get('is-current')) if any(m[6].get('is-current') for m in modes) else modes[0]
    call(*D, 'ApplyMonitorsConfig', '(uua(iiduba(ssa{sv}))a{sv})',
         (serial, 1, [(0, 0, min(mode[5], key=lambda x: abs(x - SCALE)), 0, True,
                       [(connector, mode[0], {})])], {}))
    time.sleep(1)


set_scale()


# --- the app ----------------------------------------------------------------

app_proc = None


def C(cmd, wait=0.0):
    """A command for the capture hook (win.flier-capture)."""
    call(APP, '/org/gnome/Nautilus/Devel/window/1', 'org.gtk.Actions', 'Activate', '(sava{sv})',
         ('flier-capture', [GLib.Variant('s', cmd)], {}))
    if wait:
        time.sleep(wait)


def shell_eval(js):
    ok, out = call('org.gnome.Shell', '/org/gnome/Shell', 'org.gnome.Shell', 'Eval', '(s)', (js,)).unpack()
    if not ok:
        raise SystemExit('shell eval failed: ' + out)
    return out


# The window floats (rounded corners, shadow) in the middle of the screen.
FRAME = (40, TOP + 24, 1200, 672)   # x, y, width, height (logical px)
origin = [0, 0]                     # the window frame on screen (widget coordinates start there)


def place_window():
    x, y, w, h = FRAME
    js = ("let w = global.display.get_focus_window() || global.get_window_actors().map(a => a.meta_window)"
          ".find(m => m.get_wm_class() && m.get_wm_class().includes('Nautilus'));"
          "if (w.is_maximized ? w.is_maximized() : w.get_maximized()) { try { w.unmaximize(); } catch (e) { w.unmaximize(3); } }"
          "w.move_resize_frame(false, %d, %d, %d, %d);" % (x, y, w, h))
    shell_eval(js)
    time.sleep(0.8)
    r = json.loads(shell_eval("let w = global.display.get_focus_window(); let b = w.get_frame_rect();"
                              "JSON.stringify([b.x, b.y])"))
    if isinstance(r, str):          # Eval hands back the value JSON-encoded once more
        r = json.loads(r)
    origin[0], origin[1] = r


def start_app(location, prep=None):
    global app_proc
    stop_app()
    if os.path.exists(HOME):
        shutil.rmtree(HOME)
    # A fresh copy each scene, so drags that move files start from the same place.
    subprocess.run(['cp', '-a', SRC, HOME], check=True)
    if prep:
        prep()
    env = dict(os.environ, HOME=HOME, XDG_CONFIG_HOME=HOME + '/.config',
               XDG_DATA_HOME=HOME + '/.local/share', XDG_CACHE_HOME=HOME + '/.cache',
               GSETTINGS_BACKEND='memory', GSETTINGS_SCHEMA_DIR=REPO + '/_build/data',
               FLIER_CAPTURE='1', FLIER_CAPTURE_PRESET='1', FLIER_CAPTURE_ANIMATE='1',
               FLIER_CAPTURE_ICON_THEME=os.environ.get('PRESENT_ICONS') or 'Adwaita')
    app_proc = subprocess.Popen([REPO + '/_build/src/nautilus', '--new-window', location], env=env,
                                cwd=HOME, stdout=open(WORK + '/app.log', 'a'), stderr=subprocess.STDOUT)
    for _ in range(80):
        try:
            names = call(APP, '/org/gnome/Nautilus/Devel/window/1', 'org.gtk.Actions', 'List').unpack()[0]
            if 'flier-capture' in names:
                break
        except GLib.Error:
            pass
        time.sleep(0.25)
    else:
        sys.exit('Mako did not start')
    time.sleep(1)
    place_window()


def stop_app():
    global app_proc
    if app_proc is not None:
        app_proc.terminate()
        try:
            app_proc.wait(5)
        except subprocess.TimeoutExpired:
            app_proc.kill()
        app_proc = None
        time.sleep(0.5)


def locate(text, tries=20):
    """Screen centre (x, y) and size (w, h) of what shows TEXT ([SCOPE>]TEXT[@N])."""
    path = WORK + '/locate.txt'
    for _ in range(tries):
        if os.path.exists(path):
            os.remove(path)
        C('locate:%s:%s' % (text, path))
        for _ in range(20):
            if os.path.exists(path):
                break
            time.sleep(0.02)
        if os.path.exists(path) and open(path).read().strip():
            x, y, w, h = map(float, open(path).read().split())
            return x + origin[0], y + origin[1], w, h
        time.sleep(0.15)
    shot('not-found')
    raise SystemExit('not found: ' + text)


# --- the pointer and keys ----------------------------------------------------

rd = call('org.gnome.Mutter.RemoteDesktop', '/org/gnome/Mutter/RemoteDesktop',
          'org.gnome.Mutter.RemoteDesktop', 'CreateSession').unpack()[0]
RD = 'org.gnome.Mutter.RemoteDesktop.Session'


def rdcall(method, sig, args):
    call('org.gnome.Mutter.RemoteDesktop', rd, RD, method, sig, args)


call('org.gnome.Mutter.RemoteDesktop', rd, RD, 'Start')
time.sleep(0.5)
pos = [0.0, 0.0]
rdcall('NotifyPointerMotionRelative', '(dd)', (-5000.0, -5000.0))

track = None                  # cursor samples while recording
rec_t0 = None


def sample():
    if track is not None and (not track or time.time() - rec_t0 - track[-1][0] >= 0.1):
        # In the clip's 1920x1080 pixels.
        track.append([round(time.time() - rec_t0, 3), round(pos[0] * 1.5), round((pos[1] - TOP) * 1.5)])


def warp(x, y):
    dx, dy = x - pos[0], y - pos[1]
    if dx or dy:
        rdcall('NotifyPointerMotionRelative', '(dd)', (dx, dy))
        pos[0], pos[1] = x, y
    sample()


def move(x, y, dur=None):
    """Glide to (x, y) like a hand: eased, faster over long distances."""
    sx, sy = pos
    dist = math.hypot(x - sx, y - sy)
    if dur is None:
        dur = min(0.9, 0.28 + dist / 1800)
    steps = max(2, int(dur * 60))
    for i in range(1, steps + 1):
        t = i / steps
        e = t * t * (3 - 2 * t)                      # smoothstep
        # A slight arc, as a wrist moves.
        arc = math.sin(math.pi * t) * min(40, dist * 0.06)
        nx, ny = sx + (x - sx) * e, sy + (y - sy) * e
        if dist:
            nx += -(y - sy) / dist * arc * 0.3
            ny += (x - sx) / dist * arc * 0.3
        warp(nx, ny)
        time.sleep(dur / steps)
    warp(x, y)


def W(fx, fy):
    """A point of the window, as fractions of it."""
    return (round(FRAME[0] + fx * FRAME[2]), round(FRAME[1] + fy * FRAME[3]))


def to(target, dx=0, dy=0, dur=None):
    x, y, w, h = locate(target) if isinstance(target, str) else (*target, 0, 0)
    move(x + dx, y + dy, dur)
    return x + dx, y + dy


BUTTONS = {'left': 272, 'right': 273}


def button(down, which='left'):
    rdcall('NotifyPointerButton', '(ib)', (BUTTONS[which], down))


def click(target=None, which='left', double=False, pause=0.25, **kw):
    if target is not None:
        to(target, **kw)
    time.sleep(0.12)
    for i in range(2 if double else 1):
        button(True, which)
        time.sleep(0.07)
        button(False, which)
        time.sleep(0.09)
    time.sleep(pause)


def drag(src, dst, hold=0.5, dur=1.0, mods=(), snap=None, **kw):
    """Press on SRC, pull past the drag threshold, glide to DST, hover, drop."""
    sx, sy = to(src) if not isinstance(src, tuple) else (move(*src) or src)
    time.sleep(0.15)
    for m in mods:
        key_down(m)
    button(True)
    time.sleep(0.12)
    move(sx + 6, sy + 4, 0.12)                    # past GTK's drag threshold
    move(sx + 14, sy + 8, 0.12)
    dx, dy = (dst if isinstance(dst, tuple) else locate(dst)[:2])
    move(dx + kw.get('dx', 0), dy + kw.get('dy', 0), dur)
    time.sleep(hold)                              # let the drop target show
    if snap:
        shot(snap)
    button(False)
    for m in mods:
        key_up(m)
    time.sleep(0.4)


KEYS = {'esc': 1, 'enter': 28, 'alt': 56, 'ctrl': 29, 'shift': 42, 'space': 57, 'backspace': 14,
        'up': 103, 'down': 108, 'left': 105, 'right': 106, 'tab': 15}
# Letters, digits and F keys (Linux input codes).
for i, row in enumerate(('qwertyuiop', 'asdfghjkl', 'zxcvbnm')):
    for j, c in enumerate(row):
        KEYS[c] = (16, 30, 44)[i] + j
for i in range(10):
    KEYS[str((i + 1) % 10)] = 2 + i
for i in range(10):
    KEYS['f%d' % (i + 1)] = 59 + i


def key_down(k):
    rdcall('NotifyKeyboardKeycode', '(ub)', (KEYS[k], True))


def key_up(k):
    rdcall('NotifyKeyboardKeycode', '(ub)', (KEYS[k], False))


def key(*ks):
    for k in ks:
        key_down(k)
        time.sleep(0.03)
    for k in reversed(ks):
        key_up(k)
    time.sleep(0.2)


def type_text(text):
    for c in text:
        key('shift', c.lower()) if c.isupper() else key(' ' if False else ('space' if c == ' ' else c))
        time.sleep(0.05)


def scroll(steps, dy=1):
    for _ in range(abs(steps)):
        rdcall('NotifyPointerAxisDiscrete', '(ui)', (0, dy if steps > 0 else -dy))
        time.sleep(0.08)


def s(t):
    time.sleep(t)


# --- recording ----------------------------------------------------------------

markers = []
clip = None


def mark(label):
    if rec_t0 is not None:
        markers.append({'t': round(time.time() - rec_t0, 3), 'label': label})


def rec(name):
    global clip, rec_t0, track, markers
    clip, markers, track = name, [], []
    raw = '%s/%s.raw' % (WORK, name)
    ok, path = call('org.gnome.Shell.Screencast', '/org/gnome/Shell/Screencast',
                    'org.gnome.Shell.Screencast', 'Screencast', '(sa{sv})',
                    (raw, {'draw-cursor': GLib.Variant('b', True),
                           'framerate': GLib.Variant('i', 30)})).unpack()
    if not ok:
        sys.exit('screencast failed')
    rec_t0 = time.time()
    sample()
    return path


def stop(path):
    global rec_t0, track
    s(0.4)
    duration = time.time() - rec_t0
    call('org.gnome.Shell.Screencast', '/org/gnome/Shell/Screencast',
         'org.gnome.Shell.Screencast', 'StopScreencast')
    s(0.8)
    out = '%s/%s.mp4' % (OUT, clip)
    # Frames come only when something changes: fill to a steady 30 fps.
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', path,
                    '-vf', 'crop=2560:1440:0:%d,scale=1920:1080:flags=lanczos,fps=30' % (TOP * SCALE),
                    '-t', '%.3f' % duration,
                    '-c:v', 'libx264', '-preset', 'slow', '-crf', '16', '-pix_fmt', 'yuv420p',
                    '-movflags', '+faststart', out], check=True)
    os.remove(path)
    with open('%s/%s.markers.json' % (OUT, clip), 'w') as f:
        json.dump({'clip': clip + '.mp4', 'duration': round(duration, 3), 'size': [1920, 1080],
                   'markers': markers, 'cursor': track}, f, indent=1)
    print('wrote', out, '%.1f s' % duration, flush=True)
    rec_t0, track = None, None


def shot(name):
    """A still of the screen as it is (for checking a scene)."""
    path = '%s/%s.png' % (OUT, name)
    call('org.gnome.Shell.Screenshot', '/org/gnome/Shell/Screenshot', 'org.gnome.Shell.Screenshot',
         'Screenshot', '(bbs)', (True, False, path))


# --- scenes ---------------------------------------------------------------------

P = lambda *a: os.path.join(HOME, *a)
DEEP = ('Pictures', 'Deep Sea')


def scene_tabs():
    """Tabs: drag to reorder, drag onto a side of the view to split, drag into another tile."""
    start_app(P(*DEEP, 'Sharks'))
    C('act:win.view-mode(uint32 4)', 0.5)
    for folder in ('Reef', 'Bettas', 'Moodboard'):
        C('act:win.new-tab', 0.6)
        C('go:' + P(*DEEP, folder), 0.6)
        C('act:win.view-mode(uint32 4)', 0.4)
    move(*W(.5, .55), 0.1)
    s(1)
    path = rec('tabs')
    s(0.8)
    mark('drag tab: reorder')
    drag('AdwTabBar>Moodboard', 'AdwTabBar>Reef', dx=-40, hold=0.6, dur=0.9)
    s(0.8)
    mark('drag tab: split right')
    drag('AdwTabBar>Bettas', W(.88, .5), hold=0.9, dur=1.1)
    s(1.2)
    mark('drag tab: split down')
    drag('AdwTabBar>Reef', W(.3, .8), hold=0.9, dur=1.1)
    s(1.2)
    mark('drag tab: into another tile')
    drag('AdwTabBar>Moodboard', 'AdwTabBar>Bettas', dx=70, hold=0.7, dur=1.0)
    move(*W(.68, .6))
    s(1.5)
    stop(path)


def menu(item, opener='View (Alt'):
    """Open a menu with a click and pick ITEM."""
    click(opener, pause=0.5)
    click(item, pause=0.3)


def press(k, label=None, pause=0.55):
    """A key or combo ("ctrl+alt+4"), marked for the keycaps ("key:Ctrl+Alt+4")."""
    keys = k.split('+')
    mark('key:' + (label or '+'.join(x.upper() if len(x) == 1 or x[0] == 'f' and x[1:].isdigit() else x.capitalize()
                                     for x in keys)))
    key(*keys)
    s(pause)


def scene_columns():
    """Miller columns: picked from the View menu, walked with the arrow keys, edges dragged."""
    start_app(HOME)
    C('act:win.view-mode(uint32 4)', 0.5)
    move(*W(.57, .5), 0.1)
    s(1)
    path = rec('columns')
    s(0.6)
    mark('view menu: columns')
    menu('Columns')
    s(0.8)
    mark('click: Pictures')
    click('nautilus-columns>Pictures', pause=0.8)
    move(*W(.62, .62))                  # the hand leaves the mouse for the keys
    s(0.4)
    press('right', pause=0.8)           # into Pictures: Deep Sea
    press('right', pause=0.8)           # into Deep Sea: Bettas
    for _ in range(3):
        press('down')                   # Moodboard, Reef, Sharks
    s(0.3)
    press('right', pause=0.8)           # into Sharks
    for _ in range(2):
        press('down')                   # hammerhead, mako-shark
    s(0.6)
    press('up')
    s(0.4)
    press('left', pause=0.8)            # back to Deep Sea
    press('up')                         # Reef
    press('right', pause=0.8)           # into Reef
    for _ in range(3):
        press('down')
    s(0.5)
    x, y, w, h = locate('.miller-parent')
    mark('drag edge: one column')
    drag((x + w / 2 - 2, y), (x + w / 2 + 90, y + 10), hold=0.3, dur=0.9)
    s(0.6)
    x, y, w, h = locate('.miller-parent')
    mark('alt+drag edge: all columns')
    drag((x + w / 2 - 2, y + 60), (x + w / 2 - 40, y + 70), hold=0.3, dur=0.9, mods=('alt',))
    s(0.6)
    move(*W(.62, .62))
    press('left', pause=0.8)
    press('left', pause=0.8)
    press('down')
    press('right', pause=1.2)
    stop(path)

def scene_density():
    """Density from the bottom bar's View menu, and the sidebar's from its menu."""
    start_app(P(*DEEP, 'Moodboard'))
    C('act:win.view-mode(uint32 1)', 0.5)
    move(*W(.57, .5), 0.1)
    s(1)
    path = rec('density')
    s(0.6)
    for d in ('Compact', 'Dense', 'Extra Dense'):
        mark('density: ' + d)
        menu(d)
        s(1.4)
    mark('view menu: grid')
    menu('Grid')
    s(1.2)
    for d in ('Dense', 'Comfortable'):
        mark('density: ' + d)
        menu(d)
        s(1.4)
    mark('sidebar right-click: Sidebar Density')
    click(W(.05, .93), which='right', pause=0.6)
    click('Sidebar Density', pause=0.6)
    click('Extra Dense', pause=0.3)
    s(1.4)
    click(W(.05, .93), which='right', pause=0.6)
    click('Sidebar Density', pause=0.6)
    click('Compact', pause=0.3)
    move(*W(.57, .5))
    s(1.2)
    stop(path)


def scene_sidebar():
    """The sidebar's right-click menus: groups on and off, colors, order."""
    start_app(P(*DEEP, 'Sharks'))
    C('act:win.view-mode(uint32 4)', 0.5)
    move(*W(.57, .5), 0.1)
    s(1)
    path = rec('sidebar')
    s(0.6)
    mark('right-click empty sidebar: hide Tags')
    click(W(.05, .93), which='right', pause=0.7)
    click('Tags', pause=0.3)
    s(1.2)
    mark('right-click empty sidebar: show Tags')
    click(W(.05, .93), which='right', pause=0.7)
    click('Tags', pause=0.3)
    s(1.2)
    mark('right-click group header: color')
    click('placessidebar>Places', which='right', pause=0.7)
    click('Teal', pause=0.3)
    s(1)
    mark('right-click item: color')
    click('placessidebar>Pictures', which='right', pause=0.7)
    click('Purple', pause=0.3)
    s(1)
    click('placessidebar>Videos', which='right', pause=0.7)
    click('Orange', pause=0.3)
    s(1)
    mark('drag group header: Tags above Places')
    drag('placessidebar>Tags', 'placessidebar>Places', dy=-6, hold=0.6, dur=1.0)
    s(1.2)
    mark('right-click group header: Move Down')
    click('placessidebar>Tags', which='right', pause=0.7)
    click('Move Down', pause=0.3)
    move(*W(.57, .5))
    s(1.2)
    stop(path)


def scene_files():
    """Files: drag photos from one tile into a folder in the other, a folder onto the sidebar."""
    def prep():
        # New photos in Downloads, so nothing clashes where they go.
        for i, (folder, name) in enumerate((('Reef', 'coral.jpg'), ('Reef', 'jellyfish.jpg'),
                                            ('Reef', 'crab-taxi.jpg'), ('Sharks', 'hammerhead.png'))):
            ext = os.path.splitext(name)[1]
            shutil.copy2(P(*DEEP, folder, name), P('Downloads', 'dive-%04d%s' % (412 + i, ext)))
    start_app(P('Downloads'), prep)
    C('act:win.view-mode(uint32 4)', 0.5)
    C('act:win.split-right', 0.8)
    C('go:' + P(*DEEP), 0.8)
    C('act:win.view-mode(uint32 4)', 0.8)
    move(*W(.57, .6), 0.1)
    s(1)
    path = rec('files')
    s(0.6)
    mark('click, ctrl+click: pick photos')
    click('dive-0412.jpg', dy=-40, pause=0.3)
    key_down('ctrl')
    click('dive-0413.jpg', dy=-40, pause=0.2)
    click('dive-0414.jpg', dy=-40, pause=0.2)
    key_up('ctrl')
    s(0.5)
    mark('drag: into Reef in the other tile')
    # A short hover: a longer one opens the folder (spring-loaded).
    drag(locate('dive-0414.jpg')[:2], 'nautilus-grid-view>Reef', dy=-40, hold=0.1, dur=1.0)
    s(1.2)
    mark('drag folder: onto the sidebar for a bookmark')
    drag('nautilus-grid-view>Reef', 'placessidebar>Places', dy=-18, hold=1.2, dur=1.3)
    s(1.5)
    mark('click: the new bookmark')
    click('placessidebar>Reef', pause=0.1)
    move(*W(.57, .6))            # off the row before its tooltip (a path) shows
    s(2)
    stop(path)

def scene_quicklook():
    """Quick Look on four photos: picked with the mouse, looked at with the keys."""
    start_app(P(*DEEP, 'Moodboard'))
    C('act:win.view-mode(uint32 4)', 0.5)
    move(*W(.6, .7), 0.1)
    s(1)
    path = rec('quicklook')
    s(0.6)
    mark('click, ctrl+click: four photos')
    click('mako-shark.jpg', dy=-40, pause=0.3)
    key_down('ctrl')
    for name in ('jellyfish.jpg', 'nautilus-octopus.jpg', 'coral.jpg'):
        click(name, dy=-40, pause=0.2)
    key_up('ctrl')
    move(*W(.6, .75))
    s(0.5)
    press('space', pause=2.0)
    mark('point: jellyfish')
    move(*W(.5, .3))                    # Quick Look is its own window
    s(0.4)
    press('w', pause=0.6)               # zoom in where the pointer is
    press('w', pause=0.9)
    press('right', pause=0.6)
    press('right', pause=0.8)
    press('q', pause=1.0)               # fit all four again
    press('5', pause=1.0)               # rate all four
    press('tab', pause=1.6)             # full screen
    press('tab', pause=1.0)
    press('space', pause=1.2)
    stop(path)


def scene_inspector():
    """The inspector: Alt+E, follows the selection, its grip dragged to another side."""
    start_app(P(*DEEP, 'Reef'))
    C('act:win.view-mode(uint32 4)', 0.5)
    move(*W(.6, .7), 0.1)
    s(1)
    path = rec('inspector')
    s(0.6)
    mark('click: coral.jpg')
    click('coral.jpg', dy=-40, pause=0.4)
    press('alt+e', pause=1.4)
    for name in ('jellyfish.jpg', 'crab-taxi.jpg', 'plankton-chart.jpg'):
        mark('click: ' + name)
        click(name, dy=-40, pause=1.2)
    mark('drag grip: to the bottom')
    drag('.inspector-grip', W(.6, .9), hold=0.8, dur=1.2)
    s(1.4)
    mark('drag grip: to the left')
    drag('.inspector-grip', W(.3, .5), hold=0.8, dur=1.2)
    s(1.4)
    mark('drag grip: back to the right')
    drag('.inspector-grip', W(.95, .5), hold=0.8, dur=1.2)
    s(1.2)
    move(*W(.5, .7))
    press('alt+e', pause=1.2)
    stop(path)


def scene_marks():
    """Ratings and tags: a shortcut, the right-click stars, Alt to see them, a tag, the tag's files."""
    start_app(P(*DEEP, 'Bettas'))
    C('act:win.view-mode(uint32 4)', 0.5)
    move(*W(.6, .7), 0.1)
    s(1)
    path = rec('marks')
    s(0.6)
    mark('click: betta-red.jpg')
    click('betta-red.jpg', dy=-40, pause=0.4)
    press('ctrl+alt+4', pause=1.4)
    mark('right-click: 5 stars')
    click('betta-lilac.jpg', which='right', dy=-40, pause=0.8)
    click('5 stars', pause=0.4)
    s(1)
    mark('hold Alt: stars on every tile')
    key_down('alt')
    s(2.2)
    key_up('alt')
    s(0.5)
    mark('click: betta-violet.jpg')
    click('betta-violet.jpg', dy=-40, pause=0.4)
    press('ctrl+alt+a', pause=1.2)
    mark('click: tag Print')
    click('dialog>Print', pause=0.8)
    press('esc', pause=1.0)
    mark('click sidebar tag: Print')
    click('placessidebar>Print', pause=0.2)
    move(*W(.6, .7))
    s(2.5)
    stop(path)


def scene_tiles():
    """Tiles from the keyboard, folders from the sidebar, a file dragged across, the command list."""
    start_app(P(*DEEP, 'Sharks'))
    C('act:win.view-mode(uint32 4)', 0.5)
    move(*W(.6, .7), 0.1)
    s(1)
    path = rec('tiles')
    s(0.6)
    press('f3', pause=1.0)
    mark('click sidebar: Videos')
    click('placessidebar>Videos', pause=0.2)
    move(*W(.6, .4))                    # away, before the path tooltip
    s(0.8)
    press('shift+f3', pause=1.0)
    mark('click sidebar: Documents')
    click('placessidebar>Documents', pause=0.2)
    move(*W(.6, .85))
    s(0.8)
    mark('drag: a video into Documents')
    drag(locate('sardine-school.mp4')[:2], W(.6, .85), dy=-30, hold=0.6, dur=1.2)
    s(1)
    press('alt+t', pause=0.9)
    press('alt+t', pause=0.9)
    press('alt+3', pause=1.2)
    press('ctrl+k', pause=1.0)
    mark('type: insp')
    type_text('insp')
    s(1.0)
    press('enter', pause=1.6)
    press('alt+e', pause=1.2)
    stop(path)


def scene_probe():
    """Not a clip: a screenshot and some locations, to aim the others."""
    start_app(P(*DEEP, 'Sharks'))
    C('act:win.new-tab', 0.6)
    C('go:' + P(*DEEP, 'Reef'), 1)
    shot('probe')
    for t in sys.argv[2:] if len(sys.argv) > 2 else ['AdwTabBar>Reef', 'Sharks', 'view-grid-symbolic']:
        try:
            print(t, locate(t, tries=3), flush=True)
        except SystemExit as e:
            print(e, flush=True)


SCENES = [n[6:] for n in list(globals()) if n.startswith('scene_') and n != 'scene_probe']

if __name__ == '__main__':
    os.makedirs(OUT, exist_ok=True)
    wanted = sys.argv[1:2] if len(sys.argv) > 1 else SCENES
    try:
        for name in wanted:
            globals()['scene_' + name]()
    finally:
        stop_app()
