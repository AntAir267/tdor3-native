/* The program's modules (DESIGN.md decision 3): the user's TDOR3.exe, sr.dll and its plug-ins
   and the C runtimes, each mapped at the address it was translated for (gen/table.c's
   module_table), relocated when that is not its own base, its imports connected (to the
   runtime, or to another module), and its DllMain run as Windows would. Also the runtime's own
   modules the program loads by name (the native SurRender driver standing in for
   srDD_DirectX6.dll), LoadLibrary, GetProcAddress and calls through pointers. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "runtime.h"
#include "image.h"

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

/* ------------------------------------------------------------------ module records */
typedef struct {
    const ModuleInfo *info;
    int mapped, attached, refs;
} Module;
static Module mods[16];
static pthread_mutex_t ld_lock = PTHREAD_MUTEX_INITIALIZER;

/* the runtime's own modules (DLLs the program loads by name that the runtime provides) */
typedef struct { const char *name; void (*fn)(CPU *c); } HostExport;
typedef struct { const char *file; const HostExport *exports; int n; uint32_t handle; } HostModule;
static HostModule hmods[8];
static int nhmods;
#define HOST_MODULE_BASE 0x7d000000u

void loader_add_host_module(const char *file, const void *exports, int n)
{
    if (nhmods >= 8)
        fatal("too many host modules");
    HostModule *h = &hmods[nhmods];
    h->file = file;
    h->exports = exports;
    h->n = n;
    h->handle = HOST_MODULE_BASE + 0x10000u * nhmods;
    nhmods++;
}

static const char *base_name(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':')
            b = p + 1;
    return b;
}

/* "srEXT_Unzip" or "C:\x\SREXT_UNZIP.DLL" matches srEXT_Unzip.dll */
static int same_module(const char *want, const char *file)
{
    const char *b = base_name(want);
    size_t n = strlen(b), fn = strlen(file);
    if (!strcasecmp(b, file))
        return 1;
    return n + 4 == fn && !strncasecmp(b, file, n) && !strcasecmp(file + n, ".dll");
}

static Module *module_by_name(const char *name)
{
    for (int i = 0; i < module_count; i++)
        if (same_module(name, module_table[i].file))
            return &mods[i];
    return NULL;
}

static Module *module_by_handle(uint32_t h)
{
    for (int i = 0; i < module_count; i++)
        if (module_table[i].base == h)
            return &mods[i];
    return NULL;
}

/* ------------------------------------------------------------------ mapping */
static void find_file(const char *file, char *out, size_t n)
{
    snprintf(out, n, "%s/%s", game_dir, file);
    struct stat st;
    if (stat(out, &st) == 0)
        return;
    if (path_to_host(file, out, n, 0))
        return;
    fatal("%s is missing from the game folder %s", file, game_dir);
}

