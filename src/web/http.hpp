#pragma once
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_map>

// The least HTTP/1.1 a local test bench needs: one request per connection,
// Content-Length bodies, form-encoded parameters, JSON out.
namespace web {

struct Request {
    std::string method, path, query, body;
};

constexpr size_t kMaxRequest = 64 * 1024;

// Bytes the first complete request takes; 0 = need more; npos = not HTTP (drop it).
inline size_t parse(std::string_view buf, Request& r) {
    size_t head = buf.find("\r\n\r\n");
    if (head == buf.npos) return buf.size() > kMaxRequest ? std::string_view::npos : 0;
    size_t sp1 = buf.find(' '), sp2 = sp1 == buf.npos ? buf.npos : buf.find(' ', sp1 + 1);
    if (sp2 == buf.npos || sp2 > head) return std::string_view::npos;
    r.method = std::string(buf.substr(0, sp1));
    std::string_view target = buf.substr(sp1 + 1, sp2 - sp1 - 1);
    size_t q = target.find('?');
    r.path = std::string(target.substr(0, q));
    r.query = q == target.npos ? "" : std::string(target.substr(q + 1));
    size_t len = 0;
    std::string_view headers = buf.substr(0, head);
    for (size_t at = headers.find("\r\n"); at != headers.npos; at = headers.find("\r\n", at + 2)) {
        std::string_view line = headers.substr(at + 2, headers.find("\r\n", at + 2) - at - 2);
        constexpr std::string_view kLen = "content-length:";
        if (line.size() > kLen.size()) {
            std::string lower(line.substr(0, kLen.size()));
            for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (lower == kLen) len = std::strtoul(std::string(line.substr(kLen.size())).c_str(), nullptr, 10);
        }
    }
    if (len > kMaxRequest) return std::string_view::npos;
    if (buf.size() < head + 4 + len) return 0;
    r.body = std::string(buf.substr(head + 4, len));
    return head + 4 + len;
}

// a=1&b=x%20y+z -> {a: "1", b: "x y z"}
inline std::unordered_map<std::string, std::string> form(std::string_view s) {
    auto decode = [](std::string_view v) {
        std::string out;
        for (size_t i = 0; i < v.size(); ++i) {
            if (v[i] == '+') out += ' ';
            else if (v[i] == '%' && i + 2 < v.size()) {
                out += char(std::strtol(std::string(v.substr(i + 1, 2)).c_str(), nullptr, 16));
                i += 2;
            } else out += v[i];
        }
        return out;
    };
    std::unordered_map<std::string, std::string> m;
    while (!s.empty()) {
        size_t amp = s.find('&');
        std::string_view kv = s.substr(0, amp);
        size_t eq = kv.find('=');
        m[decode(kv.substr(0, eq))] = eq == kv.npos ? "" : decode(kv.substr(eq + 1));
        if (amp == s.npos) break;
        s.remove_prefix(amp + 1);
    }
    return m;
}

inline std::string response(int code, std::string_view type, std::string_view body) {
    const char* text = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 404 ? "Not Found" : "Error";
    char head[256];
    std::snprintf(head, sizeof head,
                  "HTTP/1.1 %d %s\r\nContent-Type: %.*s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
                  "Connection: close\r\n\r\n",
                  code, text, int(type.size()), type.data(), body.size());
    return std::string(head) + std::string(body);
}

// A JSON string literal, quotes included.
inline std::string quote(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\', out += c;
        else if (static_cast<unsigned char>(c) < 0x20) {
            char b[8];
            std::snprintf(b, sizeof b, "\\u%04x", c);
            out += b;
        } else out += c;
    }
    return out + "\"";
}

}  // namespace web
