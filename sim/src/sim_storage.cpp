// Preferences (NVS) and LittleFS for the simulator, stored under sim_data/ in the
// working directory so settings and caches survive between runs.
#include <LittleFS.h>
#include <Preferences.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>

namespace stdfs = std::filesystem;

namespace sim {
std::string dataDir() {
  const char* env = getenv("FLIGHTSCNR_SIM_DATA");
  return env != nullptr && env[0] != '\0' ? std::string(env) : std::string("sim_data");
}
}  // namespace sim

// ---------------------------------------------------------------------------
// Preferences: one text file, lines "namespace\tkey\tvalue" (values escaped).
// ---------------------------------------------------------------------------
namespace {

std::mutex s_nvs_mutex;
std::map<std::string, std::map<std::string, std::string>> s_nvs;
bool s_nvs_loaded = false;

std::string nvsPath() { return sim::dataDir() + "/nvs.txt"; }

std::string escape(const std::string& in) {
  std::string out;
  for (char c : in) {
    if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else if (c == '\t') out += "\\t";
    else out += c;
  }
  return out;
}

std::string unescape(const std::string& in) {
  std::string out;
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '\\' && i + 1 < in.size()) {
      const char n = in[++i];
      out += n == 'n' ? '\n' : n == 't' ? '\t' : n;
    } else {
      out += in[i];
    }
  }
  return out;
}

void nvsLoadLocked() {
  if (s_nvs_loaded) return;
  s_nvs_loaded = true;
  std::ifstream f(nvsPath());
  std::string line;
  while (std::getline(f, line)) {
    const size_t a = line.find('\t');
    const size_t b = a == std::string::npos ? a : line.find('\t', a + 1);
    if (b == std::string::npos) continue;
    s_nvs[line.substr(0, a)][line.substr(a + 1, b - a - 1)] = unescape(line.substr(b + 1));
  }
}

void nvsSaveLocked() {
  stdfs::create_directories(sim::dataDir());
  const std::string tmp = nvsPath() + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    for (const auto& ns : s_nvs) {
      for (const auto& kv : ns.second) {
        f << ns.first << '\t' << kv.first << '\t' << escape(kv.second) << '\n';
      }
    }
  }
  std::error_code ec;
  stdfs::rename(tmp, nvsPath(), ec);
}

}  // namespace

bool Preferences::begin(const char* name, bool read_only, const char*) {
  ns_ = name;
  read_only_ = read_only;
  open_ = true;
  std::lock_guard<std::mutex> lock(s_nvs_mutex);
  nvsLoadLocked();
  return true;
}

bool Preferences::clear() {
  if (!open_ || read_only_) return false;
  std::lock_guard<std::mutex> lock(s_nvs_mutex);
  s_nvs.erase(ns_.c_str());
  nvsSaveLocked();
  return true;
}

bool Preferences::remove(const char* key) {
  if (!open_ || read_only_) return false;
  std::lock_guard<std::mutex> lock(s_nvs_mutex);
  auto it = s_nvs.find(ns_.c_str());
  if (it == s_nvs.end() || it->second.erase(key) == 0) return false;
  nvsSaveLocked();
  return true;
}

bool Preferences::isKey(const char* key) {
  String v;
  return getRaw(key, &v);
}

bool Preferences::putRaw(const char* key, const String& value) {
  if (!open_ || read_only_ || key == nullptr) return false;
  std::lock_guard<std::mutex> lock(s_nvs_mutex);
  s_nvs[ns_.c_str()][key] = value.c_str();
  nvsSaveLocked();
  return true;
}

bool Preferences::getRaw(const char* key, String* out) {
  if (!open_ || key == nullptr) return false;
  std::lock_guard<std::mutex> lock(s_nvs_mutex);
  auto ns = s_nvs.find(ns_.c_str());
  if (ns == s_nvs.end()) return false;
  auto kv = ns->second.find(key);
  if (kv == ns->second.end()) return false;
  *out = kv->second.c_str();
  return true;
}

#define SIM_PUT_NUM(fn, T, fmt, sz)                       \
  size_t Preferences::fn(const char* key, T v) {          \
    char buf[48];                                         \
    snprintf(buf, sizeof(buf), fmt, v);                   \
    return putRaw(key, String(buf)) ? (sz) : 0;           \
  }
