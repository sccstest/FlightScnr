/*
 * Waveshare ESP32-P4-WIFI6-Touch-LCD-4C speaker output: ES8311 codec + I2S + speaker PA.
 *
 * The ES8311 register sequence (reset, clock dividers for MCLK = 256 x 16 kHz, I2S slave
 * 16-bit format, DAC/HP power-up, unmute) is a compact port of Espressif's es8311.c as
 * shipped in Waveshare's ESP32-P4-WIFI6-Touch-LCD-XC example 09_Audio_Playback:
 *   SPDX-FileCopyrightText: 2015-2022 Espressif Systems (Shanghai) CO LTD
 *   SPDX-License-Identifier: Apache-2.0
 * Here it talks over the Arduino Wire bus that the GT9271 touch driver already owns
 * (src/hardware/input_p4.cpp), only from setup(), so the two never race.
 *
 * Playback runs in its own FreeRTOS task: playChime() just hands over the request, the
 * task renders the chime (shared synth) and streams it to the I2S DMA. The PA is switched
 * on only while a chime plays, to keep the speaker silent (no hiss) the rest of the time.
 */
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C) && !defined(FLIGHTSCNR_SIM)

#include "hardware/audio_out.h"

#include <Arduino.h>
#include <Wire.h>
#include <driver/i2s_std.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>

#include "hardware/pin_config.h"

namespace hardware::audio_out {
namespace {

constexpr uint32_t kRate = chime::kSampleRate;  // 16 kHz
constexpr uint32_t kMclkHz = kRate * 256;       // 4.096 MHz
constexpr size_t kChunkFrames = 256;
constexpr uint32_t kPaSettleMs = 20;
constexpr uint32_t kTailSilenceMs = 120;  // let the DMA ring drain before the PA goes off

// ES8311 DAC volume register 0x32: 0x00 = -95.5 dB ... 0xBF = 0 dB ... 0xFF = +32 dB
// (0.5 dB steps). The chime volume steps scale the samples, so the codec stays at 0 dB.
constexpr uint8_t kDacVolume0dB = 0xBF;

i2s_chan_handle_t s_tx = nullptr;
TaskHandle_t s_task = nullptr;
int16_t* s_pcm = nullptr;
bool s_ready = false;
// Pending request: kind in bits 0-7, gain x1000 in bits 8-23.
std::atomic<uint32_t> s_request{0};

bool codecWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ES8311_I2C_ADDRESS);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool codecRead(uint8_t reg, uint8_t* val) {
  Wire.beginTransmission(ES8311_I2C_ADDRESS);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(static_cast<uint8_t>(ES8311_I2C_ADDRESS), static_cast<uint8_t>(1)) != 1) {
    return false;
  }
  *val = static_cast<uint8_t>(Wire.read());
  return true;
}

bool codecUpdate(uint8_t reg, uint8_t keep_mask, uint8_t set_bits) {
  uint8_t v = 0;
  if (!codecRead(reg, &v)) {
    return false;
  }
  return codecWrite(reg, static_cast<uint8_t>((v & keep_mask) | set_bits));
}

/** es8311_init() + sample_frequency_config() for MCLK 4.096 MHz / 16 kHz, 16-bit I2S. */
bool codecInit() {
  bool ok = true;
  // Reset to defaults, then power-on command.
  ok &= codecWrite(0x00, 0x1F);
  delay(20);
  ok &= codecWrite(0x00, 0x00);
  ok &= codecWrite(0x00, 0x80);
  if (!ok) {
    return false;
  }
  // Clock manager: all clocks on, MCLK from the MCLK pin, not inverted.
  ok &= codecWrite(0x01, 0x3F);
  ok &= codecUpdate(0x06, static_cast<uint8_t>(~0x20), 0x00);  // SCLK not inverted
  // Coefficients for {mclk 4096000, rate 16000}: pre_div 1, pre_multi 1x, adc/dac div 1,
  // single speed, lrck 0x00FF, bclk_div 4, adc/dac osr 0x10.
  ok &= codecUpdate(0x02, 0x07, (0 << 5) | (0 << 3));
  ok &= codecWrite(0x03, (0 << 6) | 0x10);
  ok &= codecWrite(0x04, 0x10);
  ok &= codecWrite(0x05, 0x00);
  ok &= codecUpdate(0x06, 0xE0, 4 - 1);
  ok &= codecUpdate(0x07, 0xC0, 0x00);
  ok &= codecWrite(0x08, 0xFF);
  // Serial port: slave, I2S, 16-bit in and out.
  ok &= codecUpdate(0x00, 0xBF, 0x00);
  ok &= codecWrite(0x09, 3 << 2);
  ok &= codecWrite(0x0A, 3 << 2);
  // Power up analog, ADC modulator, DAC, HP drive; bypass ADC/DAC equalizers.
  ok &= codecWrite(0x0D, 0x01);
  ok &= codecWrite(0x0E, 0x02);
  ok &= codecWrite(0x12, 0x00);
  ok &= codecWrite(0x13, 0x10);
  ok &= codecWrite(0x1C, 0x6A);
  ok &= codecWrite(0x37, 0x08);
  // Microphone path as in the reference (analog mic, ADC gain); unused here.
  ok &= codecWrite(0x17, 0xC8);
  ok &= codecWrite(0x14, 0x1A);
  // DAC volume 0 dB and unmute.
  ok &= codecWrite(0x32, kDacVolume0dB);
  ok &= codecUpdate(0x31, static_cast<uint8_t>(~0x60), 0x00);
  return ok;
}

