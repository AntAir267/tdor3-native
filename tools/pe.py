"""Minimal PE32 reader: headers, sections, imports, exports, relocations, raw image.

PE(path, base) loads the file as Windows would map it at `base` (its relocations applied when
that is not the preferred base)."""
import struct


class PE:
    def __init__(self, path, base=None):
        self.path = path
        d = self.data = open(path, 'rb').read()
        pe = struct.unpack_from('<I', d, 0x3c)[0]
        if d[pe:pe + 4] != b'PE\0\0':
            raise ValueError('not a PE file')
        self.machine, nsec, self.timestamp, _, _, optsz, self.characteristics = \
            struct.unpack_from('<HHIIIHH', d, pe + 4)
        o = pe + 24
        if struct.unpack_from('<H', d, o)[0] != 0x10b:
            raise ValueError('not PE32')
        self.entry_rva = struct.unpack_from('<I', d, o + 16)[0]
        self.preferred_base = struct.unpack_from('<I', d, o + 28)[0]
        self.base = self.preferred_base if base is None else base
        self.size_of_image = struct.unpack_from('<I', d, o + 56)[0]
        self.size_of_headers = struct.unpack_from('<I', d, o + 60)[0]
        self.stack_reserve, self.stack_commit, self.heap_reserve, self.heap_commit = \
            struct.unpack_from('<IIII', d, o + 72)
        ndd = struct.unpack_from('<I', d, o + 92)[0]
        self.dirs = [struct.unpack_from('<II', d, o + 96 + 8 * i) for i in range(ndd)]
        self.sections = []
        so = o + optsz
        for i in range(nsec):
            nm, vs, va, rs, ra = struct.unpack_from('<8sIIII', d, so + 40 * i)
            ch = struct.unpack_from('<I', d, so + 40 * i + 36)[0]
            self.sections.append(dict(name=nm.rstrip(b'\0').decode(), va=va, vsize=vs,
                                      raw=ra, rsize=rs, flags=ch))
        self.is_dll = bool(self.characteristics & 0x2000)
        self.entry = self.base + self.entry_rva if self.entry_rva else None
        self.image = self._build_image()
        self.fixups = self._relocate()          # VAs of the 32-bit words the loader adjusts
        self.imports = self._imports()
        self.exports, self.ordinals, self.forwarders = self._exports()

    def _build_image(self):
        img = bytearray(self.size_of_image)
        hdr = self.sections[0]['raw'] if self.sections else 0x400
        img[:hdr] = self.data[:hdr]
        for s in self.sections:
            n = min(s['rsize'], s['vsize'] if s['vsize'] else s['rsize'])
            img[s['va']:s['va'] + n] = self.data[s['raw']:s['raw'] + n]
        return img

    def _relocate(self):
        """Applies the base relocations for self.base; returns the fixed-up VAs (HIGHLOW)."""
        out = []
        if len(self.dirs) <= 5 or not self.dirs[5][0]:
            if self.base != self.preferred_base:
                raise ValueError('%s has no relocations and cannot move' % self.path)
            return out
        rva, size = self.dirs[5]
        delta = (self.base - self.preferred_base) & 0xffffffff
        p, end = rva, rva + size
        while p < end:
            page, bsize = struct.unpack_from('<II', self.image, p)
            if bsize < 8:
                break
            for k in range((bsize - 8) // 2):
                e = struct.unpack_from('<H', self.image, p + 8 + 2 * k)[0]
                t, off = e >> 12, e & 0xfff
                if t == 0:
                    continue
                if t != 3:
                    raise ValueError('relocation type %d in %s' % (t, self.path))
                a = page + off
                if delta:
                    v = struct.unpack_from('<I', self.image, a)[0]
                    struct.pack_into('<I', self.image, a, (v + delta) & 0xffffffff)
                out.append(self.base + a)
            p += bsize
        return out

    def section(self, name):
        for s in self.sections:
            if s['name'] == name:
                return s
        return None

    def text_range(self):
        """(start, end) of the code section."""
        s = self.section('.text') or next(s for s in self.sections if s['flags'] & 0x20)
        return self.base + s['va'], self.base + s['va'] + max(s['vsize'], s['rsize'])

    def section_at(self, va):
        rva = va - self.base
        for s in self.sections:
            if s['va'] <= rva < s['va'] + max(s['vsize'], s['rsize']):
                return s
        return None

    def contains(self, va):
        return self.base <= va < self.base + self.size_of_image

    def u8(self, va):
        return self.image[va - self.base]

    def u32(self, va):
        return struct.unpack_from('<I', self.image, va - self.base)[0]

    def bytes(self, va, n):
        o = va - self.base
        return bytes(self.image[o:o + n])

    def cstr(self, va):
        o = va - self.base
        e = self.image.index(0, o)
        return self.image[o:e].decode('latin-1')

    def _imports(self):
        """[(dll, name_or_ordinal, iat_va)]"""
        out = []
        rva, size = self.dirs[1]
        if not rva:
            return out
        i = 0
        while True:
            olt, ts, fc, nm, iat = struct.unpack_from('<IIIII', self.image, rva + 20 * i)
            if not nm:
                break
            dll = self.cstr(self.base + nm)
            k = 0
            src = olt or iat
            while True:
                v = struct.unpack_from('<I', self.image, src + 4 * k)[0]
                if not v:
                    break
                name = ('#%d' % (v & 0xffff)) if v & 0x80000000 else self.cstr(self.base + v + 2)
                out.append((dll, name, self.base + iat + 4 * k))
                k += 1
            i += 1
        return out

    def _exports(self):
        """({name: va}, {ordinal: va}, {name or #ord: 'DLL.name'} forwarded exports)"""
        names, ords, fwd = {}, {}, {}
        rva, size = self.dirs[0]
        if not rva:
            return names, ords, fwd
        (_, _, _, _, _, obase, nfuncs, nnames, afuncs, anames, aords) = \
            struct.unpack_from('<IIHHIIIIIII', self.image, rva)
        funcs = [struct.unpack_from('<I', self.image, afuncs + 4 * i)[0] for i in range(nfuncs)]
        for i, f in enumerate(funcs):
            if not f:
                continue
            if rva <= f < rva + size:
                fwd['#%d' % (obase + i)] = self.cstr(self.base + f)
            else:
                ords[obase + i] = self.base + f
        for i in range(nnames):
            nrva = struct.unpack_from('<I', self.image, anames + 4 * i)[0]
            idx = struct.unpack_from('<H', self.image, aords + 2 * i)[0]
            n = self.cstr(self.base + nrva)
            f = funcs[idx]
            if rva <= f < rva + size:
                fwd[n] = self.cstr(self.base + f)
            else:
                names[n] = self.base + f
        return names, ords, fwd

    def export(self, name):
        """VA of an export by name or '#ordinal', None if absent or forwarded."""
        if name.startswith('#'):
            return self.ordinals.get(int(name[1:]))
        return self.exports.get(name)
