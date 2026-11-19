// UNTESTED, and not needed so far: the factory C6 firmware works with Arduino 3.3.11.
// USB <-> UART bridge from the ESP32-P4 to its on-board ESP32-C6, for flashing the C6
// without a separate USB-serial adapter.
//
// Wiring (Waveshare 4C): plug an SH1.0 4-pin cable into J4 (the C6 flashing header) and
// run its wires to the 40-pin header:
//   J4 pin 1  C6_U0TXD  ->  P4 GPIO21  (P4 RX)
//   J4 pin 2  C6_U0RXD  ->  P4 GPIO20  (P4 TX)
//   J4 pin 3  GND       ->  any GND pin
//   J4 pin 4  C6_IO9    ->  P4 GPIO22  (boot strap; driven low for download mode)
// C6_CHIP_PU (C6 enable/reset) is already wired to P4 GPIO54 on the board.
//
// The PC talks to the bridge on the P4's USB Serial/JTAG port (the "USB" Type-C, COM4 here).
// Do not let esptool toggle DTR/RTS on that port: it can reset the P4 itself. Use
//   esptool --port COM4 --baud 115200 --before no-reset --after no-reset ...
// The UART runs at a fixed 115200 (the ROM's default), so keep --baud 115200.
//
// In-band commands (sent as text on COM4; also forwarded, which the C6 ROM ignores):
//   "~C6DL~"   reset the C6 into its ROM download mode (also done at power-up)
//   "~C6RUN~"  reset the C6 into its normal firmware, to read its boot log

#include <Arduino.h>

namespace {

constexpr int kPinTx = 20;     // to C6 U0RXD
constexpr int kPinRx = 21;     // from C6 U0TXD
constexpr int kPinIo9 = 22;    // C6 strapping pin: low at reset = download mode
constexpr int kPinC6En = 54;   // C6 CHIP_PU (low = held in reset)
constexpr uint32_t kBaud = 115200;

HardwareSerial c6(1);

void resetC6(bool download) {
  pinMode(kPinIo9, OUTPUT);
  digitalWrite(kPinIo9, download ? LOW : HIGH);
  pinMode(kPinC6En, OUTPUT);
  digitalWrite(kPinC6En, LOW);
  delay(100);
  digitalWrite(kPinC6En, HIGH);
  delay(150);  // strap is sampled shortly after EN rises
  if (!download) {
    pinMode(kPinIo9, INPUT);  // let the board's pull-up own it again
  }
  while (c6.available()) c6.read();
}

// Rolling match of the in-band commands.
char s_win[8] = {0};

void scanForCommand(uint8_t b) {
  memmove(s_win, s_win + 1, sizeof(s_win) - 1);
  s_win[sizeof(s_win) - 1] = static_cast<char>(b);
  if (memcmp(s_win + 2, "~C6DL~", 6) == 0) {
    resetC6(true);
    memset(s_win, 0, sizeof(s_win));
  } else if (memcmp(s_win + 1, "~C6RUN~", 7) == 0) {
    resetC6(false);
    memset(s_win, 0, sizeof(s_win));
  }
}

}  // namespace

void setup() {
  Serial.begin(kBaud);  // USB Serial/JTAG ignores the rate
  c6.setRxBufferSize(8192);
  c6.begin(kBaud, SERIAL_8N1, kPinRx, kPinTx);
  resetC6(true);
}

void loop() {
  uint8_t buf[1024];

  int n = Serial.available();
  if (n > 0) {
    n = Serial.read(buf, n > static_cast<int>(sizeof(buf)) ? sizeof(buf) : n);
    for (int i = 0; i < n; ++i) scanForCommand(buf[i]);
    c6.write(buf, n);
  }

  n = c6.available();
  if (n > 0) {
    n = c6.read(buf, n > static_cast<int>(sizeof(buf)) ? sizeof(buf) : n);
    Serial.write(buf, n);
  }

  if (n <= 0) delay(1);
}
