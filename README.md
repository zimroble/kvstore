# kvstore

[![CI](https://github.com/zimroble/kvstore/actions/workflows/ci.yml/badge.svg)](https://github.com/zimroble/kvstore/actions/workflows/ci.yml)

A Redis-compatible in-memory key-value server written from scratch in **C++17**.
It speaks the real Redis wire protocol (RESP), so `redis-cli`, `redis-benchmark`
and existing Redis client libraries work against it unchanged.

```console
$ ./build/kvstore --aof data.aof --fsync always &
kvstore: listening on 127.0.0.1:6379
$ redis-cli SET session:42 alice EX 60
OK
$ redis-cli TTL session:42
(integer) 60
```

## Highlights

- **Single-threaded event loop on Linux `epoll`** with non-blocking sockets,
  per-connection buffers and full **request pipelining**.
- **Incremental RESP parser** that handles commands split across arbitrary TCP
  reads, binary-safe values, and inline commands.
- **Key expiry the way Redis does it**: lazy deletion on access, plus a
  background sampler (`10 Hz`) that picks random keys with TTLs from an O(1)
  sampling index and keeps going while more than 25% of a sample is stale.
- **Append-only-file persistence** with `always` / `everysec` / `no` fsync policies.
  - Write commands are flushed to the log **before** any reply is sent, so with
    `--fsync always` an acknowledged write is never lost.
  - Relative TTLs are logged as absolute deadlines, so replay is deterministic.
  - A torn final record after a crash is detected and truncated on startup.
  - `BGREWRITEAOF` compacts the log (write temp file → fsync → atomic rename → fsync dir).

## Benchmarks

`redis-benchmark`, 1M requests, 50 clients, 64-byte values, 100k-key keyspace.
Server pinned to one core of a 4-vCPU Intel Xeon @ 2.1 GHz cloud VM, with Redis
7.0.15 measured on the same machine for reference
([`bench/results.txt`](bench/results.txt), reproduce with `bench/run_bench.sh`):

| server                  | op  | pipeline | throughput    | p50     | p99     |
|-------------------------|-----|---------:|--------------:|--------:|--------:|
| kvstore                 | SET | 1        | 112,829 req/s | 0.215ms | 0.615ms |
| kvstore                 | GET | 1        | 105,141 req/s | 0.231ms | 0.623ms |
| kvstore                 | SET | 16       | 577,367 req/s | 0.903ms | 2.799ms |
| kvstore                 | GET | 16       | 968,992 req/s | 0.727ms | 1.591ms |
| kvstore + AOF everysec  | SET | 1        | 116,523 req/s | 0.215ms | 0.703ms |
| redis-server 7.0.15     | SET | 1        | 117,744 req/s | 0.215ms | 0.575ms |
| redis-server 7.0.15     | GET | 1        | 110,351 req/s | 0.223ms | 0.615ms |

Without pipelining both servers are bound by syscalls and network round-trips,
and kvstore lands within about 5–10% of Redis. Numbers vary by a few percent
between runs.

## Testing

| suite | what it covers |
|---|---|
| **40 GoogleTest unit tests** | RESP parsing (every byte-boundary split, malformed input), store semantics, a 50,000-step randomized model check against a reference map, active expiry, every command, AOF replay / torn-tail repair / corruption detection / rewrite, glob matching |
| **11 end-to-end tests** (`tests/integration_test.py`) | real TCP server: 10k-command pipelines, commands split over 7-byte writes, 4 MB binary values, 16 concurrent clients, background expiry, protocol errors, compatibility with the official `redis-cli` |
| **Crash test** | 20 trials, each killing the server with `SIGKILL` at a random moment under 4 concurrent writers and then checking every acknowledged write after restart. **0 lost out of ~36k acknowledged writes.** The test was validated by moving the AOF flush after the reply, which makes it fail on the first trial. |

CI runs everything twice: a Release build and an AddressSanitizer + UBSan build.

```bash
cmake -S . -B build && cmake --build build -j
(cd build && ctest --output-on-failure)
python3 tests/integration_test.py build/kvstore
```

## Supported commands

| group | commands |
|---|---|
| strings | `GET` `SET [EX\|PX\|EXAT\|PXAT\|KEEPTTL] [NX\|XX] [GET]` `SETNX` `MGET` `MSET` `INCR` `DECR` `INCRBY` `DECRBY` `APPEND` `STRLEN` |
| keys | `DEL` `UNLINK` `EXISTS` `KEYS pattern` `EXPIRE` `PEXPIRE` `EXPIREAT` `PEXPIREAT` `TTL` `PTTL` `PERSIST` |
| server | `PING` `ECHO` `DBSIZE` `INFO` `FLUSHALL` `FLUSHDB` `BGREWRITEAOF` `SELECT 0` `QUIT` |

## Running

```bash
# build (needs g++ ≥ 9 or clang ≥ 10, CMake ≥ 3.16; GoogleTest is fetched if not installed)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

./build/kvstore --port 6379 --aof appendonly.aof --fsync everysec
```

| flag | default | |
|---|---|---|
| `--port` | 6379 | |
| `--bind` | 127.0.0.1 | use `0.0.0.0` to listen on all interfaces |
| `--aof FILE` | off | enable persistence |
| `--fsync` | everysec | `always`, `everysec` or `no` |
| `--hz` | 10 | active-expiry cycles per second |

### Docker

```bash
docker build -t kvstore .
docker run -p 6379:6379 -v kvdata:/data kvstore
```

## Architecture

```
src/
  resp.{h,cpp}      RESP parser + encoders
  store.{h,cpp}     hash map + TTL index, lazy/active expiry
  commands.{h,cpp}  command dispatch, AOF logging, glob matcher
  aof.{h,cpp}       append-only file: buffer, fsync policy, replay, rewrite
  server.{h,cpp}    epoll event loop, connections, cron
  main.cpp          flags, signals
```

Each event-loop iteration:

1. `epoll_wait` (10 Hz timeout so the cron job runs while idle)
2. read every readable socket and execute all complete commands, collecting replies
3. **flush the AOF buffer**, plus `fdatasync` under `always`
4. write replies; register `EPOLLOUT` only for sockets whose send buffer filled
5. every 100 ms, run an active-expiry cycle

## Limitations

Strings only (no lists, hashes or sets yet), a single database, no replication,
no auth, and AOF rewrite is synchronous, so it blocks the loop on very large datasets.

## License

MIT