SIM_PUT_NUM(putUChar, uint8_t, "%u", 1)
SIM_PUT_NUM(putChar, int8_t, "%d", 1)
SIM_PUT_NUM(putUShort, uint16_t, "%u", 2)
SIM_PUT_NUM(putShort, int16_t, "%d", 2)
SIM_PUT_NUM(putInt, int32_t, "%ld", 4)
SIM_PUT_NUM(putUInt, uint32_t, "%lu", 4)
SIM_PUT_NUM(putLong64, int64_t, "%lld", 8)
SIM_PUT_NUM(putULong64, uint64_t, "%llu", 8)
SIM_PUT_NUM(putFloat, float, "%.9g", 4)
SIM_PUT_NUM(putDouble, double, "%.17g", 8)
#undef SIM_PUT_NUM

size_t Preferences::putBool(const char* key, bool v) { return putUChar(key, v ? 1 : 0); }
size_t Preferences::putString(const char* key, const char* v) {
  return putRaw(key, String(v != nullptr ? v : "")) ? strlen(v != nullptr ? v : "") + 1 : 0;
}
size_t Preferences::putBytes(const char* key, const void* v, size_t len) {
  String hex;
  const auto* p = static_cast<const uint8_t*>(v);
  char b[3];
  for (size_t i = 0; i < len; ++i) {
    snprintf(b, sizeof(b), "%02x", p[i]);
    hex += b;
  }
  return putRaw(key, hex) ? len : 0;
}

#define SIM_GET_NUM(fn, T, conv)                          \
  T Preferences::fn(const char* key, T def) {             \
    String v;                                             \
    if (!getRaw(key, &v)) return def;                     \
    return static_cast<T>(conv(v.c_str()));               \
  }
static long long simStrToLL(const char* s) { return strtoll(s, nullptr, 10); }
static unsigned long long simStrToULL(const char* s) { return strtoull(s, nullptr, 10); }
static double simStrToD(const char* s) { return strtod(s, nullptr); }
SIM_GET_NUM(getUChar, uint8_t, simStrToULL)
SIM_GET_NUM(getChar, int8_t, simStrToLL)
SIM_GET_NUM(getUShort, uint16_t, simStrToULL)
SIM_GET_NUM(getShort, int16_t, simStrToLL)
SIM_GET_NUM(getInt, int32_t, simStrToLL)
SIM_GET_NUM(getUInt, uint32_t, simStrToULL)
SIM_GET_NUM(getLong64, int64_t, simStrToLL)
SIM_GET_NUM(getULong64, uint64_t, simStrToULL)
SIM_GET_NUM(getFloat, float, simStrToD)
SIM_GET_NUM(getDouble, double, simStrToD)
#undef SIM_GET_NUM

bool Preferences::getBool(const char* key, bool def) { return getUChar(key, def ? 1 : 0) != 0; }

size_t Preferences::getString(const char* key, char* out, size_t max_len) {
  String v;
  if (!getRaw(key, &v) || out == nullptr || max_len == 0) return 0;
  const size_t n = std::min(static_cast<size_t>(v.length()), max_len - 1);
  memcpy(out, v.c_str(), n);
  out[n] = '\0';
  return n + 1;
}

String Preferences::getString(const char* key, const String& def) {
  String v;
  return getRaw(key, &v) ? v : def;
}

size_t Preferences::getBytesLength(const char* key) {
  String v;
  return getRaw(key, &v) ? v.length() / 2 : 0;
}

size_t Preferences::getBytes(const char* key, void* buf, size_t max_len) {
  String v;
  if (!getRaw(key, &v)) return 0;
  const size_t n = std::min(static_cast<size_t>(v.length() / 2), max_len);
  auto* p = static_cast<uint8_t*>(buf);
  for (size_t i = 0; i < n; ++i) {
    char b[3] = {v[i * 2], v[i * 2 + 1], 0};
    p[i] = static_cast<uint8_t>(strtoul(b, nullptr, 16));
  }
  return n;
}

