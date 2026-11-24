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

/* Replaces code that has run with other code by mapping a different page of a
 * file at the same address, with the same protection and the same sharing, as a
 * JIT that swaps in prepared pages does.  The new code must run: the fragments
 * built from the old page must not survive the mapping.
 */

#define _GNU_SOURCE 1 /* for memfd_create */
#include "tools.h"

#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define NUM_PAGES 4
#define ROUNDS 12

typedef int (*fn_t)(void);

/* Writes code that returns value. */
static void
write_code(byte *buf, int value)
{
    buf[0] = 0xb8; /* mov eax, imm32 */
    memcpy(buf + 1, &value, sizeof(value));
    buf[5] = 0xc3; /* ret */
}

int
main(void)
{
    int fd = memfd_create("mmap_fixed_code", 0);
    if (fd < 0 || ftruncate(fd, NUM_PAGES * PAGE_SIZE) != 0) {
        print("cannot create the file\n");
        return 1;
    }
    byte *file =
        mmap(NULL, NUM_PAGES * PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (file == MAP_FAILED) {
        print("cannot map the file\n");
        return 1;
    }
    /* Page i of the file returns i + 1. */
    for (int i = 0; i < NUM_PAGES; i++)
        write_code(file + i * PAGE_SIZE, i + 1);
    munmap(file, NUM_PAGES * PAGE_SIZE);

    /* The page we replace has executable neighbors that must keep working. */
    byte *code = mmap(NULL, 3 * PAGE_SIZE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) {
        print("cannot map the code\n");
        return 1;
    }
    write_code(code, 100);
    write_code(code + 2 * PAGE_SIZE, 200);
    if (mprotect(code, 3 * PAGE_SIZE, PROT_READ | PROT_EXEC) != 0) {
        print("cannot protect the code\n");
        return 1;
    }
    fn_t before = (fn_t)code;
    fn_t func = (fn_t)(code + PAGE_SIZE);
    fn_t after = (fn_t)(code + 2 * PAGE_SIZE);
    int wrong = 0;
    for (int round = 0; round < ROUNDS; round++) {
        int page = round % NUM_PAGES;
        if (mmap(code + PAGE_SIZE, PAGE_SIZE, PROT_READ | PROT_EXEC,
                 MAP_PRIVATE | MAP_FIXED, fd, page * PAGE_SIZE) != code + PAGE_SIZE) {
            print("cannot replace the code\n");
            return 1;
        }
        /* Run it more than once, as hot code is, and its neighbors too. */
        for (int i = 0; i < 3; i++) {
            if (func() != page + 1 || before() != 100 || after() != 200)
                wrong++;
        }
    }
    if (wrong != 0)
        print("%d rounds of calls ran the wrong code\n", wrong);
    else
        print("every call ran the mapped code\n");
    munmap(code, 3 * PAGE_SIZE);
    close(fd);
    return 0;
}
