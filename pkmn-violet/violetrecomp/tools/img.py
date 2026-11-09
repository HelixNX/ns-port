"""main's relocated image (build/image/main.img, a64image.py) as bytes at module offsets: pointers in data are the
absolute addresses the runtime sees (base 0x7100000000).

    python tools/img.py ptr <off hex>...     where data holds a pointer to main+<off> (luaL_Reg tables: the name
                                             pointer sits 8 bytes before the function pointer)
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
HDR = struct.Struct('<II' + 'Q' * 10 + 'II')
BASE = 0x7100000000


def load(path=None):
    path = path or os.path.join(HERE, '..', '..', 'build', 'image', 'main.img')
    b = open(path, 'rb').read()
    h = HDR.unpack_from(b, 0)
    nsym, strsz = h[-2], h[-1]
    return b[HDR.size + nsym * 16 + strsz:]


def cstr(img, off):
    e = img.find(b'\0', off)
    return img[off:e].decode('latin1')


def main():
    img = load()
    if sys.argv[1] == 'ptr':
        for a in sys.argv[2:]:
            t = struct.pack('<Q', BASE + int(a, 16))
            i = img.find(t)
            while i >= 0:
                if i % 8 == 0:
                    prev = struct.unpack_from('<Q', img, i - 8)[0] - BASE
                    name = cstr(img, prev) if 0 <= prev < len(img) else ''
                    print('main+%s at main+%x; before it %x %r' % (a, i, prev, name[:60]))
                i = img.find(t, i + 1)


if __name__ == '__main__':
    main()
