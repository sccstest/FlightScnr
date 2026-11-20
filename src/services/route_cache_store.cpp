#include "services/route_cache_store.h"

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <WebServer.h>

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cerrno>
#include <cstring>

#include "config.h"

namespace services::route_cache {

namespace {

constexpr char kPath[] = "/route_cache.csv";
constexpr char kTmpPath[] = "/route_cache.tmp";
constexpr char kHeader[] = "callsign,airline,origin,dest,source,cached_unix,api_done\n";

enum class MountState : uint8_t { kPending, kOk, kFailed };

MountState s_mount = MountState::kPending;
// Set by the route worker (markDirty), cleared by tick() when it hands a snapshot to
// the flush task, set again by the task if the write failed.
std::atomic<bool> s_dirty{false};
unsigned long s_last_flush_ms = 0;
SemaphoreHandle_t s_fs_mutex = nullptr;

/** Flush runs in a one-shot low-priority task, not on the loop: it reads the whole
 *  CSV, merges and rewrites it (flash writes stall both cores in bursts; the loop gets
 *  the CPU between them). Its stack is internal RAM and only exists during a flush. */
constexpr uint32_t kFlushTaskStackBytes = 6144;
constexpr UBaseType_t kFlushTaskPriority = 1;
std::atomic<bool> s_flush_running{false};

struct FlushJob {
  Entry* ram = nullptr;  // PSRAM snapshot of the RAM cache rows worth keeping
  size_t ram_count = 0;
  uint32_t now_sec = 0;
  uint32_t ttl_sec = 0;
};
FlushJob s_flush_job;  // owned by the flush task while s_flush_running

// Cached LittleFS usage for the [diag] line: usedBytes() walks the whole file system.
constexpr unsigned long kUsageRefreshMs = 30UL * 60UL * 1000UL;
std::atomic<uint32_t> s_usage_used{0};
std::atomic<uint32_t> s_usage_total{0};
std::atomic<unsigned long> s_usage_at_ms{0};
std::atomic<bool> s_usage_valid{false};

void refreshUsage() {
  const size_t total = LittleFS.totalBytes();
  const size_t used = LittleFS.usedBytes();
  s_usage_used.store(static_cast<uint32_t>(used));
  s_usage_total.store(static_cast<uint32_t>(total));
  s_usage_at_ms.store(millis());
  s_usage_valid.store(total > 0);
}

bool fillUsage(size_t* used_bytes, size_t* total_bytes) {
  if (!s_usage_valid.load()) {
    return false;
  }
  if (used_bytes != nullptr) {
    *used_bytes = s_usage_used.load();
  }
  if (total_bytes != nullptr) {
    *total_bytes = s_usage_total.load();
  }
  return true;
}

/**
 * Merge buffer for flush (~84 KB at 1500 rows). Too big for the loop task stack,
 * and as a static it pinned 84 KB of scarce internal RAM for the whole uptime
 * although it is only needed during a flush. Take it from PSRAM per flush.
 */
struct MergeBuffer {
  Entry* entries = nullptr;
  MergeBuffer() {
    entries = static_cast<Entry*>(heap_caps_malloc(
        sizeof(Entry) * config::kRouteCacheFileMaxEntries, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  ~MergeBuffer() { heap_caps_free(entries); }
  MergeBuffer(const MergeBuffer&) = delete;
  MergeBuffer& operator=(const MergeBuffer&) = delete;
};

void ensureFsMutex() {
  if (s_fs_mutex == nullptr) {
    s_fs_mutex = xSemaphoreCreateMutex();
  }
}

struct FsLock {
  bool acquire(TickType_t timeout) {
    ensureFsMutex();
    if (s_fs_mutex == nullptr) {
      return false;
    }
    held_ = xSemaphoreTake(s_fs_mutex, timeout) == pdTRUE;
    return held_;
  }

  ~FsLock() {
    if (held_ && s_fs_mutex != nullptr) {
      xSemaphoreGive(s_fs_mutex);
    }
  }

  bool held_ = false;
};

void noteFlushAttempt(unsigned long now_ms) {
  s_last_flush_ms = now_ms;
}

void ensureCacheFile() {
  if (!LittleFS.exists(kPath)) {
    File created = LittleFS.open(kPath, "w");
    if (created) {
      created.print(kHeader);
      created.close();
    }
  }
}

/** A flush always writes the tmp file completely before it replaces the cache, so a
 *  tmp file next to a missing cache is a finished copy (power cut between the old
 *  remove and rename): keep it. Any other leftover tmp is partial: drop it. */
void recoverInterruptedFlush() {
  if (!LittleFS.exists(kTmpPath)) {
    return;
  }
  if (!LittleFS.exists(kPath)) {
    if (LittleFS.rename(kTmpPath, kPath)) {
      Serial.println("[route_cache] restored cache from an interrupted save");
      return;
    }
  }
  LittleFS.remove(kTmpPath);
}

bool ensureMounted() {
  if (s_mount == MountState::kOk) {
    return true;
  }
  if (s_mount == MountState::kFailed) {
    return false;
  }

  if (LittleFS.begin(false)) {
    s_mount = MountState::kOk;
    recoverInterruptedFlush();
    ensureCacheFile();
    return true;
  }

  Serial.println("route_cache: partition not LittleFS — formatting spiffs slice...");
  if (!LittleFS.format()) {
    Serial.println("route_cache: format failed");
    s_mount = MountState::kFailed;
    return false;
  }
  if (!LittleFS.begin(false)) {
    Serial.println("route_cache: mount failed after format");
    s_mount = MountState::kFailed;
    return false;
  }

  Serial.println("route_cache: LittleFS ready");
  s_mount = MountState::kOk;
  ensureCacheFile();
  return true;
}

bool entryResolved(const Entry& e) {
  const bool has_data =
      e.airline[0] != '\0' || e.origin[0] != '\0' || e.dest[0] != '\0';
  return e.api_done || has_data || e.source == 5;  // 5 == ApiSource::kPrefix
}

bool entryPending(const Entry& e) {
  return e.callsign[0] != '\0' && !e.api_done && !entryResolved(e);
}

bool entryExpired(const Entry& e, uint32_t now_sec, uint32_t ttl_sec) {
  if (e.cached_at_sec == 0) {
    return true;
  }
  return (now_sec - e.cached_at_sec) > ttl_sec;
}

bool entryKeepInFlash(const Entry& e, uint32_t now_sec, uint32_t ttl_sec) {
  if (entryResolved(e) || entryPending(e)) {
    return true;
  }
  return !entryExpired(e, now_sec, ttl_sec);
}

/** Trim trailing CR/LF/spaces in place. */
void trimLine(char* line) {
  if (line == nullptr) {
    return;
  }
  size_t n = strlen(line);
  while (n > 0 &&
         (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) {
    line[n - 1] = '\0';
    --n;
  }
}

const char* nextField(char* line, char** rest) {
  char* comma = strchr(line, ',');
  if (comma != nullptr) {
    *comma = '\0';
    *rest = comma + 1;
  } else {
    *rest = nullptr;
  }
  return line;
}

bool parseCsvLine(char* line, Entry* out) {
  if (out == nullptr || line == nullptr) {
    return false;
  }
  trimLine(line);
  if (line[0] == '\0' || line[0] == '#') {
    return false;
  }
  if (strncmp(line, "callsign", 8) == 0) {
    return false;
  }

  char* rest = line;
  char* field = rest;
  nextField(rest, &rest);
  if (field[0] == '\0') {
    return false;
  }
  strncpy(out->callsign, field, sizeof(out->callsign) - 1);
  out->callsign[sizeof(out->callsign) - 1] = '\0';

  field = rest;
  if (field == nullptr) {
    return false;
  }
  nextField(field, &rest);
  strncpy(out->airline, field, sizeof(out->airline) - 1);
  out->airline[sizeof(out->airline) - 1] = '\0';

  field = rest;
  if (field == nullptr) {
    return false;
  }
  nextField(field, &rest);
  strncpy(out->origin, field, sizeof(out->origin) - 1);
  out->origin[sizeof(out->origin) - 1] = '\0';

  field = rest;
  if (field == nullptr) {
    return false;
  }
  nextField(field, &rest);
  strncpy(out->dest, field, sizeof(out->dest) - 1);
  out->dest[sizeof(out->dest) - 1] = '\0';

  field = rest;
  if (field == nullptr) {
    return false;
  }
  nextField(field, &rest);
  out->source = 0;
  if (strcmp(field, "AL") == 0) {
    out->source = 2;
  } else if (strcmp(field, "FA") == 0) {
    out->source = 3;
  } else if (strcmp(field, "FR") == 0) {
    out->source = 4;
  } else if (strcmp(field, "pfx") == 0) {
    out->source = 5;
  } else if (strcmp(field, "adb") == 0) {
    out->source = 6;  // 6 == ApiSource::kAdsbDb
  } else if (strcmp(field, "cache") == 0) {
    out->source = 1;
  }

  field = rest;
  if (field == nullptr) {
    return false;
  }
  nextField(field, &rest);
  out->cached_at_sec = static_cast<uint32_t>(strtoul(field, nullptr, 10));

  field = rest;
  if (field == nullptr) {
    return false;
  }
  out->api_done = (strcmp(field, "1") == 0 || strcmp(field, "true") == 0);
  return true;
}

void formatSourceTag(uint8_t source, char* out, size_t out_len) {
  if (out_len == 0) {
    return;
  }
  const char* tag = "";
  switch (source) {
    case 2:
      tag = "AL";
      break;
    case 3:
      tag = "FA";
      break;
    case 4:
      tag = "FR";
      break;
    case 5:
      tag = "pfx";
      break;
    case 6:
      tag = "adb";  // 6 == ApiSource::kAdsbDb
      break;
    case 1:
      tag = "cache";
      break;
    default:
      break;
  }
  strncpy(out, tag, out_len - 1);
  out[out_len - 1] = '\0';
}

int findMergedIndex(const Entry* entries, size_t count, const char* callsign) {
  for (size_t i = 0; i < count; ++i) {
    if (strcmp(entries[i].callsign, callsign) == 0) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void upsertMerged(Entry* entries, size_t* count, const Entry& e) {
  const int idx = findMergedIndex(entries, *count, e.callsign);
  if (idx >= 0) {
    entries[idx] = e;
    return;
  }
  if (*count >= config::kRouteCacheFileMaxEntries) {
    size_t oldest = 0;
    for (size_t i = 1; i < *count; ++i) {
      if (entries[i].cached_at_sec < entries[oldest].cached_at_sec) {
        oldest = i;
      }
    }
    entries[oldest] = e;
    return;
  }
  entries[*count] = e;
  ++(*count);
}

/** Upper bound for one formatted row (fields are <= 8+27+4+4+3 chars plus numbers). */
constexpr size_t kMaxRowBytes = 96;
/** Largest cache file the flush will load: header + kRouteCacheFileMaxEntries rows. */
constexpr size_t kMaxFileBytes = 192 * 1024;
constexpr size_t kWriteChunkBytes = 4096;
/** Keep this much LittleFS space free beyond the new file (old file coexists until rename). */
constexpr size_t kFreeSpaceMarginBytes = 16 * 1024;

/** Scratch block in PSRAM, freed on scope exit. data == nullptr when PSRAM is short. */
struct PsramBlock {
  uint8_t* data = nullptr;
  explicit PsramBlock(size_t bytes) {
    data = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  ~PsramBlock() { heap_caps_free(data); }
  PsramBlock(const PsramBlock&) = delete;
  PsramBlock& operator=(const PsramBlock&) = delete;
};

/** Text of one entry plus newline; returns the byte count, or 0 if it did not fit. */
size_t formatEntry(char* dst, size_t cap, const Entry& e) {
  char src[8];
  formatSourceTag(e.source, src, sizeof(src));
  const int n = snprintf(dst, cap, "%s,%s,%s,%s,%s,%lu,%u\n", e.callsign,
                         e.airline[0] != '\0' ? e.airline : "",
                         e.origin[0] != '\0' ? e.origin : "", e.dest[0] != '\0' ? e.dest : "",
                         src, static_cast<unsigned long>(e.cached_at_sec),
                         e.api_done ? 1u : 0u);
  if (n < 0 || static_cast<size_t>(n) >= cap) {
    return 0;
  }
  return static_cast<size_t>(n);
}

enum class ReadResult : uint8_t { kOk, kMissing, kError };

/**
 * Read a whole file in one go. The file handle is open for a few milliseconds
 * instead of the ~1 s a character-at-a-time parse keeps it open; in that window
 * the descriptor was seen to turn invalid (errno 9) and the old read loop then
 * spun forever because available() never reached 0.
 */
ReadResult readWholeFile(const char* path, uint8_t* buf, size_t cap, size_t* out_len) {
  *out_len = 0;
  if (!LittleFS.exists(path)) {
    return ReadResult::kMissing;
  }
  File f = LittleFS.open(path, "r");
  if (!f) {
    return ReadResult::kError;
  }
  const size_t size = f.size();
  if (size > cap) {
    f.close();
    return ReadResult::kError;
  }
  size_t got = 0;
  while (got < size) {
    const size_t n = f.read(buf + got, size - got);
    if (n == 0) {
      break;
    }
    got += n;
  }
  f.close();
  if (got != size) {
    return ReadResult::kError;
  }
  *out_len = got;
  return ReadResult::kOk;
}

/**
 * Write `len` bytes to `path` in chunks, checking every write, then confirm the
 * size on flash. On any failure the partial file is removed and false is
 * returned, so a bad write can never replace a good cache file.
 */
bool writeFileChecked(const char* path, const uint8_t* data, size_t len) {
  File f = LittleFS.open(path, "w");
  if (!f) {
    Serial.println("[route_cache] write open failed");
    return false;
  }
  size_t off = 0;
  while (off < len) {
    const size_t chunk = (len - off) < kWriteChunkBytes ? (len - off) : kWriteChunkBytes;
    errno = 0;
    const size_t w = f.write(data + off, chunk);
    if (w != chunk) {
      Serial.printf("[route_cache] write failed at %u/%u errno=%d\n",
                    static_cast<unsigned>(off + w), static_cast<unsigned>(len), errno);
      f.close();
      LittleFS.remove(path);
      return false;
    }
    off += w;
  }
  f.close();

  File check = LittleFS.open(path, "r");
  const size_t on_flash = check ? check.size() : 0;
  if (check) {
    check.close();
  }
  if (on_flash != len) {
    Serial.printf("[route_cache] size check failed: wrote %u, found %u\n",
                  static_cast<unsigned>(len), static_cast<unsigned>(on_flash));
    LittleFS.remove(path);
    return false;
  }
  return true;
}

}  // namespace

bool mount() { return ensureMounted(); }

bool lookup(const char* callsign, Entry* out, uint32_t now_sec, uint32_t ttl_sec) {
  if (out == nullptr || callsign == nullptr || callsign[0] == '\0') {
    return false;
  }
  FsLock lock;
  if (!lock.acquire(pdMS_TO_TICKS(2000))) {
    return false;
  }
  if (!ensureMounted() || !LittleFS.exists(kPath)) {
    return false;
  }

  File f = LittleFS.open(kPath, "r");
  if (!f) {
    return false;
  }

  char line[160];
  while (f.available()) {
    const size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
    if (n == 0) {
      continue;
    }
    line[n] = '\0';

    Entry parsed;
    if (!parseCsvLine(line, &parsed)) {
      continue;
    }
    if (strcmp(parsed.callsign, callsign) != 0) {
      continue;
    }
    if (entryExpired(parsed, now_sec, ttl_sec)) {
      continue;
    }
    if (!entryResolved(parsed)) {
      continue;
    }
    *out = parsed;
    f.close();
    return true;
  }

  f.close();
  return false;
}

bool lookupPermanent(const char* callsign, Entry* out) {
  if (out == nullptr || callsign == nullptr || callsign[0] == '\0') {
    return false;
  }
  FsLock lock;
  if (!lock.acquire(pdMS_TO_TICKS(2000))) {
    return false;
  }
  if (!ensureMounted() || !LittleFS.exists(kPath)) {
    return false;
  }

  File f = LittleFS.open(kPath, "r");
  if (!f) {
    return false;
  }

  char line[160];
  while (f.available()) {
    const size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
    if (n == 0) {
      continue;
    }
    line[n] = '\0';

    Entry parsed;
    if (!parseCsvLine(line, &parsed)) {
      continue;
    }
    if (strcmp(parsed.callsign, callsign) != 0) {
      continue;
    }
    if (!entryResolved(parsed) && !entryPending(parsed)) {
      continue;
    }
    *out = parsed;
    f.close();
    return true;
  }

  f.close();
  return false;
}

void markDirty() { s_dirty.store(true); }

namespace {

/** Append `e`, or overwrite the oldest row when the file limit is reached. */
void appendOrEvict(Entry* entries, size_t* count, const Entry& e) {
  if (*count < config::kRouteCacheFileMaxEntries) {
    entries[*count] = e;
    ++(*count);
    return;
  }
  size_t oldest = 0;
  for (size_t i = 1; i < *count; ++i) {
    if (entries[i].cached_at_sec < entries[oldest].cached_at_sec) {
      oldest = i;
    }
  }
  entries[oldest] = e;
}

/** The merge-and-rewrite. Runs in the flush task. Returns true when the cache file was
 *  replaced (or nothing needed writing), false to retry on the next interval. */
bool flushNow(const FlushJob& job) {
  FsLock lock;
  if (!lock.acquire(pdMS_TO_TICKS(5000))) {
    return false;
  }
  if (!ensureMounted()) {
    return false;
  }

  MergeBuffer merge;
  if (merge.entries == nullptr) {
    return false;  // no PSRAM headroom now; retry on the next interval
  }
  Entry* const merged = merge.entries;
  size_t merged_count = 0;

  // RAM rows first (newest data). They are unique callsigns (upsert over <= 64 rows).
  for (size_t i = 0; i < job.ram_count; ++i) {
    upsertMerged(merged, &merged_count, job.ram[i]);
  }
  const size_t ram_rows = merged_count;
  const uint32_t now_sec = job.now_sec;
  const uint32_t ttl_sec = job.ttl_sec;

  // Rows already on flash. Loaded in one read (see readWholeFile) and parsed from
  // memory. If the file cannot be read, do not overwrite it with a RAM-only set:
  // retry next interval, and only after repeated failures give up on its content.
  {
    static uint8_t s_read_fail_streak = 0;
    PsramBlock in(kMaxFileBytes);
    if (in.data == nullptr) {
      return false;
    }
    size_t in_len = 0;
    const ReadResult rr = readWholeFile(kPath, in.data, kMaxFileBytes, &in_len);
    if (rr == ReadResult::kError) {
      ++s_read_fail_streak;
      Serial.printf("[route_cache] read failed (%u in a row)%s\n",
                    static_cast<unsigned>(s_read_fail_streak),
                    s_read_fail_streak >= 3 ? ", rebuilding from RAM" : ", flush skipped");
      if (s_read_fail_streak < 3) {
        return false;
      }
    } else {
      s_read_fail_streak = 0;
    }

    const char* p = reinterpret_cast<const char*>(in.data);
    const char* const end = p + in_len;
    char line[160];
    while (p < end) {
      const char* nl = static_cast<const char*>(memchr(p, '\n', static_cast<size_t>(end - p)));
      const size_t span = nl != nullptr ? static_cast<size_t>(nl - p) : static_cast<size_t>(end - p);
      const size_t n = span < sizeof(line) - 1 ? span : sizeof(line) - 1;
      memcpy(line, p, n);
      line[n] = '\0';
      p += span + (nl != nullptr ? 1 : 0);
      if (n == 0) {
        continue;
      }
      Entry parsed;
      if (!parseCsvLine(line, &parsed)) {
        continue;
      }
      if (!entryKeepInFlash(parsed, now_sec, ttl_sec)) {
        continue;
      }
      // The file was written deduplicated, so a flash row can only collide with a
      // RAM row: check those few (was a scan of every merged row, O(n^2) at 1500).
      if (findMergedIndex(merged, ram_rows, parsed.callsign) >= 0) {
        continue;
      }
      appendOrEvict(merged, &merged_count, parsed);
    }
  }

  // Format the new file in memory, then write it in checked chunks.
  const size_t out_cap = sizeof(kHeader) + merged_count * kMaxRowBytes;
  PsramBlock out_block(out_cap);
  if (out_block.data == nullptr) {
    return false;
  }
  char* const out = reinterpret_cast<char*>(out_block.data);
  size_t out_len = sizeof(kHeader) - 1;
  memcpy(out, kHeader, out_len);
  for (size_t i = 0; i < merged_count; ++i) {
    out_len += formatEntry(out + out_len, out_cap - out_len, merged[i]);
  }

  const size_t fs_total = LittleFS.totalBytes();
  const size_t fs_used = LittleFS.usedBytes();
  if (fs_total < fs_used || (fs_total - fs_used) < out_len + kFreeSpaceMarginBytes) {
    Serial.printf("[route_cache] not enough flash space (%u free, need %u), flush skipped\n",
                  static_cast<unsigned>(fs_total - fs_used),
                  static_cast<unsigned>(out_len + kFreeSpaceMarginBytes));
    return false;
  }

  if (!writeFileChecked(kTmpPath, out_block.data, out_len)) {
    return false;  // retried next interval, old cache untouched
  }

  // Power-safe replace: LittleFS renames over an existing file atomically, so the old
  // cache stays readable until the new one is complete. Only if that is refused
  // (some VFS layers do) fall back to remove + rename; a cut between those two is
  // repaired at the next mount (recoverInterruptedFlush keeps the finished tmp).
  if (!LittleFS.rename(kTmpPath, kPath)) {
    if (LittleFS.exists(kPath) && !LittleFS.remove(kPath)) {
      Serial.println("[route_cache] remove failed (file busy?)");
      LittleFS.remove(kTmpPath);
      return false;
    }
    if (!LittleFS.rename(kTmpPath, kPath)) {
      Serial.println("[route_cache] rename failed");
      return false;
    }
  }

  refreshUsage();
  Serial.printf("[route_cache] saved %u entries\n", static_cast<unsigned>(merged_count));
  return true;
}

void flushTask(void*) {
  const unsigned long started = millis();
  const bool ok = flushNow(s_flush_job);
  if (!ok) {
    s_dirty.store(true);  // try again on the next interval
  }
  if (config::kSerialTraceDebug || !ok) {
    Serial.printf("[route_cache] flush %s in %lums\n", ok ? "done" : "failed",
                  millis() - started);
  }
  heap_caps_free(s_flush_job.ram);
  s_flush_job = {};
  s_flush_running.store(false);
  vTaskDelete(nullptr);
}

}  // namespace

void tick(unsigned long now_ms, ReadRamSlotFn read_slot, size_t max_slots,
          uint32_t now_sec, uint32_t ttl_sec) {
  if (!s_dirty.load()) {
    return;
  }

  if (s_last_flush_ms == 0) {
    s_last_flush_ms = now_ms;
    return;
  }
  if ((now_ms - s_last_flush_ms) < config::kRouteCacheFlushIntervalMs) {
    return;
  }
  if (read_slot == nullptr || s_flush_running.load()) {
    return;
  }
  noteFlushAttempt(now_ms);  // next try one interval from now, whatever happens

  // Snapshot the RAM rows here (64 small copies); the task does the file work.
  Entry* snap = static_cast<Entry*>(
      heap_caps_malloc(sizeof(Entry) * (max_slots > 0 ? max_slots : 1),
                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (snap == nullptr) {
    return;
  }
  size_t count = 0;
  for (size_t i = 0; i < max_slots; ++i) {
    Entry slot;
    if (!read_slot(i, &slot, max_slots) || slot.callsign[0] == '\0') {
      continue;
    }
    if (!entryKeepInFlash(slot, now_sec, ttl_sec)) {
      continue;
    }
    snap[count++] = slot;
  }

  s_flush_job.ram = snap;
  s_flush_job.ram_count = count;
  s_flush_job.now_sec = now_sec;
  s_flush_job.ttl_sec = ttl_sec;
  s_dirty.store(false);  // a markDirty() during the flush re-arms the next one
  s_flush_running.store(true);
  if (xTaskCreatePinnedToCore(flushTask, "rc_flush", kFlushTaskStackBytes, nullptr,
                              kFlushTaskPriority, nullptr, 0) != pdPASS) {
    Serial.println("[route_cache] flush task create failed; retry next interval");
    heap_caps_free(snap);
    s_flush_job = {};
    s_flush_running.store(false);
    s_dirty.store(true);
  }
}

void sendDownload(WebServer* server) {
  if (server == nullptr) {
    return;
  }
  FsLock lock;
  if (!lock.acquire(pdMS_TO_TICKS(5000))) {
    server->send(503, "text/plain; charset=utf-8", "Route cache busy, try again");
    return;
  }
  if (!ensureMounted() || !LittleFS.exists(kPath)) {
    server->send(404, "text/plain; charset=utf-8", "Route cache file not found");
    return;
  }

  File f = LittleFS.open(kPath, "r");
  if (!f) {
    server->send(500, "text/plain; charset=utf-8", "Could not open route cache");
    return;
  }

  server->sendHeader("Content-Disposition", "attachment; filename=\"route_cache.csv\"");
  server->sendHeader("Cache-Control", "no-store");
  server->streamFile(f, "text/csv; charset=utf-8");
  f.close();
}

bool clear() {
  FsLock lock;
  if (!lock.acquire(pdMS_TO_TICKS(5000))) {
    return false;
  }
  s_dirty.store(false);
  if (!ensureMounted()) {
    return true;
  }
  if (LittleFS.exists(kTmpPath)) {
    LittleFS.remove(kTmpPath);
  }
  if (LittleFS.exists(kPath) && !LittleFS.remove(kPath)) {
    Serial.println("[route_cache] clear remove failed");
    return false;
  }
  ensureCacheFile();
  refreshUsage();
  Serial.println("[route_cache] cleared");
  return true;
}

bool flashUsage(size_t* used_bytes, size_t* total_bytes) {
  if (used_bytes != nullptr) {
    *used_bytes = 0;
  }
  if (total_bytes != nullptr) {
    *total_bytes = 0;
  }
  // Cached: LittleFS.usedBytes() walks the whole file system. Refreshed after each
  // cache save (in the flush task) and otherwise at most every 30 min; other writers
  // (map bake, sky caches) show up by then.
  const bool stale = !s_usage_valid.load() ||
                     millis() - s_usage_at_ms.load() >= kUsageRefreshMs;
  if (stale) {
    if (s_flush_running.load()) {
      return s_usage_valid.load() && fillUsage(used_bytes, total_bytes);
    }
    FsLock lock;
    if (!lock.acquire(0) || !ensureMounted()) {
      return false;
    }
    refreshUsage();
  }
  return fillUsage(used_bytes, total_bytes);
}

}  // namespace services::route_cache
