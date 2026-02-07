#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace lsm {

// <db>/000123.log
std::string LogFileName(std::string_view db, std::uint64_t number);
std::string LockFileName(std::string_view db);

// True if name (a bare file name, not a path) is a log file.
bool ParseLogFileName(std::string_view name, std::uint64_t* number);

}  // namespace lsm
