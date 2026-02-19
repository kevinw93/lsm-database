#pragma once

// SSTable file layout:
//
//   data block*  | filter block | index block | footer
//
// Every block is followed by a 4-byte masked CRC32C of its contents.
// Index entries map the last internal key of each data block to that block's
// handle. The filter is a Bloom filter over user keys.
//
// Footer (40 bytes): index offset | index size | filter offset | filter size
//                    | magic, all fixed64.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "lsm/status.h"

namespace lsm {

class RandomAccessFile;

inline constexpr std::size_t kBlockTrailerSize = 4;
inline constexpr std::size_t kFooterSize = 40;
inline constexpr std::uint64_t kTableMagic = 0x6c736d6b76737374ULL;

// size excludes the trailer.
struct BlockHandle {
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
};

inline constexpr std::size_t kBlockHandleSize = 16;

void EncodeBlockHandle(std::string* dst, const BlockHandle& handle);
Status DecodeBlockHandle(std::string_view input, BlockHandle* handle);

void EncodeFooter(std::string* dst, const BlockHandle& index,
                  const BlockHandle& filter);
Status DecodeFooter(std::string_view input, BlockHandle* index,
                    BlockHandle* filter);

// Reads a block and verifies its checksum. Rejects handles that extend past
// data_end before reading, so a corrupt handle can't cause a huge allocation.
Status ReadBlock(const RandomAccessFile& file, std::uint64_t data_end,
                 const BlockHandle& handle, std::string* contents);

}  // namespace lsm
