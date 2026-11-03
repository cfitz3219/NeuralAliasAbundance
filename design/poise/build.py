"""Build Poise prototype 0.1. Requires fonttools==4.68.0, brotli==1.2.1, pillow==13.0.0."""
from pathlib import Path
import hashlib, json
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont
from fontTools.pens.ttGlyphPen import TTGlyphPen
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent
SOURCE = ROOT / 'source/Jost-wght.ttf'
assert hashlib.sha256(SOURCE.read_bytes()).hexdigest() == '6343b70971000b04c5d401c96ae08ce371086135e999d5e1e1413039c0213076', 'Unexpected upstream font'
OUT = ROOT / 'fonts'
OUT.mkdir(exist_ok=True)
manifest = {'version': '0.1', 'source_url': 'https://raw.githubusercontent.com/google/fonts/main/ofl/jost/Jost%5Bwght%5D.ttf', 'source_sha256': hashlib.sha256(SOURCE.read_bytes()).hexdigest(), 'files': {}}
sample = 'NeuralAlias Your game library DLSS DLSS 5 Neural Rendering Frame Gen Go Please make a selection Quality Ultra Performance Frame rate 120 FPS Not installed Unable to Install Checking compatibility… 0123456789 Il1 O0 *'
for style, weight in [('Light', 300), ('Regular', 400), ('Medium', 500)]:
    font = instantiateVariableFont(TTFont(SOURCE), {'wght': weight}, inplace=True)
    font.recalcTimestamp = False
    glyf = font['glyf']
    # Slightly lower the A crossbar (first contour) without changing its apex/legs.
    a = glyf['A']
    assert a.endPtsOfContours[0] == 3
    for i in range(4):
        x, y = a.coordinates[i]
        a.coordinates[i] = (x, y - 12)
    # A restrained curved foot distinguishes lowercase l from uppercase I.
    old = glyf['l']; xs = [x for x, y in old.coordinates]
    left, right = min(xs), max(xs); stem = right - left
    pen = TTGlyphPen(None)
    pen.moveTo((left, 780)); pen.lineTo((left, 60))
    pen.qCurveTo((left, 0), (left + 60, 0))
    pen.lineTo((right + 42, 0)); pen.lineTo((right + 42, stem * .82))
    pen.lineTo((right + 20, stem * .82))
    pen.qCurveTo((right, stem * .82), (right, 65))
    pen.lineTo((right, 780)); pen.closePath()
    glyf['l'] = pen.glyph()
    advance, bearing = font['hmtx']['l']
    font['hmtx']['l'] = (advance + 22, bearing)
    # Modest built-in sidebearing breathing room, inherited kerning retained.
    for name in font.getGlyphOrder():
        advance, bearing = font['hmtx'][name]
        if advance > 0:
            font['hmtx'][name] = (advance + 6, bearing)
    # Edited outlines must not retain upstream grid-fitting instructions.
    font['glyf'].removeHinting()
    for tag in ('fpgm', 'prep', 'cvt ', 'DSIG'):
        if tag in font: del font[tag]
    names = font['name']
    for id in (1,2,3,4,5,6,16,17,18,21,22,25): names.removeNames(nameID=id)
    family = 'Poise' if style == 'Regular' else 'Poise ' + style
    values = {1:family, 2:'Regular', 3:f'Poise-0.1-{style}', 4:f'Poise {style}', 5:'Version 0.100', 6:f'Poise-{style}', 16:'Poise', 17:style,
              10:'Poise prototype: modified from Jost by Owen Earl / The Jost Project Authors. Adaptation by Astra for NeuralAlias. Not an entirely original typeface.',
              13:'Licensed under the SIL Open Font License, Version 1.1. See OFL.txt.', 14:'https://openfontlicense.org'}
    for id, value in values.items():
        names.setName(value, id, 3, 1, 0x409)
        names.setName(value, id, 1, 0, 0)
    font['OS/2'].usWeightClass = weight
    font['OS/2'].fsSelection = (font['OS/2'].fsSelection & ~0x21) | 0x40
    font['head'].macStyle = 0
    font['head'].fontRevision = 0.1
    # Remove inherited variable style metadata; this package contains static faces.
    if 'STAT' in font: del font['STAT']
    for ext in ('ttf', 'woff2'):
        font.flavor = None if ext == 'ttf' else 'woff2'
        path = OUT / f'Poise-{style}.{ext}'
        font.save(path)
        check = TTFont(path, checkChecksums=2)
        check.ensureDecompiled()
        assert all(ord(c) in check.getBestCmap() for c in sample)
        assert check['name'].getDebugName(16) == 'Poise'
        assert 'fvar' not in check
        manifest['files'][path.name] = {'sha256':hashlib.sha256(path.read_bytes()).hexdigest(), 'glyphs':len(check.getGlyphOrder()), 'weight':weight}
        check.close()

(ROOT / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')

# A real raster specimen, using only the generated fonts (not generated artwork).
im = Image.new('RGB', (1800, 1200), '#f5f3ee'); d = ImageDraw.Draw(im)
def txt(x,y,text,size=26,style='Regular',fill='#272c2b'):
    f = ImageFont.truetype(str(OUT / f'Poise-{style}.ttf'), size)
    d.text((x,y),text,font=f,fill=fill)
txt(70,36,'P O I S E   /   FONT PROTOTYPE 0.1',22)
txt(70,94,'NeuralAlias',132,'Light')
txt(75,265,'Light for the name. Regular for controls. Medium for emphasis.',27)
d.line((70,330,1730,330),fill='#d5d8d1',width=1)
txt(70,365,'Your game library',45,'Light')
for y,label in [(460,'DLSS'),(520,'DLSS 5 Neural Rendering'),(580,'Frame Gen')]:
    d.rounded_rectangle((75,y+8,99,y+32),radius=4,outline='#596b60',width=2)
    txt(118,y,label,30)
d.rounded_rectangle((70,665,805,728),radius=10,fill='#4f6b56')
txt(407,675,'Go',31,'Medium','#ffffff')
txt(75,752,'Please make a selection',24)
d.rounded_rectangle((930,370,1730,830),radius=18,fill='#202625')
txt(975,400,'NeuralAlias',67,'Light','#f4f3ed')
txt(977,510,'Quality',28,'Regular','#d9dfdc')
txt(1240,510,'Ultra Performance',28,'Regular','#ffffff')
txt(977,570,'Frame rate',28,'Regular','#d9dfdc')
txt(1240,570,'120 FPS',28,'Medium','#ffffff')
txt(977,650,'Frame Gen',28,'Regular','#9eaaa5')
txt(1240,650,'Not installed',28,'Regular','#9eaaa5')
txt(977,743,'0123456789    Il1    O0',26,'Regular','#d9dfdc')
txt(70,880,'Readable details',29,'Medium')
txt(70,935,'ABCDEFGHIJKLMNOPQRSTUVWXYZ',35,'Light')
txt(70,991,'abcdefghijklmnopqrstuvwxyz  0123456789',35)
txt(70,1070,'A lower A crossbar and a curved l foot. Based on Jost; licensed under SIL OFL 1.1.',24)
txt(70,1120,'Actual font rendering. Preview only — no application integration or system installation.',22)
im.save(ROOT / 'Poise-specimen.png')
print(json.dumps(manifest,indent=2))
