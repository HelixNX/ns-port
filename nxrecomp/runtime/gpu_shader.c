/* Shaders: SPIR-V from the shader pack (shaders.pak) or the translator's files, the background translator (the
 * JIT, translate.py) for shaders without, and Vulkan shader modules.
 * Part of the NVN executor (gpu.c, gpu_int.h). */
#include "gpu_int.h"

/* ---- shaders */
char spv_dir[MAX_PATH];
static bool have_translator;            /* translate.py is there (the JIT can run) */

/* The shader pack (tools/shader/pack.c): every shader translated at build time, one zstd frame each, so a package
 * needs no translator. shaders.pak next to the executable, else in the cache's shaders\ (NX_SHADER_PACK names
 * another). Used only when it was made by the translator version in use (a package without translate.py takes the
 * pack's); loose files in spv\<version> (the JIT's) come first */
typedef struct PakEnt { u64 hash; u32 mask, size; u64 offset; u32 packed, pad; } PakEnt;
static struct { HANDLE f; PakEnt *ents; u32 n, translator; bool usable; } pak;

static void pak_open(void) {
    char path[MAX_PATH + 32];
    const char *e = getenv("NX_SHADER_PACK");
    if (e) snprintf(path, sizeof path, "%s", e);
    else {
        GetModuleFileNameA(NULL, path, MAX_PATH);
        char *s = strrchr(path, '\\');
        if (s) s[1] = 0;
        strcat(path, "shaders.pak");
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
            snprintf(path, sizeof path, "%s\\shaders\\shaders.pak", nx_cache_dir());
    }
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    u32 hdr[4];
    DWORD got;
    PakEnt *ents = NULL;
    if (ReadFile(f, hdr, sizeof hdr, &got, NULL) && got == sizeof hdr && hdr[0] == 0x5053584e && hdr[1] == 1 &&
        hdr[3] < (1u << 24) && (ents = malloc((size_t)hdr[3] * sizeof *ents + 1)) != NULL &&
        ReadFile(f, ents, hdr[3] * (DWORD)sizeof *ents, &got, NULL) && got == hdr[3] * sizeof *ents) {
        pak.f = f;
        pak.ents = ents;
        pak.n = hdr[3];
        pak.translator = hdr[2];
        hlog(1, "gpu: shader pack %s: %u shaders, translator %08x", path, pak.n, pak.translator);
        return;
    }
    hlog(0, "gpu: %s is not a shader pack", path);
    free(ents);
    CloseHandle(f);
}

static const PakEnt *pak_find(u64 hash, u32 mask) {
    if (!pak.usable) return NULL;
    u32 lo = 0, hi = pak.n;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        const PakEnt *p = &pak.ents[mid];
        if (p->hash < hash || (p->hash == hash && p->mask < mask)) lo = mid + 1;
        else hi = mid;
    }
    return lo < pak.n && pak.ents[lo].hash == hash && pak.ents[lo].mask == mask ? &pak.ents[lo] : NULL;
}

void spv_path(char *path, size_t size, u64 hash) {
    if (!spv_dir[0]) {
        snprintf(spv_dir, sizeof spv_dir, "%s\\shaders", nx_cache_dir());
        CreateDirectoryA(spv_dir, NULL);
        strcat(spv_dir, "\\spv");
        CreateDirectoryA(spv_dir, NULL);
        pak_open();
        /* translations live in a folder per translator version (FNV-1a of translate.py, as translate.py names it):
         * after a translator change every shader is translated again, nothing stale is used */
        char tp[MAX_PATH];
        translator_path(tp, sizeof tp);
        FILE *f = fopen(tp, "rb");
        u32 h = 0;
        if (f) {
            h = 2166136261u;
            int ch;
            while ((ch = fgetc(f)) != EOF) h = (h ^ (u8)ch) * 16777619u;
            fclose(f);
            have_translator = true;
        } else if (pak.f) h = pak.translator;
        if (pak.f) {
            pak.usable = pak.translator == h;
            if (!pak.usable) hlog(0, "gpu: the shader pack is from translator %08x, not %08x: not used", pak.translator, h);
        }
        if (f || pak.f) {
            size_t n = strlen(spv_dir);
            snprintf(spv_dir + n, sizeof spv_dir - n, "\\%08x", h);
            CreateDirectoryA(spv_dir, NULL);
            hlog(1, "gpu: shader translations in %s%s", spv_dir, have_translator ? "" : " (no translator: the pack only)");
        }
    }
    snprintf(path, size, "%s\\%016llx.spv", spv_dir, hash);
}

