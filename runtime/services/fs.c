/* fsp-srv: the RomFS as an IStorage (see romfs.c), and save data as host folders behind IFileSystem.
 * Save data lives in <save dir>/<type>-<user id>/; it is created when first opened, as if the game's
 * EnsureSaveData had made it. */
#include "../ipc.h"
#include <stdlib.h>
#include <windows.h>

typedef struct RomFs RomFs;
RomFs *romfs_open(const char *dir);
u64 romfs_size(RomFs *fs);
bool romfs_read(RomFs *fs, u64 off, void *dst, u64 size);

#define FS_RES(d)              (0x02u | (u32)(d) << 9)
#define R_FS_PATH_NOT_FOUND    FS_RES(1)
#define R_FS_PATH_EXISTS       FS_RES(2)
#define R_FS_NOT_EMPTY         FS_RES(8)
#define R_FS_OUT_OF_RANGE      FS_RES(3005)
#define R_FS_TARGET_NOT_FOUND  FS_RES(1002)
#define R_FS_INVALID_ARGUMENT  FS_RES(6001)

static char save_root[1024], sysdata_root[1024];
static RomFs *game_romfs;

/* ---------------------------------------------------------------------------------------------- IStorage (RomFS) */
static void st_read(SvcObj *o, IpcReq *r) {
    u64 off = in_u64(r, 0), size = in_u64(r, 8);
    const IpcBuf *b = buf_out(r, 0);
    if (!b || b->size < size) { r->result = R_FS_INVALID_ARGUMENT; return; }
    if (!romfs_read(o->data, off, (void *)(uintptr_t)b->addr, size)) r->result = R_FS_OUT_OF_RANGE;
}
static void st_size(SvcObj *o, IpcReq *r) { out_u64(r, romfs_size(o->data)); }

static const Iface iface_storage = {"IStorage", (const IpcCmd[]){
    {0, st_read, "Read"},
    {2, ipc_noop, "Flush"},
    {4, st_size, "GetSize"},
    IPC_END}, NULL};

/* ---------------------------------------------------------------------------------------------- host paths */
typedef struct HostFs { wchar_t root[512]; } HostFs;

/* the guest path (an X buffer, "/a/b") under the file system's root; false if it tries to leave it */
static bool host_path(SvcObj *o, IpcReq *r, int xi, wchar_t *out, size_t n) {
    HostFs *fs = o->data;
    if (xi >= r->nx) return false;
    char p[0x302] = {0};
    u64 len = r->x[xi].size < 0x301 ? r->x[xi].size : 0x301;
    memcpy(p, (const void *)(uintptr_t)r->x[xi].addr, len);
    if (strstr(p, "..")) return false;
    wchar_t wp[0x302];
    MultiByteToWideChar(CP_UTF8, 0, p, -1, wp, 0x302);
    for (wchar_t *c = wp; *c; c++) if (*c == L'/') *c = L'\\';
    swprintf(out, n, L"%s%s%s", fs->root, wp[0] == L'\\' ? L"" : L"\\", wp);
    size_t l = wcslen(out);
    while (l > wcslen(fs->root) && out[l - 1] == L'\\') out[--l] = 0;
    return true;
}

static u32 win_error(void) {
    switch (GetLastError()) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: return R_FS_PATH_NOT_FOUND;
    case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: return R_FS_PATH_EXISTS;
    case ERROR_DIR_NOT_EMPTY: return R_FS_NOT_EMPTY;
    default: return R_FS_INVALID_ARGUMENT;
    }
}

/* ---------------------------------------------------------------------------------------------- IFile */
typedef struct HostFile { HANDLE h; u32 mode; } HostFile;

static void file_read(SvcObj *o, IpcReq *r) {
    HostFile *f = o->data;
    u64 off = in_u64(r, 8), size = in_u64(r, 16);
    if (!r->nb) { r->result = R_FS_INVALID_ARGUMENT; return; }
    if (size > r->b[0].size) size = r->b[0].size;
    u64 done = 0;
    while (done < size) {
        OVERLAPPED ov = {0};
        ov.Offset = (DWORD)(off + done);
        ov.OffsetHigh = (DWORD)((off + done) >> 32);
        DWORD got = 0, chunk = size - done > 0x40000000 ? 0x40000000 : (DWORD)(size - done);
        if (!ReadFile(f->h, (u8 *)(uintptr_t)r->b[0].addr + done, chunk, &got, &ov) || !got) break;
        done += got;
    }
    out_u64(r, done);
}

