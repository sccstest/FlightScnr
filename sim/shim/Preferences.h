#pragma once
// NVS Preferences backed by a text file (sim_data/nvs.txt), so settings survive restarts.
#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "Arduino.h"
#include "WString.h"

class Preferences {
 public:
  bool begin(const char* name, bool read_only = false, const char* partition = nullptr);
  void end() { open_ = false; }
  bool clear();
  bool remove(const char* key);
  bool isKey(const char* key);

  size_t putBool(const char* key, bool v);
  size_t putUChar(const char* key, uint8_t v);
  size_t putChar(const char* key, int8_t v);
  size_t putUShort(const char* key, uint16_t v);
  size_t putShort(const char* key, int16_t v);
  size_t putInt(const char* key, int32_t v);
  size_t putUInt(const char* key, uint32_t v);
  size_t putLong(const char* key, int32_t v) { return putInt(key, v); }
  size_t putULong(const char* key, uint32_t v) { return putUInt(key, v); }
  size_t putLong64(const char* key, int64_t v);
  size_t putULong64(const char* key, uint64_t v);
  size_t putFloat(const char* key, float v);
  size_t putDouble(const char* key, double v);
  size_t putString(const char* key, const char* v);
  size_t putString(const char* key, const String& v) { return putString(key, v.c_str()); }
  size_t putBytes(const char* key, const void* v, size_t len);

  bool getBool(const char* key, bool def = false);
  uint8_t getUChar(const char* key, uint8_t def = 0);
  int8_t getChar(const char* key, int8_t def = 0);
  uint16_t getUShort(const char* key, uint16_t def = 0);
  int16_t getShort(const char* key, int16_t def = 0);
  int32_t getInt(const char* key, int32_t def = 0);
  uint32_t getUInt(const char* key, uint32_t def = 0);
  int32_t getLong(const char* key, int32_t def = 0) { return getInt(key, def); }
  uint32_t getULong(const char* key, uint32_t def = 0) { return getUInt(key, def); }
  int64_t getLong64(const char* key, int64_t def = 0);
  uint64_t getULong64(const char* key, uint64_t def = 0);
  float getFloat(const char* key, float def = NAN);
  double getDouble(const char* key, double def = NAN);
  size_t getString(const char* key, char* out, size_t max_len);
  String getString(const char* key, const String& def = String());
  size_t getBytesLength(const char* key);
  size_t getBytes(const char* key, void* buf, size_t max_len);
  size_t freeEntries() { return 400; }

 private:
  bool putRaw(const char* key, const String& value);
  bool getRaw(const char* key, String* out);
  String ns_;
  bool open_ = false;
  bool read_only_ = false;
};
