"""The uniapi logo: writes its SVGs, renders its PNGs and packs its Windows icon.

    python doc/logo/make.py
    python doc/logo/make.py --name-font <Orbitron.ttf>   # the name's outlines, once (name.json)

The mark: a tile split down the middle (left native, right web) with a box across the seam in
the crossed colours. Round the box each half is a bracket: the left half a [ whose rounded
corners make it a C (C++), the right half a ] whose square top-right corner and rounded
bottom make it a J (JS). Together, [ ]: CJ.

Three coloured variants: light (white and uniapi blue), dark (ink and uniapi blue) and gray (a
mid grey and a dark grey), one drawing at every size. Each has an outline round the tile, over
the background: its two colours mixed half and half, at 50 % opacity. In tile units it widens
as the icon shrinks, so the small sizes keep a visible line.

A fourth, embossed: no colours, only translucent tints and a subtle relief, so it takes the
background's colour and looks pressed into it: the C a shade lighter than the background, the J
a shade darker, the box's halves crossed; the letters raised and split at the seam, the box sunk
whole, a soft shadow outside. No outline: one mixed from its tints would be the background's own
colour; the relief gives it its edge.

The word beside the logo is always "uni" in its left colour and "api" in its right colour (the
embossed logo's: its lighter and darker tints); doc/logo/colors.json lists them per version. It
is lettered in Orbitron (regular weight, letters 0.08 em apart), whose squared letters with
rounded corners are built as the C and J are; kept as outlines, doc/logo/name.json, so no font
is needed: --name-font reads them from an Orbitron file (google/fonts, SIL Open Font License)
kept outside the repository.
Where the word sits on a light background, the dark logo goes with it (ink "uni"), on a dark one
the light logo (white "uni"). Beside text the name's short letters (u, n, a) are
exactly as tall as the text's capitals and its baseline is the text's, fitted to the screen's
pixels so they share their top and bottom pixel rows: doc/logo/name-fit.js does it, in every
app and page.

Writes doc/logo/uniapi[-dark|-gray|-emboss].svg, the logo with its name
doc/logo/uniapi-name[-dark|-gray|-emboss].svg, doc/logo/colors.json and doc/logo/name.js; with
Microsoft Edge (Windows) renders the PNGs,
doc/logo/png/uniapi[-dark|-gray|-emboss]-<size>.png (16 ... 512), and packs
doc/logo/uniapi.ico (16, 32, 48, 256) with Pillow.
"""
import os
import shutil
import subprocess
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

VARIANTS = {
    '': {'a': '#ffffff', 'b': '#1f8fe0'},
    '-dark': {'a': '#2a2f37', 'b': '#1f8fe0'},
    '-gray': {'a': '#a7adb6', 'b': '#5c636d'},
}

# The outline's opacity, round the tile over the background; its colour is the logo's two colours
# mixed half and half (mix()).
EDGE_ALPHA = 0.5


def mix(a, b):
    # Two colours (#rrggbb) half and half.
    return '#' + ''.join(f'{round((int(a[i:i + 2], 16) + int(b[i:i + 2], 16)) / 2):02x}' for i in (1, 3, 5))

# Clear space round the tile, in tile units (of 100) each side: the tile sits a little inside the
# icon, as app icons do.
PAD = 6

# The tile: rounded but for its top-right corner (radius R).
R = 22
TILE = 'M22 0 H100 V78 A22 22 0 0 1 78 100 H22 A22 22 0 0 1 0 78 V22 A22 22 0 0 1 22 0 Z'
# The box across the seam: the brackets' strokes are as thick as the corners' radius, at the
# sides and at the top and bottom.
BOX = (R, R, 50 - R, 100 - 2 * R)


def edge_for(size):
    # The outline's width outside the tile, in tile units, for the size the icon is shown at: about
    # 0.7 px at 16 and 32, 0.85 at 64, 1.4 at 128 (and in proportion above).
    return 5 if size <= 16 else 2.5 if size <= 32 else 1.5 if size <= 64 else 1.2


