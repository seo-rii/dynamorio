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

/* Linux suspends only the thread that calls vfork.  Several application
 * threads can therefore have vfork children at the same time, and one parent
 * must not reclaim another parent's child while that child is still between
 * vfork and execve.
 */

#include "tools.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define NUM_THREADS 4
#define NUM_ROUNDS 12

static const char *exec_path;
static int null_fd;
static pthread_barrier_t barrier;

static void *
spawner(void *arg)
{
    long failures = 0;
    for (int i = 0; i < NUM_ROUNDS; i++) {
        pthread_barrier_wait(&barrier);
        pid_t child = vfork();
        if (child == 0) {
            const char *argv[] = { exec_path, "/fake/path/it_worked", NULL };
            /* Only raw syscalls before execve: the parent's memory is shared. */
            dup2(null_fd, 1);
            dup2(null_fd, 2);
            execve(exec_path, (char **)argv, NULL);
            _exit(127);
        }
        int status;
        pid_t waited;
        do {
            waited = waitpid(child, &status, 0);
        } while (waited < 0 && errno == EINTR);
        if (child < 0 || waited != child || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0)
            failures++;
    }
    return (void *)failures;
}

int
main(int argc, char **argv)
{
    pthread_t threads[NUM_THREADS];
    long failures = 0;
    if (argc < 2) {
        print("usage: %s <exec-target>\n", argv[0]);
        return 1;
    }
    exec_path = argv[1];
    null_fd = open("/dev/null", O_WRONLY);
    pthread_barrier_init(&barrier, NULL, NUM_THREADS);
    for (int i = 0; i < NUM_THREADS; i++)
        pthread_create(&threads[i], NULL, spawner, NULL);
    for (int i = 0; i < NUM_THREADS; i++) {
        void *result;
        pthread_join(threads[i], &result);
        failures += (long)result;
    }
    pthread_barrier_destroy(&barrier);
    print("%d concurrent vforks, %ld failures\n", NUM_THREADS * NUM_ROUNDS, failures);
    return 0;
}
