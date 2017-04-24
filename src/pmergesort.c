/* -------------------------------------------------------------------------------------------------------------------------- */
/*  pmergesort.c                                                                                                              */
/* -------------------------------------------------------------------------------------------------------------------------- */
/*  Created by Cyril Murzin                                                                                                   */
/*  Copyright (c) 2015-2017 Ravel Developers Group. All rights reserved.                                                      */
/* -------------------------------------------------------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------------------------------------------------------- */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "pmergesort.h"

/* -------------------------------------------------------------------------------------------------------------------------- */
/* configure build                                                                                                            */
/* -------------------------------------------------------------------------------------------------------------------------- */

#ifndef CFG_PARALLEL
#define CFG_PARALLEL                1   /* enable build parallel merge sort algorithms */
#endif

#ifndef CFG_PARALLEL_USE_GCD
#define CFG_PARALLEL_USE_GCD        1   /* enable use of GCD for multi-threading */
#endif

#ifndef CFG_PARALLEL_USE_PTHREADS
#define CFG_PARALLEL_USE_PTHREADS   0   /* enable use of pthreads based pool for multi-threading */
#endif

#ifndef CFG_RAW_ACCESS
#define CFG_RAW_ACCESS              1   /* enable raw memory access, 0 implies the using of memmove & memcpy */
#endif

#ifndef CFG_AGNER_ACCESS
#define CFG_AGNER_ACCESS            0   /* enable Agner Fog asmlib memory access, http://www.agner.org/optimize/ */
#endif

#ifndef CFG_CORE_PROFILE
#define CFG_CORE_PROFILE            0
#endif

/* -------------------------------------------------------------------------------------------------------------------------- */
/* parallel fine tunings                                                                                                      */
/* -------------------------------------------------------------------------------------------------------------------------- */

#if CFG_PARALLEL

#if !CFG_PARALLEL_USE_PTHREADS && CFG_PARALLEL_USE_GCD
/*  GCD  */
#   if !defined(MAC_OS_X_VERSION_10_6) || MAC_OS_X_VERSION_MIN_REQUIRED < MAC_OS_X_VERSION_10_6
#   error define CFG_PARALLEL_USE_PTHREADS to use parallel sort with pre-Mac OS X 10.6
#   endif
#elif CFG_PARALLEL_USE_PTHREADS && !CFG_PARALLEL_USE_GCD
/*  pthreads  */
#else
#   error to use parallel sort either CFG_PARALLEL_USE_PTHREADS or CFG_PARALLEL_USE_GCD should be defined
#endif

#else

#   undef  CFG_PARALLEL_USE_GCD
#   define CFG_PARALLEL_USE_GCD        0
#   undef  CFG_PARALLEL_USE_PTHREADS
#   define CFG_PARALLEL_USE_PTHREADS   0

#endif

/* -------------------------------------------------------------------------------------------------------------------------- */
/* fine tunings                                                                                                               */
/* -------------------------------------------------------------------------------------------------------------------------- */

#define _CFG_QUEUE_OVERCOMMIT       1   /* use private GCD queue attribute to force number of threads,
                                            see Apple Co. CoreFoundation source */

#define _CFG_PRESORT                binsort_run     /* method of pre-sort for initial subsegments,
                                                        allowed: binsort, binsort_run, and binsort_mergerun */

#define _CFG_USE_4_MEM              1 && CFG_RAW_ACCESS /* use dedicated int32 type memory ops */
#define _CFG_USE_8_MEM              1 && CFG_RAW_ACCESS /* use dedicated int64 type memory ops */
#define _CFG_USE_16_MEM             1 && CFG_RAW_ACCESS /* use dedicated int128 type memory ops */

#define _CFG_TMP_ROT                8   /* max. temp. elements at stack on rotate */

#define _CFG_MIN_SUBMERGELEN        16  /* threshold to fallback from inplace symmerge to inplace merge */
#define _CFG_MIN_SUBMERGELEN1       8   /* threshold to fallback from inplace symmerge to inplace merge
                                            for short left/right segment */
