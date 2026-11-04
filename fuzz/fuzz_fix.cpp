// Fuzz target: FIX framing, parsing and a session fed arbitrary bytes.
// Invariants: frame() always makes progress and never claims more bytes than
// it was given; nothing crashes, overflows or loops, whatever arrives.
#include <cstdint>
#include <cstdlib>

#include "fix/session.hpp"

static void check(bool ok) {
    if (!ok) std::abort();
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t n) {
    std::string_view in(reinterpret_cast<const char*>(data), n);
    for (size_t at = 0; at < in.size();) {
        fix::Frame f = fix::frame(in.substr(at));
        if (f.status == fix::Frame::More) break;
        check(f.len > 0 && f.len <= in.size() - at);
        if (f.status == fix::Frame::Ok) {
            fix::Message m;
            if (m.parse(in.substr(at, f.len))) {
                (void)m.type();
                for (uint32_t t : {34u, 38u, 44u, 108u, 7u, 16u, 36u})
                    if (auto v = m.get(t)) (void)fix::to_price(*v), (void)fix::to_int(*v);
            }
        }
        at += f.len;
    }

    // An acceptor session (the gateway's side), bytes split at a point the input picks.
    constexpr Nanos t = 1'793'750'400'000'000'000;
    Session s({"EXCH", "", 30'000'000'000, false, "", 64});
    size_t cut = n ? data[0] % (n + 1) : 0;
    size_t apps = 0;
    auto app = [&](const fix::Message& m) { apps += !m.type().empty(); };
    s.on_bytes(in.data(), cut, t, app);
    s.on_bytes(in.data() + cut, n - cut, t + 1, app);
    s.on_timer(t + 100'000'000'000);
    check(s.next_in() >= 1 && s.next_out() >= 1);

    // Restoring from arbitrary journal records must not crash either.
    Session r({"EXCH", "", 30'000'000'000, false});
    r.restore(n && (data[0] & 1), in);
    return 0;
}
