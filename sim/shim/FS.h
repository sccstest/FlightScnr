#pragma once
// Minimal fs::FS / fs::File over a host folder (sim_data/littlefs).
#include <stdio.h>

#include <memory>
#include <string>

#include "Stream.h"
#include "WString.h"

#define FILE_READ "r"
#define FILE_WRITE "w"
#define FILE_APPEND "a"

namespace fs {

enum SeekMode { SeekSet = 0, SeekCur = 1, SeekEnd = 2 };

class FileImpl;

class File : public Stream {
 public:
  File() = default;
  explicit File(std::shared_ptr<FileImpl> impl) : impl_(std::move(impl)) {}

  size_t write(uint8_t c) override;
  size_t write(const uint8_t* buf, size_t size) override;
  using Print::write;
  int available() override;
  int read() override;
  int peek() override;
  void flush() override;
  size_t read(uint8_t* buf, size_t size);
  size_t readBytes(char* buffer, size_t length) {
    return read(reinterpret_cast<uint8_t*>(buffer), length);
  }
  bool seek(uint32_t pos, SeekMode mode = SeekSet);
  size_t position() const;
  size_t size() const;
  void close();
  operator bool() const;
  const char* name() const;
  const char* path() const;
  bool isDirectory() const { return false; }
  File openNextFile(const char* = "r") { return File(); }

 private:
  std::shared_ptr<FileImpl> impl_;
};

class FS {
 public:
  File open(const char* path, const char* mode = FILE_READ, bool create = false);
  File open(const String& path, const char* mode = FILE_READ, bool create = false) {
    return open(path.c_str(), mode, create);
  }
  bool exists(const char* path);
  bool exists(const String& path) { return exists(path.c_str()); }
  bool remove(const char* path);
  bool remove(const String& path) { return remove(path.c_str()); }
  bool rename(const char* from, const char* to);
  bool rename(const String& from, const String& to) { return rename(from.c_str(), to.c_str()); }
  bool mkdir(const char* path);
  bool rmdir(const char* path);

 protected:
  std::string hostPath(const char* path) const;
};

}  // namespace fs

using fs::File;
using fs::FS;
