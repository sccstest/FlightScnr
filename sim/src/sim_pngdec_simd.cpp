// PNGdec picks its ESP32-S3 SIMD RGB565 path from ARDUINO_ESP32S3_DEV, which the
// simulator inherits from the base env. The assembly can't build here, so this is
// the same conversion in plain C (RGBA8888 -> RGB565, alpha ignored).
#include <cstdint>

extern "C" void s3_rgb565(uint8_t* pSrc, uint8_t* pDest, int iCount, bool bBigEndian) {
  uint16_t* out = reinterpret_cast<uint16_t*>(pDest);
  for (int i = 0; i < iCount; ++i, pSrc += 4) {
    uint16_t px = static_cast<uint16_t>(((pSrc[0] >> 3) << 11) | ((pSrc[1] >> 2) << 5) |
                                        (pSrc[2] >> 3));
    if (bBigEndian) {
      px = static_cast<uint16_t>((px >> 8) | (px << 8));
    }
    out[i] = px;
  }
}
