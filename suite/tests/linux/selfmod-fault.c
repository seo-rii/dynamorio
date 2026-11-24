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

/* A thread faults, as at a safepoint poll of a JIT, in a block of generated
 * code that another thread's write to the same page has just flushed.  The
 * write also turns the page into a self-modifying region (the test runs with
 * -ro2sandbox_threshold 1), so the flushed block cannot be rebuilt from the
 * page as it is now: its translation must not depend on that.  Each fault must
 * be reported at the poll.
 */

#include "tools.h"

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define ROUNDS 200
/* Long enough that the other thread flushes and arms while this one copies. */
#define COPY_BYTES (16 * 1024 * 1024)

typedef void (*copy_fn)(void *dst, const void *src, size_t n, void *poll);

static unsigned char *code, *poll_page, *poll_pc;
static atomic_int armed, faults, wrong_pc, stop;

static void
handler(int sig, siginfo_t *info, ucontext_t *ucxt)
{
    unsigned char *pc = (unsigned char *)ucxt->uc_mcontext.gregs[REG_RIP];
    if ((unsigned char *)info->si_addr != poll_page) {
        print("unexpected fault at %p\n", info->si_addr);
        exit(1);
    }
    if (pc != poll_pc) {
        atomic_fetch_add(&wrong_pc, 1);
        /* Resume after the poll so that the test can go on. */
        ucxt->uc_mcontext.gregs[REG_RIP] = (greg_t)(poll_pc + 3);
    }
    atomic_fetch_add(&faults, 1);
    /* Disarm: the poll executes again. */
    mprotect(poll_page, PAGE_SIZE, PROT_READ | PROT_WRITE);
    atomic_store(&armed, 0);
}

static void *
patcher(void *arg)
{
    unsigned int seed = 1;
    while (!atomic_load(&stop)) {
        struct timespec pause = { 0, 20000 + rand_r(&seed) % 80000 };
        nanosleep(&pause, NULL);
        /* Modify another part of the code page, as a JIT patching a call. */
        code[PAGE_SIZE / 2] ^= 1;
        atomic_store(&armed, 1);
        mprotect(poll_page, PAGE_SIZE, PROT_NONE);
        while (atomic_load(&armed) && !atomic_load(&stop)) {
            /* Wait for the poll to fault. */
        }
    }
    return NULL;
}

int
main(void)
{
    static const unsigned char body[] = {
        0x49, 0x89, 0xc8, /* mov r8, rcx */
        0x48, 0x89, 0xd1, /* mov rcx, rdx */
        0xf3, 0xa4,       /* rep movsb */
        0x41, 0x85, 0x00, /* test [r8], eax: the poll */
        0xc3,             /* ret */
    };
    code = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    poll_page =
        mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED || poll_page == MAP_FAILED) {
        print("mmap failed\n");
        return 1;
    }
    memcpy(code, body, sizeof(body));
    poll_pc = code + 8;
    intercept_signal(SIGSEGV, (handler_3_t)handler, false);
    unsigned char *src = calloc(1, COPY_BYTES), *dst = calloc(1, COPY_BYTES);
    pthread_t thread;
    pthread_create(&thread, NULL, patcher, NULL);
    copy_fn copy = (copy_fn)code;
    while (atomic_load(&faults) < ROUNDS)
        copy(dst, src, COPY_BYTES, poll_page);
    atomic_store(&stop, 1);
    pthread_join(thread, NULL);
    free(src);
    free(dst);
    if (atomic_load(&wrong_pc) == 0)
        print("all faults at the poll\n");
    else
        print("%d of %d faults not at the poll\n", atomic_load(&wrong_pc), ROUNDS);
    return 0;
}
