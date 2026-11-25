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

/* Test close_range emulation: a range ending at UINT_MAX (as passed by glibc's
 * closefrom() and posix_spawn()) must finish promptly, application descriptors
 * in the range must be closed or marked close-on-exec, and failures must report
 * a positive errno through the usual -1 return.
 */

#include "tools.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_close_range
#    define SYS_close_range 436
#endif
#ifndef CLOSE_RANGE_CLOEXEC
#    define CLOSE_RANGE_CLOEXEC (1U << 2)
#endif

static long
do_close_range(unsigned int first, unsigned int last, unsigned int flags)
{
    return syscall(SYS_close_range, first, last, flags);
}

static bool
is_open(int fd)
{
    return fcntl(fd, F_GETFD) != -1;
}

int
main(void)
{
    int fds[4];
    for (int i = 0; i < 4; i++)
        fds[i] = open("/dev/null", O_RDONLY);
    if (do_close_range(fds[0], fds[0], 0) != 0 && errno == ENOSYS) {
        /* Kernels before 5.9: nothing to test. */
        print("close_range done\n");
        return 0;
    }
    if (is_open(fds[0]))
        print("ERROR: single descriptor still open\n");

    /* Invalid arguments must fail with a positive errno. */
    errno = 0;
    long res = do_close_range(fds[2], fds[1], 0);
    if (res != -1 || errno != EINVAL)
        print("ERROR: reversed range returned %ld errno %d\n", res, errno);
    errno = 0;
    res = do_close_range(fds[1], fds[3], 0x80000000U);
    if (res != -1 || errno != EINVAL)
        print("ERROR: invalid flags returned %ld errno %d\n", res, errno);
    if (!is_open(fds[1]) || !is_open(fds[3]))
        print("ERROR: a failed call closed descriptors\n");

    /* Marking a range close-on-exec keeps it open. */
    if (do_close_range(fds[1], UINT_MAX, CLOSE_RANGE_CLOEXEC) == 0) {
        for (int i = 1; i < 4; i++) {
            int flags = fcntl(fds[i], F_GETFD);
            if (flags == -1 || !(flags & FD_CLOEXEC))
                print("ERROR: descriptor %d not marked close-on-exec\n", i);
        }
    } else if (errno != EINVAL) {
        /* CLOSE_RANGE_CLOEXEC needs Linux 5.11. */
        print("ERROR: CLOSE_RANGE_CLOEXEC failed with errno %d\n", errno);
    }

    /* The whole remaining descriptor space, as closefrom(3) does. */
    if (do_close_range(fds[1], UINT_MAX, 0) != 0)
        print("ERROR: close_range to UINT_MAX failed with errno %d\n", errno);
    for (int i = 1; i < 4; i++) {
        if (is_open(fds[i]))
            print("ERROR: descriptor %d still open\n", i);
    }
    /* The runtime must still be able to write. */
    print("close_range done\n");
    return 0;
}
