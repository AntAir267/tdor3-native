/* Files: Windows paths -> host paths, file handles, directory searches.

   The program sees its folder as C:\TDOR3 and its CD as drive D: (the folder cd_dir: the CD's
   files, read only). Reads look in the user folder first and then in the game folder (which is
   never written); anything opened for writing lives in the user folder, copied there first if
   it only exists in the game folder. Lookups ignore case. */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <pthread.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "runtime.h"

#define VROOT "C:\\TDOR3"
static char cwd[512] = VROOT;            /* the program's current directory (Windows form) */
static pthread_mutex_t flock_ = PTHREAD_MUTEX_INITIALIZER;

const char *virtual_game_root(void) { return VROOT; }

/* Normalise to a path relative to the virtual root, '/' separated, no "." or "..". *outside:
   0 under the root, 2 on the CD (rel relative to it), 1 anywhere else (then *rel holds a path
   under the user folder's drive_<letter>). */
static int to_relative(const char *win, char *rel, size_t n, int *outside)
{
    char full[1024];
    *outside = 0;
    if (win[0] && win[1] == ':')
        snprintf(full, sizeof full, "%s", win);
    else if (win[0] == '\\' || win[0] == '/')
        snprintf(full, sizeof full, "C:%s", win);
    else
        snprintf(full, sizeof full, "%s\\%s", cwd, win);
    for (char *p = full; *p; p++)
        if (*p == '/') *p = '\\';
    /* split into components and resolve . and .. */
    char *comp[128];
    int nc = 0;
    char *save = NULL;
    char *drive = strtok_r(full, "\\", &save);
    for (char *t = strtok_r(NULL, "\\", &save); t; t = strtok_r(NULL, "\\", &save)) {
        if (!strcmp(t, ".") || !*t) continue;
        if (!strcmp(t, "..")) { if (nc) nc--; continue; }
        if (nc < 128) comp[nc++] = t;
    }
    int start = 0;
    if (drive && !strcasecmp(drive, "C:") && nc >= 1 && !strcasecmp(comp[0], "TDOR3"))
        start = 1;
    else if (drive && !strcasecmp(drive, CD_DRIVE))
        *outside = 2;
    else
        *outside = 1;
    size_t k = 0;
    rel[0] = 0;
    if (*outside == 1) {
        k = snprintf(rel, n, "drive_%c", drive && drive[0] ? (char)tolower((unsigned char)drive[0]) : 'c');
    }
    for (int i = start; i < nc && k < n; i++)
        k += snprintf(rel + k, n - k, "%s%s", k ? "/" : "", comp[i]);
    return 1;
}

/* Find `rel` under `base` ignoring case; writes the real host path. Returns 1 if it exists.
   If only the parent exists, writes the path with the last component as given. */
static int resolve_ci(const char *base, const char *rel, char *out, size_t n)
{
#ifdef _WIN32
    /* Windows' folders already ignore case: a path that is not there is not there in any case,
       so one look at the whole path does (not a look at each part and a folder listing; the
       game opens thousands of files at start-up with fan content) */
    if (!*rel) {
        snprintf(out, n, "%s", base);
        return 1;
    }
    snprintf(out, n, "%s/%s", base, rel);
    struct stat st;
    return stat(out, &st) == 0;
#else
    char cur[2048];
    snprintf(cur, sizeof cur, "%s", base);
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", rel);
    char *save = NULL;
    int exists = 1;
    for (char *t = strtok_r(tmp, "/", &save); t; t = strtok_r(NULL, "/", &save)) {
        char next[2048];
        snprintf(next, sizeof next, "%s/%s", cur, t);
        struct stat st;
        if (exists && stat(next, &st) != 0) {
            DIR *d = opendir(cur);
            int found = 0;
            if (d) {
                struct dirent *e;
                while ((e = readdir(d))) {
                    if (!strcasecmp(e->d_name, t)) {
                        snprintf(next, sizeof next, "%s/%s", cur, e->d_name);
                        found = 1;
                        break;
                    }
                }
                closedir(d);
            }
            if (!found)
                exists = 0;
        }
        snprintf(cur, sizeof cur, "%s", next);
    }
    snprintf(out, n, "%s", cur);
    return exists;
#endif
}

