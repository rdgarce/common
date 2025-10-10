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

#ifndef QUEUE_H
#define QUEUE_H

/* An efficient generic queue implementation.
 * This is suitable for an SPSC scenario with at most two concurrently
 * executing threads. Some notable facts:
 *
 * 1: To correctly represent the ring state you only need two variables:
 *      head + tail or head + num_elements. You don’t strictly need
 *      all three of them.
 * 2: The provided API allows skipping unnecessary user-to-user copies
 *      when producing and consuming.
 * 3: By using head + tail instead of head + num_elements, the ring can
 *      be lock-free. Even though producer and consumer access the same
 *      variables, each one only updates a single variable. Since a stale
 *      value still corresponds to a valid ring state, no incorrect states
 *      are possible. The only requirement is that head and tail must be
 *      updated with release consistency and read with acquire consistency,
 *      ensuring the buffer memory is updated before the state variable.
 *      If head + num_elements were used instead, the consumer would need
 *      to atomically update both variables after a pop to avoid an
 *      incorrect state. This would require either a lock or storing both
 *      variables in a single atomically accessible word, an unnecessary
 *      complication compared to the head + tail approach.
 * 4: By restricting the queue length to a power of 2 and storing head
 *      and tail without applying the modulo, you eliminate the ambiguity
 *      of whether the ring is empty or full when head == tail.
 *      - head == tail always means the ring is empty
 *      - (tail - head) == capacity always means the ring is full
 *      Even when tail wraps around SIZE_MAX, the implicit (mod SIZE_MAX+1)
 *      applied to all operations ensures correctness.
 *      NOTE: If the queue length is not a power of 2, this approach
 *      produces incorrect states due to the implicit modulo.
 *      EXAMPLE:
 *      Suppose the queue size is 3 and head == SIZE_MAX, about to wrap
 *      back to zero. To pop the next element you access:
 *      data[head % 3] == data[0], because SIZE_MAX % 3 == 0.
 *      After the pop, you update head as follows:
 *      head = head + 1 == 0, because SIZE_MAX + 1 == 0.
 *      The value of head has advanced, but its numeric value is still zero.
 * 5: With a power-of-two length, all modulo operations reduce to bitwise
 *      operations.
 * 6: With a power-of-two length, you can implement the queue completely
 *      branchless using bitwise operations.
 * 7: There is no need to store a pointer to the data buffer in the queue
 *      state. The queue is fully represented by head and tail. As a result,
 *      push and pop functions return indices rather than pointers.
 *      This also provides a key advantage:
 *      the queue can be used with any data type, since the indices refer
 *      to positions within the user-defined array.
 */

#include <stddef.h>

typedef struct { size_t head, tail; } Queue;

/* Given the queue [q] of size 2^[cap_lg2], sets [*count] to the number
 * of poppable elements and returns the index of the first one. */
static size_t queue_pop(Queue *q, unsigned char cap_lg2, size_t *count)
{
    // This private copy of tail is essential to maintain a coherent
    // value throughout the function, regardless of the consumer's
    // actions.
    // The ACQUIRE semantic is required because, without it, reads of
    // the data could be reordered before the q->tail read.
    // If that happens, a read might occur before the producer’s writes
    // to the same elements become visible in memory, causing the consumer
    // to read bytes that have not yet been produced.
    size_t tail = __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE);
    size_t mask = ((size_t)1 << cap_lg2) - 1;
    // The cond variable is 0 iff tail is in the same (mask + 1)-sized block;
    // otherwise, it is:
    //   -- 1, if tail is in the next block and has not wrapped around SIZE_MAX,
    //   -- a large odd negative number, if tail has wrapped around SIZE_MAX.
    // The final bitwise AND reduces these cases to just (0, 1).
    // We then use cond to conditionally subtract from the final value.
    size_t cond = ((tail >> cap_lg2) - (q->head >> cap_lg2)) & 0x1;
    *count = tail - q->head - (tail & mask) * cond;

    return q->head & mask;
}

/* Commits the pop of [count] elements from queue [q]. */
static inline void queue_commit_pop(Queue *q, size_t count)
{
    __atomic_store_n(&q->head, q->head + count, __ATOMIC_RELEASE);
}

/* Given the queue [q] of size 2^[cap_lg2], sets [*count] to the number
 * of pushable elements and returns the index of the first one. */
static size_t queue_push(Queue *q, unsigned char cap_lg2, size_t *count)
{
    size_t head = __atomic_load_n(&q->head, __ATOMIC_ACQUIRE);
    size_t mask = ((size_t)1 << cap_lg2) - 1;
    size_t cond = ((q->tail >> cap_lg2) - (head >> cap_lg2)) & 0x1;
    *count = mask + 1 - (q->tail - head) - (head & mask) * (1 - cond);

    return q->tail & mask;
}

/* Commits the push of [count] elements from queue [q]. */
static inline void queue_commit_push(Queue *q, size_t count)
{
    __atomic_store_n(&q->tail, q->tail + count, __ATOMIC_RELEASE);
}

#endif