/* a shader's SPIR-V (malloc'd, *n bytes): the loose file, else the pack; NULL if neither has it */
u32 *spv_read(u64 hash, long *n) {
    char path[MAX_PATH + 32];
    spv_path(path, sizeof path, hash);
    FILE *f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        u32 *code = sz > 0 ? malloc(sz) : NULL;
        size_t got = code ? fread(code, 1, sz, f) : 0;
        fclose(f);
        if (code && got == (size_t)sz) { *n = sz; return code; }
        free(code);
    }
    const PakEnt *e = pak_find(hash, 0);
    if (!e) return NULL;
    void *z = malloc(e->packed);
    u32 *code = malloc(e->size + 4);
    OVERLAPPED ov = {0};                /* reads at an offset: no shared file position, any thread */
    ov.Offset = (DWORD)e->offset;
    ov.OffsetHigh = (DWORD)(e->offset >> 32);
    DWORD got = 0;
    bool ok = z && code && ReadFile(pak.f, z, e->packed, &got, &ov) && got == e->packed;
    if (ok) {
        size_t r = ZSTD_decompress(code, e->size, z, e->packed);
        ok = !ZSTD_isError(r) && r == e->size;
    }
    free(z);
    if (!ok) {
        hlog(0, "gpu: shader %016llx: bad entry in the shader pack", hash);
        free(code);
        return NULL;
    }
    *n = (long)e->size;
    return code;
}

static bool spv_exists(u64 hash) {
    char path[MAX_PATH + 32];
    spv_path(path, sizeof path, hash);
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES || pak_find(hash, 0);
}
struct JitQueue jit = {.lock = SRWLOCK_INIT};
static volatile u64 jit_done;          /* the last request a finished batch covered */
/* a translation batch is done, or the preload thread has loaded a shader: the render thread may be waiting for it
 * (shader_load, shader_module_w). It polled with Sleep(1) (to the next timer tick, up to 15.6 ms) and Sleep(0) */
static SRWLOCK shader_wait_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE shader_wait_cv = CONDITION_VARIABLE_INIT;

static volatile u64 jit_last_request_ms;   /* GetTickCount64 of the last request */

/* the translator's progress through the current burst of requests (vk.c: "COMPILING SHADERS done/total"); false
 * when it has nothing to do */
bool gpu_shader_progress(u32 *done, u32 *total) {
    static u64 base;                    /* jit.seq when the burst began */
    u64 seq = __atomic_load_n(&jit.seq, __ATOMIC_ACQUIRE), fin = __atomic_load_n(&jit_done, __ATOMIC_ACQUIRE);
    if (fin >= seq) { base = seq; return false; }
    if (base > fin) base = fin;
    *done = (u32)(fin - base);
    *total = (u32)(seq - base);
    return true;
}

/* the start-up's shaders: true until the translator first has nothing to do and nothing new came for 2 s (input is
 * held until then: the first start draws little while thousands are translated); false for good after that */
bool gpu_shaders_starting(void) {
    static volatile bool over;
    static u64 t0;
    if (over) return false;
    u64 now = GetTickCount64();
    if (!t0) t0 = now;
    u64 last = jit_last_request_ms ? jit_last_request_ms : t0;
    if (__atomic_load_n(&jit_done, __ATOMIC_ACQUIRE) >= __atomic_load_n(&jit.seq, __ATOMIC_ACQUIRE) && now - last > 2000)
        over = true;
    return !over;
}

