/* Exceptions (DESIGN.md decision 4): Windows structured exceptions as MSVC 6 code uses them,
   for C++ throw/catch and __try/__except.

   Raising walks the program's own chain of handler registrations (fs:[0], records on its
   stack: {next, handler}) and calls each handler as Windows does (record, frame, context,
   dispatcher context); the translated MSVCRT handlers (__CxxFrameHandler, _except_handler3)
   do the rest: matching, unwinding (RtlUnwind, below, and their own local unwinds, which call
   destructor funclets) and calling the catch funclet. What translation cannot do by itself is
   the last step, going on in the function that caught: MSVCRT jumps into the middle of it
   (_JumpToContinuation 0x7800ac4f for C++, a call of the __except block for SEH).

   tools/eh.py finds those places from the exception tables; every function whose prologue
   registers such a handler starts by registering a frame record here (eh_push: its program
   stack position at entry, its places, a host jump buffer) and goes on at the place when
   resumed. eh_resume picks the youngest live record that holds the place and whose frame lies at or above the program stack the handler resumes with
   (deeper records are callees being abandoned), sets the program's esp and ebp and jumps
   there on the host, abandoning the host frames between (they hold nothing: guest_call's saved
   registers are replaced by the ones resumed with). */
#include <stdint.h>
#include <string.h>
#include "runtime.h"
#include "funcs.h"

static __thread EHFrame *eh_top;

/* A record lives in its function's host frame, from entry to return (eh_pop); records an
   exception jumps past are dropped by eh_resume. */
void eh_push(CPU *c, EHFrame *f, uint32_t esp, const uint32_t *at)
{
    (void)c;
    f->next = eh_top;
    f->esp = esp;
    f->at = at;
    eh_top = f;
}

void eh_pop(EHFrame *f)
{
    eh_top = f->next;
}

void eh_lost(CPU *c, EHFrame *f)
{
    fatal("resumed function at %08x: no place %08x in it", c->esp, f->target);
}

/* Goes on at `target` with these esp and ebp if a live frame holds it; else returns 0. */
static int eh_resume(CPU *c, uint32_t target, uint32_t esp, uint32_t ebp)
{
    for (EHFrame *f = eh_top; f; f = f->next) {
        if (f->esp < esp)
            continue;                    /* a callee being abandoned */
        for (const uint32_t *p = f->at; *p; p++)
            if (*p == target) {
                LOG(LOG_API, "exception handled: going on at %08x (esp %08x, ebp %08x)", target, esp, ebp);
                eh_top = f;
                f->target = target;
                c->esp = esp;
                c->ebp = ebp;
                __builtin_longjmp(f->jb, 1);
            }
    }
    return 0;
}

/* icall of code that is no function: an __except block that _except_handler3 calls (ebp is
   the frame's; the block's first act is esp = [ebp-0x18]) */
int eh_resume_call(CPU *c, uint32_t target)
{
    return eh_resume(c, target, R32(c->ebp - 0x18), c->ebp);
}

/* MSVCRT's _JumpToContinuation(target, registration): unlinks the top registration, esp from
   the frame's saved one ([reg-4]), ebp = reg + 0xc, jumps to target */
static int jump_to_continuation_hook(CPU *c)
{
    uint32_t target = R32(c->esp + 4), reg = R32(c->esp + 8);
    uint32_t top = R32(c->fs_base);
    W32(c->fs_base, R32(top));
    if (!eh_resume(c, target, R32(reg - 4), reg + 0xc))
        fatal("a C++ catch goes on at %08x, which no live function holds", target);
    return 1;
}

/* ------------------------------------------------------------------ KERNEL32 */
#define EXCEPTION_NONCONTINUABLE 1u
#define EXCEPTION_UNWINDING 2u
#define EXCEPTION_EXIT_UNWIND 4u

static const char *cxx_type(uint32_t args)
{
    uint32_t ti = R32(args + 8);
    if (ti) {
        uint32_t cta = R32(ti + 12);
        if (cta && R32(cta) > 0) {
            uint32_t ct = R32(cta + 4), td = R32(ct + 4);
            if (td) return gstr(td + 8);
        }
    }
    return "?";
}

