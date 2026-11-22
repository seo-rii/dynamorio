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

/* The x87 status word in a signal frame must be the application's.  The
 * runtime saves FP state itself when it delivers a signal that arrived during a
 * syscall.  On hosts whose kernel signal frames carry extended XSAVE state it
 * used to overwrite the saved legacy area after FNCLEX had cleared the live
 * exception flags, losing the precision (inexact) flag.
 */

#include "tools.h"

#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#define X87_PRECISION 0x20  /* PE: set by rounding 1.5 to an integer. */
#define X87_TOP_MASK 0x3800 /* One value pushed: TOP == 7. */

static volatile int handled;
static volatile uint16_t frame_status;
static uint16_t live_status;
static int rounded;
static const double one_half = 1.5;

static void
handler(int sig, siginfo_t *info, void *raw)
{
    ucontext_t *uc = (ucontext_t *)raw;
    /* Only integer accesses here, so the compiler emits no x87 code. */
    frame_status = uc->uc_mcontext.fpregs->swd;
    handled++;
}

int
main(void)
{
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_sigaction = handler;
    act.sa_flags = SA_SIGINFO;
    sigemptyset(&act.sa_mask);
    sigaction(SIGUSR1, &act, NULL);
    long pid = getpid(), tid = syscall(SYS_gettid);
    /* Set the precision flag and leave 1.5 on the stack, then signal this
     * thread with no x87 instruction in between.  Store the status word that
     * sigreturn restores, then empty the stack.
     */
    __asm__ __volatile__("fninit\n\t"
                         "fldl %[half]\n\t"
                         "fistl %[rounded]\n\t"
                         "mov %[nr], %%eax\n\t"
                         "syscall\n\t"
                         "fnstsw %[live]\n\t"
                         "fstp %%st(0)\n\t"
                         : [rounded] "=m"(rounded), [live] "=m"(live_status)
                         : [half] "m"(one_half), [nr] "i"(SYS_tgkill), "D"(pid), "S"(tid),
                           "d"(SIGUSR1)
                         : "rax", "rcx", "r11", "memory");
    print("handled %d signal(s), rounded %d\n", handled, rounded);
    print("frame status: precision %s, top %s\n",
          (frame_status & X87_PRECISION) ? "set" : "lost",
          (frame_status & X87_TOP_MASK) == X87_TOP_MASK ? "7" : "wrong");
    print("status after return: precision %s\n",
          (live_status & X87_PRECISION) ? "set" : "lost");
    return 0;
}
