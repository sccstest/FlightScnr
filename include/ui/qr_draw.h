#pragma once

#include <cstdint>

/**
 * Draw a centered black-on-white Wi-Fi join QR for an open SoftAP.
 * Payload: WIFI:T:nopass;S:<ssid>;;
 * Returns false if encode/draw failed (caller can fall back to text-only).
 * The box is 37 modules wide (version 3 plus a 4-module quiet zone each side).
 */
bool drawWifiJoinQr(int center_x, int center_y, int module_px, const char* ssid);

/** Same box size, encoding a URL (up to 42 characters), e.g. "http://192.168.1.20/". */
bool drawUrlQr(int center_x, int center_y, int module_px, const char* url);
