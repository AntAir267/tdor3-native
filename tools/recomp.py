"""Static recompiler: the game's 32-bit MSVC modules (TDOR3.exe, sr.dll and its plug-ins, the
C runtimes) -> C, all in one program image (DESIGN.md decisions 2 and 3).

    recomp.py <game folder> <build config .py> <output dir>

Writes into the output dir: funcs.h (prototypes), code_NNN.c (one C function per program
function, named by its address), table.c (address -> function table, the modules, the imports
the runtime provides and the ones another module provides), stubs.c (imports the runtime does
not provide yet), x87_special.c, image.h (constants), warnings.txt. Nothing of the game's code or
data is copied: the runtime maps the user's own files at start.
"""
import sys, os, time, hashlib, struct, importlib.util
sys.path.insert(0, os.path.dirname(__file__))
from pe import PE
from image import Image
import eh
from x86 import Decoder
from discover import Discovery
from codegen import FuncGen, Program, Unsupported, import_symbol

LINES_PER_FILE = 30000


def load_config(path):
    spec = importlib.util.spec_from_file_location('buildcfg', path)
    cfg = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(cfg)
    return cfg


def find_file(folder, name):
    """The file in folder whose name matches without regard to case (Windows names)."""
    for f in os.listdir(folder):
        if f.lower() == name.lower():
            return os.path.join(folder, f)
    sys.exit('%s is missing from %s' % (name, folder))


def code_pointers(img, m):
    """Code addresses the module holds in data: its relocated words that point into code
    (exact, for DLLs), or aligned words of its data sections that do (the exe has no
    relocations)."""
    out = set()
    if m.fixups:
        for a in m.fixups:
            v = m.u32(a)
            if img.in_text(v) and not img.in_text(a):
                out.add(v)
        return out
    for s in m.sections:
        if s['name'] in ('.rdata', '.data'):
            n = min(s['vsize'], s['rsize'])
            for off in range(0, n - 3, 4):
                v = struct.unpack_from('<I', m.image, s['va'] + off)[0]
                if img.in_text(v):
                    out.add(v)
    return out