#define _CFG_MIN_SUBMERGELEN2       4   /* threshold to fallback from binary to linear search
                                            for short left/right segment merging */

#define _CFG_BLOCKLEN_MTHRESHOLD    4
#define _CFG_BLOCKLEN_SYMMERGE      32  /* 20 was as in built-in GO language function */
#define _CFG_BLOCKLEN_MERGE         32

/* -------------------------------------------------------------------------------------------------------------------------- */

typedef struct thr_pool thr_pool_t;

#if CFG_PARALLEL
#if 1
#include <sys/sysctl.h>
#else
#include <unistd.h>
#endif

static int32_t _ncpu = -1;

#if CFG_PARALLEL_USE_PTHREADS
#define _CFG_ONCE_ARG   void
#elif CFG_PARALLEL_USE_GCD
#define _CFG_ONCE_ARG    void * ctx
#endif

static void __numCPU_initialize(_CFG_ONCE_ARG)
{
#if 1
    int32_t mib[] = { CTL_HW, HW_AVAILCPU };

    size_t sz = sizeof(_ncpu);
    if (sysctl(mib, sizeof(mib) / sizeof(mib[0]), &_ncpu, &sz, NULL, 0) != 0)
        _ncpu = 1;
    else if (_ncpu <= 0)
        _ncpu = 1;
#else
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu <= 0)
        _ncpu = 1;
    else
        _ncpu = (int32_t)ncpu;
#endif
}

/* -------------------------------------------------------------------------------------------------------------------------- */
#if CFG_PARALLEL_USE_PTHREADS
/* -------------------------------------------------------------------------------------------------------------------------- */
#include "pmergesort-tpool.inl"
/* -------------------------------------------------------------------------------------------------------------------------- */

static __attribute__((noinline)) int numCPU()
{
static pthread_once_t _once = PTHREAD_ONCE_INIT;

    if (_ncpu <= 0)
        pthread_once(&_once, __numCPU_initialize);

    return (int)_ncpu;
}

/* -------------------------------------------------------------------------------------------------------------------------- */

static pthread_key_t _sKey = 0;

static __attribute__((noinline)) void __thPool_finalize(void * value)
{
    thr_pool_destroy((thr_pool_t *)value);
}

static __attribute__((noinline)) void __thPoolKey_initialize()
{
    pthread_key_create(&_sKey, __thPool_finalize);
}

#if 0
static __attribute__((destructor)) void __thPoolKey_finalize()
{
    if (_sKey != 0)
        pthread_key_delete(_sKey);
}
#endif

static thr_pool_t * thPool()
{
static pthread_once_t _once = PTHREAD_ONCE_INIT;

    if (_sKey == 0)
        pthread_once(&_once, __thPoolKey_initialize);

    thr_pool_t * pool = (thr_pool_t *)pthread_getspecific(_sKey);
    if (pool == NULL)
    {
        /*
         *  we have to create pool with some limits on simultaneously running
         *  threads. presuambly, for better performance, there shouldn't be
         *  more threads than number of CPU cores.
         */
        pool = thr_pool_create(numCPU() / 2, numCPU(), 1, NULL);

        pthread_setspecific(_sKey, pool);
    }

    return pool;
}

/* -------------------------------------------------------------------------------------------------------------------------- */
#elif CFG_PARALLEL_USE_GCD
/* -------------------------------------------------------------------------------------------------------------------------- */
#include <dispatch/dispatch.h>
/* -------------------------------------------------------------------------------------------------------------------------- */

#if _CFG_QUEUE_OVERCOMMIT
/*!
 * @enum dispatch_queue_flags_t
 *
 * @constant DISPATCH_QUEUE_OVERCOMMIT
 * The queue will create a new thread for invoking blocks, regardless of how
 * busy the computer is.
 */
enum
{
    DISPATCH_QUEUE_OVERCOMMIT = 0x2ULL
};

#define _CFG_DISPATCH_QUEUE_FLAGS   DISPATCH_QUEUE_OVERCOMMIT
#else
#define _CFG_DISPATCH_QUEUE_FLAGS   0
#endif
/* -------------------------------------------------------------------------------------------------------------------------- */

