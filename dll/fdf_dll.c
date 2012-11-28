/*
 * FDF linkage; generated by mkfdfdll.
 * Copyright (c) 2012, SanDisk Corporation.  All rights reserved.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "fdf.h"


/*
 * Macros.
 */
#define nel(a)      (sizeof(a)/sizeof(*(a)))
#define unlikely(x) __builtin_expect((x), 0)


/*
 * FDF Library locations.
 */
static char *fdflibs[] ={
    "/usr/lib64/fdf/libfdf.so",
    "/usr/lib/fdf/libfdf.so",
    "/lib64/libfdf.so",
    "/lib/libfdf.so",
    "/usr/local/lib64/libfdf.so",
    "/usr/local/lib/libfdf.so",
    "libfdf.so",
};


/*
 * Function pointers.
 */
static FDF_status_t 
(*ptr_FDFInit)(struct FDF_state **fdf_state);

static FDF_status_t 
(*ptr_FDFInitPerThreadState)(struct FDF_state *fdf_state,
                             struct FDF_thread_state **thd_state);

static FDF_status_t 
(*ptr_FDFReleasePerThreadState)(struct FDF_thread_state **thd_state);

static FDF_status_t 
(*ptr_FDFShutdown)(struct FDF_state *fdf_state);

static FDF_status_t 
(*ptr_FDFLoadCntrPropDefaults)(FDF_container_props_t *props);

static FDF_status_t 
(*ptr_FDFOpenContainer)(struct FDF_thread_state *fdf_thread_state,
                        char *cname,
                        FDF_container_props_t *properties,
                        uint32_t flags,
                        FDF_cguid_t *cguid);

static FDF_status_t 
(*ptr_FDFCloseContainer)(struct FDF_thread_state *fdf_thread_state,
                         FDF_cguid_t cguid);

static FDF_status_t 
(*ptr_FDFDeleteContainer)(struct FDF_thread_state *fdf_thread_state,
                          FDF_cguid_t cguid);

static FDF_status_t 
(*ptr_FDFGetContainers)(struct FDF_thread_state *fdf_thread_state,
                        FDF_cguid_t *cguids,
                        uint32_t *n_cguids);

static FDF_status_t 
(*ptr_FDFGetContainerProps)(struct FDF_thread_state *fdf_thread_state,
                            FDF_cguid_t cguid,
                            FDF_container_props_t *pprops);

static FDF_status_t 
(*ptr_FDFSetContainerProps)(struct FDF_thread_state *fdf_thread_state,
                            FDF_cguid_t cguid,
                            FDF_container_props_t *pprops);

static FDF_status_t 
(*ptr_FDFReadObject)(struct FDF_thread_state *fdf_thread_state,
                     FDF_cguid_t cguid,
                     char *key,
                     uint32_t keylen,
                     char **data,
                     uint64_t *datalen);

static FDF_status_t 
(*ptr_FDFFreeBuffer)(char *buf);

static FDF_status_t 
(*ptr_FDFWriteObject)(struct FDF_thread_state *sdf_thread_state,
                      FDF_cguid_t cguid,
                      char *key,
                      uint32_t keylen,
                      char *data,
                      uint64_t datalen,
                      uint32_t flags);

static FDF_status_t 
(*ptr_FDFDeleteObject)(struct FDF_thread_state *fdf_thread_state,
                       FDF_cguid_t cguid,
                       char *key,
                       uint32_t keylen);

static FDF_status_t 
(*ptr_FDFEnumerateContainerObjects)(struct FDF_thread_state *fdf_thread_state,
                                    FDF_cguid_t cguid,
                                    struct FDF_iterator **iterator);

static FDF_status_t 
(*ptr_FDFNextEnumeratedObject)(struct FDF_thread_state *fdf_thread_state,
                               struct FDF_iterator *iterator,
                               char **key,
                               uint32_t *keylen,
                               char **data,
                               uint64_t *datalen);

static FDF_status_t 
(*ptr_FDFFinishEnumeration)(struct FDF_thread_state *fdf_thread_state,
                            struct FDF_iterator *iterator);

static FDF_status_t 
(*ptr_FDFFlushObject)(struct FDF_thread_state *fdf_thread_state,
                      FDF_cguid_t cguid,
                      char *key,
                      uint32_t keylen);

static FDF_status_t 
(*ptr_FDFFlushContainer)(struct FDF_thread_state *fdf_thread_state,
                         FDF_cguid_t cguid);

static FDF_status_t 
(*ptr_FDFFlushCache)(struct FDF_thread_state *fdf_thread_state);

