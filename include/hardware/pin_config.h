#pragma once

/**
 * Compile-time board pin map.
 *
 * Selected by PlatformIO build flag:
 *   -D FLIGHTSCNR_BOARD_TENCODER_PRO   (LilyGO T-Encoder Pro — default)
 *   -D FLIGHTSCNR_BOARD_WAVESHARE_KNOB_18  (Waveshare ESP32-S3-Knob-Touch-LCD-1.8)
 *
 * Run `pio run` (env: auto) to pick a connected board, or `pio run -e <board>`.
 */

#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
#include "boards/waveshare_p4_4c.h"
#elif defined(FLIGHTSCNR_BOARD_WAVESHARE_KNOB_18)
#include "boards/waveshare_knob_18.h"
#elif defined(FLIGHTSCNR_BOARD_TENCODER_PRO)
#include "boards/tencoder_pro.h"
#else
/* Native/host unit tests and accidental bare builds default to T-Encoder Pro. */
#include "boards/tencoder_pro.h"
#endif

/* Boards with an I2S codec + speaker (alert chimes) set this to 1 in their header. */
#ifndef FLIGHTSCNR_HAS_SPEAKER
#define FLIGHTSCNR_HAS_SPEAKER 0
#endif

/* Boards without a rotary encoder (touch only) set this to 0 in their header. */
#ifndef FLIGHTSCNR_HAS_ENCODER
#define FLIGHTSCNR_HAS_ENCODER 1
#endif
