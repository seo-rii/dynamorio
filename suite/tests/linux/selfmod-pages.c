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

/* Generated code on a writable and executable mapping of several pages, as a
 * JIT's code cache.  The test runs with -page_granular_writable_code, where a
 * write invalidates only the code of the pages it writes.  Every call must run
 * the code as last written:
 * - functions on separate pages, after rewriting one of them;
 * - a block that continues from one page into the next, after a write to the
 *   second page;
 * - an instruction whose immediate straddles two pages, after a write to the
 *   part on the second page;
 * - a function rewritten again and again, until its page is sandboxed;
 * - a function that stores to its own page;
 * - new code written to a page that had not run code yet.
 */

#include "tools.h"

#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGES 9

typedef int (*fn_t)(void);

static unsigned char *code;
static size_t page;

static int
call_at(size_t offs)
{
    return ((fn_t)(code + offs))();
}

/* mov eax, imm32; ret */
static void
emit_mov_ret(size_t offs, int value)
{
    code[offs] = 0xb8;
    memcpy(code + offs + 1, &value, sizeof(value));
    code[offs + 5] = 0xc3;
}

static void
set_imm32(size_t offs, int value)
{
    memcpy(code + offs + 1, &value, sizeof(value));
}

static void
check_result(const char *what, int got, int want)
{
    if (got != want)
        print("%s: got %d, want %d\n", what, got, want);
}

int
main(void)
{
    page = (size_t)sysconf(_SC_PAGESIZE);
    code = mmap(NULL, PAGES * page, PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) {
        print("mmap failed\n");
        return 1;
    }
    memset(code, 0xcc, PAGES * page);

    /* Pages 0-2: one function each. */
    for (int i = 0; i < 3; i++)
        emit_mov_ret(i * page, 10 * i);
    /* Pages 3-4: a block that crosses into page 4.
     *   xor eax, eax; add eax, 1; add eax, 2 | add eax, 4; ret
     */
    size_t span = 4 * page - 8;
    static const unsigned char span_code[] = {
        0x31, 0xc0,       /* xor eax, eax */
        0x90,             /* nop */
        0x83, 0xc0, 0x01, /* add eax, 1 */
        0x83, 0xc0, 0x02, /* add eax, 2 (ends 1 byte before page 4) */
        0x90,             /* nop: the last byte of page 3 */
        0x83, 0xc0, 0x04, /* add eax, 4 (on page 4) */
        0xc3,             /* ret */
    };
    memcpy(code + span - 2, span_code, sizeof(span_code));
    span -= 2;
    size_t span_imm = span + sizeof(span_code) - 2; /* the 4 on page 4 */
    /* Pages 5-6: mov eax, imm32 whose last two immediate bytes are on page 6. */
    size_t straddle = 6 * page - 3;
    emit_mov_ret(straddle, 0x00010001);
    /* Page 7: a function that stores to its own page, then reads it back.
     *   mov dword [rip + data], imm32; mov eax, [rip + data]; ret
     */
    size_t self = 7 * page, self_data = 7 * page + page / 2;
    {
        unsigned char *p = code + self;
        int disp;
        *p++ = 0xc7; /* mov r/m32, imm32 */
        *p++ = 0x05; /* rip-relative */
        disp = (int)(self_data - (self + 10));
        memcpy(p, &disp, 4);
        p += 4;
        int imm = 70;
        memcpy(p, &imm, 4);
        p += 4;
        *p++ = 0x8b; /* mov r32, r/m32 */
        *p++ = 0x05;
        disp = (int)(self_data - (self + 16));
        memcpy(p, &disp, 4);
        p += 4;
        *p++ = 0xc3;
    }
    size_t self_imm = self + 6;

    check_result("page 0", call_at(0), 0);
    check_result("page 1", call_at(page), 10);
    check_result("page 2", call_at(2 * page), 20);
    check_result("span", call_at(span), 7);
    check_result("straddle", call_at(straddle), 0x00010001);
    check_result("self", call_at(self), 70);

    /* Rewrite the function on page 1: the others keep their results. */
    set_imm32(page, 11);
    check_result("page 1 rewritten", call_at(page), 11);
    check_result("page 0 after page 1", call_at(0), 0);
    check_result("page 2 after page 1", call_at(2 * page), 20);

    /* Write the second page of the spanning block. */
    code[span_imm] = 0x08;
    check_result("span rewritten", call_at(span), 11);
    /* And the first page. */
    code[span + 5] = 0x03;
    check_result("span rewritten first page", call_at(span), 13);

    /* Write the immediate bytes on page 6 only, then those on page 5 only. */
    code[straddle + 3] = 0x02;
    check_result("straddle second page", call_at(straddle), 0x00020001);
    code[straddle + 1] = 0x05;
    check_result("straddle first page", call_at(straddle), 0x00020005);

    /* Rewrite page 2 many times, executing in between: the page goes to
     * sandboxing after -ro2sandbox_threshold write faults and keeps returning
     * the latest value.
     */
    for (int i = 0; i < 64; i++) {
        set_imm32(2 * page, 100 + i);
        check_result("page 2 repeated", call_at(2 * page), 100 + i);
        check_result("page 0 during repeats", call_at(0), 0);
    }

    /* The function that writes its own page, then that function rewritten. */
    check_result("self again", call_at(self), 70);
    {
        int imm = 71;
        memcpy(code + self_imm, &imm, 4);
    }
    check_result("self rewritten", call_at(self), 71);

    /* New code on a page that has not run code: page 8. */
    emit_mov_ret(8 * page, 80);
    check_result("page 8", call_at(8 * page), 80);
    set_imm32(8 * page, 81);
    check_result("page 8 rewritten", call_at(8 * page), 81);

    /* Everything once more. */
    check_result("page 0 final", call_at(0), 0);
    check_result("page 1 final", call_at(page), 11);
    check_result("page 2 final", call_at(2 * page), 163);
    check_result("span final", call_at(span), 13);
    check_result("straddle final", call_at(straddle), 0x00020005);
    check_result("self final", call_at(self), 71);

    munmap(code, PAGES * page);
    print("all done\n");
    return 0;
}
