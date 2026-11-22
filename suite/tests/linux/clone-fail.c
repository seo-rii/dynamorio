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

/* A clone syscall that the kernel rejects creates no thread.  Runtime state
 * reserved for the would-be child (its clone record and stack, and the count
 * of threads still initializing) must be released.  Otherwise repeated
 * failures leak memory and process exit waits for threads that never start,
 * so the failures run in a child process whose exit the parent checks.
 */

#include "tools.h"

#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NUM_ATTEMPTS 1000
#define EXIT_WAIT_SECONDS 30

static void
fail_clones(void)
{
    int unexpected = 0;
    for (int i = 0; i < NUM_ATTEMPTS; i++) {
        /* CLONE_THREAD requires CLONE_SIGHAND, so the kernel returns EINVAL
         * before creating anything.  No child stack is needed.
         */
        long res = syscall(SYS_clone, CLONE_VM | CLONE_THREAD, NULL, NULL, NULL, 0);
        if (res != -1 || errno != EINVAL)
            unexpected++;
    }
    print("%d failed clones, %d unexpected results\n", NUM_ATTEMPTS, unexpected);
}

int
main(void)
{
    pid_t child = fork();
    if (child == 0) {
        fail_clones();
        exit(0);
    }
    struct timespec pause = { 0, 50 * 1000 * 1000 };
    int status;
    for (int i = 0; i < EXIT_WAIT_SECONDS * 20; i++) {
        pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child) {
            print("child exited with status %d\n",
                  WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            return 0;
        }
        nanosleep(&pause, NULL);
    }
    kill(child, SIGKILL);
    waitpid(child, &status, 0);
    print("child did not exit\n");
    return 0;
}