static unsigned __stdcall jit_thread(void *p) {
    char dir[MAX_PATH];
    snprintf(dir, sizeof dir, "%s", spv_dir);
    char *slash = strstr(dir, "\\spv");                   /* the shader dir: above spv\<version> */
    if (slash) *slash = 0;
    const char *py = getenv("NX_PYTHON") ? getenv("NX_PYTHON") : "python";
    char tp[MAX_PATH];
    translator_path(tp, sizeof tp);
#ifdef NX_PACKAGE
    char pkpy[MAX_PATH];                                  /* the package's own Python, next to translate.py */
    snprintf(pkpy, sizeof pkpy, "%s", tp);
    char *ts = strrchr(pkpy, '\\');
    if (ts) snprintf(ts, sizeof pkpy - (size_t)(ts - pkpy), "\\python\\python.exe");
    if (!getenv("NX_PYTHON")) py = pkpy;
    /* translate.py takes glslang from %VULKAN_SDK%\Bin\glslangValidator.exe: the package's copy, translator\Bin\ */
    char sdk[MAX_PATH];
    snprintf(sdk, sizeof sdk, "%s", tp);
    if ((ts = strrchr(sdk, '\\')) != NULL) *ts = 0;
    SetEnvironmentVariableA("VULKAN_SDK", sdk);
#endif
    for (;;) {
        WaitForSingleObject(jit.wake, INFINITE);
        AcquireSRWLockExclusive(&jit.lock);
        u32 n = jit.n;
        u64 upto = jit.seq;
        u64 *hs = malloc(n * sizeof *hs + 1);
        u32 *ms = malloc(n * sizeof *ms + 1);
        memcpy(hs, jit.q, n * sizeof *hs);
        memcpy(ms, jit.qm, n * sizeof *ms);
        jit.n = 0;
        ReleaseSRWLockExclusive(&jit.lock);
        if (n) {
            char list[MAX_PATH + 16];           /* the hashes go in a file: the game makes ~2,300 shaders at once */
            snprintf(list, sizeof list, "%s\\jit_list.txt", dir);
            FILE *lf = fopen(list, "w");
            for (u32 i = 0; lf && i < n; i++)          /* <hash>:<mask>: a variant with integer inputs */
                if (ms[i]) fprintf(lf, "%016llx:%08x\n", hs[i], ms[i]);
                else fprintf(lf, "%016llx\n", hs[i]);
            if (lf) fclose(lf);
            size_t len = strlen(py) + strlen(tp) + 3 * strlen(dir) + 128;
            char *cmd = malloc(len);
            snprintf(cmd, len, "\"%s\" \"%s\" --spv \"--only=@%s\" \"%s\" \"%s\\glsl\"", py, tp, list, dir, dir);
            u64 t0 = GetTickCount64();
            /* its output through a pipe of our own (_popen needs the CRT's stdin/stdout, which may not be there) */
            SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
            HANDLE rd, wr;
            STARTUPINFOA si = {sizeof si};
            PROCESS_INFORMATION pi;
            bool ran = false;
            if (CreatePipe(&rd, &wr, &sa, 0)) {
                SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
                si.dwFlags = STARTF_USESTDHANDLES;
                si.hStdInput = NULL;
                si.hStdOutput = si.hStdError = wr;
                ran = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, NULL, &si, &pi);
                CloseHandle(wr);
                if (ran) {
                    /* translate.py runs glslang on every core at once, which starved the game's threads (a 133-shader
                     * batch: 2.6 s frames, the main thread waiting for a core 73% of the time). A job keeps it and
                     * its glslang processes at below-normal priority on the upper half of our cores */
                    static HANDLE job;
                    if (!job && (job = CreateJobObjectA(NULL, NULL))) {
                        DWORD_PTR mine = 0, sys = 0;
                        GetProcessAffinityMask(GetCurrentProcess(), &mine, &sys);
                        int nc = 0;
                        for (DWORD_PTR m = mine; m; m &= m - 1) nc++;
                        DWORD_PTR half = 0;
                        int keep = nc / 2, seen = 0;
                        for (int b = 63; b >= 0 && seen < keep; b--)
                            if (mine >> b & 1) { half |= (DWORD_PTR)1 << b; seen++; }
                        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {0};
                        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PRIORITY_CLASS |
                            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | (half ? JOB_OBJECT_LIMIT_AFFINITY : 0);
                        li.BasicLimitInformation.PriorityClass = BELOW_NORMAL_PRIORITY_CLASS;
                        li.BasicLimitInformation.Affinity = half;
                        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
                        hlog(1, "gpu: translate.py runs at below-normal priority on %d of %d cores", half ? keep : nc, nc);
                    }
                    if (job) AssignProcessToJobObject(job, pi.hProcess);
                    ResumeThread(pi.hThread);
                    char buf[4096], line[512];
                    DWORD got;
                    size_t ln = 0;
                    while (ReadFile(rd, buf, sizeof buf, &got, NULL) && got)
                        for (DWORD i = 0; i < got; i++) {
                            if (buf[i] != '\n' && buf[i] != '\r' && ln < sizeof line - 1) { line[ln++] = buf[i]; continue; }
                            if (buf[i] != '\n' && buf[i] != '\r') continue;
                            line[ln] = 0;
                            if (ln) hlog(1, "gpu: translate.py: %s", line);
                            ln = 0;
                        }
                    WaitForSingleObject(pi.hProcess, INFINITE);
                    DWORD code = 0;
                    GetExitCodeProcess(pi.hProcess, &code);
                    if (code) hlog(0, "gpu: translate.py exited with %lu", code);
                    CloseHandle(pi.hProcess);
                    CloseHandle(pi.hThread);
                }
                CloseHandle(rd);
            }
            if (!ran) hlog(0, "gpu: could not run %s (%lu)", py, GetLastError());
            hlog(1, "gpu: translate.py ran on %u shaders in %llu ms", n, GetTickCount64() - t0);
            free(cmd);
        }
        free(hs);
        free(ms);
        AcquireSRWLockExclusive(&shader_wait_lock);
        __atomic_store_n(&jit_done, upto, __ATOMIC_RELEASE);
        ReleaseSRWLockExclusive(&shader_wait_lock);
        WakeAllConditionVariable(&shader_wait_cv);
    }
    return 0;
}

