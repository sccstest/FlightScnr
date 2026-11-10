#pragma once
#include "FS.h"

namespace fs {
class LittleFSFS : public FS {
 public:
  bool begin(bool format_on_fail = false, const char* base_path = "/littlefs",
             uint8_t max_open = 10, const char* label = nullptr);
  void end() {}
  bool format();
  size_t totalBytes();
  size_t usedBytes();
};
}  // namespace fs

extern fs::LittleFSFS LittleFS;
