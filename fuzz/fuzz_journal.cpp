// Fuzz target: recovering a journal from an arbitrary file.
// Invariants: reading never runs past the file; reopening cuts it back to the
// intact prefix; a record appended afterwards reads back after that prefix.
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "journal/journal.hpp"

static void check(bool ok) {
    if (!ok) std::abort();
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t n) {
    static const std::string path = "/tmp/fuzz_journal_" + std::to_string(getpid());
    FILE* f = std::fopen(path.c_str(), "wb");
    if (n) std::fwrite(data, 1, n, f);
    std::fclose(f);

    size_t records = 0;
    size_t valid = Journal::read(path, [&](uint16_t, Nanos, std::string_view p) {
        check(p.size() <= n);
        ++records;
    });
    check(valid <= n);
    {
        Journal j(path);
        check(j.bytes() == valid);
        j.append(7, 42, std::string_view("after"));
    }
    size_t again = 0;
    std::string last;
    Journal::read(path, [&](uint16_t, Nanos, std::string_view p) {
        ++again;
        last = std::string(p);
    });
    check(again == records + 1 && last == "after");
    unlink(path.c_str());
    return 0;
}