static FDF_status_t 
(*ptr_FDFGetStats)(struct FDF_thread_state *fdf_thread_state,
                   FDF_stats_t *stats);

static FDF_status_t 
(*ptr_FDFGetContainerStats)(struct FDF_thread_state *fdf_thread_state,
                            FDF_cguid_t cguid,
                            FDF_stats_t *stats);


/*
 * Linkage table.
 */
static struct {
    const char *name;
    void       *func;
} table[] ={
    { "FDFInit",                       &ptr_FDFInit                      },
    { "FDFInitPerThreadState",         &ptr_FDFInitPerThreadState        },
    { "FDFReleasePerThreadState",      &ptr_FDFReleasePerThreadState     },
    { "FDFShutdown",                   &ptr_FDFShutdown                  },
    { "FDFLoadCntrPropDefaults",       &ptr_FDFLoadCntrPropDefaults      },
    { "FDFOpenContainer",              &ptr_FDFOpenContainer             },
    { "FDFCloseContainer",             &ptr_FDFCloseContainer            },
    { "FDFDeleteContainer",            &ptr_FDFDeleteContainer           },
    { "FDFGetContainers",              &ptr_FDFGetContainers             },
    { "FDFGetContainerProps",          &ptr_FDFGetContainerProps         },
    { "FDFSetContainerProps",          &ptr_FDFSetContainerProps         },
    { "FDFReadObject",                 &ptr_FDFReadObject                },
    { "FDFFreeBuffer",                 &ptr_FDFFreeBuffer                },
    { "FDFWriteObject",                &ptr_FDFWriteObject               },
    { "FDFDeleteObject",               &ptr_FDFDeleteObject              },
    { "FDFEnumerateContainerObjects",  &ptr_FDFEnumerateContainerObjects },
    { "FDFNextEnumeratedObject",       &ptr_FDFNextEnumeratedObject      },
    { "FDFFinishEnumeration",          &ptr_FDFFinishEnumeration         },
    { "FDFFlushObject",                &ptr_FDFFlushObject               },
    { "FDFFlushContainer",             &ptr_FDFFlushContainer            },
    { "FDFFlushCache",                 &ptr_FDFFlushCache                },
    { "FDFGetStats",                   &ptr_FDFGetStats                  },
    { "FDFGetContainerStats",          &ptr_FDFGetContainerStats         },
};


/*
 * Print out an error message and exit.
 */
static void
panic(char *fmt, ...)
{
    va_list alist;

    va_start(alist, fmt);
    vfprintf(stderr, fmt, alist);
    va_end(alist);
    fprintf(stderr, "\n");
    exit(1);
}


/*
 * An undefined symbol was found.
 */
static void
undefined(char *sym)
{
    panic("FDF: undefined symbol: %s", sym);
}


/*
 * Determine if the string ends with "No such file or directory".
 */
static int
nsfod(char *str)
{
    char *err = "No such file or directory";
    int  elen = strlen(err);
    int  slen = strlen(str);

    if (slen < elen)
        return 0;
    return !strcmp(str+slen-elen, err);
}


/*
 * Load the FDF library.
 */
static int
load(char *path)
{
    int i;
    void  *dl = dlopen(path, RTLD_LAZY);
    char *err = dlerror();

    if (!dl) {
        if (nsfod(err))
            return 0;
        panic("%s", err);
    }
    
    int n = nel(table);
    for (i = 0; i < n; i++) {
        const char *name = table[i].name;
        void *func = dlsym(dl, name);
        if (func)
            *(void **)table[i].func = func;
        else
            fprintf(stderr, "warning: FDF: undefined symbol: %s\n", name);
    }
    return 1;
}


/*
 * Load the FDF library.
 */
static void
parse(void)
{
    int i;
    char *lib = getenv("FDF_LIB");

    if (lib) {
        if (load(lib))
            return;
        panic("cannot find FDF_LIB=%s", lib);
    }

    if (load("/usr/lib64/fdf/libfdf.so"))
        return;

    for (i = 0; i < nel(fdflibs); i++)
        if (load(fdflibs[i]))
            return;
    panic("cannot find libfdf.so");
}


/*
 * FDFInit
 */
FDF_status_t 
FDFInit(struct FDF_state **fdf_state)
{
    parse();
    if (unlikely(!ptr_FDFInit))
        undefined("FDFInit");

    return (*ptr_FDFInit)(fdf_state);
}


/*
 * FDFInitPerThreadState
 */
