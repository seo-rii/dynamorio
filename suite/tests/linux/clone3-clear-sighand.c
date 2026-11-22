/* **********************************************************
 * Copyright (c) 2026 Google, Inc.  All rights reserved.
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
 * ARE DISCLAIMED. IN NO EVENT SHALL VMWARE, INC. OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

/* clone3 with CLONE_CLEAR_SIGHAND (and CLONE_VM | CLONE_VFORK, as posix_spawn
 * uses) makes the kernel reset the child's handlers, including the runtime's
 * own.  The child must still run under the runtime, observe default
 * dispositions for caught signals and kept SIG_IGN ones, keep the parent's
 * blocked mask, and leave the parent's handlers untouched.
 */

#include "tools.h"

#include <errno.h>
#include <linux/sched.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SYS_clone3
#    define SYS_clone3 435
#endif
#ifndef CLONE_CLEAR_SIGHAND
#    define CLONE_CLEAR_SIGHAND 0x100000000ULL
#endif

/* Results written by the child into shared memory. */
typedef struct {
    int error;
    int complete;
} observation_t;

static observation_t *observed;
static uint64_t expected_mask;
static volatile int parent_handler_seen;

static void
parent_handler(int sig)
{
    parent_handler_seen = 1;
}

static int
process_mask(uint64_t *mask)
{
    return syscall(SYS_rt_sigprocmask, SIG_SETMASK, NULL, mask, sizeof(*mask));
}

/* sigaction() fills only the kernel-sized part of sa_mask: compare signals. */
static uint64_t
mask_bits(const sigset_t *mask)
{
    uint64_t bits = 0;
    for (int sig = 1; sig <= 64; sig++) {
        if (sigismember(mask, sig) == 1)
            bits |= 1ULL << (sig - 1);
    }
    return bits;
}

static bool
same_action(const struct sigaction *a, const struct sigaction *b)
{
    return a->sa_handler == b->sa_handler && a->sa_flags == b->sa_flags &&
        mask_bits(&a->sa_mask) == mask_bits(&b->sa_mask);
}

static bool
reset_action(const struct sigaction *act, void (*handler)(int))
{
    return act->sa_handler == handler && act->sa_flags == 0 &&
        mask_bits(&act->sa_mask) == 0;
}

__attribute__((used, noinline, noreturn)) void
clone_clear_child(int failed_exec)
{
    struct sigaction caught, ignored;
    uint64_t mask = 0;
    memset(&caught, 0, sizeof(caught));
    memset(&ignored, 0, sizeof(ignored));
    if (sigaction(SIGUSR1, NULL, &caught) != 0 ||
        sigaction(SIGUSR2, NULL, &ignored) != 0 || process_mask(&mask) != 0)
        observed->error = 1;
    else if (!reset_action(&caught, SIG_DFL) || !reset_action(&ignored, SIG_IGN))
        observed->error = 2;
    else if (mask != expected_mask)
        observed->error = 3;
    else if (raise(SIGUSR2) != 0) /* Ignored: must not reach the parent handler. */
        observed->error = 4;
    observed->complete = 1;
    if (observed->error != 0)
        _exit(80 + observed->error);
    if (failed_exec) {
        static char path[] = "/nonexistent/clone3-child";
        char *const argv[] = { path, NULL };
        char *const envp[] = { NULL };
        execve(argv[0], argv, envp);
        _exit(errno == ENOENT ? 127 : 84);
    }
    _exit(0);
}

/* The child starts on a new stack with no C frame, so it must not return
 * through a libc wrapper.  RDX survives the syscall and carries the mode.
 */
long
raw_clone3(struct clone_args *args, size_t size, int failed_exec);
__asm__(".text\n"
        ".globl raw_clone3\n"
        ".type raw_clone3,@function\n"
        "raw_clone3:\n"
        "mov $435, %eax\n"
        "syscall\n"
        "test %rax, %rax\n"
        "jnz 1f\n"
        "mov %edx, %edi\n"
        "and $-16, %rsp\n"
        "call clone_clear_child\n"
        "ud2\n"
        "1: ret\n"
        ".size raw_clone3, .-raw_clone3\n");

static void
run_mode(const char *name, int failed_exec)
{
    struct sigaction caught, ignored, caught_before, ignored_before, caught_after,
        ignored_after;
    memset(&caught, 0, sizeof(caught));
    memset(&ignored, 0, sizeof(ignored));
    caught.sa_handler = parent_handler;
    caught.sa_flags = SA_RESTART;
    sigemptyset(&caught.sa_mask);
    sigaddset(&caught.sa_mask, SIGALRM);
    ignored.sa_handler = SIG_IGN;
    ignored.sa_flags = SA_RESTART;
    sigemptyset(&ignored.sa_mask);
    sigaddset(&ignored.sa_mask, SIGTERM);
    sigaction(SIGUSR1, &caught, NULL);
    sigaction(SIGUSR2, &ignored, NULL);
    sigaction(SIGUSR1, NULL, &caught_before);
    sigaction(SIGUSR2, NULL, &ignored_before);
    memset(observed, 0, sizeof(*observed));
    parent_handler_seen = 0;

    const size_t stack_size = 64 * 1024;
    void *stack = mmap(NULL, stack_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    struct clone_args args;
    memset(&args, 0, sizeof(args));
    args.flags = CLONE_CLEAR_SIGHAND | CLONE_VM | CLONE_VFORK;
    args.exit_signal = SIGCHLD;
    args.stack = (uintptr_t)stack;
    args.stack_size = stack_size;
    long child = raw_clone3(&args, sizeof(args), failed_exec);
    if (child == -ENOSYS || child == -EINVAL) {
        /* Kernels before 5.5 lack clone3 or CLONE_CLEAR_SIGHAND. */
        print("%s: ok\n", name);
        munmap(stack, stack_size);
        return;
    }
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid((pid_t)child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    uint64_t parent_mask = 0;
    bool parent_ok = sigaction(SIGUSR1, NULL, &caught_after) == 0 &&
        sigaction(SIGUSR2, NULL, &ignored_after) == 0 &&
        process_mask(&parent_mask) == 0 && same_action(&caught_before, &caught_after) &&
        same_action(&ignored_before, &ignored_after) && parent_mask == expected_mask;
    if (raise(SIGUSR1) != 0 || raise(SIGUSR2) != 0 || !parent_handler_seen)
        parent_ok = false;
    int child_exit = (waited == child && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    if (parent_ok && observed->complete && observed->error == 0 &&
        child_exit == (failed_exec ? 127 : 0))
        print("%s: ok\n", name);
    else {
        print("%s: FAILED parent_ok=%d complete=%d error=%d exit=%d\n", name, parent_ok,
              observed->complete, observed->error, child_exit);
    }
    munmap(stack, stack_size);
}

int
main(void)
{
    sigset_t block;
    sigemptyset(&block);
    /* A harmless blocked signal checks mask inheritance. */
    sigaddset(&block, SIGWINCH);
    sigprocmask(SIG_BLOCK, &block, NULL);
    process_mask(&expected_mask);
    observed = mmap(NULL, sizeof(*observed), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    run_mode("vfork child", 0);
    run_mode("vfork child with failed execve", 1);
    return 0;
}