# The embossed version's tints: the C (left) lighter than the background, the J (right) darker;
# the box's halves crossed (darker inside the C, lighter inside the J), as the coloured logos are.
EMBOSS_LIGHT = 0.22
EMBOSS_DARK = 0.16
# Its relief, subtle: the raised letters' rims (white top-left, black bottom-right; the sunk box's
# the other way round) and the soft shadow they cast, outside them only.
EMBOSS_HIGHLIGHT = 0.24
EMBOSS_SHADE = 0.12
EMBOSS_SHADOW = 0.16


def emboss(size=512, shown=None):
    # The embossed mark: no colours of its own, only translucent tints and a subtle relief, so it
    # takes the background's colour and looks pressed into it. Its edge comes from the relief, not
    # an outline (an outline mixed from its tints would be the background's own colour). The
    # brackets (the tile round the box, split at the seam by a groove, so they are two letters)
    # stand up: lit at their top-left rims, shaded at their bottom-right ones, casting a soft
    # shadow; the box sinks in whole (no groove through it), lit and shaded the other way round.
    # Their faces are tinted: the C lighter than the background, the J darker, the box's halves
    # crossed. Rims and groove are about a pixel at least at every size.
    px = 112 / (shown or size)          # one pixel in tile units at the size it is shown
    bevel = max(1.6, 0.9 * px)
    groove = max(1.6, 1.1 * px)
    drop = max(1.2, 0.8 * px)
    x, y, w, h = BOX
    lo, hi = 50 - groove / 2, 50 + groove / 2
    view = f'{-PAD} {-PAD} {100 + 2 * PAD} {100 + 2 * PAD}'
    area = f'x="{-PAD}" y="{-PAD}" width="{100 + 2 * PAD}" height="{100 + 2 * PAD}"'

    def rim(name, shape, dx, dy):
        # The part of `shape` that a copy moved by (dx, dy) leaves uncovered: its rim on the far side.
        return (f'<mask id="{name}" maskUnits="userSpaceOnUse" {area}>'
                f'<use href="#{shape}" fill="#fff"/><use href="#{shape}" fill="#000" transform="translate({dx:g} {dy:g})"/></mask>')

    b = bevel
    masks = (rim('raisedTL', 'raised', b, b) + rim('raisedBR', 'raised', -b, -b)
             + rim('sunkTL', 'sunk', b, b) + rim('sunkBR', 'sunk', -b, -b)
             # the shadow falls only where the brackets aren't
             + f'<mask id="around" maskUnits="userSpaceOnUse" {area}><rect {area} fill="#fff"/><use href="#raised" fill="#000"/></mask>')
    return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="{view}" width="{size}" height="{size}">