/* the record (0x50 bytes) and context (0x2cc) on the program's stack below esp */
static void make_record(CPU *c, uint32_t at, uint32_t code, uint32_t flags, uint32_t n, uint32_t args,
                        uint32_t ip)
{
    memset(gptr(at), 0, 0x50 + 0x2cc + 16);
    W32(at, code);
    W32(at + 4, flags);
    W32(at + 0xc, ip);
    if (n > 15) n = 15;
    W32(at + 0x10, n);
    for (uint32_t k = 0; k < n; k++)
        W32(at + 0x14 + 4 * k, R32(args + 4 * k));
    uint32_t ctx = at + 0x50;
    W32(ctx, 0x10007);
    W32(ctx + 0x9c, c->edi); W32(ctx + 0xa0, c->esi); W32(ctx + 0xa4, c->ebx); W32(ctx + 0xa8, c->edx);
    W32(ctx + 0xac, c->ecx); W32(ctx + 0xb0, c->eax); W32(ctx + 0xb4, c->ebp); W32(ctx + 0xb8, ip);
    W32(ctx + 0xc4, c->esp);
}

API(KERNEL32, RaiseException, 4)
{
    uint32_t code = ARG(0), flags = ARG(1) & EXCEPTION_NONCONTINUABLE, n = ARG(2), args = ARG(3);
    uint32_t ip = R32(c->esp);
    if (code == 0xe06d7363u && n >= 3)
        LOG(LOG_API, "C++ exception %s thrown (from %08x)", cxx_type(args), ip);
    else
        LOG(LOG_API, "RaiseException(%08x) from %08x", code, ip);
    uint32_t keep_esp = c->esp;
    uint32_t at = (c->esp - 0x400) & ~15u, ctx = at + 0x50, disp = at + 0x50 + 0x2cc;
    make_record(c, at, code, flags, n, args, ip);
    c->esp = at - 64;                    /* the handlers run below the record */
    for (uint32_t reg = R32(c->fs_base); reg != 0xffffffffu && reg; reg = R32(reg)) {
        uint32_t r = guest_call(c, R32(reg + 4), 4, at, reg, ctx, disp);
        if (r == 0) {                    /* ExceptionContinueExecution */
            if (flags & EXCEPTION_NONCONTINUABLE)
                fatal("a handler continued a non-continuable exception %08x", code);
            c->esp = keep_esp;
            return 0;
        }
    }
    if (code == 0xe06d7363u && n >= 3)
        fatal("the program threw a C++ exception of type %s that nothing catches", cxx_type(args));
    fatal("exception %08x from %08x that nothing handles", code, ip);
}

/* RtlUnwind(target frame, target ip, record, return value): every registration above the
   target is called to unwind, then unlinked; returns to the caller (MSVCRT's _global_unwind2
   passes its own return address as the target ip) */
API(KERNEL32, RtlUnwind, 4)
{
    uint32_t target = ARG(0), rec = ARG(2), rv = ARG(3);
    uint32_t keep_esp = c->esp;
    uint32_t at = (c->esp - 0x400) & ~15u, ctx = at + 0x50, disp = at + 0x50 + 0x2cc;
    if (rec) {
        memmove(gptr(at), gptr(rec), 0x50);
        memset(gptr(ctx), 0, 0x2cc + 16);
    } else
        make_record(c, at, 0xc0000027u, 0, 0, 0, R32(c->esp));
    W32(at + 4, R32(at + 4) | EXCEPTION_UNWINDING | (target == 0xffffffffu || !target ? EXCEPTION_EXIT_UNWIND : 0));
    c->esp = at - 64;
    uint32_t reg = R32(c->fs_base);
    while (reg != 0xffffffffu && reg && reg != target) {
        guest_call(c, R32(reg + 4), 4, at, reg, ctx, disp);
        reg = R32(reg);
        W32(c->fs_base, reg);
    }
    c->esp = keep_esp;
    return rv;
}

void eh_init(void)
{
    hook_jump_to_continuation = jump_to_continuation_hook;
}
