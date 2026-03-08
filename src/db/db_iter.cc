#include "db/db_iter.h"

#include <cassert>
#include <string>
#include <utility>

namespace lsm {
namespace {

class DBIter final : public Iterator {
 public:
  DBIter(std::unique_ptr<Iterator> internal, SequenceNumber seq,
         std::shared_ptr<const void> pin)
      : pin_(std::move(pin)), iter_(std::move(internal)), seq_(seq) {}

  bool Valid() const override { return valid_; }

  void SeekToFirst() override {
    iter_->SeekToFirst();
    FindNextUserEntry(/*skipping=*/false);
  }

  void Seek(std::string_view target) override {
    std::string ikey;
    AppendInternalKey(&ikey, {target, seq_, kValueTypeForSeek});
    iter_->Seek(ikey);
    FindNextUserEntry(/*skipping=*/false);
  }

  void Next() override {
    assert(Valid());
    skip_key_.assign(key());  // hide this key's older versions
    iter_->Next();
    FindNextUserEntry(/*skipping=*/true);
  }

  std::string_view key() const override { return ExtractUserKey(iter_->key()); }
  std::string_view value() const override { return iter_->value(); }

  Status status() const override {
    return status_.ok() ? iter_->status() : status_;
  }

 private:
  // Advances to the newest visible version of the next user key that is not
  // deleted. Entries for one user key arrive newest first, so the first
  // visible one decides: a value is emitted, a tombstone hides the key.
  void FindNextUserEntry(bool skipping) {
    valid_ = false;
    for (; iter_->Valid(); iter_->Next()) {
      const auto entry = ParseInternalKey(iter_->key());
      if (!entry) {
        status_ = Status::Corruption("bad internal key");
        return;
      }
      if (entry->sequence > seq_) continue;  // newer than the snapshot
      if (skipping && entry->user_key == skip_key_) continue;
      if (entry->type == ValueType::kDeletion) {
        skip_key_.assign(entry->user_key);
        skipping = true;
        continue;
      }
      valid_ = true;
      return;
    }
  }

  // Declared first so the iterators are destroyed before what they read.
  std::shared_ptr<const void> pin_;
  std::unique_ptr<Iterator> iter_;
  const SequenceNumber seq_;
  std::string skip_key_;
  bool valid_ = false;
  Status status_;
};

}  // namespace

std::unique_ptr<Iterator> NewDBIterator(std::unique_ptr<Iterator> internal,
                                        SequenceNumber seq,
                                        std::shared_ptr<const void> pin) {
  return std::make_unique<DBIter>(std::move(internal), seq, std::move(pin));
}

}  // namespace lsm