static __attribute__((noinline)) int numCPU()
{
static dispatch_once_t _once;

    if (_ncpu <= 0)
        dispatch_once_f(&_once, NULL, __numCPU_initialize);

    return (int)_ncpu;
}

/* -------------------------------------------------------------------------------------------------------------------------- */
#define thPool()    ((thr_pool_t *)0)
/* -------------------------------------------------------------------------------------------------------------------------- */
#endif /* CFG_PARALLEL_USE_PTHREADS */
/* -------------------------------------------------------------------------------------------------------------------------- */
#else
#define numCPU()    (0)
#define thPool()    ((thr_pool_t *)0)
/* -------------------------------------------------------------------------------------------------------------------------- */
#endif /* CFG_PARALLEL */

/* -------------------------------------------------------------------------------------------------------------------------- */

typedef int (*cmpv_t)(const void *, const void *);
typedef int (*cmpr_t)(void *, const void *, const void *);

typedef int (*sort_t)(void * base, size_t n, size_t sz, cmpv_t cmp);
typedef int (*sort_r_t)(void * base, size_t n, size_t sz, void * thunk, cmpr_t cmp);

struct _context;
struct _aux
{
    int             rc;         /* result code of effector operation */

    size_t          sz;         /* size of temp. buffer */
    void *          temp;       /* temp. buffer storage */
};
typedef struct _aux aux_t;

typedef void (*effector_t)(void * lo, void * mi, void * hi, struct _context * ctx, aux_t * aux);

/* -------------------------------------------------------------------------------------------------------------------------- */

struct _context
{
    /* sort context */

    const void *    base;           /* base address of array to sort    */
    const size_t    n;              /* number of elements               */
    const size_t    sz;             /* size of element                  */

    const void *    cmp;            /* comparator function              */
    const void *    thunk;          /* comparator thunk                 */

    /* parallel */

    const int       ncpu;           /* number of CPU cores              */
    thr_pool_t *    thpool;         /* thread pool (for pthread model)  */

    /* symmerge parallel */

    size_t          npercpu;        /* number of elements per CPU       */
    size_t          bsize;          /* initial block size               */
    effector_t      sort_effector;  /* effector to pre-sort blocks      */
    effector_t      merge_effector; /* effector to merge blocks         */

    /* symmerge parallel wrapper */

    const void *    wsort;          /* sort function to wrap            */
};
typedef struct _context context_t;

#if CFG_PARALLEL
struct _pmergesort_pass_context
{
    context_t *     ctx;

    size_t          bsz;
    size_t          dbl_bsz;

    size_t          chunksz;
    size_t          numchunks;
#if CFG_PARALLEL_USE_PTHREADS
    size_t          chunk;      /* index of chunk (for pthread model)   */
#endif

    void *          lo;
    void *          hi;

    aux_t *         auxes;      /* array of per-thread aux data         */
    effector_t      effector;   /* pass effector (pre-sort or merge)    */
};
typedef struct _pmergesort_pass_context pmergesort_pass_context_t;
#endif

/* -------------------------------------------------------------------------------------------------------------------------- */

#define IDIV_UP(N, M)               ({ __typeof__(N) __n = (N); __typeof__(M) __m = (M); (__n + (__m - 1)) / __m; })
#if defined(__clang__) || defined(__GNUC__)
#define ISIGN(V)                    ({ __typeof__(V) __v = (V); ((__v > 0) - (__v < 0)); })
#define IABS(V)                     ({ __typeof__(V) __v = (V); (__v < 0 ? -__v : __v); })
#else
#error review this implementation for used compiler
#endif

#define ELT_PTR_OFS(ctx, base, inx) ELT_PTR_((base), (inx), ELT_SZ(ctx))
#define ELT_PTR_NEXT(ctx, base)     (((void *)(base)) + ELT_SZ(ctx))
#define ELT_PTR_PREV(ctx, base)     (((void *)(base)) - ELT_SZ(ctx))
#define ELT_DIST(ctx, a, b)         ELT_DIST_((a), (b), ELT_SZ(ctx))

