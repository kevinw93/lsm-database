#include "table/format.h"

#include "util/coding.h"
#include "util/crc32c.h"
#include "util/file.h"

namespace lsm {

void EncodeBlockHandle(std::string* dst, const BlockHandle& handle) {
  PutFixed64(dst, handle.offset);
  PutFixed64(dst, handle.size);
}

Status DecodeBlockHandle(std::string_view input, BlockHandle* handle) {
  if (input.size() != kBlockHandleSize) {
    return Status::Corruption("bad block handle");
  }
  handle->offset = DecodeFixed64(input.data());
  handle->size = DecodeFixed64(input.data() + 8);
  return Status::OK();
}

void EncodeFooter(std::string* dst, const BlockHandle& index,
                  const BlockHandle& filter) {
  EncodeBlockHandle(dst, index);
  EncodeBlockHandle(dst, filter);
  PutFixed64(dst, kTableMagic);
}

Status DecodeFooter(std::string_view input, BlockHandle* index,
                    BlockHandle* filter) {
  if (input.size() != kFooterSize ||
      DecodeFixed64(input.data() + 32) != kTableMagic) {
    return Status::Corruption("not an sstable (bad magic number)");
  }
  Status s = DecodeBlockHandle(input.substr(0, 16), index);
  if (s.ok()) s = DecodeBlockHandle(input.substr(16, 16), filter);
  return s;
}

Status ReadBlock(const RandomAccessFile& file, std::uint64_t data_end,
                 const BlockHandle& handle, std::string* contents) {
  if (handle.offset > data_end || handle.size > data_end - handle.offset ||
      kBlockTrailerSize > data_end - handle.offset - handle.size) {
    return Status::Corruption("block handle out of range");
  }
  const auto n = static_cast<std::size_t>(handle.size);
  Status s = file.Read(handle.offset, n + kBlockTrailerSize, contents);
  if (!s.ok()) return s;
  const std::uint32_t expected =
      crc32c::Unmask(DecodeFixed32(contents->data() + n));
  if (crc32c::Value(contents->data(), n) != expected) {
    return Status::Corruption("block checksum mismatch");
  }
  contents->resize(n);
  return Status::OK();
}

}  // namespace lsm
