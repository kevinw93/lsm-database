# lsmkv

An LSM-tree key-value storage engine in C++20: a write-ahead log, a lock-free skiplist memtable,
SSTables with Bloom filters, leveled compaction, and snapshot reads. It's built to be correct
under concurrency and crashes. Every change is tested under AddressSanitizer + UBSan and
ThreadSanitizer, with a 60-second multithreaded stress test and a 1,000-iteration `kill -9` crash harness.

~6,800 lines of C++ including tests. No dependencies beyond the standard library (GoogleTest and
Google Benchmark for tests).

```cpp
std::unique_ptr<lsm::DB> db;
lsm::Options options;
options.create_if_missing = true;
lsm::Status s = lsm::DB::Open("/tmp/db", options, &db);

s = db->Put(lsm::WriteOptions(), "key", "value");

lsm::WriteBatch batch;                       // applied atomically
batch.Put("a", "1");
batch.Delete("b");
s = db->Write(lsm::WriteOptions(), batch);

const lsm::Snapshot* snap = db->GetSnapshot();
lsm::ReadOptions at_snap;
at_snap.snapshot = snap;
std::string value;
s = db->Get(at_snap, "a", &value);           // consistent point-in-time read
auto it = db->NewIterator(at_snap);
for (it->SeekToFirst(); it->Valid(); it->Next()) { /* ... */ }
it.reset();
db->ReleaseSnapshot(snap);
```

## Build and test

```sh
cmake --workflow --preset asan      # ASan + UBSan
cmake --workflow --preset tsan      # ThreadSanitizer
cmake --workflow --preset release   # optimized; also builds benchmarks
./build/release/benchmarks/lsm_bench --num=1000000
```

`LSM_STRESS_SECONDS` (default 5) and `LSM_CRASH_ITERATIONS` (default 50) scale the stress and
crash tests. CI (`.github/workflows/ci.yml`, Ubuntu 24.04, clang) runs every preset on every push,
with a 60-second TSan stress run and 1,000 crash iterations in release. A dev container matching CI is in
`.devcontainer/`.

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
| `tests/` | Unit tests, randomized model tests, stress test, crash harness |
| `benchmarks/` | `lsm_bench` (workloads) and `coding_bench` (micro) |

## On-disk formats

```
Internal key  user_key | fixed64(seq << 8 | type)      type: 0 delete, 1 put
              order: user key ↑, seq ↓, type ↓  (newest version first)

WAL record    masked_crc32c(4) | length(4) | payload    crc covers length + payload
  payload     fixed64 first_seq | count(4) | (type | lp key [| lp value])*

SSTable       data block* | filter block | index block | footer(40)
  block       entry* | fixed32 offset[n] | fixed32 n, then masked_crc32c(4)
  entry       varint klen | varint vlen | key | value
  index       last internal key of block i → fixed64 offset | fixed64 size
  filter      Bloom bits | k            (whole-file, over user keys)
  footer      index handle | filter handle | magic

MANIFEST      one WAL-format record:
              next_file | log_number | last_sequence | per level: (number | size | smallest | largest)*
```

`lp` = varint length prefix. Integers are little-endian; lengths are LEB128 varints, and decoders reject
truncated or overflowing input. Stored CRCs are masked (rotate + constant) because a CRC computed over data that
contains CRCs is weak. CRC32C uses the CPU's instructions when available (ARMv8, x86 SSE4.2).

## Design decisions