static void mkdirs_for(const char *path)
{
    char tmp[2048];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
}

static int copy_file(const char *from, const char *to)
{
    int a = open(from, O_RDONLY);
    if (a < 0) return 0;
    mkdirs_for(to);
    int b = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (b < 0) { close(a); return 0; }
    char buf[65536];
    ssize_t r;
    while ((r = read(a, buf, sizeof buf)) > 0)
        if (write(b, buf, r) != r) break;
    close(a);
    close(b);
    return 1;
}

int path_to_host(const char *win, char *out, size_t n, int for_write)
{
    char rel[1024];
    int outside;
    to_relative(win, rel, sizeof rel, &outside);
    char user_path[2048], game_path[2048];
    if (outside == 2 && !for_write)
        return resolve_ci(cd_dir, rel, out, n);
    if (outside == 2) {                           /* written to the CD: the user folder's drive_d */
        char r2[1100];
        snprintf(r2, sizeof r2, "drive_d/%s", rel);
        snprintf(rel, sizeof rel, "%s", r2);
        outside = 1;
    }
    int in_user = resolve_ci(user_dir, rel, user_path, sizeof user_path);
    if (outside) {
        if (for_write)
            mkdirs_for(user_path);
        snprintf(out, n, "%s", user_path);
        return in_user;
    }
    int in_game = resolve_ci(game_dir, rel, game_path, sizeof game_path);
    if (for_write) {
        if (!in_user && in_game && for_write == 2)
            copy_file(game_path, user_path);      /* opened for update: keep its contents */
        mkdirs_for(user_path);
        snprintf(out, n, "%s", user_path);
        return 1;
    }
    if (in_user) {
        snprintf(out, n, "%s", user_path);
        return 1;
    }
    snprintf(out, n, "%s", game_path);
    return in_game;
}

/* ------------------------------------------------------------------ handles */
typedef struct { int fd; char path[1024]; int is_std; } File;
static uint32_t std_handles[3];

static File *file_of(uint32_t h)
{
    return handle_get(h, H_FILE);
}

void files_init(void)
{
    for (int i = 0; i < 3; i++) {
        File *f = calloc(1, sizeof *f);
        f->fd = i;
        f->is_std = 1;
        snprintf(f->path, sizeof f->path, "<std%d>", i);
        std_handles[i] = handle_new(H_FILE, f);
    }
}

#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u

