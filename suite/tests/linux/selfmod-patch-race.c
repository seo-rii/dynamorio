/* **********************************************************
 * Copyright (c) 2026 The DynamoRIO contributors.  All rights reserved.
 * **********************************************************/

/*
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of Google, Inc. nor the names of its contributors may be
 *   used to endorse or promote products derived from this software without
 *   specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL GOOGLE, INC. OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

/* A thread keeps patching a generated function on a writable and executable
 * page, as a JIT does, while other threads call it.  The test runs with
 * -sandbox_writable, so the function's blocks are sandboxed: each checks on entry
 * that the code is still what the block was built from.  A version counter is odd
 * while a patch is in progress, and a call between two reads of the same even
 * version must return that version's result.  A block built from the code before
 * a patch must not pass its check after it.
 */

#include "tools.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/mman.h>

#define ROUNDS 4000
/* How long the patching thread waits between patches varies, so that some patches
 * land while a caller is rebuilding the function's block, however fast that is.
 */
#define SETTLE(round) (1000 << ((round) % 10))
#define NUM_CALLERS 2

typedef int (*fn_t)(int);

static unsigned char *code;
static atomic_int version; /* Odd while a patch is in progress. */
static atomic_int started, stop, stale;

#ifdef X64
static const unsigned char body[] = {
    0x48, 0x8d, 0x47, 0x00, /* lea rax, [rdi+imm8] */
    0xc3,                   /* ret */
};
#    define IMM_OFFS 3
#else
static const unsigned char body[] = {
    0x8b, 0x44, 0x24, 0x04, /* mov eax, [esp+4] */
    0x83, 0xc0, 0x00,       /* add eax, imm8 */
    0xc3,                   /* ret */
};
#    define IMM_OFFS 6
#endif

static int
expected(int v)
{
    return (v / 2) % 100;
}

static void *
caller(void *arg)
{
    fn_t fn = (fn_t)code;
    int counted = 0;
    while (!atomic_load(&stop)) {
        int v1 = atomic_load(&version);
        if (v1 % 2 != 0)
            continue;
        int res = fn(0);
        if (atomic_load(&version) == v1 && res != expected(v1))
            atomic_fetch_add(&stale, 1);
        if (!counted) {
            atomic_fetch_add(&started, 1);
            counted = 1;
        }
    }
    return NULL;
}

int
main(void)
{
    code = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) {
        print("mmap failed\n");
        return 1;
    }
    memcpy(code, body, sizeof(body));
    pthread_t threads[NUM_CALLERS];
    for (int i = 0; i < NUM_CALLERS; i++)
        pthread_create(&threads[i], NULL, caller, NULL);
    while (atomic_load(&started) < NUM_CALLERS) {
        /* Wait until every caller has run the code. */
    }
    for (int i = 1; i <= ROUNDS; i++) {
        atomic_store(&version, 2 * i - 1);
        code[IMM_OFFS] = (unsigned char)expected(2 * i);
        atomic_store(&version, 2 * i);
        for (volatile int j = 0; j < SETTLE(i); j++) {
            /* Let the callers run the patched code. */
        }
    }
    atomic_store(&stop, 1);
    for (int i = 0; i < NUM_CALLERS; i++)
        pthread_join(threads[i], NULL);
    if (atomic_load(&stale) != 0)
        print("%d calls ran stale code\n", atomic_load(&stale));
    else
        print("no call ran stale code\n");
    return 0;
}
