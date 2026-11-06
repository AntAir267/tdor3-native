/* MSVCRT's memory and string primitives done by the host (builds/tdor3.py HOOKS): same results,
   cdecl (the caller pops the arguments), each returning as the original's `ret` would. */
#include <string.h>
#include "runtime.h"
#include "funcs.h"

static inline int crt_return(CPU *c, uint32_t r)
{
    c->eax = r;
    c->ret = R32(c->esp);
    c->esp += 4;
    return 1;
}

static int memcpy_hook(CPU *c)       /* MSVCRT's memcpy is memmove: overlap allowed */
{
    uint32_t d = ARG(0), s = ARG(1), n = ARG(2);
    if (n) memmove(gptr(d), gptr(s), n);
    return crt_return(c, d);
}

static int memset_hook(CPU *c)
{
    uint32_t d = ARG(0), n = ARG(2);
    if (n) memset(gptr(d), (int)(ARG(1) & 0xff), n);
    return crt_return(c, d);
}

static int strlen_hook(CPU *c) { return crt_return(c, (uint32_t)strlen(gptr(ARG(0)))); }

static int strcpy_hook(CPU *c)
{
    uint32_t d = ARG(0);
    const char *s = gptr(ARG(1));
    memmove(gptr(d), s, strlen(s) + 1);
    return crt_return(c, d);
}

static int strcat_hook(CPU *c)
{
    uint32_t d = ARG(0);
    const char *s = gptr(ARG(1));
    char *e = gptr(d) + strlen(gptr(d));
    memmove(e, s, strlen(s) + 1);
    return crt_return(c, d);
}

static int strncat_hook(CPU *c)
{
    uint32_t d = ARG(0), n = ARG(2);
    const char *s = gptr(ARG(1));
    char *e = gptr(d) + strlen(gptr(d));
    uint32_t k = 0;
    while (k < n && s[k]) { e[k] = s[k]; k++; }
    e[k] = 0;
    return crt_return(c, d);
}

static int strncpy_hook(CPU *c)
{
    uint32_t d = ARG(0), n = ARG(2);
    const char *s = gptr(ARG(1));
    char *o = gptr(d);
    uint32_t k = 0;
    for (; k < n && s[k]; k++) o[k] = s[k];
    for (; k < n; k++) o[k] = 0;
    return crt_return(c, d);
}

void crt_init(void)
{
    hook_crt_memcpy = memcpy_hook;
    hook_crt_memmove = memcpy_hook;
    hook_crt_memset = memset_hook;
    hook_crt_strlen = strlen_hook;
    hook_crt_strcpy = strcpy_hook;
    hook_crt_strcat = strcat_hook;
    hook_crt_strncat = strncat_hook;
    hook_crt_strncpy = strncpy_hook;
}
