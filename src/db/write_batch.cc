#include "lsm/write_batch.h"

#include <utility>

#include "db/memtable.h"
#include "db/write_batch_internal.h"
#include "util/coding.h"

namespace lsm {
namespace {

void SetCount(std::string* rep, std::uint32_t count) {
  EncodeFixed32(rep->data(), count);
}

// Calls f(type, key, value) for each record. Stops at the first malformed
// record, so callers that must not act on partial input validate first.
template <typename F>
Status ForEachRecord(std::string_view rep, std::uint32_t* count, F&& f) {
  if (rep.size() < WriteBatchInternal::kHeaderSize) {
    return Status::Corruption("write batch shorter than header");
  }
  const std::uint32_t expected = DecodeFixed32(rep.data());
  rep.remove_prefix(WriteBatchInternal::kHeaderSize);

  std::uint32_t found = 0;
  while (!rep.empty()) {
    const auto tag = static_cast<std::uint8_t>(rep.front());
    rep.remove_prefix(1);
    std::string_view key;
    std::string_view value;
    if (tag == static_cast<std::uint8_t>(ValueType::kValue)) {
      if (!GetLengthPrefixedSlice(&rep, &key) ||
          !GetLengthPrefixedSlice(&rep, &value)) {
        return Status::Corruption("bad Put record in write batch");
      }
    } else if (tag == static_cast<std::uint8_t>(ValueType::kDeletion)) {
      if (!GetLengthPrefixedSlice(&rep, &key)) {
        return Status::Corruption("bad Delete record in write batch");
      }
    } else {
      return Status::Corruption("unknown record type in write batch");
    }
    f(static_cast<ValueType>(tag), key, value);
    ++found;
  }
  if (found != expected)
    return Status::Corruption("write batch count mismatch");
  *count = found;
  return Status::OK();
}

}  // namespace

WriteBatch::WriteBatch(WriteBatch&& other) noexcept
    : rep_(std::move(other.rep_)) {
  other.Clear();
}

WriteBatch& WriteBatch::operator=(WriteBatch&& other) noexcept {
  if (this != &other) {
    rep_ = std::move(other.rep_);
    other.Clear();
  }
  return *this;
}

void WriteBatch::Clear() { rep_.assign(WriteBatchInternal::kHeaderSize, '\0'); }

std::uint32_t WriteBatch::Count() const { return DecodeFixed32(rep_.data()); }

void WriteBatch::Put(std::string_view key, std::string_view value) {
  SetCount(&rep_, Count() + 1);
  rep_.push_back(static_cast<char>(ValueType::kValue));
  PutLengthPrefixedSlice(&rep_, key);
  PutLengthPrefixedSlice(&rep_, value);
}

void WriteBatch::Delete(std::string_view key) {
  SetCount(&rep_, Count() + 1);
  rep_.push_back(static_cast<char>(ValueType::kDeletion));
  PutLengthPrefixedSlice(&rep_, key);
}

Status WriteBatchInternal::Validate(std::string_view rep,
                                    std::uint32_t* count) {
  return ForEachRecord(rep, count,
                       [](ValueType, std::string_view, std::string_view) {});
}

Status WriteBatchInternal::InsertInto(std::string_view rep,
                                      SequenceNumber first_seq, MemTable* mem) {
  std::uint32_t count = 0;
  Status s = Validate(rep, &count);
  if (!s.ok()) return s;
  SequenceNumber seq = first_seq;
  return ForEachRecord(
      rep, &count,
      [&](ValueType type, std::string_view key, std::string_view value) {
        mem->Add(seq++, type, key, value);
      });
}

}  // namespace lsm
