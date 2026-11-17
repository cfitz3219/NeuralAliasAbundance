"""Synthetic PE regressions: no proprietary executable bytes or game process."""
import os
import struct
import tempfile
import unittest

from trace_rebellion_scale import trace, watchpoints


def fixture(path, inline=False, shift=0, image_base=0x140000000, broken=False):
    # Construct our own small PE with named settings and independently placed
    # code/data. Numeric lookalikes deliberately do not identify the setting.
    text, data, pdata = 0x1000, 0x3000, 0x4000
    bind, parser, apply, native, update = [x+shift for x in (0x1100,0x1200,0x1400,0x1600,0x1800)]
    ui, key = data+shift, data+0x100+shift
    setting, cache, active, fw, fh, sw, sh = [data+x+shift for x in (0x200,0x210,0x220,0x230,0x234,0x240,0x244)]
    twenty, step = data+0x250+shift, data+0x254+shift
    blob = bytearray(0x2600)
    offsets = {text:0x400, data:0x1400, pdata:0x2400}
    def put(rva, payload):
        section = max(x for x in offsets if x <= rva)
        at = offsets[section]+rva-section
        blob[at:at+len(payload)] = payload
    def emit(start):
        code = bytearray()
        def raw(payload):
            code.extend(payload)
        def rip(prefix, target):
            raw(prefix+struct.pack('<i',target-(start+len(code)+len(prefix)+4)))
        return code, raw, rip
    def dims(raw, rip):
        rip(b'\xf3\x0f\x11\x0d', active)  # movss [active],xmm1
        rip(b'\x8b\x05', fw)
        raw(b'\xf3\x48\x0f\x2a\xc0')    # cvtsi2ss xmm0,rax
        raw(b'\xf3\x0f\x59\xc1')        # mulss xmm0,xmm1
        raw(b'\xf3\x48\x0f\x2c\xc0')    # cvttss2si rax,xmm0
        rip(b'\x89\x05', sw)
        rip(b'\x8b\x05', fh)
        raw(b'\xf3\x48\x0f\x2a\xc0\xf3\x0f\x59\xc1\xf3\x48\x0f\x2c\xc0')
        if not broken:
            rip(b'\x89\x05', sh)
        rip(b'\xe8', update)
    functions = []
    code, raw, rip = emit(bind)
    rip(b'\x48\x8d\x05', setting)   # UI schema's actual pointer
    rip(b'\x48\x8d\x0d', ui)
    raw(b'\xc3')
    functions.append((bind, bind+len(code))); put(bind, code)
    code, raw, rip = emit(parser)
    rip(b'\x48\x8d\x0d', key)
    rip(b'\xf3\x0f\x59\x0d', twenty)
    rip(b'\xf3\x0f\x59\x0d', step)
    rip(b'\xf3\x0f\x11\x0d', setting)
    raw(b'\xc3')
    functions.append((parser, parser+len(code))); put(parser, code)
    code, raw, rip = emit(apply)
    rip(b'\xf3\x0f\x11\x0d', cache)
    if inline:
        dims(raw, rip)
    else:
        rip(b'\xe8', native)
    rip(b'\xf3\x0f\x10\x05', cache)
    raw(b'\xb0\x01')  # unrelated instruction before mirror
    rip(b'\xf3\x0f\x11\x05', setting)
    raw(b'\xc3')
    functions.append((apply, apply+len(code))); put(apply, code)
    if not inline:
        code, raw, rip = emit(native)
        rip(b'\xf3\x0f\x10\x0d', cache)
        dims(raw, rip); raw(b'\xc3')
        functions.append((native, native+len(code))); put(native, code)
    put(update, b'\xc3')
    put(ui, ('Frontend.Options.Display.RenderScale\0').encode('utf-16-le'))
    put(key, b'RenderScale\0')
    put(twenty, struct.pack('<ff',20,0.05))
    put(data+0x380, struct.pack('<fff',1.000001,0.666667,1.0))
    # DOS/COFF/PE32+ and three section headers; unwind ranges are ours too.
    blob[:2] = b'MZ'; struct.pack_into('<I',blob,0x3c,0x80)
    blob[0x80:0x84] = b'PE\0\0'
    struct.pack_into('<HHIIIHH',blob,0x84,0x8664,3,0,0,0,0xf0,0x22)
    opt = 0x98
    struct.pack_into('<H',blob,opt,0x20b)
    struct.pack_into('<Q',blob,opt+24,image_base)
    struct.pack_into('<II',blob,opt+32,0x1000,0x200)
    struct.pack_into('<II',blob,opt+56,0x5000,0x400)
    struct.pack_into('<I',blob,opt+108,16)
    struct.pack_into('<II',blob,opt+112+3*8,pdata,len(functions)*12)
    for n,(rva,offset,size,flags,name) in enumerate((
            (text,0x400,0x1000,0x60000020,b'.text'),
            (data,0x1400,0x1000,0xc0000040,b'.data'),
            (pdata,0x2400,0x200,0x40000040,b'.pdata'))):
        struct.pack_into('<8sIIIIIIHHI',blob,opt+0xf0+n*40,name,size,rva,size,offset,0,0,0,0,flags)
    for n,(a,b) in enumerate(functions):
        put(pdata+n*12,struct.pack('<III',a,b,pdata+0x100))
    put(pdata+0x100,b'\x01\x00\x00\x00')
    with open(path,'wb') as output:
        output.write(blob)
    return dict(setting=setting,applied_setting=cache,active_scale=active,full_width=fw,
                full_height=fh,scene_width=sw,scene_height=sh,settings_apply=apply,
                native_apply=None if inline else native,resource_update=update)


class TraceTests(unittest.TestCase):
    def test_named_consumer_chain_call_inline_and_relocation(self):
        with tempfile.TemporaryDirectory() as folder:
            for inline in (False,True):
                for shift in (0,0x80):
                    for base in (0x140000000,0x180000000):
                        with self.subTest(inline=inline,shift=shift,base=base):
                            path = os.path.join(folder,'synthetic.exe')
                            expected = fixture(path,inline,shift,base)
                            result = trace(path)
                            for key,value in expected.items():
                                self.assertEqual(result[key],value,key)
                            self.assertEqual(result['apply_shape'],'inline' if inline else 'call')
                            commands = watchpoints(result)
                            self.assertEqual(commands.count('\nba '),4)
                            self.assertIn(f'ba r 4 /1 synthetic+0x{expected["setting"]:x}',commands)
                            self.assertIn(f'ba w 4 /1 synthetic+0x{expected["scene_height"]:x}',commands)
    def test_numeric_setting_and_cache_without_both_dimension_consumers_is_insufficient(self):
        with tempfile.TemporaryDirectory() as folder:
            for inline in (False,True):
                with self.subTest(inline=inline):
                    path = os.path.join(folder,'missing-height.exe')
                    fixture(path,inline,broken=True)
                    with self.assertRaisesRegex(ValueError,'settings-to-native-apply link'):
                        trace(path)


if __name__ == '__main__':
    unittest.main()