API(KERNEL32, CreateFileA, 7)
{
    const char *name = gstr(ARG(0));
    uint32_t access = ARG(1), disp = ARG(4);
    int writing = (access & GENERIC_WRITE) != 0;
    char host[2048];
    int keep = disp == 3 /* OPEN_EXISTING */ || disp == 4 /* OPEN_ALWAYS */;
    int exists = path_to_host(name, host, sizeof host, writing ? (keep ? 2 : 1) : 0);
    int fl = writing ? ((access & GENERIC_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (disp) {
    case 1: fl |= O_CREAT | O_EXCL; break;      /* CREATE_NEW */
    case 2: fl |= O_CREAT | O_TRUNC; break;     /* CREATE_ALWAYS */
    case 3: break;                              /* OPEN_EXISTING */
    case 4: fl |= O_CREAT; break;               /* OPEN_ALWAYS */
    case 5: fl |= O_TRUNC; break;               /* TRUNCATE_EXISTING */
    }
    (void)exists;
    int fd = open(host, fl | O_CLOEXEC, 0644);
    if (fd < 0) {
        LOG(LOG_FILE, "CreateFileA(\"%s\", %x, disp %u) -> failed (%s)", name, access, disp, host);
        set_last_error(errno == ENOENT ? 2 : errno == EEXIST ? 80 : 5);
        return 0xffffffffu;
    }
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
        close(fd);
        set_last_error(5);
        return 0xffffffffu;
    }
    File *f = calloc(1, sizeof *f);
    f->fd = fd;
    snprintf(f->path, sizeof f->path, "%s", host);
    uint32_t h = handle_new(H_FILE, f);
    LOG(LOG_FILE, "CreateFileA(\"%s\", %s%s, disp %u) -> %x (%s)", name,
        access & GENERIC_READ ? "R" : "", writing ? "W" : "", disp, h, host);
    set_last_error((disp == 2 || disp == 4) && exists ? 183 : 0);
    return h;
}

API(KERNEL32, ReadFile, 5)
{
    File *f = file_of(ARG(0));
    if (ARG(3)) W32(ARG(3), 0);
    if (!f) { set_last_error(6); return 0; }
    uint32_t want = ARG(2), got = 0;
    while (got < want) {
        ssize_t r = read(f->fd, gptr(ARG(1) + got), want - got);
        if (r <= 0) break;
        got += r;
    }
    if (ARG(3)) W32(ARG(3), got);
    LOG(LOG_FILE, "ReadFile(%x, %u) -> %u", ARG(0), want, got);
    return 1;
}

API(KERNEL32, WriteFile, 5)
{
    File *f = file_of(ARG(0));
    if (ARG(3)) W32(ARG(3), 0);
    if (!f) { set_last_error(6); return 0; }
    uint32_t n = ARG(2);
    if (f->is_std) {
        LOG(LOG_ALWAYS, "program wrote to std%d: %.*s", f->fd, (int)n, gptr(ARG(1)));
        if (ARG(3)) W32(ARG(3), n);
        return 1;
    }
    ssize_t r = write(f->fd, gptr(ARG(1)), n);
    if (r < 0) { set_last_error(5); return 0; }
    if (ARG(3)) W32(ARG(3), (uint32_t)r);
    LOG(LOG_FILE, "WriteFile(%x, %u) -> %zd", ARG(0), n, r);
    return 1;
}

API(KERNEL32, SetFilePointer, 4)
{
    File *f = file_of(ARG(0));
    if (!f) { set_last_error(6); return 0xffffffffu; }
    int64_t dist = (int32_t)ARG(1);
    if (ARG(2))
        dist = (int64_t)(((uint64_t)R32(ARG(2)) << 32) | ARG(1));
    int whence = ARG(3) == 0 ? SEEK_SET : ARG(3) == 1 ? SEEK_CUR : SEEK_END;
    off_t r = lseek(f->fd, dist, whence);
    if (r < 0) { set_last_error(131); return 0xffffffffu; }
    if (ARG(2)) W32(ARG(2), (uint32_t)((uint64_t)r >> 32));
    set_last_error(0);
    return (uint32_t)r;
}

API(KERNEL32, SetEndOfFile, 1)
{
    File *f = file_of(ARG(0));
    if (!f) return 0;
    off_t p = lseek(f->fd, 0, SEEK_CUR);
    return ftruncate(f->fd, p) == 0;
}

API(KERNEL32, FlushFileBuffers, 1)
{
    return file_of(ARG(0)) != NULL;
}

API(KERNEL32, GetFileType, 1)
{
    File *f = file_of(ARG(0));
    if (!f) { set_last_error(6); return 0; }
    return f->is_std ? 2 : 1;                   /* FILE_TYPE_CHAR / FILE_TYPE_DISK */
}

API(KERNEL32, GetStdHandle, 1)
{
    int32_t n = (int32_t)ARG(0);
    if (n >= -12 && n <= -10)
        return std_handles[-10 - n];
    return 0xffffffffu;
}

API(KERNEL32, SetStdHandle, 2)
{
    int32_t n = (int32_t)ARG(0);
    if (n >= -12 && n <= -10)
        std_handles[-10 - n] = ARG(1);
    return 1;
}

API(KERNEL32, SetHandleCount, 1) { return ARG(0); }

int close_file_handle(uint32_t h)
{
    File *f = file_of(h);
    if (!f) return 0;
    if (!f->is_std)
        close(f->fd);
    free(f);
    handle_close(h);
    return 1;
}

API(KERNEL32, DeleteFileA, 1)
{
    char host[2048];
    path_to_host(gstr(ARG(0)), host, sizeof host, 0);
    char user[2048];
    snprintf(user, sizeof user, "%s/", user_dir);
    int ok = 0;
    if (!strncmp(host, user, strlen(user)))
        ok = unlink(host) == 0;
    LOG(LOG_FILE, "DeleteFileA(\"%s\") -> %d", gstr(ARG(0)), ok);
    if (!ok) set_last_error(2);
    return ok;
}

API(KERNEL32, MoveFileA, 2)
{
    char from[2048], to[2048];
    int exists = path_to_host(gstr(ARG(0)), from, sizeof from, 0);
    path_to_host(gstr(ARG(1)), to, sizeof to, 1);
    char user[2048];
    snprintf(user, sizeof user, "%s/", user_dir);
    int ok = 0;
    if (exists) {
        if (!strncmp(from, user, strlen(user)))
            ok = rename(from, to) == 0;
        else
            ok = copy_file(from, to);           /* never remove from the game folder */
    }
    LOG(LOG_FILE, "MoveFileA(\"%s\", \"%s\") -> %d", gstr(ARG(0)), gstr(ARG(1)), ok);
    if (!ok) set_last_error(2);
    return ok;
}

API(KERNEL32, SetCurrentDirectoryA, 1)
{
    const char *p = gstr(ARG(0));
    char host[2048];
    int exists = path_to_host(p, host, sizeof host, 0);
    LOG(LOG_FILE, "SetCurrentDirectoryA(\"%s\") -> %d", p, exists);
    if (!exists) { set_last_error(3); return 0; }
    pthread_mutex_lock(&flock_);
    if (p[0] && p[1] == ':')
        snprintf(cwd, sizeof cwd, "%s", p);
    else {
        char t[512];
        snprintf(t, sizeof t, "%s\\%s", cwd, p);
        snprintf(cwd, sizeof cwd, "%s", t);
    }
    size_t k = strlen(cwd);
    while (k > 3 && (cwd[k - 1] == '\\' || cwd[k - 1] == '/'))
        cwd[--k] = 0;
    pthread_mutex_unlock(&flock_);
    return 1;
}

API(KERNEL32, GetTempFileNameA, 4)
{
    static unsigned counter = 1;
    const char *dir = gstr(ARG(0)), *pre = gstr(ARG(1));
    unsigned u = ARG(2) ? ARG(2) & 0xffff : counter++;
    char name[600];
    snprintf(name, sizeof name, "%s\\%.3s%X.TMP", dir, pre ? pre : "", u);
    if (!ARG(2)) {
        char host[2048];
        path_to_host(name, host, sizeof host, 1);
        int fd = open(host, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) close(fd);
    }
    gstrncpy(ARG(3), name, 260);
    LOG(LOG_FILE, "GetTempFileNameA -> \"%s\"", name);
    return u;
}

/* ------------------------------------------------------------------ directory search */
typedef struct {
    char names[4096][64];
    char from[4096];                    /* where each name was listed: FROM_USER, FROM_GAME */
    int n, pos;
    char dir_user[2048], dir_game[2048];
} Find;

static void unix_time_to_filetime(time_t t, uint32_t at)
{
    uint64_t ft = ((uint64_t)t + 11644473600ull) * 10000000ull;
    W32(at, (uint32_t)ft);
    W32(at + 4, (uint32_t)(ft >> 32));
}

/* GetFileTime(handle, creation*, access*, write*): the file's times (srEXT_Unzip asks) */
API(KERNEL32, GetFileTime, 4)
{
    File *f = file_of(ARG(0));
    struct stat st;
    if (!f || f->is_std || fstat(f->fd, &st) != 0) { set_last_error(6); return 0; }
    if (ARG(1)) unix_time_to_filetime(st.st_mtime, ARG(1));
    if (ARG(2)) unix_time_to_filetime(st.st_atime, ARG(2));
    if (ARG(3)) unix_time_to_filetime(st.st_mtime, ARG(3));
    return 1;
}

/* GetFileInformationByHandle(handle, BY_HANDLE_FILE_INFORMATION*) */
API(KERNEL32, GetFileInformationByHandle, 2)
{
    File *f = file_of(ARG(0));
    struct stat st;
    uint32_t b = ARG(1);
    if (!f || f->is_std || fstat(f->fd, &st) != 0 || !b) { set_last_error(6); return 0; }
    memset(gptr(b), 0, 52);
    W32(b, S_ISDIR(st.st_mode) ? 0x10 : (st.st_mode & 0200) ? 0x80 : 0x01);
    unix_time_to_filetime(st.st_mtime, b + 4);
    unix_time_to_filetime(st.st_atime, b + 12);
    unix_time_to_filetime(st.st_mtime, b + 20);
    W32(b + 28, 0x7d03);                     /* volume serial */
    W32(b + 32, (uint32_t)((uint64_t)st.st_size >> 32));
    W32(b + 36, (uint32_t)st.st_size);
    W32(b + 40, (uint32_t)st.st_nlink);
    W32(b + 44, (uint32_t)((uint64_t)st.st_ino >> 32));
    W32(b + 48, (uint32_t)st.st_ino);
    return 1;
}

API(KERNEL32, SetFileTime, 4) { return file_of(ARG(0)) != NULL; }     /* (times kept as they are) */
API(KERNEL32, LockFile, 5) { return 1; }
API(KERNEL32, UnlockFile, 5) { return 1; }

enum { FROM_USER, FROM_GAME };

static int fill_find_data(Find *f, uint32_t data)
{
    while (f->pos < f->n) {
        int from = f->from[f->pos];
        const char *nm = f->names[f->pos++];
        char p[2200];
        struct stat st;
        /* looked for where it was listed only (a name in the user folder hides the game's) */
        snprintf(p, sizeof p, "%s/%s", from == FROM_USER ? f->dir_user : f->dir_game, nm);
        if (stat(p, &st) != 0)
            continue;
        memset(gptr(data), 0, 320);
        W32(data, S_ISDIR(st.st_mode) ? 0x10 : 0x20);
        unix_time_to_filetime(st.st_mtime, data + 4);
        unix_time_to_filetime(st.st_atime, data + 12);
        unix_time_to_filetime(st.st_mtime, data + 20);
        W32(data + 28, (uint32_t)((uint64_t)st.st_size >> 32));
        W32(data + 32, (uint32_t)st.st_size);
        gstrncpy(data + 44, nm, 260);
        return 1;
    }
    return 0;
}

static void add_matches(Find *f, const char *dir, const char *pat, int from)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && f->n < 4096) {
        if (fnmatch(pat, e->d_name, FNM_CASEFOLD))
            continue;
        if (strlen(e->d_name) >= 64)
            continue;
        int dup = 0;
        for (int i = 0; i < f->n && !dup; i++)
            dup = !strcasecmp(f->names[i], e->d_name);
        if (!dup) {
            f->from[f->n] = (char)from;
            snprintf(f->names[f->n++], 64, "%s", e->d_name);
        }
    }
    closedir(d);
}

