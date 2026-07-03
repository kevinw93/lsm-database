# lsmkv

An LSM-tree key-value storage engine in C++20: write-ahead log, lock-free skiplist memtable, SSTables
with Bloom filters, leveled compaction, snapshots. ~6,850 lines including tests; no dependencies beyond
the standard library (GoogleTest and Google Benchmark for tests).

```cpp
std::unique_ptr<lsm::DB> db;
lsm::Options options;
options.create_if_missing = true;
lsm::Status s = lsm::DB::Open("/tmp/db", options, &db);

s = db->Put(lsm::WriteOptions(), "key", "value");

lsm::WriteBatch batch;
batch.Put("a", "1");
batch.Delete("b");
s = db->Write(lsm::WriteOptions(), batch);

const lsm::Snapshot* snap = db->GetSnapshot();
lsm::ReadOptions at_snap;
at_snap.snapshot = snap;
std::string value;
s = db->Get(at_snap, "a", &value);
auto it = db->NewIterator(at_snap);
for (it->SeekToFirst(); it->Valid(); it->Next()) { /* ... */ }
it.reset();
db->ReleaseSnapshot(snap);
```

## Build and test

```sh
cmake --workflow --preset asan      # ASan + UBSan
cmake --workflow --preset tsan      # ThreadSanitizer
cmake --workflow --preset release   # also builds benchmarks
./build/release/benchmarks/lsm_bench --num=1000000
```

`LSM_STRESS_SECONDS` (default 5) and `LSM_CRASH_ITERATIONS` (default 50) scale the stress and crash tests.
CI runs all presets on Ubuntu 24.04 with clang, with 60 s of TSan stress and 1,000 crash iterations.
`.devcontainer/` matches CI.

## Guarantees

- **Thread safety:** all `DB` methods may be called concurrently. Iterators and `WriteBatch` are single-threaded.
- **Atomicity:** a `WriteBatch` is applied entirely or not at all, for readers and across crashes. Later
  operations in a batch win.
- **Durability:** an acknowledged write survives a process crash. With `sync=true` it also survives an OS crash
  or power loss (`fdatasync`; `F_FULLFSYNC` on macOS).
- **Snapshots:** a read at a snapshot sees every write acknowledged before `GetSnapshot` and none that start
  after it. Iterators without a snapshot use an implicit one taken at creation.
- **Corruption:** every WAL record, table block, footer and manifest is checksummed. Corrupt data is never
  returned. Reads fail with `Corruption` instead of falling through to older data.
- **Recovery:** point-in-time. A torn or corrupt WAL tail is dropped and everything before it kept.
  A sequence-number gap across files fails `Open`.
- **Lifetimes:** iterators must be destroyed, and snapshots released, before the `DB`.

## Architecture

```
            Put / Delete / Write                         Get / NewIterator
                    │                                           │
          write_mutex_ (one writer)                  copy {mem, imm, Version, seq}
                    │                                  under a short mutex_
          ┌─────────▼─────────┐                                 │
          │  WAL  (fsync opt.) │                                 │
          └─────────┬─────────┘                                 │
          ┌─────────▼─────────┐  full   ┌──────────────┐        │
          │ memtable (skiplist)├───────►│ immutable mem │◄───────┤  lock-free reads
          └───────────────────┘          └──────┬───────┘        │
                                   background    │ flush          │
                                   thread        ▼                │
                         L0  [sst][sst][sst]   (overlapping) ◄────┤  newest first
                         L1  [sst][sst]        (disjoint, 4×wbs)  │
                         L2  [sst][sst][sst]…  (40×wbs)       ◄───┤  one candidate
                         L3  …                 (unbounded)    ◄───┘  file per level
```

| Directory | Contents |
|---|---|
| `include/lsm/` | Public API: `DB`, `Options`, `Iterator`, `WriteBatch`, `Status` |
| `src/db/` | DB implementation, WAL, memtable/skiplist, versions + manifest, iterators |
| `src/table/` | SSTable builder/reader, blocks, Bloom filter, two-level iterator |
| `src/util/` | Encoding, CRC32C, MurmurHash, arena, POSIX file layer |
| `tests/` | Unit, randomized model, stress, and crash tests |
| `benchmarks/` | `lsm_bench` (workloads), `coding_bench` (micro) |

## On-disk formats

```
Internal key  user_key | fixed64(seq << 8 | type)      type: 0 delete, 1 put
              order: user key ↑, seq ↓, type ↓

WAL record    masked_crc32c(4) | length(4) | payload    crc covers length + payload
  payload     fixed64 first_seq | count(4) | (type | lp key [| lp value])*

SSTable       data block* | filter block | index block | footer(40)
  block       entry* | fixed32 offset[n] | fixed32 n, then masked_crc32c(4)
  entry       varint klen | varint vlen | key | value
  index       last internal key of block i → fixed64 offset | fixed64 size
  filter      Bloom bits | k                (whole file, over user keys)
  footer      index handle | filter handle | magic

MANIFEST      one WAL-format record:
              next_file | log_number | last_sequence | per level: (number | size | smallest | largest)*
```

`lp` = varint length prefix. Fixed-width integers are little-endian; varints are LEB128. Stored CRCs are
masked (rotate + constant). The filter hash (MurmurHash64A) is part of the format and must not change.

## Design decisions

