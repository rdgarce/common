// A lock-free, concurrent, generic queue in 32 bits
//
// This is a bit of a mess since I wanted to hit many different combinations
// of implementations with the same code, especially under TSan:
//
// impl  threads  atom target cmd
// ----  -------  ---- ------ ----------
// GCC   pthreads C11  spsc   gcc -O3 -DNTHR=1 -DPTHREADS queue.c
// GCC   pthreads GNU  spsc   gcc -O3 -std=c99 -DNTHR=1 -DPTHREADS queue.c
// GCC   win32    C11  spsc   gcc -O3 -DNTHR=1 queue.c
// GCC   win32    GNU  spsc   gcc -O3 -std=c99 -DNTHR=1 queue.c
// GCC   pthreads C11  spmc   gcc -O3 -DNTHR=2 -DPTHREADS queue.c
// GCC   pthreads GNU  spmc   gcc -O3 -std=c99 -DNTHR=2 -DPTHREADS queue.c
// GCC   win32    C11  spmc   gcc -O3 -DNTHR=2 queue.c
// GCC   win32    GNU  spmc   gcc -O3 -std=c99 -DNTHR=2 queue.c
// Clang pthreads C11  spsc   clang -O3 -DNTHR=1 -DPTHREADS queue.c
// Clang pthreads GNU  spsc   clang -O3 -std=c99 -DNTHR=1 -DPTHREADS queue.c
// Clang win32    C11  spsc   clang -O3 -DNTHR=1 queue.c
// Clang win32    GNU  spsc   clang -O3 -std=c99 -DNTHR=1 queue.c
// Clang pthreads C11  spmc   clang -O3 -DNTHR=2 -DPTHREADS queue.c
// Clang pthreads GNU  spmc   clang -O3 -std=c99 -DNTHR=2 -DPTHREADS queue.c
// Clang win32    C11  spmc   clang -O3 -DNTHR=2 queue.c
// Clang win32    GNU  spmc   clang -O3 -std=c99 -DNTHR=2 queue.c
// MSC   win32    MSC  spsc   cl /Ox /DNTHR=1 queue.c
// MSC   win32    MSC  spmc   cl /Ox /DNTHR=2 queue.c
//
// Also multiply that by multiple operating systems (Linux, Windows, BSD).
//
// Ref: https://nullprogram.com/blog/2022/05/14/
// This is free and unencumbered software released into the public domain.
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "broadcast.h"

#define QEXP  10
#ifndef NTHR
#  define NTHR 1
#endif

// Threads
#if _WIN32 && !defined(PTHREADS)
#  include <windows.h>
#  include <process.h>
#  define WHAT_THREADS "win32"
   typedef HANDLE pthread_t;
#  define pthread_create(t,p,f,a) \
       *(t) = (HANDLE)_beginthreadex(0, 0, (void *)f, a, 0, 0)
#  define pthread_join(t,r) \
       do { \
           WaitForSingleObject(t, INFINITE); \
           CloseHandle(t); \
       } while (0)
#else
#  include <pthread.h>
#  define WHAT_THREADS "pthreads"
#endif

// Atomics
#if __STDC_VERSION__ >= 201112L && !__STDC_NO_ATOMICS
#  include <stdatomic.h>
#  define WHAT_ATOMICS "C11"
#elif __GNUC__
#  define WHAT_ATOMICS "GNUC"
#  define ATOMIC_VENDOR 1
#  define _Atomic
#  define ATOMIC_LOAD(q)       __atomic_load_n(q, __ATOMIC_ACQUIRE)
#  define ATOMIC_RLOAD(q)      __atomic_load_n(q, __ATOMIC_RELAXED)
#  define ATOMIC_STORE(q, v)   __atomic_store_n(q, v, __ATOMIC_RELEASE)
#  define ATOMIC_ADD(q, c)     __atomic_add_fetch(q, c, __ATOMIC_RELEASE)
#  define ATOMIC_AND(q, m)     __atomic_and_fetch(q, m, __ATOMIC_RELEASE)
#  define ATOMIC_CAS(q, e, d)  __atomic_compare_exchange_n( \
        q, e, d, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)
#elif _MSC_VER
#  define WHAT_ATOMICS "MSC"
#  define ATOMIC_VENDOR 1
#  include <winnt.h>
#  define _Atomic volatile
#  define ATOMIC_LOAD(a)       *(a)      // MSC volatile has atomic semantics
#  define ATOMIC_RLOAD(a)      *(a)      //
#  define ATOMIC_STORE(a, v)   *(a) = v  // "
#  define ATOMIC_ADD(a, c)     InterlockedAdd(a, c)
#  define ATOMIC_AND(a, m)     InterlockedAnd(a, m)
#  define ATOMIC_CAS(a, e, d)  (InterlockedCompareExchange(a, d, *e) == *e)
#endif


char threads_stop = 0;
Broadcast q = {0};
atomic_char slots[1 << QEXP];

static void *worker(void *arg)
{
    unsigned long thid = *(pthread_t *)arg;
    unsigned long my_uuid = 0;
    while (!__atomic_load_n(&threads_stop, __ATOMIC_ACQUIRE))
    {
        Reader r;
        if (brdct_attach_reader(&q, QEXP, &r)) return NULL;
        char buff[256];
        snprintf(buff, sizeof(buff), "%lu_%lu.txt", thid, ++my_uuid);
        FILE *out_file = fopen(buff, "wb");
        if (!out_file) return NULL;

        do
        {
            Slice s = brdct_reader_slice(&q, QEXP, &r);
            size_t count = rand() % (s.cnt[0] + 1);
            size_t res = fwrite(slots + s.idx[0], 1, count, out_file);
            s.cnt[0] -= res;
            brdct_reader_commit(&q, QEXP, &r, &s);
            // Probability of leaving
        } while (rand() >= RAND_MAX / 1000);
        
        fclose(out_file);
        brdct_detach_reader(&q, QEXP, &r);
    }

    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 2) return -1;
    
    printf("Using %d "WHAT_THREADS" threads, "WHAT_ATOMICS" atomics\n", NTHR);

    FILE * in_file = fopen(argv[1], "rb");
    if (!in_file) return -1;

    pthread_t thr[NTHR] = {0};
    for (int i = 0; i < NTHR; i++)
    {
        pthread_t *p = thr + i;
        pthread_create(thr + i, 0, worker, p);
    }

    while (!feof(in_file) && !ferror(in_file))
    {
        Slice s = brdct_writer_slice(&q, QEXP);
        size_t count = rand() % (s.cnt[0] + 1);
        size_t res = fread(slots + s.idx[0], 1, count, in_file);
        s.cnt[0] -= res;
        brdct_writer_commit(&q, QEXP, &s);
    }

    __atomic_store_n(&threads_stop, 1, __ATOMIC_RELEASE);

    for (int i = 0; i < NTHR; i++)
        pthread_join(thr[i], 0);

    return 0;
}