#define MAKE_STR0(x, y)             x ## y
#define MAKE_STR1(x, y)             MAKE_STR0(x, y)
#define MAKE_FNAME0(x, y)           _ ## x ## _ ## y
#define MAKE_FNAME1(x, y)           MAKE_FNAME0(x, y)
#define _M(name)                    MAKE_FNAME1(name, SORT_SUFFIX)
#define _F(name)                    MAKE_STR1(name, SORT_IS_R)
#define _(name)                     MAKE_FNAME1(name, MAKE_STR1(SORT_SUFFIX, SORT_IS_R))

/* -------------------------------------------------------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------------------------------------------------------- */
/* memory accessors                                                                                                           */
/* -------------------------------------------------------------------------------------------------------------------------- */

#if CFG_AGNER_ACCESS
#include "asmlib.h"
#endif

static inline void _regions_swap(void * a, void * b, size_t sz)
{
#if CFG_RAW_ACCESS

#if __LP64__
    uint64_t * p64 = a;
    uint64_t * q64 = b;
    uint64_t t64;

    while (sz >= sizeof(uint64_t))
    {
        t64 = *p64;
        *p64++ = *q64;
        *q64++ = t64;

        sz -= sizeof(uint64_t);
    }

    uint8_t * p = (uint8_t *)p64;
    uint8_t * q = (uint8_t *)q64;
    uint8_t t;

    switch (sz)
    {
    case 7: t = *p; *p++ = *q; *q++ = t; /* let's violate codestyle a bit */
    case 6: t = *p; *p++ = *q; *q++ = t;
    case 5: t = *p; *p++ = *q; *q++ = t;
    case 4: t = *p; *p++ = *q; *q++ = t;
    case 3: t = *p; *p++ = *q; *q++ = t;
    case 2: t = *p; *p++ = *q; *q++ = t;
    case 1: t = *p; *p++ = *q; *q++ = t;
    case 0:
        break;
    default:
        /* should never happen */
        break;
    }
#else
    uint32_t * p32 = a;
    uint32_t * q32 = b;
    uint32_t t32;

    while (sz >= sizeof(uint32_t))
    {
        t32 = *p32;
        *p32++ = *q32;
        *q32++ = t32;

        sz -= sizeof(uint32_t);
    }

    uint8_t * p = (uint8_t *)p32;
    uint8_t * q = (uint8_t *)q32;
    uint8_t t;

    switch (sz)
    {
    case 3: t = *p; *p++ = *q; *q++ = t; /* let's violate codestyle a bit */
    case 2: t = *p; *p++ = *q; *q++ = t;
    case 1: t = *p; *p++ = *q; *q++ = t;
    case 0:
        break;
    default:
        /* should never happen */
        break;
    }
#endif

#else

    uint8_t * p = a;
    uint8_t * q = b;
    uint8_t t[128];

    while (sz >= sizeof(t))
    {
#if CFG_AGNER_ACCESS
        A_memcpy(t, p, sizeof(t));
        A_memcpy(p, q, sizeof(t));
        A_memcpy(q, t, sizeof(t));
#else
        memcpy(t, p, sizeof(t));
        memcpy(p, q, sizeof(t));
        memcpy(q, t, sizeof(t));
#endif

        p += sizeof(t);
        q += sizeof(t);
        sz -= sizeof(t);
    }

    if (sz > 0)
    {
#if CFG_AGNER_ACCESS
        A_memcpy(t, p, sz);
        A_memcpy(p, q, sz);
        A_memcpy(q, t, sz);
#else
        memcpy(t, p, sz);
        memcpy(p, q, sz);
        memcpy(q, t, sz);
#endif
    }

#endif
}

