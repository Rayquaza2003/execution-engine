// Runs a fuzz target without libFuzzer, so every build (and every sanitizer)
// exercises it under ctest: each seed in the corpus directory as is, then
// many deterministic mutations of them (bit flips, inserts, deletes,
// truncations, splices). For coverage-guided fuzzing build with -DFUZZ=ON.
// Usage: fuzz_<name>_smoke CORPUS_DIR [iterations]
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t n);

int main(int argc, char** argv) {
    std::vector<std::string> seeds;
    if (argc > 1)
        for (const auto& e : std::filesystem::directory_iterator(argv[1])) {
            std::ifstream in(e.path(), std::ios::binary);
            seeds.emplace_back(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
    if (seeds.empty()) seeds.emplace_back();
    int iterations = argc > 2 ? std::atoi(argv[2]) : 5000;
    auto run = [](const std::string& s) { LLVMFuzzerTestOneInput(reinterpret_cast<const uint8_t*>(s.data()), s.size()); };
    for (const std::string& s : seeds) run(s);

    std::mt19937_64 rng(1);
    for (int i = 0; i < iterations; ++i) {
        std::string s = seeds[rng() % seeds.size()];
        for (int m = 0, edits = 1 + int(rng() % 8); m < edits; ++m) {
            size_t at = s.empty() ? 0 : rng() % s.size();
            switch (rng() % 6) {
                case 0: if (!s.empty()) s[at] = char(s[at] ^ (1 << (rng() % 8))); break;
                case 1: s.insert(at, 1, char(rng())); break;
                case 2: if (!s.empty()) s.erase(at, 1 + rng() % 4); break;
                case 3: s.resize(at); break;
                case 4: s += seeds[rng() % seeds.size()]; break;
                case 5: if (!s.empty()) s[at] = "0123456789=\x01-.\xff"[rng() % 15]; break;
            }
        }
        run(s);
    }
    std::printf("%zu seeds, %d mutated inputs: no crash\n", seeds.size(), iterations);
}
