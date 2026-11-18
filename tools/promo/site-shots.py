"""Stills for the website: one per section, in the scheme PRESENT_SCHEME."""
import os, sys
sys.path.insert(0, os.environ['PRESENT_REPO'] + '/tools/promo')
import present as p
from PIL import Image
SCHEME = os.environ.get('PRESENT_SCHEME', 'dark')
DEST = os.environ['SITE_OUT'] + '/' + SCHEME
os.makedirs(DEST + '/web', exist_ok=True)
DEEP = ('Pictures', 'Deep Sea')
WIN = '/org/gnome/Nautilus/Devel/window/1'
only = sys.argv[1:]

def act(name, *param):
    p.call(p.APP, WIN, 'org.gtk.Actions', 'Activate', '(sava{sv})', (name, list(param), {}))
def app_act(name):
    p.call(p.APP, '/org/gnome/Nautilus/Devel', 'org.gtk.Actions', 'Activate', '(sava{sv})', (name, [], {}))
def view(n):
    p.C('act:win.view-mode(uint32 %d)' % n, 1.0)
def cut(im):
    """The window alone: its frame, with the rounded corners transparent."""
    from PIL import ImageDraw
    x, y, w, h = p.FRAME
    k = p.SCALE
    im = im.convert('RGBA').crop((int(x * k), int(y * k), int((x + w) * k), int((y + h) * k)))
    big = Image.new('L', (im.width * 4, im.height * 4), 0)
    ImageDraw.Draw(big).rounded_rectangle((0, 0, big.width - 1, big.height - 1), radius=int(15 * k * 4), fill=255)
    im.putalpha(big.resize(im.size, Image.LANCZOS))
    return im

def snap(name):
    p.s(0.4)
    p.shot('_raw')
    im = cut(Image.open(p.OUT + '/_raw.png'))
    im.save(DEST + '/' + name + '.png')
    im.resize((1600, round(im.height * 1600 / im.width)), Image.LANCZOS).save(DEST + '/web/' + name + '.webp', quality=86)
    print('shot', SCHEME, name, flush=True)

def scene(f):
    if not only or f.__name__ in only:
        try:
            f()
        except Exception as e:
            print('FAILED', f.__name__, e, flush=True)
    return f

@scene
def hero():
    p.start_app(p.P(*DEEP, 'Reef')); view(4)
    p.click('coral.jpg', dy=-40, pause=0.4); p.key('alt', 'e'); p.s(1.2)
    p.move(*p.W(.5, .95), 0.1); snap('01-hero')

@scene
def split():
    p.start_app(p.P(*DEEP, 'Sharks')); view(4)
    p.key('ctrl', 't'); p.s(0.5); p.C('go:' + p.P(*DEEP, 'Bettas'), 1)
    act('split-right'); p.s(1); p.C('go:' + p.P(*DEEP, 'Reef'), 1.2)
    p.move(*p.W(.5, .95), 0.1); snap('02-split')

@scene
def commander():
    p.start_app(p.P(*DEEP)); view(4)
    p.key('ctrl', 'e'); p.s(0.6); p.type_text('shark'); p.s(1.5); snap('03-commander')

@scene
def minimal():
    p.start_app(p.P(*DEEP, 'Bettas')); view(4)
    p.key('alt', 'q'); p.s(0.5); p.key('alt', 'shift', 'q'); p.s(0.8)
    p.move(*p.W(.5, .95), 0.1); snap('04-minimal')

@scene
def views():
    p.start_app(p.P(*DEEP)); p.move(*p.W(.5, .95), 0.1)
    view(1); snap('05-list')
    view(2); p.click('Reef', pause=0.3); p.key('right'); p.s(0.6); p.key('down'); p.key('down'); p.key('down')
    p.key('down'); p.key('right'); p.s(1); snap('06-tree')
    p.C('go:' + p.P(*DEEP, 'Reef'), 1); view(3); p.click('coral.jpg', pause=1.2); snap('07-columns')
    p.C('go:' + p.P(*DEEP), 1); view(4); snap('08-grid')

