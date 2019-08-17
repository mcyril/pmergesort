### BRIEF

Parallel in-place/out-of-place merge sort algorithm implementations written in C language

### GOAL & SOLUTION

The aim was to implement stable and performant parallel sort algorithm with the minimal memory footprint. The merge sort is used as the base since it’s highly adaptive to a parallel processing. In-place mergesort based on [SymMerge](https://dx.doi.org/10.1007%2F978-3-540-30140-0_63) algorithm from Kim Pok-Son and Arne Kutzner, "Stable Minimum Storage Merging by Symmetric Comparisons", in Susanne Albers and Tomasz Radzik, editors, Algorithms - ESA 2004, volume 3221 of Lecture Notes in Computer Science, pages 714-723. Springer, 2004.

Current implementation is macOS-centric, thus it uses [Apple GCD](https://en.wikipedia.org/wiki/Grand_Central_Dispatch) for multi-threading (if available), though it’s possible to use pthreads based multi-threading mechanism (basic Thread Pool implementation is included as well).

### INTERFACE (defined in pmergesort.h)

#### symmergesort / symmergesort\_r

In-place mergesort based on optimized [SymMerge](https://dx.doi.org/10.1007%2F978-3-540-30140-0_63) algorithm, might work as single threaded or parallel depending on configuration.

The prototype of regular **symmergesort** has function declaration similar to the standard library qsort function, so seamless replacement possible:

    void symmergesort(void * base, size_t n, size_t sz,
                       int (*cmp)(const void *, const void *));

The prototype of reentrant **symmergesort\_r** has function declaration similar to the standard library qsort\_r function, so seamless replacement possible:

    void symmergesort_r(void * base, size_t n, size_t sz, void * thunk,
                         int (*cmp)(void *, const void *, const void *));

#### pmergesort / pmergesort\_r

Out-of-place merge sort, optimized naïve implementation, might work as single threaded or parallel depending on configuration. Implemented just out of curiosity.

The prototype of regular **pmergesort** has function declaration similar to the standard library mergesort function, so seamless replacement possible:

    int pmergesort(void * base, size_t n, size_t sz,
                    int (*cmp)(const void *, const void *));

The prototype of reentrant **pmergesort\_r** has function declaration similar to the standard library qsort\_r function with result, so seamless replacement possible:

    int pmergesort_r(void * base, size_t n, size_t sz, void * thunk,
                      int (*cmp)(void *, const void *, const void *));

#### wrapmergesort / wrapmergesort\_r

The parallel compound wrapper for generic sort, meaningless if works as single threaded. Implemented just out of curiosity.

The prototype of regular **wrapmergesort** has function declaration similar to the standard library qsort function with annex argument of wrapped sort function:

    int wrapmergesort(void * base, size_t n, size_t sz,
                       int (*cmp)(const void *, const void *),
                        int (*sort)(void *, size_t, size_t,
                                     int (*)(const void *, const void *)));

The prototype of reentrant **wrapmergesort\_r** has function declaration similar to the standard library qsort\_r function with annex argument of wrapped sort function:

    int wrapmergesort_r(void * base, size_t n, size_t sz, void * thunk,
                         int (*cmp)(void *, const void *, const void *),
                          int (*sort_r)(void *, size_t, size_t, void *,
                                         int (*)(void *, const void *, const void *)));

### CONFIGURATION (see in pmergesort.c)

Configure algorithm parameters/settings using pre-processor directives (0 is ‘off’, 1 is ‘on’):

* **PMR\_PARALLEL\_USE\_GCD**
    * enable use of GCD for multi-threading
    * default is on
* **PMR\_PARALLEL\_USE\_PTHREADS**
    * enable use of pthreads based pool for multi-threading
    * default is off
* **CFG_PARALLEL\_USE\_OMP**
    * enable use of OpenMP® for multi-threading (experimental)
    * default is off
* **PMR\_RAW\_ACCESS**
    * enable raw memory access
    * off - implies the using of memmove and memcpy
    * on - use raw memory access
    * default is on
* **PMR\_RAW\_ACCESS\_ALIGNED**
    * enable aligned raw memory access
    * default is off

To enable the build of parallel merge sort algorithms set one of **PMR\_PARALLEL\_USE\_GCD**, **PMR\_PARALLEL\_USE\_PTHREADS**, or **CFG_PARALLEL\_USE\_OMP** on.

Configure intrinsic memory management and access overrides

* **PMR\_MEMCPY**
    * **memcpy** override for non raw memory access
    * default is **memcpy**
* **PMR\_MEMMOVE**
    * **memmove** override for non raw memory access
    * default is **memmove**
* **PMR\_MALLOC**
    * **malloc** override
    * default is **malloc**
* **PMR\_REALLOC**
    * **realloc** override
    * default is **realloc**
* **PMR\_FREE**
    * **free** override
    * default is **free**

Algorithms fine tuning and tweaks using pre-processor directives (see source code comments):

* **\_PMR\_QUEUE\_OVERCOMMIT**
* **\_PMR\_GCD\_OVERCOMMIT**
* **\_PMR\_PARALLEL\_MAY\_SPAWN**
* **\_PMR\_PRESORT**
* **\_PMR\_USE\_4\_MEM**
* **\_PMR\_USE\_8\_MEM**
* **\_PMR\_USE\_16\_MEM**
* **\_PMR\_RAW\_ACCESS\_ALIGNED**
* **\_PMR\_TMP\_ROT**
* **\_PMR\_MIN\_SUBMERGELEN1**
* **\_PMR\_MIN\_SUBMERGELEN2**
* **\_PMR\_BLOCKLEN\_MTHRESHOLD0**
* **\_PMR\_BLOCKLEN\_MTHRESHOLD**
* **\_PMR\_BLOCKLEN\_SYMMERGE**
* **\_PMR\_BLOCKLEN\_MERGE**

### SUPPORTED PLATFORMS

Source code has been built and tested on macOS (10.5 - 10.15) only.

Future potential:

* Can be quite easily made POSIX compatible, but not tested that yet
* Byte order independent and should be compatible with any CPU architecture, but not tested with big-endian yet

### BUILD

    clang -mmacosx-version-min=10.6 -c pmergesort.c -Ofast -o pmergesort.o

    pgcc -fast -O4 -Mipa=fast,inline,force -DMAC_OS_X_VERSION_MIN_REQUIRED=1060 -DTARGET_CPU_X86_64=1 -DTARGET_OS_MAC=1 -c pmergesort.c -o pmergesort.o

    icc -O3 -std=c99 -c pmergesort.c -o pmergesort.o

_TODO: makefile_

### PERFORMANCE

Depends on CPU power/number of cores
Ironically, on practice the single threaded **pmergesort** is faster and with twice lesser memory footprint than standard macOS **mergesort**

Time complexity:

* **symmergesort** performance (as per single threaded algorithm) is better than O(N*log(N)^2)
* **pmergesort** performance (as per single threaded algorithm) is about standard O(N*log(N))

Memory footprint:

* **symmergesort** is O(1)
* **pmergesort** is O(N/2) in worst case
