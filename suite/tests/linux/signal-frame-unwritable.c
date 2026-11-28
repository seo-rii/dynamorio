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

/* Delivers signals whose frame cannot be written to the stack.  The kernel then
 * delivers SIGSEGV instead, and when the frame for SIGSEGV itself cannot be
 * written it resets the handler and kills the process with SIGSEGV
 * (force_sigsegv()).  Each case runs in a child, whose termination the parent
 * prints.
 */

#define _GNU_SOURCE 1
#include "tools.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define STACK_SIZE (64 * 1024)
#define DEADLINE_MS 20000

static volatile int sink, depth = -1;

static void
exit_42(int sig)
{
    _exit(42);
}

static void
exit_43(int sig)
{
    _exit(43);
}

static void
exit_44(int sig)
{
    _exit(44);
}

static void
handle(int sig, void (*handler)(int), bool altstack)
{
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = handler;
    if (altstack) {
        stack_t ss;
        ss.ss_sp = malloc(STACK_SIZE);
        ss.ss_size = STACK_SIZE;
        ss.ss_flags = 0;
        if (ss.ss_sp == NULL || sigaltstack(&ss, NULL) != 0)
            _exit(3);
        act.sa_flags = SA_ONSTACK;
    }
    if (sigaction(sig, &act, NULL) != 0)
        _exit(3);
}

/* Stops only at a depth no stack reaches, so the compiler sees a base case. */
static int
recurse(int n)
{
    volatile char pad[256];
    pad[n & 255] = (char)n;
    if (n == depth)
        return 0;
    return recurse(n + 1) + pad[3];
}

static void
overflow(void)
{
    sink = recurse(0);
}

/* Recurses on a small stack whose guard page is just below it, so the fault and
 * the frame for its handler land on the guard page.
 */
static void
run_overflow(void)
{
    static ucontext_t main_uc, uc;
    byte *region = mmap(NULL, PAGE_SIZE + STACK_SIZE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region == MAP_FAILED || mprotect(region, PAGE_SIZE, PROT_NONE) != 0 ||
        getcontext(&uc) != 0)
        _exit(3);
    uc.uc_stack.ss_sp = region + PAGE_SIZE;
    uc.uc_stack.ss_size = STACK_SIZE;
    uc.uc_link = &main_uc;
    makecontext(&uc, overflow, 0);
    swapcontext(&main_uc, &uc);
}

static void
run_case(const char *name)
{
    struct rlimit no_core = { 0, 0 };
    setrlimit(RLIMIT_CORE, &no_core);
    if (strstr(name, "-alt") != NULL)
        handle(SIGSEGV, exit_43, true);
    if (strstr(name, "-segv") != NULL)
        handle(SIGSEGV, exit_42, false);
    if (strncmp(name, "overflow", 8) == 0) {
        if (strstr(name, "-") == NULL)
            handle(SIGSEGV, exit_42, false);
        run_overflow();
    } else if (strncmp(name, "segv", 4) == 0) {
        /* A fault with an unwritable stack pointer and a handler on that stack.
         * The frame address wraps around below 16.
         */
        handle(SIGSEGV, exit_42, false);
        __asm__ __volatile__("mov $16, %%rsp\n\t"
                             "mov 0, %%eax"
                             :
                             :
                             : "eax", "memory");
    } else if (strncmp(name, "ill", 3) == 0) {
        handle(SIGILL, exit_44, false);
        __asm__ __volatile__("mov $16, %%rsp\n\tud2" ::: "memory");
    } else if (strncmp(name, "usr1", 4) == 0) {
        /* A signal that arrives at a system call with an unwritable stack pointer. */
        handle(SIGUSR1, exit_44, false);
        long pid = getpid(), tid = syscall(SYS_gettid);
        __asm__ __volatile__("mov $16, %%rsp\n\tsyscall\n\tud2"
                             :
                             : "a"((long)SYS_tgkill), "D"(pid), "S"(tid),
                               "d"((long)SIGUSR1)
                             : "rcx", "r11", "memory");
    }
    _exit(2);
}

static void
report(const char *name)
{
    pid_t child = fork();
    if (child < 0) {
        print("%s: fork failed\n", name);
        return;
    }
    if (child == 0)
        run_case(name);
    int status = 0;
    struct timespec pause = { 0, 10 * 1000 * 1000 };
    for (int waited = 0; waitpid(child, &status, WNOHANG) == 0; waited += 10) {
        if (waited >= DEADLINE_MS) {
            kill(child, SIGKILL);
            waitpid(child, &status, 0);
            print("%s: no result within the deadline\n", name);
            return;
        }
        nanosleep(&pause, NULL);
    }
    if (WIFSIGNALED(status))
        print("%s: killed by signal %d\n", name, WTERMSIG(status));
    else
        print("%s: exit %d\n", name, WEXITSTATUS(status));
}

int
main(void)
{
    /* The handler sharing the overflowed stack cannot run: SIGSEGV is fatal. */
    report("overflow");
    /* On an alternate stack it runs. */
    report("overflow-alt");
    /* The same for a fault with an unwritable stack pointer. */
    report("segv");
    /* Another synchronous signal becomes SIGSEGV, which is fatal without a
     * handler that can run, or runs a handler on an alternate stack.
     */
    report("ill");
    report("ill-alt");
    /* The same for a signal delivered later, at a system call. */
    report("usr1");
    report("usr1-alt");
    report("usr1-segv");
    print("all done\n");
    return 0;
}
