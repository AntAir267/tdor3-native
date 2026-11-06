"""Several PE modules mapped at fixed addresses, seen as one program image by discovery and
code generation (the same reads as a single PE: u8, u32, bytes, cstr, section_at)."""
import struct


class Image:
    def __init__(self, modules):
        self.modules = sorted(modules, key=lambda m: m.base)
        for a, b in zip(self.modules, self.modules[1:]):
            if a.base + a.size_of_image > b.base:
                raise ValueError('%s and %s overlap' % (a.path, b.path))
        self.by_name = {}
        for m in self.modules:
            self.by_name[m.name.lower()] = m
        self.text_ranges = [m.text_range() for m in self.modules]
        self._last = self.modules[0]

    def module_at(self, va):
        m = self._last
        if m.base <= va < m.base + m.size_of_image:
            return m
        for m in self.modules:
            if m.base <= va < m.base + m.size_of_image:
                self._last = m
                return m
        return None

    def in_text(self, va):
        for lo, hi in self.text_ranges:
            if lo <= va < hi:
                return True
        return False

    def section_at(self, va):
        m = self.module_at(va)
        return m.section_at(va) if m else None

    def u8(self, va):
        return self.module_at(va).u8(va)

    def u32(self, va):
        m = self.module_at(va)
        if m is None or va + 4 > m.base + m.size_of_image:
            raise IndexError('%08x is outside the program' % va)
        return m.u32(va)

    def bytes(self, va, n):
        m = self.module_at(va)
        return m.bytes(va, n) if m else b''

    def cstr(self, va):
        return self.module_at(va).cstr(va)

    def resolve_import(self, dll, name, depth=0):
        """(module, va) of an import that a module of the image exports, else None.
        Forwarded exports are followed."""
        m = self.by_name.get(dll.lower())
        if m is None:
            return None
        va = m.export(name)
        if va is not None:
            return m, va
        f = m.forwarders.get(name)
        if f and depth < 4:
            d, n = f.split('.', 1)
            return self.resolve_import(d + '.dll', n if not n.startswith('#') else n, depth + 1)
        return None
