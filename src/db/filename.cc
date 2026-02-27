#include "db/filename.h"

#include <charconv>
#include <cstdio>

namespace lsm {

namespace {

std::string NumberedFileName(std::string_view db, std::uint64_t number,
                             const char* suffix) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "/%06llu.%s",
                static_cast<unsigned long long>(number), suffix);
  return std::string(db) + buf;
}

}  // namespace

std::string LogFileName(std::string_view db, std::uint64_t number) {
  return NumberedFileName(db, number, "log");
}

std::string TableFileName(std::string_view db, std::uint64_t number) {
  return NumberedFileName(db, number, "sst");
}

std::string LockFileName(std::string_view db) {
  return std::string(db) + "/LOCK";
}

std::string ManifestFileName(std::string_view db) {
  return std::string(db) + "/MANIFEST";
}

std::string TempFileName(std::string_view db) {
  return std::string(db) + "/MANIFEST.tmp";
}

bool ParseFileName(std::string_view name, std::uint64_t* number,
                   FileType* type) {
  if (name.ends_with(".log")) {
    *type = FileType::kLog;
  } else if (name.ends_with(".sst")) {
    *type = FileType::kTable;
  } else {
    return false;
  }
  name.remove_suffix(4);
  if (name.empty()) return false;
  const auto [end, ec] =
      std::from_chars(name.data(), name.data() + name.size(), *number);
  return ec == std::errc() && end == name.data() + name.size();
}

}  // namespace lsm