| Area | Choice | Tradeoff |
|---|---|---|
| Memtable | Skiplist, one writer, lock-free readers (acquire/release links), arena-allocated, nodes never freed individually | No memory reclamation scheme needed; concurrent inserts would need CAS |
| WAL | One record per batch, CRC + length | A corrupt record mid-file drops everything after it (prefix consistency) |
| Recovery | Never append to a replayed log; contiguous sequence numbers across files | A torn tail can't hide later records; gaps fail loudly |
| Tables | 4 KB blocks with an offset array; whole-file Bloom filter (10 bits/key, double hashing); index + filter pinned | ≤ 1 data-block read per table per lookup; no block cache or prefix compression |
| Manifest | Whole state as one record: temp → fsync → rename → dir fsync | Atomic and simple; rewrite cost grows with file count |
| Concurrency | `write_mutex_` serializes writers across WAL I/O; `mutex_` guards only pointer copies and swaps; one background thread | Readers never wait on fsync; no group commit |
| Versions | `shared_ptr` Version and memtables; compacted files unlinked immediately | Relies on POSIX unlink-while-open |
| Compaction | Leveled, L0–L3, 10× per level; a whole level merges into the overlapping next level | Simple picking; bursty latency and transient space |
| Backpressure | Stall on a busy immutable slot or ≥ 8 L0 files; flushes run between compaction outputs | Hard stop, no gradual slowdown |
| CRC32C | ARMv8 / SSE4.2 instructions, with a table fallback | ~20× faster than the table (8.65 vs 0.44 GiB/s) |

## Correctness testing

- **Fault injection:** WAL truncation at every byte offset plus 4,000 random bit-flip trials. Corrupt blocks,
  footers and manifests are detected.
- **Model tests:** random puts and deletes against `std::map` via `Get`, scans and `Seek`, at the latest state and at
  snapshots, across flushes, compactions and reopens.
- **Stress (`tests/stress_test.cc`):** 4 writers + 8 readers with a 16 KB write buffer. Without a shared lock,
  readers verify that batches are never torn, that reads fall between acknowledged and issued writes, and that scans are
  ordered. 60 s under TSan: ~350K writes and ~275K reads.
- **Crash (`tests/crash_test.cc`):** a forked child writes synced batches and acknowledges each over a pipe. The parent
  `kill -9`s it at random points (1,000 times, one DB) and verifies every acknowledged write and batch atomicity. Kills are
  confirmed to land mid-manifest-write, between table write and commit, and before WAL cleanup. This covers process crashes
  only: the page cache survives `kill -9`, so missing fsyncs can't be detected. Power-loss testing needs
  `dm-log-writes`, LazyFS or a VM power cut.

## Results

`lsm_bench --num=1000000`: 16-byte keys, 100-byte values, each key inserted once in random order. Apple M3 Pro,
macOS, APFS, release build, single run.

| Workload | ops/s | p50 | p99 | p99.9 |
|---|---:|---:|---:|---:|
| fillrandom, `sync=false` | 347,969 | 1.9 µs | 5.3 µs | 30.5 µs |
| fillrandom, `sync=true` (10K ops) | 361 | 2.98 ms | 3.96 ms | 7.20 ms |
| readrandom, present keys | 337,161 | 2.7 µs | 6.8 µs | 25.4 µs |
| readrandom, missing keys, Bloom 10 bits/key | 1,809,750 | 0.46 µs | 2.8 µs | 4.3 µs |
| readrandom, missing keys, no Bloom | 134,253 | 6.1 µs | 27.4 µs | 114.1 µs |
| readwhilewriting, 1 reader + 1 writer | 291,076 | 3.3 µs | 6.2 µs | 18.3 µs |
| readwhilewriting, 4 readers + 1 writer | 695,891 | 4.0 µs | 25.8 µs | 82.6 µs |

- **Missing keys:** 0.024 data-block reads per lookup with Bloom filters, vs 3.000 without (three files covered each key).
- **Write amplification after the fill:** 7.2× (WAL 1.2 + flush 1.1 + compaction 4.9), with data in L0–L2.
- **`sync=true`:** one device flush per write.

### Compared with LevelDB

LevelDB 1.23 `db_bench`, same machine and sizes, no compression on either side. LevelDB uses its default 8 MB
block cache and portable CRC. LevelDB skips block-checksum verification on reads by default; lsmkv verifies every
block. Mean µs per operation.

| Workload | LevelDB | lsmkv |
|---|---:|---:|
| fillrandom | 3.3 | 2.9 |
| readrandom, all keys present¹ | 1.39 | 2.97 |
| readrandom, missing keys, Bloom | 0.98 | 0.55 |
| readrandom, missing keys, no Bloom | 1.79 | 7.45 |
| readwhilewriting, 1 reader | 2.97 | 3.44 |
| readwhilewriting, 4 readers (aggregate reads/s) | ~990K | 696K |
| fillsync | 2,737 | 2,770 |

¹ LevelDB's `fillrandom` draws keys with replacement (63% exist), so its all-present figure is from `fillseq` + `readrandom`.

### Profile

readrandom, macOS `sample`, self time: `pread` 37%, CRC32C 16%, block search and decode ~17%, memtable seek ~8%.
A block cache would remove the first three on hits.

On Linux:

```sh
perf record -F 999 -g --call-graph dwarf -- ./build/release/benchmarks/lsm_bench --num=1000000
perf script | ./FlameGraph/stackcollapse-perf.pl | ./FlameGraph/flamegraph.pl > lsm.svg
```

## Next steps

1. Sharded LRU block cache.
2. Group commit: one WAL append and device flush for all queued writers.
3. File-at-a-time compaction with a per-level compact pointer.
4. Gradual write slowdown before the L0 stop.
5. Prefix compression with restart points.
6. Power-loss testing (`dm-log-writes`, LazyFS).