<title>uniapi</title>
<defs>
<clipPath id="halves"><rect x="{-PAD}" y="{-PAD}" width="{lo + PAD:g}" height="{100 + 2 * PAD}"/><rect x="{hi:g}" y="{-PAD}" width="{100 + PAD - hi:g}" height="{100 + 2 * PAD}"/></clipPath>
<clipPath id="left"><rect x="{-PAD}" y="{-PAD}" width="{50 + PAD}" height="{100 + 2 * PAD}"/></clipPath>
<clipPath id="right"><rect x="50" y="{-PAD}" width="{50 + PAD}" height="{100 + 2 * PAD}"/></clipPath>
<g id="raised" clip-path="url(#halves)"><path fill-rule="evenodd" d="{TILE} M{x} {y} H{100 - x} V{y + h} H{x} Z"/></g>
<rect id="sunk" x="{x}" y="{y}" width="{100 - 2 * x}" height="{h}"/>
<filter id="soft" x="-20%" y="-20%" width="140%" height="140%"><feGaussianBlur stdDeviation="{drop:g}"/></filter>
{masks}
</defs>
<g mask="url(#around)"><use href="#raised" fill="#000" fill-opacity="{EMBOSS_SHADOW}" transform="translate({drop:g} {drop:g})" filter="url(#soft)"/></g>
<use href="#raised" fill="#fff" fill-opacity="{EMBOSS_LIGHT}" clip-path="url(#left)"/>
<use href="#raised" fill="#000" fill-opacity="{EMBOSS_DARK}" clip-path="url(#right)"/>
<use href="#sunk" fill="#000" fill-opacity="{EMBOSS_DARK}" clip-path="url(#left)"/>
<use href="#sunk" fill="#fff" fill-opacity="{EMBOSS_LIGHT}" clip-path="url(#right)"/>
<use href="#raised" fill="#fff" fill-opacity="{EMBOSS_HIGHLIGHT}" mask="url(#raisedTL)"/>
<use href="#raised" fill="#000" fill-opacity="{EMBOSS_SHADE}" mask="url(#raisedBR)"/>
<use href="#sunk" fill="#000" fill-opacity="{EMBOSS_SHADE}" mask="url(#sunkTL)"/>
<use href="#sunk" fill="#fff" fill-opacity="{EMBOSS_HIGHLIGHT}" mask="url(#sunkBR)"/>
</svg>
'''


def svg(variant, size=512, shown=None):
    # `size`: the SVG's own width and height; `shown`: the size it is made for (the outline's width).
    if variant == '-emboss':
        return emboss(size, shown)
    v = VARIANTS[variant]
    x, y, w, h = BOX
    parts = [f'<rect width="50" height="100" fill="{v["a"]}"/>',
             f'<rect x="50" width="50" height="100" fill="{v["b"]}"/>',
             f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="{v["b"]}"/>',
             f'<rect x="50" y="{y}" width="{w}" height="{h}" fill="{v["a"]}"/>']
    # The outline: the tile's edge stroked twice its width and masked to outside the tile, so it
    # lies round the tile, over the background. Crisp: outlines are never blurred.
    line = edge_for(shown or size)
    view = f'{-PAD} {-PAD} {100 + 2 * PAD} {100 + 2 * PAD}'
    area = f'x="{-PAD}" y="{-PAD}" width="{100 + 2 * PAD}" height="{100 + 2 * PAD}"'
    return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="{view}" width="{size}" height="{size}">
<title>uniapi</title>
<defs><clipPath id="tile"><path d="{TILE}"/></clipPath>
<mask id="outside" maskUnits="userSpaceOnUse" {area}><rect {area} fill="#fff"/><path d="{TILE}" fill="#000"/></mask></defs>
<g clip-path="url(#tile)">{''.join(parts)}</g>
<path d="{TILE}" fill="none" stroke="{mix(v['a'], v['b'])}" stroke-opacity="{EDGE_ALPHA}" stroke-width="{2 * line:g}" mask="url(#outside)"/>
</svg>
'''


def edge_exe():
    for path in (r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe',
                 r'C:\Program Files\Microsoft\Edge\Application\msedge.exe'):
        if os.path.exists(path):
            return path
    return shutil.which('msedge')


def render(exe, svg_text, size, out):
    # Edge's headless screenshot of the SVG at 512 px on a transparent background (its own
    # profile, so a running Edge doesn't take the call), scaled down to `size` with Pillow.
    from PIL import Image
    # An ordinary folder (not tempfile's: Python 3.14 makes those readable by their owner only,
    # and Edge's sandboxed processes then "can't read and write to its data directory").
    tmp = os.path.join(os.environ.get('LOCALAPPDATA') or tempfile.gettempdir(), 'uniapi-logo-render')
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    try:
        src = os.path.join(tmp, 'logo.html')
        shot = os.path.join(tmp, 'shot.png')
        with open(src, 'w', encoding='utf-8') as f:
            f.write('<!doctype html><html><body style="margin:0;background:transparent">' + svg_text + '</body></html>')
        subprocess.run([exe, '--headless=new', '--disable-gpu', '--hide-scrollbars', '--force-device-scale-factor=1',
                        f'--user-data-dir={os.path.join(tmp, "profile")}', '--no-first-run',
                        '--default-background-color=00000000', '--window-size=512,512',
                        f'--screenshot={shot}', 'file:///' + src.replace(os.sep, '/')],
                       check=True, capture_output=True, timeout=120)
        # msedge.exe hands the work to a child process and returns: wait for its screenshot.
        import time
        for _ in range(600):
            if os.path.exists(shot) and os.path.getsize(shot) > 0:
                time.sleep(0.3)  # finished writing
                break
            time.sleep(0.1)
        Image.open(shot).convert('RGBA').crop((0, 0, 512, 512)).resize((size, size), Image.LANCZOS).save(out)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


