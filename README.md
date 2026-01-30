# lsmkv

An LSM-tree key-value storage engine in C++20.

## Build and test

Presets: `debug`, `asan`, `tsan`, `release`. Each configures, builds, and
tests in `build/<preset>`:

```sh
cmake --workflow --preset asan
cmake --workflow --preset tsan
```

Development targets Linux. The `.devcontainer/` image (Ubuntu 24.04, clang)
matches CI.

## Layout

```
include/lsm/   public API
src/util/      encoding, checksums
src/db/        engine internals
tests/         GoogleTest unit tests
```

## Design

### Internal keys

Every stored entry is keyed by `user_key | fixed64_le(seq << 8 | type)`.
Entries sort by user key ascending, then sequence number descending, so the
newest version of a key comes first and a seek to `(key, snapshot_seq)` lands
on the newest version visible at that snapshot. Sequence numbers are 56 bits.

### Encoding

Fixed-width integers are little-endian. Lengths are LEB128 varints. Decoders
reject truncated and overflowing input rather than returning a wrong value.

### Checksums

CRC32C (Castagnoli), table-driven, with the table generated at compile time.
Stored CRCs are masked (rotate + constant) because CRCs over data containing
CRCs are weak.
