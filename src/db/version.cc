#include "db/version.h"

#include <cstdio>

#include "db/filename.h"
#include "db/log.h"
#include "util/coding.h"
#include "util/file.h"

namespace lsm {
namespace {

// next_file | log_number | last_sequence | per level: count, then
// (number | size | smallest | largest)*. Integers are varint64.
std::string Encode(const ManifestState& s) {
  std::string out;
  PutVarint64(&out, s.next_file_number);
  PutVarint64(&out, s.log_number);
  PutVarint64(&out, s.last_sequence);
  for (const auto& level : s.version.levels) {
    PutVarint64(&out, level.size());
    for (const FileMeta& f : level) {
      PutVarint64(&out, f.number);
      PutVarint64(&out, f.size);
      PutLengthPrefixedSlice(&out, f.smallest);
      PutLengthPrefixedSlice(&out, f.largest);
    }
  }
  return out;
}

bool Decode(std::string_view in, ManifestState* s) {
  if (!GetVarint64(&in, &s->next_file_number) ||
      !GetVarint64(&in, &s->log_number) ||
      !GetVarint64(&in, &s->last_sequence)) {
    return false;
  }
  for (auto& level : s->version.levels) {
    std::uint64_t count = 0;
    if (!GetVarint64(&in, &count)) return false;
    for (std::uint64_t i = 0; i < count; ++i) {
      FileMeta f;
      std::string_view smallest;
      std::string_view largest;
      if (!GetVarint64(&in, &f.number) || !GetVarint64(&in, &f.size) ||
          !GetLengthPrefixedSlice(&in, &smallest) ||
          !GetLengthPrefixedSlice(&in, &largest)) {
        return false;
      }
      f.smallest.assign(smallest);
      f.largest.assign(largest);
      level.push_back(std::move(f));
    }
  }
  return in.empty();
}

}  // namespace

Status WriteManifest(const std::string& db, const ManifestState& state) {
  const std::string tmp = TempFileName(db);
  std::unique_ptr<WritableFile> file;
  Status s = WritableFile::Open(tmp, &file);
  if (!s.ok()) return s;
  log::Writer writer(std::move(file));
  s = writer.AddRecord(Encode(state));
  if (s.ok()) s = writer.Sync();  // contents durable before the rename
  if (s.ok()) s = writer.Close();
  if (s.ok()) s = RenameFile(tmp, ManifestFileName(db));
  if (s.ok()) s = SyncDir(db);  // the rename itself durable
  if (!s.ok()) static_cast<void>(RemoveFile(tmp));
  return s;
}

Status ReadManifest(const std::string& db, ManifestState* state) {
  const std::string fname = ManifestFileName(db);
  std::string contents;
  Status s = ReadFileToString(fname, &contents);
  if (!s.ok()) return s;
  log::Reader reader(contents);
  std::string_view record;
  *state = ManifestState();
  if (!reader.ReadRecord(&record) || !Decode(record, state)) {
    return Status::Corruption(fname, "unreadable manifest");
  }
  return Status::OK();
}

}  // namespace lsm