# Every drawing: the coloured variants and the embossed one.
ALL = (*VARIANTS, '-emboss')


def colours():
    # The word beside the logo: "uni" in the logo's left colour, "api" in its right colour, and the
    # logo's outline (for pages that outline the word too). The embossed logo's are its tints.
    out = {}
    for variant in ALL:
        name = variant.lstrip('-') or 'light'
        if variant == '-emboss':
            # As the logo: "uni" the C's lighter tint, "api" the J's darker one, the outline the
            # others have (its two colours, white and black, mixed, at EDGE_ALPHA), its shadow.
            out[name] = {'uni': f'rgba(255, 255, 255, {EMBOSS_LIGHT})', 'api': f'rgba(0, 0, 0, {EMBOSS_DARK})',
                         'outline': mix('#ffffff', '#000000'), 'outlineOpacity': EDGE_ALPHA,
                         'emboss': {'light': EMBOSS_LIGHT, 'dark': EMBOSS_DARK, 'shadow': EMBOSS_SHADOW}}
        else:
            v = VARIANTS[variant]
            out[name] = {'uni': v['a'], 'api': v['b'], 'outline': mix(v['a'], v['b']), 'outlineOpacity': EDGE_ALPHA}
    return out


# The logo with its name, as the uniapi_logo example shows them (examples/uniapi_logo/web): the
# logo LOCKUP_LOGO across, then LOCKUP_GAP, then "uniapi" from its outlines (name.json) at
# LOCKUP_NAME to the em, "uni" in the logo's left colour and "api" in its right, outlined as the
# logo is (its colours mixed, half transparent, a crisp line; never blurred); the embossed
# version's name in its tints with the same outline and its soft shadow. The name's letters are
# centred on the logo and lowered LOCKUP_DROP, their body level with the logo's middle. In
# those units (the page's pixels); the SVG is drawn LOCKUP_SCALE times that.
LOCKUP_LOGO = 40
LOCKUP_GAP = 10
LOCKUP_NAME = 26
LOCKUP_PAD = 4      # room round the letters for the outline and the shadow
LOCKUP_DROP = 3
LOCKUP_SCALE = 4

EMBOSS_NAME_FILTER = '''<defs><filter id="name-around" x="-10%" y="-40%" width="120%" height="180%" color-interpolation-filters="sRGB">
<feOffset in="SourceAlpha" dx="1" dy="1.2"/><feGaussianBlur stdDeviation="1.2"/>
<feComposite in2="SourceAlpha" operator="out" result="cast"/>
<feFlood flood-color="#000" flood-opacity="{shadow}"/><feComposite in2="cast" operator="in" result="shadow"/>
<feMorphology in="SourceAlpha" operator="dilate" radius="1"/>
<feComposite in2="SourceAlpha" operator="out" result="ring"/>
<feFlood flood-color="{outline}" flood-opacity="{opacity}"/><feComposite in2="ring" operator="in" result="outline"/>
<feMerge><feMergeNode in="shadow"/><feMergeNode in="outline"/></feMerge>
</filter></defs>'''