static inline void _region_copy(void * src, void * dst, size_t sz)
{
#if CFG_RAW_ACCESS

#if __LP64__
    uint64_t * p64 = src;
    uint64_t * q64 = dst;

    while (sz >= sizeof(uint64_t))
    {
        *q64++ = *p64++;

        sz -= sizeof(uint64_t);
    }

    uint8_t * p = (uint8_t *)p64;
    uint8_t * q = (uint8_t *)q64;

    switch (sz)
    {
    case 7: *q++ = *p++;
    case 6: *q++ = *p++;
    case 5: *q++ = *p++;
    case 4: *q++ = *p++;
    case 3: *q++ = *p++;
    case 2: *q++ = *p++;
    case 1: *q++ = *p++;
    case 0:
        break;
    default:
        /* should never happen */
        break;
    }
#else
    uint32_t * p32 = src;
    uint32_t * q32 = dst;

    while (sz >= sizeof(uint32_t))
    {
        *q32++ = *p32++;

        sz -= sizeof(uint32_t);
    }

    uint8_t * p = (uint8_t *)p32;
    uint8_t * q = (uint8_t *)q32;

    switch (sz)
    {
    case 3: *q++ = *p++;
    case 2: *q++ = *p++;
    case 1: *q++ = *p++;
    case 0:
        break;
    default:
        /* should never happen */
        break;
    }
#endif

#else

#if CFG_AGNER_ACCESS
    A_memcpy(dst, src, sz);
#else
    memcpy(dst, src, sz);
#endif

#endif
}

static inline void _region_move_right(void * src, void * dst, size_t sz)
{
#if CFG_RAW_ACCESS

#if __LP64__
    uint64_t * p64 = (uint64_t *)((uint8_t *)src + sz);
    uint64_t * q64 = (uint64_t *)((uint8_t *)dst + sz);

    while (sz >= sizeof(uint64_t))
    {
        *--q64 = *--p64;

        sz -= sizeof(uint64_t);
    }

    uint8_t * p = (uint8_t *)p64;
    uint8_t * q = (uint8_t *)q64;

    switch (sz)
    {
    case 7: *--q = *--p;
    case 6: *--q = *--p;
    case 5: *--q = *--p;
    case 4: *--q = *--p;
    case 3: *--q = *--p;
    case 2: *--q = *--p;
    case 1: *--q = *--p;
    case 0:
        break;
    default:
        /* should never happen */
        break;
    }
#else
    uint32_t * p32 = (uint32_t *)((uint8_t *)src + sz);
    uint32_t * q32 = (uint32_t *)((uint8_t *)dst + sz);

    while (sz >= sizeof(uint32_t))
    {
        *--q32 = *--p32;

        sz -= sizeof(uint32_t);
    }

    uint8_t * p = (uint8_t *)p32;
    uint8_t * q = (uint8_t *)q32;

    switch (sz)
    {
    case 3: *--q = *--p;
    case 2: *--q = *--p;
    case 1: *--q = *--p;
    case 0:
        break;
    default:
        /* should never happen */
        break;
    }
#endif

#else

#if CFG_AGNER_ACCESS
    A_memmove(dst, src, sz);
#else
    memmove(dst, src, sz);
#endif

#endif
}

static inline void _region_move_left(void * src, void * dst, size_t sz)
{
#if CFG_RAW_ACCESS

    _region_copy(src, dst, sz);

#else

#if CFG_AGNER_ACCESS
    A_memmove(dst, src, sz);
#else
    memmove(dst, src, sz);
#endif

#endif
}

/* -------------------------------------------------------------------------------------------------------------------------- */
/* allocate or adjust size of temporary storage if needed                                                                     */
/* -------------------------------------------------------------------------------------------------------------------------- */

static inline void * _aux_alloc(aux_t * aux, size_t sz)
{
    void * tmp = aux->temp;
    if (tmp == NULL || aux->sz < sz)
    {
        tmp = realloc(tmp, sz);
        if (tmp == NULL)
        {
            aux->rc = 1;
        }
        else
        {
            aux->sz = sz;
            aux->temp = tmp;
        }
    }

    return tmp;
}

static inline void _aux_free(aux_t * aux)
{
    if (aux->temp != NULL)
    {
        free(aux->temp);
        aux->temp = NULL;
    }
}