/* queue a translation: the request's number (jit_done reaches it when its batch is done), 0 if the JIT is off */
static u64 jit_request_variant(u64 hash, u32 mask) {
    static volatile LONG state;         /* 0 not started, 1 starting, 2 on, 3 off */
    if (state != 2 && state != 3) {
        if (InterlockedCompareExchange(&state, 1, 0) == 0) {
            char tmp[MAX_PATH + 32];
            spv_path(tmp, sizeof tmp, 0);           /* finds the translator and the pack */
            bool on = env.shader_jit && have_translator;
            if (!have_translator) hlog(1, "gpu: no shader translator: shaders come from the pack, others are skipped");
            if (on) {
                jit.wake = CreateEventA(NULL, FALSE, FALSE, NULL);
                _beginthreadex(NULL, 0, jit_thread, NULL, 0, NULL);
            }
            InterlockedExchange(&state, on ? 2 : 3);
        }
        while (state == 1) Sleep(0);
    }
    if (state == 3) return 0;
    AcquireSRWLockExclusive(&jit.lock);
    if (jit.n == jit.cap) {
        jit.cap = jit.cap * 2 + 64;
        jit.q = realloc(jit.q, jit.cap * sizeof *jit.q);
        jit.qm = realloc(jit.qm, jit.cap * sizeof *jit.qm);
    }
    jit.qm[jit.n] = mask;
    jit.q[jit.n++] = hash;
    u64 seq = ++jit.seq;
    jit_last_request_ms = GetTickCount64();
    ReleaseSRWLockExclusive(&jit.lock);
    SetEvent(jit.wake);
    return seq;
}

static u64 jit_request(u64 hash) { return jit_request_variant(hash, 0); }

static VkShaderModule shader_load(ShaderCode *s, bool wait);

/* Shaders loaded ahead of their first draw: reading a translation and making its module took 10-35 ms on the
 * render thread in the frame that first drew with it (a 58 ms frame for three in the field). The game makes its
 * shader objects while it loads, well before it draws with them, so a thread loads each one then
 * (NX_SHADER_PRELOAD=0: at the first draw, as before). load_state says who loads a shader: this thread or the
 * draw that got there first */
static struct { SRWLOCK lock; CONDITION_VARIABLE cv; ShaderCode **q; u32 n, cap, head; bool started; } preload = {.lock = SRWLOCK_INIT};
static volatile LONG preloaded;

static unsigned __stdcall preload_thread(void *arg) {
    (void)arg;
    for (;;) {
        AcquireSRWLockExclusive(&preload.lock);
        while (preload.head == preload.n) SleepConditionVariableSRW(&preload.cv, &preload.lock, INFINITE, 0);
        ShaderCode *s = preload.q[preload.head++];
        if (preload.head == preload.n) preload.head = preload.n = 0;
        ReleaseSRWLockExclusive(&preload.lock);
        if (InterlockedCompareExchange(&s->load_state, 1, 0) != 0) continue;    /* a draw has it */
        shader_load(s, true);
        AcquireSRWLockExclusive(&shader_wait_lock);
        InterlockedExchange(&s->load_state, 2);
        ReleaseSRWLockExclusive(&shader_wait_lock);
        WakeAllConditionVariable(&shader_wait_cv);
        if (InterlockedIncrement(&preloaded) % 500 == 0) hlog(1, "gpu: %ld shaders loaded ahead of their draws", preloaded);
    }
}

