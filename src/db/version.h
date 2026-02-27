#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "db/dbformat.h"
#include "lsm/status.h"
#include "table/table.h"

namespace lsm {

inline constexpr int kNumLevels = 4;

struct FileMeta {
  std::uint64_t number = 0;
  std::uint64_t size = 0;
  std::string smallest;  // internal keys
  std::string largest;
  std::shared_ptr<Table> table;  // not persisted
};

// Immutable once published. Readers hold a shared_ptr, so a Version (and the
// tables it references) outlives any reader using it.
struct Version {
  // Level 0: newest file first; key ranges may overlap.
  std::vector<FileMeta> levels[kNumLevels];
};

// Everything the MANIFEST persists.
struct ManifestState {
  std::uint64_t next_file_number = 1;
  // WALs numbered below this are fully reflected in the tables.
  std::uint64_t log_number = 0;
  // Highest sequence number stored in the tables.
  SequenceNumber last_sequence = 0;
  Version version;
};

// Atomically replaces <db>/MANIFEST: write temp, fsync, rename, fsync dir.
Status WriteManifest(const std::string& db, const ManifestState& state);
// Tables are not opened.
Status ReadManifest(const std::string& db, ManifestState* state);

}  // namespace lsm
