/*
 * MIT License
 * 
 * Copyright (c) 2025 Raffaele del Gaudio, https://delgaudio.me
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
 
#ifndef BROADCAST_H
#define BROADCAST_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#if ATOMIC_LLONG_LOCK_FREE == 2 || (ATOMIC_LONG_LOCK_FREE == 2 && ULONG_MAX == UINT64_MAX)
#define BRDCT_MAX_CAPLG2 33
#define BRDCT_RESIZE 0
#define brdct_t uint_fast64_t
#define atomic_brdct_t atomic_uint_fast64_t
typedef uint32_t Reader;
#else
#define BRDCT_MAX_CAPLG2 17
#define BRDCT_RESIZE 1
#define brdct_t uint_fast32_t
#define atomic_brdct_t atomic_uint_fast32_t
typedef uint16_t Reader;
#endif

typedef union
{
    atomic_brdct_t raw;
    struct
    {
        brdct_t tail      : (33  >> BRDCT_RESIZE) + BRDCT_RESIZE;
        brdct_t nreaders  : 15  >> BRDCT_RESIZE;
        brdct_t ncycled   : 15  >> BRDCT_RESIZE;
        // hstate values (N is the size of the queue):
        // - 0 if head and tail are in the same block of N/2 elements and
        //      head is at the start of this block,
        // - 1 if tail is in the next block and head is at the start of
        //      the previous block.
        brdct_t hstate    : 1;
    };
} Broadcast;

typedef struct
{
    size_t idx[2];
    size_t cnt[2];
    size_t len;
} Slice;

static int brdct_attach_reader(Broadcast *brc, unsigned char caplg2, Reader *r)
{
    Broadcast curr, new;
    curr.raw = atomic_load_explicit(&brc->raw, memory_order_relaxed);
    do
    {
        if (curr.nreaders == ((1 << 15) - 1) >> BRDCT_RESIZE) return -1;
        new = curr;
        new.nreaders += 1;
    } while (!atomic_compare_exchange_strong_explicit(&brc->raw, &curr.raw,
        new.raw, memory_order_acq_rel, memory_order_relaxed));
    
    brdct_t halflen = (brdct_t)1 << (caplg2 - 1);
    *r = (new.tail & -halflen) - halflen * new.hstate;
    
    return 0;
}

static void brdct_detach_reader(Broadcast *brc, unsigned char caplg2, Reader *r)
{
    Broadcast curr, new;
    curr.raw = atomic_load_explicit(&brc->raw, memory_order_relaxed);
    do
    {
        new = curr;
        new.nreaders -= 1;
        if (new.hstate && new.tail >> (caplg2 - 1) == *r >> (caplg2 - 1))
            new.ncycled -= 1;
    } while (!atomic_compare_exchange_strong_explicit(&brc->raw, &curr.raw,
        new.raw, memory_order_acq_rel, memory_order_relaxed));
}

static Slice brdct_reader_slice(Broadcast *brc, unsigned char caplg2, Reader *r)
{
    Broadcast curr;
    curr.raw = atomic_load_explicit(&brc->raw, memory_order_acquire);
    brdct_t mask = ((brdct_t)1 << caplg2) - 1;
    Slice s = { .idx[0] = *r & mask, .cnt[0] = curr.tail - *r };
    if (curr.tail >> caplg2 != *r >> caplg2)
    {
        s.cnt[0] -= curr.tail & mask;
        s.cnt[1] = curr.tail & mask;
    }
    s.len = s.cnt[0] + s.cnt[1];
    
    return s;
}

static void brdct_reader_commit(Broadcast *brc, unsigned char caplg2, Reader *r, Slice *s)
{
    size_t count = s->len - (s->cnt[0] + s->cnt[1]);
    Reader prev = *r;
    *r += count;
    if (*r >> (caplg2 - 1) == prev >> (caplg2 - 1)) return;
    
    Broadcast curr, new;
    curr.raw = atomic_load_explicit(&brc->raw, memory_order_relaxed);
    do
    {
        new = curr;
        new.ncycled += 1;
    } while (!atomic_compare_exchange_strong_explicit(&brc->raw, &curr.raw,
        new.raw, memory_order_acq_rel, memory_order_relaxed));
}

static Slice brdct_writer_slice(Broadcast *brc, unsigned char caplg2)
{
    Broadcast curr, new;
    curr.raw = atomic_load_explicit(&brc->raw, memory_order_acquire);
    do
    {
        new = curr;
        if (new.nreaders == 0 || new.ncycled < new.nreaders) break;
        new.ncycled = 0;
        new.hstate = 0;
    } while (!atomic_compare_exchange_strong_explicit(&brc->raw, &curr.raw,
        new.raw, memory_order_acq_rel, memory_order_relaxed));
    
    brdct_t halflen = (brdct_t)1 << (caplg2 - 1);
    brdct_t head = (new.tail & -halflen) - halflen * new.hstate;
    brdct_t mask = ((brdct_t)1 << caplg2) - 1;
    Slice s = { .idx[0] = new.tail & mask,
        .cnt[0] = mask + 1 - (new.tail - head) };
    if (new.tail >> caplg2 == head >> caplg2)
    {
        s.cnt[0] -= head & mask;
        s.cnt[1] = head & mask;
    }
    s.len = s.cnt[0] + s.cnt[1];
    
    // Blocks productions that would lead to a full queue.
    if (new.tail + s.len - head == (brdct_t)1 << caplg2)
    {
        s.len -= 1;
        if (s.cnt[1] > 0) s.cnt[1] -= 1;
        else if (s.cnt[0] > 0) s.cnt[0] -= 1;
    }

    return s;
}

static void brdct_writer_commit(Broadcast *brc, unsigned char caplg2, Slice *s)
{
    size_t count = s->len - (s->cnt[0] + s->cnt[1]);
    Broadcast curr, new;
    curr.raw = atomic_load_explicit(&brc->raw, memory_order_relaxed);
    do
    {
        new = curr;
        new.tail += count;
        if (new.tail >> (caplg2 - 1) != curr.tail >> (caplg2 - 1))
            new.hstate = 1;
    } while (!atomic_compare_exchange_strong_explicit(&brc->raw, &curr.raw,
        new.raw, memory_order_acq_rel, memory_order_relaxed));
}

#undef brdct_t
#undef atomic_brdct_t
#undef BRDCT_RESIZE

#endif // BROADCAST_H
