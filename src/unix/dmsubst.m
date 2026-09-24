/* SPDX-License-Identifier: LGPL-2.1-or-later */

/* PROTOTYPE: hand D3DMetal the OpenXR runtime's swapchain MTLTexture.
 *
 * See src/include/dmsubst.h. Technique after utmapp/d3dmetal-native
 * (src/dmn_share_metal.mm, MIT): swizzle the Metal texture-creation selectors
 * on the concrete device/heap/buffer classes; a thread-local "arm" set by the
 * bridge right before ID3D11Device::CreateTexture2D makes the next texture
 * D3DMetal creates on that thread come back as ours.
 *
 * Unlike d3dmetal-native we are inside the same process as the OpenXR runtime,
 * so the substitute is the runtime's real texture (or a view of it) - no shm,
 * no linear/buffer-backed impostor, no copy.
 *
 * Everything here also logs, because the point of the prototype is to learn
 * what D3DMetal 4.0 actually does: OXR_DMSUBST_TRACE=1 logs substitutions,
 * heap/residency interaction with substituted textures and queue creation;
 * OXR_DMSUBST_TRACE=2 additionally logs every Metal allocation D3DMetal makes.
 * Logs go to stderr prefixed "[dmsubst]". */

#import <Metal/Metal.h>
#import <objc/runtime.h>
#import <objc/message.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach-o/dyld.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>

#include "wine/debug_slim.h"
#include "dmsubst.h"


typedef int32_t NTSTATUS;
#define STATUS_SUCCESS 0

/* ---- logging ------------------------------------------------------------- */

static int trace_level(void)
{
    static int level = -1;
    if (level < 0)
    {
        const char *e = getenv("OXR_DMSUBST_TRACE");
        level = e && *e ? atoi(e) : 0;
    }
    return level;
}

static uint64_t tid(void)
{
    uint64_t t = 0;
    pthread_threadid_np(NULL, &t);
    return t;
}

/* ---- the bridge log file -------------------------------------------------
 * Always on, low volume: session/swapchain setup, substitution results,
 * errors, a backend summary. Independent of WINEDEBUG and of where the game's
 * stderr goes (usually nowhere when launched from Steam). Path:
 * $OXR_BRIDGE_LOG, default /tmp/wineopenxr.log; OXR_BRIDGE_LOG=0 disables */

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

void oxr_bridge_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void oxr_bridge_log(const char *fmt, ...)
{
    const char *path = getenv("OXR_BRIDGE_LOG");
    char line[1024], stamp[32];
    struct timespec ts;
    struct tm tm;
    va_list ap;
    FILE *f;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);

    fprintf(stderr, "[wineopenxr] %s\n", line);
    if (path && !strcmp(path, "0"))
        return;
    pthread_mutex_lock(&g_log_lock);
    f = fopen(path && *path ? path : "/tmp/wineopenxr.log", "a");
    if (f)
    {
        fprintf(f, "%s.%03ld [pid %d] %s\n", stamp, ts.tv_nsec / 1000000, (int)getpid(), line);
        fclose(f);
    }
    pthread_mutex_unlock(&g_log_lock);
}

