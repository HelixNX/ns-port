/* The game's RomFS as one virtual image: the SDK reads the RomFS through an IStorage and parses it itself, but
 * the dump holds the extracted files (11 GB). So the metadata (header, hash tables, directory and file entries)
 * is built here in memory from a scan of the directory, and reads of the data area go to the host files.
 *
 * The same serves system data archives (fsp-srv OpenDataStorageByDataId) from build/sysdata/<data id>/.
 *
 * Layout: header (0x50) | directory hash table | directory entries | file hash table | file entries |
 *         (aligned to 0x200) file data, each file aligned to 0x10. */
#include "hos.h"
#include <stdlib.h>
#include <windows.h>

typedef struct RomFile { wchar_t *path; u64 offset, size; HANDLE h; } RomFile;

typedef struct RomFs {
    u8 *meta;
    u64 meta_size, data_offset, total_size;
    RomFile *files;
    int nfiles, capfiles;
} RomFs;
static RomFs *B;                                /* the image being built (start-up, one at a time) */
static SRWLOCK open_lock = SRWLOCK_INIT;

typedef struct Node {
    char name[256];
    wchar_t *path;
    bool dir;
    u64 size;
    struct Node *parent, *child, *sibling;     /* children in name order */
    u32 off;                                    /* entry offset in its table */
    u32 file_index;
} Node;

static int count_dirs, count_files;                  /* of the image being built */
static u32 dir_table_size, file_table_size;

static u32 align4(u32 v) { return (v + 3) & ~3u; }

static Node *scan(const wchar_t *path, const char *name, Node *parent) {
    Node *n = calloc(1, sizeof *n);
    snprintf(n->name, sizeof n->name, "%s", name);
    n->path = _wcsdup(path);
    n->dir = true;
    n->parent = parent;
    count_dirs++;
    dir_table_size += 0x18 + align4((u32)strlen(name));
    wchar_t pattern[1024];
    swprintf(pattern, 1024, L"%s\\*", path);
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(pattern, &fd);
    if (f == INVALID_HANDLE_VALUE) return n;
    Node *kids[4096];
    int nk = 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        wchar_t sub[1024];
        swprintf(sub, 1024, L"%s\\%s", path, fd.cFileName);
        char uname[256];
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, uname, sizeof uname, NULL, NULL);
        Node *k;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            k = scan(sub, uname, n);
        } else {
            k = calloc(1, sizeof *k);
            snprintf(k->name, sizeof k->name, "%s", uname);
            k->path = _wcsdup(sub);
            k->size = (u64)fd.nFileSizeHigh << 32 | fd.nFileSizeLow;
            k->parent = n;
            count_files++;
            file_table_size += 0x20 + align4((u32)strlen(uname));
        }
        if (nk == 4096) fatal("romfs: too many entries in one directory");
        kids[nk++] = k;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    /* directories first, then files, each by name: the order of the sibling chains */
    for (int i = 1; i < nk; i++)
        for (int j = i; j > 0; j--) {
            Node *a = kids[j - 1], *b = kids[j];
            if (a->dir != b->dir ? b->dir : strcmp(a->name, b->name) > 0) { kids[j - 1] = b; kids[j] = a; }
            else break;
        }
    for (int i = nk - 1; i >= 0; i--) { kids[i]->sibling = n->child; n->child = kids[i]; }
    return n;
}

static u32 path_hash(u32 parent, const char *name) {
    u32 h = parent ^ 123456789u;
    for (const u8 *p = (const u8 *)name; *p; p++) {
        h = (h >> 5) | (h << 27);
        h ^= *p;
    }
    return h;
}

static u32 bucket_count(u32 n) {
    if (n < 3) return 3;
    if (n < 19) return n | 1;
    u32 c = n;
    static const u32 primes[] = {2, 3, 5, 7, 11, 13, 17};
    for (;;) {
        bool div = false;
        for (int i = 0; i < 7; i++) if (c % primes[i] == 0) div = true;
        if (!div) return c;
        c++;
    }
}

/* assign entry offsets (directories breadth first, files in their directory's order) and data offsets */
static Node *queue[1 << 14];