/* -------------------------------------------------------------------------------------------------------------------------- */

#if _CFG_USE_4_MEM

#define SORT_SUFFIX                 4

#define ELT_OF_SZ(n, sz)            ((n) << 2)
#define ELT_PTR_(base, inx, sz)     ({ __typeof__(inx) __inx = (inx); ((void *)(base)) + ISIGN(__inx) * (IABS(__inx) << 2); })
#define ELT_DIST_(a, b, sz)         ((((void *)(a)) - ((void *)(b))) >> 2)

#define ELT_TYPE                    uint32_t
#include "pmergesort-mem-n.inl"

#include "pmergesort-mem.inl"

#undef ELT_TYPE
#undef ELT_DIST_
#undef ELT_PTR_
#undef ELT_OF_SZ

#undef SORT_SUFFIX

#endif

/* -------------------------------------------------------------------------------------------------------------------------- */

#if _CFG_USE_8_MEM

#define SORT_SUFFIX                 8

#define ELT_OF_SZ(n, sz)            ((n) << 3)
#define ELT_PTR_(base, inx, sz)     ({ __typeof__(inx) __inx = (inx); ((void *)(base)) + ISIGN(__inx) * (IABS(__inx) << 3); })
#define ELT_DIST_(a, b, sz)         ((((void *)(a)) - ((void *)(b))) >> 3)

#if __LP64__
#define ELT_TYPE                    uint64_t
#include "pmergesort-mem-n.inl"
#else
#include "pmergesort-mem-sz.inl"
#endif

#include "pmergesort-mem.inl"

#undef ELT_TYPE
#undef ELT_DIST_
#undef ELT_PTR_
#undef ELT_OF_SZ

#undef SORT_SUFFIX

#endif

/* -------------------------------------------------------------------------------------------------------------------------- */

#if _CFG_USE_16_MEM

#define SORT_SUFFIX                 16

#define ELT_OF_SZ(n, sz)            ((n) << 4)
#define ELT_PTR_(base, inx, sz)     ({ __typeof__(inx) __inx = (inx); ((void *)(base)) + ISIGN(__inx) * (IABS(__inx) << 4); })
#define ELT_DIST_(a, b, sz)         ((((void *)(a)) - ((void *)(b))) >> 4)

#include "pmergesort-mem-sz.inl"
#include "pmergesort-mem.inl"

#undef ELT_DIST_
#undef ELT_PTR_
#undef ELT_OF_SZ

#undef SORT_SUFFIX

#endif

/* -------------------------------------------------------------------------------------------------------------------------- */

#define SORT_SUFFIX                 sz

#define ELT_OF_SZ(n, sz)            ((sz) * (n))
#define ELT_PTR_(base, inx, sz)     (((void *)(base)) + (sz) * (inx))
#define ELT_DIST_(a, b, sz)         ((((void *)(a)) - ((void *)(b))) / (sz))

#include "pmergesort-mem-sz.inl"
#include "pmergesort-mem.inl"

#undef ELT_DIST_
#undef ELT_PTR_
#undef ELT_OF_SZ

#undef SORT_SUFFIX

/* -------------------------------------------------------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------------------------------------------------------- */

#define SORT_IS_R                   v
#define CALL_CMP(ctx, a, b)         ((cmpv_t)((ctx)->cmp))((a), (b))
#define CALL_SORT(ctx, a, n)        ((sort_t)((ctx)->wsort))((a), (n), (ctx)->sz, (cmpv_t)(ctx)->cmp)

#include "pmergesort.inl"

/* -------------------------------------------------------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------------------------------------------------------- */

void symmergesort(void * base, size_t n, size_t sz, int (*cmp)(const void *, const void *))
{
    if (n < 2) /* have nothing to sort */
        return;

    context_t ctx = { base, n, sz, cmp, NULL, numCPU(), thPool(), 0, 0, NULL, NULL, NULL };

    _F(symmergesort)(&ctx);
}

