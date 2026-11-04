#pragma once
#include <pthread.h>
#ifdef __linux__
#include <sched.h>
#endif

#include <cstdlib>
#include <string>
#include <vector>

// Thread placement for benchmarks. On Linux, pin a thread to one CPU (pair
// with isolcpus / nohz_full on the kernel command line for quiet cores). macOS
// can't pin, so the best available is the highest QoS class, which steers a
// thread to the performance cores.

// CPUs to pin to, from the environment: PIN_CPUS="2,3,4,5". Empty = don't pin.
inline std::vector<int> pin_cpus() {
    std::vector<int> v;
    const char* e = std::getenv("PIN_CPUS");
    for (const char* p = e; p && *p;) {
        char* end;
        long c = std::strtol(p, &end, 10);
        if (end == p) break;
        v.push_back(int(c));
        p = *end == ',' ? end + 1 : end;
    }
    return v;
}

// Pins the calling thread to cpus[i % size] if PIN_CPUS is set; true if pinned.
inline bool place_thread(size_t i) {
    static const std::vector<int> cpus = pin_cpus();
#ifdef __linux__
    if (cpus.empty()) return false;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpus[i % cpus.size()], &set);
    return pthread_setaffinity_np(pthread_self(), sizeof set, &set) == 0;
#elif defined(__APPLE__)
    (void)i;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    return false;
#else
    (void)i;
    return false;
#endif
}