static void assign(Node *root, u64 *data_pos) {
    int head = 0, tail = 0;
    u32 doff = 0, foff = 0;
    queue[tail++] = root;
    while (head < tail) {
        Node *d = queue[head++];
        d->off = doff;
        doff += 0x18 + align4((u32)strlen(d->name));
        for (Node *k = d->child; k; k = k->sibling) {
            if (k->dir) {
                if (tail == (int)(sizeof queue / sizeof *queue)) fatal("romfs: too many directories");
                queue[tail++] = k;
            } else {
                k->off = foff;
                foff += 0x20 + align4((u32)strlen(k->name));
                *data_pos = (*data_pos + 15) & ~15ull;
                if (B->nfiles == B->capfiles) {
                    B->capfiles = B->capfiles ? B->capfiles * 2 : 1024;
                    B->files = realloc(B->files, B->capfiles * sizeof *B->files);
                }
                k->file_index = (u32)B->nfiles;
                B->files[B->nfiles++] = (RomFile){k->path, *data_pos, k->size, NULL};
                *data_pos += k->size;
            }
        }
    }
}

static void put32(u8 *p, u32 v) { memcpy(p, &v, 4); }
static void put64(u8 *p, u64 v) { memcpy(p, &v, 8); }

static u32 first_child(Node *d, bool dir) {
    for (Node *k = d->child; k; k = k->sibling)
        if (k->dir == dir) return k->off;
    return 0xffffffffu;
}

static u32 next_sibling(Node *n) {
    for (Node *k = n->sibling; k; k = k->sibling)
        if (k->dir == n->dir) return k->off;
    return 0xffffffffu;
}

static void write_tables(Node *root) {
    u32 dbuckets = bucket_count((u32)count_dirs), fbuckets = bucket_count((u32)count_files);
    u64 dht = 0x50, dmt = dht + 4ull * dbuckets, fht = dmt + dir_table_size, fmt = fht + 4ull * fbuckets;
    B->meta_size = fmt + file_table_size;
    B->data_offset = (B->meta_size + 0x1ff) & ~0x1ffull;
    B->meta = calloc(1, B->meta_size);
    u8 *h = B->meta;
    put64(h + 0x00, 0x50);
    put64(h + 0x08, dht); put64(h + 0x10, 4ull * dbuckets);
    put64(h + 0x18, dmt); put64(h + 0x20, dir_table_size);
    put64(h + 0x28, fht); put64(h + 0x30, 4ull * fbuckets);
    put64(h + 0x38, fmt); put64(h + 0x40, file_table_size);
    put64(h + 0x48, B->data_offset);
    memset(B->meta + dht, 0xff, 4ull * dbuckets);
    memset(B->meta + fht, 0xff, 4ull * fbuckets);
    /* walk all nodes in directory order again */
    int head = 0, tail = 0;
    queue[tail++] = root;
    while (head < tail) {
        Node *d = queue[head++];
        u32 parent = d->parent ? d->parent->off : 0;
        u8 *e = B->meta + dmt + d->off;
        u32 hash = path_hash(parent, d->name) % dbuckets;
        u32 nl = (u32)strlen(d->name);
        put32(e + 0x00, parent);
        put32(e + 0x04, d->parent ? next_sibling(d) : 0xffffffffu);
        put32(e + 0x08, first_child(d, true));
        put32(e + 0x0c, first_child(d, false));
        memcpy(e + 0x10, B->meta + dht + 4 * hash, 4);
        put32(B->meta + dht + 4 * hash, d->off);
        put32(e + 0x14, nl);
        memcpy(e + 0x18, d->name, nl);
        for (Node *k = d->child; k; k = k->sibling) {
            if (k->dir) { queue[tail++] = k; continue; }
            u8 *f = B->meta + fmt + k->off;
            u32 fh = path_hash(d->off, k->name) % fbuckets;
            u32 kl = (u32)strlen(k->name);
            put32(f + 0x00, d->off);
            put32(f + 0x04, next_sibling(k));
            put64(f + 0x08, B->files[k->file_index].offset);
            put64(f + 0x10, k->size);
            memcpy(f + 0x18, B->meta + fht + 4 * fh, 4);
            put32(B->meta + fht + 4 * fh, k->off);
            put32(f + 0x1c, kl);
            memcpy(f + 0x20, k->name, kl);
        }
    }
}