int pmergesort(void * base, size_t n, size_t sz, int (*cmp)(const void *, const void *))
{
    if (n < 2) /* have nothing to sort */
        return 0;

    context_t ctx = { base, n, sz, cmp, NULL, numCPU(), thPool(), 0, 0, NULL, NULL, NULL };

    return _F(pmergesort)(&ctx);
}

int wrapmergesort(void * base, size_t n, size_t sz, int (*cmp)(const void *, const void *), int (*sort)(void *, size_t, size_t, int (*)(const void *, const void *)))
{
    if (n < 2) /* have nothing to sort */
        return 0;

    context_t ctx = { base, n, sz, cmp, NULL, numCPU(), thPool(), 0, 0, NULL, NULL, sort };

    return _F(wrapmergesort)(&ctx);
}

#if CFG_CORE_PROFILE
void insertionsort(void * base, size_t n, size_t sz, int (*cmp)(const void *, const void *))
{
    if (n < 2) /* have nothing to sort */
        return;

    context_t ctx = { base, n, sz, cmp, NULL, 0, NULL, 0, 0, NULL, NULL, NULL };

    _F(insertionsort)(&ctx);
}
#endif

#if CFG_CORE_PROFILE
void insertionsort_run(void * base, size_t n, size_t sz, int (*cmp)(const void *, const void *))
{
    if (n < 2) /* have nothing to sort */
        return;

    context_t ctx = { base, n, sz, cmp, NULL, 0, NULL, 0, 0, NULL, NULL, NULL };

    _F(insertionsort_run)(&ctx);
}
#endif

#if CFG_CORE_PROFILE
void insertionsort_mergerun(void * base, size_t n, size_t sz, int (*cmp)(const void *, const void *))
{
    if (n < 2) /* have nothing to sort */
        return;

    context_t ctx = { base, n, sz, cmp, NULL, 0, NULL, 0, 0, NULL, NULL, NULL };

    _F(insertionsort_mergerun)(&ctx);
}
#endif

/* -------------------------------------------------------------------------------------------------------------------------- */

#undef CALL_SORT
#undef CALL_CMP
#undef SORT_IS_R

/* -------------------------------------------------------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------------------------------------------------------- */

#define SORT_IS_R                   r
#define CALL_CMP(ctx, a, b)         ((cmpr_t)((ctx)->cmp))((void *)(ctx)->thunk, (a), (b))
#define CALL_SORT(ctx, a, n)        ((sort_r_t)((ctx)->wsort))((a), (n), (ctx)->sz, (void *)(ctx)->thunk, (cmpr_t)(ctx)->cmp)

#include "pmergesort.inl"

/* -------------------------------------------------------------------------------------------------------------------------- */

void symmergesort_r(void * base, size_t n, size_t sz, void * thunk, int (*cmp)(void *, const void *, const void *))
{
    if (n < 2) /* have nothing to sort */
        return;

    context_t ctx = { base, n, sz, cmp, thunk, numCPU(), thPool(), 0, 0, NULL, NULL, NULL };

    _F(symmergesort)(&ctx);
}

int pmergesort_r(void * base, size_t n, size_t sz, void * thunk, int (*cmp)(void *, const void *, const void *))
{
    if (n < 2) /* have nothing to sort */
        return 0;

    context_t ctx = { base, n, sz, cmp, thunk, numCPU(), thPool(), 0, 0, NULL, NULL, NULL };

    return _F(pmergesort)(&ctx);
}

int wrapmergesort_r(void * base, size_t n, size_t sz, void * thunk, int (*cmp)(void *, const void *, const void *), int (*sort_r)(void *, size_t, size_t, void *, int (*)(void *, const void *, const void *)))
{
    if (n < 2) /* have nothing to sort */
        return 0;

    context_t ctx = { base, n, sz, cmp, thunk, numCPU(), thPool(), 0, 0, NULL, NULL, sort_r };

    return _F(wrapmergesort)(&ctx);
}

/* -------------------------------------------------------------------------------------------------------------------------- */

#undef CALL_SORT
#undef CALL_CMP
#undef SORT_IS_R

/* -------------------------------------------------------------------------------------------------------------------------- */
