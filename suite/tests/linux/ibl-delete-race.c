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

/* A thread calls generated code through a register while another thread keeps
 * writing to the same page.  Each write flushes the page's fragments, which
 * removes the function from the indirect branch lookup tables, often while the
 * caller is on the lookup's hit path for it (the test runs with
 * -slowdown_ibl_found, which widens that path in builds with internal options).
 * The caller must still reach the function: a hit on an entry that is being
 * removed must exit to the function's tag, not to the removal marker.
 */

#include "tools.h"

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>

#define ROUNDS 200

typedef int (*fn_t)(int);

static unsigned char *code;
static atomic_int calls, stop;

static void
handler(int sig, siginfo_t *info, ucontext_t *ucxt)
{
    print("unexpected signal %d at %p\n", sig, info->si_addr);
    exit(1);
}

static void *
caller(void *arg)
{
    fn_t fn = (fn_t)code;
    int sum = 0;
    while (!atomic_load(&stop)) {
        sum += fn(sum & 0xff);
        atomic_fetch_add(&calls, 1);
    }
    return (void *)(ptr_int_t)sum;
}

int
main(void)
{
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
    code = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) {
        print("mmap failed\n");
        return 1;
    }
    memcpy(code, body, sizeof(body));
    intercept_signal(SIGSEGV, (handler_3_t)handler, false);
    pthread_t thread;
    pthread_create(&thread, NULL, caller, NULL);
    for (int i = 0; i < ROUNDS; i++) {
        int seen = atomic_load(&calls);
        while (atomic_load(&calls) - seen < 2) {
            /* Let the caller run the code again, so that it is in the tables. */
        }
        /* Modify another part of the code page, as a JIT patching a call. */
        code[PAGE_SIZE / 2] ^= 1;
    }
    atomic_store(&stop, 1);
    pthread_join(thread, NULL);
    print("all calls reached the function\n");
    return 0;
}