/* a virtual RomFS image of a host directory, or NULL if there is no such directory */
RomFs *romfs_open(const char *dir) {
    wchar_t wdir[1024];
    MultiByteToWideChar(CP_UTF8, 0, dir, -1, wdir, 1024);
    DWORD a = GetFileAttributesW(wdir);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return NULL;
    static SRWLOCK build_lock = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&build_lock);
    B = calloc(1, sizeof *B);
    count_dirs = count_files = 0;
    dir_table_size = file_table_size = 0;
    Node *root = scan(wdir, "", NULL);
    u64 pos = 0;
    assign(root, &pos);
    write_tables(root);
    B->total_size = B->data_offset + pos;
    RomFs *fs = B;
    B = NULL;
    hlog(1, "romfs %s: %d directories, %d files, image of %llu MB (tables %llu KB)", dir, count_dirs, count_files,
         fs->total_size >> 20, fs->meta_size >> 10);
    ReleaseSRWLockExclusive(&build_lock);
    return fs;
}

u64 romfs_size(RomFs *fs) { return fs->total_size; }

static HANDLE file_handle(RomFile *f) {
    HANDLE h = __atomic_load_n(&f->h, __ATOMIC_ACQUIRE);
    if (h) return h;
    AcquireSRWLockExclusive(&open_lock);
    if (!f->h) {
        h = CreateFileW(f->path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) fatal("romfs: cannot open %ls", f->path);
        __atomic_store_n(&f->h, h, __ATOMIC_RELEASE);
    }
    ReleaseSRWLockExclusive(&open_lock);
    return f->h;
}

/* reads [off, off+size) of the virtual image; returns false past the end */
bool romfs_read(RomFs *fs, u64 off, void *dst, u64 size) {
    u8 *out = dst;
    if (off + size > fs->total_size || off + size < off) return false;
    while (size) {
        if (off < fs->data_offset) {                     /* tables, and the padding after them */
            u64 n = fs->data_offset - off < size ? fs->data_offset - off : size;
            for (u64 i = 0; i < n; i++) out[i] = off + i < fs->meta_size ? fs->meta[off + i] : 0;
            off += n; out += n; size -= n;
            continue;
        }
        u64 d = off - fs->data_offset;
        int lo = 0, hi = fs->nfiles - 1;                  /* last file starting at or before d */
        while (lo < hi) {
            int mid = (lo + hi + 1) / 2;
            if (fs->files[mid].offset <= d) lo = mid; else hi = mid - 1;
        }
        RomFile *f = fs->nfiles ? &fs->files[lo] : NULL;
        if (!f || d < f->offset || d >= f->offset + f->size) {   /* padding between files */
            u64 next = fs->total_size - fs->data_offset;
            if (f && d < f->offset) next = f->offset;
            else if (f && lo + 1 < fs->nfiles) next = fs->files[lo + 1].offset;
            u64 n = next - d < size ? next - d : size;
            memset(out, 0, n);
            off += n; out += n; size -= n;
            continue;
        }
        u64 n = f->offset + f->size - d < size ? f->offset + f->size - d : size;
        if (d == f->offset && log_level >= 2) {          /* what the game loads (reads from a file's start) */
            const wchar_t *rel = wcsstr(f->path, L"romfs");
            hlog(2, "romfs: %ls (%llx of %llx bytes)", rel ? rel + 5 : f->path, n, f->size);
        }
        static const char *trace;                        /* MK8_FS_TRACE=<part of a path>: every read of that file */
        static wchar_t wtrace[260];
        if (!trace && (trace = getenv("MK8_FS_TRACE")) != NULL) MultiByteToWideChar(CP_UTF8, 0, trace, -1, wtrace, 260);
        else if (!trace) trace = "";
        if (wtrace[0] && wcsstr(f->path, wtrace))
            hlog(0, "romfs: read %ls +%llx size %llx", wcsrchr(f->path, L'/') ? wcsrchr(f->path, L'/') + 1 : f->path,
                 d - f->offset, n);
        HANDLE h = file_handle(f);
        u64 done = 0;
        while (done < n) {
            OVERLAPPED ov = {0};
            u64 fo = d - f->offset + done;
            ov.Offset = (DWORD)fo;
            ov.OffsetHigh = (DWORD)(fo >> 32);
            DWORD chunk = n - done > 0x40000000 ? 0x40000000 : (DWORD)(n - done), got = 0;
            if (!ReadFile(h, out + done, chunk, &got, &ov) || !got) fatal("romfs: read error in %ls", f->path);
            done += got;
        }
        off += n; out += n; size -= n;
    }
    return true;
}