// ---------------------------------------------------------------------------
// LittleFS -> sim_data/littlefs/
// ---------------------------------------------------------------------------
namespace fs {

class FileImpl {
 public:
  FILE* fp = nullptr;
  std::string path;
  std::string name;
  ~FileImpl() {
    if (fp != nullptr) fclose(fp);
  }
};

size_t File::write(uint8_t c) { return write(&c, 1); }
size_t File::write(const uint8_t* buf, size_t size) {
  return impl_ && impl_->fp ? fwrite(buf, 1, size, impl_->fp) : 0;
}
int File::available() {
  if (!impl_ || !impl_->fp) return 0;
  const long pos = ftell(impl_->fp);
  fseek(impl_->fp, 0, SEEK_END);
  const long end = ftell(impl_->fp);
  fseek(impl_->fp, pos, SEEK_SET);
  return static_cast<int>(end - pos);
}
int File::read() {
  if (!impl_ || !impl_->fp) return -1;
  const int c = fgetc(impl_->fp);
  return c == EOF ? -1 : c;
}
int File::peek() {
  if (!impl_ || !impl_->fp) return -1;
  const int c = fgetc(impl_->fp);
  if (c == EOF) return -1;
  ungetc(c, impl_->fp);
  return c;
}
void File::flush() {
  if (impl_ && impl_->fp) fflush(impl_->fp);
}
size_t File::read(uint8_t* buf, size_t size) {
  return impl_ && impl_->fp ? fread(buf, 1, size, impl_->fp) : 0;
}
bool File::seek(uint32_t pos, SeekMode mode) {
  return impl_ && impl_->fp &&
         fseek(impl_->fp, static_cast<long>(pos),
               mode == SeekCur ? SEEK_CUR : mode == SeekEnd ? SEEK_END : SEEK_SET) == 0;
}
size_t File::position() const {
  return impl_ && impl_->fp ? static_cast<size_t>(ftell(impl_->fp)) : 0;
}
size_t File::size() const {
  if (!impl_ || !impl_->fp) return 0;
  fflush(impl_->fp);
  std::error_code ec;
  const auto n = stdfs::file_size(impl_->path, ec);
  return ec ? 0 : static_cast<size_t>(n);
}
void File::close() { impl_.reset(); }
File::operator bool() const { return impl_ && impl_->fp; }
const char* File::name() const { return impl_ ? impl_->name.c_str() : ""; }
const char* File::path() const { return impl_ ? impl_->name.c_str() : ""; }

std::string FS::hostPath(const char* path) const {
  std::string p = path != nullptr ? path : "";
  while (!p.empty() && p[0] == '/') p.erase(0, 1);
  return sim::dataDir() + "/littlefs/" + p;
}

File FS::open(const char* path, const char* mode, bool) {
  const std::string host = hostPath(path);
  stdfs::create_directories(stdfs::path(host).parent_path());
  std::string m = mode != nullptr ? mode : "r";
  if (m.find('b') == std::string::npos) m += "b";
  FILE* fp = fopen(host.c_str(), m.c_str());
  if (fp == nullptr) return File();
  auto impl = std::make_shared<FileImpl>();
  impl->fp = fp;
  impl->path = host;
  impl->name = path;
  return File(impl);
}
bool FS::exists(const char* path) {
  std::error_code ec;
  return stdfs::exists(hostPath(path), ec);
}
bool FS::remove(const char* path) {
  std::error_code ec;
  return stdfs::remove(hostPath(path), ec);
}
bool FS::rename(const char* from, const char* to) {
  std::error_code ec;
  stdfs::rename(hostPath(from), hostPath(to), ec);
  return !ec;
}
bool FS::mkdir(const char* path) {
  std::error_code ec;
  return stdfs::create_directories(hostPath(path), ec) || !ec;
}
bool FS::rmdir(const char* path) {
  std::error_code ec;
  return stdfs::remove(hostPath(path), ec);
}

bool LittleFSFS::begin(bool, const char*, uint8_t, const char*) {
  std::error_code ec;
  stdfs::create_directories(sim::dataDir() + "/littlefs", ec);
  return true;
}
bool LittleFSFS::format() {
  std::error_code ec;
  stdfs::remove_all(sim::dataDir() + "/littlefs", ec);
  return begin();
}
// Same budget as the device's spiffs/LittleFS slice in default_16MB.csv.
size_t LittleFSFS::totalBytes() { return 3u * 1024u * 1024u; }
size_t LittleFSFS::usedBytes() {
  size_t total = 0;
  std::error_code ec;
  for (const auto& e : stdfs::recursive_directory_iterator(sim::dataDir() + "/littlefs", ec)) {
    if (e.is_regular_file(ec)) total += static_cast<size_t>(e.file_size(ec));
  }
  return total;
}

}  // namespace fs

fs::LittleFSFS LittleFS;