static void file_write(SvcObj *o, IpcReq *r) {
    HostFile *f = o->data;
    u64 off = in_u64(r, 8), size = in_u64(r, 16);
    if (!r->na || size > r->a[0].size) { r->result = R_FS_INVALID_ARGUMENT; return; }
    OVERLAPPED ov = {0};
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)(off >> 32);
    DWORD put = 0;
    if (size && !WriteFile(f->h, (const void *)(uintptr_t)r->a[0].addr, (DWORD)size, &put, &ov)) r->result = win_error();
}

static void file_flush(SvcObj *o, IpcReq *r) { FlushFileBuffers(((HostFile *)o->data)->h); }

static void file_set_size(SvcObj *o, IpcReq *r) {
    FILE_END_OF_FILE_INFO e;
    e.EndOfFile.QuadPart = (LONGLONG)in_u64(r, 0);
    if (!SetFileInformationByHandle(((HostFile *)o->data)->h, FileEndOfFileInfo, &e, sizeof e)) r->result = win_error();
}

static void file_get_size(SvcObj *o, IpcReq *r) {
    LARGE_INTEGER s;
    GetFileSizeEx(((HostFile *)o->data)->h, &s);
    out_u64(r, (u64)s.QuadPart);
}

static void file_destroy(SvcObj *o) {
    HostFile *f = o->data;
    CloseHandle(f->h);
    free(f);
}

static const Iface iface_file = {"IFile", (const IpcCmd[]){
    {0, file_read, "Read"},
    {1, file_write, "Write"},
    {2, file_flush, "Flush"},
    {3, file_set_size, "SetSize"},
    {4, file_get_size, "GetSize"},
    IPC_END}, file_destroy};

/* ---------------------------------------------------------------------------------------------- IDirectory */
typedef struct DirEntry { char name[0x301]; u8 pad[3]; s8 type; u8 pad2[3]; s64 size; } DirEntry;
typedef struct HostDir { DirEntry *e; u64 n, pos; } HostDir;

static void dir_read(SvcObj *o, IpcReq *r) {
    HostDir *d = o->data;
    u64 cap = r->nb ? r->b[0].size / sizeof(DirEntry) : 0, k = 0;
    DirEntry *out = r->nb ? (DirEntry *)(uintptr_t)r->b[0].addr : NULL;
    while (k < cap && d->pos < d->n) out[k++] = d->e[d->pos++];
    out_u64(r, k);
}

static void dir_count(SvcObj *o, IpcReq *r) { out_u64(r, ((HostDir *)o->data)->n); }

static void dir_destroy(SvcObj *o) {
    HostDir *d = o->data;
    free(d->e);
    free(d);
}

static const Iface iface_dir = {"IDirectory", (const IpcCmd[]){
    {0, dir_read, "Read"},
    {1, dir_count, "GetEntryCount"},
    IPC_END}, dir_destroy};

/* ---------------------------------------------------------------------------------------------- IFileSystem */
#define PATH_ARGS wchar_t p[1024]; if (!host_path(o, r, 0, p, 1024)) { r->result = R_FS_PATH_NOT_FOUND; return; }

static void fs_create_file(SvcObj *o, IpcReq *r) {
    PATH_ARGS
    u64 size = in_u64(r, 8);
    HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { r->result = win_error(); return; }
    FILE_END_OF_FILE_INFO e;
    e.EndOfFile.QuadPart = (LONGLONG)size;
    SetFileInformationByHandle(h, FileEndOfFileInfo, &e, sizeof e);
    CloseHandle(h);
}

static void fs_delete_file(SvcObj *o, IpcReq *r) { PATH_ARGS if (!DeleteFileW(p)) r->result = win_error(); }
static void fs_create_dir(SvcObj *o, IpcReq *r) { PATH_ARGS if (!CreateDirectoryW(p, NULL)) r->result = win_error(); }
static void fs_delete_dir(SvcObj *o, IpcReq *r) { PATH_ARGS if (!RemoveDirectoryW(p)) r->result = win_error(); }