/* sorted by name, case ignored (the names are unique so), each keeping where it was listed */
static Find *sorting;
static int cmp_idx(const void *a, const void *b)
{
    return strcasecmp(sorting->names[*(const int *)a], sorting->names[*(const int *)b]);
}

static void sort_names(Find *f)
{
    static int idx[4096];
    static char names[4096][64], from[4096];
    for (int i = 0; i < f->n; i++)
        idx[i] = i;
    sorting = f;
    qsort(idx, f->n, sizeof idx[0], cmp_idx);
    for (int i = 0; i < f->n; i++) {
        memcpy(names[i], f->names[idx[i]], 64);
        from[i] = f->from[idx[i]];
    }
    memcpy(f->names, names, (size_t)f->n * 64);
    memcpy(f->from, from, (size_t)f->n);
}

API(KERNEL32, FindFirstFileA, 2)
{
    const char *spec = gstr(ARG(0));
    char dirpart[600], pat[260];
    const char *slash = strrchr(spec, '\\');
    if (!slash) slash = strrchr(spec, '/');
    if (slash) {
        snprintf(dirpart, sizeof dirpart, "%.*s", (int)(slash - spec), spec);
        snprintf(pat, sizeof pat, "%s", slash + 1);
    } else {
        snprintf(dirpart, sizeof dirpart, ".");
        snprintf(pat, sizeof pat, "%s", spec);
    }
    if (!strcmp(pat, "*.*"))
        snprintf(pat, sizeof pat, "*");
    Find *f = calloc(1, sizeof *f);
    char rel[1024];
    int outside;
    to_relative(dirpart, rel, sizeof rel, &outside);
    if (outside == 2) {                         /* the CD: nothing of it in the user folder */
        resolve_ci(cd_dir, rel, f->dir_game, sizeof f->dir_game);
        snprintf(f->dir_user, sizeof f->dir_user, "%s/.none", user_dir);
    } else {
        resolve_ci(user_dir, rel, f->dir_user, sizeof f->dir_user);
        if (outside)
            snprintf(f->dir_game, sizeof f->dir_game, "%s", f->dir_user);
        else
            resolve_ci(game_dir, rel, f->dir_game, sizeof f->dir_game);
    }
    add_matches(f, f->dir_user, pat, FROM_USER);
    add_matches(f, f->dir_game, pat, FROM_GAME);
    sort_names(f);
    LOG(LOG_FILE, "FindFirstFileA(\"%s\") -> %d matches", spec, f->n);
    if (!fill_find_data(f, ARG(1))) {
        free(f);
        set_last_error(2);
        return 0xffffffffu;
    }
    return handle_new(H_FIND, f);
}

