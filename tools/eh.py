"""Exception handling tables of MSVC 6 code (C++ try/catch and SEH __try/__except), found in
the program image: where execution resumes after a handler, and the handler pieces the
runtime calls as functions.

C++ (FuncInfo, magic 0x19930520): each function with try blocks registers an __ehhandler stub
`mov eax, FuncInfo; jmp __CxxFrameHandler`. FuncInfo {magic, maxState, pUnwindMap, nTryBlocks,
pTryBlockMap, ...}; UnwindMap entries {toState, action} (action: a destructor funclet, called);
TryBlockMap entries {tryLow, tryHigh, catchHigh, nCatches, pHandlerArray}; HandlerType
{adjectives, pType, dispCatchObj, addressOfHandler} (the catch funclet, called; it returns the
address to resume at in `mov eax, imm32 ... ret`).

SEH (_except_handler3): the function's prologue pushes -1, its scope table and the handler;
scope table entries {enclosingLevel, filter, handler}: with a filter (called) the handler is the
__except block, entered by a call that never returns (a place to resume at); without one it is
a __finally block (called).

scan() returns (entries, places, ...): addresses to translate as functions, and per
registration (the __ehhandler stub or the scope table, which the owning function's prologue
pushes) the addresses execution may go on at after its handlers (codegen gives the pushing
functions a resume point, runtime/eh.c jumps there)."""
import struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

FUNCINFO_MAGIC = 0x19930520


def _u32(img, va):
    try:
        return img.u32(va)
    except (IndexError, AttributeError):
        return None


def _continuations(img, md, start, limit=0x400):
    """The `mov eax, imm32` values a catch funclet returns (each `ret` after one)."""
    out = set()
    data = img.bytes(start, limit)
    last_imm = None
    for i in md.disasm(data, start):
        if i.mnemonic == 'mov' and i.op_str.startswith('eax, 0x') and i.size == 5:
            last_imm = int(i.op_str[5:], 16)
        elif i.mnemonic == 'ret':
            if last_imm is not None and img.in_text(last_imm):
                out.add(last_imm)
                last_imm = None
            else:
                break
        elif i.mnemonic in ('int3',):
            break
    return out


def handler_addresses(img, name):
    """Every address that means MSVCRT's `name`: the export, the import slots that hold it and
    `jmp [slot]` thunks to it."""
    out = set()
    msvcrt = img.by_name.get('msvcrt.dll')
    if msvcrt and msvcrt.export(name):
        out.add(msvcrt.export(name))
    for m in img.modules:
        for dll, n, iat in m.imports:
            if n != name:
                continue
            out.add(iat)
            lo, hi = m.text_range()
            code = img.bytes(lo, hi - lo)
            pat = b'\xff\x25' + struct.pack('<I', iat)
            k = code.find(pat)
            while k >= 0:
                out.add(lo + k)
                k = code.find(pat, k + 1)
    return out


def scan(img, cxx_handlers=None, seh_handlers=None):
    """cxx_handlers / seh_handlers: addresses that mean __CxxFrameHandler / _except_handler3
    (the functions, the import slots holding them and jump thunks to them)."""
    if cxx_handlers is None:
        cxx_handlers = handler_addresses(img, '__CxxFrameHandler')
    if seh_handlers is None:
        seh_handlers = handler_addresses(img, '_except_handler3')
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    entries, places = set(), {}
    funcinfos, scopetables = {}, set()          # FuncInfo -> its stubs
    for m in img.modules:
        lo, hi = m.text_range()
        code = img.bytes(lo, hi - lo)
        # __ehhandler: B8 <FuncInfo> then E9 rel32 / FF 25 [slot]
        k = code.find(b'\xb8')
        while k >= 0 and k + 10 <= len(code):
            fi = struct.unpack_from('<I', code, k + 1)[0]
            tgt = None
            if code[k + 5] == 0xe9:
                tgt = (lo + k + 10 + struct.unpack_from('<i', code, k + 6)[0]) & 0xffffffff
            elif code[k + 5] == 0xff and code[k + 6] == 0x25:
                tgt = struct.unpack_from('<I', code, k + 7)[0]
            if tgt in cxx_handlers and _u32(img, fi) == FUNCINFO_MAGIC:
                funcinfos.setdefault(fi, set()).add(lo + k)
            k = code.find(b'\xb8', k + 1)
        # SEH3 prologue: 6A FF 68 <scopetable> 68 <handler> 64 A1 00 00 00 00
        k = code.find(b'\x6a\xff\x68')
        while k >= 0 and k + 18 <= len(code):
            if code[k + 7] == 0x68 and code[k + 12:k + 18] == b'\x64\xa1\x00\x00\x00\x00':
                st, h = struct.unpack_from('<I', code, k + 3)[0], struct.unpack_from('<I', code, k + 8)[0]
                if h in seh_handlers:
                    scopetables.add(st)
            k = code.find(b'\x6a\xff\x68', k + 1)
    for fi, stubs in sorted(funcinfos.items()):
        conts = set()
        maxstate, punwind, ntry, ptry = (_u32(img, fi + 4), _u32(img, fi + 8), _u32(img, fi + 12),
                                         _u32(img, fi + 16))
        for s in range(min(maxstate or 0, 1000)):
            act = _u32(img, punwind + 8 * s + 4)
            if act and img.in_text(act):
                entries.add(act)
        for t in range(min(ntry or 0, 1000)):
            tb = ptry + 20 * t
            ncatch, parr = _u32(img, tb + 12), _u32(img, tb + 16)
            for h in range(min(ncatch or 0, 100)):
                fn = _u32(img, parr + 16 * h + 12)
                if fn and img.in_text(fn):
                    entries.add(fn)
                    conts |= _continuations(img, md, fn)
        if conts:
            for st in stubs:
                places.setdefault(st, set()).update(conts)
    for st in sorted(scopetables):
        for e in range(64):
            enc, flt, hnd = _u32(img, st + 12 * e), _u32(img, st + 12 * e + 4), _u32(img, st + 12 * e + 8)
            if enc is None or not (enc == 0xffffffff or enc < e) or not hnd or not img.in_text(hnd):
                break
            if flt:
                if not img.in_text(flt):
                    break
                entries.add(flt)
                places.setdefault(st, set()).add(hnd)
            else:
                entries.add(hnd)
    return entries, places, len(funcinfos), len(scopetables)