static void delete_tree(const wchar_t *p, bool keep_root) {
    wchar_t pat[1100];
    swprintf(pat, 1100, L"%s\\*", p);
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(pat, &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            wchar_t sub[1100];
            swprintf(sub, 1100, L"%s\\%s", p, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) delete_tree(sub, false);
            else DeleteFileW(sub);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    if (!keep_root) RemoveDirectoryW(p);
}

static void fs_delete_dir_rec(SvcObj *o, IpcReq *r) { PATH_ARGS delete_tree(p, false); }
static void fs_clean_dir_rec(SvcObj *o, IpcReq *r) { PATH_ARGS delete_tree(p, true); }

static void fs_rename(SvcObj *o, IpcReq *r) {
    wchar_t a[1024], b[1024];
    if (!host_path(o, r, 0, a, 1024) || !host_path(o, r, 1, b, 1024)) { r->result = R_FS_PATH_NOT_FOUND; return; }
    if (!MoveFileExW(a, b, 0)) r->result = win_error();
}

static void fs_entry_type(SvcObj *o, IpcReq *r) {
    PATH_ARGS
    DWORD a = GetFileAttributesW(p);
    if (a == INVALID_FILE_ATTRIBUTES) { r->result = R_FS_PATH_NOT_FOUND; return; }
    out_u32(r, a & FILE_ATTRIBUTE_DIRECTORY ? 0 : 1);
}

static void fs_open_file(SvcObj *o, IpcReq *r) {
    PATH_ARGS
    u32 mode = in_u32(r, 0);
    DWORD access = (mode & 1 ? GENERIC_READ : 0) | (mode & 6 ? GENERIC_WRITE : 0);
    HANDLE h = CreateFileW(p, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { r->result = win_error(); return; }
    HostFile *f = calloc(1, sizeof *f);
    f->h = h;
    f->mode = mode;
    out_object(r, svcobj_new(&iface_file, f, NULL));
}

static void fs_open_dir(SvcObj *o, IpcReq *r) {
    PATH_ARGS
    u32 filter = in_u32(r, 0);                       /* 1 directories, 2 files */
    HostDir *d = calloc(1, sizeof *d);
    wchar_t pat[1100];
    swprintf(pat, 1100, L"%s\\*", p);
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) { free(d); r->result = R_FS_PATH_NOT_FOUND; return; }
    u64 cap = 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        bool isdir = fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
        if (!(filter & (isdir ? 1 : 2))) continue;
        if (d->n == cap) { cap = cap ? cap * 2 : 16; d->e = realloc(d->e, cap * sizeof *d->e); }
        DirEntry *e = &d->e[d->n++];
        memset(e, 0, sizeof *e);
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, e->name, sizeof e->name, NULL, NULL);
        e->type = isdir ? 0 : 1;
        e->size = isdir ? 0 : (s64)((u64)fd.nFileSizeHigh << 32 | fd.nFileSizeLow);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    out_object(r, svcobj_new(&iface_dir, d, NULL));
}

static void fs_free_space(SvcObj *o, IpcReq *r) { out_u64(r, 0x10000000); }
static void fs_total_space(SvcObj *o, IpcReq *r) { out_u64(r, 0x10000000); }

static void fs_destroy(SvcObj *o) { free(o->data); }

static const Iface iface_fs = {"IFileSystem", (const IpcCmd[]){
    {0, fs_create_file, "CreateFile"},
    {1, fs_delete_file, "DeleteFile"},
    {2, fs_create_dir, "CreateDirectory"},
    {3, fs_delete_dir, "DeleteDirectory"},
    {4, fs_delete_dir_rec, "DeleteDirectoryRecursively"},
    {5, fs_rename, "RenameFile"},
    {6, fs_rename, "RenameDirectory"},
    {7, fs_entry_type, "GetEntryType"},
    {8, fs_open_file, "OpenFile"},
    {9, fs_open_dir, "OpenDirectory"},
    {10, ipc_noop, "Commit"},
    {11, fs_free_space, "GetFreeSpaceSize"},
    {12, fs_total_space, "GetTotalSpaceSize"},
    {13, fs_clean_dir_rec, "CleanDirectoryRecursively"},
    IPC_END}, fs_destroy};

/* ---------------------------------------------------------------------------------------------- fsp-srv */
static void fsp_open_save(SvcObj *o, IpcReq *r) {
    u8 space = in_u8(r, 0);
    u64 program = in_u64(r, 8), user_lo = in_u64(r, 16), user_hi = in_u64(r, 24), sys_id = in_u64(r, 32);
    u8 type = in_u8(r, 40);
    HostFs *fs = calloc(1, sizeof *fs);
    wchar_t wroot[1024];
    MultiByteToWideChar(CP_UTF8, 0, save_root, -1, wroot, 1024);
    CreateDirectoryW(wroot, NULL);
    swprintf(fs->root, 512, L"%s\\%u-%016llx%016llx-%llx", wroot, type, user_hi, user_lo, sys_id);
    CreateDirectoryW(fs->root, NULL);
    hlog(1, "fs: OpenSaveDataFileSystem(space %u, program %llx, type %u) -> %ls", space, program, type, fs->root);
    out_object(r, svcobj_new(&iface_fs, fs, "save"));
}

static void fsp_open_romfs(SvcObj *o, IpcReq *r) {
    if (!game_romfs) { r->result = R_FS_TARGET_NOT_FOUND; return; }
    out_object(r, svcobj_new(&iface_storage, game_romfs, "romfs"));
}

/* system data archives: build/sysdata/<data id>/ holds the extracted archive (a firmware dump's, if the user puts
 * one there); missing ones get the minimal placeholder files below */