API(KERNEL32, FindNextFileA, 2)
{
    Find *f = handle_get(ARG(0), H_FIND);
    if (!f || !fill_find_data(f, ARG(1))) {
        set_last_error(18);                     /* ERROR_NO_MORE_FILES */
        return 0;
    }
    return 1;
}

API(KERNEL32, FindClose, 1)
{
    Find *f = handle_get(ARG(0), H_FIND);
    if (!f) return 0;
    free(f);
    handle_close(ARG(0));
    return 1;
}

/* ------------------------------------------------------------------ drives, attributes, folders */
#define DRIVE_FIXED 3
#define DRIVE_CDROM 5
#define CD_LABEL "TDOR3"

API(KERNEL32, GetLogicalDrives, 0) { return (1u << 2) | (1u << (CD_DRIVE[0] - 'A')); }

API(KERNEL32, GetLogicalDriveStringsA, 2)
{
    static const char list[] = "C:\\\0" CD_DRIVE "\\\0";
    uint32_t need = sizeof list;               /* includes the final NUL */
    if (ARG(0) < need)
        return need;
    memcpy(gptr(ARG(1)), list, need);
    return need - 1;
}

static int drive_letter(uint32_t root)
{
    const char *r = gstr(root);
    char d = r && r[0] && r[1] == ':' ? (char)toupper((unsigned char)r[0]) : 'C';
    if (r && (r[0] == '\\' || r[0] == '/') && !r[1]) d = 'C';
    return d;
}