| Area | Choice | Why | Cost / what I'd change at scale |
|---|---|---|---|
| Memtable | Skiplist, one writer + lock-free readers, acquire/release links, arena-allocated | Readers never block; nodes are never freed individually, so no hazard pointers or epochs | Concurrent inserts need CAS (RocksDB's concurrent memtable) |
| WAL | One record per batch, CRC + length, no 32 KB block fragmentation | Simplest safe framing; point-in-time recovery | A torn record in the middle of a file loses everything after it, by design (prefix consistency) |
| Recovery | Never append to a replayed log; contiguous sequence numbers enforced across logs | A torn tail would hide later records; gaps fail loudly instead of becoming silent data loss | — |
| Sync | Every write reaches the kernel; `sync=true` adds `fdatasync` (`F_FULLFSYNC` on macOS) | Survives `kill -9` always, power loss when synced | Group commit to batch fsyncs |
| Tables | 4 KB blocks with an offset array (binary search), whole-file Bloom filter (10 bits/key, double hashing), index + filter pinned | ≤ 1 data-block read per table per lookup; ~0.8% false positives | Prefix compression; partitioned filters; **block cache** |
| Manifest | Whole state as one record; temp → fsync → rename → dir fsync | Atomic, durable, no version-edit log to replay | Version-edit log once there are thousands of files |
| Concurrency | `write_mutex_` for writers (held across WAL I/O); `mutex_` only for pointer copies and swaps; one background thread | Readers never wait on fsync; the background thread never needs the writer lock, so no deadlock | — |
| Versions | `shared_ptr` Version / memtables; compacted files unlinked immediately | Readers and iterators pin what they read; POSIX keeps unlinked files readable while open | Deferred deletion on non-POSIX systems |
| Snapshots | A snapshot is a sequence number, tracked in a multiset | Compaction keeps any version the oldest snapshot can see | — |
| Compaction | Leveled, L0–L3, 10× per level; a whole level merges into the overlapping next level; outputs split between user keys | Simple picking with no per-level state; asymptotic write amplification the same as LevelDB | Bursty latency and transient space; LevelDB's file-at-a-time with a compact pointer |
| Backpressure | Stall on a busy immutable slot or ≥ 8 L0 files; flushes run between compaction outputs | Bounded memory and read amplification; writers don't wait for a whole compaction | Gradual slowdown before the hard stop |

## Correctness testing

- **Unit + fault injection:** WAL truncated at every byte offset and 4,000 random bit-flip trials; corrupted
  table blocks, footers and manifests are all detected and never returned as data.
- **Randomized model tests:** random puts and deletes compared against `std::map` through `Get`, full scans and
  `Seek`, at the latest state and at many snapshots, across flushes, compactions and reopens.
- **Stress (`tests/stress_test.cc`):** 4 writers + 8 readers (Gets, snapshots, iterators) with a 16 KB write buffer.
  Readers check, without a shared lock, that batches are never torn, that reads are bounded by acknowledged
  and issued writes, and that scans are ordered. Each 60 s TSan run is ~350K writes and ~275K reads, clean.
  Injected bugs (publishing the sequence number before the memtable insert, clearing the immutable memtable
  outside the version-swap critical section, deleting in-progress compaction outputs) are each caught.
- **Crash (`tests/crash_test.cc`):** a forked child writes synced batches and acknowledges them over a pipe; the parent
  `kill -9`s it at random points, 1,000 times against one DB, and verifies every acknowledged write and batch
  atomicity. In a 1,000-kill release run (before macOS syncs switched to `F_FULLFSYNC`), kills landed
  mid-manifest-write (180), with uncommitted tables on disk (657), and before WAL cleanup (175); with full syncs, 200 kills
  still hit all three windows (28 / 132 / 50).
  Injected ordering bugs (writing the manifest in place, deleting files before commit) are caught within 15 iterations.
  This proves crash safety for process crashes, **not power loss**: after `kill -9` the page cache survives, so missing
  fsyncs are invisible. That needs `dm-log-writes`, LazyFS or a VM power cut.

## Results

`lsm_bench --num=1000000`: 16-byte keys, 100-byte values, keys inserted once in random order. Apple M3 Pro
(12 cores), macOS, APFS, release build. One run, with the DB opened fresh per workload group.

| Workload | ops/s | p50 | p99 | p99.9 |
|---|---:|---:|---:|---:|
| fillrandom, `sync=false` | 347,969 | 1.9 µs | 5.3 µs | 30.5 µs |
| fillrandom, `sync=true` (10K ops) | 361 | 2.98 ms | 3.96 ms | 7.20 ms |
| readrandom, present keys | 337,161 | 2.7 µs | 6.8 µs | 25.4 µs |
| readrandom, missing keys, Bloom 10 bits/key | 1,809,750 | 0.46 µs | 2.8 µs | 4.3 µs |
| readrandom, missing keys, no Bloom | 134,253 | 6.1 µs | 27.4 µs | 114.1 µs |
| readwhilewriting, 1 reader + 1 writer | 291,076 | 3.3 µs | 6.2 µs | 18.3 µs |
| readwhilewriting, 4 readers + 1 writer | 695,891 | 4.0 µs | 25.8 µs | 82.6 µs |

**What the numbers show:**
- **Bloom filters.** A missing key costs **0.024 data-block reads with filters vs 3.000 without**. At the
  end of this fill, two L0 files and one L2 file covered each key, and 3 × 0.8% matches the false-positive rate. That's
  **13.5×** the throughput.
- **Lock-free reads scale.** 4 readers get 2.4× the throughput of 1 while a writer runs. Readers only take a mutex for a
  few pointer copies.
- **Write amplification after fillrandom: 7.2×** (WAL 1.2 + flush 1.1 + compaction 4.9), with data in L0–L2. The ~25×
  estimate is for data that has reached L3 in steady state; a single 116 MB fill doesn't get there.
- **`sync=true` costs one real device flush per write (~3 ms).** On macOS that's `fcntl(F_FULLFSYNC)`, because plain `fsync`
  doesn't flush the drive's cache. That's the case for group commit.

### Compared with LevelDB

LevelDB 1.23 `db_bench` on the same machine, same sizes, no compression on either side. LevelDB uses its default
8 MB block cache and its portable software CRC (no crc32c library installed). Mean µs per operation; lower is better.

| Workload | LevelDB | lsmkv | |
|---|---:|---:|---|
| fillrandom | 3.3 | 2.9 | on par |
| readrandom, all keys present¹ | 1.39 | 2.97 | 2.1× slower: no block cache, every read verifies its CRC |
| readrandom, missing keys, Bloom | 0.98 | 0.55 | 1.8× faster: whole-file filter checked before the index |
| readrandom, missing keys, no Bloom | 1.79 | 7.45 | 4× slower: 3 block reads per lookup, none cached |
| readwhilewriting, 1 reader | 2.97 | 3.44 | on par |
| readwhilewriting, 4 readers (aggregate reads/s) | ~990K | 696K | 0.7×: no block cache |
| fillsync | 2,737 | 2,770 | on par: both pay one `F_FULLFSYNC` |

¹ LevelDB's `fillrandom` draws keys with replacement, so only 63% exist; its all-present figure is from `fillseq` + `readrandom`.

**How the gap was closed.** The first version was **4–10× slower on reads**. A profile showed the byte-at-a-time
software CRC32C at ~80% of read CPU (~9 µs to verify each 4 KB block), and LevelDB doesn't verify block checksums on
reads by default (`verify_checksums = false`). Disabling verification in an experiment matched LevelDB, confirming the
diagnosis. Rather than drop the check, I switched to the CPU's CRC32C instructions (ARMv8 `crc32cx`, x86 SSE4.2 `crc32q`
with runtime detection), which took checksum throughput from 0.44 to 8.65 GiB/s (≈20×) and read throughput from 75K to
337K ops/s, **while still verifying every block read**. The same comparison exposed a durability bug: `sync=true`
used plain `fsync` on macOS, which isn't durable there. It now uses `F_FULLFSYNC`, the same as LevelDB.

