#pragma once
#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>

#include "core/types.hpp"

// Append-only binary journal of input events: everything needed to rebuild
// state by replaying it. A record is
//
//   u32 payload length | u16 type | u16 0 | u64 time | payload | u32 CRC32
//
// with the CRC over header and payload. A crash can leave a half-written
// record at the end; reading stops at the first record that is short or
// fails its CRC, and opening the journal for append cuts that tail off.
//
// Durability is the caller's choice per batch: flush() hands the bytes to
// the OS (survives the process dying), sync() also forces them to the disk
// (survives power loss; F_FULLFSYNC on macOS, where plain fsync doesn't
// flush the drive's cache).
inline uint32_t crc32(const void* data, size_t n, uint32_t crc = 0) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    auto* p = static_cast<const uint8_t*>(data);
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

class Journal {
public:
    struct Header {
        uint32_t len;
        uint16_t type, pad;
        uint64_t time;
    };
    static_assert(sizeof(Header) == 16);

    // Opens path for append, creating it; a torn tail from a crash is cut off first.
    explicit Journal(const std::string& path) {
        size_t valid = read(path, [](uint16_t, Nanos, std::string_view) {});
        fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT, 0644);
        if (fd_ < 0 || ftruncate(fd_, off_t(valid)) != 0 || lseek(fd_, 0, SEEK_END) < 0) {
            std::perror(path.c_str());
            std::abort();
        }
        bytes_ = valid;
    }
    ~Journal() {
        flush();
        ::close(fd_);
    }
    Journal(const Journal&) = delete;
    Journal& operator=(const Journal&) = delete;

    void append(uint16_t type, Nanos t, std::string_view payload) {
        Header h{uint32_t(payload.size()), type, 0, t};
        uint32_t crc = crc32(payload.data(), payload.size(), crc32(&h, sizeof h));
        buf_.append(reinterpret_cast<const char*>(&h), sizeof h);
        buf_.append(payload);
        buf_.append(reinterpret_cast<const char*>(&crc), sizeof crc);
        ++records_;
    }
    template <class T>
    void append(uint16_t type, Nanos t, const T& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        append(type, t, std::string_view(reinterpret_cast<const char*>(&v), sizeof v));
    }

    // Hands everything appended so far to the OS.
    void flush() {
        size_t at = 0;
        while (at < buf_.size()) {
            ssize_t n = ::write(fd_, buf_.data() + at, buf_.size() - at);
            if (n <= 0) {
                std::perror("journal write");
                std::abort();  // can't promise durability any more: stop rather than lie
            }
            at += size_t(n);
        }
        bytes_ += buf_.size();
        buf_.clear();
    }
    // flush(), then wait until the data is on the disk.
    void sync() {
        flush();
#ifdef __APPLE__
        if (fcntl(fd_, F_FULLFSYNC) == 0) return;
#endif
        fsync(fd_);
    }

    size_t bytes() const { return bytes_ + buf_.size(); }
    uint64_t records() const { return records_; }  // appended through this object

    // Calls f(type, time, payload) for each intact record from the start;
    // returns the length of the intact prefix.
    template <class F>
    static size_t read(const std::string& path, F&& f) {
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) return 0;
        std::string data;
        char chunk[1 << 16];
        for (ssize_t n; (n = ::read(fd, chunk, sizeof chunk)) > 0;) data.append(chunk, size_t(n));
        ::close(fd);
        size_t at = 0;
        while (data.size() - at >= sizeof(Header) + 4) {
            Header h;
            std::memcpy(&h, data.data() + at, sizeof h);
            size_t end = at + sizeof h + h.len;
            if (h.pad != 0 || end + 4 > data.size()) break;
            uint32_t crc;
            std::memcpy(&crc, data.data() + end, 4);
            if (crc != crc32(data.data() + at, sizeof h + h.len)) break;
            f(h.type, Nanos(h.time), std::string_view(data).substr(at + sizeof h, h.len));
            at = end + 4;
        }
        return at;
    }

private:
    int fd_ = -1;
    std::string buf_;
    size_t bytes_ = 0;
    uint64_t records_ = 0;
};
