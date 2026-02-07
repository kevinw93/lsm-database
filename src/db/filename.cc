#include "db/filename.h"

#include <charconv>
#include <cstdio>

namespace lsm {

std::string LogFileName(std::string_view db, std::uint64_t number) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "/%06llu.log",
                static_cast<unsigned long long>(number));
  return std::string(db) + buf;
}

std::string LockFileName(std::string_view db) {
  return std::string(db) + "/LOCK";
}

bool ParseLogFileName(std::string_view name, std::uint64_t* number) {
  constexpr std::string_view kSuffix = ".log";
  if (!name.ends_with(kSuffix)) return false;
  name.remove_suffix(kSuffix.size());
  if (name.empty()) return false;
  const auto [end, ec] =
      std::from_chars(name.data(), name.data() + name.size(), *number);
  return ec == std::errc() && end == name.data() + name.size();
}

}  // namespace lsm