static void preload_request(ShaderCode *s) {
    if (!env.shader_preload) return;
    AcquireSRWLockExclusive(&preload.lock);
    if (!preload.started) {
        preload.started = true;
        InitializeConditionVariable(&preload.cv);
        _beginthreadex(NULL, 0, preload_thread, NULL, 0, NULL);
    }
    if (preload.n == preload.cap) {
        preload.cap = preload.cap * 2 + 256;
        preload.q = realloc(preload.q, preload.cap * sizeof *preload.q);
    }
    preload.q[preload.n++] = s;
    ReleaseSRWLockExclusive(&preload.lock);
    WakeConditionVariable(&preload.cv);
}

/* nvn.c: the game made a shader (its code is dumped): translate it now if it has no SPIR-V, else load it */
void gpu_shader_created(ShaderCode *s) {
    if (!spv_exists(s->hash)) s->jit_seq = jit_request(s->hash);
    else preload_request(s);
}

static bool jit_pending(u64 seq) { return seq && __atomic_load_n(&jit_done, __ATOMIC_ACQUIRE) < seq; }

/* NULL either for good (s->missing) or, when !wait, while its translation is still running (try again later) */
static VkShaderModule shader_load(ShaderCode *s, bool wait) {
    long n = 0;
    u32 *code = spv_read(s->hash, &n);
    if (!code) {
        if (!s->jit_seq) s->jit_seq = jit_request(s->hash);
        u64 seq = s->jit_seq;
        if (!wait && jit_pending(seq)) return NULL;
        if (seq) {
            u64 t0 = GetTickCount64();
            AcquireSRWLockExclusive(&shader_wait_lock);
            while (__atomic_load_n(&jit_done, __ATOMIC_ACQUIRE) < seq)
                SleepConditionVariableSRW(&shader_wait_cv, &shader_wait_lock, INFINITE, 0);
            ReleaseSRWLockExclusive(&shader_wait_lock);
            hlog(1, "gpu: waited %llu ms for shader %016llx", GetTickCount64() - t0, s->hash);
            hf.jit += (double)(GetTickCount64() - t0);
            hf.njit++;
            code = spv_read(s->hash, &n);
        }
        if (!code) {
            s->missing = true;
            hlog(0, "gpu: shader %016llx has no translation%s", s->hash,
                 seq ? " (translate.py failed on it)" : have_translator ? "" : " (not in the shader pack: its draws are skipped)");
            return NULL;
        }
    }
    u32 cb_id[17], ncb = 0;             /* the cbN variables (OpName comes before OpDecorate) */
    u8 cb_n[17];
    for (u32 k = 0; k < 16; k++) s->cb_of_slot[k] = (u8)k;
    s->slots_used = 0;
    /* NX_FEEDBACK_LOG: the texture slots read, from the access chains into cb2 (cb2.d[i][j]: byte 16i + 4j) */
    u32 cb2 = 0, bound = n >= 20 ? code[3] : 0;
    u64 *cval = env.feedback_log && bound ? calloc(bound, sizeof *cval) : NULL;    /* OpConstant: 1 << 32 | value */
    s->tex_slots = 0;
    s->tex_dyn = false;
    s->texint_slots = 0;
    u32 texint_id = 0;                  /* the constant decorated SpecId 2 (TEX_INT) */
    for (long i = 5; i < n / 4;) {      /* what the shader touches: PhysicalStorageBufferAddresses, sampled images */
        u32 op = code[i] & 0xffff, wc = code[i] >> 16;
        if (op == 71 && wc == 4 && code[i + 2] == 1 && code[i + 3] == 2) texint_id = code[i + 1];
        if (op == 50 && wc == 4 && texint_id && code[i + 2] == texint_id) s->texint_slots = code[i + 3];   /* its default */
        if (op == 5 && wc >= 3 && ncb < 17) {                   /* OpName "cb<N>": c[N] */
            const char *nm = (const char *)&code[i + 2];
            if (nm[0] == 'c' && nm[1] == 'b' && nm[2] >= '0' && nm[2] <= '9') {
                u32 v = 0, k = 2;
                while (nm[k] >= '0' && nm[k] <= '9' && k < 5) v = v * 10 + (u32)(nm[k++] - '0');
                if (!nm[k] && v <= 16) { cb_id[ncb] = code[i + 1]; cb_n[ncb++] = (u8)v; }
                if (!nm[k] && v == 2) cb2 = code[i + 1];
            }
        }
        if (cval && op == 43 && wc == 4 && code[i + 2] < bound) cval[code[i + 2]] = 1ull << 32 | code[i + 3];
        if (cval && (op == 65 || op == 66) && wc >= 4 && cb2 && code[i + 3] == cb2) {  /* Op(InBounds)AccessChain */
            const u32 *x = &code[i + 4];
            if (wc == 7 && x[1] < bound && x[2] < bound && cval[x[1]] && cval[x[2]]) {
                u32 off = (u32)cval[x[1]] * 16 + (u32)cval[x[2]] * 4;
                if (off >= 0x20 && off < 0x120 && !(off & 7)) s->tex_slots |= 1u << ((off - 0x20) >> 3);
            } else s->tex_dyn = true;
        }
        if (op == 71 && wc == 4 && code[i + 2] == 33)          /* OpDecorate Binding: its slot */
            for (u32 k = 0; k < ncb; k++)
                if (cb_id[k] == code[i + 1]) {
                    s->cb_of_slot[code[i + 3] & 15] = cb_n[k];
                    s->slots_used |= (u16)(1u << (code[i + 3] & 15));
                }
        if (op == 17 && wc == 2 && code[i + 1] == 5347) s->caps |= SHC_GLOBAL;
        if (op == 17 && wc == 2 && code[i + 1] == 53) s->caps |= SHC_XFB;         /* TransformFeedback */
        if (op == 27) s->caps |= SHC_TEXTURE;
        if (op == 25 && wc >= 9 && code[i + 7] == 1 && (code[i + 3] != 1 || code[i + 5]))
            s->caps |= SHC_TEX_NON2D;           /* OpTypeImage, sampled, not plain 2D (Dim 1, not arrayed) */
        if (op == 25 && wc >= 9 && code[i + 7] == 2) s->caps |= SHC_IMAGE;     /* OpTypeImage, Sampled = 2: storage */
        i += wc ? wc : 1;
    }
    free(cval);
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)n, .pCode = code};
    VkShaderModule m;
    VkResult r = vkCreateShaderModule(vk.dev, &ci, NULL, &m);
    free(code);
    if (r != VK_SUCCESS) {
        s->missing = true;
        hlog(0, "gpu: shader %016llx: vkCreateShaderModule failed", s->hash);
        return NULL;
    }
    s->module = m;
    return m;
}

