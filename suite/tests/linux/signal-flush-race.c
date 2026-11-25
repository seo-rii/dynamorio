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
 * page, as a JIT does, while other threads call it and are sent signals.  Each
 * patch flushes the function's fragments, often while a caller is inside one, and
 * a signal that arrives in a fragment unlinks it so that the caller leaves it
 * soon.  The caller has to be able to finish that fragment and to leave it:
 * unlinking it must not free it, and the signal code must not keep a pointer to
 * it once the flush frees it.
 */

#include "tools.h"

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define ROUNDS 3000
/* How long the patching thread waits between patches varies, so that patches
 * land at different points of the callers' work.
 */
#define SETTLE(round) (1000 << ((round) % 8))
#define NUM_CALLERS 2
#define NUM_FUNCS 4

typedef int (*fn_t)(int);

static unsigned char *code;
static atomic_int started, stop, signals;

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
#define FUNC_SPACING 64

static void
handler(int sig)
{
    atomic_fetch_add(&signals, 1);
}

static void *
caller(void *arg)
{
    int counted = 0;
    int sum = 0;
    while (!atomic_load(&stop)) {
        for (int i = 0; i < NUM_FUNCS; i++)
            sum += ((fn_t)(code + i * FUNC_SPACING))(i);
        if (!counted) {
            atomic_fetch_add(&started, 1);
            counted = 1;
        }
    }
    return (void *)(ptr_int_t)sum;
}

static void *
signaler(void *arg)
{
    pthread_t *threads = (pthread_t *)arg;
    for (int i = 0; !atomic_load(&stop); i = (i + 1) % NUM_CALLERS) {
        pthread_kill(threads[i], SIGUSR1);
        usleep(200);
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
    for (int i = 0; i < NUM_FUNCS; i++)
        memcpy(code + i * FUNC_SPACING, body, sizeof(body));
    intercept_signal(SIGUSR1, (handler_3_t)handler, false);
    pthread_t threads[NUM_CALLERS], signal_thread;
    for (int i = 0; i < NUM_CALLERS; i++)
        pthread_create(&threads[i], NULL, caller, NULL);
    while (atomic_load(&started) < NUM_CALLERS) {
        /* Wait until every caller has run the code. */
    }
    pthread_create(&signal_thread, NULL, signaler, threads);
    for (int i = 1; i <= ROUNDS; i++) {
        code[(i % NUM_FUNCS) * FUNC_SPACING + IMM_OFFS] = (unsigned char)(i % 100);
        for (volatile int j = 0; j < SETTLE(i); j++) {
            /* Let the callers run the patched code. */
        }
    }
    atomic_store(&stop, 1);
    for (int i = 0; i < NUM_CALLERS; i++)
        pthread_join(threads[i], NULL);
    pthread_join(signal_thread, NULL);
    if (atomic_load(&signals) == 0)
        print("no signal was delivered\n");
    else
        print("all done\n");
    return 0;
}