def lockup(variant, name):
    s = LOCKUP_NAME / name['em']
    word_h = (name['bottom'] - name['top']) * s + 2 * LOCKUP_PAD
    top = (LOCKUP_LOGO - word_h) / 2 + LOCKUP_DROP + LOCKUP_PAD   # the letters' box, its top
    x = LOCKUP_LOGO + LOCKUP_GAP
    place = f'translate({x:g} {top - name["top"] * s:.3f}) scale({s:g})'
    width = round(x + name['width'] * s + LOCKUP_PAD)
    height = LOCKUP_LOGO
    # The logo as an SVG of its own inside this one (its ids are its own: tile, outside).
    logo = svg(variant, size=LOCKUP_LOGO, shown=LOCKUP_LOGO * LOCKUP_SCALE)
    logo = logo.replace(' xmlns="http://www.w3.org/2000/svg"', '').replace('<title>uniapi</title>\n', '')
    c = colours()[variant.lstrip('-') or 'light']
    uni, api = name['uni'], name['api']
    if variant == '-emboss':
        e = c['emboss']
        word = (EMBOSS_NAME_FILTER.format(shadow=e['shadow'], outline=c['outline'], opacity=c['outlineOpacity'])
                + f'\n<g filter="url(#name-around)"><g transform="{place}" fill="#000"><path d="{uni}"/><path d="{api}"/></g></g>'
                + f'\n<g transform="{place}"><path d="{uni}" fill="#fff" fill-opacity="{e["light"]}"/>'
                + f'<path d="{api}" fill="#000" fill-opacity="{e["dark"]}"/></g>')
    else:
        # A stroke twice the outline's width under the letters (paint-order): one width shows round them.
        word = (f'<g transform="{place}" stroke="{c["outline"]}" stroke-opacity="{c["outlineOpacity"]}" '
                f'stroke-width="{2 / s:g}" stroke-linejoin="round" paint-order="stroke fill">\n'
                f'<path d="{uni}" fill="{c["uni"]}"/><path d="{api}" fill="{c["api"]}"/></g>')
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" '
            f'width="{width * LOCKUP_SCALE}" height="{height * LOCKUP_SCALE}" role="img" aria-label="uniapi">\n'
            f'<title>uniapi</title>\n{logo.strip()}\n{word}\n</svg>\n')


def write_lockups():
    import json
    with open(os.path.join(HERE, 'name.json'), encoding='utf-8') as f:
        name = json.load(f)
    for variant in ALL:
        with open(os.path.join(HERE, f'uniapi-name{variant}.svg'), 'w', encoding='utf-8', newline='\n') as f:
            f.write(lockup(variant, name))