bool i2sInit() {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = 6;
  chan_cfg.dma_frame_num = 240;
  chan_cfg.auto_clear = true;  // underrun plays silence, not the last buffer again
  esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, nullptr);
  if (err != ESP_OK) {
    Serial.printf("[audio] i2s_new_channel failed: %s\n", esp_err_to_name(err));
    return false;
  }
  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kRate),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                      I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = static_cast<gpio_num_t>(AUDIO_I2S_MCLK),
              .bclk = static_cast<gpio_num_t>(AUDIO_I2S_BCLK),
              .ws = static_cast<gpio_num_t>(AUDIO_I2S_LRCK),
              .dout = static_cast<gpio_num_t>(AUDIO_I2S_DOUT),
              .din = I2S_GPIO_UNUSED,
              .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
          },
  };
  std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;  // 4.096 MHz MCLK for the ES8311
  err = i2s_channel_init_std_mode(s_tx, &std_cfg);
  if (err == ESP_OK) {
    err = i2s_channel_enable(s_tx);
  }
  if (err != ESP_OK) {
    Serial.printf("[audio] i2s std init failed: %s\n", esp_err_to_name(err));
    i2s_del_channel(s_tx);
    s_tx = nullptr;
    return false;
  }
  return true;
}

void writeSilence(uint32_t ms) {
  int16_t zeros[kChunkFrames * 2] = {};
  size_t frames = kRate * ms / 1000;
  while (frames > 0) {
    const size_t n = frames < kChunkFrames ? frames : kChunkFrames;
    size_t written = 0;
    i2s_channel_write(s_tx, zeros, n * 2 * sizeof(int16_t), &written, portMAX_DELAY);
    frames -= n;
  }
}

/** Stream mono PCM as stereo frames. Returns false if a newer request cut it short. */
bool streamPcm(const int16_t* pcm, size_t samples) {
  int16_t frame[kChunkFrames * 2];
  size_t pos = 0;
  while (pos < samples) {
    if (ulTaskNotifyTake(pdTRUE, 0) > 0) {
      return false;  // newer request: the caller renders and plays that one instead
    }
    const size_t n = (samples - pos) < kChunkFrames ? (samples - pos) : kChunkFrames;
    for (size_t i = 0; i < n; ++i) {
      frame[i * 2] = pcm[pos + i];
      frame[i * 2 + 1] = pcm[pos + i];
    }
    size_t written = 0;
    i2s_channel_write(s_tx, frame, n * 2 * sizeof(int16_t), &written, portMAX_DELAY);
    pos += n;
  }
  return true;
}

void audioTask(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    digitalWrite(AUDIO_PA_EN, HIGH);
    writeSilence(kPaSettleMs);
    bool finished = false;
    while (!finished) {
      const uint32_t req = s_request.load();
      const auto kind = static_cast<chime::Kind>(req & 0xFF);
      const float gain = static_cast<float>((req >> 8) & 0xFFFF) / 1000.0f;
      const size_t n = chime::render(kind, gain, s_pcm, chime::kMaxSamples);
      finished = streamPcm(s_pcm, n);
    }
    writeSilence(kTailSilenceMs);
    if (ulTaskNotifyTake(pdTRUE, 0) > 0) {
      xTaskNotifyGive(s_task);  // a request landed during the tail: keep the PA on
      continue;
    }
    digitalWrite(AUDIO_PA_EN, LOW);
  }
}

}  // namespace

bool init() {
  if (s_ready) {
    return true;
  }
  pinMode(AUDIO_PA_EN, OUTPUT);
  digitalWrite(AUDIO_PA_EN, LOW);
  s_pcm = static_cast<int16_t*>(
      heap_caps_malloc(chime::kMaxSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM));
  if (s_pcm == nullptr) {
    Serial.println("[audio] no memory for the chime buffer");
    return false;
  }
  // MCLK must be running before the codec's clock manager is configured.
  if (!i2sInit()) {
    return false;
  }
  if (!codecInit()) {
    Serial.println("[audio] ES8311 not responding at 0x18; speaker disabled");
    return false;
  }
  if (xTaskCreatePinnedToCore(audioTask, "audio", 6144, nullptr, 3, &s_task, 0) != pdPASS) {
    Serial.println("[audio] task create failed");
    return false;
  }
  s_ready = true;
  Serial.println("[audio] ES8311 ready (16 kHz, speaker PA on GPIO 53)");
  return true;
}

bool playChime(chime::Kind kind, float gain) {
  if (!s_ready) {
    return false;
  }
  if (gain < 0.0f) gain = 0.0f;
  if (gain > 1.0f) gain = 1.0f;
  const uint32_t g = static_cast<uint32_t>(gain * 1000.0f + 0.5f);
  s_request.store(static_cast<uint32_t>(kind) | (g << 8));
  xTaskNotifyGive(s_task);
  return true;
}

}  // namespace hardware::audio_out

#endif  // FLIGHTSCNR_BOARD_WAVESHARE_P4_4C && !FLIGHTSCNR_SIM
