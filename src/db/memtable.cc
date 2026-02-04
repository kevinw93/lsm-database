#include "db/memtable.h"

#include <cassert>
#include <cstring>

namespace lsm {
namespace {

// memcpy with a null pointer is undefined even for n == 0, and an empty
// string_view may have data() == nullptr.
void CopyBytes(char* dst, std::string_view src) {
  if (!src.empty()) std::memcpy(dst, src.data(), src.size());
}

class MemTableIterator final : public Iterator {
 public:
  explicit MemTableIterator(const SkipList* table) : iter_(table) {}

  bool Valid() const override { return iter_.Valid(); }
  void SeekToFirst() override { iter_.SeekToFirst(); }
  void Seek(std::string_view target) override { iter_.Seek(target); }
  void Next() override { iter_.Next(); }
  std::string_view key() const override { return iter_.key(); }
  std::string_view value() const override { return iter_.value(); }
  Status status() const override { return Status::OK(); }

 private:
  SkipList::Iterator iter_;
};

}  // namespace

// Entry bytes in the arena: user_key | trailer | value. The skiplist node
// holds views into this buffer.
void MemTable::Add(SequenceNumber seq, ValueType type,
                   std::string_view user_key, std::string_view value) {
  if (type == ValueType::kDeletion) value = {};
  const std::size_t ikey_size = user_key.size() + kInternalKeyTrailerSize;
  char* buf = arena_.Allocate(ikey_size + value.size());
  CopyBytes(buf, user_key);
  EncodeFixed64(buf + user_key.size(), PackSequenceAndType(seq, type));
  CopyBytes(buf + ikey_size, value);
  table_.Insert(std::string_view(buf, ikey_size),
                std::string_view(buf + ikey_size, value.size()));
}

MemTable::GetResult MemTable::Get(std::string_view user_key,
                                  SequenceNumber seq,
                                  std::string* value) const {
  std::string seek_key;
  AppendInternalKey(&seek_key, {user_key, seq, kValueTypeForSeek});

  SkipList::Iterator iter(&table_);
  iter.Seek(seek_key);
  if (!iter.Valid()) return GetResult::kNotPresent;

  const auto entry = ParseInternalKey(iter.key());
  assert(entry.has_value());
  if (entry->user_key != user_key) return GetResult::kNotPresent;
  if (entry->type == ValueType::kDeletion) return GetResult::kDeleted;
  value->assign(iter.value());
  return GetResult::kFound;
}

std::unique_ptr<Iterator> MemTable::NewIterator() const {
  return std::make_unique<MemTableIterator>(&table_);
}

}  // namespace lsm
