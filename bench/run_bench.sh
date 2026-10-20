#!/usr/bin/env bash
# Benchmarks kvstore (and, if installed, the real redis-server for reference)
# with the official redis-benchmark tool. The server is pinned to CPU 0 and
# the load generator to the remaining CPUs.
#
#   bench/run_bench.sh [path/to/kvstore]
set -euo pipefail

BIN=${1:-build/kvstore}
N=${N:-1000000}
CLIENTS=${CLIENTS:-50}
PORT_KV=7380
PORT_REDIS=7381
LOADGEN_CPUS="1-$(( $(nproc) - 1 ))"

bench() {  # port label pipeline
    taskset -c "$LOADGEN_CPUS" redis-benchmark -p "$1" -n "$N" -c "$CLIENTS" -P "$3" \
        -t set,get -r 100000 -d 64 --csv 2>/dev/null |
        tr -d '"' |
        awk -F, -v label="$2" -v p="$3" 'NR > 1 {
            printf "%-22s %-4s pipeline=%-3s %10.0f req/s   p50=%sms  p99=%sms\n", label, $1, p, $2, $5, $7 }'
}

wait_port() { for _ in $(seq 50); do redis-cli -p "$1" ping >/dev/null 2>&1 && return; sleep 0.1; done; }

echo "machine: $(nproc) CPUs, $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | xargs)"
echo "requests=$N clients=$CLIENTS value=64B keyspace=100k"
echo

taskset -c 0 "$BIN" --port $PORT_KV 2>/dev/null &
KV_PID=$!
trap 'kill $KV_PID 2>/dev/null || true; [ -n "${REDIS_PID:-}" ] && kill $REDIS_PID 2>/dev/null || true' EXIT
wait_port $PORT_KV
bench $PORT_KV kvstore 1
bench $PORT_KV kvstore 16
kill $KV_PID; wait $KV_PID 2>/dev/null || true

AOF_DIR=$(mktemp -d)
taskset -c 0 "$BIN" --port $PORT_KV --aof "$AOF_DIR/bench.aof" --fsync everysec 2>/dev/null &
KV_PID=$!
wait_port $PORT_KV
bench $PORT_KV "kvstore+aof(everysec)" 1
kill $KV_PID; wait $KV_PID 2>/dev/null || true
rm -rf "$AOF_DIR"

if command -v redis-server >/dev/null; then
    taskset -c 0 redis-server --port $PORT_REDIS --save '' --appendonly no --io-threads 1 >/dev/null &
    REDIS_PID=$!
    wait_port $PORT_REDIS
    bench $PORT_REDIS redis-server 1
    bench $PORT_REDIS redis-server 16
fi