FDF_status_t 
FDFInitPerThreadState(struct FDF_state *fdf_state,
                      struct FDF_thread_state **thd_state)
{
    if (unlikely(!ptr_FDFInitPerThreadState))
        undefined("FDFInitPerThreadState");

    return (*ptr_FDFInitPerThreadState)(fdf_state, thd_state);
}


/*
 * FDFReleasePerThreadState
 */
FDF_status_t 
FDFReleasePerThreadState(struct FDF_thread_state **thd_state)
{
    if (unlikely(!ptr_FDFReleasePerThreadState))
        undefined("FDFReleasePerThreadState");

    return (*ptr_FDFReleasePerThreadState)(thd_state);
}


/*
 * FDFShutdown
 */
FDF_status_t 
FDFShutdown(struct FDF_state *fdf_state)
{
    if (unlikely(!ptr_FDFShutdown))
        undefined("FDFShutdown");

    return (*ptr_FDFShutdown)(fdf_state);
}


/*
 * FDFLoadCntrPropDefaults
 */
FDF_status_t 
FDFLoadCntrPropDefaults(FDF_container_props_t *props)
{
    if (unlikely(!ptr_FDFLoadCntrPropDefaults))
        undefined("FDFLoadCntrPropDefaults");

    return (*ptr_FDFLoadCntrPropDefaults)(props);
}


/*
 * FDFOpenContainer
 */
FDF_status_t 
FDFOpenContainer(struct FDF_thread_state *fdf_thread_state,
                 char *cname,
                 FDF_container_props_t *properties,
                 uint32_t flags,
                 FDF_cguid_t *cguid)
{
    if (unlikely(!ptr_FDFOpenContainer))
        undefined("FDFOpenContainer");

    return (*ptr_FDFOpenContainer)(fdf_thread_state,
                                   cname,
                                   properties,
                                   flags,
                                   cguid);
}


/*
 * FDFCloseContainer
 */
FDF_status_t 
FDFCloseContainer(struct FDF_thread_state *fdf_thread_state,
                  FDF_cguid_t cguid)
{
    if (unlikely(!ptr_FDFCloseContainer))
        undefined("FDFCloseContainer");

    return (*ptr_FDFCloseContainer)(fdf_thread_state, cguid);
}


/*
 * FDFDeleteContainer
 */
FDF_status_t 
FDFDeleteContainer(struct FDF_thread_state *fdf_thread_state,
                   FDF_cguid_t cguid)
{
    if (unlikely(!ptr_FDFDeleteContainer))
        undefined("FDFDeleteContainer");

    return (*ptr_FDFDeleteContainer)(fdf_thread_state, cguid);
}


/*
 * FDFGetContainers
 */
FDF_status_t 
FDFGetContainers(struct FDF_thread_state *fdf_thread_state,
                 FDF_cguid_t *cguids,
                 uint32_t *n_cguids)
{
    if (unlikely(!ptr_FDFGetContainers))
        undefined("FDFGetContainers");

    return (*ptr_FDFGetContainers)(fdf_thread_state, cguids, n_cguids);
}


/*
 * FDFGetContainerProps
 */
FDF_status_t 
FDFGetContainerProps(struct FDF_thread_state *fdf_thread_state,
                     FDF_cguid_t cguid,
                     FDF_container_props_t *pprops)
{
    if (unlikely(!ptr_FDFGetContainerProps))
        undefined("FDFGetContainerProps");

    return (*ptr_FDFGetContainerProps)(fdf_thread_state, cguid, pprops);
}


/*
 * FDFSetContainerProps
 */
FDF_status_t 
FDFSetContainerProps(struct FDF_thread_state *fdf_thread_state,
                     FDF_cguid_t cguid,
                     FDF_container_props_t *pprops)
{
    if (unlikely(!ptr_FDFSetContainerProps))
        undefined("FDFSetContainerProps");

    return (*ptr_FDFSetContainerProps)(fdf_thread_state, cguid, pprops);
}


/*
 * FDFReadObject
 */
FDF_status_t 
FDFReadObject(struct FDF_thread_state *fdf_thread_state,
              FDF_cguid_t cguid,
              char *key,
              uint32_t keylen,
              char **data,
              uint64_t *datalen)
{
    if (unlikely(!ptr_FDFReadObject))
        undefined("FDFReadObject");

    return (*ptr_FDFReadObject)(fdf_thread_state,
                                cguid,
                                key,
                                keylen,
                                data,
                                datalen);
}


/*
 * FDFFreeBuffer
 */
FDF_status_t 
FDFFreeBuffer(char *buf)
{
    if (unlikely(!ptr_FDFFreeBuffer))
        undefined("FDFFreeBuffer");

    return (*ptr_FDFFreeBuffer)(buf);
}