### Profile

macOS `sample` (self time, idle background thread excluded) before the CRC change:

| Phase | Top frames | Reading |
|---|---|---|
| readrandom | `crc32c::Extend` ≈ 80% of busy CPU, `pread` ≈ 10% | Software CRC (~440 MB/s) on every block read was most of the 12 µs p50 → fixed with hardware CRC32C |
| fillrandom | `crc32c::Extend`, `write` (one syscall per `Put`), `SkipList::Insert` | Checksumming, then syscall cost |

With CRC at ~0.4 µs per block, the read path is now `pread` + block parsing + index/memtable search. The next win is
a block cache.

On Linux, use perf for a flame graph:

```sh
perf record -F 999 -g --call-graph dwarf -- ./build/release/benchmarks/lsm_bench --num=1000000
perf script | ./FlameGraph/stackcollapse-perf.pl | ./FlameGraph/flamegraph.pl > lsm.svg
```

## What I'd change next

1. **A sharded LRU block cache:** hits skip `pread`, the CRC, and block parsing. That's the remaining read gap to LevelDB.
2. **Group commit:** a leader writer batches queued writes into one WAL append + device flush. `sync=true` throughput
   scales with concurrent writers instead of being capped at one ~3 ms flush per write.
3. **File-at-a-time compaction with a compact pointer:** bounds compaction latency and transient disk space.
4. **Gradual write slowdown** before the L0 hard stop, to avoid latency cliffs.
5. **Prefix compression with restart points** in blocks: smaller tables and better cache density.
6. **Power-loss testing** with `dm-log-writes` or LazyFS, to verify the flush ordering the crash harness can't see.
