#!/usr/bin/env bash
# Every benchmark that needs no market data, with the machine described first,
# so a result file says where it came from. Built in Release.
#
# Usage: scripts/bench_linux.sh [BUILD_DIR]        (default: build)
#   PIN_CPUS=2,3,4,5 pins the threaded benches' threads to those CPUs.
#
# For numbers worth quoting, use a dedicated Linux box: boot with
# isolcpus=2-5 nohz_full=2-5 rcu_nocbs=2-5, set the governor to performance,
# disable turbo/SMT if you can, then run with PIN_CPUS=2,3,4,5. A shared cloud
# VM (like a CI runner) gives honest Linux/x86 numbers but noisy tails.
set -euo pipefail
build=${1:-build}
echo "== machine"
uname -srm
if command -v lscpu >/dev/null; then lscpu | grep -E "Model name|^CPU\(s\)|Thread|MHz|L1d|L2|L3" || true; fi
[ -r /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ] && echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
[ -r /sys/devices/system/cpu/isolated ] && echo "isolated cpus: $(cat /sys/devices/system/cpu/isolated)"
echo "PIN_CPUS=${PIN_CPUS:-<unset>}"
"${CXX:-c++}" --version | head -1
echo

echo "== order book (5 runs)"
for i in 1 2 3 4 5; do "$build/bench_book" | grep -E "throughput|checksum|allocations" | tr '\n' ' '; echo; done
"$build/bench_book" | grep -A7 "latency ns"
echo
echo "== pre-trade risk";      "$build/bench_risk"
echo "== market data";         "$build/bench_md"
echo "== SPSC vs mutex";       "$build/bench_spsc" 10000000
echo "== pipeline (synthetic)"; "$build/bench_pipeline"
echo "== TCP loopback";        "$build/bench_tcp"
echo "== journal";             "$build/bench_journal" /tmp
