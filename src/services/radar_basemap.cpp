// The zoom blit touches every pixel of a 720x720 map; the framework default is -Os.
#pragma GCC optimize("O2")

#include "services/radar_basemap.h"

#include <Arduino.h>
#include <FS.h>
#include <JPEGDEC.h>
#include <LittleFS.h>
#include <Preferences.h>

#include "config.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include <esp_heap_caps.h>

// ESP32-P4 hardware JPEG decoder (esp_driver_jpeg). Real device only; the simulator
// and the S3 boards use JPEGDEC.
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C) && !defined(FLIGHTSCNR_SIM) && \
    defined(CONFIG_SOC_JPEG_DECODE_SUPPORTED) && __has_include(<driver/jpeg_decode.h>)
#include <driver/jpeg_decode.h>
#include <driver/ppa.h>
#include <esp_cache.h>
#define FS_BASEMAP_HW_JPEG 1
#else
#define FS_BASEMAP_HW_JPEG 0
#endif

#include "services/map_center.h"
#include "services/https_lock.h"
#include "services/route_cache_store.h"
#include "ui/radar_display.h"
#include "ui/radar_scale.h"

namespace services::basemap {

namespace {

constexpr char kPath[] = "/basemap.jpg";
constexpr char kTmpPath[] = "/basemap.tmp";
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
// Sharper copies for the smaller range presets, baked in the same session as the
// main image (same center / facing / style). Bit i of the mask = a file for
// ui::radar::kRangeMileOptions[i]. Without them, zooming in only upscales the main
// bake (10x at 3 mi from a 30 mi bake: very soft).
constexpr char kDetailMaskKey[] = "lvl";
uint8_t s_detail_mask = 0;
#endif
constexpr char kStoreNs[] = "fs_basemap";
constexpr char kEnKey[] = "en";
constexpr char kLatKey[] = "lat";
constexpr char kLonKey[] = "lon";
constexpr char kMiKey[] = "mi";
constexpr char kFacingKey[] = "fac";
constexpr char kStyleKey[] = "sty";
constexpr char kValidKey[] = "ok";
constexpr char kContrastDarkKey[] = "ctr_d";
constexpr char kContrastLightKey[] = "ctr_l";
constexpr char kWashVfrKey[] = "wsh_v";

constexpr uint8_t kDefaultContrastDarkPct = 100;
constexpr uint8_t kDefaultContrastLightPct = 100;
constexpr uint8_t kDefaultWashVfrPct = 55;
constexpr uint8_t kMaxContrastPct = 200;

/** Skip JPEG decode on the loop task when memory/TLS is tight (decode can stall
 *  the UI for seconds and fights ADS-B for internal heap). */
constexpr uint32_t kMinFreeHeapToDecode = 22000;
constexpr uint32_t kMinContigHeapToDecode = 10000;

bool s_enabled = false;
Meta s_meta{};
uint8_t s_contrast_dark_pct = kDefaultContrastDarkPct;
uint8_t s_contrast_light_pct = kDefaultContrastLightPct;
uint8_t s_wash_vfr_pct = kDefaultWashVfrPct;

/** Decoded bakes kept in PSRAM (1 MB each on the 4C), so zooming between ranges that
 *  were seen recently is a copy or a stretch, not a new JPEG decode. */
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
constexpr size_t kCacheSlots = 3;
#else
constexpr size_t kCacheSlots = 1;
#endif
/** A new slot is only allocated while this much PSRAM stays free after it; otherwise
 *  the least recently used slot's buffer is reused. */
constexpr size_t kPsramReserveBytes = 4u * 1024u * 1024u;
/** Slot buffers are cache-line aligned so the hardware decoder can write them. */
constexpr size_t kSlotAlign = 128;

struct CacheSlot {
  uint16_t* px = nullptr;
  bool valid = false;
  uint8_t mi = 0;  // coverage (miles) of the bake decoded here
  uint32_t stamp = 0;
};
CacheSlot s_slots[kCacheSlots];
uint32_t s_slot_clock = 0;

#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
/** The compressed bake files (60-110 KB each) kept in PSRAM after their first read:
 *  reading one from LittleFS took 47-66 ms on the device, as long as the hardware
 *  decode and the blit together. Dropped with the decoded cache on any bake change. */
struct RawJpeg {
  char path[24] = "";
  uint8_t* data = nullptr;
  size_t len = 0;
};
constexpr size_t kRawCacheEntries = 8;  // the main bake + one per range preset
RawJpeg s_raw[kRawCacheEntries];
#endif

constexpr size_t kSlotPixels = static_cast<size_t>(kPixelSize) * static_cast<size_t>(kPixelSize);
constexpr size_t kSlotBytes = kSlotPixels * sizeof(uint16_t);

/** Cache LittleFS presence — exists()/open on a missing path spams VFS errors
 *  and was hit from every stalled radar full-draw retry. */
enum class FileState : uint8_t { Unknown, Present, Absent };
FileState s_file_state = FileState::Unknown;

File s_upload;
bool s_upload_active = false;
size_t s_upload_bytes = 0;
bool s_upload_failed = false;

struct DrawCtx {
  uint16_t* pixels = nullptr;
  int w = 0;
  int h = 0;
};

DrawCtx s_draw_ctx;

void* psramAlloc(size_t bytes) {
  return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void psramFree(void* p) {
  if (p != nullptr) {
    heap_caps_free(p);
  }
}

bool ensureFs() { return services::route_cache::mount(); }

void persistEnabled() {
  Preferences prefs;
  if (prefs.begin(kStoreNs, false)) {
    prefs.putBool(kEnKey, s_enabled);
    prefs.end();
  }
}

#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
void detailPath(uint8_t mi, char* out, size_t len) {
  snprintf(out, len, "/basemap_%u.jpg", static_cast<unsigned>(mi));
}

int rangeIndex(uint8_t mi) {
  for (size_t i = 0; i < ui::radar::kRangeMileOptionCount; ++i) {
    if (ui::radar::kRangeMileOptions[i] == mi) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void persistDetailMask() {
  Preferences prefs;
  if (prefs.begin(kStoreNs, false)) {
    prefs.putUChar(kDetailMaskKey, s_detail_mask);
    prefs.end();
  }
}

/** Delete every detail bake (a new main bake or a clear invalidates them). */
void removeDetailFiles() {
  char path[24];
  for (size_t i = 0; i < ui::radar::kRangeMileOptionCount; ++i) {
    detailPath(ui::radar::kRangeMileOptions[i], path, sizeof(path));
    if ((s_detail_mask & (1U << i)) != 0 || LittleFS.exists(path)) {
      LittleFS.remove(path);
    }
  }
  s_detail_mask = 0;
  persistDetailMask();
}
#endif

/** Bake to draw from for the live range: the smallest detail bake that still covers
 *  it, else the main one. */
uint8_t sourceMiles(uint8_t live_mi) {
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
  for (size_t i = 0; i < ui::radar::kRangeMileOptionCount; ++i) {
    const uint8_t r = ui::radar::kRangeMileOptions[i];
    if (r >= live_mi && r < s_meta.range_miles && (s_detail_mask & (1U << i)) != 0) {
      return r;
    }
  }
#else
  (void)live_mi;
#endif
  return s_meta.range_miles;
}

void persistMeta() {
  Preferences prefs;
  if (!prefs.begin(kStoreNs, false)) {
    return;
  }
  prefs.putBool(kValidKey, s_meta.valid);
  if (s_meta.valid) {
    prefs.putDouble(kLatKey, s_meta.lat);
    prefs.putDouble(kLonKey, s_meta.lon);
    prefs.putUChar(kMiKey, s_meta.range_miles);
    prefs.putUShort(kFacingKey, s_meta.facing_deg);
    prefs.putUChar(kStyleKey, static_cast<uint8_t>(s_meta.style));
  }
  prefs.end();
}

void loadPrefs() {
  Preferences prefs;
  if (!prefs.begin(kStoreNs, true)) {
    return;
  }
  s_enabled = prefs.getBool(kEnKey, false);
  s_contrast_dark_pct = prefs.getUChar(kContrastDarkKey, kDefaultContrastDarkPct);
  s_contrast_light_pct = prefs.getUChar(kContrastLightKey, kDefaultContrastLightPct);
  s_wash_vfr_pct = prefs.getUChar(kWashVfrKey, kDefaultWashVfrPct);
  if (s_contrast_dark_pct > kMaxContrastPct) {
    s_contrast_dark_pct = kMaxContrastPct;
  }
  if (s_contrast_light_pct > kMaxContrastPct) {
    s_contrast_light_pct = kMaxContrastPct;
  }
  if (s_wash_vfr_pct > 100) {
    s_wash_vfr_pct = 100;
  }
  s_meta.valid = prefs.getBool(kValidKey, false);
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
  s_detail_mask = prefs.getUChar(kDetailMaskKey, 0);
#endif
  if (s_meta.valid) {
    s_meta.lat = prefs.getDouble(kLatKey, 0);
    s_meta.lon = prefs.getDouble(kLonKey, 0);
    s_meta.range_miles = prefs.getUChar(kMiKey, 0);
    s_meta.facing_deg = prefs.getUShort(kFacingKey, 0);
    const uint8_t sty = prefs.getUChar(kStyleKey, static_cast<uint8_t>(Style::Dark));
    if (sty == static_cast<uint8_t>(Style::Light)) {
      s_meta.style = Style::Light;
    } else if (sty == static_cast<uint8_t>(Style::Vfr)) {
      s_meta.style = Style::Vfr;
    } else if (sty == static_cast<uint8_t>(Style::Voyager)) {
      s_meta.style = Style::Voyager;
    } else {
      s_meta.style = Style::Dark;
    }
  }
  prefs.end();
}

void freeSlot(CacheSlot* slot) {
  psramFree(slot->px);
  *slot = CacheSlot{};
}

/** Forget every decoded bake (new bake, clear, orphaned file). */
void freeCache() {
  for (CacheSlot& slot : s_slots) {
    freeSlot(&slot);
  }
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
  for (RawJpeg& raw : s_raw) {
    psramFree(raw.data);
    raw = RawJpeg{};
  }
#endif
}

CacheSlot* findSlot(uint8_t mi) {
  for (CacheSlot& slot : s_slots) {
    if (slot.valid && slot.mi == mi) {
      return &slot;
    }
  }
  return nullptr;
}

/** Slot to decode a new bake into, with a buffer: a spare allocated one, a new one
 *  while PSRAM has room, else the least recently used. Null if nothing can be had. */
CacheSlot* claimSlot() {
  CacheSlot* spare = nullptr;
  CacheSlot* empty = nullptr;
  CacheSlot* lru = nullptr;
  for (CacheSlot& slot : s_slots) {
    if (slot.px != nullptr && !slot.valid) {
      spare = spare != nullptr ? spare : &slot;
    } else if (slot.px == nullptr) {
      empty = empty != nullptr ? empty : &slot;
    } else if (lru == nullptr || slot.stamp < lru->stamp) {
      lru = &slot;
    }
  }
  if (spare != nullptr) {
    return spare;
  }
  if (empty != nullptr) {
    const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    // Always allow the first slot (the old single cache did); more only with headroom.
    if (lru == nullptr || free_psram >= kSlotBytes + kPsramReserveBytes) {
      empty->px = static_cast<uint16_t*>(
          heap_caps_aligned_alloc(kSlotAlign, kSlotBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (empty->px != nullptr) {
        empty->valid = false;
        return empty;
      }
    }
  }
  if (lru != nullptr) {
    lru->valid = false;
    return lru;
  }
  return nullptr;
}

void noteFilePresent() { s_file_state = FileState::Present; }
void noteFileAbsent() { s_file_state = FileState::Absent; }
void noteFileUnknown() { s_file_state = FileState::Unknown; }

bool filePresent() {
  if (s_file_state == FileState::Present) {
    return true;
  }
  if (s_file_state == FileState::Absent) {
    return false;
  }
  if (!ensureFs()) {
    noteFileAbsent();
    return false;
  }
  // Single probe — ESP32 LittleFS.exists() opens the path and logs on miss.
  if (LittleFS.exists(kPath)) {
    noteFilePresent();
    return true;
  }
  noteFileAbsent();
  return false;
}

void clearMetaIfOrphaned() {
  if (!s_meta.valid) {
    return;
  }
  if (filePresent()) {
    return;
  }
  // NVS says we have a bake but the JPEG is gone — stop retrying.
  s_meta = {};
  persistMeta();
  if (s_enabled) {
    s_enabled = false;
    persistEnabled();
  }
  freeCache();
}

int jpegDrawCallback(JPEGDRAW* draw) {
  DrawCtx* ctx = &s_draw_ctx;
  if (ctx->pixels == nullptr || draw == nullptr || draw->pPixels == nullptr) {
    return 0;
  }
  const int src_stride = draw->iWidth;
  int copy_w = (draw->iWidthUsed > 0) ? draw->iWidthUsed : draw->iWidth;
  int src_x = 0;
  int dst_x = draw->x;
  if (dst_x < 0) {
    src_x = -dst_x;
    copy_w += dst_x;
    dst_x = 0;
  }
  if (dst_x + copy_w > ctx->w) {
    copy_w = ctx->w - dst_x;
  }
  if (copy_w <= 0 || src_x >= src_stride) {
    return 1;
  }
  for (int row = 0; row < draw->iHeight; ++row) {
    const int dy = draw->y + row;
    if (dy < 0 || dy >= ctx->h) {
      continue;
    }
    const uint16_t* src = draw->pPixels + row * src_stride + src_x;
    uint16_t* dst = ctx->pixels + static_cast<size_t>(dy) * static_cast<size_t>(ctx->w) +
                    static_cast<size_t>(dst_x);
    memcpy(dst, src, static_cast<size_t>(copy_w) * sizeof(uint16_t));
  }
  return 1;
}

/** Software decode (JPEGDEC) of a whole bake into `out` (kPixelSize square). */
bool softDecode(uint8_t* jpeg, size_t len, uint16_t* out) {
  memset(out, 0, kSlotBytes);
  s_draw_ctx.pixels = out;
  s_draw_ctx.w = kPixelSize;
  s_draw_ctx.h = kPixelSize;

  // JPEGDEC embeds large file/huffman buffers — keep it out of .bss / internal RAM.
  void* jpeg_obj_mem = psramAlloc(sizeof(JPEGDEC));
  if (jpeg_obj_mem == nullptr) {
    s_draw_ctx.pixels = nullptr;
    Serial.println("[basemap] JPEGDEC alloc failed");
    return false;
  }
  auto* dec = new (jpeg_obj_mem) JPEGDEC();
  bool ok = false;
  if (!dec->openRAM(jpeg, static_cast<int>(len), jpegDrawCallback)) {
    Serial.println("[basemap] jpeg open failed");
  } else {
    dec->setPixelType(RGB565_LITTLE_ENDIAN);
    const int jw = dec->getWidth();
    const int jh = dec->getHeight();
    if (jw < kPixelSize - 8 || jh < kPixelSize - 8 || jw > kPixelSize + 16 ||
        jh > kPixelSize + 16) {
      Serial.printf("[basemap] unexpected size %dx%d (want %d)\n", jw, jh, kPixelSize);
    } else {
      ok = dec->decode(0, 0, 0) == 1;
      if (!ok) {
        Serial.println("[basemap] jpeg decode failed");
      }
    }
    dec->close();
  }
  dec->~JPEGDEC();
  psramFree(jpeg_obj_mem);
  s_draw_ctx.pixels = nullptr;
  return ok;
}

#if FS_BASEMAP_HW_JPEG
// ---- ESP32-P4 hardware JPEG decoder ----------------------------------------------
// About 10x faster than JPEGDEC. The first decode of each boot runs both decoders and
// compares them; the hardware one is only used when its picture matches (colors, byte
// order). Any later error falls back to JPEGDEC; three in a row turn it off until
// reboot. The engine is created per decode and deleted right after, so it holds no
// interrupt or DMA memory between zooms.
// The engine's own YUV->RGB step (2D-DMA colour conversion) came out 1-2 steps too
// bright against JPEGDEC on the device (bias -134/-163/-202 x100), consistent with a
// limited-range (video) matrix applied to full-range JFIF data. OkYuv asks the engine
// for raw YUV 4:4:4 and converts it here with the JFIF full-range formula instead.
// OkPpa: raw YUV 4:2:0 from the engine, then the pixel accelerator (PPA) converts it
// with the full-range matrix (the PPA takes full range only for YUV420 input).
enum class HwState : uint8_t { Untested, Ok, OkSwapped, OkYuv, OkPpa, Off };
HwState s_hw_state = HwState::Untested;
bool s_ppa_rgb_swap = false;
bool s_ppa_byte_swap = false;
/** Byte positions of Y, Cb, Cr inside each 3-byte YUV444 pixel (found by the self-check). */
uint8_t s_yuv_pos[3] = {0, 1, 2};
/** YUV444 scratch for OkYuv (720*720*3 bytes, PSRAM, DMA-aligned), kept between decodes. */
uint8_t* s_yuv_buf = nullptr;
constexpr size_t kYuvBytes = kSlotPixels * 3u;
constexpr size_t kYuv420Bytes = kSlotPixels * 3u / 2u;
uint8_t s_hw_failures = 0;
/** Internal DMA-capable RAM the decoder may use (descriptors, header tables) must not
 *  starve the Wi-Fi link: skip the hardware below this much free. */
constexpr size_t kMinDmaFreeForHw = 48u * 1024u;

size_t dmaFree() { return heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL); }

uint8_t* allocJpegInput(size_t len) {
  jpeg_decode_memory_alloc_cfg_t cfg = {};
  cfg.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER;
  size_t got = 0;
  return static_cast<uint8_t*>(jpeg_alloc_decoder_mem(len, &cfg, &got));
}

/** 1 = decoded into `out` (cache-line aligned, `out_bytes` long) in `format`, 0 = not
 *  attempted (size or memory precondition), -1 = the hardware failed. */
/** Before a DMA engine writes `buf`: write back and drop any cached lines of it, so a
 *  dirty line (the CPU wrote these slots before: JPEGDEC, memset) evicted mid-transfer
 *  can't land on top of the new pixels. */
void cacheCleanForDma(void* buf, size_t bytes) {
  esp_cache_msync(buf, bytes,
                  ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE |
                      ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

int hwDecodeRaw(const uint8_t* jpeg, size_t len, jpeg_dec_output_format_t format, uint8_t* out,
                size_t out_bytes) {
  jpeg_decode_picture_info_t info = {};
  if (jpeg_decoder_get_info(jpeg, static_cast<uint32_t>(len), &info) != ESP_OK) {
    return -1;
  }
  // The hardware pads output to 16 px; only an exact 720x720 bake goes straight in.
  if (info.width != static_cast<uint32_t>(kPixelSize) ||
      info.height != static_cast<uint32_t>(kPixelSize)) {
    return 0;
  }
  const size_t dma_free = dmaFree();
  if (dma_free < kMinDmaFreeForHw) {
    Serial.printf("[basemap] hw jpeg skipped: dma free %u\n", static_cast<unsigned>(dma_free));
    return 0;
  }
  jpeg_decode_engine_cfg_t eng_cfg = {};
  eng_cfg.intr_priority = 0;
  eng_cfg.timeout_ms = 500;
  jpeg_decoder_handle_t eng = nullptr;
  esp_err_t err = jpeg_new_decoder_engine(&eng_cfg, &eng);
  if (err != ESP_OK || eng == nullptr) {
    Serial.printf("[basemap] hw jpeg engine: %s\n", esp_err_to_name(err));
    return -1;
  }
  cacheCleanForDma(out, out_bytes);
  jpeg_decode_cfg_t dec_cfg = {};
  dec_cfg.output_format = format;
  dec_cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;  // little-endian RGB565
  dec_cfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;      // JFIF
  uint32_t out_size = 0;
  err = jpeg_decoder_process(eng, &dec_cfg, jpeg, static_cast<uint32_t>(len), out,
                             static_cast<uint32_t>(out_bytes), &out_size);
  jpeg_del_decoder_engine(eng);
  if (err != ESP_OK || out_size < out_bytes) {
    Serial.printf("[basemap] hw jpeg decode fmt=%d: %s out=%lu\n", static_cast<int>(format),
                  esp_err_to_name(err), static_cast<unsigned long>(out_size));
    return -1;
  }
  return 1;
}

/** JFIF (full-range BT.601) YCbCr -> RGB565 tables, built once. */
struct YuvTables {
  int16_t r_cr[256];
  int16_t b_cb[256];
  int16_t g_cbcr_cb[256];
  int16_t g_cbcr_cr[256];
  uint8_t clamp[256 + 512];  // index v + 256 for v in [-256, 511]
  bool ready = false;
};
YuvTables* s_yuv_tables = nullptr;
YuvTables s_yuv_tables_store;  // ~2.3 KB internal RAM: looked up 4x per pixel

bool ensureYuvTables() {
  if (s_yuv_tables != nullptr) {
    return true;
  }
  YuvTables* t = &s_yuv_tables_store;
  for (int i = 0; i < 256; ++i) {
    const int c = i - 128;
    // 16.16 fixed point, rounded.
    t->r_cr[i] = static_cast<int16_t>((91881 * c + 32768) >> 16);       // 1.402
    t->b_cb[i] = static_cast<int16_t>((116130 * c + 32768) >> 16);      // 1.772
    t->g_cbcr_cb[i] = static_cast<int16_t>((-22554 * c) >> 16);          // -0.344136
    t->g_cbcr_cr[i] = static_cast<int16_t>((-46802 * c + 32768) >> 16);  // -0.714136
  }
  for (int v = -256; v < 512; ++v) {
    t->clamp[v + 256] = static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
  }
  t->ready = true;
  s_yuv_tables = t;
  return true;
}

/** YUV444 (byte order s_yuv_pos-style `pos`) -> native RGB565, truncating like JPEGDEC. */
void yuv444To565(const uint8_t* yuv, uint16_t* out, size_t n, const uint8_t pos[3]) {
  const YuvTables& t = *s_yuv_tables;
  const uint8_t* clamp = t.clamp + 256;
  const uint8_t py = pos[0];
  const uint8_t pu = pos[1];
  const uint8_t pv = pos[2];
  if (py == 2 && pu == 1 && pv == 0) {
    // The order the device reports (Cr, Cb, Y): constant offsets, two pixels per pass,
    // one 32-bit store.
    uint32_t* out32 = reinterpret_cast<uint32_t*>(out);
    const size_t pairs = n / 2;
    for (size_t i = 0; i < pairs; ++i, yuv += 6) {
      const int y0 = yuv[2];
      const int y1 = yuv[5];
      const int rc0 = t.r_cr[yuv[0]];
      const int gc0 = t.g_cbcr_cb[yuv[1]] + t.g_cbcr_cr[yuv[0]];
      const int bc0 = t.b_cb[yuv[1]];
      const int rc1 = t.r_cr[yuv[3]];
      const int gc1 = t.g_cbcr_cb[yuv[4]] + t.g_cbcr_cr[yuv[3]];
      const int bc1 = t.b_cb[yuv[4]];
      const uint32_t p0 = ((clamp[y0 + rc0] & 0xF8u) << 8) | ((clamp[y0 + gc0] & 0xFCu) << 3) |
                          (clamp[y0 + bc0] >> 3);
      const uint32_t p1 = ((clamp[y1 + rc1] & 0xF8u) << 8) | ((clamp[y1 + gc1] & 0xFCu) << 3) |
                          (clamp[y1 + bc1] >> 3);
      out32[i] = p0 | (p1 << 16);
    }
    return;
  }
  for (size_t i = 0; i < n; ++i, yuv += 3) {
    const int y = yuv[py];
    const uint8_t cb = yuv[pu];
    const uint8_t cr = yuv[pv];
    const uint8_t r = clamp[y + t.r_cr[cr]];
    const uint8_t g = clamp[y + t.g_cbcr_cb[cb] + t.g_cbcr_cr[cr]];
    const uint8_t b = clamp[y + t.b_cb[cb]];
    out[i] = static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
  }
}

bool ensureYuvBuf() {
  if (s_yuv_buf != nullptr) {
    return true;
  }
  jpeg_decode_memory_alloc_cfg_t cfg = {};
  cfg.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER;
  size_t got = 0;
  s_yuv_buf = static_cast<uint8_t*>(jpeg_alloc_decoder_mem(kYuvBytes, &cfg, &got));
  return s_yuv_buf != nullptr && ensureYuvTables();
}

void swapBytes(uint16_t* px, size_t n);

/** PPA: YUV 4:2:0 (full range, BT.601) -> RGB565 into a cache-aligned 720x720 slot. */
bool ppaYuv420To565(const uint8_t* yuv, uint16_t* out, bool rgb_swap) {
  ppa_client_config_t cc = {};
  cc.oper_type = PPA_OPERATION_SRM;
  cc.max_pending_trans_num = 1;
  ppa_client_handle_t client = nullptr;
  esp_err_t err = ppa_register_client(&cc, &client);
  if (err != ESP_OK || client == nullptr) {
    Serial.printf("[basemap] ppa client: %s\n", esp_err_to_name(err));
    return false;
  }
  ppa_srm_oper_config_t op = {};
  op.in.buffer = yuv;
  op.in.pic_w = kPixelSize;
  op.in.pic_h = kPixelSize;
  op.in.block_w = kPixelSize;
  op.in.block_h = kPixelSize;
  op.in.srm_cm = PPA_SRM_COLOR_MODE_YUV420;
  op.in.yuv_range = PPA_COLOR_RANGE_FULL;
  op.in.yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601;
  op.out.buffer = out;
  op.out.buffer_size = kSlotBytes;
  op.out.pic_w = kPixelSize;
  op.out.pic_h = kPixelSize;
  op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
  op.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
  op.scale_x = 1.0f;
  op.scale_y = 1.0f;
  op.rgb_swap = rgb_swap;
  op.mode = PPA_TRANS_MODE_BLOCKING;
  cacheCleanForDma(out, kSlotBytes);
  err = ppa_do_scale_rotate_mirror(client, &op);
  ppa_unregister_client(client);
  if (err != ESP_OK) {
    Serial.printf("[basemap] ppa srm: %s\n", esp_err_to_name(err));
    return false;
  }
  return true;
}

/** Decode into a 720x720 RGB565 slot with whichever hardware path the self-check chose. */
int hwDecode(const uint8_t* jpeg, size_t len, uint16_t* out) {
  if (s_hw_state == HwState::OkPpa) {
    if (!ensureYuvBuf()) {
      return 0;
    }
    const int r = hwDecodeRaw(jpeg, len, JPEG_DECODE_OUT_FORMAT_YUV420, s_yuv_buf, kYuv420Bytes);
    if (r <= 0) {
      return r;
    }
    if (!ppaYuv420To565(s_yuv_buf, out, s_ppa_rgb_swap)) {
      return -1;
    }
    if (s_ppa_byte_swap) {
      swapBytes(out, kSlotPixels);
    }
    return 1;
  }
  if (s_hw_state == HwState::OkYuv) {
    if (!ensureYuvBuf()) {
      return 0;
    }
    const int r = hwDecodeRaw(jpeg, len, JPEG_DECODE_OUT_FORMAT_YUV444, s_yuv_buf, kYuvBytes);
    if (r > 0) {
      yuv444To565(s_yuv_buf, out, kSlotPixels, s_yuv_pos);
    }
    return r;
  }
  const int r = hwDecodeRaw(jpeg, len, JPEG_DECODE_OUT_FORMAT_RGB565,
                            reinterpret_cast<uint8_t*>(out), kSlotBytes);
  if (r > 0 && s_hw_state == HwState::OkSwapped) {
    swapBytes(out, kSlotPixels);
  }
  return r;
}

void swapBytes(uint16_t* px, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    px[i] = static_cast<uint16_t>((px[i] << 8) | (px[i] >> 8));
  }
}

/** Mean |difference| per channel (in 5-bit steps, green halved) and mean signed
 *  difference per channel, x100, over every 3rd pixel in both directions. */
struct Diff {
  int32_t mean_abs_x100;
  int32_t bias_r_x100;
  int32_t bias_g_x100;
  int32_t bias_b_x100;
};

Diff compare565(const uint16_t* ref, const uint16_t* test, bool swap) {
  int64_t sum_abs = 0;
  int64_t sr = 0;
  int64_t sg = 0;
  int64_t sb = 0;
  int64_t n = 0;
  for (int y = 1; y < kPixelSize; y += 3) {
    for (int x = 1; x < kPixelSize; x += 3) {
      const size_t i = static_cast<size_t>(y) * kPixelSize + static_cast<size_t>(x);
      const uint16_t a = ref[i];
      uint16_t b = test[i];
      if (swap) {
        b = static_cast<uint16_t>((b << 8) | (b >> 8));
      }
      const int dr = static_cast<int>(a >> 11) - static_cast<int>(b >> 11);
      const int dg = static_cast<int>((a >> 5) & 0x3F) - static_cast<int>((b >> 5) & 0x3F);
      const int db = static_cast<int>(a & 0x1F) - static_cast<int>(b & 0x1F);
      sum_abs += 2 * std::abs(dr) + std::abs(dg) + 2 * std::abs(db);  // green is 6-bit
      sr += dr;
      sg += dg;
      sb += db;
      ++n;
    }
  }
  Diff d{};
  if (n > 0) {
    d.mean_abs_x100 = static_cast<int32_t>(sum_abs * 100 / (6 * n));
    d.bias_r_x100 = static_cast<int32_t>(sr * 100 / n);
    d.bias_g_x100 = static_cast<int32_t>(sg * 100 / (2 * n));
    d.bias_b_x100 = static_cast<int32_t>(sb * 100 / n);
  }
  return d;
}

bool diffAcceptable(const Diff& d) {
  // Two correct decoders differ by rounding and chroma upsampling: well under one step
  // on average, with no overall color shift. A wrong byte order is far off.
  return d.mean_abs_x100 <= 100 && std::abs(d.bias_r_x100) <= 50 &&
         std::abs(d.bias_g_x100) <= 50 && std::abs(d.bias_b_x100) <= 50;
}

/** First decode of the boot: `ref` is JPEGDEC's picture. Decode again in hardware and
 *  keep the hardware only if it matches. */
void hwSelfCheck(const uint8_t* jpeg, size_t len, const uint16_t* ref) {
  if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < kSlotBytes + kPsramReserveBytes) {
    return;  // try on a later decode
  }
  auto* tmp = static_cast<uint16_t*>(
      heap_caps_aligned_alloc(kSlotAlign, kSlotBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (tmp == nullptr) {
    return;
  }
  const size_t dma_before = dmaFree();
  const unsigned long t0 = millis();
  const int r = hwDecodeRaw(jpeg, len, JPEG_DECODE_OUT_FORMAT_RGB565,
                            reinterpret_cast<uint8_t*>(tmp), kSlotBytes);
  const unsigned long hw_ms = millis() - t0;
  if (r == 0) {
    psramFree(tmp);
    return;  // not applicable to this bake; try the next one
  }
  const char* verdict = "off (no match), using JPEGDEC";
  Diff direct{};
  Diff swapped{};
  if (r > 0) {
    direct = compare565(ref, tmp, false);
    swapped = compare565(ref, tmp, true);
    if (diffAcceptable(direct)) {
      s_hw_state = HwState::Ok;
      verdict = "ok";
    } else if (diffAcceptable(swapped)) {
      s_hw_state = HwState::OkSwapped;
      verdict = "ok (byte-swapped)";
    }
  }
  Serial.printf(
      "[basemap] hw jpeg self-check rgb565: %s hw_ms=%lu diff=%ld bias=%ld/%ld/%ld "
      "swapped_diff=%ld (x100, 5-bit steps) dma_free=%u->%u\n",
      r > 0 ? (s_hw_state == HwState::Untested ? "no match" : verdict) : "decoder error", hw_ms,
      static_cast<long>(direct.mean_abs_x100), static_cast<long>(direct.bias_r_x100),
      static_cast<long>(direct.bias_g_x100), static_cast<long>(direct.bias_b_x100),
      static_cast<long>(swapped.mean_abs_x100), static_cast<unsigned>(dma_before),
      static_cast<unsigned>(dmaFree()));

  // The engine's own colour conversion did not match. First choice: raw YUV 4:2:0 and
  // the PPA's full-range conversion (hardware end to end).
  if (s_hw_state == HwState::Untested && ensureYuvBuf()) {
    const unsigned long tp = millis();
    const int rp =
        hwDecodeRaw(jpeg, len, JPEG_DECODE_OUT_FORMAT_YUV420, s_yuv_buf, kYuv420Bytes);
    const unsigned long dec_ms = millis() - tp;
    if (rp > 0) {
      for (int sw = 0; sw < 2 && s_hw_state == HwState::Untested; ++sw) {
        const unsigned long tc = millis();
        if (!ppaYuv420To565(s_yuv_buf, tmp, sw == 1)) {
          break;
        }
        const unsigned long ppa_ms = millis() - tc;
        const Diff d = compare565(ref, tmp, false);
        const Diff ds = compare565(ref, tmp, true);
        Serial.printf(
            "[basemap] hw jpeg yuv420+ppa rgb_swap=%d: hw_ms=%lu ppa_ms=%lu diff=%ld "
            "bias=%ld/%ld/%ld swapped_diff=%ld\n",
            sw, dec_ms, ppa_ms, static_cast<long>(d.mean_abs_x100),
            static_cast<long>(d.bias_r_x100), static_cast<long>(d.bias_g_x100),
            static_cast<long>(d.bias_b_x100), static_cast<long>(ds.mean_abs_x100));
        if (diffAcceptable(d) || diffAcceptable(ds)) {
          s_hw_state = HwState::OkPpa;
          s_ppa_rgb_swap = sw == 1;
          s_ppa_byte_swap = !diffAcceptable(d);
          verdict = "ok (yuv420 + PPA colour)";
        }
      }
    } else {
      Serial.println("[basemap] hw jpeg yuv420: decoder error");
    }
  }

  // Fallback: raw YUV 4:4:4 converted in software. The byte order inside each pixel is
  // not documented, so test all six.
  if (s_hw_state == HwState::Untested && ensureYuvBuf()) {
    const unsigned long ty = millis();
    const int ry = hwDecodeRaw(jpeg, len, JPEG_DECODE_OUT_FORMAT_YUV444, s_yuv_buf, kYuvBytes);
    const unsigned long yuv_hw_ms = millis() - ty;
    if (ry > 0) {
      static const uint8_t kPerms[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2},
                                           {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
      int best = -1;
      Diff best_d{};
      unsigned long conv_ms = 0;
      for (int p = 0; p < 6; ++p) {
        const unsigned long tc = millis();
        yuv444To565(s_yuv_buf, tmp, kSlotPixels, kPerms[p]);
        conv_ms = millis() - tc;
        const Diff d = compare565(ref, tmp, false);
        Serial.printf("[basemap] hw jpeg yuv444 order=%u%u%u diff=%ld bias=%ld/%ld/%ld\n",
                      kPerms[p][0], kPerms[p][1], kPerms[p][2],
                      static_cast<long>(d.mean_abs_x100), static_cast<long>(d.bias_r_x100),
                      static_cast<long>(d.bias_g_x100), static_cast<long>(d.bias_b_x100));
        if (best < 0 || d.mean_abs_x100 < best_d.mean_abs_x100) {
          best = p;
          best_d = d;
        }
      }
      const uint8_t* c = s_yuv_buf + (static_cast<size_t>(360) * kPixelSize + 360) * 3;
      Serial.printf(
          "[basemap] hw jpeg yuv444: hw_ms=%lu conv_ms=%lu center bytes=%02x %02x %02x "
          "ref565=%04x best=%d diff=%ld\n",
          yuv_hw_ms, conv_ms, c[0], c[1], c[2], ref[360 * kPixelSize + 360], best,
          static_cast<long>(best_d.mean_abs_x100));
      if (best >= 0 && diffAcceptable(best_d)) {
        memcpy(s_yuv_pos, kPerms[best], sizeof(s_yuv_pos));
        s_hw_state = HwState::OkYuv;
        verdict = "ok (yuv444 + software colour)";
      }
    } else {
      Serial.println("[basemap] hw jpeg yuv444: decoder error");
    }
  }
  if (s_hw_state == HwState::Untested) {
    s_hw_state = HwState::Off;
  }
  if (s_hw_state != HwState::OkYuv && s_hw_state != HwState::OkPpa && s_yuv_buf != nullptr) {
    psramFree(s_yuv_buf);  // only the YUV paths keep the 1.5 MB scratch
    s_yuv_buf = nullptr;
  }
  Serial.printf("[basemap] hw jpeg self-check: %s\n", verdict);
  psramFree(tmp);
}
#endif  // FS_BASEMAP_HW_JPEG

/** Decode the bake at `path` (coverage `mi` miles) into a cache slot. */
CacheSlot* decodeFileToSlot(const char* path, uint8_t mi) {
  const unsigned long t0 = millis();
  uint8_t* jpeg = nullptr;
  size_t len = 0;
  bool jpeg_cached = false;  // owned by s_raw: do not free here
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
  RawJpeg* raw_free = nullptr;
  for (RawJpeg& raw : s_raw) {
    if (raw.data != nullptr && strcmp(raw.path, path) == 0) {
      jpeg = raw.data;
      len = raw.len;
      jpeg_cached = true;
      break;
    }
    if (raw.data == nullptr && raw_free == nullptr) {
      raw_free = &raw;
    }
  }
#endif

  if (jpeg == nullptr) {
    if (!ensureFs() || !LittleFS.exists(path)) {
      return nullptr;
    }
    File f = LittleFS.open(path, "r");
    if (!f) {
      return nullptr;
    }
    len = f.size();
    if (len < 64 || len > kMaxJpegBytes) {
      f.close();
      Serial.printf("[basemap] reject size %u\n", static_cast<unsigned>(len));
      return nullptr;
    }
#if FS_BASEMAP_HW_JPEG
    if (s_hw_state != HwState::Off) {
      jpeg = allocJpegInput(len);  // DMA-readable input for the hardware decoder
    }
#endif
    if (jpeg == nullptr) {
      jpeg = static_cast<uint8_t*>(psramAlloc(len));
    }
    if (jpeg == nullptr) {
      f.close();
      Serial.println("[basemap] jpeg alloc failed");
      return nullptr;
    }
    const size_t got = f.read(jpeg, len);
    f.close();
    if (got != len) {
      psramFree(jpeg);
      return nullptr;
    }
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
    if (raw_free != nullptr && strlen(path) < sizeof(raw_free->path)) {
      strncpy(raw_free->path, path, sizeof(raw_free->path) - 1);
      raw_free->data = jpeg;
      raw_free->len = len;
      jpeg_cached = true;
    }
#endif
  }
  const unsigned long read_ms = millis() - t0;

  CacheSlot* slot = claimSlot();
  if (slot == nullptr) {
    if (!jpeg_cached) {
      psramFree(jpeg);
    }
    Serial.println("[basemap] pixel alloc failed");
    return nullptr;
  }

  const unsigned long t_decode = millis();
  bool ok = false;
  bool used_hw = false;
#if FS_BASEMAP_HW_JPEG
  if (s_hw_state == HwState::Ok || s_hw_state == HwState::OkSwapped ||
      s_hw_state == HwState::OkYuv || s_hw_state == HwState::OkPpa) {
    const int r = hwDecode(jpeg, len, slot->px);
    if (r > 0) {
      ok = true;
      used_hw = true;
      s_hw_failures = 0;
    } else if (r < 0 && ++s_hw_failures >= 3) {
      s_hw_state = HwState::Off;
      Serial.println("[basemap] hw jpeg off after 3 errors, using JPEGDEC");
    }
  }
#endif
  if (!ok) {
    ok = softDecode(jpeg, len, slot->px);
  }
  const unsigned long decode_ms = millis() - t_decode;
#if FS_BASEMAP_HW_JPEG
  if (ok && !used_hw && s_hw_state == HwState::Untested) {
    hwSelfCheck(jpeg, len, slot->px);
  }
#endif
  if (!jpeg_cached) {
    psramFree(jpeg);
  }
  if (!ok) {
    freeSlot(slot);
    return nullptr;
  }
  slot->valid = true;
  slot->mi = mi;
  slot->stamp = ++s_slot_clock;
  Serial.printf("[basemap] decoded %dx%d jpeg=%uB decode_ms=%lu total_ms=%lu\n", kPixelSize,
                kPixelSize, static_cast<unsigned>(len), decode_ms, millis() - t0);
  Serial.printf("[perf] decode ms=%lu read_ms=%lu hw=%d bake=%u mi file_cached=%d\n", decode_ms,
                read_ms, used_hw ? 1 : 0, static_cast<unsigned>(mi), jpeg_cached ? 1 : 0);
  return slot;
}

bool anglesNear(uint16_t a, uint16_t b) {
  int d = static_cast<int>(a) - static_cast<int>(b);
  while (d < -180) {
    d += 360;
  }
  while (d > 180) {
    d -= 360;
  }
  return d >= -2 && d <= 2;
}

// ---- Bilinear zoom ----------------------------------------------------------------
// The output is a uniform stretch of the center of the bake, so each output column
// always samples the same two source columns with the same weight, and each output row
// the same two source rows: both go in tables once per blit. Weights are 1/32 px. Each
// source row is interpolated horizontally once (rows repeat while zoomed in), with the
// three channels kept apart in two words so nothing is rounded until the end.

/** Same sample position as the old float code: center + (i - center) * scale. */
struct Taps {
  uint16_t a[kPixelSize];   // left / top source index
  uint16_t b[kPixelSize];   // right / bottom source index
  uint8_t w[kPixelSize];    // weight of b, 0..32
  bool inside[kPixelSize];  // false: outside the bake, draw black
};

void buildTaps(float scale, Taps* t) {
  const float c = static_cast<float>(kPixelSize - 1) * 0.5f;
  const int lim = kPixelSize - 1;
  for (int i = 0; i < kPixelSize; ++i) {
    const float s = c + (static_cast<float>(i) - c) * scale;
    t->inside[i] = !(s < -1.0f || s > static_cast<float>(kPixelSize));
    const int i0 = static_cast<int>(floorf(s));
    int wt = static_cast<int>((s - static_cast<float>(i0)) * 32.0f + 0.5f);
    wt = wt < 0 ? 0 : (wt > 32 ? 32 : wt);
    const int ia = i0 < 0 ? 0 : (i0 > lim ? lim : i0);
    const int ib = i0 + 1 < 0 ? 0 : (i0 + 1 > lim ? lim : i0 + 1);
    t->a[i] = static_cast<uint16_t>(ia);
    t->b[i] = static_cast<uint16_t>(ib);
    t->w[i] = static_cast<uint8_t>(wt);
  }
}

/** RGB565 -> blue in bits 0-4, red in bits 16-20 (room to scale by 1024). */
inline uint32_t spreadRb(uint16_t c) {
  return (static_cast<uint32_t>(c) & 0x001Fu) | ((static_cast<uint32_t>(c) & 0xF800u) << 5);
}

/** One source row, interpolated at every output column (fields scaled by 32). */
void hRow(const uint16_t* src_row, const Taps& tx, uint32_t* rb, uint16_t* g) {
  for (int x = 0; x < kPixelSize; ++x) {
    const uint16_t ca = src_row[tx.a[x]];
    const uint16_t cb = src_row[tx.b[x]];
    const uint32_t wb = tx.w[x];
    const uint32_t wa = 32u - wb;
    rb[x] = spreadRb(ca) * wa + spreadRb(cb) * wb;
    g[x] = static_cast<uint16_t>(((ca >> 5) & 0x3Fu) * wa + ((cb >> 5) & 0x3Fu) * wb);
  }
}

struct ZoomScratch {
  Taps tx;
  Taps ty;
  uint32_t rb[2][kPixelSize];
  uint16_t g[2][kPixelSize];
};
ZoomScratch* s_zoom = nullptr;  // ~14 KB, PSRAM, kept between blits

void zoomBlit(const uint16_t* src, uint16_t* dst, float scale) {
  if (s_zoom == nullptr) {
    s_zoom = static_cast<ZoomScratch*>(psramAlloc(sizeof(ZoomScratch)));
    if (s_zoom == nullptr) {
      s_zoom = static_cast<ZoomScratch*>(malloc(sizeof(ZoomScratch)));
    }
    if (s_zoom == nullptr) {
      memset(dst, 0, kSlotBytes);
      return;
    }
  }
  ZoomScratch& z = *s_zoom;
  buildTaps(scale, &z.tx);
  buildTaps(scale, &z.ty);  // square, same scale both ways
  // Line buffer k holds source row row_of[k]; top/bottom pick which is which.
  int row_of[2] = {-1, -1};

  for (int y = 0; y < kPixelSize; ++y) {
    uint16_t* out = dst + static_cast<size_t>(y) * kPixelSize;
    if (!z.ty.inside[y]) {
      memset(out, 0, kPixelSize * sizeof(uint16_t));
      continue;
    }
    const int ra = z.ty.a[y];
    const int rbw = z.ty.b[y];
    int top = row_of[0] == ra ? 0 : (row_of[1] == ra ? 1 : -1);
    int bot = row_of[0] == rbw ? 0 : (row_of[1] == rbw ? 1 : -1);
    if (top < 0) {
      top = bot == 0 ? 1 : 0;  // the buffer the bottom row does not need
      hRow(src + static_cast<size_t>(ra) * kPixelSize, z.tx, z.rb[top], z.g[top]);
      row_of[top] = ra;
      if (bot == top) {
        bot = -1;
      }
    }
    if (bot < 0) {
      bot = ra == rbw ? top : 1 - top;
      if (row_of[bot] != rbw) {
        hRow(src + static_cast<size_t>(rbw) * kPixelSize, z.tx, z.rb[bot], z.g[bot]);
        row_of[bot] = rbw;
      }
    }
    const uint32_t wb = z.ty.w[y];
    const uint32_t wa = 32u - wb;
    const uint32_t* rb0 = z.rb[top];
    const uint32_t* rb1 = z.rb[bot];
    const uint16_t* g0 = z.g[top];
    const uint16_t* g1 = z.g[bot];
    for (int x = 0; x < kPixelSize; ++x) {
      if (!z.tx.inside[x]) {
        out[x] = 0;
        continue;
      }
      // Fields now scaled by 1024; +512 rounds half up, like the old lroundf.
      const uint32_t rb = rb0[x] * wa + rb1[x] * wb + 0x02000200u;
      const uint32_t g = static_cast<uint32_t>(g0[x]) * wa + static_cast<uint32_t>(g1[x]) * wb + 512u;
      out[x] = static_cast<uint16_t>((((rb >> 26) & 0x1Fu) << 11) | (((g >> 10) & 0x3Fu) << 5) |
                                     ((rb >> 10) & 0x1Fu));
    }
  }
}

}  // namespace

void init() {
  ensureFs();
  loadPrefs();
  noteFileUnknown();
  clearMetaIfOrphaned();
}

bool enabled() { return s_enabled; }

void saveEnabledFromForm(const char* checkbox_value) {
  bool on = false;
  if (checkbox_value != nullptr && checkbox_value[0] != '\0') {
    if ((checkbox_value[0] == 'T' || checkbox_value[0] == 't') && checkbox_value[1] == '\0') {
      on = true;
    } else if (strcmp(checkbox_value, "on") == 0) {
      on = true;
    }
  }
  const bool changed = on != s_enabled;
  s_enabled = on;
  persistEnabled();
  if (changed) {
    ui::radarDisplayInvalidateBasemap();
  }
}

namespace {

uint8_t parsePct(const char* value, uint8_t fallback, uint8_t max_pct) {
  if (value == nullptr || value[0] == '\0') {
    return fallback;
  }
  char* end = nullptr;
  const long v = strtol(value, &end, 10);
  if (end == value) {
    return fallback;
  }
  if (v < 0) {
    return 0;
  }
  if (v > static_cast<long>(max_pct)) {
    return max_pct;
  }
  return static_cast<uint8_t>(v);
}

void persistBakeAdjust() {
  Preferences prefs;
  if (!prefs.begin(kStoreNs, false)) {
    return;
  }
  prefs.putUChar(kContrastDarkKey, s_contrast_dark_pct);
  prefs.putUChar(kContrastLightKey, s_contrast_light_pct);
  prefs.putUChar(kWashVfrKey, s_wash_vfr_pct);
  prefs.end();
}

}  // namespace

uint8_t contrastPercentDark() { return s_contrast_dark_pct; }
uint8_t contrastPercentLight() { return s_contrast_light_pct; }
uint8_t washPercentVfr() { return s_wash_vfr_pct; }

void saveBakeAdjustFromForm(const char* dark_contrast_pct, const char* light_contrast_pct,
                            const char* vfr_wash_pct) {
  s_contrast_dark_pct = parsePct(dark_contrast_pct, s_contrast_dark_pct, kMaxContrastPct);
  s_contrast_light_pct = parsePct(light_contrast_pct, s_contrast_light_pct, kMaxContrastPct);
  s_wash_vfr_pct = parsePct(vfr_wash_pct, s_wash_vfr_pct, 100);
  persistBakeAdjust();
  Serial.printf("[basemap] contrast dark=%u%% light=%u%% vfr_wash=%u%%\n",
                static_cast<unsigned>(s_contrast_dark_pct),
                static_cast<unsigned>(s_contrast_light_pct),
                static_cast<unsigned>(s_wash_vfr_pct));
}

void invalidateCache() { freeCache(); }

Meta storedMeta() { return s_meta; }

Style storedStyle() {
  if (s_meta.valid) return s_meta.style;
  return config::kFactoryBasemapStyle <= static_cast<uint8_t>(Style::Voyager)
             ? static_cast<Style>(config::kFactoryBasemapStyle)
             : Style::Dark;
}

bool wantsDisplay() {
  return s_enabled && s_meta.valid && metaMatchesLive() && filePresent();
}

bool cacheReady() {
  for (const CacheSlot& slot : s_slots) {
    if (slot.valid) {
      return true;
    }
  }
  return false;
}

bool hasImage() {
  if (!s_meta.valid) {
    return false;
  }
  return filePresent();
}

bool metaMatchesLive() {
  if (!s_meta.valid || s_meta.range_miles == 0) {
    return false;
  }
  const double dlat = fabs(s_meta.lat - services::map_center::latitude());
  const double dlon = fabs(s_meta.lon - services::map_center::longitude());
  if (dlat > 0.00015 || dlon > 0.00015) {
    return false;
  }
  // Bake covers max range; zooming in is OK. Zooming past baked miles is not.
  if (ui::radar::scaleActiveMiles() > s_meta.range_miles) {
    return false;
  }
  if (!anglesNear(s_meta.facing_deg, ui::radar::facingDeg())) {
    return false;
  }
  return true;
}

bool heapOkToDecode() {
#if !defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
  // S3: the decode competes with TLS for internal heap and stalled the loop.
  // The 4C decodes into PSRAM in a fraction of a second, and with a bake per
  // zoom level it decodes on every zoom step: waiting for an idle HTTPS slot
  // (ADS-B polls nearly back to back) left the radar blank after zooming.
  if (services::https::busy()) {
    return false;
  }
#endif
  return ESP.getFreeHeap() >= kMinFreeHeapToDecode &&
         ESP.getMaxAllocHeap() >= kMinContigHeapToDecode;
}

bool blitRgb565(uint16_t* dst, int w, int h) {
  const unsigned long t0 = millis();
  if (dst == nullptr || w != kPixelSize || h != kPixelSize) {
    return false;
  }
  if (!s_enabled || !s_meta.valid || !metaMatchesLive()) {
    return false;
  }
  if (!filePresent()) {
    clearMetaIfOrphaned();
    return false;
  }
  const uint8_t live_mi = ui::radar::scaleActiveMiles();
  const uint8_t baked_mi = sourceMiles(live_mi);
  CacheSlot* slot = findSlot(baked_mi);
  if (slot == nullptr) {
    // Never decode on the radar stall path while TLS is active / heap is low —
    // JPEGDEC blocked the loop for ~18s in the field and left max_blk wedged.
    if (!heapOkToDecode()) {
      Serial.printf("[basemap] decode defer https=%d heap=%u max_blk=%u\n",
                    services::https::busy() ? 1 : 0, ESP.getFreeHeap(),
                    ESP.getMaxAllocHeap());
      return false;
    }
    const char* path = kPath;
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
    char detail[24];
    if (baked_mi != s_meta.range_miles) {
      detailPath(baked_mi, detail, sizeof(detail));
      path = detail;
    }
#endif
    slot = decodeFileToSlot(path, baked_mi);
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
    static uint8_t s_detail_fails[8] = {};
    const int fail_idx = rangeIndex(baked_mi);
#endif
    if (slot == nullptr) {
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
      if (path != kPath && fail_idx >= 0 && fail_idx < 8) {
        // Missing or repeatedly bad detail bake: forget it so the next frame uses the
        // main one. A one-off failure (a passing memory shortage) keeps it: dropping
        // the bit is saved to flash, which disabled a good bake until the next Make map.
        const bool missing = !ensureFs() || !LittleFS.exists(path);
        if (missing || ++s_detail_fails[fail_idx] >= 3) {
          s_detail_fails[fail_idx] = 0;
          s_detail_mask = static_cast<uint8_t>(s_detail_mask & ~(1U << fail_idx));
          persistDetailMask();
        }
      }
#endif
      return false;
    }
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
    if (fail_idx >= 0 && fail_idx < 8) {
      s_detail_fails[fail_idx] = 0;
    }
#endif
  } else {
    slot->stamp = ++s_slot_clock;
  }

  const unsigned long t_blit = millis();
  if (live_mi == baked_mi) {
    memcpy(dst, slot->px, kSlotBytes);
    const unsigned long now = millis();
    Serial.printf("[basemap] blit 1:1 ms=%lu\n", now - t0);
    Serial.printf("[perf] blit 1:1 ms=%lu total_ms=%lu\n", now - t_blit, now - t0);
    return true;
  }

  // Zoomed in vs bake: bilinear-sample the center crop and stretch to the panel.
  // Nearest-neighbor looked blocky; bilinear softens upscales (no new detail).
  zoomBlit(slot->px, dst, static_cast<float>(live_mi) / static_cast<float>(baked_mi));
  const unsigned long now = millis();
  Serial.printf("[basemap] blit zoom live=%u bake=%u bilinear ms=%lu\n",
                static_cast<unsigned>(live_mi), static_cast<unsigned>(baked_mi), now - t0);
  Serial.printf("[perf] blit zoom ms=%lu total_ms=%lu\n", now - t_blit, now - t0);
  return true;
}

void statusText(char* buf, size_t len) {
  if (buf == nullptr || len == 0) {
    return;
  }
  if (!hasImage()) {
    snprintf(buf, len, "No basemap on flash.");
    return;
  }
  const char* style_name = "dark";
  if (s_meta.style == Style::Light) {
    style_name = "light";
  } else if (s_meta.style == Style::Vfr) {
    style_name = "vfr";
  } else if (s_meta.style == Style::Voyager) {
    style_name = "voyager";
  }
  if (!metaMatchesLive()) {
    const uint8_t live_mi = ui::radar::scaleActiveMiles();
    if (live_mi > s_meta.range_miles) {
      snprintf(buf, len,
               "Stored %s @ %u mi — live range %u mi exceeds bake (regenerate).",
               style_name, static_cast<unsigned>(s_meta.range_miles),
               static_cast<unsigned>(live_mi));
    } else {
      snprintf(buf, len,
               "Stored %s: %.4f,%.4f @ %u mi facing %u — does not match current "
               "center/facing (regenerate).",
               style_name, s_meta.lat, s_meta.lon,
               static_cast<unsigned>(s_meta.range_miles),
               static_cast<unsigned>(s_meta.facing_deg));
    }
    return;
  }
  const uint8_t live_mi = ui::radar::scaleActiveMiles();
  if (live_mi < s_meta.range_miles) {
    snprintf(buf, len,
             "Ready (%s): %.4f,%.4f bake %u mi (showing %u mi zoom) facing %u%s",
             style_name, s_meta.lat, s_meta.lon,
             static_cast<unsigned>(s_meta.range_miles), static_cast<unsigned>(live_mi),
             static_cast<unsigned>(s_meta.facing_deg),
             s_enabled ? " (enabled)" : " (disabled)");
  } else {
    snprintf(buf, len, "Ready (%s): %.4f,%.4f @ %u mi facing %u%s", style_name,
             s_meta.lat, s_meta.lon, static_cast<unsigned>(s_meta.range_miles),
             static_cast<unsigned>(s_meta.facing_deg),
             s_enabled ? " (enabled)" : " (disabled)");
  }
}

void uploadBegin() {
  uploadAbort();
  if (!ensureFs()) {
    s_upload_failed = true;
    return;
  }
  LittleFS.remove(kTmpPath);
  s_upload = LittleFS.open(kTmpPath, "w");
  if (!s_upload) {
    s_upload_failed = true;
    return;
  }
  s_upload_active = true;
  s_upload_bytes = 0;
  s_upload_failed = false;
}

bool uploadWrite(const uint8_t* data, size_t len) {
  if (!s_upload_active || s_upload_failed || !s_upload) {
    return false;
  }
  if (s_upload_bytes + len > kMaxJpegBytes) {
    Serial.println("[basemap] upload too large");
    s_upload_failed = true;
    uploadAbort();
    return false;
  }
  const size_t wrote = s_upload.write(data, len);
  s_upload_bytes += wrote;
  if (wrote != len) {
    s_upload_failed = true;
    uploadAbort();
    return false;
  }
  return true;
}

bool uploadFinish(size_t total_bytes, Style style, uint8_t range_miles) {
  if (!s_upload_active || s_upload_failed || !s_upload) {
    uploadAbort();
    return false;
  }
  s_upload.close();
  s_upload_active = false;
  (void)total_bytes;
  if (s_upload_bytes < 64) {
    LittleFS.remove(kTmpPath);
    return false;
  }
  LittleFS.remove(kPath);
  if (!LittleFS.rename(kTmpPath, kPath)) {
    File src = LittleFS.open(kTmpPath, "r");
    File dst = LittleFS.open(kPath, "w");
    if (!src || !dst) {
      if (src) {
        src.close();
      }
      if (dst) {
        dst.close();
      }
      LittleFS.remove(kTmpPath);
      return false;
    }
    uint8_t buf[512];
    while (src.available()) {
      const int n = src.read(buf, sizeof(buf));
      if (n <= 0) {
        break;
      }
      dst.write(buf, static_cast<size_t>(n));
    }
    src.close();
    dst.close();
    LittleFS.remove(kTmpPath);
  }

  noteFilePresent();
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
  removeDetailFiles();  // they belong to the previous bake; the page re-sends them
#endif
  s_meta.lat = services::map_center::latitude();
  s_meta.lon = services::map_center::longitude();
  uint8_t mi = range_miles;
  bool mi_ok = false;
  for (size_t i = 0; i < ui::radar::kRangeMileOptionCount; ++i) {
    if (ui::radar::kRangeMileOptions[i] == mi) {
      mi_ok = true;
      break;
    }
  }
  if (!mi_ok) {
    mi = ui::radar::scaleActiveMiles();
    for (size_t i = 0; i < ui::radar::kRangeMileOptionCount; ++i) {
      if (ui::radar::kRangeMileOptions[i] == mi) {
        mi_ok = true;
        break;
      }
    }
    if (!mi_ok) {
      mi = ui::radar::kRangeMileOptions[ui::radar::kRangeMileOptionCount - 1];
    }
  }
  s_meta.range_miles = mi;
  s_meta.facing_deg = ui::radar::facingDeg();
  if (style == Style::Light) {
    s_meta.style = Style::Light;
  } else if (style == Style::Vfr) {
    s_meta.style = Style::Vfr;
  } else if (style == Style::Voyager) {
    s_meta.style = Style::Voyager;
  } else {
    s_meta.style = Style::Dark;
  }
  s_meta.valid = true;
  persistMeta();
  freeCache();
  s_enabled = true;
  persistEnabled();
  Serial.printf("[basemap] saved %u bytes meta=%.5f,%.5f mi=%u fac=%u sty=%u\n",
                static_cast<unsigned>(s_upload_bytes), s_meta.lat, s_meta.lon,
                static_cast<unsigned>(s_meta.range_miles),
                static_cast<unsigned>(s_meta.facing_deg),
                static_cast<unsigned>(s_meta.style));
  ui::radarDisplayInvalidateBasemap();
  return true;
}

#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
bool uploadFinishDetail(uint8_t range_miles) {
  if (!s_upload_active || s_upload_failed || !s_upload) {
    uploadAbort();
    return false;
  }
  s_upload.close();
  s_upload_active = false;
  const int idx = rangeIndex(range_miles);
  // Detail bakes only make sense under a matching main bake, for a smaller range.
  if (s_upload_bytes < 64 || idx < 0 || !s_meta.valid || range_miles >= s_meta.range_miles) {
    LittleFS.remove(kTmpPath);
    return false;
  }
  char path[24];
  detailPath(range_miles, path, sizeof(path));
  LittleFS.remove(path);
  if (!LittleFS.rename(kTmpPath, path)) {
    LittleFS.remove(kTmpPath);
    return false;
  }
  s_detail_mask = static_cast<uint8_t>(s_detail_mask | (1U << idx));
  persistDetailMask();
  freeCache();  // a sharper bake may now cover the live range; drop older decodes
  Serial.printf("[basemap] detail %u mi saved (%u bytes, mask 0x%02X)\n",
                static_cast<unsigned>(range_miles), static_cast<unsigned>(s_upload_bytes),
                static_cast<unsigned>(s_detail_mask));
  ui::radarDisplayInvalidateBasemap();
  return true;
}
#endif

void uploadAbort() {
  if (s_upload) {
    s_upload.close();
  }
  s_upload_active = false;
  if (ensureFs()) {
    LittleFS.remove(kTmpPath);
  }
}

bool clear() {
  freeCache();
  if (!ensureFs()) {
    noteFileAbsent();
    return false;
  }
  LittleFS.remove(kPath);
  LittleFS.remove(kTmpPath);
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
  removeDetailFiles();
#endif
  noteFileAbsent();
  s_meta = {};
  persistMeta();
  s_enabled = false;
  persistEnabled();
  ui::radarDisplayInvalidateBasemap();
  return true;
}

}  // namespace services::basemap