static const u8 nftr[16] = {'N', 'F', 'T', 'R', 1}, nfsr[16] = {'N', 'F', 'S', 'R', 1};
static const struct { u64 id; const char *name; const u8 *data; u32 size; } placeholders[] = {
    {0x0100000000000802ull, "NXTextureLowLinear.dat", nftr, 16},      /* MiiModel */
    {0x0100000000000802ull, "NXTextureLowSRGB.dat", nftr, 16},
    {0x0100000000000802ull, "NXTextureMidLinear.dat", nftr, 16},
    {0x0100000000000802ull, "NXTextureMidSRGB.dat", nftr, 16},
    {0x0100000000000802ull, "ShapeHigh.dat", nfsr, 16},
    {0x0100000000000802ull, "ShapeMid.dat", nfsr, 16},
};

static RomFs *sysdata[64];
static u64 sysdata_ids[64];
static int nsysdata;

static RomFs *open_sysdata(u64 id) {
    for (int i = 0; i < nsysdata; i++)
        if (sysdata_ids[i] == id) return sysdata[i];
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/%016llx", sysdata_root, id);
    RomFs *fs = romfs_open(dir);
    if (!fs) {
        bool made = false;
        for (size_t i = 0; i < sizeof placeholders / sizeof *placeholders; i++) {
            if (placeholders[i].id != id) continue;
            CreateDirectoryA(sysdata_root, NULL);
            CreateDirectoryA(dir, NULL);
            char path[1200];
            snprintf(path, sizeof path, "%s/%s", dir, placeholders[i].name);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(placeholders[i].data, 1, placeholders[i].size, f); fclose(f); }
            made = true;
        }
        if (made) {
            hlog(0, "fs: system data %016llx: wrote placeholder files to %s", id, dir);
            fs = romfs_open(dir);
        }
    }
    if (fs && nsysdata < 64) { sysdata_ids[nsysdata] = id; sysdata[nsysdata++] = fs; }
    return fs;
}

static void fsp_open_data_by_id(SvcObj *o, IpcReq *r) {
    u64 id = in_u64(r, 8);
    RomFs *fs = open_sysdata(id);
    if (!fs) {
        hlog(0, "fs: OpenDataStorageByDataId(storage %u, data %016llx): not available", in_u8(r, 0), id);
        r->result = R_FS_TARGET_NOT_FOUND;
        return;
    }
    out_object(r, svcobj_new(&iface_storage, fs, "sysdata"));
}

static void fsp_open_patch(SvcObj *o, IpcReq *r) { r->result = R_FS_TARGET_NOT_FOUND; }
static void fsp_log_mode(SvcObj *o, IpcReq *r) { out_u32(r, 0); }

static void fsp_open_sd(SvcObj *o, IpcReq *r) {
    hlog(1, "fs: OpenSdCardFileSystem: refused");
    r->result = FS_RES(2001);                         /* SD card not inserted */
}

static const Iface iface_fsp = {"fsp-srv", (const IpcCmd[]){
    {1, ipc_noop, "SetCurrentProcess"},
    {18, fsp_open_sd, "OpenSdCardFileSystem"},
    {22, ipc_noop, "CreateSaveDataFileSystem"},
    {51, fsp_open_save, "OpenSaveDataFileSystem"},
    {200, fsp_open_romfs, "OpenDataStorageByCurrentProcess"},
    {202, fsp_open_data_by_id, "OpenDataStorageByDataId"},
    {203, fsp_open_patch, "OpenPatchDataStorageByCurrentProcess"},
    {1003, ipc_noop, "DisableAutoSaveDataCreation"},
    {1004, ipc_noop, "SetGlobalAccessLogMode"},
    {1005, fsp_log_mode, "GetGlobalAccessLogMode"},
    {1006, ipc_noop, "OutputAccessLogToSdCard"},
    IPC_END}, NULL};

static SvcObj *open_fsp(const char *name) { return svcobj_new(&iface_fsp, NULL, name); }

static char romfs_dir[1100];
const char *fs_romfs_dir(void) { return romfs_dir; }      /* the host folder behind the game's RomFS (movie.c) */

void fs_register(const char *game_dir, const char *save_dir) {
    snprintf(romfs_dir, sizeof romfs_dir, "%s/romfs", game_dir);
    snprintf(save_root, sizeof save_root, "%s", save_dir);
    snprintf(sysdata_root, sizeof sysdata_root, "%s/../sysdata", save_dir);
    game_romfs = romfs_open(romfs_dir);
    if (!game_romfs) hlog(0, "fs: no RomFS at %s", romfs_dir);
    service_register("fsp-srv", open_fsp);
}