#define DLOG(lvl, fmt, ...) do { if (trace_level() >= (lvl)) \
    fprintf(stderr, "[dmsubst tid=%llu] " fmt "\n", (unsigned long long)tid(), ##__VA_ARGS__); } while (0)
#define DERR(fmt, ...) \
    oxr_bridge_log("dmsubst ERROR: " fmt, ##__VA_ARGS__)

static const char *desc_str(MTLTextureDescriptor *d, char *buf, size_t n)
{
    snprintf(buf, n, "%lux%lu fmt=%lu type=%lu arr=%lu mips=%lu samples=%lu usage=0x%lx storage=%lu",
             (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.pixelFormat,
             (unsigned long)d.textureType, (unsigned long)d.arrayLength,
             (unsigned long)d.mipmapLevelCount, (unsigned long)d.sampleCount,
             (unsigned long)d.usage, (unsigned long)d.storageMode);
    return buf;
}

/* Which image called us (for "who queried -heap?" style questions) */
static const char *caller_image(void *ret)
{
    Dl_info info;
    const char *slash;
    if (!ret || !dladdr(ret, &info) || !info.dli_fname)
        return "?";
    slash = strrchr(info.dli_fname, '/');
    return slash ? slash + 1 : info.dli_fname;
}

/* ---- global counters ----------------------------------------------------- */

static _Atomic uint64_t g_n_textures, g_n_heaps, g_n_resset_add, g_n_resset_add_subst,
    g_n_heap_queries_subst, g_n_mtl4_queues, g_n_classic_queues, g_n_mtl4_commits;

/* ---- the set of objects we handed D3DMetal ------------------------------- */

#define MAX_SUBST 256
static _Atomic(void *) g_subst[MAX_SUBST];
static _Atomic unsigned g_nsubst;

static void subst_note(id obj)
{
    unsigned i = atomic_fetch_add(&g_nsubst, 1);
    if (i < MAX_SUBST)
        atomic_store(&g_subst[i], (void *)obj);
}

/* runtime textures whose views we handed out (tracked apart from the views) */
static _Atomic(void *) g_parent[MAX_SUBST];
static _Atomic unsigned g_nparent;

static void parent_note(id obj)
{
    unsigned i = atomic_fetch_add(&g_nparent, 1);
    if (i < MAX_SUBST)
        atomic_store(&g_parent[i], (void *)obj);
}

static int is_parent(id obj)
{
    unsigned n = atomic_load(&g_nparent), i;
    if (n > MAX_SUBST)
        n = MAX_SUBST;
    for (i = 0; obj && i < n; i++)
        if (atomic_load(&g_parent[i]) == (void *)obj)
            return 1;
    return 0;
}

static int is_subst(id obj)
{
    unsigned n = atomic_load(&g_nsubst), i;
    if (!obj)
        return 0;
    if (n > MAX_SUBST)
        n = MAX_SUBST;
    for (i = 0; i < n; i++)
        if (atomic_load(&g_subst[i]) == (void *)obj)
            return 1;
    return 0;
}

/* ---- swizzle bookkeeping -------------------------------------------------
 * Originals are keyed by (class that IMPLEMENTS the method, selector), as in
 * d3dmetal-native: method_setImplementation mutates the Method wherever it is
 * defined, so recording under a subclass would lose it for siblings. */

#define MAX_ORIG 256
struct orig_entry { Class cls; SEL sel; IMP imp; };
static struct orig_entry g_orig[MAX_ORIG];
static _Atomic unsigned g_norig;
static pthread_mutex_t g_install_lock = PTHREAD_MUTEX_INITIALIZER;

static IMP lookup_orig(Class c, SEL sel)
{
    unsigned n = atomic_load_explicit(&g_norig, memory_order_acquire), i;
    Class k;
    for (k = c; k; k = class_getSuperclass(k))
        for (i = 0; i < n; i++)
            if (g_orig[i].cls == k && g_orig[i].sel == sel)
                return g_orig[i].imp;
    return NULL;
}

static Class implementing_class(Class cls, Method m)
{
    Class k;
    for (k = cls; k; k = class_getSuperclass(k))
    {
        unsigned n = 0, i;
        Method *list = class_copyMethodList(k, &n);
        int here = 0;
        for (i = 0; i < n && !here; i++)
            here = list[i] == m;
        free(list);
        if (here)
            return k;
    }
    return cls;
}

/* caller holds g_install_lock */
static int install_one(Class cls, SEL sel, IMP repl, const char *what)
{
    Method m;
    Class owner;
    unsigned n, i;

    if (!cls)
        return 0;
    m = class_getInstanceMethod(cls, sel);
    if (!m)
    {
        DLOG(1, "%s: class %s does not implement %s", what, class_getName(cls), sel_getName(sel));
        return 0;
    }
    owner = implementing_class(cls, m);
    n = atomic_load(&g_norig);
    for (i = 0; i < n; i++)
        if (g_orig[i].cls == owner && g_orig[i].sel == sel)
            return 0; /* already ours */
    if (method_getImplementation(m) == repl)
        return 0;
    if (n >= MAX_ORIG)
    {
        DERR("swizzle table full");
        return 0;
    }
    g_orig[n].cls = owner;
    g_orig[n].sel = sel;
    g_orig[n].imp = method_getImplementation(m);
    atomic_store_explicit(&g_norig, n + 1, memory_order_release);
    method_setImplementation(m, repl);
    DLOG(1, "swizzled -[%s %s] (%s, via %s)", class_getName(owner), sel_getName(sel), what,
         class_getName(cls));
    return 1;
}

struct job { const char *sel; IMP repl; };

static void install_jobs(Class cls, const struct job *jobs, size_t n, const char *what)
{
    size_t i;
    if (!cls)
        return;
    pthread_mutex_lock(&g_install_lock);
    for (i = 0; i < n; i++)
        install_one(cls, sel_registerName(jobs[i].sel), jobs[i].repl, what);
    pthread_mutex_unlock(&g_install_lock);
}

#define ORIG(self_, cmd_) lookup_orig(object_getClass(self_), cmd_)

/* ---- the arm --------------------------------------------------------------- */

struct arm
{
    int armed;
    int probe;
    id<MTLTexture> tex;      /* borrowed; the runtime owns it */
    int captured;
    int is_view;
    unsigned seen;
    uint64_t global_at_arm;
    int have_desc;
    MTLPixelFormat pf;
    MTLTextureType type;
    NSUInteger w, h, arr, mips;
    MTLTextureUsage usage;
    MTLStorageMode storage;
    int same_device;
    char where[48];
    int want_event;
    int event_shared;
    int capture;
    id created;
    id event;                /* captured, +1 */
    unsigned events_seen;
};

static __thread struct arm t_arm;

static void class_swizzle_texture(id tex);

/* Build what D3DMetal gets back: the runtime's texture itself when it matches
 * the descriptor exactly, else a view of it (the runtime allocates its images
 * with MTLTextureUsagePixelFormatView, so a typeless->typed or sRGB<->linear
 * reinterpretation is legal). Returns +1 or nil. */
static id<MTLTexture> make_substitute(id<MTLDevice> dev, MTLTextureDescriptor *desc, const char *where)
{
    id<MTLTexture> tex = t_arm.tex;
    NSUInteger want_arr = desc.textureType == MTLTextureType2DArray ? desc.arrayLength : 1;
    NSUInteger have_arr = tex.textureType == MTLTextureType2DArray ? tex.arrayLength : 1;
    char b[200];
    id<MTLTexture> out;

    if (desc.width != tex.width || desc.height != tex.height || desc.sampleCount != tex.sampleCount ||
        desc.mipmapLevelCount != tex.mipmapLevelCount || want_arr != have_arr ||
        (desc.textureType != MTLTextureType2D && desc.textureType != MTLTextureType2DArray))
    {
        DERR("%s: armed descriptor (%s) does not fit the runtime texture "
             "(%lux%lu fmt=%lu type=%lu arr=%lu mips=%lu); not substituting", where,
             desc_str(desc, b, sizeof(b)), (unsigned long)tex.width, (unsigned long)tex.height,
             (unsigned long)tex.pixelFormat, (unsigned long)tex.textureType,
             (unsigned long)tex.arrayLength, (unsigned long)tex.mipmapLevelCount);
        return nil;
    }
    if (desc.usage & ~tex.usage)
        DERR("%s: D3DMetal wants usage 0x%lx, the runtime texture has 0x%lx (missing 0x%lx)",
             where, (unsigned long)desc.usage, (unsigned long)tex.usage,
             (unsigned long)(desc.usage & ~tex.usage));
    if (desc.storageMode != tex.storageMode)
        DLOG(1, "%s: storage mode differs (D3DMetal %lu, runtime %lu)", where,
             (unsigned long)desc.storageMode, (unsigned long)tex.storageMode);
    if (dev != tex.device)
        DERR("%s: D3DMetal's device %p is not the runtime texture's device %p", where,
             (void *)dev, (void *)tex.device);

    if (desc.pixelFormat == tex.pixelFormat && desc.textureType == tex.textureType &&
        !getenv("OXR_DMSUBST_FORCE_VIEW"))
    {
        out = [tex retain];
        t_arm.is_view = 0;
    }
    else
    {
        out = [tex newTextureViewWithPixelFormat:desc.pixelFormat
                                     textureType:desc.textureType
                                          levels:NSMakeRange(0, tex.mipmapLevelCount)
                                          slices:NSMakeRange(0, have_arr)];
        if (!out)
        {
            DERR("%s: view fmt=%lu type=%lu of the runtime texture (fmt=%lu type=%lu) failed",
                 where, (unsigned long)desc.pixelFormat, (unsigned long)desc.textureType,
                 (unsigned long)tex.pixelFormat, (unsigned long)tex.textureType);
            return nil;
        }
        t_arm.is_view = 1;
    }
    subst_note(out);
    if (out != tex)
    {
        parent_note(tex);
        class_swizzle_texture(tex);
    }
    class_swizzle_texture(out);
    DLOG(1, "SUBSTITUTED in %s: D3DMetal asked %s -> runtime tex %p%s (%p)", where,
         desc_str(desc, b, sizeof(b)), (void *)tex, t_arm.is_view ? " as view" : "", (void *)out);
    return out;
}

/* Common prologue of every texture-creation hook. Returns the substitute (+1)
 * or nil to fall through to the original. */
static id<MTLTexture> on_texture_create(id<MTLDevice> dev, MTLTextureDescriptor *desc, const char *where)
{
    char b[200];
    atomic_fetch_add(&g_n_textures, 1);
    if (!t_arm.armed)
    {
        DLOG(2, "alloc %-28s %s", where, desc_str(desc, b, sizeof(b)));
        return nil;
    }
    t_arm.seen++;
    DLOG(1, "ARMED alloc %-22s %s (#%u on this thread)", where, desc_str(desc, b, sizeof(b)), t_arm.seen);
    if (!t_arm.have_desc)
    {
        t_arm.have_desc = 1;
        t_arm.pf = desc.pixelFormat;
        t_arm.type = desc.textureType;
        t_arm.w = desc.width;
        t_arm.h = desc.height;
        t_arm.arr = desc.arrayLength;
        t_arm.mips = desc.mipmapLevelCount;
        t_arm.usage = desc.usage;
        t_arm.storage = desc.storageMode;
        t_arm.same_device = t_arm.tex ? dev == t_arm.tex.device : 0;
        snprintf(t_arm.where, sizeof(t_arm.where), "%s", where);
    }
    if (t_arm.probe || !t_arm.tex || t_arm.captured)
        return nil;
    {
        id<MTLTexture> sub = make_substitute(dev, desc, where);
        if (sub)
        {
            t_arm.captured = 1;
            snprintf(t_arm.where, sizeof(t_arm.where), "%s", where);
        }
        return sub;
    }
}

/* ---- hooks: texture creation ------------------------------------------------ */

static void class_swizzle_heap(id heap);
static void class_swizzle_buffer(id buf);
static void class_swizzle_resset(id set);
static void class_swizzle_mtl4q(id q);
static void class_swizzle_classicq(id q);
static void class_swizzle_cb(id cb);
static void class_swizzle_texview(id tex);

static id swz_dev_newtex(id self, SEL _cmd, MTLTextureDescriptor *desc)
{
    IMP orig = ORIG(self, _cmd);
    id sub = on_texture_create(self, desc, "dev newTextureWithDescriptor:");
    id tex;
    if (sub)
        return sub;
    tex = orig ? ((id (*)(id, SEL, MTLTextureDescriptor *))orig)(self, _cmd, desc) : nil;
    if (t_arm.armed && t_arm.capture && !t_arm.created && tex)
        t_arm.created = [tex retain];
    class_swizzle_texview(tex);
    return tex;
}

static id swz_dev_newsharedtex(id self, SEL _cmd, MTLTextureDescriptor *desc)
{
    IMP orig = ORIG(self, _cmd);
    id sub = on_texture_create(self, desc, "dev newSharedTextureWithDescriptor:");
    if (sub)
        return sub;
    return orig ? ((id (*)(id, SEL, MTLTextureDescriptor *))orig)(self, _cmd, desc) : nil;
}

static id swz_dev_newtex_ios(id self, SEL _cmd, MTLTextureDescriptor *desc, void *ios, NSUInteger plane)
{
    IMP orig = ORIG(self, _cmd);
    id sub = on_texture_create(self, desc, "dev newTextureWithDescriptor:iosurface:");
    if (sub)
        return sub;
    return orig ? ((id (*)(id, SEL, MTLTextureDescriptor *, void *, NSUInteger))orig)(self, _cmd, desc, ios, plane) : nil;
}

static id swz_heap_newtex(id self, SEL _cmd, MTLTextureDescriptor *desc)
{
    IMP orig = ORIG(self, _cmd);
    id sub = on_texture_create([(id<MTLHeap>)self device], desc, "heap newTextureWithDescriptor:");
    if (sub)
        return sub;
    return orig ? ((id (*)(id, SEL, MTLTextureDescriptor *))orig)(self, _cmd, desc) : nil;
}

static id swz_heap_newtex_off(id self, SEL _cmd, MTLTextureDescriptor *desc, NSUInteger off)
{
    IMP orig = ORIG(self, _cmd);
    id sub = on_texture_create([(id<MTLHeap>)self device], desc, "heap newTextureWithDescriptor:offset:");
    if (sub)
        return sub;
    return orig ? ((id (*)(id, SEL, MTLTextureDescriptor *, NSUInteger))orig)(self, _cmd, desc, off) : nil;
}

static id swz_buf_newtex(id self, SEL _cmd, MTLTextureDescriptor *desc, NSUInteger off, NSUInteger bpr)
{
    IMP orig = ORIG(self, _cmd);
    id sub = on_texture_create([(id<MTLBuffer>)self device], desc, "buf newTextureWithDescriptor:offset:bytesPerRow:");
    if (sub)
        return sub;
    return orig ? ((id (*)(id, SEL, MTLTextureDescriptor *, NSUInteger, NSUInteger))orig)(self, _cmd, desc, off, bpr) : nil;
}

/* ---- hooks: events (GPU-sync investigation) ------------------------------- */

static id on_event_create(id ev, const char *where)
{
    if (t_arm.armed && t_arm.want_event)
    {
        t_arm.events_seen++;
        DLOG(1, "ARMED %s -> %s %p", where, ev ? class_getName(object_getClass(ev)) : "nil", (void *)ev);
        if (!t_arm.event && ev)
        {
            t_arm.event = [ev retain];
            snprintf(t_arm.where, sizeof(t_arm.where), "%s", where);
        }
    }
    else
        DLOG(2, "%s -> %p", where, (void *)ev);
    return ev;
}

static id swz_dev_newsharedevent(id self, SEL _cmd)
{
    IMP orig = ORIG(self, _cmd);
    return on_event_create(orig ? ((id (*)(id, SEL))orig)(self, _cmd) : nil, "dev newSharedEvent");
}

static id swz_dev_newsharedevent_opts(id self, SEL _cmd, NSUInteger opts)
{
    IMP orig = ORIG(self, _cmd);
    return on_event_create(orig ? ((id (*)(id, SEL, NSUInteger))orig)(self, _cmd, opts) : nil,
                           "dev newSharedEventWithOptions:");
}

static id swz_dev_newevent(id self, SEL _cmd)
{
    IMP orig = ORIG(self, _cmd);
    if (t_arm.armed && t_arm.event_shared && !t_arm.event)
    {
        /* MTLSharedEvent conforms to MTLEvent: D3DMetal can signal it like its
         * own, and the CPU (and other queues) can observe it */
        IMP mk = lookup_orig(object_getClass(self), @selector(newSharedEvent));
        id ev = mk ? ((id (*)(id, SEL))mk)(self, @selector(newSharedEvent)) : nil;
        if (ev)
        {
            DLOG(1, "SUBSTITUTED dev newEvent with MTLSharedEvent %p", (void *)ev);
            return on_event_create(ev, "dev newEvent (substituted MTLSharedEvent)");
        }
    }
    return on_event_create(orig ? ((id (*)(id, SEL))orig)(self, _cmd) : nil, "dev newEvent");
}

/* ---- hooks: heaps, buffers, queues, residency sets (trace + class discovery) */

static id swz_dev_newheap(id self, SEL _cmd, MTLHeapDescriptor *desc)
{
    IMP orig = ORIG(self, _cmd);
    id heap = orig ? ((id (*)(id, SEL, MTLHeapDescriptor *))orig)(self, _cmd, desc) : nil;
    atomic_fetch_add(&g_n_heaps, 1);
    DLOG(t_arm.armed ? 1 : 2, "%snewHeapWithDescriptor: size=%lu type=%ld storage=%lu -> %s %p",
         t_arm.armed ? "ARMED " : "", (unsigned long)desc.size, (long)desc.type,
         (unsigned long)desc.storageMode, heap ? class_getName(object_getClass(heap)) : "nil", (void *)heap);
    class_swizzle_heap(heap);
    return heap;
}

static id swz_dev_newbuf(id self, SEL _cmd, NSUInteger len, MTLResourceOptions opts)
{
    IMP orig = ORIG(self, _cmd);
    id buf = orig ? ((id (*)(id, SEL, NSUInteger, MTLResourceOptions))orig)(self, _cmd, len, opts) : nil;
    DLOG(t_arm.armed ? 1 : 3, "%snewBufferWithLength:%lu options:0x%lx", t_arm.armed ? "ARMED " : "",
         (unsigned long)len, (unsigned long)opts);
    class_swizzle_buffer(buf);
    return buf;
}

static id swz_dev_newq(id self, SEL _cmd)
{
    IMP orig = ORIG(self, _cmd);
    id q = orig ? ((id (*)(id, SEL))orig)(self, _cmd) : nil;
    atomic_fetch_add(&g_n_classic_queues, 1);
    DLOG(1, "classic newCommandQueue -> %s %p (from %s)", q ? class_getName(object_getClass(q)) : "nil",
         (void *)q, caller_image(__builtin_return_address(0)));
    class_swizzle_classicq(q);
    return q;
}

static id swz_dev_newqmax(id self, SEL _cmd, NSUInteger max)
{
    IMP orig = ORIG(self, _cmd);
    id q = orig ? ((id (*)(id, SEL, NSUInteger))orig)(self, _cmd, max) : nil;
    atomic_fetch_add(&g_n_classic_queues, 1);
    DLOG(1, "classic newCommandQueueWithMaxCommandBufferCount:%lu -> %p (from %s)", (unsigned long)max,
         (void *)q, caller_image(__builtin_return_address(0)));
    class_swizzle_classicq(q);
    return q;
}

static id swz_dev_newmtl4q(id self, SEL _cmd)
{
    IMP orig = ORIG(self, _cmd);
    id q = orig ? ((id (*)(id, SEL))orig)(self, _cmd) : nil;
    atomic_fetch_add(&g_n_mtl4_queues, 1);
    DLOG(1, "newMTL4CommandQueue -> %s %p", q ? class_getName(object_getClass(q)) : "nil", (void *)q);
    class_swizzle_mtl4q(q);
    return q;
}

static id swz_dev_newmtl4q_desc(id self, SEL _cmd, id desc, id *err)
{
    IMP orig = ORIG(self, _cmd);
    id q = orig ? ((id (*)(id, SEL, id, id *))orig)(self, _cmd, desc, err) : nil;
    atomic_fetch_add(&g_n_mtl4_queues, 1);
    DLOG(1, "newMTL4CommandQueueWithDescriptor: -> %s %p", q ? class_getName(object_getClass(q)) : "nil", (void *)q);
    class_swizzle_mtl4q(q);
    return q;
}

static id swz_dev_newresset(id self, SEL _cmd, id desc, id *err)
{
    IMP orig = ORIG(self, _cmd);
    id s = orig ? ((id (*)(id, SEL, id, id *))orig)(self, _cmd, desc, err) : nil;
    DLOG(1, "newResidencySetWithDescriptor: -> %s %p", s ? class_getName(object_getClass(s)) : "nil", (void *)s);
    class_swizzle_resset(s);
    return s;
}

static void swz_resset_add(id self, SEL _cmd, id alloc)
{
    IMP orig = ORIG(self, _cmd);
    atomic_fetch_add(&g_n_resset_add, 1);
    if (is_subst(alloc))
    {
        atomic_fetch_add(&g_n_resset_add_subst, 1);
        DLOG(1, "residency set %p addAllocation: SUBSTITUTED texture %p", (void *)self, (void *)alloc);
    }
    if (orig)
        ((void (*)(id, SEL, id))orig)(self, _cmd, alloc);
}

static void swz_resset_adds(id self, SEL _cmd, const id *allocs, NSUInteger count)
{
    IMP orig = ORIG(self, _cmd);
    NSUInteger i;
    atomic_fetch_add(&g_n_resset_add, count);
    for (i = 0; i < count; i++)
        if (is_subst(allocs[i]))
        {
            atomic_fetch_add(&g_n_resset_add_subst, 1);
            DLOG(1, "residency set %p addAllocations: includes SUBSTITUTED texture %p", (void *)self, (void *)allocs[i]);
        }
    if (orig)
        ((void (*)(id, SEL, const id *, NSUInteger))orig)(self, _cmd, allocs, count);
}

static void swz_resset_remove(id self, SEL _cmd, id alloc)
{
    IMP orig = ORIG(self, _cmd);
    if (is_subst(alloc))
        DLOG(1, "residency set %p removeAllocation: SUBSTITUTED texture %p", (void *)self, (void *)alloc);
    if (orig)
        ((void (*)(id, SEL, id))orig)(self, _cmd, alloc);
}

static void swz_mtl4q_commit(id self, SEL _cmd, const id *cbs, NSUInteger count)
{
    IMP orig = ORIG(self, _cmd);
    uint64_t n = atomic_fetch_add(&g_n_mtl4_commits, 1);
    if (n < 3)
        DLOG(1, "MTL4 queue %p commit:count:%lu (first commits) from %s", (void *)self, (unsigned long)count,
             caller_image(__builtin_return_address(0)));
    if (orig)
        ((void (*)(id, SEL, const id *, NSUInteger))orig)(self, _cmd, cbs, count);
}

static void swz_mtl4q_commit_opts(id self, SEL _cmd, const id *cbs, NSUInteger count, id opts)
{
    IMP orig = ORIG(self, _cmd);
    uint64_t n = atomic_fetch_add(&g_n_mtl4_commits, 1);
    if (n < 3)
        DLOG(1, "MTL4 queue %p commit:count:%lu options: (first commits) from %s", (void *)self,
             (unsigned long)count, caller_image(__builtin_return_address(0)));
    if (orig)
        ((void (*)(id, SEL, const id *, NSUInteger, id))orig)(self, _cmd, cbs, count, opts);
}

static _Atomic uint64_t g_n_classic_cbs;

static void note_classic_cb(id q, const char *how, void *ret)
{
    uint64_t n = atomic_fetch_add(&g_n_classic_cbs, 1);
    if (n < 6 || (trace_level() >= 2 && n % 500 == 0))
        DLOG(1, "classic queue %p %s #%llu from %s", (void *)q, how, (unsigned long long)n,
             caller_image(ret));
}

static id swz_q_cb(id self, SEL _cmd)
{
    IMP orig = ORIG(self, _cmd);
    id cb;
    note_classic_cb(self, "commandBuffer", __builtin_return_address(0));
    cb = orig ? ((id (*)(id, SEL))orig)(self, _cmd) : nil;
    class_swizzle_cb(cb);
    return cb;
}

static id swz_q_cb_unret(id self, SEL _cmd)
{
    IMP orig = ORIG(self, _cmd);
    id cb;
    note_classic_cb(self, "commandBufferWithUnretainedReferences", __builtin_return_address(0));
    cb = orig ? ((id (*)(id, SEL))orig)(self, _cmd) : nil;
    class_swizzle_cb(cb);
    return cb;
}

static id swz_q_cb_desc(id self, SEL _cmd, id desc)
{
    IMP orig = ORIG(self, _cmd);
    id cb;
    note_classic_cb(self, "commandBufferWithDescriptor:", __builtin_return_address(0));
    cb = orig ? ((id (*)(id, SEL, id))orig)(self, _cmd, desc) : nil;
    class_swizzle_cb(cb);
    return cb;
}

/* ---- render-pass / texture-view trace (OXR_DMSUBST_RPTRACE=1) -------------
 * Diagnostic for layered (render-target-array-index) rendering: logs the
 * render pass D3DMetal encodes whenever an attachment is an array or MSAA
 * texture, and the texture views it makes of such textures */

static int rptrace(void)
{
    static int level = -1;
    if (level < 0)
    {
        const char *e = getenv("OXR_DMSUBST_RPTRACE");
        level = e && *e ? atoi(e) : 0;
    }
    return level;
}

static int tex_interesting(id<MTLTexture> t)
{
    return t && (t.textureType == MTLTextureType2DArray || t.textureType == MTLTextureType2DMultisampleArray ||
                 t.textureType == MTLTextureType2DMultisample || t.arrayLength > 1 ||
                 (t.parentTexture && t.parentTexture.arrayLength > 1));
}

static const char *tex_str(id<MTLTexture> t, char *b, size_t n)
{
    if (!t) { snprintf(b, n, "nil"); return b; }
    snprintf(b, n, "%p type=%lu arr=%lu samples=%lu fmt=%lu %lux%lu%s", (void *)t, (unsigned long)t.textureType,
             (unsigned long)t.arrayLength, (unsigned long)t.sampleCount, (unsigned long)t.pixelFormat,
             (unsigned long)t.width, (unsigned long)t.height, t.parentTexture ? " (view)" : "");
    return b;
}

static void log_rpd(MTLRenderPassDescriptor *d, const char *how)
{
    static _Atomic unsigned n;
    char b[160], r[160];
    int i, any = 0;
    if (!d || atomic_load(&n) > (unsigned)(rptrace() > 1 ? 100000 : 400))
        return;
    for (i = 0; i < 8; i++)
        any |= tex_interesting(d.colorAttachments[i].texture);
    any |= tex_interesting(d.depthAttachment.texture) | tex_interesting(d.stencilAttachment.texture);
    if (!any)
        return;
    atomic_fetch_add(&n, 1);
    fprintf(stderr, "[dmsubst rp] from %s: renderTargetArrayLength=%lu rtSize=%lux%lu defaultRasterSampleCount=%lu\n", how,
            (unsigned long)d.renderTargetArrayLength, (unsigned long)d.renderTargetWidth,
            (unsigned long)d.renderTargetHeight, (unsigned long)d.defaultRasterSampleCount);
    for (i = 0; i < 8; i++)
    {
        MTLRenderPassColorAttachmentDescriptor *c = d.colorAttachments[i];
        if (!c.texture)
            continue;
        fprintf(stderr, "[dmsubst rp]   color%d %s slice=%lu load=%lu store=%lu resolve=%s rslice=%lu\n", i,
                tex_str(c.texture, b, sizeof(b)), (unsigned long)c.slice, (unsigned long)c.loadAction,
                (unsigned long)c.storeAction, tex_str(c.resolveTexture, r, sizeof(r)), (unsigned long)c.resolveSlice);
    }
    if (d.depthAttachment.texture)
        fprintf(stderr, "[dmsubst rp]   depth %s slice=%lu load=%lu store=%lu\n",
                tex_str(d.depthAttachment.texture, b, sizeof(b)), (unsigned long)d.depthAttachment.slice,
                (unsigned long)d.depthAttachment.loadAction, (unsigned long)d.depthAttachment.storeAction);
    if (d.stencilAttachment.texture)
        fprintf(stderr, "[dmsubst rp]   stencil %s slice=%lu\n", tex_str(d.stencilAttachment.texture, b, sizeof(b)),
                (unsigned long)d.stencilAttachment.slice);
}

/* ---- D3DMetal layered-MSAA-depth workaround --------------------------------
 * D3DMetal 4.0b1 encodes a pass whose depth/stencil attachment is a
 * 2DMultisampleArray (a D3D11 TEXTURE2DMSARRAY DSV) with renderTargetArrayLength
 * = 1, even when every attachment is a full 2-layer array bound at slice 0 (the
 * same pass without the MSAA depth, or with a non-MSAA depth array, gets 2).
 * On macOS a layer count of 1 disables layered rendering, so the
 * SV_RenderTargetArrayIndex a vertex/geometry shader writes is ignored and all
 * instances land in layer 0, and the deferred clear (load action) never reaches
 * layer 1. That is exactly Unity's single-pass-instanced stereo with MSAA:
 * both eyes superimposed in slice 0, slice 1 never written.
 *
 * Fix: for passes D3DMetal itself encodes, when the depth or stencil
 * attachment is a 2DMultisampleArray and every attachment (and resolve
 * target) is an array texture bound at slice 0, set renderTargetArrayLength to
 * the smallest layer count. Only ever raises a count of 0/1.
 * OXR_DMSUBST_LAYER_FIX=0 disables it. */

static int layer_fix_enabled(void)
{
    static int on = -1;
    if (on < 0)
    {
        const char *e = getenv("OXR_DMSUBST_LAYER_FIX");
        on = e && *e ? atoi(e) : 1;
    }
    return on;
}

/* Is the call site in D3DMetal? A handful of call sites, so cache them */
static int caller_is_d3dmetal(void *ret)
{
    static struct { _Atomic(void *) addr; _Atomic int yes; } cache[16];
    static _Atomic unsigned next;
    unsigned i;
    Dl_info info;
    int yes;

    for (i = 0; i < 16; i++)
        if (atomic_load(&cache[i].addr) == ret)
            return atomic_load(&cache[i].yes);
    yes = dladdr(ret, &info) && info.dli_fname && strstr(info.dli_fname, "D3DMetal");
    i = atomic_fetch_add(&next, 1) % 16;
    atomic_store(&cache[i].yes, yes);
    atomic_store(&cache[i].addr, ret);
    return yes;
}

static int layers_at_slice0(id<MTLTexture> t, NSUInteger slice, NSUInteger *min_layers)
{
    if (!t)
        return 1;
    if ((t.textureType != MTLTextureType2DArray && t.textureType != MTLTextureType2DMultisampleArray) || slice)
        return 0;
    if (t.arrayLength < *min_layers)
        *min_layers = t.arrayLength;
    return 1;
}

static _Atomic uint64_t g_n_layer_fixes;

static void fix_layered_msaa_depth(MTLRenderPassDescriptor *d, void *ret)
{
    id<MTLTexture> dt = d.depthAttachment.texture, st = d.stencilAttachment.texture;
    NSUInteger layers = NSUIntegerMax;
    int i;

    if (!d || d.renderTargetArrayLength > 1 || !layer_fix_enabled())
        return;
    if (!((dt && dt.textureType == MTLTextureType2DMultisampleArray) ||
          (st && st.textureType == MTLTextureType2DMultisampleArray)))
        return;
    for (i = 0; i < 8; i++)
    {
        MTLRenderPassColorAttachmentDescriptor *c = d.colorAttachments[i];
        if (!c.texture)
            continue;
        if (!layers_at_slice0(c.texture, c.slice, &layers) ||
            !layers_at_slice0(c.resolveTexture, c.resolveSlice, &layers))
            return;
    }
    if (!layers_at_slice0(dt, d.depthAttachment.slice, &layers) ||
        !layers_at_slice0(st, d.stencilAttachment.slice, &layers))
        return;
    if (layers < 2 || layers == NSUIntegerMax || !caller_is_d3dmetal(ret))
        return;
    if (atomic_fetch_add(&g_n_layer_fixes, 1) == 0)
        fprintf(stderr, "[dmsubst] D3DMetal layered pass with a multisample-array depth attachment had "
                "renderTargetArrayLength=%lu; setting %lu so SV_RenderTargetArrayIndex works "
                "(OXR_DMSUBST_LAYER_FIX=0 disables)\n", (unsigned long)d.renderTargetArrayLength,
                (unsigned long)layers);
    d.renderTargetArrayLength = layers;
}

static id swz_cb_rce(id self, SEL _cmd, MTLRenderPassDescriptor *d)
{
    IMP orig = ORIG(self, _cmd);
    void *ret = __builtin_return_address(0);
    if (rptrace())
        log_rpd(d, caller_image(ret));
    fix_layered_msaa_depth(d, ret);
    if (rptrace() > 2)
        log_rpd(d, "  after fix");
    return orig ? ((id (*)(id, SEL, id))orig)(self, _cmd, d) : nil;
}

static id swz_cb_prce(id self, SEL _cmd, MTLRenderPassDescriptor *d)
{
    IMP orig = ORIG(self, _cmd);
    void *ret = __builtin_return_address(0);
    if (rptrace())
        log_rpd(d, caller_image(ret));
    fix_layered_msaa_depth(d, ret);
    return orig ? ((id (*)(id, SEL, id))orig)(self, _cmd, d) : nil;
}

static void class_swizzle_cb(id cb)
{
    static const struct job jobs[] = {
        { "renderCommandEncoderWithDescriptor:", (IMP)swz_cb_rce },
        { "parallelRenderCommandEncoderWithDescriptor:", (IMP)swz_cb_prce },
    };
    static _Atomic(Class) memo;
    Class c = cb ? object_getClass(cb) : Nil;
    if (!c || atomic_load(&memo) == c)
        return;
    install_jobs(c, jobs, sizeof(jobs) / sizeof(jobs[0]), "command-buffer");
    atomic_store(&memo, c);
}

static void log_view(id self, id view, MTLPixelFormat pf, MTLTextureType type, NSRange levels, NSRange slices, const char *how)
{
    static _Atomic unsigned n;
    char b[160], v[160];
    if (!tex_interesting(self) || atomic_fetch_add(&n, 1) > (unsigned)(rptrace() > 1 ? 100000 : 400))
        return;
    fprintf(stderr, "[dmsubst view] %s of %s: fmt=%lu type=%lu levels=%lu+%lu slices=%lu+%lu -> %s\n", how,
            tex_str(self, b, sizeof(b)), (unsigned long)pf, (unsigned long)type, (unsigned long)levels.location,
            (unsigned long)levels.length, (unsigned long)slices.location, (unsigned long)slices.length,
            tex_str(view, v, sizeof(v)));
}

static id swz_tex_view4(id self, SEL _cmd, MTLPixelFormat pf, MTLTextureType type, NSRange levels, NSRange slices)
{
    IMP orig = ORIG(self, _cmd);
    id v = orig ? ((id (*)(id, SEL, MTLPixelFormat, MTLTextureType, NSRange, NSRange))orig)(self, _cmd, pf, type, levels, slices) : nil;
    log_view(self, v, pf, type, levels, slices, "view");
    return v;
}

static id swz_tex_view5(id self, SEL _cmd, MTLPixelFormat pf, MTLTextureType type, NSRange levels, NSRange slices,
                        MTLTextureSwizzleChannels sw)
{
    IMP orig = ORIG(self, _cmd);
    id v = orig ? ((id (*)(id, SEL, MTLPixelFormat, MTLTextureType, NSRange, NSRange, MTLTextureSwizzleChannels))orig)(
                      self, _cmd, pf, type, levels, slices, sw) : nil;
    log_view(self, v, pf, type, levels, slices, "view+swizzle");
    return v;
}

static id swz_tex_viewdesc(id self, SEL _cmd, id desc)
{
    IMP orig = ORIG(self, _cmd);
    id v = orig ? ((id (*)(id, SEL, id))orig)(self, _cmd, desc) : nil;
    NSRange lv = [[desc valueForKey:@"levelRange"] rangeValue], sl = [[desc valueForKey:@"sliceRange"] rangeValue];
    log_view(self, v, (MTLPixelFormat)[[desc valueForKey:@"pixelFormat"] unsignedIntegerValue],
             (MTLTextureType)[[desc valueForKey:@"textureType"] unsignedIntegerValue], lv, sl, "viewWithDescriptor");
    return v;
}

static void class_swizzle_texview(id tex)
{
    static const struct job jobs[] = {
        { "newTextureViewWithPixelFormat:textureType:levels:slices:", (IMP)swz_tex_view4 },
        { "newTextureViewWithPixelFormat:textureType:levels:slices:swizzle:", (IMP)swz_tex_view5 },
        { "newTextureViewWithDescriptor:", (IMP)swz_tex_viewdesc },
    };
    static _Atomic(Class) memo;
    Class c = tex ? object_getClass(tex) : Nil;
    if (!rptrace() || !c || atomic_load(&memo) == c)
        return;
    install_jobs(c, jobs, sizeof(jobs) / sizeof(jobs[0]), "texture-view");
    atomic_store(&memo, c);
}

static void class_swizzle_classicq(id q)
{
    static const struct job jobs[] = {
        { "commandBuffer", (IMP)swz_q_cb },
        { "commandBufferWithUnretainedReferences", (IMP)swz_q_cb_unret },
        { "commandBufferWithDescriptor:", (IMP)swz_q_cb_desc },
    };
    static _Atomic(Class) memo;
    Class c = q ? object_getClass(q) : Nil;
    if (!c || atomic_load(&memo) == c)
        return;
    install_jobs(c, jobs, sizeof(jobs) / sizeof(jobs[0]), "classic-queue");
    atomic_store(&memo, c);
}

static void swz_mtl4q_addset(id self, SEL _cmd, id set)
{
    IMP orig = ORIG(self, _cmd);
    DLOG(1, "MTL4 queue %p addResidencySet: %p", (void *)self, (void *)set);
    if (orig)
        ((void (*)(id, SEL, id))orig)(self, _cmd, set);
}

/* ---- hooks on the substituted texture's class --------------------------------
 * The runtime's texture has no heap. d3dmetal-native found D3DMetal 3.0's
 * D3DMTexture::Finalize zero-fills a new Shared-storage texture through
 * [[tex heap] newBufferWithLength:...] and dereferences the result
 * unconditionally, so a heapless texture crashed it. Log every heap-ness query
 * on a texture we handed out; with OXR_DMSUBST_SHADOW_HEAP=1 also hand back a
 * scratch shadow heap (never the texture's real bytes). */

@interface OXRDmsShadowHeap : NSObject
{
@public
    id<MTLBuffer> scratch;
    NSUInteger claim;
}
@end

@implementation OXRDmsShadowHeap
- (void)dealloc { [scratch release]; [super dealloc]; }
- (NSUInteger)size { return claim; }
- (MTLResourceOptions)resourceOptions { return scratch.resourceOptions; }
- (id)newBufferWithLength:(NSUInteger)len options:(MTLResourceOptions)opts offset:(NSUInteger)off
{
    (void)opts; (void)off;
    DLOG(1, "shadow heap: newBufferWithLength:%lu offset:%lu -> scratch", (unsigned long)len, (unsigned long)off);
    return [scratch.device newBufferWithLength:len ? len : 16 options:MTLResourceStorageModeShared];
}
- (id)forwardingTargetForSelector:(SEL)sel
{
    if (scratch && [scratch respondsToSelector:sel])
        return scratch;
    return [super forwardingTargetForSelector:sel];
}
@end

static const void *kShadowKey = &kShadowKey;

static id swz_tex_heap(id self, SEL _cmd)
{
    IMP orig = ORIG(self, _cmd);
    id h = orig ? ((id (*)(id, SEL))orig)(self, _cmd) : nil;
    if (!h && is_parent(self))
    {
        static _Atomic unsigned n;
        if (atomic_fetch_add(&n, 1) < 4)
            DLOG(1, "-heap of the RUNTIME's texture %p (parent of a substituted view) from %s",
                 (void *)self, caller_image(__builtin_return_address(0)));
    }
    if (!h && is_subst(self))
    {
        atomic_fetch_add(&g_n_heap_queries_subst, 1);
        DLOG(1, "-heap of SUBSTITUTED texture %p (none) from %s", (void *)self,
             caller_image(__builtin_return_address(0)));
        if (getenv("OXR_DMSUBST_SHADOW_HEAP") && atoi(getenv("OXR_DMSUBST_SHADOW_HEAP")))
        {
            OXRDmsShadowHeap *sh = objc_getAssociatedObject(self, kShadowKey);
            if (!sh)
            {
                sh = [[OXRDmsShadowHeap alloc] init];
                sh->claim = [(id<MTLTexture>)self allocatedSize];
                sh->scratch = [[(id<MTLTexture>)self device] newBufferWithLength:sh->claim ? sh->claim : 16
                                                                         options:MTLResourceStorageModeShared];
                objc_setAssociatedObject(self, kShadowKey, sh, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
                [sh release];
            }
            return sh;
        }
    }
    return h;
}

static NSUInteger swz_tex_heapoffset(id self, SEL _cmd)
{
    IMP orig = ORIG(self, _cmd);
    if (is_subst(self))
    {
        atomic_fetch_add(&g_n_heap_queries_subst, 1);
        DLOG(1, "-heapOffset of SUBSTITUTED texture %p from %s", (void *)self,
             caller_image(__builtin_return_address(0)));
    }
    return orig ? ((NSUInteger (*)(id, SEL))orig)(self, _cmd) : 0;
}

static void swz_tex_makealiasable(id self, SEL _cmd)
{
    IMP orig = ORIG(self, _cmd);
    if (is_subst(self))
    {
        DLOG(1, "D3DMetal called -makeAliasable on SUBSTITUTED texture %p (ignored)", (void *)self);
        return;
    }
    if (orig)
        ((void (*)(id, SEL))orig)(self, _cmd);
}

static void class_swizzle_texture(id tex)
{
    static const struct job jobs[] = {
        { "heap", (IMP)swz_tex_heap },
        { "heapOffset", (IMP)swz_tex_heapoffset },
        { "makeAliasable", (IMP)swz_tex_makealiasable },
    };
    install_jobs(object_getClass(tex), jobs, sizeof(jobs) / sizeof(jobs[0]), "texture");
}

static void class_swizzle_heap(id heap)
{
    static const struct job jobs[] = {
        { "newTextureWithDescriptor:", (IMP)swz_heap_newtex },
        { "newTextureWithDescriptor:offset:", (IMP)swz_heap_newtex_off },
    };
    static _Atomic(Class) memo;
    Class c = heap ? object_getClass(heap) : Nil;
    if (!c || atomic_load(&memo) == c)
        return;
    install_jobs(c, jobs, sizeof(jobs) / sizeof(jobs[0]), "heap");
    atomic_store(&memo, c);
}

static void class_swizzle_buffer(id buf)
{
    static const struct job jobs[] = {
        { "newTextureWithDescriptor:offset:bytesPerRow:", (IMP)swz_buf_newtex },
    };
    static _Atomic(Class) memo;
    Class c = buf ? object_getClass(buf) : Nil;
    if (!c || atomic_load(&memo) == c)
        return;
    install_jobs(c, jobs, sizeof(jobs) / sizeof(jobs[0]), "buffer");
    atomic_store(&memo, c);
}

static void class_swizzle_resset(id set)
{
    static const struct job jobs[] = {
        { "addAllocation:", (IMP)swz_resset_add },
        { "addAllocations:count:", (IMP)swz_resset_adds },
        { "removeAllocation:", (IMP)swz_resset_remove },
    };
    static _Atomic(Class) memo;
    Class c = set ? object_getClass(set) : Nil;
    if (!c || atomic_load(&memo) == c)
        return;
    install_jobs(c, jobs, sizeof(jobs) / sizeof(jobs[0]), "residency-set");
    atomic_store(&memo, c);
}

static void class_swizzle_mtl4q(id q)
{
    static const struct job jobs[] = {
        { "commit:count:", (IMP)swz_mtl4q_commit },
        { "commit:count:options:", (IMP)swz_mtl4q_commit_opts },
        { "addResidencySet:", (IMP)swz_mtl4q_addset },
    };
    static _Atomic(Class) memo;
    Class c = q ? object_getClass(q) : Nil;
    if (!c || atomic_load(&memo) == c)
        return;
    install_jobs(c, jobs, sizeof(jobs) / sizeof(jobs[0]), "mtl4-queue");
    atomic_store(&memo, c);
}

static void swizzle_device_class(Class cls)
{
    static const struct job jobs[] = {
        { "newTextureWithDescriptor:", (IMP)swz_dev_newtex },
        { "newSharedTextureWithDescriptor:", (IMP)swz_dev_newsharedtex },
        { "newTextureWithDescriptor:iosurface:plane:", (IMP)swz_dev_newtex_ios },
        { "newHeapWithDescriptor:", (IMP)swz_dev_newheap },
        { "newBufferWithLength:options:", (IMP)swz_dev_newbuf },
        { "newCommandQueue", (IMP)swz_dev_newq },
        { "newCommandQueueWithMaxCommandBufferCount:", (IMP)swz_dev_newqmax },
        { "newMTL4CommandQueue", (IMP)swz_dev_newmtl4q },
        { "newMTL4CommandQueueWithDescriptor:error:", (IMP)swz_dev_newmtl4q_desc },
        { "newResidencySetWithDescriptor:error:", (IMP)swz_dev_newresset },
        { "newSharedEvent", (IMP)swz_dev_newsharedevent },
        { "newSharedEventWithOptions:", (IMP)swz_dev_newsharedevent_opts },
        { "newEvent", (IMP)swz_dev_newevent },
    };
    install_jobs(cls, jobs, sizeof(jobs) / sizeof(jobs[0]), "device");
}

/* Create one of each object kind ourselves so the concrete heap / buffer /
 * residency-set / MTL4-queue classes get swizzled even if D3DMetal made its
 * first ones before we were installed. */
static void prime_classes(id<MTLDevice> dev)
{
    MTLHeapDescriptor *hd = [[MTLHeapDescriptor alloc] init];
    id heap, buf;
    hd.size = 64 * 1024;
    hd.storageMode = MTLStorageModePrivate;
    heap = [dev newHeapWithDescriptor:hd];
    [heap release];
    hd.type = MTLHeapTypePlacement;
    heap = [dev newHeapWithDescriptor:hd];
    [heap release];
    [hd release];
    buf = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    [buf release];
    {
        /* our own queue + command buffer: swizzles the concrete classes
         * D3DMetal's (possibly already existing) queues hand out */
        id<MTLCommandQueue> q = [dev newCommandQueue];
        atomic_fetch_sub(&g_n_classic_queues, 1);
        @autoreleasepool {
            id cb = [q commandBuffer];
            id cbu = [q commandBufferWithUnretainedReferences];
            class_swizzle_cb(cb);
            class_swizzle_cb(cbu);
        }
        [q release];
    }
    if ([dev respondsToSelector:@selector(newResidencySetWithDescriptor:error:)])
    {
        MTLResidencySetDescriptor *rd = [[MTLResidencySetDescriptor alloc] init];
        id set = [dev newResidencySetWithDescriptor:rd error:nil];
        [set release];
        [rd release];
    }
    if ([dev respondsToSelector:sel_registerName("newMTL4CommandQueue")])
    {
        id q = ((id (*)(id, SEL))objc_msgSend)(dev, sel_registerName("newMTL4CommandQueue"));
        atomic_fetch_sub(&g_n_mtl4_queues, 1); /* ours, not D3DMetal's */
        [q release];
    }
}

static int g_installed;

static void install(void)
{
    @autoreleasepool {
        NSArray<id<MTLDevice>> *all;
        id<MTLDevice> def;
        Class seen[8];
        unsigned nseen = 0, i;

        pthread_mutex_lock(&g_install_lock);
        if (g_installed)
        {
            pthread_mutex_unlock(&g_install_lock);
            return;
        }
        g_installed = 1;
        pthread_mutex_unlock(&g_install_lock);

        all = MTLCopyAllDevices();
        def = MTLCreateSystemDefaultDevice();
        for (id<MTLDevice> d in all)
        {
            Class c = object_getClass(d);
            for (i = 0; i < nseen && seen[i] != c; i++)
                ;
            if (i == nseen && nseen < 8)
                seen[nseen++] = c;
        }
        if (def)
        {
            Class c = object_getClass(def);
            for (i = 0; i < nseen && seen[i] != c; i++)
                ;
            if (i == nseen && nseen < 8)
                seen[nseen++] = c;
        }
        for (i = 0; i < nseen; i++)
            swizzle_device_class(seen[i]);
        if (def)
            prime_classes(def);
        DLOG(1, "installed on %u device class(es); default device %p (%s)", nseen, (void *)def,
             def ? def.name.UTF8String : "none");
        [def release];
        [all release];
    }
}

/* ---- readback ------------------------------------------------------------- */

static float half_to_float(uint16_t h)
{
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1f, m = h & 0x3ff, f;
    float out;
    if (e == 0)
        f = (s << 31) | (m ? 0 : 0); /* flush denormals: good enough for a probe */
    else if (e == 31)
        f = (s << 31) | 0x7f800000 | (m << 13);
    else
        f = (s << 31) | ((e + 112) << 23) | (m << 13);
    memcpy(&out, &f, 4);
    return out;
}

static int readback(struct dmsubst_params *p)
{
    @autoreleasepool {
        id<MTLTexture> tex = (id<MTLTexture>)(void *)(uintptr_t)p->mtl_texture;
        id<MTLDevice> dev;
        id<MTLCommandQueue> q;
        id<MTLCommandBuffer> cb;
        id<MTLBlitCommandEncoder> blit;
        id<MTLBuffer> buf;
        MTLPixelFormat pf;
        const uint8_t *b;
        int i;

        if (!tex)
            return -1;
        dev = tex.device;
        pf = tex.pixelFormat;
        p->tex_pixel_format = (uint32_t)pf;
        buf = [dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
        q = [dev newCommandQueue];
        cb = [q commandBuffer];
        if (p->event)
            [cb encodeWaitForEvent:(id<MTLEvent>)(void *)(uintptr_t)p->event value:p->event_value];
        blit = [cb blitCommandEncoder];
        [blit copyFromTexture:tex
                  sourceSlice:p->slice
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(p->x, p->y, 0)
                   sourceSize:MTLSizeMake(1, 1, 1)
                     toBuffer:buf
            destinationOffset:0
       destinationBytesPerRow:256
     destinationBytesPerImage:256];
        [blit endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        if (cb.status != MTLCommandBufferStatusCompleted)
        {
            DERR("readback command buffer failed: %s", cb.error.description.UTF8String);
            [q release];
            [buf release];
            return -2;
        }
        b = buf.contents;
        memcpy(p->raw, b, 16);
        switch (pf)
        {
        case MTLPixelFormatRGBA8Unorm: case MTLPixelFormatRGBA8Unorm_sRGB:
            for (i = 0; i < 4; i++) p->rgba[i] = b[i] / 255.0f;
            break;
        case MTLPixelFormatBGRA8Unorm: case MTLPixelFormatBGRA8Unorm_sRGB:
            p->rgba[0] = b[2] / 255.0f; p->rgba[1] = b[1] / 255.0f;
            p->rgba[2] = b[0] / 255.0f; p->rgba[3] = b[3] / 255.0f;
            break;
        case MTLPixelFormatRGBA16Float:
            for (i = 0; i < 4; i++) p->rgba[i] = half_to_float(((const uint16_t *)b)[i]);
            break;
        case MTLPixelFormatRGBA32Float:
            memcpy(p->rgba, b, 16);
            break;
        case MTLPixelFormatDepth32Float:
            memcpy(&p->rgba[0], b, 4);
            p->rgba[1] = p->rgba[2] = p->rgba[3] = 0.0f;
            break;
        default:
            for (i = 0; i < 4; i++) p->rgba[i] = -1.0f;
            break;
        }
        [q release];
        [buf release];
        return 0;
    }
}

static int dump_ppm(struct dmsubst_params *p)
{
    @autoreleasepool {
        id<MTLTexture> tex = (id<MTLTexture>)(void *)(uintptr_t)p->mtl_texture;
        NSUInteger w, h, bpr, x, y;
        id<MTLBuffer> buf;
        id<MTLCommandQueue> q;
        id<MTLCommandBuffer> cb;
        id<MTLBlitCommandEncoder> blit;
        const uint8_t *px;
        int bgra;
        FILE *f;
        double sum[3] = { 0, 0, 0 };

        if (!tex)
            return -1;
        switch (tex.pixelFormat)
        {
        case MTLPixelFormatBGRA8Unorm: case MTLPixelFormatBGRA8Unorm_sRGB: bgra = 1; break;
        case MTLPixelFormatRGBA8Unorm: case MTLPixelFormatRGBA8Unorm_sRGB: bgra = 0; break;
        default: DERR("dump: unsupported pixel format %lu", (unsigned long)tex.pixelFormat); return -2;
        }
        w = tex.width; h = tex.height; bpr = w * 4;
        buf = [tex.device newBufferWithLength:bpr * h options:MTLResourceStorageModeShared];
        q = [tex.device newCommandQueue];
        cb = [q commandBuffer];
        if (p->event)
            [cb encodeWaitForEvent:(id<MTLEvent>)(void *)(uintptr_t)p->event value:p->event_value];
        blit = [cb blitCommandEncoder];
        [blit copyFromTexture:tex sourceSlice:p->slice sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(w, h, 1) toBuffer:buf destinationOffset:0
       destinationBytesPerRow:bpr destinationBytesPerImage:bpr * h];
        [blit endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        px = buf.contents;
        f = fopen(p->where, "wb");
        if (!f)
        {
            [q release]; [buf release];
            return -3;
        }
        fprintf(f, "P6\n%lu %lu\n255\n", (unsigned long)w, (unsigned long)h);
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
            {
                const uint8_t *s = px + y * bpr + x * 4;
                uint8_t rgb[3] = { bgra ? s[2] : s[0], s[1], bgra ? s[0] : s[2] };
                fwrite(rgb, 1, 3, f);
                sum[0] += rgb[0]; sum[1] += rgb[1]; sum[2] += rgb[2];
            }
        fclose(f);
        oxr_bridge_log("dumped %s (%lux%lu slice %u fmt %lu), mean rgb %.1f %.1f %.1f", p->where, (unsigned long)w,
             (unsigned long)h, p->slice, (unsigned long)tex.pixelFormat, sum[0] / (w * h), sum[1] / (w * h),
             sum[2] / (w * h));
        [q release];
        [buf release];
        return 0;
    }
}

/* ---- entry point ------------------------------------------------------------ */

static uint32_t detect(void)
{
    uint32_t flags = 0, n = _dyld_image_count(), i;
    for (i = 0; i < n; i++)
    {
        const char *name = _dyld_get_image_name(i);
        if (!name)
            continue;
        if (strstr(name, "D3DMetal.framework/"))
            flags |= DMSUBST_DETECT_D3DMETAL;
        if (strstr(name, "/winemetal.so"))
            flags |= DMSUBST_DETECT_DXMT;
    }
    if (atomic_load(&g_n_mtl4_queues))
        flags |= DMSUBST_DETECT_MTL4;
    return flags;
}

int dmsubst_process_has_d3dmetal(void)
{
    return !!(detect() & DMSUBST_DETECT_D3DMETAL);
}

/* Debug knobs for games launched from Steam: a running Steam client does not
 * pick up new cxbottle.conf [EnvironmentVariables] until it is restarted, and
 * Windows Steam's launch options cannot set variables. So KEY=VALUE lines in
 * ~/Library/Application Support/OXRSys/wineopenxr.env (or $OXR_BRIDGE_ENV_FILE)
 * are applied at load, without overriding anything already set. The PE half
 * reads them through DMSUBST_OP_GETENV (its CRT has its own environment) */
static void load_env_file(void)
{
    const char *file = getenv("OXR_BRIDGE_ENV_FILE"), *home = getenv("HOME");
    char path[1024], line[512];
    FILE *f;
    int n = 0;

    if (file && *file)
        snprintf(path, sizeof(path), "%s", file);
    else if (home)
        snprintf(path, sizeof(path), "%s/Library/Application Support/OXRSys/wineopenxr.env", home);
    else
        return;
    if (!(f = fopen(path, "r")))
        return;
    while (fgets(line, sizeof(line), f))
    {
        char *k = line, *eq, *end;
        while (*k == ' ' || *k == '\t') k++;
        if (*k == '#' || !(eq = strchr(k, '=')))
            continue;
        *eq = 0;
        for (end = eq + 1 + strlen(eq + 1); end > eq + 1 && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' '); )
            *--end = 0;
        for (end = eq; end > k && (end[-1] == ' ' || end[-1] == '\t'); )
            *--end = 0;
        if (*k && !getenv(k))
        {
            setenv(k, eq + 1, 0);
            n++;
        }
    }
    fclose(f);
    if (n)
        oxr_bridge_log("applied %d setting(s) from %s", n, path);
}

/* Runs when Wine dlopens this .so, i.e. when wineopenxr.dll is loaded (usually
 * the game's first OpenXR call, before it creates its D3D11 device). */
__attribute__((constructor)) static void dmsubst_early_init(void)
{
    load_env_file();
    const char *e = getenv("OXR_DMSUBST_EARLY");
    if (e && atoi(e))
        install();
}

NTSTATUS wine_dmsubst(void *args)
{
    struct dmsubst_params *p = args;

    p->status = 0;
    p->thread_id = tid();
    switch (p->op)
    {
    case DMSUBST_OP_DETECT:
        p->detect_flags = detect();
        break;
    case DMSUBST_OP_INSTALL:
        install();
        p->detect_flags = detect();
        break;
    case DMSUBST_OP_ARM:
        if (t_arm.armed)
            DERR("re-armed while armed");
        memset(&t_arm, 0, sizeof(t_arm));
        t_arm.armed = 1;
        t_arm.probe = !!(p->flags & DMSUBST_ARM_PROBE);
        t_arm.want_event = !!(p->flags & (DMSUBST_ARM_EVENT | DMSUBST_ARM_EVENT_SHARED));
        t_arm.event_shared = !!(p->flags & DMSUBST_ARM_EVENT_SHARED);
        t_arm.capture = !!(p->flags & DMSUBST_ARM_CAPTURE);
        t_arm.tex = (id<MTLTexture>)(void *)(uintptr_t)p->mtl_texture;
        t_arm.global_at_arm = atomic_load(&g_n_textures);
        DLOG(1, "armed%s for runtime texture %p", t_arm.probe ? " (probe)" : "", (void *)t_arm.tex);
        break;
    case DMSUBST_OP_DISARM:
        p->captured = t_arm.captured;
        p->seen = t_arm.seen;
        p->global_seen = (uint32_t)(atomic_load(&g_n_textures) - t_arm.global_at_arm);
        p->result_is_view = t_arm.is_view;
        p->desc_pixel_format = (uint32_t)t_arm.pf;
        p->desc_texture_type = (uint32_t)t_arm.type;
        p->desc_width = (uint32_t)t_arm.w;
        p->desc_height = (uint32_t)t_arm.h;
        p->desc_array_length = (uint32_t)t_arm.arr;
        p->desc_mips = (uint32_t)t_arm.mips;
        p->desc_usage = t_arm.usage;
        p->desc_storage_mode = (uint32_t)t_arm.storage;
        p->same_device = t_arm.same_device;
        memcpy(p->where, t_arm.where, sizeof(p->where));
        p->event = (uint64_t)(uintptr_t)t_arm.event;
        p->created_texture = (uint64_t)(uintptr_t)t_arm.created;
        if (t_arm.want_event)
            p->seen = t_arm.events_seen;
        if (t_arm.armed && !t_arm.probe && t_arm.tex && !t_arm.captured)
            DERR("armed create reached none of the hooked Metal entry points on this thread "
                 "(%u creations on other threads meanwhile)", p->global_seen - p->seen);
        DLOG(1, "disarmed: captured=%d seen=%u global=%u", t_arm.captured, t_arm.seen, p->global_seen);
        memset(&t_arm, 0, sizeof(t_arm));
        break;
    case DMSUBST_OP_READBACK:
        p->status = readback(p);
        break;
    case DMSUBST_OP_THREAD:
        break;
    case DMSUBST_OP_DUMP:
        p->status = dump_ppm(p);
        break;
    case DMSUBST_OP_EVENT_VALUE:
        p->event_value = p->mtl_texture ? [(id<MTLSharedEvent>)(void *)(uintptr_t)p->mtl_texture signaledValue] : 0;
        break;
    case DMSUBST_OP_LOG:
        if (p->text)
            oxr_bridge_log("%s", (const char *)(uintptr_t)p->text);
        break;
    case DMSUBST_OP_GETENV:
    {
        const char *v;
        p->where[sizeof(p->where) - 1] = 0;
        v = getenv(p->where);
        if (!v)
            p->status = -1;
        else
            snprintf(p->where, sizeof(p->where), "%s", v);
        break;
    }
    case DMSUBST_OP_STATS:
        p->n_textures = atomic_load(&g_n_textures);
        p->n_heaps = atomic_load(&g_n_heaps);
        p->n_resset_add = atomic_load(&g_n_resset_add);
        p->n_resset_add_subst = atomic_load(&g_n_resset_add_subst);
        p->n_heap_queries_subst = atomic_load(&g_n_heap_queries_subst);
        p->n_mtl4_queues = atomic_load(&g_n_mtl4_queues);
        p->n_classic_queues = atomic_load(&g_n_classic_queues);
        p->n_mtl4_commits = atomic_load(&g_n_mtl4_commits);
        p->n_classic_cbs = atomic_load(&g_n_classic_cbs);
        p->detect_flags = detect();
        break;
    default:
        p->status = -1;
    }
    return STATUS_SUCCESS;
}
