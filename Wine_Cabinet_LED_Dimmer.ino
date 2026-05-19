#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <ArduinoOTA.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <time.h>
#include "version.h"

namespace Config {
constexpr uint8_t kMosfetPins[] = {16, 17, 26, 27};
constexpr size_t kMosfetCount = sizeof(kMosfetPins) / sizeof(kMosfetPins[0]);

constexpr uint8_t kStatusLedPin = 23;
constexpr bool kStatusLedActiveHigh = true;

// Update these three inputs if your final wiring uses different GPIOs.
constexpr uint8_t kDoorSensor1Pin = 18;
constexpr uint8_t kDoorSensor2Pin = 19;
constexpr uint8_t kOverrideButtonPin = 21;

constexpr uint32_t kPwmFrequencyHz = 5000;
constexpr uint8_t kPwmResolutionBits = 12;
constexpr uint16_t kOnBrightness = 3000;
constexpr unsigned long kFadeDurationMs = 800;

constexpr char kDeviceName[] = "wine-cabinet-dimmer";
constexpr char kWiFiPortalName[] = "WineCabinetDimmer";
constexpr char kOtaPassword[] = "";

constexpr unsigned long kButtonDebounceMs = 50;
constexpr unsigned long kPortalTimeoutSeconds = 180;

constexpr char kNtpServer[] = "pool.ntp.org";
constexpr long  kGmtOffsetSec = 3600;      // UTC+1 (CET) — adjust for your timezone
constexpr int   kDaylightOffsetSec = 3600; // +1 hour DST
}  // namespace Config

enum class ControlMode : uint8_t {
  kAuto,
  kForceOn,
  kForceOff,
};

enum class StatusLedMode : uint8_t {
  kBooting,
  kPortal,
  kConnected,
  kOtaActive,
  kError,
};

WebServer webServer(80);
WiFiManager wifiManager;
Preferences prefs;

char g_hostname[32] = "wine-cabinet";

ControlMode g_controlMode = ControlMode::kAuto;
StatusLedMode g_statusLedMode = StatusLedMode::kBooting;

bool g_wifiConnected = false;
bool g_portalActive = false;
bool g_pendingRestart = false;
bool g_lightsOn = false;
bool g_otaError = false;
bool g_otaServicesStarted = false;

bool g_lastRawButtonPressed = false;
bool g_stableButtonPressed = false;
unsigned long g_lastButtonChangeMs = 0;

unsigned long g_lastLedToggleMs = 0;
bool g_statusLedOutput = false;
unsigned long g_lastReconnectAttemptMs = 0;
uint16_t g_currentBrightness = 0;
uint16_t g_fadeStartBrightness = 0;
uint16_t g_targetBrightness = 0;
unsigned long g_fadeStartMs = 0;
bool g_fadeActive = false;

// Scheduler: lights are allowed only between onHour:onMin and offHour:offMin.
// If schedEnabled is false the schedule is ignored entirely.
struct TimeOfDay {
  uint8_t hour;
  uint8_t minute;
};
bool      g_schedEnabled = false;
TimeOfDay g_schedOn  = {8,  0};
TimeOfDay g_schedOff = {23, 0};
// Bitmask: bit 0 = Monday … bit 6 = Sunday. Default = all days.
uint8_t   g_schedDays = 0x7F;
bool      g_ntpSynced = false;

unsigned long g_ledBlinkUntilMs = 0;  // debug blink: LED on until this timestamp
bool g_lastDoor1 = false;
bool g_lastDoor2 = false;

// Auto-return: when both doors have been open for g_bothDoorsTimeoutMs, revert to Auto.
unsigned long g_bothDoorsTimeoutMs = 2UL * 60UL * 1000UL;
unsigned long g_bothDoorsOpenSinceMs = 0;   // millis() when both doors first opened
bool          g_bothDoorsWereOpen    = false;

bool readActiveLowInput(uint8_t pin) {
  return digitalRead(pin) == LOW;
}

void triggerDebugBlink() {
  g_ledBlinkUntilMs = millis() + 500;
}

bool isAnyDoorOpen() {
  return readActiveLowInput(Config::kDoorSensor1Pin) == false ||
         readActiveLowInput(Config::kDoorSensor2Pin) == false;
}

bool isDoor1Open() {
  return readActiveLowInput(Config::kDoorSensor1Pin) == false;
}

bool isDoor2Open() {
  return readActiveLowInput(Config::kDoorSensor2Pin) == false;
}

// Returns true if the current local time falls within the allowed window.
// Handles overnight ranges (e.g. 22:00 – 06:00) correctly.
bool isWithinSchedule() {
  if (!g_ntpSynced) {
    return true; // no time yet — don't block lights
  }
  struct tm t;
  if (!getLocalTime(&t)) {
    return true;
  }
  // tm_wday: 0=Sun,1=Mon…6=Sat — map to bit 0=Mon…bit 6=Sun
  const uint8_t wday = (t.tm_wday == 0) ? 6 : (t.tm_wday - 1);
  if (!(g_schedDays & (1 << wday))) {
    return false; // today not in active days
  }
  const uint16_t now  = t.tm_hour * 60 + t.tm_min;
  const uint16_t on   = g_schedOn.hour  * 60 + g_schedOn.minute;
  const uint16_t off  = g_schedOff.hour * 60 + g_schedOff.minute;
  if (on <= off) {
    return now >= on && now < off;   // normal window: 08:00 – 23:00
  } else {
    return now >= on || now < off;   // overnight window: 22:00 – 06:00
  }
}

bool shouldLightsBeOn() {
  if (g_controlMode == ControlMode::kForceOn)  return true;
  if (g_controlMode == ControlMode::kForceOff) return false;
  if (g_schedEnabled && !isWithinSchedule())   return false;
  return isAnyDoorOpen();
}

void writeStatusLed(bool on) {
  g_statusLedOutput = on;
  const uint8_t level =
      (Config::kStatusLedActiveHigh == on) ? HIGH : LOW;
  digitalWrite(Config::kStatusLedPin, level);
}

void setStatusLedMode(StatusLedMode mode) {
  if (g_statusLedMode == mode) {
    return;
  }

  g_statusLedMode = mode;
  g_lastLedToggleMs = 0;
}

void updateStatusLed() {
  const unsigned long now = millis();

  switch (g_statusLedMode) {
    case StatusLedMode::kConnected:
      writeStatusLed(now < g_ledBlinkUntilMs);
      break;

    case StatusLedMode::kBooting:
      if (now - g_lastLedToggleMs >= 150) {
        g_lastLedToggleMs = now;
        writeStatusLed(!g_statusLedOutput);
      }
      break;

    case StatusLedMode::kPortal:
      if (now - g_lastLedToggleMs >= 500) {
        g_lastLedToggleMs = now;
        writeStatusLed(!g_statusLedOutput);
      }
      break;

    case StatusLedMode::kOtaActive:
      if (now - g_lastLedToggleMs >= 80) {
        g_lastLedToggleMs = now;
        writeStatusLed(!g_statusLedOutput);
      }
      break;

    case StatusLedMode::kError:
      if (now - g_lastLedToggleMs >= 120) {
        g_lastLedToggleMs = now;
        writeStatusLed(!g_statusLedOutput);
      }
      break;
  }
}