def main():
    gamedir, cfgpath, out = sys.argv[1:4]
    cfg = load_config(cfgpath)
    mods = []
    for d in cfg.MODULES:
        path = find_file(gamedir, d['file'])
        sha = hashlib.sha256(open(path, 'rb').read()).hexdigest()
        if sha != d['sha256']:
            sys.exit('%s is not the file this build describes (%s; sha256 %s)' % (path, cfg.NAME, sha))
        m = PE(path, d['base'])
        m.name = d['file']
        m.load = d['load']
        mods.append(m)
    img = Image(mods)

    # imports: the ones another translated module exports, and the ones the runtime provides
    redirect, links, host = {}, {}, {}
    for m in mods:
        for dll, name, iat in m.imports:
            r = img.resolve_import(dll, name)
            if r is None:
                host[iat] = (dll, name)
                continue
            tm, va = r
            links[iat] = (va, dll, name)
            if img.in_text(va):
                redirect[iat] = va
    dec = Decoder(img, redirect)
    seeds = set()
    for m in mods:
        if m.entry:
            seeds.add(m.entry)
        for name, va in m.exports.items():
            if img.in_text(va):
                seeds.add(va)
        for o, va in m.ordinals.items():
            if img.in_text(va):
                seeds.add(va)
    forced = set()
    for f, n in getattr(cfg, 'NORETURN_EXPORTS', []):
        mm = img.by_name.get(f.lower())
        va = mm.export(n) if mm else None
        if va:
            forced.add(va)
    # code pointers held in data: exact for relocated modules (trusted as entries: static
    # constructors, vtables, exception handlers' funclets), guessed for the exe (checked)
    cands = set()
    for m in mods:
        if m.fixups:
            seeds |= code_pointers(img, m)
        else:
            cands |= code_pointers(img, m)

    # exception handling: handler funclets are functions; places a catch / __except goes on at
    eh_entries, eh_places, n_fi, n_st = eh.scan(img)
    seeds |= eh_entries
    print('exception tables: %d C++ (FuncInfo), %d SEH scope tables; %d handler funclets, %d places '
          'to go on at' % (n_fi, n_st, len(eh_entries), len(set().union(*eh_places.values()))))

    t0 = time.time()
    disc = Discovery(img, dec, {va: n for va, (d, n) in host.items()}, cfg.NORETURN_IMPORTS,
                     cfg.EXTRA_ENTRIES, cfg.JUMP_TABLES, cfg.NOT_ENTRIES, forced)
    funcs = disc.run(sorted(seeds), cands)
    errs = [(e, a, msg) for e, f in funcs.items() for a, msg in f.errors]
    print('discovery: %d functions, %d instructions, %d errors (%.1fs)' % (
        len(funcs), sum(len(f.insns) for f in funcs.values()), len(errs), time.time() - t0))
    for m in mods:
        lo, hi = m.text_range()
        n = sum(1 for e in funcs if lo <= e < hi)
        print('  %-18s %08x %5d functions' % (m.name, m.base, n))
    for e in errs[:20]:
        print('  error in %08x at %08x: %s' % e)
    for l in disc.log[:40]:
        print('  ' + l)

    # every function's extent, for finding who holds an address (work tools)
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, 'extents.txt'), 'w', newline='\n') as fh:
        for e in sorted(funcs):
            f = funcs[e]
            lo = min(f.insns) if f.insns else e
            hi = max(a + i.size for a, i in f.insns.items()) if f.insns else e
            fh.write('%08x %08x %08x %d\n' % (e, lo, hi, len(f.insns)))
    with open(os.path.join(out, 'rejected.txt'), 'w', newline='\n') as fh:
        for a, why in sorted(disc.rejected.items()):
            fh.write('%08x %s\n' % (a, why[0] if why else '?'))

    prog = Program(img, funcs, disc.noreturn, host, cfg.NORETURN_IMPORTS,
                   getattr(cfg, 'NONLOCAL_JUMPS', []))
    prog.set_tables(getattr(cfg, 'TABLES', []))
    prog.hooks = dict(getattr(cfg, 'HOOKS', {}))
    prog.eh_places = eh_places
    prog.inline = dict(getattr(cfg, 'INLINE', {}))
    prog.set_consts(getattr(cfg, 'CONSTS', {}))
    prog.flag_interface(log=lambda s: None)
    prog.fpu_interface()
    os.makedirs(out, exist_ok=True)
    for fn in os.listdir(out):
        if fn.startswith('code_') and fn.endswith('.c'):
            os.remove(os.path.join(out, fn))

    # ---------------------------------------------------------------- functions
    warns = []
    chunks, cur, curlines = [], [], 0
    nlines = 0
    for e in sorted(funcs):
        g = FuncGen(prog, funcs[e])
        try:
            text = g.gen()
        except Unsupported as ex:
            warns.append('%08x: %s' % (e, ex))
            text = ('/* %08x: not translated: %s */\nvoid %s(CPU *c)\n{\n    unsupported(c, 0x%xu, "%s");\n}\n'
                    % (e, ex, prog.fname(e), e, str(ex).replace('"', "'")))
        warns.extend('%08x: %s' % (e, w) for w in g.warn)
        n = text.count('\n')
        nlines += n
        cur.append(text)
        curlines += n
        if curlines > LINES_PER_FILE:
            chunks.append(cur)
            cur, curlines = [], 0
    if cur:
        chunks.append(cur)
    for k, ch in enumerate(chunks):
        with open(os.path.join(out, 'code_%03d.c' % k), 'w', newline='\n') as fh:
            fh.write('/* Generated from the user\'s game files by tools/recomp.py. Do not edit or share. */\n')
            fh.write('#include "cpu.h"\n#include "funcs.h"\n\n')
            fh.write('\n'.join(ch))
    host_syms = sorted(set(import_symbol(d, n) for d, n in host.values()))
    with open(os.path.join(out, 'funcs.h'), 'w', newline='\n') as fh:
        fh.write('#pragma once\n#include "cpu.h"\n')
        for e in sorted(funcs):
            fh.write('void %s(CPU *c);\n' % prog.fname(e))
        for s in host_syms:
            fh.write('void %s(CPU *c);\n' % s)
        for t in prog.tables:
            fh.write('extern uint32_t reloc_%s, limit_%s;\n' % (t['name'], t['name']))
        for name in sorted(getattr(cfg, 'CONSTS', {})):
            fh.write('extern uint32_t rc_%s;\n' % name)
        for a, h in sorted(prog.hooks.items()):
            fh.write('extern int (*hook_%s)(CPU *c);   /* at %08x; returns 1 if it did the call itself */\n' % (h, a))
            fh.write('void %s_orig(CPU *c);\n' % prog.fname(a))

    # ---------------------------------------------------------------- tables
    with open(os.path.join(out, 'table.c'), 'w', newline='\n') as fh:
        fh.write('/* Generated by tools/recomp.py */\n#include "cpu.h"\n#include "funcs.h"\n#include "runtime.h"\n\n')
        fh.write('const FuncEntry func_table[] = {\n')
        for e in sorted(funcs):
            fh.write('    {0x%08xu, %s},\n' % (e, prog.fname(e)))
        fh.write('};\nconst int func_count = %d;\n\n' % len(funcs))
        fh.write('/* imports the runtime provides */\nconst ImportEntry import_table[] = {\n')
        for va, (dll, name) in sorted(host.items()):
            fh.write('    {0x%08xu, "%s", "%s", %s},\n' % (va, dll, name.replace('\\', '\\\\'), import_symbol(dll, name)))
        fh.write('};\nconst int import_count = %d;\n\n' % len(host))
        fh.write('/* imports another translated module provides: slot -> address */\nconst ImportLink import_links[] = {\n')
        for va, (t, dll, name) in sorted(links.items()):
            fh.write('    {0x%08xu, 0x%08xu},   /* %s!%s */\n' % (va, t, dll, name.replace('*/', '* /')))
        fh.write('};\nconst int import_link_count = %d;\n\n' % len(links))
        fh.write('const ModuleInfo module_table[] = {\n')
        for d in cfg.MODULES:
            m = img.by_name[d['file'].lower()]
            lo, hi = m.text_range()
            fh.write('    {"%s", "%s", 0x%08xu, 0x%08xu, 0x%08xu, 0x%08xu, 0x%08xu, 0x%08xu, %d, %d},\n' % (
                m.name, d['sha256'], m.base, m.preferred_base, m.size_of_image, m.entry or 0, lo, hi,
                int(m.is_dll), int(d['load'] == 'start')))
        fh.write('};\nconst int module_count = %d;\n\n' % len(cfg.MODULES))
        for t in prog.tables:
            fh.write('uint32_t reloc_%s = 0x%08xu, limit_%s = %du;\n' % (t['name'], t['base'], t['name'], t['count']))
        for a, h in sorted(prog.hooks.items()):
            fh.write('int (*hook_%s)(CPU *c);\n' % h)
        for name, d in sorted(getattr(cfg, 'CONSTS', {}).items()):
            fh.write('uint32_t rc_%s = 0x%xu;\n' % (name, d['value']))
        fh.write('const TableEntry table_list[] = {\n')
        for t in prog.tables:
            fh.write('    {"%s", &reloc_%s, &limit_%s, 0x%08xu, %du, %du},\n' % (
                t['name'], t['name'], t['name'], t['base'], t['entry'], t['count']))
        fh.write('    {0}\n};\n')
    # imports the runtime does not implement: weak stubs that say so
    with open(os.path.join(out, 'stubs.c'), 'w', newline='\n') as fh:
        fh.write('/* Generated by tools/recomp.py */\n#include "cpu.h"\n#include "funcs.h"\n#include "runtime.h"\n\n')
        done = set()
        for va, (dll, name) in sorted(host.items()):
            sym = import_symbol(dll, name)
            if sym in done:
                continue
            done.add(sym)
            fh.write('__attribute__((weak)) void %s(CPU *c) { missing_import(c, "%s", "%s"); }\n'
                     % (sym, dll, name.replace('\\', '\\\\')))
    trans = {}
    for f in funcs.values():
        for a, i in f.insns.items():
            if i.mn in ('fsin', 'fcos', 'fsincos', 'fptan', 'fpatan', 'fyl2x', 'fyl2xp1', 'f2xm1'):
                trans[a] = i.raw[-1]
    with open(os.path.join(out, 'x87_special.c'), 'w', newline='\n') as fh:
        fh.write('/* Generated by tools/recomp.py: x87 transcendental instructions (address, opcode byte) */\n')
        fh.write('#include <stdint.h>\nconst uint32_t x87_special_addr[] = {\n')
        fh.write(''.join('    0x%08xu,\n' % a for a in sorted(trans)) or '    0,\n')
        fh.write('};\nconst uint8_t x87_special_op[] = {\n')
        fh.write(''.join('    0x%02x,\n' % trans[a] for a in sorted(trans)) or '    0,\n')
        fh.write('};\nconst int x87_special_count = %d;\n' % len(trans))
        # rdtsc and cpuid: the reference build answers them as the translated code does
        cpu = {a: i.raw[-1] for f in funcs.values() for a, i in f.insns.items() if i.mn in ('rdtsc', 'cpuid')}
        fh.write('const uint32_t cpu_special_addr[] = {\n')
        fh.write(''.join('    0x%08xu,\n' % a for a in sorted(cpu)) or '    0,\n')
        fh.write('};\nconst uint8_t cpu_special_op[] = {\n')
        fh.write(''.join('    0x%02x,\n' % cpu[a] for a in sorted(cpu)) or '    0,\n')
        fh.write('};\nconst int cpu_special_count = %d;\n' % len(cpu))
    exe = img.by_name[next(d['file'] for d in cfg.MODULES if d['file'].lower().endswith('.exe')).lower()]
    with open(os.path.join(out, 'image.h'), 'w', newline='\n') as fh:
        fh.write('#pragma once\n')
        fh.write('#define IMAGE_BASE 0x%08xu\n' % exe.base)
        fh.write('#define IMAGE_SIZE 0x%08xu\n' % exe.size_of_image)
        fh.write('#define IMAGE_ENTRY 0x%08xu\n' % exe.entry)
        fh.write('#define EXE_NAME "%s"\n' % exe.name)
        fh.write('#define STACK_RESERVE 0x%08xu\n' % exe.stack_reserve)
        fh.write('#define BUILD_NAME "%s"\n' % cfg.NAME)
        for name, a in sorted(getattr(cfg, 'DATA', {}).items()):
            fh.write('#define GAME_%s 0x%08xu\n' % (name.upper(), a))
    with open(os.path.join(out, 'warnings.txt'), 'w', newline='\n') as fh:
        fh.write('\n'.join(warns) + '\n')
    print('generated %d functions, %d lines in %d files, %d warnings (%.1fs)' % (
        len(funcs), nlines, len(chunks), len(warns), time.time() - t0))
    print('imports: %d from the runtime (%d distinct), %d between modules (%d calls made direct)' % (
        len(host), len(host_syms), len(links), len(redirect)))
    for w in warns[:30]:
        print('  ' + w)


if __name__ == '__main__':
    main()
