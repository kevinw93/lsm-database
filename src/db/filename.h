#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace lsm {

// <db>/000123.log
std::string LogFileName(std::string_view db, std::uint64_t number);
std::string LockFileName(std::string_view db);
// <db>/000123.sst
std::string TableFileName(std::string_view db, std::uint64_t number);
std::string ManifestFileName(std::string_view db);
// Staging file for the manifest; a leftover one is garbage.
std::string TempFileName(std::string_view db);

enum class FileType { kLog, kTable };

// Parses a bare file name (not a path) of the form 000123.log / 000123.sst.
bool ParseFileName(std::string_view name, std::uint64_t* number,
                   FileType* type);

}  // namespace lsm