/*
 * FDFWriteObject
 */
FDF_status_t 
FDFWriteObject(struct FDF_thread_state *sdf_thread_state,
               FDF_cguid_t cguid,
               char *key,
               uint32_t keylen,
               char *data,
               uint64_t datalen,
               uint32_t flags)
{
    if (unlikely(!ptr_FDFWriteObject))
        undefined("FDFWriteObject");

    return (*ptr_FDFWriteObject)(sdf_thread_state,
                                 cguid,
                                 key,
                                 keylen,
                                 data,
                                 datalen,
                                 flags);
}


/*
 * FDFDeleteObject
 */
FDF_status_t 
FDFDeleteObject(struct FDF_thread_state *fdf_thread_state,
                FDF_cguid_t cguid,
                char *key,
                uint32_t keylen)
{
    if (unlikely(!ptr_FDFDeleteObject))
        undefined("FDFDeleteObject");

    return (*ptr_FDFDeleteObject)(fdf_thread_state, cguid, key, keylen);
}


/*
 * FDFEnumerateContainerObjects
 */
FDF_status_t 
FDFEnumerateContainerObjects(struct FDF_thread_state *fdf_thread_state,
                             FDF_cguid_t cguid,
                             struct FDF_iterator **iterator)
{
    if (unlikely(!ptr_FDFEnumerateContainerObjects))
        undefined("FDFEnumerateContainerObjects");

    return (*ptr_FDFEnumerateContainerObjects)(fdf_thread_state,
                                               cguid,
                                               iterator);
}


/*
 * FDFNextEnumeratedObject
 */
FDF_status_t 
FDFNextEnumeratedObject(struct FDF_thread_state *fdf_thread_state,
                        struct FDF_iterator *iterator,
                        char **key,
                        uint32_t *keylen,
                        char **data,
                        uint64_t *datalen)
{
    if (unlikely(!ptr_FDFNextEnumeratedObject))
        undefined("FDFNextEnumeratedObject");

    return (*ptr_FDFNextEnumeratedObject)(fdf_thread_state,
                                          iterator,
                                          key,
                                          keylen,
                                          data,
                                          datalen);
}


/*
 * FDFFinishEnumeration
 */
FDF_status_t 
FDFFinishEnumeration(struct FDF_thread_state *fdf_thread_state,
                     struct FDF_iterator *iterator)
{
    if (unlikely(!ptr_FDFFinishEnumeration))
        undefined("FDFFinishEnumeration");

    return (*ptr_FDFFinishEnumeration)(fdf_thread_state, iterator);
}


/*
 * FDFFlushObject
 */
FDF_status_t 
FDFFlushObject(struct FDF_thread_state *fdf_thread_state,
               FDF_cguid_t cguid,
               char *key,
               uint32_t keylen)
{
    if (unlikely(!ptr_FDFFlushObject))
        undefined("FDFFlushObject");

    return (*ptr_FDFFlushObject)(fdf_thread_state, cguid, key, keylen);
}


/*
 * FDFFlushContainer
 */
FDF_status_t 
FDFFlushContainer(struct FDF_thread_state *fdf_thread_state,
                  FDF_cguid_t cguid)
{
    if (unlikely(!ptr_FDFFlushContainer))
        undefined("FDFFlushContainer");

    return (*ptr_FDFFlushContainer)(fdf_thread_state, cguid);
}


/*
 * FDFFlushCache
 */
FDF_status_t 
FDFFlushCache(struct FDF_thread_state *fdf_thread_state)
{
    if (unlikely(!ptr_FDFFlushCache))
        undefined("FDFFlushCache");

    return (*ptr_FDFFlushCache)(fdf_thread_state);
}


/*
 * FDFGetStats
 */
FDF_status_t 
FDFGetStats(struct FDF_thread_state *fdf_thread_state, FDF_stats_t *stats)
{
    if (unlikely(!ptr_FDFGetStats))
        undefined("FDFGetStats");

    return (*ptr_FDFGetStats)(fdf_thread_state, stats);
}


/*
 * FDFGetContainerStats
 */
FDF_status_t 
FDFGetContainerStats(struct FDF_thread_state *fdf_thread_state,
                     FDF_cguid_t cguid,
                     FDF_stats_t *stats)
{
    if (unlikely(!ptr_FDFGetContainerStats))
        undefined("FDFGetContainerStats");

    return (*ptr_FDFGetContainerStats)(fdf_thread_state, cguid, stats);
}