static void map_module(Module *m)
{
    const ModuleInfo *mi = m->info;
    char path[2048];
    find_file(mi->file, path, sizeof path);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        fatal("cannot open %s: %s", path, strerror(errno));
    struct stat st;
    fstat(fd, &st);
    uint8_t *f = malloc(st.st_size);
    if (read(fd, f, st.st_size) != st.st_size)
        fatal("cannot read %s", path);
    close(fd);
    uint32_t pe = rd32(f + 0x3c);
    if (pe + 0xf8 > (uint32_t)st.st_size || memcmp(f + pe, "PE\0\0", 4))
        fatal("%s is not a Windows module", path);
    uint16_t nsec = rd16(f + pe + 6), optsz = rd16(f + pe + 20);
    const uint8_t *opt = f + pe + 24;
    if (rd32(opt + 28) != mi->preferred || rd32(opt + 56) != mi->size ||
        (mi->entry && rd32(opt + 16) + mi->base != mi->entry))
        fatal("%s is not the %s this build was translated from (%s)", path, mi->file, BUILD_NAME);
    uint32_t size = (mi->size + 0xfff) & ~0xfffu;
    void *mem = mmap((void *)(uintptr_t)mi->base, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (mem != (void *)(uintptr_t)mi->base)
        fatal("cannot map %s at %08x: %s", mi->file, mi->base, strerror(errno));
    uint32_t hdrsize = rd32(opt + 60);
    memcpy(mem, f, hdrsize < (uint32_t)st.st_size ? hdrsize : (uint32_t)st.st_size);
    const uint8_t *sh = opt + optsz;
    for (int i = 0; i < nsec; i++, sh += 40) {
        uint32_t vs = rd32(sh + 8), va = rd32(sh + 12), rs = rd32(sh + 16), ra = rd32(sh + 20);
        uint32_t n = rs < vs || !vs ? rs : vs;
        if (ra + n > (uint32_t)st.st_size)
            n = ra < (uint32_t)st.st_size ? (uint32_t)st.st_size - ra : 0;
        if (va + n > size)
            fatal("damaged section in %s", path);
        memcpy((uint8_t *)mem + va, f + ra, n);
    }
    free(f);
    /* relocations, for a module moved from its own base */
    uint32_t delta = mi->base - mi->preferred;
    if (delta) {
        uint8_t *img = mem;
        uint32_t rrva = rd32(img + pe + 24 + 96 + 8 * 5), rsize = rd32(img + pe + 24 + 96 + 8 * 5 + 4);
        uint32_t p = rrva, end = rrva + rsize;
        while (p + 8 <= end) {
            uint32_t page = rd32(img + p), bsize = rd32(img + p + 4);
            if (bsize < 8)
                break;
            for (uint32_t k = 0; k < (bsize - 8) / 2; k++) {
                uint16_t e = rd16(img + p + 8 + 2 * k);
                if ((e >> 12) == 3) {
                    uint32_t a = page + (e & 0xfff), v = rd32(img + a) + delta;
                    memcpy(img + a, &v, 4);
                }
            }
            p += bsize;
        }
    }
    m->mapped = 1;
    LOG(LOG_API, "mapped %s at %08x", mi->file, mi->base);
}

/* ------------------------------------------------------------------ calls through pointers */
/* every translated function by its address: a table per 4 KB page of the program's code */
static void (**fn_pages[1u << 20])(CPU *);

static void build_function_index(void)
{
    for (int i = 0; i < func_count; i++) {
        uint32_t a = func_table[i].addr;
        void (***pg)(CPU *) = &fn_pages[a >> 12];
        if (!*pg)
            *pg = calloc(4096, sizeof **pg);
        (*pg)[a & 0xfff] = func_table[i].fn;
    }
}

void (*translated_fn(uint32_t a))(CPU *)
{
    void (**pg)(CPU *) = fn_pages[a >> 12];
    return pg ? pg[a & 0xfff] : NULL;
}

int in_translated_code(uint32_t a)
{
    for (int i = 0; i < module_count; i++)
        if (a - module_table[i].text_lo < module_table[i].text_hi - module_table[i].text_lo)
            return 1;
    return 0;
}

/* ------------------------------------------------------------------ imports */
static void connect_imports(void)
{
    for (int i = 0; i < import_count; i++) {
        const ImportEntry *e = &import_table[i];
        char *name = malloc(strlen(e->dll) + strlen(e->name) + 2);
        sprintf(name, "%s!%s", e->dll, e->name);
        W32(e->iat, thunk_addr(e->fn, name));
    }
    for (int i = 0; i < import_link_count; i++)
        W32(import_links[i].iat, import_links[i].target);
}

/* ------------------------------------------------------------------ DllMain */
static void attach(Module *m, CPU *c)
{
    if (m->attached || !m->info->is_dll)
        return;
    m->attached = 1;
    if (m->info->entry) {
        LOG(LOG_API, "%s: DllMain(PROCESS_ATTACH)", m->info->file);
        uint32_t r = guest_call(c, m->info->entry, 3, m->info->base, 1u, 0u);
        if (!r)
            fatal("%s failed to start (its DllMain returned 0)", m->info->file);
    }
}

void loader_setup(void)
{
    for (int i = 0; i < module_count; i++) {
        mods[i].info = &module_table[i];
        map_module(&mods[i]);
    }
    build_function_index();
    connect_imports();
}

/* The DLLs the program starts with, in the order the build lists them (the C runtimes first),
   on the main thread before the program's entry point. */
/* module i's place in memory (size 0 while not mapped); -1 past the last (the reference build) */
int loader_module_at(int i, uint32_t *base, uint32_t *size)
{
    if (i < 0 || i >= module_count)
        return -1;
    *base = module_table[i].base;
    *size = mods[i].mapped ? module_table[i].size : 0;
    return i;
}

void loader_start(CPU *c)
{
    for (int i = 0; i < module_count; i++)
        if (module_table[i].at_start) {
            mods[i].refs = 1;
            attach(&mods[i], c);
        }
}

/* New threads: DLL_THREAD_ATTACH to every attached DLL (MSVCRT keeps per-thread data). */
void loader_thread_attach(CPU *c, int reason)
{
    for (int i = 0; i < module_count; i++)
        if (mods[i].attached && module_table[i].entry)
            guest_call(c, module_table[i].entry, 3, module_table[i].base, (uint32_t)reason, 0u);
}

/* ------------------------------------------------------------------ the program's view */
static uint32_t module_load(const char *name)
{
    Module *m = module_by_name(name);
    if (m) {
        pthread_mutex_lock(&ld_lock);
        m->refs++;
        pthread_mutex_unlock(&ld_lock);
        attach(m, current_cpu());
        return m->info->base;
    }
    for (int i = 0; i < nhmods; i++)
        if (same_module(name, hmods[i].file))
            return hmods[i].handle;
    return 0;
}

API(KERNEL32, LoadLibraryA, 1)
{
    const char *n = gstr(ARG(0));
    uint32_t r = n ? module_load(n) : 0;
    LOG(LOG_API, "LoadLibraryA(\"%s\") -> %x", n ? n : "", r);
    if (!r)
        set_last_error(126);              /* ERROR_MOD_NOT_FOUND: the Glide drivers and the rest */
    return r;
}

API(KERNEL32, LoadLibraryExA, 3)
{
    const char *n = gstr(ARG(0));
    uint32_t r = n ? module_load(n) : 0;
    LOG(LOG_API, "LoadLibraryExA(\"%s\", %x) -> %x", n ? n : "", ARG(2), r);
    if (!r)
        set_last_error(126);
    return r;
}

API(KERNEL32, FreeLibrary, 1)
{
    Module *m = module_by_handle(ARG(0));
    if (m && m->refs > 0)
        m->refs--;                        /* modules stay mapped and attached: nothing unloads */
    return 1;
}

API(KERNEL32, DisableThreadLibraryCalls, 1) { return 1; }

API(KERNEL32, GetModuleHandleA, 1)
{
    const char *n = gstr(ARG(0));
    uint32_t r = 0;
    if (!n)
        r = IMAGE_BASE;
    else {
        Module *m = module_by_name(n);
        if (m && (m->attached || !m->info->is_dll || m->info->at_start))
            r = m->info->base;
        else if (!strncasecmp(base_name(n), "kernel32", 8))
            r = 0x7c800000u;
        else if (!strncasecmp(base_name(n), "user32", 6))
            r = 0x7e410000u;
    }
    LOG(LOG_API, "GetModuleHandleA(%s) -> %x", n ? n : "NULL", r);
    if (!r)
        set_last_error(126);
    return r;
}

static const char *module_file(uint32_t h)
{
    if (!h)
        return EXE_NAME;
    Module *m = module_by_handle(h);
    if (m)
        return m->info->file;
    for (int i = 0; i < nhmods; i++)
        if (hmods[i].handle == h)
            return hmods[i].file;
    return NULL;
}

API(KERNEL32, GetModuleFileNameA, 3)
{
    const char *f = module_file(ARG(0));
    if (!f || !ARG(2)) { set_last_error(126); return 0; }
    char b[600];
    snprintf(b, sizeof b, "%s\\%s", virtual_game_root(), f);
    gstrncpy(ARG(1), b, ARG(2));
    return strlen(gptr(ARG(1)));
}

API(KERNEL32, GetModuleFileNameW, 3)
{
    const char *f = module_file(ARG(0));
    if (!f || !ARG(2)) { set_last_error(126); return 0; }
    char b[600];
    snprintf(b, sizeof b, "%s\\%s", virtual_game_root(), f);
    uint32_t n = 0;
    for (; b[n] && n + 1 < ARG(2); n++)
        W16(ARG(1) + 2 * n, (uint8_t)b[n]);
    W16(ARG(1) + 2 * n, 0);
    return n;
}

/* An export of a mapped module, by name or ordinal, read from its own export table. */
static uint32_t pe_export(uint32_t base, uint32_t np)
{
    uint32_t pe = R32(base + 0x3c);
    uint32_t erva = R32(base + pe + 24 + 96), esize = R32(base + pe + 24 + 96 + 4);
    if (!erva)
        return 0;
    uint32_t ed = base + erva;
    uint32_t obase = R32(ed + 16), nfuncs = R32(ed + 20), nnames = R32(ed + 24);
    uint32_t afuncs = base + R32(ed + 28), anames = base + R32(ed + 32), aords = base + R32(ed + 36);
    uint32_t idx = 0xffffffffu;
    if (np < 0x10000)
        idx = np - obase;
    else {
        const char *want = gstr(np);
        for (uint32_t i = 0; i < nnames; i++)
            if (!strcmp(gstr(base + R32(anames + 4 * i)), want)) {
                idx = R16(aords + 2 * i);
                break;
            }
    }
    if (idx >= nfuncs)
        return 0;
    uint32_t f = R32(afuncs + 4 * idx);
    if (!f || (f >= erva && f < erva + esize))
        return 0;                          /* forwarded: none in these modules */
    return base + f;
}

static uint32_t user32_proc(const char *name)
{
    for (int i = 0; i < import_count; i++)
        if (!strcasecmp(import_table[i].dll, "USER32.dll") && !strcmp(import_table[i].name, name))
            return R32(import_table[i].iat);
    return 0;
}

static uint32_t kernel32_proc(const char *name)
{
    for (int i = 0; i < import_count; i++)
        if (!strcasecmp(import_table[i].dll, "KERNEL32.dll") && !strcmp(import_table[i].name, name))
            return R32(import_table[i].iat);
    return 0;
}

API(KERNEL32, GetProcAddress, 2)
{
    uint32_t h = ARG(0), np = ARG(1);
    const char *name = np < 0x10000 ? NULL : gstr(np);
    uint32_t r = 0;
    Module *m = module_by_handle(h);
    if (m)
        r = pe_export(h, np);
    else if (h == 0x7e410000u && name)
        r = user32_proc(name);
    else if (h == 0x7c800000u && name)
        r = kernel32_proc(name);
    else
        for (int i = 0; i < nhmods; i++)
            if (hmods[i].handle == h && name)
                for (int k = 0; k < hmods[i].n; k++)
                    if (!strcmp(hmods[i].exports[k].name, name))
                        r = thunk_addr(hmods[i].exports[k].fn, hmods[i].exports[k].name);
    if (name)
        LOG(LOG_API, "GetProcAddress(%x, \"%s\") -> %x", h, name, r);
    else
        LOG(LOG_API, "GetProcAddress(%x, #%u) -> %x", h, np, r);
    if (!r)
        set_last_error(127);
    return r;
}