API(KERNEL32, GetDriveTypeA, 1)
{
    int d = drive_letter(ARG(0));
    uint32_t t = d == 'C' ? DRIVE_FIXED : d == CD_DRIVE[0] ? DRIVE_CDROM : 1;   /* 1: no root */
    LOG(LOG_FILE, "GetDriveTypeA(\"%s\") -> %u", gstr(ARG(0)) ? gstr(ARG(0)) : "", t);
    return t;
}

API(KERNEL32, GetVolumeInformationA, 8)
{
    int d = drive_letter(ARG(0));
    const char *label = d == CD_DRIVE[0] ? CD_LABEL : d == 'C' ? "SYSTEM" : NULL;
    LOG(LOG_FILE, "GetVolumeInformationA(\"%s\") -> %s", gstr(ARG(0)) ? gstr(ARG(0)) : "", label ? label : "none");
    if (!label) { set_last_error(21); return 0; }   /* ERROR_NOT_READY */
    if (ARG(1) && ARG(2)) gstrncpy(ARG(1), label, ARG(2));
    if (ARG(3)) W32(ARG(3), d == CD_DRIVE[0] ? 0x3a1f0c99u : 0x1c5a7e10u);
    if (ARG(4)) W32(ARG(4), d == CD_DRIVE[0] ? 110 : 255);
    if (ARG(5)) W32(ARG(5), d == CD_DRIVE[0] ? 0x00000001u : 0x000700ffu);
    if (ARG(6) && ARG(7)) gstrncpy(ARG(6), d == CD_DRIVE[0] ? "CDFS" : "NTFS", ARG(7));
    return 1;
}