void configurePwmOutput(uint8_t pin, uint8_t channel) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  ledcAttachChannel(pin, Config::kPwmFrequencyHz, Config::kPwmResolutionBits,
                    channel);
  ledcWriteChannel(channel, 0);
#else
  ledcSetup(channel, Config::kPwmFrequencyHz, Config::kPwmResolutionBits);
  ledcAttachPin(pin, channel);
  ledcWrite(channel, 0);
#endif
}

void writePwmOutput(uint8_t pin, uint8_t channel, uint32_t duty) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  (void)pin;
  ledcWriteChannel(channel, duty);
#else
  (void)pin;
  ledcWrite(channel, duty);
#endif
}

void writeAllMosfets(uint16_t duty) {
  for (size_t i = 0; i < Config::kMosfetCount; ++i) {
    writePwmOutput(Config::kMosfetPins[i], static_cast<uint8_t>(i), duty);
  }
}

void setBrightnessImmediate(uint16_t duty) {
  g_currentBrightness = duty;
  writeAllMosfets(duty);
}

void startFadeToBrightness(uint16_t duty) {
  if (g_targetBrightness == duty && (g_fadeActive || g_currentBrightness == duty)) {
    return;
  }

  g_fadeStartBrightness = g_currentBrightness;
  g_targetBrightness = duty;
  g_fadeStartMs = millis();
  g_fadeActive = (g_fadeStartBrightness != g_targetBrightness);

  if (!g_fadeActive) {
    setBrightnessImmediate(duty);
  }
}

void applyLightingState(bool lightsOn) {
  if (g_lightsOn == lightsOn) {
    return;
  }

  g_lightsOn = lightsOn;
  startFadeToBrightness(lightsOn ? Config::kOnBrightness : 0);
}

void updateFade() {
  if (!g_fadeActive) {
    return;
  }

  const unsigned long now = millis();
  const unsigned long elapsed = now - g_fadeStartMs;

  if (elapsed >= Config::kFadeDurationMs) {
    setBrightnessImmediate(g_targetBrightness);
    g_fadeActive = false;
    return;
  }

  const int32_t delta =
      static_cast<int32_t>(g_targetBrightness) -
      static_cast<int32_t>(g_fadeStartBrightness);
  const int32_t nextBrightness =
      static_cast<int32_t>(g_fadeStartBrightness) +
      ((delta * static_cast<int32_t>(elapsed)) /
       static_cast<int32_t>(Config::kFadeDurationMs));

  if (nextBrightness != g_currentBrightness) {
    setBrightnessImmediate(static_cast<uint16_t>(nextBrightness));
  }
}

void handleDoorInputs() {
  const bool d1 = isDoor1Open();
  const bool d2 = isDoor2Open();
  if (d1 != g_lastDoor1 || d2 != g_lastDoor2) {
    g_lastDoor1 = d1;
    g_lastDoor2 = d2;
    triggerDebugBlink();
  }
}

void handleBothDoorsOpenTimeout() {
  const bool bothOpen = isDoor1Open() && isDoor2Open();
  const unsigned long now = millis();

  if (bothOpen) {
    if (!g_bothDoorsWereOpen) {
      g_bothDoorsWereOpen = true;
      g_bothDoorsOpenSinceMs = now;
    } else if (g_controlMode != ControlMode::kAuto &&
               now - g_bothDoorsOpenSinceMs >= g_bothDoorsTimeoutMs) {
      g_controlMode = ControlMode::kAuto;
      triggerDebugBlink();
    }
  } else {
    g_bothDoorsWereOpen = false;
  }
}

void handleOverrideButton() {
  const unsigned long now = millis();
  const bool rawPressed = readActiveLowInput(Config::kOverrideButtonPin);

  if (rawPressed != g_lastRawButtonPressed) {
    g_lastRawButtonPressed = rawPressed;
    g_lastButtonChangeMs = now;
  }

  if (now - g_lastButtonChangeMs < Config::kButtonDebounceMs) {
    return;
  }

  if (rawPressed == g_stableButtonPressed) {
    return;
  }

  g_stableButtonPressed = rawPressed;

  if (!g_stableButtonPressed) {
    return;
  }

  switch (g_controlMode) {
    case ControlMode::kAuto:     g_controlMode = ControlMode::kForceOn;  break;
    case ControlMode::kForceOn:  g_controlMode = ControlMode::kForceOff; break;
    case ControlMode::kForceOff: g_controlMode = ControlMode::kAuto;     break;
  }
  g_bothDoorsOpenSinceMs = millis();
  triggerDebugBlink();
}