/* wait: false for draws with NX_ASYNC_SHADERS (shader_load) */
VkShaderModule shader_module_w(ShaderCode *s, bool wait) {
    if (s->module) return s->module;
    if (s->missing) return NULL;
    if (!wait && jit_pending(s->jit_seq)) return NULL;     /* cheap: no file lookup while it is being translated */
    double t0 = ms_now();
    VkShaderModule m;
    if (InterlockedCompareExchange(&s->load_state, 1, 0) == 1) {    /* the preload thread is on it: nearly done */
        AcquireSRWLockExclusive(&shader_wait_lock);
        while (s->load_state == 1) SleepConditionVariableSRW(&shader_wait_cv, &shader_wait_lock, INFINITE, 0);
        ReleaseSRWLockExclusive(&shader_wait_lock);
        m = s->module;
    } else {                            /* ours (or loaded and failed before: as it was) */
        m = shader_load(s, wait);
        InterlockedExchange(&s->load_state, m || s->missing ? 2 : 0);
    }
    hf.shader += ms_now() - t0;
    hf.nshader++;
    return m;
}

/* the integer-input mask of the bound vertex attributes: 2 bits per location, 1 unsigned, 2 signed (8/16-bit
 * UINT / SINT, fetched as USCALED / SSCALED: the vertex shader's INT_ATTRS specialization constant, translate.py) */
u32 int_attr_mask(void) {
    u32 mask = 0;
    for (u32 i = 0; i < st.nattribs && i < 16; i++) {
        u32 f = VA_FORMAT(st.attribs[i]);
        if (!f || VA_STREAM(st.attribs[i]) >= 31) continue;
        VkFormat vf = vk_format(f);
        u32 k = vf >= VK_FORMAT_R8_UNORM && vf <= VK_FORMAT_R8G8B8A8_SRGB ? (vf - VK_FORMAT_R8_UNORM) % 7 :
                vf >= VK_FORMAT_R16_UNORM && vf <= VK_FORMAT_R16G16B16A16_SFLOAT ? (vf - VK_FORMAT_R16_UNORM) % 7 : 0;
        if (k == 4) mask |= 1u << (2 * i);              /* UINT */
        else if (k == 5) mask |= 2u << (2 * i);         /* SINT */
    }
    return mask;
}