@scene
def inspector():
    p.start_app(p.P(*DEEP, 'Sharks')); view(1)
    p.click('mako-shark.jpg', pause=0.4); p.key('alt', 'e'); p.s(1.4)
    p.move(*p.W(.5, .95), 0.1); snap('09-inspector')

@scene
def marks():
    p.start_app(p.P(*DEEP, 'Moodboard')); view(1)
    p.click('Size', which='right', pause=0.8); p.click('Rating', pause=1)
    p.click('Size', which='right', pause=0.8); p.click('Tags', pause=1.2)
    p.move(*p.W(.5, .95), 0.1); snap('10-marks')

@scene
def colors():
    p.start_app(p.P(*DEEP)); view(4)
    p.key('ctrl', 'a'); p.s(0.3); p.click('Reef', which='right', dy=-40, pause=1)
    p.click('Rainbow', pause=1); p.click(p.W(.6, .85), pause=0.6)
    p.move(*p.W(.5, .95), 0.1); snap('11-colors')

@scene
def groups():
    p.start_app(p.P(*DEEP, 'Moodboard')); view(1)
    p.click('Name', pause=0.2)
    p.move(*p.W(.5, .95), 0.1)
    p.key('ctrl', 'g'); p.s(0.6); p.key('ctrl', 'g'); p.s(1.2); snap('12-group-letter')
    p.key('ctrl', 'g'); p.s(1.2); snap('12-group-rating')

@scene
def flat():
    p.start_app(p.P(*DEEP)); view(1)
    p.key('alt', 'r'); p.s(2); p.move(*p.W(.5, .95), 0.1); snap('13-flat')

@scene
def looks():
    p.start_app(p.P(*DEEP, 'Reef')); view(4); p.move(*p.W(.5, .95), 0.1)
    p.C('set:window-opacity=80', 1.2); snap('14-see-through')
    p.C('set:window-opacity=100', 0.3)
    p.C('go:' + p.P(*DEEP, 'Moodboard'), 1); view(1)
    p.C("set:density='extra-dense'", 1.2); snap('15-dense')
    p.C("set:density='comfortable'", 0.3); p.C('set:alternate-rows=true', 1.2); snap('16-stripes')
    p.C('set:alternate-rows=false', 0.3)
    p.C('go:' + p.P(*DEEP, 'Reef'), 1); view(4); act('split-right'); p.s(1)
    p.C('go:' + p.P(*DEEP, 'Sharks'), 1)
    p.C('set:framed-panels=true', 1.2); snap('17-framed')
    for t in ('nord', 'kids', 'fog'):
        p.C("set:color-theme='%s'" % t, 1.4); snap('18-theme-' + t)

@scene
def prefs():
    p.start_app(p.P(*DEEP)); view(4)
    app_act('preferences'); p.s(1.5)
    p.click('Appearance', pause=1.2); snap('19-prefs-themes')
    p.click('Organize', pause=1.2); snap('20-prefs-organize')
    p.click('Shortcuts', pause=1.2); snap('21-prefs-shortcuts')

@scene
def hints():
    p.start_app(p.P(*DEEP, 'Reef')); view(4); p.move(*p.W(.5, .95), 0.1)
    p.C('set:shortcut-hints=true', 0.5)
    p.key('alt', '1'); p.s(0.35); snap('22-hint')
    view(4); p.s(1.5)
    p.to('Show Inspector'); p.s(2.2); snap('23-tooltip')

@scene
def quicklook():
    p.start_app(p.P(*DEEP, 'Moodboard')); view(4)
    p.click('coral.jpg', dy=-40, pause=0.3); p.key('ctrl', 'a'); p.s(0.3); p.key('space'); p.s(2.2)
    p.move(*p.W(.5, .97), 0.1); snap('24-quicklook')

@scene
def typing():
    p.start_app(p.P(*DEEP, 'Moodboard')); view(4)
    p.click(p.W(.6, .9), pause=0.3); p.type_text('betta'); p.s(1.4)
    p.move(*p.W(.5, .95), 0.1); snap('25-typing')

p.stop_app()
