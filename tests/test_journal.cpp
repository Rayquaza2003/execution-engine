#undef NDEBUG  // keep asserts on in Release builds
#include <cassert>
#include <cstdio>
#include <string>
#include <unistd.h>
#include <vector>

#include "journal/journal.hpp"

static std::string temp_path() {
    char p[] = "/tmp/journal_test_XXXXXX";
    int fd = mkstemp(p);
    close(fd);
    return p;
}

struct Rec {
    uint16_t type;
    Nanos t;
    std::string payload;
};

static std::vector<Rec> all(const std::string& path, size_t* valid = nullptr) {
    std::vector<Rec> v;
    size_t n = Journal::read(path, [&](uint16_t type, Nanos t, std::string_view p) { v.push_back({type, t, std::string(p)}); });
    if (valid) *valid = n;
    return v;
}

static void test_crc32_reference_value() {
    assert(crc32("123456789", 9) == 0xCBF43926u);  // the standard CRC-32 check value
}

static void test_append_flush_read_back() {
    std::string path = temp_path();
    struct Px {
        int64_t px;
        uint32_t qty;
    };
    {
        Journal j(path);
        j.append(1, 100, std::string_view("hello"));
        j.append(2, 200, Px{15025, 300});
        j.append(3, 300, std::string_view(""));
        assert(j.records() == 3);
    }  // destructor flushes
    std::vector<Rec> r = all(path);
    assert(r.size() == 3 && r[0].type == 1 && r[0].t == 100 && r[0].payload == "hello");
    Px px;
    std::memcpy(&px, r[1].payload.data(), sizeof px);
    assert(r[1].payload.size() == sizeof(Px) && px.px == 15025 && px.qty == 300 && r[2].payload.empty());
    unlink(path.c_str());
}

// A crash mid-write leaves part of a record: reading stops before it, and
// reopening cuts it off so new records follow the last good one.
static void test_torn_tail_is_cut_off() {
    std::string path = temp_path();
    {
        Journal j(path);
        j.append(1, 1, std::string_view("first"));
        j.append(1, 2, std::string_view("second"));
    }
    size_t good;
    all(path, &good);
    FILE* f = std::fopen(path.c_str(), "ab");
    std::fwrite("\x07\x00\x00\x00\x01\x00", 1, 6, f);  // a header cut short
    std::fclose(f);
    size_t valid;
    assert(all(path, &valid).size() == 2 && valid == good);
    {
        Journal j(path);
        assert(j.bytes() == good);
        j.append(1, 3, std::string_view("third"));
    }
    std::vector<Rec> r = all(path);
    assert(r.size() == 3 && r[2].payload == "third");
    unlink(path.c_str());
}

// A flipped bit fails the CRC: nothing from that record on is trusted.
static void test_corrupt_record_stops_reading() {
    std::string path = temp_path();
    {
        Journal j(path);
        for (int i = 0; i < 5; ++i) j.append(1, Nanos(i), std::string_view("payload"));
    }
    size_t rec = sizeof(Journal::Header) + 7 + 4;
    FILE* f = std::fopen(path.c_str(), "r+b");
    std::fseek(f, long(2 * rec + sizeof(Journal::Header) + 3), SEEK_SET);
    std::fputc('X', f);
    std::fclose(f);
    size_t valid;
    assert(all(path, &valid).size() == 2 && valid == 2 * rec);
    unlink(path.c_str());
}

int main() {
    test_crc32_reference_value();
    test_append_flush_read_back();
    test_torn_tail_is_cut_off();
    test_corrupt_record_stops_reading();
    std::puts("all tests passed");
}
