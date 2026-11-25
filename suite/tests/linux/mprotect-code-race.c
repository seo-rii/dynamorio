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

/* Threads call a generated function while another thread keeps switching its
 * page between read-only and writable, as a JIT that patches under a W^X
 * discipline does, without changing the code.  Making the page writable removes
 * its executable area before the kernel changes the protection, and a caller that
 * executes the code in between adds the area back.  The area must not be added
 * as read-only code: the page is about to be writable, and its writes would go
 * unnoticed.  Debug builds check that no block is built from such an area while
 * the page is writable.
 */

#include "tools.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/mman.h>

#define ROUNDS 5000
#define NUM_CALLERS 3

typedef int (*fn_t)(int);

static unsigned char *code;
static atomic_int started, stop, wrong;

#ifdef X64
static const unsigned char body[] = {
    0x48, 0x8d, 0x47, 0x01, /* lea rax, [rdi+1] */
    0xc3,                   /* ret */
};
#else
static const unsigned char body[] = {
    0x8b, 0x44, 0x24, 0x04, /* mov eax, [esp+4] */
    0x40,                   /* inc eax */
    0xc3,                   /* ret */
};
#endif

static void *
caller(void *arg)
{
    fn_t fn = (fn_t)code;
    int n = 0;
    while (!atomic_load(&stop)) {
        if (fn(n & 0xffff) != (n & 0xffff) + 1)
            atomic_fetch_add(&wrong, 1);
        if (n++ == 0)
            atomic_fetch_add(&started, 1);
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
    if (mprotect(code, PAGE_SIZE, PROT_READ | PROT_EXEC) != 0) {
        print("mprotect failed\n");
        return 1;
    }
    pthread_t threads[NUM_CALLERS];
    for (int i = 0; i < NUM_CALLERS; i++)
        pthread_create(&threads[i], NULL, caller, NULL);
    while (atomic_load(&started) < NUM_CALLERS) {
        /* Wait until every caller has run the code. */
    }
    for (int i = 0; i < ROUNDS; i++) {
        if (mprotect(code, PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC) != 0 ||
            mprotect(code, PAGE_SIZE, PROT_READ | PROT_EXEC) != 0) {
            print("mprotect failed\n");
            return 1;
        }
        for (volatile int j = 0; j < 2000; j++) {
            /* Let the callers run the code between changes. */
        }
    }
    atomic_store(&stop, 1);
    for (int i = 0; i < NUM_CALLERS; i++)
        pthread_join(threads[i], NULL);
    if (atomic_load(&wrong) != 0)
        print("%d calls returned a wrong value\n", atomic_load(&wrong));
    else
        print("all calls returned the right value\n");
    return 0;
}