def main():
    for variant in ALL:
        with open(os.path.join(HERE, f'uniapi{variant}.svg'), 'w', encoding='utf-8', newline='\n') as f:
            f.write(svg(variant))
    import json
    with open(os.path.join(HERE, 'colors.json'), 'w', encoding='utf-8', newline='\n') as f:
        json.dump(colours(), f, indent=2)
        f.write('\n')
    # The name beside text: name.js (its outlines and each coloured variant's colours, as a script so
    # a page opened from disk can use it too) and name-fit.js, which places it (x-height = the
    # text's capitals, baselines shared, fitted to the screen's pixels).
    with open(os.path.join(HERE, 'name.json'), encoding='utf-8') as f:
        name = json.load(f)
    name['colors'] = {(v.lstrip('-') or 'light'): {'uni': VARIANTS[v]['a'], 'api': VARIANTS[v]['b'],
                                                  'outline': mix(VARIANTS[v]['a'], VARIANTS[v]['b']),
                                                  'outlineOpacity': EDGE_ALPHA} for v in VARIANTS}
    script = ('// Written by doc/logo/make.py: the uniapi name\'s outlines (name.json) and each logo\n'
              '// variant\'s colours, for name-fit.js.\n'
              f'window.UNIAPI_NAME = {json.dumps(name, indent=2)};\n')
    with open(os.path.join(HERE, 'name.js'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(script)
    # The logo with its name.
    write_lockups()
    exe = edge_exe()
    if not exe:
        print('SVGs written; no Microsoft Edge for the PNGs and the icon')
        return
    png = os.path.join(HERE, 'png')
    os.makedirs(png, exist_ok=True)
    for variant in ALL:
        for size in (16, 32, 48, 64, 128, 256, 512):
            render(exe, svg(variant, shown=size), size, os.path.join(png, f'uniapi{variant}-{size}.png'))
    from PIL import Image
    sizes = (16, 32, 48, 256)
    images = [Image.open(os.path.join(png, f'uniapi-{s}.png')).convert('RGBA') for s in sizes]
    images[-1].save(os.path.join(HERE, 'uniapi.ico'), sizes=[(s, s) for s in sizes], append_images=images[:-1])
    print('SVGs, PNGs and uniapi.ico written')


# The name's lettering: "uniapi" in Orbitron at its regular weight, its letters spaced 0.08 em
# apart, as outlines (doc/logo/name.json), so no font is needed or kept here.
NAME_TEXT = 'uniapi'
NAME_WEIGHT = 400
NAME_SPACING = 0.08


def extract_name(font_path):
    # Reads the name's six letters from an Orbitron font file (the variable one from google/fonts,
    # kept outside the repository) and writes doc/logo/name.json: the outlines of "uni" and "api"
    # as SVG path data in a 100-unit em (the baseline at y 0, y down), the word's width, its ink's
    # top and bottom, the short letters' height (u, n, a: the x-height), and where "api" starts. Kerning is left out (Orbitron's lowercase has none
    # that matters here).
    import json
    from fontTools.ttLib import TTFont
    from fontTools.varLib.instancer import instantiateVariableFont
    from fontTools.pens.svgPathPen import SVGPathPen
    from fontTools.pens.transformPen import TransformPen
    from fontTools.pens.boundsPen import BoundsPen
    font = TTFont(font_path)
    if 'fvar' in font:
        font = instantiateVariableFont(font, {'wght': NAME_WEIGHT})
    scale = 100 / font['head'].unitsPerEm
    cmap, glyphs, metrics = font.getBestCmap(), font.getGlyphSet(), font['hmtx']
    number = lambda v: f'{v:.2f}'.rstrip('0').rstrip('.')
    parts, x, split = {'uni': '', 'api': ''}, 0.0, 0.0
    top, bottom, xheight = 0.0, 0.0, 0.0
    for i, ch in enumerate(NAME_TEXT):
        name = cmap[ord(ch)]
        if i == 3:
            split = x - NAME_SPACING * 100 / 2
        pen = SVGPathPen(glyphs, ntos=number)
        glyphs[name].draw(TransformPen(pen, (scale, 0, 0, -scale, x, 0)))
        parts['uni' if i < 3 else 'api'] += pen.getCommands()
        bounds = BoundsPen(glyphs)
        glyphs[name].draw(TransformPen(bounds, (scale, 0, 0, -scale, x, 0)))
        if bounds.bounds:
            top, bottom = min(top, bounds.bounds[1]), max(bottom, bounds.bounds[3])
            if ch == 'u':
                xheight = -bounds.bounds[1]   # the short letters' height above the baseline
        x += metrics[name][0] * scale + NAME_SPACING * 100
    data = {'text': NAME_TEXT, 'font': f'Orbitron {NAME_WEIGHT}, letter-spacing {NAME_SPACING} em', 'em': 100,
            'width': round(x - NAME_SPACING * 100, 2), 'top': round(top, 2), 'bottom': round(bottom, 2),
            'xheight': round(xheight, 2),
            'split': round(split, 2), 'uni': parts['uni'], 'api': parts['api']}
    with open(os.path.join(HERE, 'name.json'), 'w', encoding='utf-8', newline='\n') as f:
        json.dump(data, f, indent=2)
        f.write('\n')
    print('name.json written')


if __name__ == '__main__':
    import sys
    if len(sys.argv) == 3 and sys.argv[1] == '--name-font':
        extract_name(sys.argv[2])
    else:
        main()
