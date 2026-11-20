#pragma once

// Big file-scope objects that would otherwise sit in internal RAM (.bss / .data).
//
// On the ESP32-P4 the Wi-Fi link (ESP-Hosted over SDIO), lwIP and mbedTLS all need
// internal, DMA-capable RAM. The first hardware boot had ~176 KB of statics in
// internal RAM and only ~10 KB of main DRAM free, so any burst of incoming data
// (a phone loading the setup page, the first larger download) stalled the link.
// These macros move a big static into PSRAM on the P4 and leave it a plain static
// everywhere else (S3 boards, the desktop simulator).
//
// Use, at file scope inside the usual anonymous namespace:
//
//   using AircraftArray = Aircraft[kMaxAircraft];
//   FS_BIG_STATIC(AircraftArray, s_aircraft)
//   #define s_aircraft FS_BIG_STATIC_REF(s_aircraft)
//
// After that, `s_aircraft` behaves like the original object: indexing, sizeof(),
// array-to-pointer decay and member calls all work. The object is created
// (value-initialized, so default member initializers run) on first use, which must
// be at run time (after PSRAM is up), not from another static's constructor.

#include <cstddef>
#include <cstdlib>
#include <new>

#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C) && !defined(FLIGHTSCNR_SIM)

#include <esp_heap_caps.h>

namespace fs_util {

template <typename T>
T* constructInPsram() {
  // A little slack in case the compiler adds bookkeeping to an array placement-new.
  const size_t bytes = sizeof(T) + 16;
  void* mem = heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (mem == nullptr) {
    mem = calloc(1, bytes);  // PSRAM missing or full: fall back to the normal heap
  }
  if (mem == nullptr) {
    abort();
  }
  ::new (mem) T();
  return static_cast<T*>(mem);
}

}  // namespace fs_util

#define FS_BIG_STATIC(Type, name)                         \
  Type& name##_big_static_ref() {                         \
    static Type* const p = ::fs_util::constructInPsram<Type>(); \
    return *p;                                            \
  }
#define FS_BIG_STATIC_REF(name) (name##_big_static_ref())

#else  // S3 boards and the simulator: an ordinary static.

#define FS_BIG_STATIC(Type, name) Type name##_big_static_storage{};
#define FS_BIG_STATIC_REF(name) name##_big_static_storage

#endif
