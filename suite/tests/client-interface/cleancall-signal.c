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

/* The application spins, without system calls, until its SIGUSR1 handler has
 * run.  The client sends that signal to the thread from inside a clean callee,
 * so it arrives while the thread executes client code, not code cache.  The
 * runtime must still deliver it before the thread runs on in the cache for
 * long: the loop's limit takes far longer than delivery should.
 */

#include "tools.h"

#include <signal.h>

#define LIMIT 20000000

static volatile int handled;
static volatile long iteration;
static volatile long handled_at;

static void
handler(int sig, siginfo_t *info, ucontext_t *ucxt)
{
    handled = 1;
    handled_at = iteration;
}

int
main(void)
{
    intercept_signal(SIGUSR1, handler, false);
    for (iteration = 0; iteration < LIMIT && !handled; iteration++) {
        /* No system call: only signal delivery can end this loop early. */
    }
    if (handled && handled_at < LIMIT / 2)
        print("signal handled while spinning\n");
    else
        print("signal not handled while spinning: %d at %ld\n", handled, handled_at);
    return 0;
}