API(KERNEL32, GetDiskFreeSpaceA, 5)
{
    if (ARG(1)) W32(ARG(1), 8);                 /* sectors per cluster */
    if (ARG(2)) W32(ARG(2), 512);               /* bytes per sector */
    if (ARG(3)) W32(ARG(3), 0x100000);          /* free clusters: 2 GB, what old checks expect */
    if (ARG(4)) W32(ARG(4), 0x200000);
    return 1;
}

API(KERNEL32, GetFileAttributesA, 1)
{
    char p[2048];
    const char *name = gstr(ARG(0));
    struct stat st;
    if (!name || !path_to_host(name, p, sizeof p, 0) || stat(p, &st) != 0) {
        LOG(LOG_FILE, "GetFileAttributesA(\"%s\") -> none", name ? name : "");
        set_last_error(2);
        return 0xffffffffu;
    }
    uint32_t a = S_ISDIR(st.st_mode) ? 0x10 : 0x20;
    if (strstr(p, cd_dir) == p) a |= 1;         /* the CD is read only */
    LOG(LOG_FILE, "GetFileAttributesA(\"%s\") -> %x", name, a);
    return a;
}

API(KERNEL32, SetFileAttributesA, 2) { return 1; }

API(KERNEL32, CreateDirectoryA, 2)
{
    char p[2048];
    const char *name = gstr(ARG(0));
    if (!name) return 0;
    path_to_host(name, p, sizeof p, 1);
    int r = mkdir(p, 0755);
    LOG(LOG_FILE, "CreateDirectoryA(\"%s\") -> %s", name, p);
    if (r != 0) { set_last_error(errno == EEXIST ? 183 : 3); return 0; }
    return 1;
}

API(KERNEL32, RemoveDirectoryA, 1)
{
    char p[2048];
    const char *name = gstr(ARG(0));
    if (!name || !path_to_host(name, p, sizeof p, 0)) return 0;
    if (strncmp(p, user_dir, strlen(user_dir))) return 0;     /* the game folder is never changed */
    return rmdir(p) == 0;
}

API(KERNEL32, GetCurrentDirectoryA, 2)
{
    uint32_t need = (uint32_t)strlen(cwd) + 1;
    if (ARG(0) < need) return need;
    gstrncpy(ARG(1), cwd, ARG(0));
    return need - 1;
}

API(KERNEL32, GetFullPathNameA, 4)
{
    const char *name = gstr(ARG(0));
    char full[1024];
    if (!name) return 0;
    if (name[0] && name[1] == ':')
        snprintf(full, sizeof full, "%s", name);
    else if (name[0] == '\\' || name[0] == '/')
        snprintf(full, sizeof full, "%.2s%s", cwd, name);
    else
        snprintf(full, sizeof full, "%s\\%s", cwd, name);
    for (char *q = full; *q; q++)
        if (*q == '/') *q = '\\';
    uint32_t need = (uint32_t)strlen(full) + 1;
    if (ARG(1) < need) return need;
    gstrncpy(ARG(2), full, ARG(1));
    if (ARG(3)) {
        char *bs = strrchr(full, '\\');
        W32(ARG(3), ARG(2) + (uint32_t)(bs ? bs + 1 - full : 0));
    }
    return need - 1;
}
