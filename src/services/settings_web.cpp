#include "services/settings_web.h"

#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>

#include <esp_heap_caps.h>
#include <esp_ota_ops.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

#ifdef WM_MDNS
#include <ESPmDNS.h>
#endif

#include "config.h"
#include "services/firmware_image.h"
#include "hardware/buzzer.h"
#include "hardware/display_brightness.h"
#include "services/adsb_client.h"
#include "services/api_keys.h"
#include "services/clock_time.h"
#include "services/map_center.h"
#include "services/route_cache_store.h"
#include "services/route_lookup.h"
#include "services/settings_apply.h"
#include "services/settings_state.h"
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
#include "services/sky_events.h"
#endif
#include "services/tz_lookup.h"
#include "services/weather.h"
#include "services/aircraft_alert.h"
#include "services/device_identity.h"
#include "services/disclaimer_acceptance.h"
#include "services/off_hours.h"
#include "services/wifi_setup.h"
#include "services/radar_basemap.h"
#include "services/rain_radar.h"
#include "services/ota_github.h"
#include "ui/display_prefs.h"
#include "ui/radar_accent.h"
#include "ui/radar_scale.h"
#include "ui/radar_theme.h"

namespace {

WebServer* s_server = nullptr;
bool s_active = false;

/** Page compose buffer — PSRAM, allocated on first request.
 *  Must not be a static internal-DRAM array (24 KB of DRAM gone at link time)
 *  and must never pass through WebServer::send(code, type, const char*): that
 *  overload copies the whole page into an internal-heap String, which under a
 *  ~26 KB post-detail heap starved lwIP (min free 5 KB), stalled loop() for
 *  ~12 s, and left max_blk permanently fragmented below the panel/TLS gates. */
constexpr size_t kSettingsPageCap = 65536;
char* s_settings_page = nullptr;

int appendJsonString(char* out, size_t out_len, size_t* used, const char* value);

char* settingsPageBuffer() {
  if (s_settings_page == nullptr) {
    s_settings_page = static_cast<char*>(
        heap_caps_malloc(kSettingsPageCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  return s_settings_page;
}

const char kPageHead[] = R"HTML(<!DOCTYPE html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>FlightScnr Settings</title>
<style>
:root{--bg:#0a0a0a;--card:#141414;--card2:#1c1c1c;--line:#333;--text:#ececec;
--muted:#9a9a9a;--accent:#1a9c3c;--accent2:#22c24c;--field:#1e1e1e;--fline:#3a3a3a;--link:#cfcfcf;}
*{box-sizing:border-box;}
body{font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;margin:0;background:var(--bg);
color:var(--text);line-height:1.45;padding-bottom:5.5rem;overflow-wrap:anywhere;}
.wrap{max-width:34rem;margin:0 auto;padding:1.25rem 1rem;}
.app-head{display:flex;align-items:center;gap:.65rem;margin-bottom:.25rem;}
.logo{width:2.1rem;height:2.1rem;border-radius:50%;flex:none;border:1px solid #0a5;
background:radial-gradient(circle at 50% 50%,#0c2,#063 70%,#021 100%);}
h1{font-size:1.3rem;margin:0;}
.subtitle{color:var(--muted);font-size:.85rem;margin:.15rem 0 1.1rem;}
.banner{background:#143d22;border:1px solid #1a9c3c;color:#d8ffe4;border-radius:10px;
padding:.7rem .85rem;margin:0 0 1rem;font-size:.9rem;}
.banner b{color:#fff;}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;margin:0 0 .9rem;overflow:hidden;}
.card>summary{list-style:none;cursor:pointer;display:flex;align-items:center;gap:.6rem;
padding:.85rem 1rem;font-weight:600;font-size:.98rem;user-select:none;min-height:3.2rem;}
.card>summary::-webkit-details-marker,.adv>summary::-webkit-details-marker{display:none;}
.adv{margin:.9rem 0 0;border-top:1px solid var(--line);}
.adv>summary{list-style:none;cursor:pointer;min-height:2.9rem;display:flex;align-items:center;
color:var(--muted);font-size:.88rem;}
.adv>summary::before{content:"\25B8";margin-right:.45rem;transition:transform .18s;}
.adv[open]>summary::before{transform:rotate(90deg);}
.grp{border-color:#444;}.grp>.body{padding:.25rem .6rem .6rem;}
.grp .card{margin:.6rem 0 0;background:var(--card2);}
.dev{background:#3a3214;border:1px solid #a8892a;color:#fde9a8;border-radius:9px;
padding:.5rem .65rem;font-size:.8rem;margin:.6rem 0 0;}
.card>summary .ico{width:1.6rem;height:1.6rem;border-radius:7px;flex:none;display:grid;
place-items:center;font-size:.95rem;background:var(--card2);border:1px solid var(--line);}
.card>summary .chev{margin-left:auto;color:var(--muted);transition:transform .18s;font-size:.8rem;}
.card[open]>summary .chev{transform:rotate(90deg);}
.card>summary .sum{color:var(--muted);font-weight:400;font-size:.78rem;margin-left:.1rem;}
.card .body{padding:.25rem 1rem 1rem;border-top:1px solid var(--line);}
label{display:block;margin:.85rem 0 .3rem;font-size:.84rem;color:#d2d2d2;}
input,select{width:100%;padding:.6rem .65rem;border-radius:9px;border:1px solid var(--fline);
background:var(--field);color:#fff;font-size:1rem;outline:none;min-height:2.75rem;}
input:focus,select:focus{border-color:var(--accent2);box-shadow:0 0 0 2px rgba(34,194,76,.2);}
input::placeholder{color:#777;}
.hint,.note{color:var(--muted);font-size:.78rem;margin:.4rem 0 0;}
.row2{display:grid;grid-template-columns:1fr 1fr;gap:.6rem;}
@media(max-width:24rem){.row2{grid-template-columns:1fr;}}
.chk{display:flex;align-items:center;gap:.6rem;border-bottom:1px solid rgba(120,120,120,.22);}
.chk:last-of-type{border-bottom:none;}
.chk label{margin:0;font-size:.9rem;color:var(--text);flex:1;padding:.75rem 0;min-height:2.9rem;}
.switch{position:relative;width:2.6rem;height:1.5rem;flex:none;}
.switch input{position:absolute;opacity:0;left:-.3rem;right:-.3rem;top:-.65rem;width:auto;
height:2.8rem;min-height:0;margin:0;cursor:pointer;}
.switch .track{position:absolute;inset:0;border-radius:999px;background:#333;
border:1px solid var(--fline);transition:.18s;pointer-events:none;}
.switch .track::after{content:"";position:absolute;top:50%;left:.18rem;transform:translateY(-50%);
width:1.05rem;height:1.05rem;border-radius:50%;background:#9a9a9a;transition:.18s;}
.switch input:checked + .track{background:var(--accent);border-color:var(--accent2);}
.switch input:checked + .track::after{left:1.32rem;background:#fff;}
.api{border:1px solid var(--line);border-radius:11px;padding:.2rem .8rem .8rem;margin:.8rem 0;background:var(--card2);}
.api .api-head{display:flex;align-items:center;gap:.6rem;padding:.65rem 0 0;}
.api .api-head .name{font-weight:600;font-size:.92rem;}
.api .api-head .badge{margin-left:auto;font-size:.7rem;padding:.12rem .5rem;border-radius:999px;
background:#262626;border:1px solid #444;color:#bbb;}
.usage{font-size:.76rem;color:var(--muted);margin:.5rem 0 0;background:#161616;
border:1px solid var(--line);border-radius:8px;padding:.45rem .6rem;}
.usage b{color:#ddd;}
a{color:var(--link);}
.dl{display:inline-flex;align-items:center;gap:.4rem;text-decoration:none;background:var(--card2);
border:1px solid var(--line);border-radius:9px;padding:.55rem .8rem;font-size:.88rem;margin-top:.5rem;
min-height:2.75rem;}
.savebar{position:fixed;left:0;right:0;bottom:0;z-index:20;padding:1rem;
background:linear-gradient(180deg,rgba(10,10,10,0),rgba(10,10,10,.92) 30%,#0a0a0a);}
.savebar .inner{max-width:34rem;margin:0 auto;}
button.save{width:100%;padding:.85rem;font-size:1.02rem;font-weight:700;border:none;
border-radius:11px;background:var(--accent);color:#fff;cursor:pointer;box-shadow:0 6px 20px rgba(26,156,60,.35);}
button.save:hover{background:var(--accent2);}
.wifi-note{font-size:.78rem;color:#8a8a8a;margin-top:.55rem;}
.net-row{border:1px solid var(--line);border-radius:10px;padding:.65rem .75rem;margin:.55rem 0;
background:var(--card2);}
.net-row .top{display:flex;align-items:baseline;gap:.5rem;flex-wrap:wrap;}
.net-row .ord{font-weight:700;color:#fff;}
.net-row .ssid{font-weight:600;}
.net-row .skip{font-size:.72rem;color:#c9a227;margin-left:.15rem;}
.net-actions{display:flex;flex-wrap:wrap;gap:.4rem;margin-top:.5rem;}
button.sm,a.sm{font-size:.85rem;padding:.4rem .75rem;border-radius:8px;border:1px solid var(--fline);
background:#262626;color:#eee;cursor:pointer;text-decoration:none;display:inline-block;min-height:2.75rem;}
button:disabled{opacity:.45;cursor:default;}
button.sm.danger{border-color:#633;background:#2a1515;color:#fcc;}
.wifi-add{margin-top:.75rem;padding-top:.65rem;border-top:1px solid var(--line);}
.foot{text-align:center;font-size:.8rem;color:var(--muted);margin:1.2rem 0 .2rem;}
.foot a{color:var(--link);}
</style></head><body>
<div class="wrap">
<div class="app-head"><div class="logo"></div><h1>FlightScnr settings</h1></div>
<p class="subtitle">Change what you like, then tap <strong>Save</strong> at the bottom.
Nothing changes on the radar until you do.</p>
<form id="fs-save" method="POST" action="/save">
)HTML";

void formatUsdMicro(uint32_t micro, char* out, size_t len, int decimals) {
  snprintf(out, len, "%.*f", decimals, static_cast<double>(micro) / 1000000.0);
}

void appendRaw(char* page, size_t len, size_t* used, const char* html) {
  if (page == nullptr || used == nullptr || html == nullptr || *used >= len) {
    return;
  }
  const int n = snprintf(page + *used, len - *used, "%s", html);
  if (n > 0) {
    const size_t space = len - *used;
    *used += static_cast<size_t>(n) < space ? static_cast<size_t>(n) : space - 1;
  }
}

void appendClamped(char* page, size_t len, size_t* used, int n) {
  if (used == nullptr || n <= 0 || *used >= len) {
    return;
  }
  const size_t space = len - *used;
  *used += static_cast<size_t>(n) < space ? static_cast<size_t>(n) : space - 1;
}

/** Firmware / GitHub OTA card (end of page; page cap must fit this after other cards). */
void appendFirmwareUpdateCard(char* page, size_t* used) {
  if (page == nullptr || used == nullptr) {
    return;
  }
  const char* cur = services::ota_github::currentVersion();
  const char* latest = services::ota_github::latestTag();
  const bool avail = services::ota_github::updateAvailable();
  char gh_status[160];
  if (avail && latest[0] != '\0') {
    snprintf(gh_status, sizeof(gh_status),
             "Update available: <b>%s</b> (running %s). Install from GitHub below, "
             "then reset the device.",
             latest, cur);
  } else if (latest[0] != '\0') {
    snprintf(gh_status, sizeof(gh_status), "Up to date (running %s, latest %s).", cur,
             latest);
  } else {
    snprintf(gh_status, sizeof(gh_status),
             "Running <b>%s</b>. Check GitHub for a newer release, or upload an app "
             "<code>.bin</code> manually.",
             cur);
  }
  const int fw_n = snprintf(
      page + *used, kSettingsPageCap - *used,
      "<details class=\"card\" id=\"firmware\"%s>"
      "<summary><span class=\"ico\">&#8635;</span>"
      "Firmware update<span class=\"sum\">GitHub / upload</span>"
      "<span class=\"chev\">&#9656;</span></summary><div class=\"body\">"
      "<p class=\"hint\" id=\"fw_gh_status\">%s</p>"
      "<p style=\"margin-top:.4rem\">"
      "<button id=\"fw_check\" class=\"sm\" type=\"button\">Check GitHub</button> "
      "<button id=\"fw_install\" class=\"sm\" type=\"button\"%s>Install latest</button></p>"
      "<div class=\"banner\" style=\"background:#3d1414;border-color:#c33;margin-top:.75rem\">"
      "<b>Warning.</b> Installs the published <b>app image</b> only "
      "(<code>FlightScnr-" FLIGHTSCNR_BOARD_NAME "-app.bin</code>) &mdash; not the merged factory "
      "image. Keep power connected. After success, press the device&rsquo;s "
      "<b>reset button</b> (or unplug/replug) to boot the new firmware.</div>"
      "<label for=\"fw_file\" style=\"margin-top:.6rem\">Manual upload (optional)</label>"
      "<input id=\"fw_file\" type=\"file\" accept=\".bin\">"
      "<p style=\"margin-top:.6rem\">"
      "<button id=\"fw_btn\" class=\"sm\" type=\"button\">Upload &amp; flash</button></p>"
      "<div id=\"fw_bar\" style=\"display:none;height:10px;border-radius:6px;"
      "background:#262626;border:1px solid var(--line);margin-top:.6rem;"
      "overflow:hidden\"></div>"
      "<p id=\"fw_msg\" class=\"note\"></p>"
      "<script>"
      "(function(){"
      "var b=document.getElementById('fw_btn'),f=document.getElementById('fw_file'),"
      "m=document.getElementById('fw_msg'),bar=document.getElementById('fw_bar'),"
      "chk=document.getElementById('fw_check'),ins=document.getElementById('fw_install'),"
      "st=document.getElementById('fw_gh_status');"
      "function setBusy(on){b.disabled=on;f.disabled=on;chk.disabled=on;ins.disabled=on;}"
      "function setPct(p){"
      "p=Math.max(0,Math.min(100,Number(p)||0));"
      "bar.style.display='block';"
      "bar.style.background='linear-gradient(90deg,#22c24c 0 '+p+'%%,#333 '+p+'%%)';"
      "}"
      "function fmtMb(n){return(n/1048576).toFixed(1);}"
      // The check runs in the background on the device; poll until it is done.
      "function ghPoll(url,tries){"
      "fetch(url).then(function(r){return r.json();})"
      ".then(function(j){"
      "if(j.checking&&tries>0){setTimeout(function(){ghPoll('/ota/github/status',tries-1);},1500);"
      "return;}"
      "ghShow(j);setBusy(false);"
      "if(st&&st.innerHTML.indexOf('Update available')>=0){ins.disabled=false;}"
      "}).catch(function(e){m.textContent='Check error: '+e;setBusy(false);});"
      "}"
      "chk.addEventListener('click',function(){"
      "m.textContent='Checking GitHub\\u2026';setBusy(true);"
      "ghPoll('/ota/github/status?force=1',20);"
      "});"
      "function ghShow(j){"
      "if(!j.ok&&!j.latest){m.textContent='Check failed: '+(j.error||'unknown');return;}"
      "if(j.available){st.innerHTML='Update available: <b>'+j.latest+"
      "'</b> (running '+j.current+').';"
      "m.textContent=j.refreshed?'Newer release found.':"
      "'Newer release (cached). '+ (j.warning||'');"
      "ins.disabled=false;}"
      "else{st.textContent='Up to date (running '+j.current+', latest '+(j.latest||'?')+').';"
      "m.textContent=j.refreshed?'Already on the latest release.':"
      "'Cached result: up to date. '+(j.warning||'');}"
      "}"
      "function pollInstall(tries){"
      "fetch('/ota/github/progress').then(function(r){return r.json();})"
      ".then(function(j){"
      "var p=typeof j.percent==='number'?j.percent:parseInt(j.percent,10)||0;"
      "setPct(p);"
      "if(j.state==='running'){"
      "var msg='Downloading / flashing '+p+'%%';"
      "if(j.bytes){msg+=' ('+fmtMb(j.bytes)+(j.total?' / '+fmtMb(j.total):'')+' MB)';}"
      "m.textContent=msg;setTimeout(function(){pollInstall(8);},400);}"
      "else if(j.state==='succeeded'){setPct(100);"
      "m.textContent='Update installed \\u2014 press the device\\u2019s reset button "
      "to boot the new firmware.';setBusy(false);}"
      "else if(j.state==='idle'){setTimeout(function(){pollInstall(tries);},300);}"
      "else{m.textContent='Install failed: '+(j.error||j.state);setBusy(false);}"
      "}).catch(function(e){"
      "if(tries>0){setTimeout(function(){pollInstall(tries-1);},500);return;}"
      "m.textContent='Progress error: '+e+' \\u2014 refresh the page. The install may "
      "still be running.';setBusy(false);});"
      "}"
      "ins.addEventListener('click',function(){"
      "if(!confirm('Download and flash the latest GitHub app image? Keep power on. "
      "You will need to reset afterwards.'))return;"
      "m.textContent='Starting install\\u2026';setBusy(true);setPct(0);"
      "fetch('/ota/github/install',{method:'POST'}).then(function(r){"
      "if(!r.ok)return r.text().then(function(t){throw new Error(t||r.status);});"
      "pollInstall(8);"
      "}).catch(function(e){m.textContent='Install error: '+e;setBusy(false);});"
      "});"
      "b.addEventListener('click',function(){"
      "if(!f.files||!f.files[0]){m.textContent='Choose a firmware.bin file first.';return;}"
      "if(!confirm('Flash this firmware? Do not upload the merged image, and keep "
      "power connected. You will need to reset the device afterwards.'))return;"
      "var fd=new FormData();fd.append('firmware',f.files[0]);"
      "var x=new XMLHttpRequest();x.open('POST','/update');"
      "setBusy(true);setPct(0);"
      "x.upload.onprogress=function(e){if(e.lengthComputable){"
      "var p=Math.round(e.loaded/e.total*100);setPct(p);"
      "m.textContent='Uploading '+p+'%%';}};"
      "x.onload=function(){if(x.status==200){setPct(100);"
      "m.textContent='Update installed \\u2014 press the device\\u2019s reset button "
      "to boot the new firmware.';}"
      "else{m.textContent='Failed: '+(x.responseText||('HTTP '+x.status));"
      "setBusy(false);}};"
      "x.onerror=function(){m.textContent='Upload error (connection lost).';setBusy(false);};"
      "x.send(fd);"
      "});"
      "})();"
      "</script></div></details>",
      avail ? " open" : "", gh_status, avail ? "" : " disabled");
  if (fw_n < 0 || static_cast<size_t>(fw_n) >= (kSettingsPageCap - *used)) {
    Serial.printf("[settings] firmware card truncated used=%u need=%d cap=%u\n",
                  static_cast<unsigned>(*used), fw_n,
                  static_cast<unsigned>(kSettingsPageCap));
  }
  appendClamped(page, kSettingsPageCap, used, fw_n);
}

// Emits a labelled on/off toggle row (label left, switch right). id is reused as name.
// form="fs-save" lets toggles in the Advanced group (outside the <form>) still save.
void appendToggle(char* page, size_t len, size_t* used, const char* id, const char* label_html,
                  bool checked) {
  if (page == nullptr || used == nullptr || *used >= len) {
    return;
  }
  const int n = snprintf(
      page + *used, len - *used,
      "<div class=\"chk\"><label for=\"%s\">%s</label>"
      "<span class=\"switch\"><input id=\"%s\" name=\"%s\" form=\"fs-save\" type=\"checkbox\" "
      "value=\"T\"%s><span class=\"track\"></span></span></div>",
      id, label_html, id, id, checked ? " checked" : "");
  appendClamped(page, len, used, n);
}

// Emits an inline toggle switch (no row chrome) for use inside an API header.
void appendInlineToggle(char* page, size_t len, size_t* used, const char* id, bool checked) {
  if (page == nullptr || used == nullptr || *used >= len) {
    return;
  }
  const int n = snprintf(
      page + *used, len - *used,
      "<span class=\"switch\"><input id=\"%s\" name=\"%s\" form=\"fs-save\" type=\"checkbox\" "
      "value=\"T\"%s><span class=\"track\"></span></span>",
      id, id, checked ? " checked" : "");
  appendClamped(page, len, used, n);
}

void redirectToSettings(const char* query) {
  char loc[32];
  if (query != nullptr && query[0] != '\0') {
    snprintf(loc, sizeof(loc), "/?%s", query);
  } else {
    snprintf(loc, sizeof(loc), "/");
  }
  s_server->sendHeader("Location", loc, true);
  s_server->send(303, "text/plain", "");
}

void sendLocationErrorPage() {
  char page[960];
  snprintf(page, sizeof(page),
           "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
           "<title>Location not saved</title></head>"
           "<body style=\"font-family:system-ui,sans-serif;max-width:28rem;margin:1.5rem auto;"
           "padding:0 1rem;background:#000;color:#ececec\">"
           "<h1 style=\"font-size:1.25rem;color:#f66\">Location not saved</h1>"
           "<p>Your other settings were saved, but the <strong>Radar center</strong> "
           "location could not be read. Enter two numbers with a comma between them "
           "(latitude, then longitude), for example:</p>"
           "<p style=\"font-family:monospace;background:#222;padding:.75rem;border-radius:6px\">"
           "51.507400, -0.127800</p>"
           "<p style=\"color:#9a9a9a;font-size:.9rem\">Spaces around the comma are fine. "
           "Latitude must be between &minus;90 and 90; longitude between &minus;180 and 180.</p>"
           "<p><a href=\"/\" style=\"color:#cfcfcf\">Back to settings</a></p>"
           "</body></html>");
  s_server->send(400, "text/html; charset=utf-8", page);
}

void appendAccentOptions(char* buf, size_t len, size_t* used) {
  const uint8_t current = ui::radar::accentColorIndex();
  for (uint8_t i = 0; i < ui::radar::kRadarAccentCount; ++i) {
    const int n = snprintf(buf + *used, len - *used, "<option value=\"%u\"%s>%s</option>",
                           static_cast<unsigned>(i), (i == current) ? " selected" : "",
                           ui::radar::accentColorNameAt(i));
    appendClamped(buf, len, used, n);
  }
}

void appendRangeOptions(char* buf, size_t len, size_t* used) {
  const uint8_t active = ui::radar::scaleActiveMiles();
  for (size_t i = 0; i < ui::radar::kRangeMileOptionCount; ++i) {
    const unsigned mi = ui::radar::kRangeMileOptions[i];
    const float km = static_cast<float>(mi) * ui::radar::kStatuteMileKm;
    const float nm = static_cast<float>(mi) * ui::radar::kStatuteMileKm / ui::radar::kNauticalMileKm;
    const int n = snprintf(
        buf + *used, len - *used,
        "<option value=\"%u\"%s>%u mi &middot; %.1f km &middot; %.1f nm</option>", mi,
        mi == active ? " selected" : "", mi, static_cast<double>(km),
        static_cast<double>(nm));
    if (n > 0) {
      appendClamped(buf, len, used, n);
    }
  }
}

void handleSettingsPage() {
  // No apikeys::load() here: the keys live in RAM (loaded at boot and at server
  // start; every save updates RAM too). Re-reading NVS per request raced the
  // route worker and cost ~20 NVS reads.
  char* const page = settingsPageBuffer();
  if (page == nullptr) {
    s_server->send(503, "text/plain", "Out of memory");
    return;
  }
  size_t used = 0;
  char masked[24];
  char watch_buf[160];
  char watch_type_buf[96];
  char watch_reg_buf[160];
  services::alert::watchCallsignsFormatted(watch_buf, sizeof(watch_buf));
  services::alert::watchTypesFormatted(watch_type_buf, sizeof(watch_type_buf));
  services::alert::watchRegsFormatted(watch_reg_buf, sizeof(watch_reg_buf));
  const size_t watch_count = services::alert::watchCallsignCount();
  const size_t watch_type_count = services::alert::watchTypeCount();
  const size_t watch_reg_count = services::alert::watchRegCount();

  const int head_n = snprintf(page, kSettingsPageCap, "%s", kPageHead);
  if (head_n > 0) {
    used = static_cast<size_t>(head_n) < kSettingsPageCap
               ? static_cast<size_t>(head_n)
               : kSettingsPageCap - 1;
  }

  if (s_server->hasArg("saved")) {
    appendRaw(page, kSettingsPageCap, &used,
              "<div class=\"banner\"><b>Saved.</b> Settings applied — radar will refresh."
              "</div>");
  }
  if (s_server->hasArg("wifi_ok")) {
    appendRaw(page, kSettingsPageCap, &used,
              "<div class=\"banner\"><b>Wi&#8209;Fi updated.</b></div>");
  }
  if (s_server->hasArg("wifi_err")) {
    appendRaw(page, kSettingsPageCap, &used,
              "<div class=\"banner\" style=\"background:#3d1414;border-color:#c33\">"
              "<b>Wi&#8209;Fi change failed.</b> Check SSID/password and free slots (max 3)."
              "</div>");
  }
  if (s_server->hasArg("cache_cleared")) {
    appendRaw(page, kSettingsPageCap, &used,
              "<div class=\"banner\"><b>Route cache cleared.</b> Open flight detail again to "
              "re-fetch routes.</div>");
  }
  if (s_server->hasArg("cache_err")) {
    appendRaw(page, kSettingsPageCap, &used,
              "<div class=\"banner\" style=\"background:#3d1414;border-color:#c33\">"
              "<b>Could not clear route cache.</b> Try again in a moment."
              "</div>");
  }
  if (s_server->hasArg("disclaimer_cleared")) {
    appendRaw(page, kSettingsPageCap, &used,
              "<div class=\"banner\"><b>Disclaimer acceptance cleared.</b> The next boot will "
              "wait for on-screen Accept (no auto-continue).</div>");
  }
  if (services::ota_github::updateAvailable()) {
    const char* latest = services::ota_github::latestTag();
    char upd_banner[220];
    snprintf(upd_banner, sizeof(upd_banner),
             "<div class=\"banner\"><b>Firmware update available</b>%s%s. "
             "<a href=\"#firmware\" style=\"color:#9f9\">Open Firmware update</a> to install."
             "</div>",
             (latest[0] != '\0') ? ": " : "",
             (latest[0] != '\0') ? latest : "");
    appendRaw(page, kSettingsPageCap, &used, upd_banner);
  }

  // ---------- Radar card ----------
  appendRaw(page, kSettingsPageCap, &used,
            "<details class=\"card\" open><summary><span class=\"ico\">&#9678;</span>Radar"
            "<span class=\"sum\">location, range, planes</span><span class=\"chev\">&#9656;</span>"
            "</summary><div class=\"body\">");

  char center_value[48];
  snprintf(center_value, sizeof(center_value), "%.6f, %.6f",
           services::map_center::latitude(), services::map_center::longitude());
  const int center_n = snprintf(
      page + used, kSettingsPageCap - used,
      "<label for=\"radar_center\">Radar center (lat, lon)</label>"
      "<input id=\"radar_center\" name=\"radar_center\" type=\"text\" required "
      "autocomplete=\"off\" inputmode=\"decimal\" placeholder=\"37.636422, -122.365968\" "
      "value=\"%s\">"
      "<p class=\"hint\">Your location &mdash; paste from Google Maps (long-press the map, "
      "then copy the two numbers).</p>",
      center_value);
  appendClamped(page, kSettingsPageCap, &used, center_n);

  const ui::radar::DistanceUnit unit = ui::radar::distanceUnit();
  appendRaw(page, kSettingsPageCap, &used,
            "<label for=\"range_mi\">Range</label>"
            "<select id=\"range_mi\" name=\"range_mi\">");
  appendRangeOptions(page, kSettingsPageCap, &used);
  const int units_n = snprintf(
      page + used, kSettingsPageCap - used,
      "</select>"
      "<label for=\"dist_unit\">Distance units</label>"
      "<select id=\"dist_unit\" name=\"dist_unit\">"
      "<option value=\"km\"%s>kilometers</option>"
      "<option value=\"mi\"%s>statute miles</option>"
      "<option value=\"nm\"%s>nautical miles</option>"
      "</select>",
      unit == ui::radar::DistanceUnit::Km ? " selected" : "",
      unit == ui::radar::DistanceUnit::StatuteMile ? " selected" : "",
      unit == ui::radar::DistanceUnit::NauticalMile ? " selected" : "");
  appendClamped(page, kSettingsPageCap, &used, units_n);

  const int min_n = snprintf(
      page + used, kSettingsPageCap - used,
      "<div class=\"row2\"><div>"
      "<label for=\"min_height\">Hide planes below (ft)</label>"
      "<input id=\"min_height\" name=\"min_height\" type=\"number\" min=\"0\" step=\"1\" "
      "inputmode=\"numeric\" value=\"%d\">"
      "</div><div>"
      "<label for=\"max_height\">Hide planes above (ft)</label>"
      "<input id=\"max_height\" name=\"max_height\" type=\"number\" min=\"0\" step=\"1\" "
      "inputmode=\"numeric\" value=\"%d\">"
      "</div></div>"
      "<p class=\"hint\">In feet above the ground. 0 = off. 500 hides planes on the ground "
      "and taxiing.</p>",
      services::adsb::altitudeFloorFt(), services::adsb::altitudeCeilingFt());
  appendClamped(page, kSettingsPageCap, &used, min_n);

  appendToggle(page, kSettingsPageCap, &used, "plane_labels",
               "Plane labels (text next to each plane)",
               !ui::displayPrefsHideBlipDetails());
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
  appendToggle(page, kSettingsPageCap, &used, "overhead_sign",
               "Overhead sign (banner for the closest plane)",
               ui::displayPrefsOverheadSignEnabled());
  appendToggle(page, kSettingsPageCap, &used, "hide_airliners",
               "Hide airliners and cargo jets (special ones still show)",
               services::alert::airlinersHidden());
#endif

  {
    const int facing_n = snprintf(
        page + used, kSettingsPageCap - used,
        "<label for=\"facing_deg\">Which way the top of the screen faces (degrees)</label>"
        "<input id=\"facing_deg\" name=\"facing_deg\" type=\"number\" min=\"0\" max=\"359\" "
        "step=\"5\" inputmode=\"numeric\" value=\"%u\">"
        "<p class=\"hint\">0 = North, 90 = East, 180 = South, 270 = West (steps of 5). "
#if FLIGHTSCNR_HAS_ENCODER
        "On the device: Settings &rarr; Display &rarr; Facing, then turn the dial."
#else
        "On the device: swipe left on the radar, Settings 2/4 &rarr; Facing, then tap the "
        "left or right side to turn and the middle to save."
#endif
        "</p>",
        static_cast<unsigned>(ui::radar::facingDeg()));
    if (facing_n > 0) {
      appendClamped(page, kSettingsPageCap, &used, facing_n);
    }
  }

  appendRaw(page, kSettingsPageCap, &used,
            "<details class=\"adv\"><summary>Look of the radar</summary>"
            "<label for=\"radar_accent\">Color theme</label>"
            "<select id=\"radar_accent\" name=\"radar_accent\">");
  appendAccentOptions(page, kSettingsPageCap, &used);
  appendRaw(page, kSettingsPageCap, &used, "</select>");
  appendToggle(page, kSettingsPageCap, &used, "show_cardinals", "Show compass rose",
               ui::radar::showCompassRose());
  appendToggle(page, kSettingsPageCap, &used, "show_sweep", "Show radar sweep line",
               ui::displayPrefsSweepLineEnabled());
  appendRaw(page, kSettingsPageCap, &used, "</details>");

  appendRaw(page, kSettingsPageCap, &used, "</div></details>");

  // ---------- Map card (Carto / FAA VFR bake, made in the browser) ----------
  {
    char bm_status[192];
    char carto_masked[48];
    services::basemap::statusText(bm_status, sizeof(bm_status));
    services::apikeys::maskedCarto(carto_masked, sizeof(carto_masked));
    // The stored bake's style, or the board's factory style (Voyager on the 4C) when
    // nothing is baked yet. This used to fall back to Dark, so a first bake on the
    // gift unit came out near-black unless the user changed the dropdown.
    const auto bm_style = services::basemap::storedStyle();
    const bool dark_sel = bm_style == services::basemap::Style::Dark;
    const bool light_sel = bm_style == services::basemap::Style::Light;
    const bool vfr_sel = bm_style == services::basemap::Style::Vfr;
    const bool voyager_sel = bm_style == services::basemap::Style::Voyager;
    const int bm_n = snprintf(
        page + used, kSettingsPageCap - used,
        "<details class=\"card\"><summary><span class=\"ico\">&#127758;</span>"
        "Map<span class=\"sum\">map under the radar</span>"
        "<span class=\"chev\">&#9656;</span></summary><div class=\"body\">"
        "<p class=\"note\">An optional street or aviation map under the radar. This page "
        "downloads it and sends it to the device: tap <b>Make map</b> and keep the page "
        "open (about a minute). "
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
        "It is made for the largest range, with a sharp copy for every closer zoom, so "
        "zooming in stays crisp. "
#endif
        "Make it again after you change the location or facing.</p>"
        "<p class=\"hint\" id=\"bm_status\">%s</p>",
        bm_status);
    appendClamped(page, kSettingsPageCap, &used, bm_n);
    appendToggle(page, kSettingsPageCap, &used, "use_basemap", "Show the map on the radar",
                 services::basemap::enabled());
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
    appendToggle(page, kSettingsPageCap, &used, "use_rain",
                 "Live rain radar on the map (US only, NOAA)", services::rain::enabled());
#endif
    const int bm_n2 = snprintf(
        page + used, kSettingsPageCap - used,
        "<label for=\"basemap_style\">Map style</label>"
        "<select id=\"basemap_style\">"
        "<option value=\"voyager\"%s>Voyager (light, like Google Maps)</option>"
        "<option value=\"light\"%s>Positron (pale grey)</option>"
        "<option value=\"dark\"%s>Dark Matter (dark)</option>"
        "<option value=\"vfr\"%s>VFR Sectional (FAA pilot chart, US only)</option>"
        "</select>"
        "<label for=\"carto_key\">CARTO key: %s</label>"
        "<input id=\"carto_key\" name=\"carto_key\" type=\"password\" autocomplete=\"off\" "
        "placeholder=\"paste key\">"
        "<p class=\"hint\">Needed for Voyager, Positron and Dark Matter (not for VFR). "
        "Free at <a href=\"https://carto.com/basemaps/apikey/\" target=\"_blank\" "
        "rel=\"noopener\">carto.com/basemaps/apikey</a>. Saved when you tap Make map or "
        "Save.</p>",
        voyager_sel ? " selected" : "", light_sel ? " selected" : "",
        dark_sel ? " selected" : "", vfr_sel ? " selected" : "", carto_masked);
    appendClamped(page, kSettingsPageCap, &used, bm_n2);
#if !defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
    {
      const uint8_t live_mi = ui::radar::scaleActiveMiles();
      const uint8_t max_mi =
          ui::radar::kRangeMileOptions[ui::radar::kRangeMileOptionCount - 1];
      const int cov_n = snprintf(
          page + used, kSettingsPageCap - used,
          "<label for=\"basemap_coverage\">Map coverage</label>"
          "<select id=\"basemap_coverage\">"
          "<option value=\"current\" selected>Current range (%u mi) &mdash; sharper</option>"
          "<option value=\"max\">Maximum range (%u mi) &mdash; zoom-friendly</option>"
          "</select>"
          "<p class=\"hint\"><b>Current range</b> is sharper. <b>Maximum range</b> lets you "
          "zoom in without making it again, but looks softer. Zooming out past the map "
          "needs a new one.</p>",
          static_cast<unsigned>(live_mi), static_cast<unsigned>(max_mi));
      appendClamped(page, kSettingsPageCap, &used, cov_n);
    }
#endif
    const int adv_n = snprintf(
        page + used, kSettingsPageCap - used,
        "<details class=\"adv\"><summary>Advanced: map contrast</summary>"
        "<div class=\"row2\"><div>"
        "<label for=\"bm_contrast_dark\">Dark contrast (%%)</label>"
        "<input id=\"bm_contrast_dark\" name=\"bm_contrast_dark\" type=\"number\" "
        "min=\"0\" max=\"200\" step=\"5\" value=\"%u\">"
        "</div><div>"
        "<label for=\"bm_contrast_light\">Light / Voyager contrast (%%)</label>"
        "<input id=\"bm_contrast_light\" name=\"bm_contrast_light\" type=\"number\" "
        "min=\"0\" max=\"200\" step=\"5\" value=\"%u\">"
        "</div></div>"
        "<label for=\"bm_wash_vfr\">VFR fade toward white (%%)</label>"
        "<input id=\"bm_wash_vfr\" name=\"bm_wash_vfr\" type=\"number\" min=\"0\" max=\"100\" "
        "step=\"1\" value=\"%u\">"
        "<p class=\"hint\">100 = normal contrast; lower is flatter, higher is stronger. "
        "Used the next time you make the map.</p></details>",
        static_cast<unsigned>(services::basemap::contrastPercentDark()),
        static_cast<unsigned>(services::basemap::contrastPercentLight()),
        static_cast<unsigned>(services::basemap::washPercentVfr()));
    appendClamped(page, kSettingsPageCap, &used, adv_n);
  }
  {
    char carto_js[services::apikeys::kMaxSingleKeyLen + 8];
    size_t carto_js_len = 0;
    appendJsonString(carto_js, sizeof(carto_js), &carto_js_len, services::apikeys::cartoKey());
    if (carto_js_len >= sizeof(carto_js)) {
      carto_js_len = sizeof(carto_js) - 1;
    }
    carto_js[carto_js_len] = '\0';
    // lat/lon/facing/range here are only fallbacks: Make map reads the device's
    // current values from the latest /api/settings poll (window.fsDev) at click time.
    const int bm2 = snprintf(
        page + used, kSettingsPageCap - used,
        "<p style=\"margin-top:.6rem\">"
        "<button id=\"bm_gen\" class=\"sm\" type=\"button\">Make map</button> "
        "<button id=\"bm_clear\" class=\"sm\" type=\"button\">Delete map</button></p>"
        "<p id=\"bm_msg\" class=\"note\"></p>"
        "<p class=\"hint\">&copy; OpenStreetMap / &copy; CARTO / &copy; FAA.</p>"
        "<script>"
        "(function(){"
        "var SIZE=%d,CX=%d,CY=%d,OUTER=%d;"
        "var lat0=%.6f,lon0=%.6f,cur0=%u,maxMiles=%u,facing0=%u;"
        "var lat=lat0,lon=lon0,facing=facing0;"
        "var cartoKey=%s;"
        "function readDev(){var j=window.fsDev||{};"
        "var c=String(j.radar_center||'').split(','),a=parseFloat(c[0]),b=parseFloat(c[1]);"
        "lat=isNaN(a)?lat0:a;lon=isNaN(b)?lon0:b;"
        "var f=parseInt(j.facing_deg,10);facing=isNaN(f)?facing0:f;}"
        "function activeCartoKey(){"
        "var el=document.getElementById('carto_key');"
        "var v=el&&el.value?el.value.trim():'';"
        "return v||cartoKey;}"
        "var msg=document.getElementById('bm_msg');"
        "var gen=document.getElementById('bm_gen'),clr=document.getElementById('bm_clear');"
        "var styleEl=document.getElementById('basemap_style');"
        "var covEl=document.getElementById('basemap_coverage');"
        "function styleKey(){"
        "var v=styleEl&&styleEl.value;"
        "if(v==='light')return'light';if(v==='vfr')return'vfr';"
        "if(v==='voyager')return'voyager';return'dark';}"
        "function styleLabel(){"
        "var k=styleKey();"
        "if(k==='light')return'Positron (light)';"
        "if(k==='voyager')return'Voyager';"
        "if(k==='vfr')return'VFR Sectional';"
        "return'Dark Matter (dark)';}"
        "function bakeMiles(){"
        "if(!covEl||covEl.value==='max')return maxMiles;"
        "var r=parseInt((window.fsDev||{}).range_mi,10);return r>0?r:cur0;}"
        "function fillRgb(){"
        "var k=styleKey();"
        "if(k==='light')return[240,240,240];"
        "if(k==='voyager')return[242,239,230];"
        "if(k==='vfr')return[248,248,242];"
        "return[2,15,3];}"
        "function fillCss(){var c=fillRgb();return'rgb('+c[0]+','+c[1]+','+c[2]+')';}"
        "function readPct(id,defPct,maxPct){"
        "var el=document.getElementById(id);var v=el?parseFloat(el.value):defPct;"
        "if(isNaN(v))v=defPct;return Math.max(0,Math.min(maxPct,v));}"
        "function contrastFactor(){"
        "var k=styleKey();"
        "if(k==='dark')return readPct('bm_contrast_dark',%u,200)/100;"
        "if(k==='light'||k==='voyager')return readPct('bm_contrast_light',%u,200)/100;"
        "return 1;}"
        "function washAmount(){"
        "return styleKey()==='vfr'?readPct('bm_wash_vfr',%u,100)/100:0;}"
        "function washTarget(){return 255;}"
        "var ADJ=['bm_contrast_dark','bm_contrast_light','bm_wash_vfr'];"
        "function persistBakeAdjust(){"
        "var fd=new FormData();"
        "ADJ.forEach(function(id){"
        "var el=document.getElementById(id);if(el)fd.append(id,el.value);});"
        "var ckEl=document.getElementById('carto_key');"
        "if(ckEl&&ckEl.value&&ckEl.value.trim())fd.append('carto_key',ckEl.value.trim());"
        "return fetch('/basemap/adjust',{method:'POST',body:fd}).then(function(r){"
        "if(r.ok){var k=activeCartoKey();if(k)cartoKey=k;"
        "var d=window.fsDirty||{};ADJ.concat(['carto_key']).forEach(function(i){delete d[i];});}"
        "return r;"
        "}).catch(function(){});}"
        "function mercX(L){return(L+180)/360;}"
        "function mercY(A){var s=Math.sin(A*Math.PI/180);return.5-Math.log((1+s)/(1-s))/(4*Math.PI);}"
        "function tileXY(A,L,z){var n=Math.pow(2,z);return[mercX(L)*n,mercY(A)*n];}"
        "function pickZ(labelKm){var mpp=(labelKm*1000)/OUTER;var c=Math.cos(lat*Math.PI/180);"
        "var z=Math.log2(156543.03392*c/mpp);z=Math.round(z);"
        "if(styleKey()==='vfr')return Math.max(8,Math.min(12,z));"
        "return Math.max(6,Math.min(15,z));}"
        "function tileUrl(z,x,y){"
        "if(styleKey()==='vfr')"
        "return'https://tiles.arcgis.com/tiles/ssFJjBXIUyZDrSYZ/arcgis/rest/services/"
        "VFR_Sectional/MapServer/tile/'+z+'/'+y+'/'+x;"
        "var path='rastertiles/dark_nolabels';"
        "if(styleKey()==='light')path='rastertiles/light_nolabels';"
        "else if(styleKey()==='voyager')path='rastertiles/voyager_nolabels';"
        "var url='https://a.basemaps.cartocdn.com/'+path+'/'+z+'/'+x+'/'+y+'.png';"
        "var key=activeCartoKey();"
        "if(key)url+='?key='+encodeURIComponent(key);"
        "return url;}"
        "function loadTile(z,x,y){return new Promise(function(res,rej){"
        "var i=new Image();i.crossOrigin='anonymous';"
        "i.onload=function(){res(i)};"
        "i.onerror=function(){"
        "if(styleKey()==='vfr')res(null);"
        "else rej(new Error('tile '+z+'/'+x+'/'+y));};"
        "i.src=tileUrl(z,x,y);});}"
        "async function bakeOne(miles,detail){"
        "var labelKm=miles*1.609344,ppm=OUTER/labelKm;"
        "var coverR=Math.ceil(Math.hypot(CX,CY))+2;"
        "msg.textContent='Fetching '+styleLabel()+' @ '+miles+' mi\\u2026';"
        "var z=pickZ(labelKm),f=facing*Math.PI/180,cf=Math.cos(f),sf=Math.sin(f);"
        "var corners=[];"
        "for(var a=0;a<360;a+=30){var r=coverR,ex=(Math.sin(a*Math.PI/180)*r)/ppm,"
        "ny=(Math.cos(a*Math.PI/180)*r)/ppm;"
        "var e=ex*cf+ny*sf,n=-ex*sf+ny*cf;"
        "var R=6371.0088,dLat=n/R*180/Math.PI,dLon=e/(R*Math.cos(lat*Math.PI/180))*180/Math.PI;"
        "corners.push([lat+dLat,lon+dLon]);}"
        "var minX=1e9,maxX=-1e9,minY=1e9,maxY=-1e9;"
        "corners.forEach(function(p){var t=tileXY(p[0],p[1],z);minX=Math.min(minX,t[0]);"
        "maxX=Math.max(maxX,t[0]);minY=Math.min(minY,t[1]);maxY=Math.max(maxY,t[1]);});"
        "var x0=Math.floor(minX)-1,x1=Math.ceil(maxX)+1,y0=Math.floor(minY)-1,y1=Math.ceil(maxY)+1;"
        "var tiles={},jobs=[],got=0;"
        "for(var x=x0;x<=x1;x++)for(var y=y0;y<=y1;y++)(function(X,Y){"
        "jobs.push(loadTile(z,X,Y).then(function(img){if(img){tiles[X+','+Y]=img;got++;}}));})(x,y);"
        "await Promise.all(jobs);"
        "if(!got)throw new Error('No map tiles loaded (check the internet connection)');"
        "msg.textContent='Compositing\\u2026';"
        "var tw=(x1-x0+1)*256,th=(y1-y0+1)*256;"
        "var mc=document.createElement('canvas');mc.width=tw;mc.height=th;"
        "var mg=mc.getContext('2d');mg.fillStyle=fillCss();mg.fillRect(0,0,tw,th);"
        "for(var x=x0;x<=x1;x++)for(var y=y0;y<=y1;y++){var im=tiles[x+','+y];"
        "if(im)mg.drawImage(im,(x-x0)*256,(y-y0)*256);}"
        "var src=mg.getImageData(0,0,tw,th).data;"
        "var c=document.createElement('canvas');c.width=SIZE;c.height=SIZE;"
        "var g=c.getContext('2d');var out=g.createImageData(SIZE,SIZE),d=out.data;"
        "var fr=fillRgb(),wash=washAmount(),keep=1-wash,wt=washTarget(),cfac=contrastFactor();"
        "for(var i=0;i<d.length;i+=4){d[i]=fr[0];d[i+1]=fr[1];d[i+2]=fr[2];d[i+3]=255;}"
        "for(var py=0;py<SIZE;py++)for(var px=0;px<SIZE;px++){"
        "var dx=px-CX,dy=CY-py;"
        "var ex=dx/ppm,ny=dy/ppm;var e=ex*cf+ny*sf,n=-ex*sf+ny*cf;"
        "var R=6371.0088,dLat=n/R*180/Math.PI,dLon=e/(R*Math.cos(lat*Math.PI/180))*180/Math.PI;"
        "var A=lat+dLat,L=lon+dLon;var t=tileXY(A,L,z);"
        "var fx=(t[0]-x0)*256,fy=(t[1]-y0)*256;"
        "var x0i=Math.floor(fx),y0i=Math.floor(fy);"
        "var tx=fx-x0i,ty=fy-y0i;"
        "if(x0i<0||y0i<0||x0i+1>=tw||y0i+1>=th)continue;"
        "function samp(ix,iy){var o=(iy*tw+ix)*4;return[src[o],src[o+1],src[o+2]];}"
        "var p00=samp(x0i,y0i),p10=samp(x0i+1,y0i),p01=samp(x0i,y0i+1),p11=samp(x0i+1,y0i+1);"
        "var di=(py*SIZE+px)*4;"
        "for(var k=0;k<3;k++){var v0=p00[k]+(p10[k]-p00[k])*tx,v1=p01[k]+(p11[k]-p01[k])*tx;"
        "var v=v0+(v1-v0)*ty;"
        "if(wash>0){v=v*keep+wt*wash;}"
        "else if(cfac!==1){v=(v-128)*cfac+128;}"
        "d[di+k]=Math.max(0,Math.min(255,Math.round(v)));}d[di+3]=255;}"
        "g.putImageData(out,0,0);"
        "msg.textContent='Uploading\\u2026';"
        "var blob=await new Promise(function(r){c.toBlob(r,'image/jpeg',0.88);});"
        "if(!blob)throw new Error('JPEG encode failed');"
        "var fd=new FormData();fd.append('basemap',blob,'basemap.jpg');"
        "var q='style='+encodeURIComponent(styleKey())+'&mi='+miles+(detail?'&detail=1':'');"
        "await new Promise(function(res,rej){"
        "var x=new XMLHttpRequest();x.open('POST','/basemap/upload?'+q);"
        "x.onload=function(){if(x.status==200)res();"
        "else rej(new Error('Upload failed: '+(x.responseText||x.status)));};"
        "x.onerror=function(){rej(new Error('Upload error'));};x.send(fd);});"
        "}"
        "async function bake(){"
        "await persistBakeAdjust();"
        "var miles=bakeMiles();"
        "await bakeOne(miles,false);"
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
        // Sharper copies for every smaller zoom preset, so zooming in stays crisp.
        "var LEVELS=[2,3,6,8,10,20,30];"
        "for(var li=0;li<LEVELS.length;li++){if(LEVELS[li]<miles)await bakeOne(LEVELS[li],true);}"
#endif
        // No page reload afterwards: that dropped any other unsaved edits. The status
        // line and the Show-map switch update from the next poll.
        "msg.textContent='Map saved. It shows on the radar now.';"
        "}"
        "function busy(on){gen.disabled=on;clr.disabled=on;window.fsBusy=on;}"
        "gen.addEventListener('click',function(){"
        "var dd=window.fsDirty||{};"
        "if(dd.radar_center||dd.facing_deg){msg.textContent='Save first: you changed the "
        "location or facing. Tap Save, then Make map.';return;}"
        "if(styleKey()!=='vfr'&&!activeCartoKey()){"
        "msg.textContent='This style needs a CARTO key: paste it above, or pick VFR.';"
        "return;}"
        "readDev();var miles=bakeMiles();"
        "if(!confirm('Make the '+styleLabel()+' map around '+lat.toFixed(4)+', '+"
        "lon.toFixed(4)+' ('+miles+' mi)?\\n'"
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
        "+'Takes about a minute. Keep this page open.'"
#else
        "+(miles===maxMiles"
        "?'Zooming in will look softer (make it at the current range for sharper detail).'"
        ":'Zooming out past '+miles+' mi needs a new map.')"
#endif
        "))return;"
        "busy(true);"
        "bake().catch(function(e){msg.textContent=String(e&&e.message||e);})"
        ".then(function(){busy(false);});});"
        "clr.addEventListener('click',function(){"
        "if(!confirm('Delete the stored map?'))return;"
        "fetch('/basemap/clear',{method:'POST'}).then(function(r){"
        "msg.textContent=r.ok?'Map deleted.':'Could not delete the map.';})"
        ".catch(function(){msg.textContent='Could not reach the device.';});});"
        "})();"
        "</script></div></details>",
        ui::radar::kSize, ui::radar::kCenterX, ui::radar::kCenterY, ui::radar::kGridOuterRadius,
        services::map_center::latitude(), services::map_center::longitude(),
        static_cast<unsigned>(ui::radar::scaleActiveMiles()),
        static_cast<unsigned>(
            ui::radar::kRangeMileOptions[ui::radar::kRangeMileOptionCount - 1]),
        static_cast<unsigned>(ui::radar::facingDeg()), carto_js,
        static_cast<unsigned>(services::basemap::contrastPercentDark()),
        static_cast<unsigned>(services::basemap::contrastPercentLight()),
        static_cast<unsigned>(services::basemap::washPercentVfr()));
    appendClamped(page, kSettingsPageCap, &used, bm2);
  }

  // ---------- Alerts card ----------
  appendRaw(page, kSettingsPageCap, &used,
            "<details class=\"card\" open><summary><span class=\"ico\">&#9888;</span>Alerts"
            "<span class=\"sum\">aircraft alerts</span><span class=\"chev\">&#9656;</span>"
            "</summary><div class=\"body\">");
  appendToggle(page, kSettingsPageCap, &used, "alert_mil", "Alert on military planes",
               services::alert::militaryAlertEnabled());
  appendToggle(page, kSettingsPageCap, &used, "alert_emrg",
               "Alert on emergencies (squawk 7700 / 7600 / 7500)",
               services::alert::emergencyAlertEnabled());
  appendToggle(page, kSettingsPageCap, &used, "alert_heli", "Alert on helicopters",
               services::alert::helicopterAlertEnabled());
  appendToggle(page, kSettingsPageCap, &used, "alert_ga",
               "Alert on private planes",
               services::alert::privateAlertEnabled());
  appendToggle(page, kSettingsPageCap, &used, "alert_appr",
               "Alert when a plane is about to pass overhead (within 2 mi, in the next 3 min), "
               "not when it first appears",
               services::alert::alertOnApproachEnabled());
  appendToggle(page, kSettingsPageCap, &used, "alert_ovh",
               "Overhead alert (any plane below 10,000 ft about to pass overhead)",
               services::alert::overheadAnyEnabled());
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
  appendToggle(page, kSettingsPageCap, &used, "alert_notbl",
               "Special planes alert (go outside and look: Hurricane Hunters, Coast Guard, "
               "tankers, Air Force One, 747s, A380, Beluga...)",
               services::alert::notableAlertEnabled());
#endif
  appendToggle(page, kSettingsPageCap, &used, "type_colors",
               "Color planes by type (red military, blue helicopter, white private)",
               services::alert::typeColorsEnabled());
  appendToggle(page, kSettingsPageCap, &used, "alert_hide",
               "Show only alerted planes on the radar",
               services::alert::hideNonAlertedEnabled());
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
  appendToggle(page, kSettingsPageCap, &used, "sky_launch",
               "Rocket launches from Cape Canaveral / KSC: countdown banner and chime (T-10 min, liftoff)",
               services::sky::launchesEnabled());
  appendToggle(page, kSettingsPageCap, &used, "sky_iss",
               "Visible ISS passes: banner and chime 2 min before",
               services::sky::issEnabled());
#endif
  const int watch_n = snprintf(
      page + used, kSettingsPageCap - used,
      "<label for=\"alert_watch\">Watch these flights (e.g. UAL123)</label>"
      "<input id=\"alert_watch\" type=\"text\" "
      "autocomplete=\"off\" placeholder=\"ACA739, UAL123\" value=\"%s\">",
      watch_buf);

  appendClamped(page, kSettingsPageCap, &used, watch_n);
  if (watch_count == 0) {
    appendRaw(page, kSettingsPageCap, &used,
              "<p class=\"usage\">No flight numbers tracked.</p>");
  } else {
    const int tracked_n = snprintf(page + used, kSettingsPageCap - used,
                                   "<p class=\"usage\"><b>Tracking %u:</b> %s</p>",
                                   static_cast<unsigned>(watch_count), watch_buf);
    if (tracked_n > 0) {
      appendClamped(page, kSettingsPageCap, &used, tracked_n);
    }
  }
  appendRaw(page, kSettingsPageCap, &used,
            "<p class=\"hint\">Comma-separated callsigns (3-letter airline + flight number). "
#if FLIGHTSCNR_HAS_SPEAKER
            "Chimes and flashes the ring when a watched flight appears on the radar. "
#elif FLIGHTSCNR_HAS_HAPTIC
            "Buzzes and highlights when a watched flight appears on radar. "
#else
            "Highlights a watched flight when it appears on the radar. "
#endif
            "To clear all, delete the text in the field above and tap <b>Save</b>.</p>");

  appendRaw(page, kSettingsPageCap, &used,
            (watch_type_count != 0 || watch_reg_count != 0)
                ? "<details class=\"adv\" open><summary>Watch plane types or tail numbers</summary>"
                : "<details class=\"adv\"><summary>Watch plane types or tail numbers</summary>");
  const int watch_type_n = snprintf(
      page + used, kSettingsPageCap - used,
      "<label for=\"alert_watch_type\">Watch these plane types (e.g. B738)</label>"
      "<input id=\"alert_watch_type\" type=\"text\" "
      "autocomplete=\"off\" placeholder=\"B738, A333, E75L\" value=\"%s\">",
      watch_type_buf);
  appendClamped(page, kSettingsPageCap, &used, watch_type_n);
  if (watch_type_count == 0) {
    appendRaw(page, kSettingsPageCap, &used,
              "<p class=\"usage\">No aircraft types tracked.</p>");
  } else {
    const int tracked_types_n =
        snprintf(page + used, kSettingsPageCap - used,
                 "<p class=\"usage\"><b>Tracking %u types:</b> %s</p>",
                 static_cast<unsigned>(watch_type_count), watch_type_buf);
    if (tracked_types_n > 0) {
      appendClamped(page, kSettingsPageCap, &used, tracked_types_n);
    }
  }
  appendRaw(page, kSettingsPageCap, &used,
            "<p class=\"hint\">Comma-separated <b>ICAO type designators</b> from ADS-B "
            "(2&#8211;4 chars, e.g. <code>B738</code>, <code>A333</code>). "
            "All aircraft of that type are alerted. Use the code shown on flight detail / "
            "radar tags &#8212; not marketing names like A330-743. "
            "Clear the field and <b>Save</b> to remove all types.</p>");

  const int watch_reg_n = snprintf(
      page + used, kSettingsPageCap - used,
      "<label for=\"alert_watch_reg\">Watch these tail numbers (e.g. N2136U)</label>"
      "<input id=\"alert_watch_reg\" type=\"text\" "
      "autocomplete=\"off\" placeholder=\"N2136U, CS-TPQ\" value=\"%s\">",
      watch_reg_buf);
  appendClamped(page, kSettingsPageCap, &used, watch_reg_n);
  if (watch_reg_count == 0) {
    appendRaw(page, kSettingsPageCap, &used,
              "<p class=\"usage\">No registrations tracked.</p>");
  } else {
    const int tracked_regs_n =
        snprintf(page + used, kSettingsPageCap - used,
                 "<p class=\"usage\"><b>Tracking %u regs:</b> %s</p>",
                 static_cast<unsigned>(watch_reg_count), watch_reg_buf);
    if (tracked_regs_n > 0) {
      appendClamped(page, kSettingsPageCap, &used, tracked_regs_n);
    }
  }
  appendRaw(page, kSettingsPageCap, &used,
            "<p class=\"hint\">Comma-separated civil registrations from ADS-B "
            "(e.g. <code>N2136U</code>, <code>CS-TPQ</code>). Hyphens optional when matching. "
            "Clear the field and <b>Save</b> to remove all.</p></details>");
  appendRaw(page, kSettingsPageCap, &used, "</div></details>");

  // ---------- Sound / haptic card ----------
#if FLIGHTSCNR_HAS_HAPTIC
  appendRaw(page, kSettingsPageCap, &used,
            "<details class=\"card\"><summary><span class=\"ico\">&#9835;</span>Haptic"
            "<span class=\"sum\">vibration</span><span class=\"chev\">&#9656;</span>"
            "</summary><div class=\"body\">");
  appendToggle(page, kSettingsPageCap, &used, "ui_beep", "Vibration on touch and knob",
               hardware::buzzerEnabled());
  appendRaw(page, kSettingsPageCap, &used,
            "<p class=\"hint\">Aircraft alerts show a thick red ring on the outer "
            "edge for 10 seconds. Vibration is only for taps and knob turns.</p>");
#else
#if FLIGHTSCNR_HAS_SPEAKER
  constexpr const char* kSoundSum = "alert chimes";
  constexpr const char* kSoundToggle = "Alert sound (chimes on the speaker)";
  constexpr const char* kSoundLevel = "Chime volume (A quiet &ndash; E loud)";
#else
  constexpr const char* kSoundSum = "UI beep";
  constexpr const char* kSoundToggle = "UI beep on touch and knob";
  constexpr const char* kSoundLevel = "Beep tone";
#endif
  {
    const int n = snprintf(
        page + used, kSettingsPageCap - used,
        "<details class=\"card\"><summary><span class=\"ico\">&#9835;</span>Sound"
        "<span class=\"sum\">%s</span><span class=\"chev\">&#9656;</span>"
        "</summary><div class=\"body\">",
        kSoundSum);
    appendClamped(page, kSettingsPageCap, &used, n);
  }
  appendToggle(page, kSettingsPageCap, &used, "ui_beep", kSoundToggle,
               hardware::buzzerEnabled());
  const char beep_tone = hardware::buzzerToneLetter();
  const int beep_n = snprintf(
      page + used, kSettingsPageCap - used,
      "<label for=\"beep_tone\">%s</label>"
      "<select id=\"beep_tone\" name=\"beep_tone\">"
      "<option value=\"A\"%s>A</option>"
      "<option value=\"B\"%s>B</option>"
      "<option value=\"C\"%s>C</option>"
      "<option value=\"D\"%s>D</option>"
      "<option value=\"E\"%s>E</option>"
      "</select>",
      kSoundLevel, beep_tone == 'A' ? " selected" : "", beep_tone == 'B' ? " selected" : "",
      beep_tone == 'C' ? " selected" : "", beep_tone == 'D' ? " selected" : "",
      beep_tone == 'E' ? " selected" : "");
  appendClamped(page, kSettingsPageCap, &used, beep_n);
#endif
  appendRaw(page, kSettingsPageCap, &used, "</div></details>");

  // ---------- Display & screens card ----------
  appendRaw(page, kSettingsPageCap, &used,
            "<details class=\"card\"><summary><span class=\"ico\">&#9788;</span>"
            "Screen &amp; clock<span class=\"sum\">brightness, clock, time zone</span>"
            "<span class=\"chev\">&#9656;</span></summary><div class=\"body\">");

  const uint8_t bright = hardware::displayBrightnessPercent();
  const int bright_n = snprintf(
      page + used, kSettingsPageCap - used,
      "<label for=\"bright_pct\">Screen brightness</label>"
      "<select id=\"bright_pct\" name=\"bright_pct\">"
      "<option value=\"20\"%s>20%%</option>"
      "<option value=\"40\"%s>40%%</option>"
      "<option value=\"60\"%s>60%%</option>"
      "<option value=\"80\"%s>80%%</option>"
      "<option value=\"100\"%s>100%%</option>"
      "</select>",
      bright == 20 ? " selected" : "", bright == 40 ? " selected" : "",
      bright == 60 ? " selected" : "", bright == 80 ? " selected" : "",
      bright == 100 ? " selected" : "");
  appendClamped(page, kSettingsPageCap, &used, bright_n);

  const unsigned long detail_sec = ui::displayPrefsFlightDetailTimeoutMs() / 1000UL;
  const unsigned long clock_sec = ui::displayPrefsClockWeatherTimeoutMs() / 1000UL;
  const int timeouts_n = snprintf(
      page + used, kSettingsPageCap - used,
      "<div class=\"row2\"><div>"
      "<label for=\"detail_timeout\">Close flight details after</label>"
      "<select id=\"detail_timeout\" name=\"detail_timeout\">"
      "<option value=\"0\"%s>Manual (swipe)</option>"
      "<option value=\"10\"%s>10 seconds</option>"
      "<option value=\"20\"%s>20 seconds</option>"
      "<option value=\"30\"%s>30 seconds</option>"
      "</select></div><div>"
      "<label for=\"clock_timeout\">Close clock / forecast after</label>"
      "<select id=\"clock_timeout\" name=\"clock_timeout\">"
      "<option value=\"0\"%s>Manual (swipe)</option>"
      "<option value=\"5\"%s>5 seconds</option>"
      "<option value=\"10\"%s>10 seconds</option>"
      "<option value=\"15\"%s>15 seconds</option>"
      "</select></div></div>",
      detail_sec == 0 ? " selected" : "", detail_sec == 10 ? " selected" : "",
      detail_sec == 20 ? " selected" : "", detail_sec == 30 ? " selected" : "",
      clock_sec == 0 ? " selected" : "", clock_sec == 5 ? " selected" : "",
      clock_sec == 10 ? " selected" : "", clock_sec == 15 ? " selected" : "");
  appendClamped(page, kSettingsPageCap, &used, timeouts_n);

  appendToggle(page, kSettingsPageCap, &used, "idle_clock",
               "Show the clock when no planes are near",
               ui::displayPrefsAutoIdleClockEnabled());
  appendToggle(page, kSettingsPageCap, &used, "clock_24h", "24-hour clock",
               services::clock::use24Hour());
  appendToggle(page, kSettingsPageCap, &used, "date_numeric",
               "Numeric date (20.07.2026)", services::clock::useNumericDate());
  appendToggle(page, kSettingsPageCap, &used, "auto_timezone",
               "Set the time zone from my location (with daylight saving)",
               services::clock::useAutoTimezone());
  appendRaw(page, kSettingsPageCap, &used,
#if FLIGHTSCNR_HAS_ENCODER
            "<p class=\"note\">Looks up the local time zone for your location over "
            "Wi&#8209;Fi. Turn the clock-settings knob on the device to override manually.</p>"
#else
            "<p class=\"note\">Looks up the local time zone for your location over "
            "Wi&#8209;Fi. To set it by hand on the device: swipe down from the radar to the "
            "clock, swipe left to Clock settings, tap Timezone, then tap its left or right "
            "side (this turns the automatic zone off).</p>"
#endif
  );

  appendRaw(page, kSettingsPageCap, &used, "</div></details>");

  // ---------- Off-Hours card ----------
  appendRaw(page, kSettingsPageCap, &used,
            "<details class=\"card\"><summary><span class=\"ico\">&#9790;</span>"
            "Night mode<span class=\"sum\">quiet hours</span>"
            "<span class=\"chev\">&#9656;</span></summary><div class=\"body\">");
  appendToggle(page, kSettingsPageCap, &used, "night_en", "Night mode",
               services::offhours::enabled());
  {
    const auto night_mode = services::offhours::mode();
    const int nm = snprintf(
        page + used, kSettingsPageCap - used,
        "<label for=\"night_mode\">At night</label>"
        "<select id=\"night_mode\" name=\"night_mode\">"
        "<option value=\"0\"%s>Dim clock (20%%)</option>"
        "<option value=\"1\"%s>Turn off display</option>"
        "</select>",
        night_mode == services::offhours::Mode::Dim ? " selected" : "",
        night_mode == services::offhours::Mode::DisplayOff ? " selected" : "");
    if (nm > 0) appendClamped(page, kSettingsPageCap, &used, nm);
  }
  {
    const uint16_t start = services::offhours::startMinute();
    const uint16_t end = services::offhours::endMinute();
    const int nt = snprintf(
        page + used, kSettingsPageCap - used,
        "<div class=\"row2\">"
        "<div><label for=\"night_start\">Start</label>"
        "<input type=\"time\" id=\"night_start\" name=\"night_start\" value=\"%02u:%02u\"></div>"
        "<div><label for=\"night_end\">End</label>"
        "<input type=\"time\" id=\"night_end\" name=\"night_end\" value=\"%02u:%02u\"></div>"
        "</div>",
        start / 60, start % 60, end / 60, end % 60);
    if (nt > 0) appendClamped(page, kSettingsPageCap, &used, nt);
  }
  appendRaw(page, kSettingsPageCap, &used,
#if FLIGHTSCNR_HAS_KNOB_BUTTON
            "<p class=\"note\">At night the device shows a dim clock or turns the screen "
            "off, and stops downloading flights. Knob press wakes the device.</p>");
#else
            "<p class=\"note\">At night the device shows a dim clock or turns the screen "
            "off, and stops downloading flights. Tap the screen to wake it for 5 minutes.</p>");
#endif
  appendRaw(page, kSettingsPageCap, &used, "</div></details>");

  // ---------- Weather card ----------
  appendRaw(page, kSettingsPageCap, &used,
            "<details class=\"card\"><summary><span class=\"ico\">&#9729;</span>Weather"
            "<span class=\"sum\">&deg;C / &deg;F, forecast</span><span class=\"chev\">&#9656;</span>"
            "</summary><div class=\"body\">");
  {
    const int wu_n = snprintf(
        page + used, kSettingsPageCap - used,
        "<label for=\"weather_units\">Temperature</label>"
        "<select id=\"weather_units\" name=\"weather_units\">"
        "<option value=\"metric\"%s>Celsius (&deg;C)</option>"
        "<option value=\"imperial\"%s>Fahrenheit (&deg;F)</option>"
        "</select>"
        "<p class=\"note\">The weather shows on the clock screen; swipe right there for the "
        "3-day forecast.</p>",
        services::weather::useImperial() ? "" : " selected",
        services::weather::useImperial() ? " selected" : "");
    appendClamped(page, kSettingsPageCap, &used, wu_n);
  }
  // Open-Meteo (free, key-less; used when Tomorrow.io is off/keyless or fails).
  appendToggle(page, kSettingsPageCap, &used, "use_openmeteo",
               "Free weather from Open-Meteo (no key needed)",
               services::apikeys::useOpenMeteo());
  appendRaw(page, kSettingsPageCap, &used,
            "<details class=\"adv\"><summary>Advanced: Tomorrow.io key</summary>");
  appendToggle(page, kSettingsPageCap, &used, "use_weather", "Use Tomorrow.io",
               services::apikeys::useWeather());
  services::apikeys::maskedWeather(masked, sizeof(masked));
  const int wx_n = snprintf(
      page + used, kSettingsPageCap - used,
      "<label for=\"weather_key\">Tomorrow.io key: %s</label>"
      "<input id=\"weather_key\" name=\"weather_key\" type=\"password\" "
      "autocomplete=\"off\" placeholder=\"paste key\">"
      "<p class=\"hint\">Optional. Open-Meteo is used when Tomorrow.io is off or "
      "unavailable.</p></details></div></details>",
      masked);
  appendClamped(page, kSettingsPageCap, &used, wx_n);

  // Close the settings form but keep .wrap open so Wi-Fi / route-cache cards
  // share the same column spacing (nested forms cannot live inside /save).
  const int form_close_n = snprintf(
      page + used, kSettingsPageCap - used,
      "<input type=\"hidden\" name=\"alert_watch\" id=\"alert_watch_post\" value=\"%s\">"
      "<input type=\"hidden\" name=\"alert_watch_type\" id=\"alert_watch_type_post\" value=\"%s\">"
      "<input type=\"hidden\" name=\"alert_watch_reg\" id=\"alert_watch_reg_post\" value=\"%s\">"
      "<script>"
      "document.getElementById('fs-save').addEventListener('submit',function(){"
      "var v=document.getElementById('alert_watch'),h=document.getElementById('alert_watch_post');"
      "if(v&&h){h.value=v.value;}"
      "var vt=document.getElementById('alert_watch_type'),"
      "ht=document.getElementById('alert_watch_type_post');"
      "if(vt&&ht){ht.value=vt.value;}"
      "var vr=document.getElementById('alert_watch_reg'),"
      "hr=document.getElementById('alert_watch_reg_post');"
      "if(vr&&hr){hr.value=vr.value;}"
      "});"
      "</script>"
      "</form>",
      watch_buf, watch_type_buf, watch_reg_buf);
  appendClamped(page, kSettingsPageCap, &used, form_close_n);

  // ---------- Wi-Fi networks card (above route cache) ----------
  appendRaw(page, kSettingsPageCap, &used,
            "<details class=\"card\"><summary><span class=\"ico\">W</span>"
            "Wi&#8209;Fi networks<span class=\"sum\">up to 3</span>"
            "<span class=\"chev\">&#9656;</span></summary><div class=\"body\">"
            "<p class=\"note\">The device tries them in order, #1 first. A network that "
            "keeps failing is skipped for now, until you edit or remove it.</p>");

  const uint8_t net_n = wifiNetsCount();
  if (net_n == 0) {
    appendRaw(page, kSettingsPageCap, &used, "<p class=\"note\">No saved networks.</p>");
  }
  for (uint8_t i = 0; i < net_n; ++i) {
    char ssid[33] = "";
    wifiNetsGetSsid(i, ssid, sizeof(ssid));
    char esc[96];
    size_t eo = 0;
    for (size_t c = 0; ssid[c] != '\0' && eo + 6 < sizeof(esc); ++c) {
      if (ssid[c] == '&') {
        memcpy(esc + eo, "&amp;", 5);
        eo += 5;
      } else if (ssid[c] == '<') {
        memcpy(esc + eo, "&lt;", 4);
        eo += 4;
      } else if (ssid[c] == '"') {
        memcpy(esc + eo, "&quot;", 6);
        eo += 6;
      } else {
        esc[eo++] = ssid[c];
      }
    }
    esc[eo] = '\0';

    const int row_n = snprintf(
        page + used, kSettingsPageCap - used,
        "<div class=\"net-row\"><div class=\"top\"><span class=\"ord\">#%u</span>"
        "<span class=\"ssid\">%s</span>%s</div>"
        "<div class=\"net-actions\">"
        "<form method=\"POST\" action=\"/wifi/up\" style=\"display:inline\">"
        "<input type=\"hidden\" name=\"i\" value=\"%u\">"
        "<button class=\"sm\" type=\"submit\"%s>Move up</button></form>"
        "<form method=\"POST\" action=\"/wifi/down\" style=\"display:inline\">"
        "<input type=\"hidden\" name=\"i\" value=\"%u\">"
        "<button class=\"sm\" type=\"submit\"%s>Move down</button></form>"
        "<form method=\"POST\" action=\"/wifi/remove\" style=\"display:inline\" "
        "onsubmit=\"return confirm('Remove this network?');\">"
        "<input type=\"hidden\" name=\"i\" value=\"%u\">"
        "<button class=\"sm danger\" type=\"submit\">Remove</button></form>"
        "</div>"
        "<form method=\"POST\" action=\"/wifi/pass\" style=\"margin-top:.55rem\">"
        "<input type=\"hidden\" name=\"i\" value=\"%u\">"
        "<label for=\"pass%u\">Update password</label>"
        "<div class=\"row2\"><input id=\"pass%u\" name=\"p\" type=\"password\" "
        "autocomplete=\"new-password\" placeholder=\"new password\">"
        "<button class=\"sm\" type=\"submit\">Update</button></div></form></div>",
        static_cast<unsigned>(i + 1), esc,
        wifiNetsIsDemoted(i) ? " <span class=\"skip\">temporarily skipped</span>" : "",
        static_cast<unsigned>(i), i == 0 ? " disabled" : "",
        static_cast<unsigned>(i), (i + 1 >= net_n) ? " disabled" : "",
        static_cast<unsigned>(i), static_cast<unsigned>(i), static_cast<unsigned>(i),
        static_cast<unsigned>(i));
    if (row_n > 0) {
      appendClamped(page, kSettingsPageCap, &used, row_n);
    }
  }

  if (net_n < config::kWifiMaxNetworks) {
    const int add_n = snprintf(
        page + used, kSettingsPageCap - used,
        "<div class=\"wifi-add\"><form method=\"POST\" action=\"/wifi/add\">"
        "<label for=\"wifi_ssid\">Add network (%u of %u used)</label>"
        "<input id=\"wifi_ssid\" name=\"s\" maxlength=\"32\" required "
        "placeholder=\"SSID\">"
        "<label for=\"wifi_pass\">Password</label>"
        "<input id=\"wifi_pass\" name=\"p\" type=\"password\" maxlength=\"63\" "
        "autocomplete=\"new-password\" placeholder=\"password\">"
        "<p style=\"margin-top:.6rem\"><button class=\"sm\" type=\"submit\">"
        "Add network</button></p></form></div>",
        static_cast<unsigned>(net_n), static_cast<unsigned>(config::kWifiMaxNetworks));
    if (add_n > 0) {
      appendClamped(page, kSettingsPageCap, &used, add_n);
    }
  } else {
    appendRaw(page, kSettingsPageCap, &used,
              "<p class=\"note\">All 3 places are used. Remove one to add another.</p>");
  }

  appendRaw(page, kSettingsPageCap, &used,
#if FLIGHTSCNR_HAS_KNOB_BUTTON
            "<p class=\"wifi-note\">Hold knob 5&nbsp;s clears all networks and opens the setup "
            "portal.</p></div></details>");
#else
            "<p class=\"wifi-note\">To forget every network and start Wi&#8209;Fi setup again: on "
            "the device, swipe left on the radar, then on Settings 1/4 tap <b>Reset Wi-Fi</b> "
            "twice.</p></div></details>");
#endif

  // ---------- Advanced group (collapsed): route lookups, cache, disclaimer, firmware ----------
  // Route-lookup inputs sit outside the <form>; form="fs-save" still saves them.
  appendRaw(page, kSettingsPageCap, &used,
            services::ota_github::updateAvailable()
                ? "<details class=\"card grp\" open>"
                : "<details class=\"card grp\">");
  appendRaw(page, kSettingsPageCap, &used,
            "<summary><span class=\"ico\">&#9881;</span>Advanced"
            "<span class=\"sum\">route lookups, cache, firmware</span>"
            "<span class=\"chev\">&#9656;</span></summary><div class=\"body\">");

  // ---------- Route APIs card ----------
  // Runtime order: AirLabs → FlightAware → FR24 → adsbdb → prefix.
  appendRaw(page, kSettingsPageCap, &used,
            "<details class=\"card\"><summary><span class=\"ico\">&#9992;</span>Route lookups"
            "<span class=\"sum\">API keys, monthly limits</span><span class=\"chev\">&#9656;</span>"
            "</summary><div class=\"body\">"
            "<p class=\"note\">Where a flight comes from and goes to, shown on the flight "
            "details screen (tap a plane). The free adsbdb source needs no key. Paid services "
            "need a key: paste it and its switch turns on. They are tried in order "
            "<b>AirLabs &rarr; FlightAware &rarr; FR24 &rarr; adsbdb</b>. Several keys: separate "
            "them with commas; when one reaches its monthly limit the next is used. Leave a key "
            "box empty to keep the saved key. Limits reset on the 1st of the month.</p>");

  // AirLabs (1st priority)
  appendRaw(page, kSettingsPageCap, &used, "<div class=\"api\"><div class=\"api-head\">");
  appendInlineToggle(page, kSettingsPageCap, &used, "use_airlabs",
                     services::apikeys::useAirLabs());
  appendRaw(page, kSettingsPageCap, &used,
            "<span class=\"name\">AirLabs</span><span class=\"badge\">1st</span></div>");

  services::apikeys::maskedAirLabs(masked, sizeof(masked));
  char al_used_note[64];
  if (services::apikeys::airLabsMaxCalls() == 0) {
    snprintf(al_used_note, sizeof(al_used_note), "Used this month: %u (unlimited cap)",
             static_cast<unsigned>(services::apikeys::airLabsCallsUsed()));
  } else {
    snprintf(al_used_note, sizeof(al_used_note), "Used this month: %u / %u",
             static_cast<unsigned>(services::apikeys::airLabsCallsUsed()),
             static_cast<unsigned>(services::apikeys::airLabsMaxCalls()));
  }
  const int al_n = snprintf(
      page + used, kSettingsPageCap - used,
      "<label for=\"airlabs_key\">API keys: %s</label>"
      "<input id=\"airlabs_key\" name=\"airlabs_key\" form=\"fs-save\" type=\"password\" "
      "autocomplete=\"off\" placeholder=\"key1, key2, key3\">"
      "<label for=\"airlabs_max_calls\">Max calls / month per key (0 = unlimited; "
      "free tier 1,000/mo)</label>"
      "<input id=\"airlabs_max_calls\" name=\"airlabs_max_calls\" form=\"fs-save\" type=\"number\" min=\"0\" "
      "step=\"1\" value=\"%u\">"
      "<p class=\"usage\">%s</p></div>",
      masked, static_cast<unsigned>(services::apikeys::airLabsMaxCalls()), al_used_note);
  appendClamped(page, kSettingsPageCap, &used, al_n);

  // FlightAware (2nd priority)
  appendRaw(page, kSettingsPageCap, &used, "<div class=\"api\"><div class=\"api-head\">");
  appendInlineToggle(page, kSettingsPageCap, &used, "use_flightaware",
                     services::apikeys::useFlightAware());
  appendRaw(page, kSettingsPageCap, &used,
            "<span class=\"name\">FlightAware</span><span class=\"badge\">2nd</span></div>");

  services::apikeys::maskedFlightAware(masked, sizeof(masked));
  char fa_budget[16];
  char fa_cost[16];
  char fa_spent[16];
  formatUsdMicro(services::apikeys::flightAwareBudgetUsdMicro(), fa_budget, sizeof(fa_budget), 2);
  formatUsdMicro(services::apikeys::flightAwareCostUsdMicro(), fa_cost, sizeof(fa_cost), 4);
  formatUsdMicro(services::apikeys::flightAwareSpentUsdMicro(), fa_spent, sizeof(fa_spent), 4);
  char fa_used_note[72];
  if (services::apikeys::flightAwareBudgetUsdMicro() == 0) {
    snprintf(fa_used_note, sizeof(fa_used_note), "Spent this month: $%s (unlimited budget)",
             fa_spent);
  } else {
    snprintf(fa_used_note, sizeof(fa_used_note), "Spent this month: $%s of $%s", fa_spent,
             fa_budget);
  }
  const int fa_n = snprintf(
      page + used, kSettingsPageCap - used,
      "<label for=\"flightaware_key\">AeroAPI keys: %s</label>"
      "<input id=\"flightaware_key\" name=\"flightaware_key\" form=\"fs-save\" type=\"password\" "
      "autocomplete=\"off\" placeholder=\"key1, key2, key3\">"
      "<div class=\"row2\"><div>"
      "<label for=\"flightaware_max_usd\">Max budget / key ($, 0 = unlimited)</label>"
      "<input id=\"flightaware_max_usd\" name=\"flightaware_max_usd\" form=\"fs-save\" type=\"number\" min=\"0\" "
      "step=\"0.01\" value=\"%s\">"
      "</div><div>"
      "<label for=\"flightaware_cost_usd\">Cost per call ($)</label>"
      "<input id=\"flightaware_cost_usd\" name=\"flightaware_cost_usd\" form=\"fs-save\" type=\"number\" min=\"0\" "
      "step=\"0.0001\" value=\"%s\">"
      "</div></div>"
      "<p class=\"hint\">AeroAPI GET /flights/{ident}; default $0.005 per result set.</p>"
      "<p class=\"usage\">%s</p></div>",
      masked, fa_budget, fa_cost, fa_used_note);
  appendClamped(page, kSettingsPageCap, &used, fa_n);

  // FlightRadar24 (3rd priority)
  appendRaw(page, kSettingsPageCap, &used, "<div class=\"api\"><div class=\"api-head\">");
  appendInlineToggle(page, kSettingsPageCap, &used, "use_fr24", services::apikeys::useFr24());
  appendRaw(page, kSettingsPageCap, &used,
            "<span class=\"name\">FlightRadar24</span><span class=\"badge\">3rd</span></div>");

  services::apikeys::maskedFr24(masked, sizeof(masked));
  char fr_budget[16];
  char fr_cost[16];
  char fr_spent[16];
  formatUsdMicro(services::apikeys::fr24BudgetUsdMicro(), fr_budget, sizeof(fr_budget), 2);
  formatUsdMicro(services::apikeys::fr24CostUsdMicro(), fr_cost, sizeof(fr_cost), 4);
  formatUsdMicro(services::apikeys::fr24SpentUsdMicro(), fr_spent, sizeof(fr_spent), 4);
  char fr_used_note[72];
  if (services::apikeys::fr24BudgetUsdMicro() == 0) {
    snprintf(fr_used_note, sizeof(fr_used_note), "Spent this month: $%s (unlimited budget)",
             fr_spent);
  } else {
    snprintf(fr_used_note, sizeof(fr_used_note), "Spent this month: $%s of $%s", fr_spent,
             fr_budget);
  }
  const int fr_n = snprintf(
      page + used, kSettingsPageCap - used,
      "<label for=\"fr24_key\">API tokens: %s</label>"
      "<input id=\"fr24_key\" name=\"fr24_key\" form=\"fs-save\" type=\"password\" "
      "autocomplete=\"off\" placeholder=\"token1, token2, token3\">"
      "<div class=\"row2\"><div>"
      "<label for=\"fr24_max_usd\">Max budget / key ($, 0 = unlimited)</label>"
      "<input id=\"fr24_max_usd\" name=\"fr24_max_usd\" form=\"fs-save\" type=\"number\" min=\"0\" "
      "step=\"0.01\" value=\"%s\">"
      "</div><div>"
      "<label for=\"fr24_cost_usd\">Cost per call ($)</label>"
      "<input id=\"fr24_cost_usd\" name=\"fr24_cost_usd\" form=\"fs-save\" type=\"number\" min=\"0\" "
      "step=\"0.0001\" value=\"%s\">"
      "</div></div>"
      "<p class=\"usage\">%s</p></div>",
      masked, fr_budget, fr_cost, fr_used_note);
  appendClamped(page, kSettingsPageCap, &used, fr_n);

  // adsbdb.com (free, key-less; used after the paid APIs)
  appendRaw(page, kSettingsPageCap, &used, "<div class=\"api\">");
  appendToggle(page, kSettingsPageCap, &used, "use_adsbdb", "Use adsbdb (free)",
               services::apikeys::useAdsbDb());
  appendRaw(page, kSettingsPageCap, &used,
            "<p class=\"usage\">Free route source (adsbdb.com), no key needed. "
            "Used after the paid APIs above (default on).</p></div>");

  appendRaw(page, kSettingsPageCap, &used, "</div></details>");

  // ---------- Route cache card ----------
  appendRaw(page, kSettingsPageCap, &used,
            "<details class=\"card\"><summary><span class=\"ico\">&#8681;</span>Route cache"
            "<span class=\"sum\">download / clear</span><span class=\"chev\">&#9656;</span>"
            "</summary><div class=\"body\">"
            "<p class=\"note\">Routes already looked up are kept on the device (saved about "
            "every 10&nbsp;min), so the same flight is not looked up (and billed) again. Clear "
            "it if routes look wrong after changing route-lookup settings.</p>"
            "<a class=\"dl\" href=\"/route_cache.csv\" download=\"route_cache.csv\">"
            "&#8681;&nbsp; Download route_cache.csv</a>"
            "<form method=\"POST\" action=\"/route_cache/clear\" style=\"margin-top:.75rem\""
            " onsubmit=\"return confirm('Clear all cached airline/route lookups?');\">"
            "<button class=\"sm\" type=\"submit\">Clear route cache</button>"
            "</form></div></details>");

  // ---------- Disclaimer card (clear only; enable is touchscreen-only) ----------
  {
    const bool remembered = services::disclaimer::isRemembered();
    const int disc_n = snprintf(
        page + used, kSettingsPageCap - used,
        "<details class=\"card\"><summary><span class=\"ico\">&#9878;</span>Disclaimer"
        "<span class=\"sum\">%s</span><span class=\"chev\">&#9656;</span>"
        "</summary><div class=\"body\">"
        "<p class=\"note\">The safety disclaimer appears at every start. "
        "<b>Don&rsquo;t show again</b> can only be turned on at the device "
        "(tap the checkbox, then <b>Accept</b>). After that, the screen shows a "
        "five-second countdown instead of the Accept button."
#if FLIGHTSCNR_HAS_ENCODER
        " Knob/encoder cannot accept or remember."
#endif
        "</p>"
        "<p id=\"disclaimer_status\">Status: <b>%s</b></p>",
        remembered ? "remembered" : "ask every boot",
        remembered ? "Remembered on this device (5&nbsp;s countdown)"
                   : "Not remembered &mdash; wait for Accept each boot");
    appendClamped(page, kSettingsPageCap, &used, disc_n);
    if (remembered) {
      appendRaw(page, kSettingsPageCap, &used,
                "<form method=\"POST\" action=\"/disclaimer/clear\" style=\"margin-top:.75rem\""
                " onsubmit=\"return confirm('Clear saved disclaimer acceptance? "
                "The next boot will wait for on-screen Accept.');\">"
                "<button class=\"sm\" type=\"submit\">Clear saved disclaimer acceptance</button>"
                "</form>");
    }
    appendRaw(page, kSettingsPageCap, &used, "</div></details>");
  }

  appendFirmwareUpdateCard(page, &used);

  // Plain restart: keeps Wi-Fi, settings and the map (unlike Reset Wi-Fi).
  appendRaw(page, kSettingsPageCap, &used,
            "<details class=\"card\"><summary><span class=\"ico\">&#8635;</span>Restart device"
            "<span class=\"chev\">&#9656;</span></summary><div class=\"body\">"
            "<p class=\"note\">Restarts the radar. Wi-Fi, settings and the map are kept. "
            "It is back in about 15 seconds.</p>"
            "<form method=\"POST\" action=\"/restart\""
            " onsubmit=\"return confirm('Restart the radar now?');\">"
            "<button class=\"sm\" type=\"submit\">Restart now</button></form>"
            "</div></details>");

  appendRaw(page, kSettingsPageCap, &used, "</div></details>");

  // Live sync with the device. Rules (the page must never lose or silently revert an edit):
  //  - a field the user touched (input/change) is "dirty" and is never overwritten;
  //  - the focused field is not overwritten (it is retried after it loses focus);
  //  - if the device changed a dirty field, a note says so instead of overwriting it;
  //  - polls 1 s, backs off to 3 s after ~10 s without changes, stops while hidden;
  //  - leaving the page with unsaved edits (or during Make map) asks first.
  // Save reloads the page, which clears the dirty flags.
  const int tail_n = snprintf(
      page + used, kSettingsPageCap - used,
      "<p class=\"foot\"><a href=\"%s\" target=\"_blank\" rel=\"noopener\">"
      "github.com/yashmulgaonkar/FlightScnr</a> &middot; CC BY-NC-SA 4.0</p>"
      "</div>"
      "<div class=\"savebar\"><div class=\"inner\">"
      "<p id=\"dev_note\" class=\"dev\" role=\"status\" style=\"display:none;margin:0 0 .5rem\"></p>"
      "<button class=\"save\" type=\"submit\" form=\"fs-save\">Save</button></div></div>"
      "<div id=\"sync_note\" role=\"status\" style=\"display:none;position:fixed;left:50%%;"
      "bottom:4.6rem;transform:translateX(-50%%);z-index:25;background:#143d22;"
      "border:1px solid #1a9c3c;color:#d8ffe4;border-radius:999px;padding:.35rem .75rem;"
      "font-size:.78rem\">Updated from the device</div>"
      "<script>"
      "(function(){"
      "var IDS=('radar_center range_mi dist_unit min_height max_height radar_accent "
      "show_cardinals facing_deg show_sweep plane_labels use_basemap bm_contrast_dark "
      "bm_contrast_light bm_wash_vfr bright_pct detail_timeout clock_timeout idle_clock "
      "clock_24h date_numeric auto_timezone night_en night_mode night_start night_end ui_beep "
      "beep_tone alert_mil alert_emrg alert_heli alert_ga alert_appr alert_ovh type_colors "
      "alert_hide alert_watch alert_watch_type alert_watch_reg use_airlabs use_flightaware "
      "use_fr24 use_adsbdb airlabs_max_calls flightaware_max_usd flightaware_cost_usd "
      "fr24_max_usd fr24_cost_usd use_weather use_openmeteo weather_units"
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
      " overhead_sign hide_airliners use_rain alert_notbl sky_launch sky_iss"
#endif
      "').split(' ');"
      "var F=document.getElementById('fs-save'),dirty=window.fsDirty={},base={},last=null,"
      "quiet=0,seq=0,timer=null,saving=false;"
      // basemap_style is a choice for the next Make map, not a saved setting.
      "function mark(e){var t=e.target;"
      "if(t&&t.id&&t.form===F&&t.id!=='basemap_style'){dirty[t.id]=1;quiet=0;}}"
      "document.addEventListener('input',mark,true);"
      "document.addEventListener('change',mark,true);"
      "F.addEventListener('submit',function(){saving=true;});"
      "window.addEventListener('beforeunload',function(e){"
      "if(saving)return;"
      "if(window.fsBusy||Object.keys(dirty).length){e.preventDefault();e.returnValue='';}});"
      "function val(el){return el.type==='checkbox'?el.checked:el.value;}"
      "function lbl(id){var el=document.getElementById(id),"
      "a=el&&el.closest?el.closest('.api'):null,n=a&&a.querySelector('.name');"
      "if(n)return n.textContent;"
      "var l=document.querySelector('label[for=\"'+id+'\"]');"
      "return l?l.textContent.replace(/\\s*\\(.*$/,''):id;}"
      "function apply(j){"
      "window.fsDev=j;var chg=0,conf=[];"
      "IDS.forEach(function(id){"
      "var el=document.getElementById(id);if(!el||!(id in j))return;"
      "var v=el.type==='checkbox'?!!j[id]:String(j[id]);"
      "if(base[id]===v)return;"
      "if(dirty[id]){if(val(el)===v){base[id]=v;delete dirty[id];}"
      "else if(id in base)conf.push(lbl(id));return;}"
      "if(el===document.activeElement)return;"
      "base[id]=v;"
      "if(val(el)!==v){if(el.type==='checkbox')el.checked=v;else el.value=v;chg=1;}});"
      "var dn=document.getElementById('dev_note');"
      "if(conf.length){dn.textContent='Also changed on the device: '+conf.join(', ')+"
      "'. Save keeps what you entered here; reload the page to see the device\\u2019s values.';"
      "dn.style.display='block';}"
      "var bs=document.getElementById('bm_status');"
      "if(bs&&typeof j.bm_status==='string')bs.textContent=j.bm_status;"
      "var ds=document.getElementById('disclaimer_status');"
      "if(ds&&typeof j.disclaimer_remembered==='boolean'){"
      "ds.innerHTML=j.disclaimer_remembered"
      "?'Status: <b>Remembered on this device (5&nbsp;s countdown)</b>'"
      ":'Status: <b>Not remembered &mdash; wait for Accept each boot</b>';}"
      "if(chg){var n=document.getElementById('sync_note');"
      "n.style.display='block';clearTimeout(n._t);"
      "n._t=setTimeout(function(){n.style.display='none';},1500);}"
      "}"
      "function poll(s){"
      "if(s!==seq)return;"
      "if(document.visibilityState==='hidden'){timer=null;return;}"
      "fetch('/api/settings',{cache:'no-store'}).then(function(r){return r.text();})"
      ".then(function(t){if(t!==last){var j=JSON.parse(t);last=t;quiet=0;apply(j);}"
      "else quiet++;})"
      ".catch(function(){quiet++;})"
      ".then(function(){if(s===seq)timer=setTimeout(function(){poll(s);},"
      "quiet>10?3000:1000);});"
      "}"
      "function arm(){if(timer)clearTimeout(timer);timer=null;seq++;quiet=0;poll(seq);}"
      "document.addEventListener('visibilitychange',function(){"
      "if(document.visibilityState==='visible')arm();"
      "else{if(timer)clearTimeout(timer);timer=null;seq++;}"
      "});"
      "arm();"
      "})();"
      "</script>"
      "</body></html>",
      config::kGithubRepoUrl);
  if (tail_n > 0) {
    const size_t space = kSettingsPageCap - used;
    used += static_cast<size_t>(tail_n) < space ? static_cast<size_t>(tail_n) : space - 1;
  }
  if (used >= kSettingsPageCap) {
    used = kSettingsPageCap - 1;
  }
  page[used] = '\0';
  if (used >= kSettingsPageCap - 512) {
    Serial.printf("[settings] page warn used=%u cap=%u\n", static_cast<unsigned>(used),
                  static_cast<unsigned>(kSettingsPageCap));
  }

  // Zero-copy send: set the length up front, emit headers with an empty body,
  // then stream the PSRAM buffer directly. Never pass `page` to send() — the
  // const char* overload duplicates the whole page into an internal-heap String.
  s_server->setContentLength(used);
  s_server->send(200, "text/html; charset=utf-8", "");
  s_server->sendContent(page, used);
}

void handleSave() {
  if (s_server->method() != HTTP_POST) {
    s_server->send(405, "text/plain", "Method Not Allowed");
    return;
  }

  const bool loc_ok = settingsApplyFromForm(
      s_server->arg("radar_center").c_str(), nullptr, nullptr,
      s_server->arg("dist_unit").c_str(), s_server->arg("use_miles").c_str(),
      s_server->arg("show_cardinals").c_str(),
      s_server->arg("min_height").c_str(),
      s_server->arg("max_height").c_str(),
      s_server->arg("range_mi").c_str(), s_server->arg("airlabs_key").c_str(),
      s_server->arg("flightaware_key").c_str(), s_server->arg("fr24_key").c_str(),
      s_server->arg("use_airlabs").c_str(), s_server->arg("use_flightaware").c_str(),
      s_server->arg("use_fr24").c_str(), s_server->arg("airlabs_max_calls").c_str(),
      s_server->arg("flightaware_max_usd").c_str(),
      s_server->arg("flightaware_cost_usd").c_str(), s_server->arg("fr24_max_usd").c_str(),
      s_server->arg("fr24_cost_usd").c_str(), s_server->arg("ui_beep").c_str(),
      s_server->arg("beep_tone").c_str(), s_server->arg("bright_pct").c_str(),
      s_server->arg("show_sweep").c_str(), s_server->arg("detail_timeout").c_str());

  const bool use_weather_before = services::apikeys::useWeather();
  const bool use_openmeteo_before = services::apikeys::useOpenMeteo();
  const bool weather_key_saved =
      services::apikeys::saveWeatherKeyFromForm(s_server->arg("weather_key").c_str());
  services::apikeys::saveCartoKeyFromForm(s_server->arg("carto_key").c_str());
  services::apikeys::saveWeatherEnabledFromForm(s_server->arg("use_weather").c_str());
  services::apikeys::saveOpenMeteoEnabledFromForm(s_server->arg("use_openmeteo").c_str());
  services::apikeys::saveAdsbDbEnabledFromForm(s_server->arg("use_adsbdb").c_str());
  services::weather::saveUnitsFromForm(s_server->arg("weather_units").c_str());
  ui::displayPrefsSaveClockWeatherTimeoutFromForm(s_server->arg("clock_timeout").c_str());
  // The page shows "Plane labels" (on = labels shown); the stored setting is the
  // inverse ("hide blip details"). An unchecked box is simply absent from the form.
  ui::displayPrefsSaveHideBlipDetailsFromForm(s_server->hasArg("plane_labels") ? "" : "T");
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
  ui::displayPrefsSaveOverheadSignFromForm(s_server->arg("overhead_sign").c_str());
  services::alert::saveHideAirlinersFromForm(s_server->arg("hide_airliners").c_str());
#endif
  ui::displayPrefsSaveAutoIdleClockFromForm(s_server->arg("idle_clock").c_str());
  services::basemap::saveEnabledFromForm(s_server->arg("use_basemap").c_str());
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
  services::rain::saveEnabledFromForm(s_server->arg("use_rain").c_str());
#endif
  services::basemap::saveBakeAdjustFromForm(s_server->arg("bm_contrast_dark").c_str(),
                                            s_server->arg("bm_contrast_light").c_str(),
                                            s_server->arg("bm_wash_vfr").c_str());
  ui::radar::saveFacingDegFromForm(s_server->arg("facing_deg").c_str());
  services::clock::saveHourFormatFromForm(s_server->arg("clock_24h").c_str());
  services::clock::saveDateFormatFromForm(s_server->arg("date_numeric").c_str());
  const bool auto_tz_before = services::clock::useAutoTimezone();
  services::clock::saveAutoTimezoneFromForm(s_server->arg("auto_timezone").c_str());
  ui::radar::accentSaveFromForm(s_server->arg("radar_accent").c_str());
  services::offhours::saveFromForm(s_server->arg("night_en").c_str(),
                                   s_server->arg("night_mode").c_str(),
                                   s_server->arg("night_start").c_str(),
                                   s_server->arg("night_end").c_str());
  const bool watch_arg_present = s_server->hasArg("alert_watch");
  char watch_form[160] = "";
  if (watch_arg_present) {
    strncpy(watch_form, s_server->arg("alert_watch").c_str(), sizeof(watch_form) - 1);
    watch_form[sizeof(watch_form) - 1] = '\0';
    Serial.printf("[alert] form watch='%s'\n", watch_form);
  } else {
    Serial.println("[alert] form watch arg missing (keeping saved list)");
  }
  const bool watch_type_arg_present = s_server->hasArg("alert_watch_type");
  char watch_type_form[96] = "";
  if (watch_type_arg_present) {
    strncpy(watch_type_form, s_server->arg("alert_watch_type").c_str(),
            sizeof(watch_type_form) - 1);
    watch_type_form[sizeof(watch_type_form) - 1] = '\0';
    Serial.printf("[alert] form watch_type='%s'\n", watch_type_form);
  } else {
    Serial.println("[alert] form watch_type arg missing (keeping saved list)");
  }
  const bool watch_reg_arg_present = s_server->hasArg("alert_watch_reg");
  char watch_reg_form[160] = "";
  if (watch_reg_arg_present) {
    strncpy(watch_reg_form, s_server->arg("alert_watch_reg").c_str(),
            sizeof(watch_reg_form) - 1);
    watch_reg_form[sizeof(watch_reg_form) - 1] = '\0';
    Serial.printf("[alert] form watch_reg='%s'\n", watch_reg_form);
  } else {
    Serial.println("[alert] form watch_reg arg missing (keeping saved list)");
  }
  services::alert::saveFromForm(s_server->arg("alert_mil").c_str(),
                                s_server->arg("alert_emrg").c_str(),
                                s_server->arg("alert_hide").c_str(),
                                watch_arg_present ? watch_form : nullptr, watch_arg_present,
                                watch_type_arg_present ? watch_type_form : nullptr,
                                watch_type_arg_present,
                                watch_reg_arg_present ? watch_reg_form : nullptr,
                                watch_reg_arg_present);
  services::alert::saveApproachFromForm(s_server->arg("alert_appr").c_str(),
                                        s_server->arg("alert_ovh").c_str());
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
  services::alert::saveNotableFromForm(s_server->arg("alert_notbl").c_str());
#endif
  services::alert::saveClassAlertsFromForm(s_server->arg("alert_heli").c_str(),
                                           s_server->arg("alert_ga").c_str(),
                                           s_server->arg("type_colors").c_str());
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
  services::sky::saveFromForm(s_server->arg("sky_launch").c_str(),
                              s_server->arg("sky_iss").c_str());
#endif

  Serial.printf("Settings web save (lat/lon %s)\n", loc_ok ? "ok" : "invalid");

  if (!loc_ok) {
    sendLocationErrorPage();
    return;
  }

  if (weather_key_saved || use_weather_before != services::apikeys::useWeather() ||
      use_openmeteo_before != services::apikeys::useOpenMeteo()) {
    services::weather::notifyEnabledChanged();
  }

  if (auto_tz_before != services::clock::useAutoTimezone()) {
    if (services::clock::useAutoTimezone()) {
      services::tzlookup::notifyLocationChanged();
    }
  }

  redirectToSettings("saved=1");
  s_server->client().flush();
  settingsNotifySaved();
}

void handleRouteCacheDownload() {
  services::route_cache::sendDownload(s_server);
}

void handleRouteCacheClear() {
  if (s_server->method() != HTTP_POST) {
    s_server->send(405, "text/plain", "Method Not Allowed");
    return;
  }
  services::route::clearRamCache();
  const bool ok = services::route_cache::clear();
  redirectToSettings(ok ? "cache_cleared=1" : "cache_err=1");
}

void handleDisclaimerClear() {
  if (s_server->method() != HTTP_POST) {
    s_server->send(405, "text/plain", "Method Not Allowed");
    return;
  }
  services::disclaimer::clear();
  redirectToSettings("disclaimer_cleared=1");
  settingsNotifySaved();
}

void handleRestart() {
  if (s_server->method() != HTTP_POST) {
    s_server->send(405, "text/plain", "Method Not Allowed");
    return;
  }
  Serial.println("[settings] restart requested from the web page");
  s_server->send(200, "text/html",
                 "<!doctype html><meta name=\"viewport\" content=\"width=device-width\">"
                 "<body style=\"font-family:system-ui,sans-serif;background:#111;color:#eee;"
                 "max-width:28rem;margin:2rem auto;padding:0 1rem\">"
                 "<h1 style=\"font-size:1.2rem\">Restarting...</h1>"
                 "<p>The radar is restarting. This page reloads in 20 seconds.</p>"
                 "<script>setTimeout(function(){location.href='/';},20000);</script>");
  delay(500);  // let the reply leave before the reset
  ESP.restart();
}

bool s_basemap_upload_failed = false;

void handleBasemapUploadDone() {
  if (s_basemap_upload_failed) {
    s_server->send(400, "text/plain", "basemap upload failed");
    return;
  }
  s_server->send(200, "text/plain", "ok");
  settingsNotifySaved();
}

void handleBasemapUpload() {
  HTTPUpload& upload = s_server->upload();
  switch (upload.status) {
    case UPLOAD_FILE_START: {
      s_basemap_upload_failed = false;
      Serial.printf("[basemap] upload start: %s\n", upload.filename.c_str());
      services::basemap::uploadBegin();
      break;
    }
    case UPLOAD_FILE_WRITE: {
      if (s_basemap_upload_failed) {
        break;
      }
      if (!services::basemap::uploadWrite(upload.buf, upload.currentSize)) {
        s_basemap_upload_failed = true;
      }
      break;
    }
    case UPLOAD_FILE_END: {
      if (s_basemap_upload_failed) {
        services::basemap::uploadAbort();
        break;
      }
      services::basemap::Style style = services::basemap::Style::Dark;
      if (s_server->hasArg("style")) {
        const String sty = s_server->arg("style");
        if (sty == "light") {
          style = services::basemap::Style::Light;
        } else if (sty == "vfr") {
          style = services::basemap::Style::Vfr;
        } else if (sty == "voyager") {
          style = services::basemap::Style::Voyager;
        }
      }
      uint8_t mi = ui::radar::scaleActiveMiles();
      if (s_server->hasArg("mi")) {
        const long v = strtol(s_server->arg("mi").c_str(), nullptr, 10);
        if (v > 0 && v <= 255) {
          mi = static_cast<uint8_t>(v);
        }
      }
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
      if (s_server->hasArg("detail")) {
        if (!services::basemap::uploadFinishDetail(mi)) {
          s_basemap_upload_failed = true;
        }
        break;
      }
#endif
      if (!services::basemap::uploadFinish(upload.totalSize, style, mi)) {
        s_basemap_upload_failed = true;
      }
      break;
    }
    case UPLOAD_FILE_ABORTED: {
      s_basemap_upload_failed = true;
      services::basemap::uploadAbort();
      break;
    }
    default:
      break;
  }
}

void handleBasemapClear() {
  if (s_server->method() != HTTP_POST) {
    s_server->send(405, "text/plain", "Method Not Allowed");
    return;
  }
  // Called from the page's script (fetch), so answer plainly instead of redirecting:
  // a redirect made the browser download the whole settings page again for nothing.
  const bool ok = services::basemap::clear();
  s_server->send(ok ? 200 : 500, "text/plain", ok ? "ok" : "basemap clear failed");
  if (ok) {
    settingsNotifySaved();
  }
}

void handleBasemapAdjust() {
  if (s_server->method() != HTTP_POST) {
    s_server->send(405, "text/plain", "Method Not Allowed");
    return;
  }
  services::basemap::saveBakeAdjustFromForm(s_server->arg("bm_contrast_dark").c_str(),
                                            s_server->arg("bm_contrast_light").c_str(),
                                            s_server->arg("bm_wash_vfr").c_str());
  if (s_server->hasArg("carto_key")) {
    services::apikeys::saveCartoKeyFromForm(s_server->arg("carto_key").c_str());
  }
  settingsStateBump();
  s_server->send(200, "text/plain", "ok");
}

void handleWifiAdd() {
  if (s_server->method() != HTTP_POST) {
    s_server->send(405, "text/plain", "Method Not Allowed");
    return;
  }
  char err[96];
  const bool ok =
      wifiNetsAddOrUpdate(s_server->arg("s").c_str(), s_server->arg("p").c_str(), err,
                          sizeof(err));
  if (ok) {
    settingsStateBump();
  }
  redirectToSettings(ok ? "wifi_ok=1" : "wifi_err=1");
}

void handleWifiRemove() {
  if (s_server->method() != HTTP_POST) {
    s_server->send(405, "text/plain", "Method Not Allowed");
    return;
  }
  const bool ok = wifiNetsRemove(static_cast<uint8_t>(s_server->arg("i").toInt()));
  if (ok) {
    settingsStateBump();
  }
  redirectToSettings(ok ? "wifi_ok=1" : "wifi_err=1");
}

void handleWifiUp() {
  if (s_server->method() != HTTP_POST) {
    s_server->send(405, "text/plain", "Method Not Allowed");
    return;
  }
  wifiNetsMoveUp(static_cast<uint8_t>(s_server->arg("i").toInt()));
  settingsStateBump();
  redirectToSettings("wifi_ok=1");
}

void handleWifiDown() {
  if (s_server->method() != HTTP_POST) {
    s_server->send(405, "text/plain", "Method Not Allowed");
    return;
  }
  wifiNetsMoveDown(static_cast<uint8_t>(s_server->arg("i").toInt()));
  settingsStateBump();
  redirectToSettings("wifi_ok=1");
}

void handleWifiPass() {
  if (s_server->method() != HTTP_POST) {
    s_server->send(405, "text/plain", "Method Not Allowed");
    return;
  }
  const bool ok = wifiNetsUpdatePassword(static_cast<uint8_t>(s_server->arg("i").toInt()),
                                         s_server->arg("p").c_str());
  if (ok) {
    settingsStateBump();
  }
  redirectToSettings(ok ? "wifi_ok=1" : "wifi_err=1");
}

// ---------- Firmware update (OTA over Update.h) ----------
//
// The upload streams into the *inactive* OTA app partition; otadata is only
// switched by Update.end(true) after the whole image is written and validated.
// A wrong/aborted upload therefore leaves the running firmware untouched — no
// brick risk. On success the user is told to reset the device manually: a
// programmatic reset (esp_restart or a full RTC-WDT chip reset) hangs early in
// boot on this board (ESP32-S3, USB-CDC-on-boot) when no USB host is attached —
// only the hardware/power reset recovers. See docs/adr/0003-ota-manual-restart.md.

bool s_ota_upload_failed = false;   // set on any per-request upload error
bool s_ota_header_checked = false;  // magic byte checked on the first write chunk
size_t s_ota_since_breather = 0;    // bytes written since the last delay(1)

// Give lower-priority tasks (incl. the idle task, which resets the hardware
// task WDT) a slice roughly every 16 KB of flash writes. A bare yield()/
// esp_task_wdt_reset() from the prio-1 loopTask is a no-op for the WDT here, so
// we actually block briefly instead.
constexpr size_t kOtaBreatherBytes = 16u * 1024u;

size_t otaAppPartitionSize() {
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
  return next != nullptr ? static_cast<size_t>(next->size) : 0;
}

void handleUpdateUpload() {
  HTTPUpload& upload = s_server->upload();
  switch (upload.status) {
    case UPLOAD_FILE_START: {
      s_ota_upload_failed = false;
      s_ota_header_checked = false;
      s_ota_since_breather = 0;
      Serial.printf("[ota] upload start: %s\n", upload.filename.c_str());
      // Size is not known up front from a browser multipart upload, so let
      // Update size the target partition itself.
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
        s_ota_upload_failed = true;
      }
      break;
    }
    case UPLOAD_FILE_WRITE: {
      if (s_ota_upload_failed) {
        break;  // already failed: discard the rest
      }
      if (!s_ota_header_checked) {
        s_ota_header_checked = true;
        // Only the magic byte is meaningful here: upload.totalSize is still 0
        // during the first chunk on the ESP32 WebServer, so pass 0 (unknown)
        // and defer the size check to UPLOAD_FILE_END where the total is final.
        if (!services::ota::firmwareHeaderLooksValid(
                upload.buf, upload.currentSize, /*total_size=*/0,
                otaAppPartitionSize())) {
          Serial.println("[ota] rejected: image header looks invalid");
          Update.abort();
          s_ota_upload_failed = true;
          break;
        }
      }
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
        s_ota_upload_failed = true;
        break;
      }
      // A bare yield()/esp_task_wdt_reset() from the prio-1 loopTask does not
      // let the idle task run to pet the hardware WDT, so actually block for a
      // tick every ~16 KB to keep the long flash write from starving it.
      s_ota_since_breather += upload.currentSize;
      if (s_ota_since_breather >= kOtaBreatherBytes) {
        s_ota_since_breather = 0;
        delay(1);
      }
      break;
    }
    case UPLOAD_FILE_END: {
      if (s_ota_upload_failed) {
        break;
      }
      // totalSize is final here — reject an oversized or implausibly small
      // image before flipping otadata. Wrong sizes never reach the header
      // check's size rules mid-stream (totalSize is 0 on the first chunk).
      if (!services::ota::firmwareSizeLooksValid(upload.totalSize,
                                                 otaAppPartitionSize())) {
        Serial.printf("[ota] rejected: size %u out of range\n",
                      static_cast<unsigned>(upload.totalSize));
        Update.abort();
        s_ota_upload_failed = true;
        break;
      }
      if (Update.end(true)) {
        Serial.printf("[ota] update ok: %u bytes\n",
                      static_cast<unsigned>(upload.totalSize));
      } else {
        Update.printError(Serial);
        s_ota_upload_failed = true;
      }
      break;
    }
    case UPLOAD_FILE_ABORTED: {
      Serial.println("[ota] upload aborted");
      Update.abort();
      s_ota_upload_failed = true;
      break;
    }
    default:
      break;
  }
  yield();
}

void handleUpdateDone() {
  if (s_ota_upload_failed || !Update.isFinished() || Update.hasError()) {
    char msg[96];
    snprintf(msg, sizeof(msg), "Update failed (error %u). Device not restarted.",
             static_cast<unsigned>(Update.getError()));
    // Close the connection (as in the success path) so an idle keep-alive
    // socket can't block the next handleClient(). Header must be set before send.
    s_server->sendHeader("Connection", "close");
    s_server->send(500, "text/plain; charset=utf-8", msg);
    s_server->client().stop();
    return;
  }
  // Close the connection after this response so the browser can't hold a
  // keep-alive socket open — an idle keep-alive would block the next
  // handleClient() and stall the poll loop. Header must be set before send().
  // We do NOT reboot programmatically: on this board a soft/WDT reset hangs the
  // USB-CDC boot with no host attached, so the new firmware only starts after a
  // manual reset. See docs/adr/0003-ota-manual-restart.md.
  s_server->sendHeader("Connection", "close");
  s_server->send(200, "text/plain; charset=utf-8",
                 "Update installed. Press the reset button on the device (or "
                 "unplug/replug power) to start the new firmware.");
  s_server->client().stop();
}

void handleNotFound() {
  s_server->sendHeader("Location", "/", true);
  s_server->send(302, "text/plain", "");
}

void handleOtaGithubStatus() {
  const bool force =
      s_server->hasArg("force") && s_server->arg("force") == "1";
  // Never blocks: a forced check is queued on the HTTPS worker and the page polls
  // this URL again while "checking" is true.
  const bool refreshed = services::ota_github::checkLatest(force);
  const bool checking = services::ota_github::checkInProgress();
  const bool have_cache = services::ota_github::latestTag()[0] != '\0';
  char body[380];
  if (checking) {
    snprintf(body, sizeof(body),
             "{\"ok\":true,\"checking\":true,\"refreshed\":false,\"current\":\"%s\","
             "\"latest\":\"%s\",\"available\":%s}",
             services::ota_github::currentVersion(), services::ota_github::latestTag(),
             services::ota_github::updateAvailable() ? "true" : "false");
    s_server->send(200, "application/json; charset=utf-8", body);
    return;
  }
  if (!refreshed && !have_cache) {
    snprintf(body, sizeof(body),
             "{\"ok\":false,\"refreshed\":false,\"error\":\"check failed\","
             "\"current\":\"%s\",\"latest\":\"\",\"available\":false}",
             services::ota_github::currentVersion());
    s_server->send(503, "application/json; charset=utf-8", body);
    return;
  }
  // Cache is usable even if a forced refresh could not run (ADS-B/HTTPS busy).
  snprintf(body, sizeof(body),
           "{\"ok\":true,\"refreshed\":%s,\"current\":\"%s\",\"latest\":\"%s\","
           "\"available\":%s%s}",
           refreshed ? "true" : "false", services::ota_github::currentVersion(),
           services::ota_github::latestTag(),
           services::ota_github::updateAvailable() ? "true" : "false",
           refreshed ? "" : ",\"warning\":\"could not refresh; showing cached result\"");
  s_server->send(200, "application/json; charset=utf-8", body);
}

void handleOtaGithubInstall() {
  if (s_server->method() != HTTP_POST) {
    s_server->send(405, "text/plain", "Method Not Allowed");
    return;
  }
  if (services::ota_github::installState() ==
      services::ota_github::InstallState::Running) {
    s_server->send(409, "text/plain; charset=utf-8", "install already running");
    return;
  }
  if (!services::ota_github::startInstall()) {
    const char* err = services::ota_github::installError();
    s_server->send(500, "text/plain; charset=utf-8",
                   err[0] != '\0' ? err : "failed to start install");
    return;
  }
  // Close so the progress poll is not stuck behind this keep-alive socket.
  // An idle keep-alive blocks the next handleClient(); the browser then reports
  // "TypeError: Failed to fetch" on /ota/github/progress.
  s_server->sendHeader("Connection", "close");
  s_server->send(200, "application/json; charset=utf-8",
                 "{\"ok\":true,\"state\":\"running\"}");
  s_server->client().stop();
}

void handleOtaGithubProgress() {
  const auto st = services::ota_github::installState();
  const char* state = "idle";
  if (st == services::ota_github::InstallState::Running) {
    state = "running";
  } else if (st == services::ota_github::InstallState::Succeeded) {
    state = "succeeded";
  } else if (st == services::ota_github::InstallState::Failed) {
    state = "failed";
  }
  char body[320];
  snprintf(body, sizeof(body),
           "{\"state\":\"%s\",\"percent\":%u,\"bytes\":%u,\"total\":%u,\"error\":\"%s\"}",
           state, static_cast<unsigned>(services::ota_github::installPercent()),
           static_cast<unsigned>(services::ota_github::installBytes()),
           static_cast<unsigned>(services::ota_github::installTotal()),
           services::ota_github::installError());
  s_server->sendHeader("Connection", "close");
  s_server->send(200, "application/json; charset=utf-8", body);
  s_server->client().stop();
}

/** Append a JSON string value with escaping. Returns chars written (excl NUL) or -1. */
int appendJsonString(char* out, size_t out_len, size_t* used, const char* value) {
  if (out == nullptr || used == nullptr || *used + 2 >= out_len) {
    return -1;
  }
  out[(*used)++] = '"';
  if (value != nullptr) {
    for (const char* p = value; *p != '\0'; ++p) {
      if (*used + 7 >= out_len) {
        return -1;
      }
      const unsigned char c = static_cast<unsigned char>(*p);
      if (c == '"' || c == '\\') {
        out[(*used)++] = '\\';
        out[(*used)++] = static_cast<char>(c);
      } else if (c < 0x20) {
        // Skip control chars in settings strings.
        continue;
      } else {
        out[(*used)++] = static_cast<char>(c);
      }
    }
  }
  if (*used + 1 >= out_len) {
    return -1;
  }
  out[(*used)++] = '"';
  return 0;
}

const char* basemapStyleKey(services::basemap::Style style) {
  switch (style) {
    case services::basemap::Style::Light:
      return "light";
    case services::basemap::Style::Voyager:
      return "voyager";
    case services::basemap::Style::Vfr:
      return "vfr";
    default:
      return "dark";
  }
}

const char* distUnitKey(ui::radar::DistanceUnit unit) {
  switch (unit) {
    case ui::radar::DistanceUnit::StatuteMile:
      return "mi";
    case ui::radar::DistanceUnit::NauticalMile:
      return "nm";
    default:
      return "km";
  }
}

void handleSettingsApi() {
  // Polled every 1-3 s while the page is open: everything here is read from RAM.
  // (apikeys::load() used to run here: ~20 NVS reads per poll, String temporaries,
  // a possible NVS write from the monthly reset, and a race with the route worker.)

  char watch_buf[160];
  char watch_type_buf[96];
  char watch_reg_buf[160];
  services::alert::watchCallsignsFormatted(watch_buf, sizeof(watch_buf));
  services::alert::watchTypesFormatted(watch_type_buf, sizeof(watch_type_buf));
  services::alert::watchRegsFormatted(watch_reg_buf, sizeof(watch_reg_buf));

  char fa_budget[16];
  char fa_cost[16];
  char fr_budget[16];
  char fr_cost[16];
  formatUsdMicro(services::apikeys::flightAwareBudgetUsdMicro(), fa_budget, sizeof(fa_budget), 2);
  formatUsdMicro(services::apikeys::flightAwareCostUsdMicro(), fa_cost, sizeof(fa_cost), 4);
  formatUsdMicro(services::apikeys::fr24BudgetUsdMicro(), fr_budget, sizeof(fr_budget), 2);
  formatUsdMicro(services::apikeys::fr24CostUsdMicro(), fr_cost, sizeof(fr_cost), 4);

  const uint16_t night_start = services::offhours::startMinute();
  const uint16_t night_end = services::offhours::endMinute();
  char night_start_s[8];
  char night_end_s[8];
  snprintf(night_start_s, sizeof(night_start_s), "%02u:%02u", night_start / 60, night_start % 60);
  snprintf(night_end_s, sizeof(night_end_s), "%02u:%02u", night_end / 60, night_end % 60);

  const auto bm_style = services::basemap::storedStyle();

  // Compose into a reusable PSRAM/DRAM buffer — avoid WebServer String copy.
  constexpr size_t kApiCap = 6144;
  static char* s_api_buf = nullptr;
  if (s_api_buf == nullptr) {
    s_api_buf = static_cast<char*>(
        heap_caps_malloc(kApiCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_api_buf == nullptr) {
      s_api_buf = static_cast<char*>(malloc(kApiCap));
    }
  }
  if (s_api_buf == nullptr) {
    s_server->send(503, "text/plain", "Out of memory");
    return;
  }

  size_t used = 0;
  const int head_n = snprintf(
      s_api_buf, kApiCap,
      "{"
      "\"rev\":%lu,"
      "\"updated_ms\":%lu,"
      "\"radar_center\":\"%.6f, %.6f\","
      "\"range_mi\":\"%u\","
      "\"dist_unit\":\"%s\","
      "\"min_height\":\"%d\","
      "\"max_height\":\"%d\","
      "\"radar_accent\":\"%u\","
      "\"show_cardinals\":%s,"
      "\"facing_deg\":\"%u\","
      "\"show_sweep\":%s,"
      "\"plane_labels\":%s,"
      "\"use_basemap\":%s,"
      "\"basemap_style\":\"%s\","
      "\"bm_contrast_dark\":\"%u\","
      "\"bm_contrast_light\":\"%u\","
      "\"bm_wash_vfr\":\"%u\","
      "\"bright_pct\":\"%u\","
      "\"detail_timeout\":\"%lu\","
      "\"clock_timeout\":\"%lu\","
      "\"idle_clock\":%s,"
      "\"clock_24h\":%s,"
      "\"date_numeric\":%s,"
      "\"auto_timezone\":%s,"
      "\"night_en\":%s,"
      "\"night_mode\":\"%u\","
      "\"night_start\":\"%s\","
      "\"night_end\":\"%s\","
      "\"ui_beep\":%s,"
#if !FLIGHTSCNR_HAS_HAPTIC
      "\"beep_tone\":\"%c\","
#endif
      "\"alert_mil\":%s,"
      "\"alert_emrg\":%s,"
      "\"alert_hide\":%s,"
      "\"alert_heli\":%s,"
      "\"alert_ga\":%s,"
      "\"alert_appr\":%s,"
      "\"alert_ovh\":%s,"
      "\"type_colors\":%s,",
      static_cast<unsigned long>(settingsStateRev()),
      static_cast<unsigned long>(settingsStateUpdatedMs()),
      services::map_center::latitude(), services::map_center::longitude(),
      static_cast<unsigned>(ui::radar::scaleActiveMiles()),
      distUnitKey(ui::radar::distanceUnit()), services::adsb::altitudeFloorFt(),
      services::adsb::altitudeCeilingFt(),
      static_cast<unsigned>(ui::radar::accentColorIndex()),
      ui::radar::showCompassRose() ? "true" : "false",
      static_cast<unsigned>(ui::radar::facingDeg()),
      ui::displayPrefsSweepLineEnabled() ? "true" : "false",
      ui::displayPrefsHideBlipDetails() ? "false" : "true",
      services::basemap::enabled() ? "true" : "false", basemapStyleKey(bm_style),
      static_cast<unsigned>(services::basemap::contrastPercentDark()),
      static_cast<unsigned>(services::basemap::contrastPercentLight()),
      static_cast<unsigned>(services::basemap::washPercentVfr()),
      static_cast<unsigned>(hardware::displayBrightnessPercent()),
      ui::displayPrefsFlightDetailTimeoutMs() / 1000UL,
      ui::displayPrefsClockWeatherTimeoutMs() / 1000UL,
      ui::displayPrefsAutoIdleClockEnabled() ? "true" : "false",
      services::clock::use24Hour() ? "true" : "false",
      services::clock::useNumericDate() ? "true" : "false",
      services::clock::useAutoTimezone() ? "true" : "false",
      services::offhours::enabled() ? "true" : "false",
      static_cast<unsigned>(services::offhours::mode()), night_start_s, night_end_s,
      hardware::buzzerEnabled() ? "true" : "false",
#if !FLIGHTSCNR_HAS_HAPTIC
      hardware::buzzerToneLetter(),
#endif
      services::alert::militaryAlertEnabled() ? "true" : "false",
      services::alert::emergencyAlertEnabled() ? "true" : "false",
      services::alert::hideNonAlertedEnabled() ? "true" : "false",
      services::alert::helicopterAlertEnabled() ? "true" : "false",
      services::alert::privateAlertEnabled() ? "true" : "false",
      services::alert::alertOnApproachEnabled() ? "true" : "false",
      services::alert::overheadAnyEnabled() ? "true" : "false",
      services::alert::typeColorsEnabled() ? "true" : "false");
  if (head_n <= 0) {
    s_server->send(500, "text/plain", "json error");
    return;
  }
  used = static_cast<size_t>(head_n);

  auto appendKeyString = [&](const char* key, const char* value) -> bool {
    if (used + strlen(key) + 4 >= kApiCap) {
      return false;
    }
    used += static_cast<size_t>(
        snprintf(s_api_buf + used, kApiCap - used, "\"%s\":", key));
    if (appendJsonString(s_api_buf, kApiCap, &used, value) < 0) {
      return false;
    }
    if (used + 1 >= kApiCap) {
      return false;
    }
    s_api_buf[used++] = ',';
    return true;
  };

  char bm_status[192];
  services::basemap::statusText(bm_status, sizeof(bm_status));
  if (!appendKeyString("bm_status", bm_status) ||
      !appendKeyString("alert_watch", watch_buf) ||
      !appendKeyString("alert_watch_type", watch_type_buf) ||
      !appendKeyString("alert_watch_reg", watch_reg_buf)) {
    s_server->send(500, "text/plain", "json overflow");
    return;
  }
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
  {
    const int rain_n = snprintf(s_api_buf + used, kApiCap - used, "\"use_rain\":%s,",
                                services::rain::enabled() ? "true" : "false");
    if (rain_n <= 0 || static_cast<size_t>(rain_n) >= kApiCap - used) {
      s_server->send(500, "text/plain", "json overflow");
      return;
    }
    used += static_cast<size_t>(rain_n);
  }
#endif

#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
  {
    const int n = snprintf(s_api_buf + used, kApiCap - used,
                           "\"overhead_sign\":%s,\"hide_airliners\":%s,\"alert_notbl\":%s,",
                           ui::displayPrefsOverheadSignEnabled() ? "true" : "false",
                           services::alert::airlinersHidden() ? "true" : "false",
                           services::alert::notableAlertEnabled() ? "true" : "false");
    if (n <= 0 || static_cast<size_t>(n) >= kApiCap - used) {
      s_server->send(500, "text/plain", "json overflow");
      return;
    }
    used += static_cast<size_t>(n);
    const int sky_n = snprintf(s_api_buf + used, kApiCap - used,
                               "\"sky_launch\":%s,\"sky_iss\":%s,",
                               services::sky::launchesEnabled() ? "true" : "false",
                               services::sky::issEnabled() ? "true" : "false");
    if (sky_n <= 0 || static_cast<size_t>(sky_n) >= kApiCap - used) {
      s_server->send(500, "text/plain", "json overflow");
      return;
    }
    used += static_cast<size_t>(sky_n);
  }
#endif
  const int tail_n = snprintf(
      s_api_buf + used, kApiCap - used,
      "\"use_airlabs\":%s,"
      "\"use_flightaware\":%s,"
      "\"use_fr24\":%s,"
      "\"use_adsbdb\":%s,"
      "\"airlabs_max_calls\":\"%u\","
      "\"flightaware_max_usd\":\"%s\","
      "\"flightaware_cost_usd\":\"%s\","
      "\"fr24_max_usd\":\"%s\","
      "\"fr24_cost_usd\":\"%s\","
      "\"use_weather\":%s,"
      "\"use_openmeteo\":%s,"
      "\"weather_units\":\"%s\","
      "\"disclaimer_remembered\":%s"
      "}",
      services::apikeys::useAirLabs() ? "true" : "false",
      services::apikeys::useFlightAware() ? "true" : "false",
      services::apikeys::useFr24() ? "true" : "false",
      services::apikeys::useAdsbDb() ? "true" : "false",
      static_cast<unsigned>(services::apikeys::airLabsMaxCalls()), fa_budget, fa_cost,
      fr_budget, fr_cost, services::apikeys::useWeather() ? "true" : "false",
      services::apikeys::useOpenMeteo() ? "true" : "false",
      services::weather::useImperial() ? "imperial" : "metric",
      services::disclaimer::isRemembered() ? "true" : "false");
  if (tail_n <= 0 || static_cast<size_t>(tail_n) >= kApiCap - used) {
    s_server->send(500, "text/plain", "json overflow");
    return;
  }
  used += static_cast<size_t>(tail_n);

  s_server->setContentLength(used);
  s_server->send(200, "application/json; charset=utf-8", "");
  s_server->sendContent(s_api_buf, used);
}

void registerRoutes() {
  s_server->on("/", HTTP_GET, handleSettingsPage);
  s_server->on("/settings", HTTP_GET, handleSettingsPage);
  s_server->on("/api/settings", HTTP_GET, handleSettingsApi);
  s_server->on("/save", HTTP_POST, handleSave);
  s_server->on("/route_cache.csv", HTTP_GET, handleRouteCacheDownload);
  s_server->on("/route_cache/clear", HTTP_POST, handleRouteCacheClear);
  s_server->on("/disclaimer/clear", HTTP_POST, handleDisclaimerClear);
  s_server->on("/restart", HTTP_POST, handleRestart);
  s_server->on("/basemap/upload", HTTP_POST, handleBasemapUploadDone, handleBasemapUpload);
  s_server->on("/basemap/clear", HTTP_POST, handleBasemapClear);
  s_server->on("/basemap/adjust", HTTP_POST, handleBasemapAdjust);
  s_server->on("/wifi/add", HTTP_POST, handleWifiAdd);
  s_server->on("/wifi/remove", HTTP_POST, handleWifiRemove);
  s_server->on("/wifi/up", HTTP_POST, handleWifiUp);
  s_server->on("/wifi/down", HTTP_POST, handleWifiDown);
  s_server->on("/wifi/pass", HTTP_POST, handleWifiPass);
  s_server->on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  s_server->on("/ota/github/status", HTTP_GET, handleOtaGithubStatus);
  s_server->on("/ota/github/install", HTTP_POST, handleOtaGithubInstall);
  s_server->on("/ota/github/progress", HTTP_GET, handleOtaGithubProgress);
  s_server->onNotFound(handleNotFound);
}

}  // namespace

void settingsWebStart() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  if (s_active && s_server != nullptr) {
    return;
  }

  // The only NVS read of the keys for the web page: the handlers serve from RAM
  // afterwards (every save path updates RAM as well as NVS).
  services::apikeys::load();

  settingsWebStop();

  s_server = new WebServer(80);
  // Without this, handleClient() sleeps 1 ms on every idle loop pass.
  s_server->enableDelay(false);
  registerRoutes();
  s_server->begin();
  s_active = true;

  WiFi.setHostname(services::device_identity::portalHostname());

#ifdef WM_MDNS
  MDNS.end();
  if (MDNS.begin(services::device_identity::portalHostname())) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("Settings web: http://%s.local/  http://%s/\n",
                  services::device_identity::portalHostname(),
                  WiFi.localIP().toString().c_str());
  } else {
    Serial.printf("Settings web: http://%s/  (mDNS unavailable)\n",
                  WiFi.localIP().toString().c_str());
  }
#else
  Serial.printf("Settings web: http://%s/\n", WiFi.localIP().toString().c_str());
#endif
}

void settingsWebStop() {
  if (s_server != nullptr) {
    s_server->stop();
    delete s_server;
    s_server = nullptr;
  }
  if (s_settings_page != nullptr) {
    heap_caps_free(s_settings_page);
    s_settings_page = nullptr;
  }
  s_active = false;
#ifdef WM_MDNS
  MDNS.end();
#endif
}

void settingsWebPoll() {
  if (!s_active || s_server == nullptr) {
    return;
  }
  s_server->handleClient();
}

bool settingsWebActive() { return s_active; }