// CSS served as a separate endpoint to keep HTML clean.
// Edit wine_cabinet.css locally, then paste updated content here.
static const char kPageCss[] PROGMEM = R"CSS(
:root{--bg:#f5efe6;--card:#fffdf9;--ink:#2f241f;--ink-muted:#7a6358;
--accent:#8e4b2f;--accent-hover:#7a3d25;--line:#dfd1c4;
--success-bg:#eaf3ec;--success-ink:#4a7c59;--danger-bg:#fdecea;
--danger-ink:#c0392b;--warn-bg:#fff4e5;--warn-ink:#a05a00;
--info-bg:#eef1fb;--info-ink:#3a4fa0;}
*,*::before,*::after{box-sizing:border-box;}
body{margin:0;font-family:Georgia,'Times New Roman',serif;
background:linear-gradient(160deg,#f7f0e8 0%,#ece1d2 100%);
color:var(--ink);min-height:100vh;}
.wrap{max-width:660px;margin:0 auto;padding:44px 20px 64px;}
.header{text-align:center;margin-bottom:32px;}
.header .icon{font-size:2.8rem;line-height:1;margin-bottom:10px;}
.header h1{margin:0 0 4px;font-size:2rem;letter-spacing:-.02em;}
.header .subtitle{margin:0;font-size:.95rem;color:var(--ink-muted);font-style:italic;}
.header .byline{margin:6px 0 0;font-size:.72rem;letter-spacing:.08em;text-transform:uppercase;
color:var(--ink-muted);font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;opacity:.7;}
.card{background:var(--card);border:1px solid var(--line);border-radius:20px;
padding:26px 26px 22px;
box-shadow:0 18px 52px rgba(0,0,0,.08),0 2px 8px rgba(0,0,0,.04);margin-bottom:18px;}
.card-title{margin:0 0 16px;font-size:.72rem;font-weight:700;letter-spacing:.1em;
text-transform:uppercase;color:var(--ink-muted);
font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;}
.status-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px;}
.status-item{background:var(--bg);border:1px solid var(--line);border-radius:12px;padding:12px 14px;}
.status-label{font-size:.68rem;letter-spacing:.09em;text-transform:uppercase;
color:var(--ink-muted);font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;margin-bottom:4px;}
.status-value{font-size:.92rem;font-weight:bold;}
.badge{display:inline-block;padding:2px 10px;border-radius:999px;font-size:.75rem;
font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;font-weight:700;letter-spacing:.04em;}
.badge-open{background:var(--danger-bg);color:var(--danger-ink);}
.badge-closed{background:var(--success-bg);color:var(--success-ink);}
.badge-auto{background:var(--info-bg);color:var(--info-ink);}
.badge-force{background:var(--warn-bg);color:var(--warn-ink);}
.badge-off{background:#f0f0f0;color:#555;}
.update-banner{display:none;background:#1a1a2e;color:#e8e8f0;border-radius:16px;
padding:18px 22px;margin-bottom:18px;
font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;}
.update-banner .update-title{font-size:.95rem;font-weight:700;margin-bottom:4px;}
.update-banner .update-notes{font-size:.82rem;opacity:.75;margin-bottom:14px;}
.update-banner .btn-update{background:#4f8ef7;color:#fff;border:0;border-radius:999px;
padding:11px 24px;font-size:.9rem;font-weight:600;cursor:pointer;width:100%;
font-family:inherit;transition:background .15s;}
.update-banner .btn-update:hover{background:#3a7de0;}
.update-banner .btn-update:disabled{opacity:.5;cursor:not-allowed;}
.upload-area{border:2px dashed var(--line);border-radius:14px;padding:28px 20px;
text-align:center;cursor:pointer;transition:border-color .2s,background .2s;
margin-bottom:16px;background:var(--bg);}
.upload-area:hover,.upload-area.dragover{border-color:var(--accent);background:#fdf5ef;}
.upload-area input[type=file]{display:none;}
.upload-icon{font-size:2rem;margin-bottom:8px;}
.upload-hint{font-size:.85rem;color:var(--ink-muted);
font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;}
.upload-filename{margin-top:8px;font-size:.85rem;color:var(--accent);
font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;font-weight:600;}
.progress-wrap{display:none;margin-bottom:16px;}
.progress-track{height:8px;background:var(--line);border-radius:999px;overflow:hidden;}
.progress-bar{height:100%;width:0%;background:linear-gradient(90deg,var(--accent),#c47348);
border-radius:999px;transition:width .3s ease;}
.progress-pct{font-size:.75rem;color:var(--ink-muted);
font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;margin-top:5px;text-align:right;}
.btn{display:flex;align-items:center;justify-content:center;gap:8px;width:100%;
background:var(--accent);color:#fff;border:0;border-radius:999px;padding:14px 28px;
font-size:1rem;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;
font-weight:600;cursor:pointer;transition:background .15s,transform .1s;letter-spacing:.02em;}
.btn:hover{background:var(--accent-hover);}
.btn:active{transform:scale(.98);}
.btn:disabled{opacity:.45;cursor:not-allowed;transform:none;}
#msg,#sched-msg,#wifi-msg,#timeout-msg{display:none;margin-top:14px;padding:11px 15px;
border-radius:10px;font-size:.88rem;
font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;}
#msg.info,#sched-msg.info,#wifi-msg.info,#timeout-msg.info{display:block;background:var(--info-bg);color:var(--info-ink);}
#msg.success,#sched-msg.success,#wifi-msg.success,#timeout-msg.success{display:block;background:var(--success-bg);color:var(--success-ink);}
#msg.error,#sched-msg.error,#wifi-msg.error,#timeout-msg.error{display:block;background:var(--danger-bg);color:var(--danger-ink);}
.footer{text-align:center;font-size:.75rem;color:var(--ink-muted);
font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;margin-top:28px;}
.sched-row{display:flex;align-items:center;gap:12px;flex-wrap:wrap;margin-bottom:14px;}
.sched-row label{font-size:.82rem;color:var(--ink-muted);
font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;min-width:36px;}
input[type=time]{border:1px solid var(--line);border-radius:8px;padding:8px 10px;
font-size:.95rem;background:var(--bg);color:var(--ink);font-family:inherit;}
input[type=time]:focus{outline:2px solid var(--accent);border-color:transparent;}
.toggle-row{display:flex;align-items:center;gap:10px;margin-bottom:18px;}
.toggle-row span{font-size:.88rem;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;}
.toggle{position:relative;display:inline-block;width:44px;height:24px;}
.toggle input{opacity:0;width:0;height:0;}
.slider{position:absolute;inset:0;background:var(--line);border-radius:999px;
transition:background .2s;cursor:pointer;}
.slider::before{content:'';position:absolute;width:18px;height:18px;left:3px;bottom:3px;
background:#fff;border-radius:50%;transition:transform .2s;}
.toggle input:checked+.slider{background:var(--accent);}
.toggle input:checked+.slider::before{transform:translateX(20px);}
.btn-ghost{background:transparent;border:1px solid var(--line);color:var(--ink-muted);}
.btn-ghost:hover{background:var(--bg);border-color:var(--accent);color:var(--accent);}
.btn-danger{background:#c0392b;}
.btn-danger:hover{background:#a93226;}
.wifi-grid{display:flex;flex-direction:column;gap:0;margin-bottom:18px;
border:1px solid var(--line);border-radius:12px;overflow:hidden;}
.wifi-row{display:flex;justify-content:space-between;align-items:center;
padding:10px 14px;border-bottom:1px solid var(--line);}
.wifi-row:last-child{border-bottom:none;}
.wifi-label{font-size:.75rem;letter-spacing:.06em;text-transform:uppercase;
color:var(--ink-muted);font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;}
.wifi-value{font-size:.9rem;font-weight:600;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;}
.danger-zone{border:1px solid #f5c6c2;border-radius:12px;overflow:hidden;}
.danger-zone summary{padding:11px 14px;font-size:.85rem;font-weight:600;cursor:pointer;
color:#c0392b;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;
list-style:none;display:flex;align-items:center;gap:6px;user-select:none;}
.danger-zone summary::after{content:'›';margin-left:auto;font-size:1.1rem;
transition:transform .2s;}
.danger-zone[open] summary::after{transform:rotate(90deg);}
.danger-zone summary::-webkit-details-marker{display:none;}
.danger-body{padding:14px;background:#fff8f8;border-top:1px solid #f5c6c2;}
.danger-desc{margin:0 0 14px;font-size:.82rem;color:#a93226;
font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;}
.day-row{display:flex;gap:6px;margin-bottom:16px;flex-wrap:wrap;}
.day-btn{position:relative;cursor:pointer;}
.day-btn input{position:absolute;opacity:0;width:0;height:0;}
.day-btn span{display:flex;align-items:center;justify-content:center;
width:38px;height:38px;border-radius:50%;border:1px solid var(--line);
background:var(--bg);font-size:.78rem;font-weight:700;
font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;
color:var(--ink-muted);transition:background .15s,color .15s,border-color .15s;
user-select:none;}
.day-btn input:checked+span{background:var(--accent);color:#fff;border-color:var(--accent);}
.day-btn:hover span{border-color:var(--accent);color:var(--accent);}
.day-btn input:checked+span:hover{background:var(--accent-hover);}
)CSS";

// Returns "HH:MM" string for an input[type=time] value field.
String fmtTime(uint8_t h, uint8_t m) {
  char buf[6];
  snprintf(buf, sizeof(buf), "%02u:%02u", h, m);
  return String(buf);
}

String buildWebOtaPage() {
  const bool door1 = isDoor1Open();
  const bool door2 = isDoor2Open();

  // Current local time for display
  char timeBuf[9] = "–";
  if (g_ntpSynced) {
    struct tm t;
    if (getLocalTime(&t)) {
      snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d",
               t.tm_hour, t.tm_min, t.tm_sec);
    }
  }

  String html;
  html.reserve(3200);
  html += F("<!DOCTYPE html><html lang='en'><head>"
            "<meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>Wine Cabinet</title>"
            "<link rel='stylesheet' href='/style.css'>"
            "</head><body><div class='wrap'>"
            "<div class='update-banner' id='update-banner'>"
            "<div class='update-title'>&#x2B06;&nbsp;Update available: <span id='update-version'></span></div>"
            "<div class='update-notes' id='update-notes'></div>"
            "<button class='btn-update' id='update-btn' onclick='installUpdate()'>Install Now</button>"
            "<div id='update-msg' style='margin-top:10px;font-size:.82rem;'></div>"
            "</div>"
            "<div class='header'>"
            "<div class='icon'>🍷</div>"
            "<h1>Wine Cabinet</h1>"
            "<p class='subtitle'>Firmware update &amp; status</p>"
            "<p class='byline'>Created by Hjort3Design</p>"
            "</div>"

            "<div class='card'>"
            "<div class='card-title'>Status</div>"
            "<div class='status-grid'>"

            "<div class='status-item'>"
            "<div class='status-label'>Door 1</div>"
            "<div class='status-value' id='val-door1'>");
  html += door1
      ? F("<span class='badge badge-open'>OPEN</span>")
      : F("<span class='badge badge-closed'>CLOSED</span>");
  html += F("</div></div>"

            "<div class='status-item'>"
            "<div class='status-label'>Door 2</div>"
            "<div class='status-value' id='val-door2'>");
  html += door2
      ? F("<span class='badge badge-open'>OPEN</span>")
      : F("<span class='badge badge-closed'>CLOSED</span>");
  html += F("</div></div>"

            "<div class='status-item'>"
            "<div class='status-label'>Control</div>"
            "<div class='status-value' id='val-mode'>");
  if      (g_controlMode == ControlMode::kForceOn)  html += F("<span class='badge badge-force'>FORCE ON</span>");
  else if (g_controlMode == ControlMode::kForceOff) html += F("<span class='badge badge-off'>FORCE OFF</span>");
  else                                              html += F("<span class='badge badge-auto'>AUTO</span>");
  html += F("</div></div>"

            "<div class='status-item'>"
            "<div class='status-label'>Time</div>"
            "<div class='status-value' id='val-time'>");
  html += timeBuf;
  html += F("</div></div>"

            "</div>"  // .status-grid
            "<div style='margin-top:16px;'>"
            "<button class='btn' type='button' onclick='cycleMode()' id='mode-btn'>"
            "&#x1F4A1;&nbsp;Cycle Mode"
            "</button>"
            "</div>"
            "</div>"  // .card

            // ── Scheduler card ──────────────────────────────
            "<div class='card'>"
            "<div class='card-title'>Schedule</div>"
            "<form id='sched-form'>"
            "<div class='toggle-row'>"
            "<label class='toggle'>"
            "<input type='checkbox' id='sched-en'");
  if (g_schedEnabled) html += F(" checked");
  html += F(">"
            "<span class='slider'></span>"
            "</label>"
            "<span>Enable schedule</span>"
            "</div>"

            // Day-of-week picker
            "<div class='day-row'>");
  // bit 0=Mon…bit 6=Sun, labels Mon–Sun
  const char* dayLabels[] = {"Mo","Tu","We","Th","Fr","Sa","Su"};
  for (uint8_t i = 0; i < 7; i++) {
    html += F("<label class='day-btn'>"
              "<input type='checkbox' name='day' value='");
    html += String(i);
    html += F("'");
    if (g_schedDays & (1 << i)) html += F(" checked");
    html += F("><span>");
    html += dayLabels[i];
    html += F("</span></label>");
  }
  html += F("</div>"

            "<div class='sched-row'>"
            "<label>On</label>"
            "<input type='time' id='sched-on' value='");
  html += fmtTime(g_schedOn.hour, g_schedOn.minute);
  html += F("'>"
            "<label>Off</label>"
            "<input type='time' id='sched-off' value='");
  html += fmtTime(g_schedOff.hour, g_schedOff.minute);
  html += F("'>"
            "</div>"
            "<button class='btn' type='button' onclick='saveSched()'>Save Schedule</button>"
            "</form>"
            "<div id='sched-msg'></div>"
            "</div>"

            // ── Auto-return timeout card ────────────────────
            "<div class='card'>"
            "<div class='card-title'>Auto-Return Timeout</div>"
            "<p style='margin:0 0 14px;font-size:.85rem;color:var(--ink-muted);"
            "font-family:-apple-system,BlinkMacSystemFont,\"Segoe UI\",sans-serif;'>"
            "When both doors stay open this long, the mode resets to Auto.</p>"
            "<div class='sched-row'>"
            "<label>Minutes</label>"
            "<input type='number' id='timeout-min' min='1' max='60' value='");
  html += String(g_bothDoorsTimeoutMs / 60000UL);
  html += F("' style='width:80px;border:1px solid var(--line);border-radius:8px;"
            "padding:8px 10px;font-size:.95rem;background:var(--bg);color:var(--ink);'>"
            "</div>"
            "<button class='btn' type='button' onclick='saveTimeout()'>Save Timeout</button>"
            "<div id='timeout-msg'></div>"
            "</div>"

            // ── Wi-Fi card ──────────────────────────────────
            "<div class='card'>"
            "<div class='card-title'>Wi-Fi</div>"
            "<div class='wifi-grid'>"

            "<div class='wifi-row'>"
            "<span class='wifi-label'>SSID</span>"
            "<span class='wifi-value'>");
  html += WiFi.isConnected() ? WiFi.SSID() : "—";
  html += F("</span></div>"

            "<div class='wifi-row'>"
            "<span class='wifi-label'>IP Address</span>"
            "<span class='wifi-value'>");
  html += WiFi.isConnected() ? WiFi.localIP().toString() : "—";
  html += F("</span></div>"

            "<div class='wifi-row'>"
            "<span class='wifi-label'>MAC Address</span>"
            "<span class='wifi-value'>");
  html += WiFi.macAddress();
  html += F("</span></div>"

            "<div class='wifi-row'>"
            "<span class='wifi-label'>Signal</span>"
            "<span class='wifi-value' id='val-rssi'>");
  if (WiFi.isConnected()) {
    int rssi = WiFi.RSSI();
    html += String(rssi);
    html += F(" dBm");
  } else {
    html += F("—");
  }
  html += F("</span></div>"
            "</div>"  // .wifi-grid

            "<div class='sched-row' style='margin-top:16px;'>"
            "<label style='min-width:110px;'>Device Name</label>"
            "<input type='text' id='hostname-input' maxlength='31' value='");
  html += String(g_hostname);
  html += F("' oninput=\"document.getElementById('hostname-preview').textContent="
            "this.value.trim()?this.value.trim()+'.local':'';\" "
            "style='flex:1;border:1px solid var(--line);border-radius:8px;"
            "padding:8px 10px;font-size:.95rem;background:var(--bg);color:var(--ink);'>"
            "</div>"
            "<p style='font-size:.8rem;color:var(--ink-muted);margin:4px 0 10px;'>"
            "Your device will be reachable at&nbsp;"
            "<strong id='hostname-preview'>");
  html += String(g_hostname);
  html += F(".local</strong>"
            "</p>"
            "<button class='btn' type='button' onclick='saveHostname()'>Save Device Name</button>"
            "<div id='wifi-msg'></div>"

            "<details class='danger-zone' style='margin-top:16px;'>"
            "<summary>Advanced</summary>"
            "<div class='danger-body'>"
            "<p class='danger-desc'>Erases saved Wi-Fi credentials and reboots into setup mode.</p>"
            "<button class='btn btn-danger' type='button' onclick='resetWifi()'>"
            "&#x26A0;&nbsp;Reset Wi-Fi &amp; Reboot"
            "</button>"
            "</div>"
            "</details>"
            "</div>"

            "<div class='card'>"
            "<div class='card-title'>Upload Firmware</div>"
            "<form id='ota' method='POST' action='/update' enctype='multipart/form-data'>"
            "<div class='upload-area' id='drop' onclick=\"document.getElementById('fw').click()\">"
            "<div class='upload-icon'>📦</div>"
            "<div class='upload-hint'>Click to choose a .bin file</div>"
            "<div class='upload-filename' id='fname'></div>"
            "<input type='file' id='fw' name='update' accept='.bin' required>"
            "</div>"
            "<div class='progress-wrap' id='pwrap'>"
            "<div class='progress-track'><div class='progress-bar' id='pbar'></div></div>"
            "<div class='progress-pct' id='ppct'>0%</div>"
            "</div>"
            "<button class='btn' type='submit' id='sbtn' disabled>"
            "&#8593;&nbsp;Upload Firmware"
            "</button>"
            "</form>"
            "<div id='msg'></div>"
            "</div>"  // .card

            "<div class='footer'>Firmware version: " FIRMWARE_VERSION "</div>"

            "</div>"  // .wrap

            "<script>"
            "var fw=document.getElementById('fw'),"
            "drop=document.getElementById('drop'),"
            "fname=document.getElementById('fname'),"
            "sbtn=document.getElementById('sbtn'),"
            "pwrap=document.getElementById('pwrap'),"
            "pbar=document.getElementById('pbar'),"
            "ppct=document.getElementById('ppct'),"
            "msg=document.getElementById('msg'),"
            "form=document.getElementById('ota');"
            "fw.onchange=function(){"
            "if(fw.files[0]){fname.textContent=fw.files[0].name;sbtn.disabled=false;}"
            "};"
            "['dragover','dragleave','drop'].forEach(function(e){"
            "drop.addEventListener(e,function(ev){ev.preventDefault();});"
            "});"
            "drop.addEventListener('dragover',function(){drop.classList.add('dragover');});"
            "drop.addEventListener('dragleave',function(){drop.classList.remove('dragover');});"
            "drop.addEventListener('drop',function(ev){"
            "drop.classList.remove('dragover');"
            "var f=ev.dataTransfer.files[0];"
            "if(f&&f.name.endsWith('.bin')){"
            "var dt=new DataTransfer();dt.items.add(f);fw.files=dt.files;"
            "fname.textContent=f.name;sbtn.disabled=false;"
            "}"
            "});"
            "form.onsubmit=function(e){"
            "e.preventDefault();"
            "var fd=new FormData(form);"
            "var xhr=new XMLHttpRequest();"
            "xhr.open('POST','/update');"
            "sbtn.disabled=true;"
            "pwrap.style.display='block';"
            "msg.className='info';msg.textContent='Uploading...';"
            "xhr.upload.onprogress=function(ev){"
            "if(ev.lengthComputable){"
            "var p=Math.round(ev.loaded*100/ev.total);"
            "pbar.style.width=p+'%';ppct.textContent=p+'%';"
            "}"
            "};"
            "xhr.onload=function(){"
            "if(xhr.status===200){"
            "msg.className='success';"
            "msg.textContent='Update complete — rebooting...';"
            "}else{"
            "msg.className='error';"
            "msg.textContent='Upload failed ('+xhr.status+').';"
            "sbtn.disabled=false;"
            "}"
            "};"
            "xhr.onerror=function(){"
            "msg.className='error';msg.textContent='Network error.';"
            "sbtn.disabled=false;"
            "};"
            "xhr.send(fd);"
            "};"
            // Live status polling every 1 second
            "function badge(open,labels){"
            "return open"
            "?\"<span class='badge badge-open'>\"+labels[0]+\"</span>\""
            ":\"<span class='badge badge-closed'>\"+labels[1]+\"</span>\";"
            "}"
            "function modeBadge(m){"
            "if(m==='FORCE_ON') return\"<span class='badge badge-force'>FORCE ON</span>\";"
            "if(m==='FORCE_OFF') return\"<span class='badge badge-off'>FORCE OFF</span>\";"
            "return\"<span class='badge badge-auto'>AUTO</span>\";"
            "}"
            "function pollStatus(){"
            "fetch('/status').then(function(r){return r.json();}).then(function(d){"
            "document.getElementById('val-door1').innerHTML=badge(d.door1,['OPEN','CLOSED']);"
            "document.getElementById('val-door2').innerHTML=badge(d.door2,['OPEN','CLOSED']);"
            "document.getElementById('val-mode').innerHTML=modeBadge(d.mode);"
            "if(d.time){document.getElementById('val-time').textContent=d.time;}"
            "if(d.rssi!=null){document.getElementById('val-rssi').textContent=d.rssi+' dBm';}"
            "}).catch(function(){});"
            "}"
            "setInterval(pollStatus,1000);"
            "function showMsg(id,cls,txt){"
            "var el=document.getElementById(id);"
            "el.className=cls;el.textContent=txt;el.style.display='block';"
            "setTimeout(function(){el.style.display='none';},3000);"
            "}"
            "function saveSched(){"
            "var en=document.getElementById('sched-en').checked;"
            "var on=document.getElementById('sched-on').value;"
            "var off=document.getElementById('sched-off').value;"
            "var days=[].slice.call(document.querySelectorAll('input[name=day]:checked'))"
            ".map(function(c){return'day='+c.value;}).join('&');"
            "fetch('/schedule',{method:'POST',"
            "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
            "body:'en='+(en?'1':'0')+'&on='+on+'&off='+off+(days?'&'+days:'')})"
            ".then(function(r){return r.text();})"
            ".then(function(){showMsg('sched-msg','success','Schedule saved.');})"
            ".catch(function(){showMsg('sched-msg','error','Save failed.');});"
            "}"
            "function saveHostname(){"
            "var h=document.getElementById('hostname-input').value.trim();"
            "if(!h||h.length>31){showMsg('wifi-msg','error','Enter a name up to 31 characters.');return;}"
            "showMsg('wifi-msg','info','Saving — device will reboot...');"
            "fetch('/set-hostname',{method:'POST',"
            "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
            "body:'hostname='+encodeURIComponent(h)})"
            ".catch(function(){});"
            "}"
            "function resetWifi(){"
            "if(!confirm('Reset Wi-Fi credentials and reboot?'))return;"
            "fetch('/reset-wifi',{method:'POST'})"
            ".then(function(){showMsg('wifi-msg','info','Rebooting into Wi-Fi setup...');})"
            ".catch(function(){showMsg('wifi-msg','error','Request failed.');});"
            "}"
            "function saveTimeout(){"
            "var m=parseInt(document.getElementById('timeout-min').value,10);"
            "if(isNaN(m)||m<1||m>60){"
            "showMsg('timeout-msg','error','Enter a value between 1 and 60.');return;}"
            "fetch('/set-timeout',{method:'POST',"
            "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
            "body:'minutes='+m})"
            ".then(function(){showMsg('timeout-msg','success','Timeout saved.');})"
            ".catch(function(){showMsg('timeout-msg','error','Save failed.');});"
            "}"
            "function cycleMode(){"
            "var btn=document.getElementById('mode-btn');"
            "btn.disabled=true;"
            "fetch('/cycle-mode',{method:'POST'})"
            ".then(function(r){return r.text();})"
            ".then(function(m){"
            "document.getElementById('val-mode').innerHTML=modeBadge(m);"
            "btn.disabled=false;"
            "})"
            ".catch(function(){btn.disabled=false;});"
            "}"
            // ── OTA update check ────────────────────────────
            "var g_updateBinUrl=null;"
            "function semverNewer(remote,local){"
            "function parts(v){return v.replace(/^v/,'').split('.').map(Number);}"
            "var r=parts(remote),l=parts(local);"
            "for(var i=0;i<3;i++){if((r[i]||0)>(l[i]||0))return true;"
            "if((r[i]||0)<(l[i]||0))return false;}return false;}"
            "function checkForUpdate(){"
            "var LOCAL='" FIRMWARE_VERSION "';"
            "fetch('https://raw.githubusercontent.com/Hjort3Design/wine-cabinet-firmware/main/version.json')"
            ".then(function(r){return r.json();})"
            ".then(function(d){"
            "if(semverNewer(d.version,LOCAL)){"
            "g_updateBinUrl=d.bin_url;"
            "document.getElementById('update-version').textContent=d.version;"
            "document.getElementById('update-notes').textContent=d.notes;"
            "document.getElementById('update-banner').style.display='block';"
            "}"
            "}).catch(function(){});"
            "}"
            "function installUpdate(){"
            "if(!g_updateBinUrl)return;"
            "var btn=document.getElementById('update-btn');"
            "var msg=document.getElementById('update-msg');"
            "btn.disabled=true;"
            "msg.textContent='Downloading firmware...';"
            "fetch(g_updateBinUrl)"
            ".then(function(r){"
            "if(!r.ok)throw new Error('Download failed');"
            "msg.textContent='Installing...';"
            "return r.blob();"
            "})"
            ".then(function(blob){"
            "var fd=new FormData();"
            "fd.append('update',new File([blob],'firmware.bin'));"
            "var xhr=new XMLHttpRequest();"
            "xhr.open('POST','/update');"
            "xhr.upload.onprogress=function(ev){"
            "if(ev.lengthComputable){"
            "var p=Math.round(ev.loaded*100/ev.total);"
            "msg.textContent='Installing... '+p+'%';"
            "}"
            "};"
            "xhr.onload=function(){"
            "if(xhr.status===200){"
            "msg.textContent='Update complete — rebooting...';"
            "document.getElementById('update-banner').style.background='#1a2e1a';"
            "}else{"
            "msg.textContent='Install failed ('+xhr.status+').';"
            "btn.disabled=false;"
            "}"
            "};"
            "xhr.onerror=function(){msg.textContent='Network error.';btn.disabled=false;};"
            "xhr.send(fd);"
            "})"
            ".catch(function(e){msg.textContent=e.message;btn.disabled=false;});"
            "}"
            "checkForUpdate();"
            "</script>"
            "</body></html>");
  return html;
}

void handleWebRoot() {
  webServer.send(200, "text/html", buildWebOtaPage());
}

void handleWebCss() {
  webServer.send_P(200, "text/css", kPageCss);
}

void handleWebStatus() {
  char timeBuf[9] = "";
  if (g_ntpSynced) {
    struct tm t;
    if (getLocalTime(&t)) {
      snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d",
               t.tm_hour, t.tm_min, t.tm_sec);
    }
  }
  String json;
  json.reserve(120);
  json += F("{\"door1\":");
  json += isDoor1Open() ? "true" : "false";
  json += F(",\"door2\":");
  json += isDoor2Open() ? "true" : "false";
  json += F(",\"mode\":\"");
  if      (g_controlMode == ControlMode::kForceOn)  json += "FORCE_ON";
  else if (g_controlMode == ControlMode::kForceOff) json += "FORCE_OFF";
  else                                              json += "AUTO";
  json += F("\",\"time\":\"");
  json += timeBuf;
  json += F("\",\"rssi\":");
  json += WiFi.isConnected() ? String(WiFi.RSSI()) : "null";
  json += F("}");
  webServer.send(200, "application/json", json);
}

void handleWebSchedulePost() {
  const String en  = webServer.arg("en");
  const String on  = webServer.arg("on");   // "HH:MM"
  const String off = webServer.arg("off");  // "HH:MM"

  if (on.length() == 5 && off.length() == 5) {
    g_schedOn.hour    = (uint8_t)on.substring(0, 2).toInt();
    g_schedOn.minute  = (uint8_t)on.substring(3, 5).toInt();
    g_schedOff.hour   = (uint8_t)off.substring(0, 2).toInt();
    g_schedOff.minute = (uint8_t)off.substring(3, 5).toInt();
  }
  g_schedEnabled = (en == "1");

  // Rebuild days bitmask from repeated "day" args (0=Mon…6=Sun)
  g_schedDays = 0;
  const int argCount = webServer.args();
  for (int i = 0; i < argCount; i++) {
    if (webServer.argName(i) == "day") {
      const uint8_t d = (uint8_t)webServer.arg(i).toInt();
      if (d < 7) g_schedDays |= (1 << d);
    }
  }

  saveSchedulePref();

  Serial.printf("Schedule %s  on=%02u:%02u  off=%02u:%02u  days=0x%02X\n",
                g_schedEnabled ? "enabled" : "disabled",
                g_schedOn.hour, g_schedOn.minute,
                g_schedOff.hour, g_schedOff.minute,
                g_schedDays);

  webServer.send(200, "text/plain", "OK");
}

void handleWebResetWifi() {
  webServer.send(200, "text/plain", "Resetting Wi-Fi. Rebooting...");
  delay(200);
  wifiManager.resetSettings();
  ESP.restart();
}

void handleWebUpdateResult() {
  const bool success = !Update.hasError();
  webServer.send(
      success ? 200 : 500, "text/plain",
      success ? "Update complete. Rebooting..." : "OTA update failed.");

  if (success) {
    g_pendingRestart = true;
  } else {
    g_otaError = true;
    setStatusLedMode(StatusLedMode::kError);
  }
}

void handleWebUpdateUpload() {
  HTTPUpload& upload = webServer.upload();

  if (upload.status == UPLOAD_FILE_START) {
    g_otaError = false;
    setStatusLedMode(StatusLedMode::kOtaActive);
    const bool ok = Update.begin(UPDATE_SIZE_UNKNOWN);
    if (!ok) {
      Update.printError(Serial);
    }
    return;
  }

  if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
    return;
  }

  if (upload.status == UPLOAD_FILE_END) {
    const bool ok = Update.end(true);
    if (!ok) {
      Update.printError(Serial);
      g_otaError = true;
      setStatusLedMode(StatusLedMode::kError);
    }
    return;
  }

  if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    g_otaError = true;
    setStatusLedMode(StatusLedMode::kError);
  }
}

void loadPrefs() {
  prefs.begin("wine-cab", true);  // read-only
  g_bothDoorsTimeoutMs = prefs.getULong("door_timeout", 2UL * 60UL * 1000UL);
  prefs.getString("hostname", g_hostname, sizeof(g_hostname));
  if (g_hostname[0] == '\0') strlcpy(g_hostname, "wine-cabinet", sizeof(g_hostname));
  // Schedule
  g_schedEnabled    = prefs.getBool("sched_en",     false);
  g_schedOn.hour    = prefs.getUChar("sched_on_h",  8);
  g_schedOn.minute  = prefs.getUChar("sched_on_m",  0);
  g_schedOff.hour   = prefs.getUChar("sched_off_h", 23);
  g_schedOff.minute = prefs.getUChar("sched_off_m", 0);
  g_schedDays       = prefs.getUChar("sched_days",  0x7F);
  prefs.end();
}

void saveTimeoutPref() {
  prefs.begin("wine-cab", false);
  prefs.putULong("door_timeout", g_bothDoorsTimeoutMs);
  prefs.end();
}

void saveHostnamePref() {
  prefs.begin("wine-cab", false);
  prefs.putString("hostname", g_hostname);
  prefs.end();
}

void saveSchedulePref() {
  prefs.begin("wine-cab", false);
  prefs.putBool("sched_en",     g_schedEnabled);
  prefs.putUChar("sched_on_h",  g_schedOn.hour);
  prefs.putUChar("sched_on_m",  g_schedOn.minute);
  prefs.putUChar("sched_off_h", g_schedOff.hour);
  prefs.putUChar("sched_off_m", g_schedOff.minute);
  prefs.putUChar("sched_days",  g_schedDays);
  prefs.end();
}

void handleWebSetTimeout() {
  const String val = webServer.arg("minutes");
  if (val.length() > 0) {
    const unsigned long minutes = (unsigned long)val.toInt();
    if (minutes >= 1 && minutes <= 60) {
      g_bothDoorsTimeoutMs = minutes * 60UL * 1000UL;
      saveTimeoutPref();
    }
  }
  webServer.send(200, "text/plain", "OK");
}

void handleWebSetHostname() {
  const String val = webServer.arg("hostname");
  if (val.length() >= 1 && val.length() <= 31) {
    strlcpy(g_hostname, val.c_str(), sizeof(g_hostname));
    saveHostnamePref();
    webServer.send(200, "text/plain", "OK");
    delay(100);
    ESP.restart();
  } else {
    webServer.send(400, "text/plain", "Invalid hostname");
  }
}

void handleWebCycleMode() {
  switch (g_controlMode) {
    case ControlMode::kAuto:     g_controlMode = ControlMode::kForceOn;  break;
    case ControlMode::kForceOn:  g_controlMode = ControlMode::kForceOff; break;
    case ControlMode::kForceOff: g_controlMode = ControlMode::kAuto;     break;
  }
  g_bothDoorsOpenSinceMs = millis();
  triggerDebugBlink();

  const char* modeStr =
      (g_controlMode == ControlMode::kForceOn)  ? "FORCE_ON"  :
      (g_controlMode == ControlMode::kForceOff) ? "FORCE_OFF" : "AUTO";
  webServer.send(200, "text/plain", modeStr);
}

void setupWebOta() {
  if (g_otaServicesStarted) {
    return;
  }

  webServer.on("/", HTTP_GET, handleWebRoot);
  webServer.on("/style.css", HTTP_GET, handleWebCss);
  webServer.on("/status", HTTP_GET, handleWebStatus);
  webServer.on("/cycle-mode", HTTP_POST, handleWebCycleMode);
  webServer.on("/set-timeout", HTTP_POST, handleWebSetTimeout);
  webServer.on("/schedule", HTTP_POST, handleWebSchedulePost);
  webServer.on("/reset-wifi", HTTP_POST, handleWebResetWifi);
  webServer.on("/set-hostname", HTTP_POST, handleWebSetHostname);
  webServer.on(
      "/update", HTTP_POST, handleWebUpdateResult, handleWebUpdateUpload);
  webServer.begin();
  g_otaServicesStarted = true;
}

void setupArduinoOta() {
  ArduinoOTA.setHostname(g_hostname);

  if (Config::kOtaPassword[0] != '\0') {
    ArduinoOTA.setPassword(Config::kOtaPassword);
  }

  ArduinoOTA.onStart([]() {
    g_otaError = false;
    setStatusLedMode(StatusLedMode::kOtaActive);
  });

  ArduinoOTA.onEnd([]() {
    setStatusLedMode(StatusLedMode::kConnected);
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    if (total == 0) {
      return;
    }

    static unsigned int lastPercent = 0;
    const unsigned int percent = (progress * 100U) / total;
    if (percent != lastPercent) {
      lastPercent = percent;
      Serial.printf("Arduino OTA progress: %u%%\n", percent);
    }
  });

  ArduinoOTA.onError([](ota_error_t error) {
    g_otaError = true;
    setStatusLedMode(StatusLedMode::kError);
    Serial.printf("Arduino OTA error: %u\n", static_cast<unsigned int>(error));
  });

  ArduinoOTA.begin();
}

void startMdns() {
  MDNS.begin(g_hostname);
  MDNS.addService("http", "tcp", 80);
}

void connectWiFi() {
  WiFi.setHostname(g_hostname);
  wifiManager.setConfigPortalBlocking(false);
  wifiManager.setConfigPortalTimeout(Config::kPortalTimeoutSeconds);
  wifiManager.setAPCallback([](WiFiManager*) {
    g_portalActive = true;
    setStatusLedMode(StatusLedMode::kPortal);
  });
  wifiManager.setSaveConfigCallback([]() {
    Serial.println("Wi-Fi credentials saved.");
  });

  const bool connected =
      wifiManager.autoConnect(Config::kWiFiPortalName);

  g_wifiConnected = connected;
  g_portalActive = !connected;
  setStatusLedMode(connected ? StatusLedMode::kConnected
                             : StatusLedMode::kPortal);
}

void serviceWiFiPortal() {
  if (!g_portalActive) {
    return;
  }

  wifiManager.process();

  if (WiFi.isConnected()) {
    g_portalActive = false;
    g_wifiConnected = true;
    setStatusLedMode(StatusLedMode::kConnected);
    configTime(Config::kGmtOffsetSec, Config::kDaylightOffsetSec, Config::kNtpServer);
    g_ntpSynced = true;
    setupArduinoOta();
    setupWebOta();
    startMdns();
    Serial.print("Wi-Fi connected. IP: ");
    Serial.println(WiFi.localIP());
  }
}

void serviceWiFiReconnect() {
  if (g_portalActive || WiFi.isConnected()) {
    return;
  }

  const unsigned long now = millis();
  if (now - g_lastReconnectAttemptMs < 5000) {
    return;
  }

  g_lastReconnectAttemptMs = now;
  WiFi.reconnect();
}

void setupInputs() {
  pinMode(Config::kDoorSensor1Pin, INPUT_PULLUP);
  pinMode(Config::kDoorSensor2Pin, INPUT_PULLUP);
  pinMode(Config::kOverrideButtonPin, INPUT_PULLUP);

  g_lastRawButtonPressed = readActiveLowInput(Config::kOverrideButtonPin);
  g_stableButtonPressed = g_lastRawButtonPressed;
  g_lastDoor1 = isDoor1Open();
  g_lastDoor2 = isDoor2Open();
}

void setupOutputs() {
  pinMode(Config::kStatusLedPin, OUTPUT);
  writeStatusLed(false);

  for (size_t i = 0; i < Config::kMosfetCount; ++i) {
    configurePwmOutput(Config::kMosfetPins[i], static_cast<uint8_t>(i));
  }
}

void printStartupSummary() {
  Serial.println();
  Serial.println("Wine Cabinet LED Dimmer starting");
  Serial.printf("Door sensor pins: %u, %u\n", Config::kDoorSensor1Pin,
                Config::kDoorSensor2Pin);
  Serial.printf("Override button pin: %u\n", Config::kOverrideButtonPin);
  Serial.printf("Status LED pin: %u\n", Config::kStatusLedPin);
  Serial.printf("Default brightness: %u\n", Config::kOnBrightness);
  Serial.printf("Fade duration: %lu ms\n", Config::kFadeDurationMs);
}

void setup() {
  Serial.begin(115200);
  delay(200);

  loadPrefs();
  setupOutputs();
  setupInputs();
  setStatusLedMode(StatusLedMode::kBooting);
  setBrightnessImmediate(0);
  g_lightsOn = false;
  printStartupSummary();

  connectWiFi();

  if (g_wifiConnected) {
    Serial.print("Wi-Fi connected. IP: ");
    Serial.println(WiFi.localIP());
    configTime(Config::kGmtOffsetSec, Config::kDaylightOffsetSec, Config::kNtpServer);
    g_ntpSynced = true;
    setupArduinoOta();
    setupWebOta();
    startMdns();
  } else {
    Serial.println("Wi-Fi portal active. Connect to configure credentials.");
  }
}

void loop() {
  handleDoorInputs();
  handleOverrideButton();
  handleBothDoorsOpenTimeout();
  applyLightingState(shouldLightsBeOn());
  updateFade();

  if (g_portalActive) {
    serviceWiFiPortal();
  } else if (g_wifiConnected && WiFi.isConnected()) {
    ArduinoOTA.handle();
    webServer.handleClient();
  } else if (g_wifiConnected && !WiFi.isConnected()) {
    g_wifiConnected = false;
    setStatusLedMode(StatusLedMode::kError);
  } else if (!g_wifiConnected && WiFi.isConnected()) {
    g_wifiConnected = true;
    setStatusLedMode(StatusLedMode::kConnected);
  }

  serviceWiFiReconnect();

  if (g_pendingRestart) {
    delay(250);
    ESP.restart();
  }

  if (!g_otaError && g_wifiConnected && WiFi.isConnected() &&
      g_statusLedMode != StatusLedMode::kOtaActive) {
    setStatusLedMode(StatusLedMode::kConnected);
  }

  updateStatusLed();
}
