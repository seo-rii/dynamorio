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

/* Inserts a clean call at the start of every block.  Once one block has run
 * FIRE times, the callee sends SIGUSR1 to its own thread: the kernel delivers
 * it on return from the system call, inside the callee.  With "out_of_line" the
 * clean call saves its context out of line.
 */

#include "dr_api.h"
#include "client_tools.h"

#include <signal.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define FIRE 200000
#define SLOTS (1 << 16)

static unsigned int counts[SLOTS];
static volatile int fired;

static void
callee(unsigned int *count)
{
    if (++*count == FIRE && !fired) {
        fired = 1;
        syscall(SYS_tgkill, dr_get_process_id(),
                dr_get_thread_id(dr_get_current_drcontext()), SIGUSR1);
    }
}

static bool out_of_line;

static dr_emit_flags_t
event_bb(void *drcontext, void *tag, instrlist_t *bb, bool for_trace, bool translating)
{
    unsigned int *count = &counts[((ptr_uint_t)tag >> 2) & (SLOTS - 1)];
    dr_insert_clean_call_ex(drcontext, bb, instrlist_first_app(bb), (void *)callee,
                            out_of_line ? DR_CLEANCALL_ALWAYS_OUT_OF_LINE : 0, 1,
                            OPND_CREATE_INTPTR(count));
    return DR_EMIT_DEFAULT;
}

DR_EXPORT void
dr_client_main(client_id_t id, int argc, const char *argv[])
{
    out_of_line = argc > 1 && strcmp(argv[1], "out_of_line") == 0;
    dr_register_bb_event(event_bb);
}
