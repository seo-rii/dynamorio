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

/* Unit tests for the online (pipe) trace reader. */

#include "test_helpers.h"
#include "reader/ipc_reader.h"
#include "common/named_pipe.h"
#include "common/trace_entry.h"

#include <assert.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <iostream>
#include <string>
#include <vector>

namespace dynamorio {
namespace drmemtrace {
namespace {

// Exposes the raw entries the reader returns, so the test can look past the
// end-of-stream footer without the reader's own processing of those entries.
class raw_ipc_reader_t : public ipc_reader_t {
public:
    raw_ipc_reader_t(const char *name)
        : ipc_reader_t(name, 0)
    {
    }
    trace_entry_t *
    next_raw()
    {
        return read_next_entry();
    }
};

trace_entry_t
make_entry(trace_type_t type, unsigned short size, addr_t addr)
{
    trace_entry_t entry;
    entry.type = static_cast<unsigned short>(type);
    entry.size = size;
    entry.addr = addr;
    return entry;
}

// The writer closes the pipe after one write. Once the reader has returned
// every entry of that write, it reports the end with a footer and then has
// nothing more to return: it must not hand back what is left in its buffer.
bool
test_no_entries_after_eof()
{
    const std::string name =
        "drmemtrace_ipc_reader_test." + std::to_string(static_cast<long>(getpid()));
    const addr_t tid = 42, pid = 43, pc = 0x1000;
    trace_entry_t bundle = make_entry(TRACE_TYPE_INSTR_BUNDLE, 1, 0);
    bundle.length[0] = 3;
    const std::vector<trace_entry_t> written = {
        make_entry(TRACE_TYPE_HEADER, 0, TRACE_ENTRY_VERSION),
        make_entry(TRACE_TYPE_THREAD, sizeof(addr_t), tid),
        make_entry(TRACE_TYPE_PID, sizeof(addr_t), pid),
        make_entry(TRACE_TYPE_INSTR, 4, pc),
        bundle,
        make_entry(TRACE_TYPE_INSTR, 2, pc + 7),
        make_entry(TRACE_TYPE_THREAD_EXIT, sizeof(addr_t), tid),
    };
    raw_ipc_reader_t reader(name.c_str());
    if (!reader) {
        std::cerr << "failed to create the pipe\n";
        return false;
    }
    pid_t child = fork();
    if (child == 0) {
        named_pipe_t writer(name.c_str());
        if (!writer.open_for_write())
            _exit(1);
        ssize_t size = static_cast<ssize_t>(written.size() * sizeof(written[0]));
        if (writer.write(written.data(), size) != size)
            _exit(1);
        writer.close();
        _exit(0);
    }
    assert(child > 0);
    // Starts the iteration, which consumes entries up to the first instruction.
    if (!reader.init()) {
        std::cerr << "failed to open the pipe for reading\n";
        return false;
    }
    std::vector<trace_entry_t> read;
    trace_entry_t *entry;
    while ((entry = reader.next_raw()) != nullptr) {
        read.push_back(*entry);
        if (read.size() > 2 * written.size())
            break;
    }
    int status;
    waitpid(child, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::cerr << "the writer failed\n";
        return false;
    }
    // The entries after the first instruction, then the footer, then nothing.
    std::vector<trace_entry_t> expected(written.begin() + 4, written.end());
    expected.push_back(make_entry(TRACE_TYPE_FOOTER, 0, 0));
    if (read.size() != expected.size()) {
        std::cerr << "read " << read.size() << " entries after the first instruction, "
                  << "expected " << expected.size() << "\n";
        for (const trace_entry_t &e : read) {
            std::cerr << "  " << trace_type_names[e.type] << " size=" << e.size
                      << " addr=0x" << std::hex << e.addr << std::dec << "\n";
        }
        return false;
    }
    for (size_t i = 0; i < expected.size(); ++i) {
        if (read[i].type != expected[i].type || read[i].size != expected[i].size ||
            read[i].addr != expected[i].addr) {
            std::cerr << "entry " << i << " is " << trace_type_names[read[i].type]
                      << ", expected " << trace_type_names[expected[i].type] << "\n";
            return false;
        }
    }
    return true;
}

} // namespace

int
test_main(int argc, const char *argv[])
{
    if (!test_no_entries_after_eof())
        return 1;
    std::cerr << "all done\n";
    return 0;
}

} // namespace drmemtrace
} // namespace dynamorio
