#include "m5_compat.h"
#include <LittleFS.h>
#include <stdarg.h>
#include <time.h>
#include "ble_bridge.h"
#include "data.h"
#include "buddy.h"

TFT_eSprite spr = TFT_eSprite(&M5.Lcd);

// Advertise as "Claude-XXXX" (last two BT MAC bytes) so multiple sticks
// in one room are distinguishable in the desktop picker. Name persists in
// btName for the BLUETOOTH info page.
static char btName[16] = "Claude";
static void startBt() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(btName, sizeof(btName), "Claude-%02X%02X", mac[4], mac[5]);
  bleInit(btName);
}

#include "character.h"
#include "stats.h"
int W = 135, H = 240;
int CX = 67;
int CY_BASE = 120;
const int LED_PIN = BUDDY_DEFAULT_LED_PIN;   // red LED, active-low (S3: G19)

// Colors used across multiple UI surfaces
const uint16_t HOT   = 0xFA20;   // red-orange: warnings, impatience, deny
const uint16_t PANEL = 0x2104;   // overlay panel background

enum PersonaState { P_SLEEP, P_IDLE, P_BUSY, P_ATTENTION, P_CELEBRATE, P_DIZZY, P_HEART };
const char* stateNames[] = { "sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart" };

TamaState    tama;
PersonaState baseState   = P_SLEEP;
PersonaState activeState = P_SLEEP;
uint32_t     oneShotUntil = 0;
uint32_t     lastShakeCheck = 0;
float        accelBaseline = 1.0f;
unsigned long t = 0;

// Menu
bool    menuOpen    = false;
uint8_t menuSel     = 0;
uint8_t brightLevel = 4;           // 0..4 → ScreenBreath 20..100
bool    btnALong    = false;

enum DisplayMode { DISP_NORMAL, DISP_PET, DISP_INFO, DISP_COUNT };
uint8_t displayMode = DISP_NORMAL;
uint8_t infoPage = 0;
uint8_t petPage = 0;
const uint8_t PET_PAGES = 2;
uint8_t msgScroll = 0;
uint16_t lastLineGen = 0;
char     lastPromptId[40] = "";
char     lastHandledPromptId[40] = "";
uint32_t lastInteractMs = 0;
bool     dimmed = false;
bool     screenOff = false;
bool     swallowBtnA = false;
bool     swallowBtnB = false;
bool     buddyMode = false;
bool     gifAvailable = false;
uint8_t  interactivePage = 0;
int8_t   interactiveAnswers[TamaState::INTERACTIVE_Q_MAX] = { -1, -1, -1, -1 };
char     lastInteractiveId[32] = "";
uint8_t  lastInteractiveQuestionIndex = 0;
bool     interactiveSubmitting = false;
const uint8_t SPECIES_GIF = 0xFF;   // species NVS sentinel: use the installed GIF

static bool uiLandscape() {
  return settings().clockRot == 1;
}

static int contentTop() {
  return uiLandscape() ? 10 : 70;
}

static int landscapeSidePanelX() {
  return 136;
}

static int landscapeSidePanelW() {
  return W - landscapeSidePanelX() - 6;
}

static int landscapePhotoX() { return 20; }
static int landscapePhotoY() { return 34; }
static int landscapePhotoW() { return 76; }
static int landscapePhotoH() { return 92; }
static int landscapePhotoFrameLeft() { return 0; }
static int landscapePhotoFrameRight() { return 98; }
static int landscapePhotoFrameBottom() { return 135; }

static void clearLandscapePhotoSurround(const Palette& p, int top, int bottom) {
  spr.fillRect(landscapePhotoFrameRight(), top, W - landscapePhotoFrameRight(), bottom - top, p.bg);
  spr.fillRect(landscapePhotoFrameLeft(), top, landscapePhotoFrameRight(), landscapePhotoY() - top, p.bg);
  spr.fillRect(landscapePhotoFrameLeft(), landscapePhotoY(), landscapePhotoX() - landscapePhotoFrameLeft(), landscapePhotoH(), p.bg);
  spr.fillRect(landscapePhotoX() + landscapePhotoW(), landscapePhotoY(),
               landscapePhotoFrameRight() - (landscapePhotoX() + landscapePhotoW()),
               landscapePhotoH(), p.bg);
  spr.fillRect(landscapePhotoFrameLeft(), landscapePhotoY() + landscapePhotoH(),
               landscapePhotoFrameRight(), bottom - (landscapePhotoY() + landscapePhotoH()), p.bg);
}

static void configureUiGeometry(bool force = false) {
  int nextW = uiLandscape() ? 240 : 135;
  int nextH = uiLandscape() ? 135 : 240;
  uint8_t rot = uiLandscape() ? 1 : 0;
  bool changed = force || nextW != W || nextH != H;
  W = nextW;
  H = nextH;
  CX = W / 2;
  CY_BASE = H / 2;
  M5.Display.setRotation(rot);
  if (changed) {
    if (spr.width() > 0) spr.deleteSprite();
    spr.setColorDepth(16);
    spr.createSprite(W, H);
  }
}

// Cycle GIF (if installed) → ASCII species 0..N-1 → GIF. Persisted to the
// existing "species" NVS key; 0xFF means GIF mode.
static void nextPet() {
  uint8_t n = buddySpeciesCount();
  if (!buddyMode) {                          // GIF → species 0
    buddyMode = true;
    buddySetSpeciesIdx(0);
    speciesIdxSave(0);
  } else if (buddySpeciesIdx() + 1 >= n && gifAvailable) {  // last species → GIF
    buddyMode = false;
    speciesIdxSave(SPECIES_GIF);
  } else {                                   // species i → species i+1
    buddyNextSpecies();
  }
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
}
uint32_t wakeTransitionUntil = 0;
uint32_t comboPressStartMs = 0;
bool     comboScreenHandled = false;
const uint32_t SCREEN_OFF_MS = 30000;
const uint32_t USB_IDLE_SLEEP_MS = 30UL * 60UL * 1000UL;
const uint32_t ENERGY_RESTORE_SLEEP_MS = 30UL * 60UL * 1000UL;
const uint32_t IDLE_SCREEN_OFF_MS = 30000;
const uint32_t BUSY_DIM_MS = 30000;
const uint32_t SLEEP_SCREEN_OFF_MS = 10000;
const uint32_t SHORT_DUTY_WINDOW_MS = 5UL * 60UL * 1000UL;
const uint32_t SHORT_ADV_ON_MS = 5000;
const uint32_t SHORT_ADV_PERIOD_MS = 10000;
const uint32_t LONG_ADV_ON_MS = 5000;
const uint32_t LONG_ADV_PERIOD_MS = 60000;
const uint8_t  MIN_VISIBLE_BRIGHTNESS = 8;

bool     napping = false;
uint32_t napStartMs = 0;
uint32_t promptArrivedMs = 0;
uint32_t idleStartedMs = 0;
bool     sleepStateActive = true;
uint32_t sleepStateStartedMs = 0;
PersonaState lastBaseState = P_SLEEP;

// IMU-backed face-down nap is disabled for the Codex buddy build.
static bool isFaceDown() {
  return false;
}

static void applyBrightness() { compat::setScreenBrightness0_100(20 + brightLevel * 20); }

static void wake() {
  lastInteractMs = millis();
  if (screenOff) {
    compat::screenPower(true);
    applyBrightness();
    screenOff = false;
    wakeTransitionUntil = millis() + 12000;
  }
  if (dimmed) { applyBrightness(); dimmed = false; }
  bleSetAdvertising(true);
}
bool     responseSent = false;
bool     interactiveAlertLatched = false;

static bool startsWith(const char* text, const char* prefix) {
  if (!text || !prefix) return false;
  size_t n = strlen(prefix);
  return strncmp(text, prefix, n) == 0;
}

static bool isInteractiveWaiting(const TamaState& s) {
  if (s.promptId[0] != 0) return false;
  if (s.sessionsWaiting == 0) return false;
  return startsWith(s.msg, "input needed") || startsWith(s.msg, "choice needed");
}

static bool interactiveUiEnabled() {
  return settings().interactiveQA;
}

static bool interactiveActive() {
  return interactiveUiEnabled() && isInteractiveWaiting(tama);
}

static bool interactiveHostSubmitting() {
  return strcmp(tama.interactiveStatus, "submitting") == 0;
}

static bool interactiveSubmissionPending() {
  return interactiveSubmitting || interactiveHostSubmitting();
}

static uint8_t drawCenteredWrappedBlock(const char* text, int topY, int maxRows, uint16_t color, int maxPx = 124, int lineHeight = 14);
static uint8_t drawWrappedParagraph(const char* text, int x, int y, int maxRows, int maxPx, int lineHeight, uint16_t color);
static int paragraphLineHeight(const char* text, int baseLineHeight);
static uint8_t drawBodyWrappedParagraph(const char* text, int x, int y, int maxRows, int maxPx, int lineHeight, uint16_t color);

static uint8_t interactivePageCount() {
  return 1;
}

static int interactivePageForQuestion(uint8_t page, uint8_t* questionIndex, int8_t* optionIndex) {
  if (page == 0) { *questionIndex = 0; *optionIndex = -1; return 0; }
  if (page == 1) { *questionIndex = 0; *optionIndex = -1; return 1; }
  if (tama.interactiveQuestionCount > 0) {
    uint8_t optPage = page - 2;
    if (optPage < tama.interactiveOptionCounts[0]) {
      *questionIndex = 0;
      *optionIndex = (int8_t)optPage;
      return 2;
    }
  }
  *questionIndex = 0; *optionIndex = -1;
  return 0;
}

static void resetInteractiveAnswers() {
  for (uint8_t i = 0; i < TamaState::INTERACTIVE_Q_MAX; i++) interactiveAnswers[i] = -1;
  interactivePage = 0;
  interactiveSubmitting = false;
}

static void beepInteractiveAlert() {
  compat::beep(900, 50);
  delay(40);
  compat::beep(1200, 50);
}

static void beepWorkComplete() {
  if (!settings().sound) return;
  compat::beep(1600, 45);
  delay(35);
  compat::beep(2200, 70);
}

struct BatteryUiState {
  bool initialized = false;
  uint32_t lastSampleMs = 0;
  int filteredBatMv = 0;
  int filteredCurrentMa = 0;
  int filteredVbusMv = 0;
};

static BatteryUiState batteryUi;

static void sampleBatteryUi(uint32_t now) {
  int rawBatMv = (int)(compat::batVoltageV() * 1000);
  int rawCurrentMa = (int)compat::batCurrentMA();
  int rawVbusMv = (int)(compat::vbusVoltageV() * 1000);

  if (!batteryUi.initialized) {
    batteryUi.initialized = true;
    batteryUi.filteredBatMv = rawBatMv;
    batteryUi.filteredCurrentMa = rawCurrentMa;
    batteryUi.filteredVbusMv = rawVbusMv;
    batteryUi.lastSampleMs = now;
    return;
  }

  if ((now - batteryUi.lastSampleMs) < 1500) return;
  batteryUi.lastSampleMs = now;

  // Battery voltage jitters noticeably on the PMIC ADC; smooth it before
  // mapping to a percentage so the UI does not bounce every frame.
  batteryUi.filteredBatMv = (batteryUi.filteredBatMv * 7 + rawBatMv) / 8;
  batteryUi.filteredCurrentMa = (batteryUi.filteredCurrentMa * 3 + rawCurrentMa) / 4;
  batteryUi.filteredVbusMv = (batteryUi.filteredVbusMv * 3 + rawVbusMv) / 4;
}

static void beep(uint16_t freq, uint16_t dur) {
  if (!settings().sound) return;
  compat::beep(freq, dur);
}

static void sendCmd(const char* json) {
  Serial.println(json);
  size_t n = strlen(json);
  bleWrite((const uint8_t*)json, n);
  bleWrite((const uint8_t*)"\n", 1);
}

static void sendInteractiveSelection(uint8_t questionIndex, uint8_t answerIndex) {
  char cmd[192];
  snprintf(
    cmd,
    sizeof(cmd),
    "{\"cmd\":\"interactive_select\",\"id\":\"%s\",\"question_index\":%u,\"answer\":%u}",
    tama.interactiveId,
    questionIndex,
    answerIndex
  );
  sendCmd(cmd);
}
const uint8_t INFO_PAGES = 6;
const uint8_t INFO_PG_BUTTONS = 2;
const uint8_t INFO_PG_CREDITS = 5;

void applyDisplayMode() {
  bool peek = false;
  if (displayMode == DISP_PET) peek = uiLandscape() ? (petPage == 0) : true;
  else if (displayMode == DISP_INFO) peek = uiLandscape() ? (infoPage == 0) : true;
  characterSetPeek(peek);
  buddySetPeek(peek);
  // Clear the whole sprite on mode switch. drawInfo/drawPet clear their
  // own regions when they run, but when you switch FROM info/pet TO normal,
  // those functions stop running and their stale pixels stay behind. Full
  // clear is cheap and guarantees no leftovers between modes.
  spr.fillSprite(0x0000);
  characterInvalidate();  // redraws character on next tick (text mode path)
}

const char* menuItems[] = { "settings", "turn off", "help", "about", "demo", "close" };
const uint8_t MENU_N = 6;

bool    settingsOpen = false;
uint8_t settingsSel  = 0;
const char* settingsItems[] = { "brightness", "sound", "bluetooth", "wifi", "led", "transcript", "interactive qa", "rotation", "ascii pet", "reset", "back" };
const uint8_t SETTINGS_N = 11;

bool    resetOpen = false;
uint8_t resetSel  = 0;
const char* resetItems[] = { "delete char", "factory reset", "back" };
const uint8_t RESET_N = 3;
static uint32_t resetConfirmUntil = 0;
static uint8_t  resetConfirmIdx = 0xFF;

static void applySetting(uint8_t idx) {
  Settings& s = settings();
  switch (idx) {
    case 0:
      brightLevel = (brightLevel + 1) % 5;
      applyBrightness();
      return;
    case 1: s.sound = !s.sound; break;
    case 2:
      // BT toggle is a stored preference only — BLE stays live. Turning
      // BLE off cleanly would require tearing down the BLE stack which
      // the Arduino BLE library doesn't do reliably. If we need a
      // hard-off someday, stop advertising via BLEDevice::getAdvertising().
      s.bt = !s.bt;
      break;
    case 3: s.wifi = !s.wifi; break;   // stored only — no WiFi stack linked
    case 4: s.led = !s.led; break;
    case 5: s.hud = !s.hud; break;
    case 6: s.interactiveQA = !s.interactiveQA; break;
    case 7: s.clockRot = (s.clockRot + 1) % 2; break;
    case 8: nextPet(); return;
    case 9: resetOpen = true; resetSel = 0; resetConfirmIdx = 0xFF; return;
    case 10: settingsOpen = false; characterInvalidate(); return;
  }
  settingsSave();
  if (idx == 7) {
    configureUiGeometry(true);
    applyDisplayMode();
  }
}

// Tap-twice confirm: first tap arms (label flips to "really?"), second
// within 3s executes. Scrolling away clears the arm.
static void applyReset(uint8_t idx) {
  uint32_t now = millis();
  bool armed = (resetConfirmIdx == idx) && (int32_t)(now - resetConfirmUntil) < 0;

  if (idx == 2) { resetOpen = false; return; }

  if (!armed) {
    resetConfirmIdx = idx;
    resetConfirmUntil = now + 3000;
    beep(1400, 60);
    return;
  }

  beep(800, 200);
  if (idx == 0) {
    // delete char: wipe /characters/, reboot into ASCII mode
    File d = LittleFS.open("/characters");
    if (d && d.isDirectory()) {
      File e;
      while ((e = d.openNextFile())) {
        char path[80];
        snprintf(path, sizeof(path), "/characters/%s", e.name());
        if (e.isDirectory()) {
          File f;
          while ((f = e.openNextFile())) {
            char fp[128];
            snprintf(fp, sizeof(fp), "%s/%s", path, f.name());
            f.close();
            LittleFS.remove(fp);
          }
          e.close();
          LittleFS.rmdir(path);
        } else {
          e.close();
          LittleFS.remove(path);
        }
      }
      d.close();
    }
  } else {
    // factory reset: NVS namespace wipe + filesystem format + BLE bonds.
    // Clears stats, owner, petname, species, settings, GIF characters,
    // and any stored LTKs so the next desktop has to re-pair.
    _prefs.begin("buddy", false);
    _prefs.clear();
    _prefs.end();
    LittleFS.format();
    bleClearBonds();
  }
  delay(300);
  ESP.restart();
}

// Footer hint row inside a menu panel.
// Portrait: "<downLbl> ↓  <rightLbl> →"
// Landscape: "<downLbl> →  <rightLbl> ↑"
// Panels add MENU_HINT_H to height and call this at bottom.
const int MENU_HINT_H = 14;
static void drawMenuHints(const Palette& p, int mx, int mw, int hy,
                          const char* downLbl = "A", const char* rightLbl = "B") {
  spr.drawFastHLine(mx + 6, hy - 4, mw - 12, p.textDim);
  spr.setTextColor(p.textDim, PANEL);
  // 6px/glyph at size 1; triangle goes 4px after the label ends
  bool land = uiLandscape();
  int x = mx + 8;
  spr.setCursor(x, hy); spr.print(downLbl);
  x += strlen(downLbl) * 6 + 4;
  if (land) spr.fillTriangle(x, hy, x, hy + 6, x + 5, hy + 3, p.textDim);
  else spr.fillTriangle(x, hy + 1, x + 6, hy + 1, x + 3, hy + 6, p.textDim);
  x = mx + mw / 2 + 4;
  spr.setCursor(x, hy); spr.print(rightLbl);
  x += strlen(rightLbl) * 6 + 4;
  if (land) spr.fillTriangle(x + 3, hy, x, hy + 6, x + 6, hy + 6, p.textDim);
  else spr.fillTriangle(x, hy, x, hy + 6, x + 5, hy + 3, p.textDim);
}

static void drawSettings() {
  const Palette& p = characterPalette();
  const bool land = uiLandscape();
  const int rowH = 14;
  const int visibleRows = land ? 7 : SETTINGS_N;
  int mw = 118;
  int mh = 16 + visibleRows * rowH + MENU_HINT_H;
  if (land && mh > H - 12) mh = H - 12;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr.setTextSize(1);
  Settings& s = settings();
  bool vals[] = { s.sound, s.bt, s.wifi, s.led, s.hud, s.interactiveQA };
  int start = 0;
  if (land && SETTINGS_N > visibleRows) {
    start = settingsSel - visibleRows / 2;
    if (start < 0) start = 0;
    int maxStart = SETTINGS_N - visibleRows;
    if (start > maxStart) start = maxStart;
  }
  int end = land ? min<int>(SETTINGS_N, start + visibleRows) : SETTINGS_N;
  for (int i = start; i < end; i++) {
    int row = i - start;
    bool sel = (i == settingsSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 8 + row * rowH);
    spr.print(sel ? "> " : "  ");
    spr.print(settingsItems[i]);
    spr.setCursor(mx + mw - 36, my + 8 + row * rowH);
    spr.setTextColor(p.textDim, PANEL);
    if (i == 0) {
      spr.printf("%u/4", brightLevel);
    } else if (i >= 1 && i <= 6) {
      spr.setTextColor(vals[i-1] ? GREEN : p.textDim, PANEL);
      spr.print(vals[i-1] ? " on" : "off");
    } else if (i == 7) {
      static const char* const RN[] = { "port", "land" };
      spr.print(RN[s.clockRot]);
    } else if (i == 8) {
      uint8_t total = buddySpeciesCount() + (gifAvailable ? 1 : 0);
      uint8_t pos   = buddyMode ? buddySpeciesIdx() + 1 : total;
      spr.printf("%u/%u", pos, total);
    }
  }
  if (land && start > 0) {
    spr.fillTriangle(mx + mw - 10, my + 6, mx + mw - 14, my + 10, mx + mw - 6, my + 10, p.textDim);
  }
  if (land && end < SETTINGS_N) {
    int by = my + mh - MENU_HINT_H - 6;
    spr.fillTriangle(mx + mw - 10, by + 4, mx + mw - 14, by, mx + mw - 6, by, p.textDim);
  }
  drawMenuHints(p, mx, mw, my + mh - 12, "Next", "Change");
}

static void drawReset() {
  const Palette& p = characterPalette();
  int mw = 118, mh = 16 + RESET_N * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, HOT);
  spr.setTextSize(1);
  for (int i = 0; i < RESET_N; i++) {
    bool sel = (i == resetSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 8 + i * 14);
    spr.print(sel ? "> " : "  ");
    bool armed = (i == resetConfirmIdx) &&
                 (int32_t)(millis() - resetConfirmUntil) < 0;
    if (armed) spr.setTextColor(HOT, PANEL);
    spr.print(armed ? "really?" : resetItems[i]);
  }
  drawMenuHints(p, mx, mw, my + mh - 12);
}

void menuConfirm() {
  switch (menuSel) {
    case 0: settingsOpen = true; menuOpen = false; settingsSel = 0; break;
    case 1: compat::powerOff(); break;
    case 2:
    case 3:
      menuOpen = false;
      displayMode = DISP_INFO;
      infoPage = (menuSel == 2) ? INFO_PG_BUTTONS : INFO_PG_CREDITS;
      applyDisplayMode();
      characterInvalidate();
      break;
    case 4: dataSetDemo(!dataDemo()); break;
    case 5: menuOpen = false; characterInvalidate(); break;
  }
}

void drawMenu() {
  const Palette& p = characterPalette();
  int mw = 118, mh = 16 + MENU_N * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr.setTextSize(1);
  for (int i = 0; i < MENU_N; i++) {
    bool sel = (i == menuSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 8 + i * 14);
    spr.print(sel ? "> " : "  ");
    spr.print(menuItems[i]);
    if (i == 4) spr.print(dataDemo() ? "  on" : "  off");
  }
  drawMenuHints(p, mx, mw, my + mh - 12);
}

// Clock orientation: gravity along the in-plane X axis means the stick is
// on its side. Signed counter for hysteresis on both transitions — same
// pattern as face-down nap.
//   0 = portrait (sprite path, pet sleeps underneath)
//   1 = landscape, BtnA-side down (M5.Lcd rotation 1)
//   3 = landscape, USB-side down (M5.Lcd rotation 3)
static uint8_t clockOrient   = 0;
static int8_t  orientFrames  = 0;
static uint8_t paintedOrient = 0;
// RTC and IMU share an I2C bus. Reading the RTC at 60fps starves the IMU
// reads in clockUpdateOrient — orientation detection gets noisy. Cache the
// time once per second; mood logic and drawClock both read from here.
static RTC_TimeTypeDef _clkTm;
static RTC_DateTypeDef _clkDt;
uint32_t               _clkLastRead = 0;   // zeroed by data.h on time-sync
time_t                 _clkEpochLocal = 0; // local epoch (already tz-adjusted)
uint32_t               _clkEpochSetMs = 0;
bool                   _clkEpochValid = false;
static bool            _onUsb       = false;
static void clockRefreshRtc() {
  if (millis() - _clkLastRead < 1000) return;
  _clkLastRead = millis();
  _onUsb = compat::usbPresent();
  if (_clkEpochValid) {
    time_t cur = _clkEpochLocal + (time_t)((millis() - _clkEpochSetMs) / 1000);
    struct tm lt;
    gmtime_r(&cur, &lt);
    _clkTm.hours = (int8_t)lt.tm_hour;
    _clkTm.minutes = (int8_t)lt.tm_min;
    _clkTm.seconds = (int8_t)lt.tm_sec;
    _clkDt.year = (int16_t)(lt.tm_year + 1900);
    _clkDt.month = (int8_t)(lt.tm_mon + 1);
    _clkDt.date = (int8_t)lt.tm_mday;
    _clkDt.weekDay = (int8_t)lt.tm_wday;
  } else {
    M5.Rtc.getTime(&_clkTm);
    M5.Rtc.getDate(&_clkDt);
  }
}

static uint8_t _orientFromAx(float ax) {
  return (ax >= 0) ? 3 : 1;
}

static void clockUpdateOrient() {
  clockOrient = 0;
  orientFrames = 0;
}

// Clock face: shown when charging on USB with nothing else going on.
// Portrait paints the upper ~110px to the sprite; pet renders below.
// Landscape draws direct to LCD with rotation — sprite stays untouched.
static const char* const MON[] = {
  "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
};
static const char* const DOW[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

static uint8_t clockDow() {
  int wd = _clkDt.weekDay;
  if (wd < 0 || wd > 6) return 0;
  return (uint8_t)wd;
}

static uint8_t _lcdRotForClock(uint8_t orient) {
  // StickS3 mounting is reversed from our logical 1/3 orientation labels.
  if (orient == 1) return 3;
  if (orient == 3) return 1;
  return orient;
}

static void drawClock() {
  const Palette& p = characterPalette();

  // RTC fields are signed 8-bit in M5Unified; sanitize before formatting so
  // transient invalid reads never render as odd values.
  int hh = _clkTm.hours;
  int mm = _clkTm.minutes;
  int sec = _clkTm.seconds;
  int month = _clkDt.month;
  int day = _clkDt.date;
  if (hh < 0 || hh > 23) hh = 0;
  if (mm < 0 || mm > 59) mm = 0;
  if (sec < 0 || sec > 59) sec = 0;
  if (month < 1 || month > 12) month = 1;
  if (day < 1 || day > 31) day = 1;

  uint8_t mi = (uint8_t)(month - 1);
  uint8_t wd = clockDow();

  char hm[6]; snprintf(hm, sizeof(hm), "%02d:%02d", hh, mm);
  char ss[4]; snprintf(ss, sizeof(ss), ":%02d", sec);
  char dl[12]; snprintf(dl, sizeof(dl), "%s %02d", MON[mi], day);

  int clockTop = uiLandscape() ? 0 : 140;
  if (uiLandscape()) {
    int panelX = landscapeSidePanelX();
    int panelW = landscapeSidePanelW();
    spr.fillRect(panelX, 0, panelW, H, p.bg);
  } else {
    spr.fillRect(0, clockTop, W, H - clockTop, p.bg);
  }
  spr.setTextDatum(MC_DATUM);
  if (uiLandscape()) {
    int panelX = landscapeSidePanelX();
    int panelW = landscapeSidePanelW();
    char wdl[16]; snprintf(wdl, sizeof(wdl), "%s %02d", MON[mi], day);
    spr.setTextSize(1);
    spr.setTextColor(p.textDim, p.bg);
    spr.drawString("codex idle", panelX + panelW / 2, 26);
    spr.setTextSize(3); spr.setTextColor(p.text, p.bg);    spr.drawString(hm, panelX + panelW / 2, 58);
    spr.setTextSize(1); spr.setTextColor(p.textDim, p.bg); spr.drawString(ss, panelX + panelW / 2, 82);
    spr.setTextColor(p.body, p.bg);                         spr.drawString(wdl, panelX + panelW / 2, 104);
  } else {
    spr.setTextSize(4); spr.setTextColor(p.text, p.bg);    spr.drawString(hm, CX, 168);
    spr.setTextSize(2); spr.setTextColor(p.textDim, p.bg); spr.drawString(ss, CX, 198);
    spr.setTextSize(1);                                     spr.drawString(dl, CX, 220);
  }
  spr.setTextDatum(TL_DATUM);
}

PersonaState derive(const TamaState& s) {
  if (!s.connected)            return P_SLEEP;
  if (s.sessionsWaiting > 0)   return P_ATTENTION;
  if (s.recentlyCompleted)     return P_CELEBRATE;
  if (s.sessionsRunning >= 1)  return P_BUSY;
  return P_IDLE;   // connected, 0+ sessions, nothing urgent — hang out
}

void triggerOneShot(PersonaState s, uint32_t durMs) {
  activeState = s;
  oneShotUntil = millis() + durMs;
}

bool checkShake() {
  return false;
}




// Persistent screen-level title row ("INFO  n/3") matching the PET header,
// then a per-page section label below it. The fixed title is the cue that
// B cycles pages here just like it does on PET.
static void _infoHeader(const Palette& p, int& y, const char* section, uint8_t page) {
  int left = uiLandscape() ? 8 : 4;
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(left, y); spr.print("Info");
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - (uiLandscape() ? 32 : 28), y); spr.printf("%u/%u", page + 1, INFO_PAGES);
  y += uiLandscape() ? 10 : 12;
  spr.setTextColor(p.body, p.bg);
  spr.setCursor(left, y); spr.print(section);
  y += uiLandscape() ? 10 : 12;
}

void drawPasskey() {
  const Palette& p = characterPalette();
  spr.fillSprite(p.bg);
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(8, 56);  spr.print("BLUETOOTH PAIRING");
  spr.setCursor(8, 184); spr.print("enter on desktop:");
  spr.setTextSize(3);
  spr.setTextColor(p.text, p.bg);
  char b[8]; snprintf(b, sizeof(b), "%06lu", (unsigned long)blePasskey());
  spr.setCursor((W - 18 * 6) / 2, 110);
  spr.print(b);
}

void drawInfo() {
  const Palette& p = characterPalette();
  const int TOP = contentTop();
  bool showGif = uiLandscape() ? (infoPage == 0) : true;
  if (uiLandscape() && showGif) {
    clearLandscapePhotoSurround(p, TOP, H);
  } else {
    spr.fillRect(0, TOP, W, H - TOP, p.bg);
  }
  spr.setTextSize(1);
  int y = TOP + 2;
  auto ln = [&](const char* fmt, ...) {
    char b[32]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    spr.setCursor(uiLandscape() ? 8 : 4, y); spr.print(b); y += uiLandscape() ? 7 : 8;
  };

  if (infoPage == 0) {
    _infoHeader(p, y, "CODEX", infoPage);
    uint32_t age = (millis() - tama.lastUpdated) / 1000;
    if (uiLandscape()) {
      const int leftX = 150;
      const int rightX = 208;
      const int runX = 150;
      const int waitX = 208;
      spr.setTextColor(p.textDim, p.bg);
      spr.setTextDatum(TC_DATUM);
      spr.drawString("RUNNING", runX, y);
      spr.drawString("WAITING", waitX, y);
      spr.setTextDatum(TL_DATUM);
      y += 10;
      spr.setTextColor(p.text, p.bg);
      spr.setTextSize(3);
      spr.setTextDatum(TC_DATUM);
      spr.drawString(String(tama.sessionsRunning), runX, y);
      spr.drawString(String(tama.sessionsWaiting), waitX, y);
      spr.setTextDatum(TL_DATUM);
      spr.setTextSize(1);
      y += 28;

      auto kv = [&](int cx, int yy, const char* key, const char* val) {
        spr.setTextColor(p.textDim, p.bg);
        spr.setTextDatum(TC_DATUM);
        spr.drawString(key, cx, yy);
        spr.setTextColor(p.text, p.bg);
        spr.drawString(val, cx, yy + 8);
        spr.setTextDatum(TL_DATUM);
      };

      char ageBuf[12];
      snprintf(ageBuf, sizeof(ageBuf), "%lus", (unsigned long)age);
      y += 6;
      kv(leftX, y, "SESSIONS", String(tama.sessionsTotal).c_str());
      kv(rightX, y, "LINK", dataScenarioName());
      y += 20;
      kv(leftX, y, "LAST MSG", ageBuf);
      kv(rightX, y, "STATE", stateNames[activeState]);
      y += 21;
      spr.setTextColor(p.textDim, p.bg);
      spr.setTextDatum(TC_DATUM);
      spr.drawString("BLE", (leftX + rightX) / 2 - 28, y);
      spr.setTextColor(p.text, p.bg);
      spr.drawString(!bleConnected() ? "-" : bleSecure() ? "encrypted" : "OPEN", (leftX + rightX) / 2 + 24, y);
      spr.setTextDatum(TL_DATUM);
    } else {
      const int LEFT_X = 17;
      const int RUN_CENTER_X = 38;
      const int WAIT_CENTER_X = 96;
      const int WAIT_LABEL_X = WAIT_CENTER_X - 21;

      spr.setTextColor(p.textDim, p.bg);
      spr.setCursor(LEFT_X, y);
      spr.print("RUNNING");
      spr.setCursor(WAIT_LABEL_X, y);
      spr.print("WAITING");
      y += 10;

      spr.setTextColor(p.text, p.bg);
      spr.setTextSize(3);
      spr.setTextDatum(TC_DATUM);
      spr.drawString(String(tama.sessionsRunning), RUN_CENTER_X, y);
      spr.drawString(String(tama.sessionsWaiting), WAIT_CENTER_X, y);
      spr.setTextDatum(TL_DATUM);
      spr.setTextSize(1);
      y += 30;

      spr.setTextColor(p.textDim, p.bg);
      spr.setCursor(LEFT_X, y);
      spr.print("SESSIONS");
      spr.setTextColor(p.text, p.bg);
      spr.setTextDatum(TC_DATUM);
      spr.drawString(String(tama.sessionsTotal), WAIT_CENTER_X, y);
      spr.setTextDatum(TL_DATUM);
      y += 16;

      spr.setTextColor(p.textDim, p.bg);
      spr.setCursor(LEFT_X, y);
      spr.print("LINK");
      spr.setTextColor(p.text, p.bg);
      spr.setTextDatum(TC_DATUM);
      spr.drawString(dataScenarioName(), WAIT_CENTER_X, y);
      spr.setTextDatum(TL_DATUM);
      y += 14;

      spr.setTextColor(p.textDim, p.bg);
      spr.setCursor(LEFT_X, y);
      spr.print("BLE");
      spr.setTextColor(p.text, p.bg);
      spr.setTextDatum(TC_DATUM);
      spr.drawString(!bleConnected() ? "-" : bleSecure() ? "encrypted" : "OPEN", WAIT_CENTER_X, y);
      spr.setTextDatum(TL_DATUM);
      y += 14;

      spr.setTextColor(p.textDim, p.bg);
      spr.setCursor(LEFT_X, y);
      spr.print("LAST MSG");
      spr.setTextColor(p.text, p.bg);
      spr.setTextDatum(TC_DATUM);
      spr.drawString(String((unsigned long)age) + "s", WAIT_CENTER_X, y);
      spr.setTextDatum(TL_DATUM);
      y += 14;

      spr.setTextColor(p.textDim, p.bg);
      spr.setCursor(LEFT_X, y);
      spr.print("STATE");
      spr.setTextColor(p.text, p.bg);
      spr.setTextDatum(TC_DATUM);
      spr.drawString(stateNames[activeState], WAIT_CENTER_X, y);
      spr.setTextDatum(TL_DATUM);
    }

  } else if (infoPage == 1) {
    _infoHeader(p, y, "ABOUT", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    if (uiLandscape()) {
      int textX = 8;
      int textW = W - textX - 8;
      y = TOP + 18;
      const int lh = 12;
      y += drawBodyWrappedParagraph("I watch your Codex desktop sessions.", textX, y, 4, textW, lh, p.textDim) * lh + 3;
      y += drawBodyWrappedParagraph("I sleep when nothing's happening, wake when you start working, get impatient when approvals pile up.", textX, y, 5, textW, lh, p.textDim) * lh + 3;
      spr.setTextColor(p.text, p.bg);
      y += drawBodyWrappedParagraph("Press A on a prompt to approve from here.", textX, y, 4, textW, lh, p.text) * lh + 3;
      spr.setTextColor(p.textDim, p.bg);
      drawBodyWrappedParagraph("18 species. Settings > ascii pet to cycle.", textX, y, 3, textW, lh, p.textDim);
    } else {
      ln("I watch your Codex");
      ln("desktop sessions.");
      y += 6;
      ln("I sleep when nothing's");
      ln("happening, wake when");
      ln("you start working,");
      ln("get impatient when");
      ln("approvals pile up.");
      y += 6;
      spr.setTextColor(p.text, p.bg);
      ln("Press A on a prompt");
      ln("to approve from here.");
      y += 6;
      spr.setTextColor(p.textDim, p.bg);
      ln("18 species. Settings");
      ln("> ascii pet to cycle.");
    }

  } else if (infoPage == 2) {
    _infoHeader(p, y, "BUTTONS", infoPage);
    if (uiLandscape()) {
      int textX = 8;
      int textW = W - textX - 8;
      y = TOP + 18;
      const int lh = 12;
      spr.setTextColor(p.text, p.bg);
      y += drawBodyWrappedParagraph("A   front", textX, y, 1, textW, lh, p.text) * lh;
      spr.setTextColor(p.textDim, p.bg);
      y += drawBodyWrappedParagraph("next screen   approve prompt", textX + 8, y, 3, textW - 8, lh, p.textDim) * lh + 3;
      spr.setTextColor(p.text, p.bg);
      y += drawBodyWrappedParagraph("B   right side", textX, y, 1, textW, lh, p.text) * lh;
      spr.setTextColor(p.textDim, p.bg);
      y += drawBodyWrappedParagraph("next page   deny prompt", textX + 8, y, 3, textW - 8, lh, p.textDim) * lh + 3;
      spr.setTextColor(p.text, p.bg);
      y += drawBodyWrappedParagraph("hold A", textX, y, 1, textW, lh, p.text) * lh;
      spr.setTextColor(p.textDim, p.bg);
      y += drawBodyWrappedParagraph("menu", textX + 8, y, 2, textW - 8, lh, p.textDim) * lh + 3;
      spr.setTextColor(p.text, p.bg);
      y += drawBodyWrappedParagraph("Power  left side", textX, y, 1, textW, lh, p.text) * lh;
      spr.setTextColor(p.textDim, p.bg);
      drawBodyWrappedParagraph("tap = screen off   hold 6s = off", textX + 8, y, 3, textW - 8, lh, p.textDim);
    } else {
      spr.setTextColor(p.text, p.bg);    ln("A   front");
      spr.setTextColor(p.textDim, p.bg); ln("    next screen");
      ln("    approve prompt"); y += 4;
      spr.setTextColor(p.text, p.bg);    ln("B   right side");
      spr.setTextColor(p.textDim, p.bg); ln("    next page");
      ln("    deny prompt"); y += 4;
      spr.setTextColor(p.text, p.bg);    ln("hold A");
      spr.setTextColor(p.textDim, p.bg); ln("    menu"); y += 4;
      spr.setTextColor(p.text, p.bg);    ln("Power  left side");
      spr.setTextColor(p.textDim, p.bg); ln("    tap = screen off");
      ln("    hold 6s = off");
    }

  } else if (infoPage == 3) {
    _infoHeader(p, y, "DEVICE", infoPage);

    int vBat_mV = batteryUi.filteredBatMv;
    int iBat_mA = batteryUi.filteredCurrentMa;
    int vBus_mV = batteryUi.filteredVbusMv;
    int pct = (vBat_mV - 3200) / 10;   // (v-3.2)/(4.2-3.2)*100 = (v-3.2)*100 = (mv-3200)/10
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    bool usb = vBus_mV > 4000;
    bool charging = usb && iBat_mA > 1;
    bool full = usb && vBat_mV > 4100 && iBat_mA < 10;

    if (uiLandscape()) {
      const int leftX = 10;
      const int rightX = 126;
      spr.setTextColor(p.text, p.bg);
      spr.setTextSize(2);
      spr.setCursor(leftX, y);
      spr.printf("%d%%", pct);
      spr.setTextSize(1);
      spr.setTextColor(full ? GREEN : (charging ? HOT : p.textDim), p.bg);
      spr.setCursor(leftX + 48, y + 4);
      spr.print(full ? "full" : (charging ? "charging" : (usb ? "usb" : "battery")));

      auto kv = [&](int xx, int yy, const char* key, const char* val) {
        spr.setTextColor(p.textDim, p.bg);
        spr.drawString(key, xx, yy);
        spr.setTextColor(p.text, p.bg);
        spr.drawString(val, xx, yy + 9);
      };
      char batBuf[16], curBuf[16], usbBuf[16], upBuf[16], heapBuf[16], tempBuf[16];
      snprintf(batBuf, sizeof(batBuf), "%d.%02dV", vBat_mV/1000, (vBat_mV%1000)/10);
      snprintf(curBuf, sizeof(curBuf), "%+dmA", iBat_mA);
      snprintf(usbBuf, sizeof(usbBuf), "%d.%02dV", vBus_mV/1000, (vBus_mV%1000)/10);
      uint32_t up = millis() / 1000;
      snprintf(upBuf, sizeof(upBuf), "%luh %02lum", up / 3600, (up / 60) % 60);
      snprintf(heapBuf, sizeof(heapBuf), "%uKB", ESP.getFreeHeap() / 1024);
      snprintf(tempBuf, sizeof(tempBuf), "%dC", compat::chipTempC());
      int col1 = 10, col2 = 86, col3 = 162;
      y += 18;
      kv(col1, y, "BATTERY", batBuf);
      kv(col2, y, "CURRENT", curBuf);
      kv(col3, y, usb ? "USB IN" : "UPTIME", usb ? usbBuf : upBuf);
      y += 22;
      kv(col1, y, usb ? "UPTIME" : "HEAP", usb ? upBuf : heapBuf);
      kv(col2, y, "OWNER", ownerName()[0] ? ownerName() : "-");
      kv(col3, y, "BRIGHT", String(brightLevel).c_str());
      y += 22;
      kv(col1, y, "BT", settings().bt ? (dataBtActive() ? "linked" : "on") : "off");
      kv(col2, y, "TEMP", tempBuf);
    } else {
      spr.setTextColor(p.text, p.bg);
      spr.setTextSize(2);
      spr.setCursor(4, y);
      spr.printf("%d%%", pct);
      spr.setTextSize(1);
      spr.setTextColor(full ? GREEN : (charging ? HOT : p.textDim), p.bg);
      spr.setCursor(60, y + 4);
      spr.print(full ? "full" : (charging ? "charging" : (usb ? "usb" : "battery")));
      y += 20;

      spr.setTextColor(p.textDim, p.bg);
      ln("  battery  %d.%02dV", vBat_mV/1000, (vBat_mV%1000)/10);
      ln("  current  %+dmA", iBat_mA);
      if (usb) ln("  usb in   %d.%02dV", vBus_mV/1000, (vBus_mV%1000)/10);
      y += 8;

      spr.setTextColor(p.text, p.bg);
      ln("SYSTEM");
      spr.setTextColor(p.textDim, p.bg);
      if (ownerName()[0]) ln("  owner    %s", ownerName());
      uint32_t up = millis() / 1000;
      ln("  uptime   %luh %02lum", up / 3600, (up / 60) % 60);
      ln("  heap     %uKB", ESP.getFreeHeap() / 1024);
      ln("  bright   %u/4", brightLevel);
      ln("  bt       %s", settings().bt ? (dataBtActive() ? "linked" : "on") : "off");
      ln("  temp     %dC", compat::chipTempC());
    }

  } else if (infoPage == 4) {
    _infoHeader(p, y, "BLUETOOTH", infoPage);
    bool linked = settings().bt && dataBtActive();

    spr.setTextColor(linked ? GREEN : (settings().bt ? HOT : p.textDim), p.bg);
    spr.setTextSize(2);
    spr.setCursor(4, y);
    spr.print(linked ? "linked" : (settings().bt ? "discover" : "off"));
    spr.setTextSize(1);
    y += 20;

    spr.setTextColor(p.textDim, p.bg);
    spr.setTextColor(p.text, p.bg);
    ln("  %s", btName);
    spr.setTextColor(p.textDim, p.bg);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    ln("  %02X:%02X:%02X:%02X:%02X:%02X",
       mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    y += 8;

    if (linked) {
      uint32_t age = (millis() - tama.lastUpdated) / 1000;
      ln("  last msg  %lus", (unsigned long)age);
    } else if (settings().bt) {
      spr.setTextColor(p.text, p.bg);
      ln("TO PAIR");
      spr.setTextColor(p.textDim, p.bg);
      ln(" Open Codex desktop");
      ln(" > Developer");
      ln(" > Hardware Buddy");
      y += 4;
      ln(" auto-connects via BLE");
    }

  } else {
    _infoHeader(p, y, "CREDITS", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("made by");
    y += 4;
    spr.setTextColor(p.text, p.bg);
    ln("Felix Rieseberg");
    y += 8;
    spr.setTextColor(p.textDim, p.bg);
    ln("mod by");
    y += 4;
    spr.setTextColor(p.text, p.bg);
    ln("arnoldn");
    y += 12;
    spr.setTextColor(p.textDim, p.bg);
    ln("source");
    y += 4;
    spr.setTextColor(p.text, p.bg);
    if (uiLandscape()) {
      ln("cd /home/arnold/Projects/codex-buddy");
      ln("claude-desktop-buddy-s3");
      ln("./.venv/bin/python3 -m");
      ln("platformio run -t upload");
    } else {
      ln("cd /home/arnold/Projects");
      ln("/codex-buddy/claude-des");
      ln("ktop-buddy-s3");
      ln("./.venv/bin/python3 -m");
      ln(" platformio run -t");
      ln(" upload");
    }
    y += 12;
    spr.setTextColor(p.textDim, p.bg);
    ln("hardware");
    y += 4;
    ln("M5StickC Plus S3");
    ln("ESP32-S3 + BMI270");
  }
}


// Greedy word-wrap into fixed-width rows. Continuation rows get a leading
// space. Returns number of rows written.
static uint8_t wrapInto(const char* in, char out[][24], uint8_t maxRows, uint8_t width) {
  uint8_t row = 0, col = 0;
  const char* p = in;
  while (*p && row < maxRows) {
    while (*p == ' ') p++;                     // skip leading spaces
    // measure next word
    const char* w = p;
    while (*p && *p != ' ') p++;
    uint8_t wlen = p - w;
    if (wlen == 0) break;
    uint8_t need = (col > 0 ? 1 : 0) + wlen;
    if (col + need > width) {
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;              // continuation indent
    }
    if (col > 1 || (col == 1 && out[row][0] != ' ')) out[row][col++] = ' ';
    else if (col == 1 && row > 0) {}           // already have the indent space
    // hard-break words that still don't fit
    while (wlen > width - col) {
      uint8_t take = width - col;
      memcpy(&out[row][col], w, take); col += take; w += take; wlen -= take;
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;
    }
    memcpy(&out[row][col], w, wlen); col += wlen;
  }
  if (col > 0 && row < maxRows) { out[row][col] = 0; row++; }
  return row;
}

static uint8_t wrapIntoCenteredWords(const char* in, char out[][24], uint8_t maxRows, int maxPx) {
  if (!in || !*in || maxRows == 0) return 0;

  uint8_t row = 0;
  out[row][0] = 0;
  const char* p = in;

  while (*p && row < maxRows) {
    while (*p == ' ') p++;
    if (!*p) break;

    const char* w = p;
    while (*p && *p != ' ') p++;
    size_t wlen = (size_t)(p - w);
    if (wlen == 0) break;
    if (wlen > 23) wlen = 23;

    char word[24];
    memcpy(word, w, wlen);
    word[wlen] = 0;

    char candidate[24];
    if (out[row][0]) snprintf(candidate, sizeof(candidate), "%s %s", out[row], word);
    else snprintf(candidate, sizeof(candidate), "%s", word);

    if (spr.textWidth(candidate) <= maxPx) {
      snprintf(out[row], 24, "%s", candidate);
      continue;
    }

    if (out[row][0] == 0) {
      // A single overlong token cannot be wrapped without splitting.
      // Leave it intact on its own line and let the caller decide whether
      // to shorten upstream content.
      snprintf(out[row], 24, "%s", word);
    }

    if (++row >= maxRows) return row;
    out[row][0] = 0;
    snprintf(out[row], 24, "%s", word);
  }

  return out[row][0] ? row + 1 : row;
}

static uint8_t wrapIntoBodyWords(const char* in, char out[][96], uint8_t maxRows, int maxPx) {
  if (!in || !*in || maxRows == 0) return 0;

  uint8_t row = 0;
  out[row][0] = 0;
  const char* p = in;

  while (*p && row < maxRows) {
    while (*p == ' ') p++;
    if (!*p) break;

    const char* w = p;
    while (*p && *p != ' ') p++;
    size_t wlen = (size_t)(p - w);
    if (wlen == 0) break;
    if (wlen > 95) wlen = 95;

    char word[96];
    memcpy(word, w, wlen);
    word[wlen] = 0;

    char candidate[96];
    if (out[row][0]) snprintf(candidate, sizeof(candidate), "%s %s", out[row], word);
    else snprintf(candidate, sizeof(candidate), "%s", word);

    if (spr.textWidth(candidate) <= maxPx) {
      snprintf(out[row], sizeof(out[row]), "%s", candidate);
      continue;
    }

    if (out[row][0] == 0) {
      snprintf(out[row], sizeof(out[row]), "%s", word);
    }

    if (++row >= maxRows) return row;
    out[row][0] = 0;
    snprintf(out[row], sizeof(out[row]), "%s", word);
  }

  return out[row][0] ? row + 1 : row;
}

static void drawApproval() {
  const Palette& p = characterPalette();
  const bool land = uiLandscape();
  const int AREA = land ? (H - 12) : 78;
  const int x = land ? landscapeSidePanelX() : 0;
  const int y = land ? 6 : (H - AREA);
  const int w = land ? landscapeSidePanelW() : W;
  spr.fillRect(x, y, w, AREA, p.bg);
  if (land) spr.drawRoundRect(x, y, w, AREA, 4, p.textDim);
  else spr.drawFastHLine(0, y, W, p.textDim);

  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(x + 4, y + 4);
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  if (waited >= 10) spr.setTextColor(HOT, p.bg);
  spr.printf("approve? %lus", (unsigned long)waited);

  int toolLen = strlen(tama.promptTool);
  spr.setTextColor(p.text, p.bg);
  if (land) {
    spr.setTextSize(1);
    drawCenteredWrappedBlock(tama.promptTool, y + 24, 2, p.text, w - 12, 12);
    spr.setTextColor(p.textDim, p.bg);
    drawCenteredWrappedBlock(tama.promptHint, y + 50, 3, p.textDim, w - 12, 11);
  } else {
    spr.setTextSize(toolLen <= 10 ? 2 : 1);
    spr.setCursor(4, y + (toolLen <= 10 ? 14 : 18));
    spr.print(tama.promptTool);
    spr.setTextSize(1);

    spr.setTextColor(p.textDim, p.bg);
    int hlen = strlen(tama.promptHint);
    spr.setCursor(4, y + 34);
    spr.printf("%.21s", tama.promptHint);
    if (hlen > 21) {
      spr.setCursor(4, y + 42);
      spr.printf("%.21s", tama.promptHint + 21);
    }
  }

  if (responseSent) {
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(x + 4, y + AREA - 12);
    spr.print("sent...");
  } else {
    spr.setTextColor(GREEN, p.bg);
    spr.setCursor(x + 4, y + AREA - 12);
    spr.print("A: approve");
    spr.setTextColor(HOT, p.bg);
    spr.setCursor(x + w - 48, y + AREA - 12);
    spr.print("B: deny");
  }
}

static uint8_t utf8CharLen(uint8_t lead) {
  if ((lead & 0x80) == 0x00) return 1;
  if ((lead & 0xE0) == 0xC0) return 2;
  if ((lead & 0xF0) == 0xE0) return 3;
  if ((lead & 0xF8) == 0xF0) return 4;
  return 1;
}

static bool containsNonAscii(const char* text) {
  if (!text) return false;
  for (const uint8_t* p = (const uint8_t*)text; *p; ++p) {
    if (*p & 0x80) return true;
  }
  return false;
}

static const lgfx::IFont* interactiveTextFont(const char* text) {
  if (containsNonAscii(text)) return &fonts::efontCN_16;
  return &fonts::Font2;
}

static void restoreDefaultUiFont() {
  spr.setFont(&fonts::Font0);
  spr.setTextSize(1);
}

static uint8_t wrapIntoCenteredUtf8(const char* in, char out[][96], uint8_t maxRows, int maxPx) {
  if (!in || !*in || maxRows == 0) return 0;

  uint8_t row = 0;
  uint8_t colBytes = 0;
  out[row][0] = 0;
  const uint8_t* p = (const uint8_t*)in;

  while (*p && row < maxRows) {
    if (*p == '\n' || *p == '\r') {
      if (colBytes > 0) {
        out[row][colBytes] = 0;
        if (++row >= maxRows) return row;
        out[row][0] = 0;
        colBytes = 0;
      }
      ++p;
      continue;
    }

    if (colBytes == 0) {
      while (*p == ' ') ++p;
      if (!*p) break;
    }

    uint8_t len = utf8CharLen(*p);
    char glyph[8] = {0};
    for (uint8_t i = 0; i < len && p[i]; i++) glyph[i] = (char)p[i];

    char candidate[96];
    memcpy(candidate, out[row], colBytes);
    memcpy(candidate + colBytes, glyph, len);
    candidate[colBytes + len] = 0;

    if (colBytes > 0 && spr.textWidth(candidate) > maxPx) {
      out[row][colBytes] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = 0;
      colBytes = 0;
      continue;
    }

    memcpy(out[row] + colBytes, glyph, len);
    colBytes += len;
    out[row][colBytes] = 0;
    p += len;
  }

  return out[row][0] ? row + 1 : row;
}

static uint8_t drawCenteredWrappedBlock(const char* text, int topY, int maxRows, uint16_t color, int maxPx, int lineHeight) {
  char lines[12][96] = {{0}};
  uint8_t rows = 0;
  spr.setFont(interactiveTextFont(text));
  spr.setTextSize(1);
  if (containsNonAscii(text)) {
    rows = wrapIntoCenteredUtf8(text ? text : "", lines, maxRows, maxPx);
  } else {
    char asciiLines[12][24] = {{0}};
    rows = wrapIntoCenteredWords(text ? text : "", asciiLines, maxRows, maxPx);
    for (uint8_t i = 0; i < rows; i++) snprintf(lines[i], sizeof(lines[i]), "%s", asciiLines[i]);
  }
  spr.setTextColor(color, characterPalette().bg);
  for (uint8_t i = 0; i < rows; i++) spr.drawString(lines[i], W / 2, topY + i * lineHeight);
  restoreDefaultUiFont();
  return rows;
}

static uint8_t drawLandscapeWrappedStatus(const char* text, char lines[][24], uint8_t maxRows, int maxPx) {
  spr.setFont(&fonts::Font0);
  spr.setTextSize(2);
  return wrapIntoCenteredWords(text ? text : "", lines, maxRows, maxPx);
}

static int paragraphLineHeight(const char* text, int baseLineHeight) {
  if (containsNonAscii(text)) return baseLineHeight < 18 ? 18 : baseLineHeight;
  return baseLineHeight < 12 ? 12 : baseLineHeight;
}

static uint8_t drawBodyWrappedParagraph(const char* text, int x, int y, int maxRows, int maxPx, int lineHeight, uint16_t color) {
  char lines[12][96] = {{0}};
  spr.setFont(&fonts::Font0);
  spr.setTextSize(1);
  uint8_t rows = wrapIntoBodyWords(text ? text : "", lines, maxRows, maxPx);
  spr.setTextColor(color, characterPalette().bg);
  for (uint8_t i = 0; i < rows; i++) spr.drawString(lines[i], x, y + i * lineHeight);
  restoreDefaultUiFont();
  return rows;
}

static uint8_t drawWrappedParagraph(const char* text, int x, int y, int maxRows, int maxPx, int lineHeight, uint16_t color) {
  char lines[12][96] = {{0}};
  uint8_t rows = 0;
  spr.setFont(interactiveTextFont(text));
  spr.setTextSize(1);
  if (containsNonAscii(text)) {
    rows = wrapIntoCenteredUtf8(text ? text : "", lines, maxRows, maxPx);
  } else {
    char asciiLines[12][24] = {{0}};
    rows = wrapIntoCenteredWords(text ? text : "", asciiLines, maxRows, maxPx);
    for (uint8_t i = 0; i < rows; i++) snprintf(lines[i], sizeof(lines[i]), "%s", asciiLines[i]);
  }
  spr.setTextColor(color, characterPalette().bg);
  for (uint8_t i = 0; i < rows; i++) spr.drawString(lines[i], x, y + i * lineHeight);
  restoreDefaultUiFont();
  return rows;
}

static void drawLandscapeWrappedStatusBlock(const char* text, int centerX, int topY, uint16_t color) {
  char lines[6][24] = {{0}};
  uint8_t rows = drawLandscapeWrappedStatus(text, lines, 6, landscapeSidePanelW() - 14);
  spr.setTextColor(color, characterPalette().bg);
  for (uint8_t i = 0; i < rows; i++) spr.drawString(lines[i], centerX, topY + i * 18);
  spr.setTextSize(1);
}

static void drawInteractive() {
  const Palette& p = characterPalette();
  const bool land = uiLandscape();
  const int AREA = land ? (H - 12) : 94;
  const int x = land ? landscapeSidePanelX() : 0;
  const int y = land ? 6 : (H - AREA);
  const int w = land ? landscapeSidePanelW() : W;
  spr.fillRect(x, y, w, AREA, p.bg);
  if (land) spr.drawRoundRect(x, y, w, AREA, 4, p.textDim);
  else spr.drawFastHLine(0, y, W, p.textDim);

  spr.setTextDatum(TL_DATUM);
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(x + 4, y + 4);
  spr.print("input needed");

  spr.setTextDatum(MC_DATUM);
  spr.setTextSize(2);
  spr.setTextColor(p.text, p.bg);
  spr.drawString("extra input", x + w / 2, y + 24);

  spr.setTextSize(1);
  spr.setTextColor(p.body, p.bg);
  spr.drawString("Continue on desktop", x + w / 2, y + 46);
  spr.setTextColor(p.textDim, p.bg);
  spr.drawString("codex is waiting for you", x + w / 2, y + AREA - 18);
  spr.setTextDatum(TL_DATUM);
}

static void tinyHeart(int x, int y, bool filled, uint16_t col) {
  if (filled) {
    spr.fillCircle(x - 2, y, 2, col);
    spr.fillCircle(x + 2, y, 2, col);
    spr.fillTriangle(x - 4, y + 1, x + 4, y + 1, x, y + 5, col);
  } else {
    spr.drawCircle(x - 2, y, 2, col);
    spr.drawCircle(x + 2, y, 2, col);
    spr.drawLine(x - 4, y + 1, x, y + 5, col);
    spr.drawLine(x + 4, y + 1, x, y + 5, col);
  }
}

static void formatCompactValue(uint32_t value, char* out, size_t outLen) {
  if (value >= 1000000UL) snprintf(out, outLen, "%lu.%luM", value / 1000000UL, (value / 100000UL) % 10UL);
  else if (value >= 1000UL) snprintf(out, outLen, "%lu.%luK", value / 1000UL, (value / 100UL) % 10UL);
  else snprintf(out, outLen, "%lu", value);
}

static void drawUsageBar(int x, int y, int w, int h, uint8_t pct, uint16_t fill, uint16_t border) {
  spr.drawRoundRect(x, y, w, h, 4, border);
  spr.fillRoundRect(x + 1, y + 1, w - 2, h - 2, 3, TFT_BLACK);
  int inner = w - 4;
  int fillW = (inner * pct) / 100;
  if (fillW > 0) spr.fillRoundRect(x + 2, y + 2, fillW, h - 4, 2, fill);
}

static void drawUsageCard(
  const Palette& p, int x, int y, int w, const char* label, const char* sublabel,
  uint8_t pct, uint16_t accent
) {
  spr.setTextDatum(TL_DATUM);
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  char titleBuf[24];
  snprintf(titleBuf, sizeof(titleBuf), "%s %s", label, sublabel);
  spr.drawString(titleBuf, x, y);

  spr.setTextDatum(TR_DATUM);
  spr.setTextColor(accent, p.bg);
  char pctBuf[8];
  snprintf(pctBuf, sizeof(pctBuf), "%u%%", pct);
  spr.drawString(pctBuf, x + w, y);

  drawUsageBar(x, y + 12, w, 9, pct, accent, p.textDim);
}

static void formatResetTime(uint32_t resetEpoch, char* out, size_t outLen) {
  if (resetEpoch == 0) {
    snprintf(out, outLen, "--");
    return;
  }
  time_t t = (time_t)resetEpoch;
  struct tm lt;
  gmtime_r(&t, &lt);
  snprintf(out, outLen, "%02d/%02d %02d:%02d", lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min);
}

static void drawPetStats(const Palette& p) {
  const int TOP = contentTop();
  bool showGif = uiLandscape() ? (petPage == 0) : true;
  if (uiLandscape() && showGif) {
    clearLandscapePhotoSurround(p, TOP, H);
  } else {
    spr.fillRect(0, TOP, W, H - TOP, p.bg);
  }
  spr.setTextDatum(TL_DATUM);
  spr.setTextSize(1);

  if (uiLandscape()) {
    const int leftX = 8;
    const int levelX = 8;
    const int rightX = 130;
    const int rightW = W - rightX - 8;
    int y = TOP + 8;

    uint8_t mood = statsMoodTier();
    uint16_t moodCol = (mood >= 3) ? RED : (mood >= 2) ? HOT : p.textDim;
    spr.setTextColor(p.textDim, p.bg);
    int moodY = 100;
    spr.drawString("mood", leftX, moodY);
    for (int i = 0; i < 4; i++) tinyHeart(leftX + 34 + i * 11, moodY + 3, i < mood, moodCol);

    int levelY = 118;
    spr.fillRoundRect(levelX, levelY - 2, 34, 14, 3, p.body);
    spr.setTextColor(p.bg, p.body);
    spr.setTextDatum(MC_DATUM);
    char levelBuf[12];
    snprintf(levelBuf, sizeof(levelBuf), "Lv.%u", stats().level);
    spr.drawString(levelBuf, levelX + 17, levelY + 4);
    spr.setTextDatum(TL_DATUM);
    spr.setTextColor(p.textDim, p.bg);
    uint8_t fed = statsFedProgress();
    uint8_t fedDots = (uint8_t)((fed * 7 + 9) / 10);
    for (int i = 0; i < 7; i++) {
      int px = 48 + i * 7;
      if (i < fedDots) spr.fillCircle(px, levelY + 4, 2, p.body);
      else spr.drawCircle(px, levelY + 4, 2, p.textDim);
    }

    y = 32;
    uint16_t usageCol5h2 = (tama.usage5hRemaining < 20) ? HOT : p.body;
    drawUsageCard(p, rightX, y - 2, rightW, "usage", "5h", tama.usage5hRemaining, usageCol5h2);

    y += 24;
    uint16_t usageCol1w = (tama.usageWeekRemaining < 20) ? HOT : p.body;
    drawUsageCard(p, rightX, y - 2, rightW, "usage", "1w", tama.usageWeekRemaining, usageCol1w);

    const int statsY = 78;
    const int statsLeft = 136;
    const int statsRight = rightX + rightW;
    const int rowGap = 10;
    char totalBuf[16], todayBuf[16], decisionBuf[16], napBuf[16], weekReset[20];
    formatCompactValue(stats().tokens, totalBuf, sizeof(totalBuf));
    formatCompactValue(tama.tokensToday, todayBuf, sizeof(todayBuf));
    snprintf(decisionBuf, sizeof(decisionBuf), "%u/%u", stats().approvals, stats().denials);
    uint32_t nap = stats().napSeconds;
    snprintf(napBuf, sizeof(napBuf), "%luh%02lum", nap / 3600UL, (nap / 60UL) % 60UL);
    formatResetTime(tama.usageWeekResetAt, weekReset, sizeof(weekReset));

    spr.setTextDatum(TL_DATUM);
    spr.setTextColor(p.textDim, p.bg);
    spr.drawString("total", statsLeft, statsY + rowGap * 0);
    spr.drawString("today", statsLeft, statsY + rowGap * 1);
    spr.drawString("yes/no", statsLeft, statsY + rowGap * 2);
    spr.drawString("napped", statsLeft, statsY + rowGap * 3);
    spr.drawString("refresh", statsLeft, statsY + rowGap * 4);
    spr.setTextColor(p.text, p.bg);
    spr.setTextDatum(TR_DATUM);
    spr.drawString(totalBuf, statsRight, statsY + rowGap * 0);
    spr.drawString(todayBuf, statsRight, statsY + rowGap * 1);
    spr.drawString(decisionBuf, statsRight, statsY + rowGap * 2);
    spr.drawString(napBuf, statsRight, statsY + rowGap * 3);
    spr.drawString(weekReset, statsRight, statsY + rowGap * 4);
    spr.setTextDatum(TL_DATUM);
  } else {
    const int cardX = 6;
    const int cardW = W - 12;
    int y = TOP + 16;

    uint8_t mood = statsMoodTier();
    uint16_t moodCol = (mood >= 3) ? RED : (mood >= 2) ? HOT : p.textDim;
    spr.setTextDatum(TL_DATUM);
    spr.setTextColor(p.textDim, p.bg);
    spr.drawString("mood", cardX, y);
    for (int i = 0; i < 4; i++) tinyHeart(cardX + 40 + i * 15, y + 3, i < mood, moodCol);

    y += 16;
    uint16_t usageCol5h = (tama.usage5hRemaining < 20) ? HOT : p.body;
    drawUsageCard(
      p, cardX, y, cardW, "usage", "5h",
      tama.usage5hRemaining, usageCol5h
    );

    y += 28;
    uint16_t usageCol1w = (tama.usageWeekRemaining < 20) ? HOT : p.body;
    drawUsageCard(
      p, cardX, y, cardW, "usage", "1w",
      tama.usageWeekRemaining, usageCol1w
    );

    y += 30;
    spr.fillRoundRect(6, y - 2, 48, 14, 3, p.body);
    spr.setTextColor(p.bg, p.body);
    spr.setTextDatum(MC_DATUM);
    char levelBuf[12];
    snprintf(levelBuf, sizeof(levelBuf), "Lv.%u", stats().level);
    spr.drawString(levelBuf, 30, y + 4);
    spr.setTextDatum(TL_DATUM);
    spr.setTextColor(p.textDim, p.bg);
    uint8_t fed = statsFedProgress();
    for (int i = 0; i < 10; i++) {
      int px = 72 + i * 6;
      if (i < fed) spr.fillCircle(px, y + 4, 2, p.body);
      else spr.drawCircle(px, y + 4, 2, p.textDim);
    }

    y += 20;
    spr.setTextDatum(TL_DATUM);
    const int leftX = 18;
    const int rightX = 76;
    spr.setTextColor(p.textDim, p.bg);
    spr.drawString("total", leftX, y);
    spr.drawString("today", rightX, y);
    spr.drawString("yes/no", leftX, y + 22);
    spr.drawString("napped", rightX, y + 22);

    char totalBuf[16];
    char todayBuf[16];
    formatCompactValue(stats().tokens, totalBuf, sizeof(totalBuf));
    formatCompactValue(tama.tokensToday, todayBuf, sizeof(todayBuf));
    spr.setTextColor(p.text, p.bg);
    spr.drawString(totalBuf, leftX, y + 10);
    spr.drawString(todayBuf, rightX, y + 10);
    char decisionBuf[16];
    snprintf(decisionBuf, sizeof(decisionBuf), "%u/%u", stats().approvals, stats().denials);
    spr.drawString(decisionBuf, leftX, y + 32);
    uint32_t nap = stats().napSeconds;
    char napBuf[16];
    snprintf(napBuf, sizeof(napBuf), "%luh%02lum", nap / 3600UL, (nap / 60UL) % 60UL);
    spr.drawString(napBuf, rightX, y + 32);

    y += 44;
    char weekReset[20];
    formatResetTime(tama.usageWeekResetAt, weekReset, sizeof(weekReset));
    spr.setTextColor(p.textDim, p.bg);
    spr.drawString("refresh", leftX, y);
    spr.setTextDatum(TL_DATUM);
    spr.setTextColor(p.text, p.bg);
    spr.drawString(weekReset, 64, y);
    spr.setTextDatum(TL_DATUM);
  }
}

static void drawPetHowTo(const Palette& p) {
  const int TOP = contentTop();
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);
  int y = TOP + 2;
  auto ln = [&](uint16_t c, const char* s) {
    spr.setTextColor(c, p.bg); spr.setCursor(6, y); spr.print(s); y += 9;
  };
  auto gap = [&]() { y += 4; };

  y += uiLandscape() ? 8 : 12;  // room for the PET header drawn by drawPet()
  if (uiLandscape()) {
    int textX = 8;
    int textW = W - textX - 8;
    y = TOP + 18;
    const int lh = 12;
    y += drawBodyWrappedParagraph("MOOD", textX, y, 1, textW, lh, p.body) * lh;
    y += drawBodyWrappedParagraph("approve fast = up   deny lots = down", textX + 8, y, 3, textW - 8, lh, p.textDim) * lh + 4;

    y += drawBodyWrappedParagraph("USAGE", textX, y, 1, textW, lh, p.body) * lh;
    y += drawBodyWrappedParagraph("5h = codex left   week = quota left", textX + 8, y, 3, textW - 8, lh, p.textDim) * lh + 4;

    y += drawBodyWrappedParagraph("LEVEL", textX, y, 1, textW, lh, p.body) * lh;
    y += drawBodyWrappedParagraph("50K tokens = level up + confetti", textX + 8, y, 3, textW - 8, lh, p.textDim) * lh + 4;

    y += drawBodyWrappedParagraph("idle 30s = off   any button = wake", textX, y, 3, textW, lh, p.textDim) * lh + 4;
    drawBodyWrappedParagraph("A: screens   B: page   hold A: menu", textX, y, 3, textW, lh, p.textDim);
  } else {
    ln(p.body,    "MOOD");
    ln(p.textDim, " approve fast = up");
    ln(p.textDim, " deny lots = down"); gap();

    ln(p.body,    "USAGE");
    ln(p.textDim, " 5h = codex left");
    ln(p.textDim, " week = quota left"); gap();

    ln(p.body,    "LEVEL");
    ln(p.textDim, " 50K tokens =");
    ln(p.textDim, " level up + confetti"); gap();

    ln(p.textDim, "idle 30s = off");
    ln(p.textDim, "any button = wake"); gap();

    ln(p.textDim, "A: screens  B: page");
    ln(p.textDim, "hold A: menu");
  }
}

void drawPet() {
  const Palette& p = characterPalette();
  int y = contentTop();

  if (petPage == 0) drawPetStats(p);
  else drawPetHowTo(p);

  // Header on top of whichever page drew — title left, counter right
  spr.setTextSize(1);
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(4, y + 2);
  if (ownerName()[0]) {
    spr.printf("%s's %s", ownerName(), petName());
  } else {
    spr.print(petName());
  }
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - 28, y + 2);
  spr.printf("%u/%u", petPage + 1, PET_PAGES);
}

static bool promptActive() {
  return tama.promptId[0] && !responseSent;
}

void drawHUD() {
  if (promptActive()) { drawApproval(); return; }
  if (interactiveActive()) { drawInteractive(); return; }
  const Palette& p = characterPalette();
  const int SHOW = 2, LH = 16, MAX_PX = 118;
  const bool land = uiLandscape();
  const int AREA = land ? (H - 12) : (SHOW * LH + 10);
  const int TOP = land ? 6 : 170;
  const int X = land ? landscapeSidePanelX() : 0;
  const int panelW = land ? landscapeSidePanelW() : W;
  spr.fillRect(X, TOP, panelW, AREA, p.bg);
  spr.setTextSize(land ? 1 : 2);
  spr.setTextDatum(MC_DATUM);

  if (tama.lineGen != lastLineGen) { msgScroll = 0; lastLineGen = tama.lineGen; wake(); }

  if (land) {
    char lines[6][24] = {{0}};
    uint8_t rows = drawLandscapeWrappedStatus(tama.msg, lines, 6, panelW - 14);
    int blockH = rows * 18;
    int startY = (H - blockH) / 2;
    if (startY < 26) startY = 26;
    spr.setTextColor(p.text, p.bg);
    for (uint8_t i = 0; i < rows; i++) spr.drawString(lines[i], X + panelW / 2, startY + i * 18);
    spr.setTextDatum(TL_DATUM);
    return;
  }

  if (tama.nLines == 0) {
    static char msgDisp[8][24];
    uint8_t nMsg = wrapIntoCenteredWords(tama.msg, msgDisp, 8, land ? (panelW - 12) : MAX_PX);
    if (nMsg == 0) {
      spr.setTextDatum(TL_DATUM);
      return;
    }
    spr.setTextColor(p.text, p.bg);
    uint8_t shown = nMsg > (land ? 5 : SHOW) ? (land ? 5 : SHOW) : nMsg;
    int start = nMsg - shown;
    for (uint8_t i = 0; i < shown; i++) {
      spr.drawString(msgDisp[start + i], X + panelW / 2, TOP + (land ? 18 : 8) + i * (land ? 12 : LH));
    }
    spr.setTextDatum(TL_DATUM);
    return;
  }

  // Wrap all transcript lines into a flat display buffer. Track which
  // transcript index each display row came from, so we can dim older ones.
  static char disp[32][24];
  static uint8_t srcOf[32];
  uint8_t nDisp = 0;
  for (uint8_t i = 0; i < tama.nLines && nDisp < 32; i++) {
    uint8_t got = wrapIntoCenteredWords(tama.lines[i], &disp[nDisp], 32 - nDisp, land ? (panelW - 12) : MAX_PX);
    for (uint8_t j = 0; j < got; j++) srcOf[nDisp + j] = i;
    nDisp += got;
  }

  uint8_t visibleRows = land ? 5 : SHOW;
  uint8_t maxBack = (nDisp > visibleRows) ? (nDisp - visibleRows) : 0;
  if (msgScroll > maxBack) msgScroll = maxBack;

  int end = (int)nDisp - msgScroll;
  int start = end - visibleRows; if (start < 0) start = 0;
  uint8_t newest = tama.nLines - 1;
  for (int i = 0; start + i < end; i++) {
    uint8_t row = start + i;
    bool fresh = (srcOf[row] == newest) && (msgScroll == 0);
    spr.setTextColor(fresh ? p.text : p.textDim, p.bg);
    spr.drawString(disp[row], X + panelW / 2, TOP + (land ? 18 : 8) + i * (land ? 12 : LH));
  }
  spr.setTextDatum(TL_DATUM);
  if (msgScroll > 0) {
    spr.setTextSize(1);
    spr.setTextColor(p.body, p.bg);
    spr.setCursor(X + panelW - 22, TOP + AREA - 10);
    spr.printf("-%u", msgScroll);
  }
}

void setup() {
  auto cfg = M5.config();
  cfg.output_power = false;   // No external 5V accessories in this project.
  cfg.internal_imu = false;   // IMU-driven shake/flip/rotation logic is disabled.
  cfg.internal_mic = false;   // Mic is unused; skip its I2S init and bias power.
  M5.begin(cfg);
  M5.Display.setRotation(0);
  M5.Speaker.begin();
  M5.Speaker.setVolume(160);
  startBt();
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);   // off
  applyBrightness();
  lastInteractMs = millis();
  statsLoad();
  settingsLoad();
  petNameLoad();
  dataLoadCache(&tama);

  // BLE stays always-on; s.bt is stored as a preference only.
  configureUiGeometry(true);
  // Load GIF character first (if any), so buddyInit() skips ASCII rendering
  // when GIF mode is active. Fixes the "buddy appears as ASCII then suddenly
  // changes to GIF" on boot after a character transfer.
  characterInit(nullptr);
  gifAvailable = characterLoaded();
  // species NVS: 0..N-1 = ASCII species, 0xFF = use GIF (also the default,
  // so a fresh install lands on the GIF). With no GIF installed, 0xFF falls
  // through to buddyInit()'s clamped default.
  buddyMode = !(gifAvailable && speciesIdxLoad() == SPECIES_GIF);
  buddyInit();
  applyDisplayMode();

  {
    const Palette& p = characterPalette();
    spr.fillSprite(p.bg);
    spr.setTextDatum(MC_DATUM);
    spr.setTextSize(2);
    if (ownerName()[0]) {
      char line[40];
      snprintf(line, sizeof(line), "%s's", ownerName());
      spr.setTextColor(p.text, p.bg);   spr.drawString(line, W/2, H/2 - 12);
      spr.setTextColor(p.body, p.bg);   spr.drawString(petName(), W/2, H/2 + 12);
    } else {
      // First boot, no owner pushed yet — say hi.
      spr.setTextColor(p.body, p.bg);   spr.drawString("Hello!", W/2, H/2 - 12);
      spr.setTextSize(1);
      spr.setTextColor(p.textDim, p.bg);
      spr.drawString("a buddy appears", W/2, H/2 + 12);
    }
    spr.setTextDatum(TL_DATUM); spr.setTextSize(1);
    spr.pushSprite(0, 0);
    delay(1800);
  }

  Serial.printf("buddy: %s\n", buddyMode ? "ASCII mode" : "GIF character loaded");
}

void loop() {
  M5.update();
  t++;
  uint32_t now = millis();
  sampleBatteryUi(now);

  dataPoll(&tama);
  if (statsPollLevelUp()) triggerOneShot(P_CELEBRATE, 3000);
  baseState = derive(tama);
  if (baseState == P_BUSY && lastBaseState != P_BUSY) wake();
  if (baseState == P_IDLE && lastBaseState == P_BUSY && !promptActive() && !interactiveActive()) {
    beepWorkComplete();
  }
  if (baseState == P_IDLE) {
    if (idleStartedMs == 0) idleStartedMs = now;
  } else {
    idleStartedMs = 0;
  }

  // After waking the screen, hold sleep for 12s so users see the wake-up
  // animation. Urgent states (attention, celebrate, busy) override this.
  if (baseState == P_IDLE && (int32_t)(now - wakeTransitionUntil) < 0) baseState = P_SLEEP;

  if ((int32_t)(now - oneShotUntil) >= 0) activeState = baseState;

  // Red status LED is only useful while the screen is visible. Turn it off
  // during screen-off and face-down nap so it does not waste power in
  // "sleeping" states.
  if (!screenOff && !napping && activeState == P_ATTENTION && settings().led) {
    digitalWrite(LED_PIN, (now / 400) % 2 ? LOW : HIGH);
  } else {
    digitalWrite(LED_PIN, HIGH);
  }

  // shake → dizzy + force scenario advance
  if (now - lastShakeCheck > 50) {
    lastShakeCheck = now;
    if (!menuOpen && !screenOff && checkShake() && (int32_t)(now - oneShotUntil) >= 0) {
      wake();
      triggerOneShot(P_DIZZY, 2000);
      Serial.println("shake: dizzy");
    }
  }

  // BtnA: step through fake scenarios
  // Prompt arrival: beep, reset response flag
  if (strcmp(tama.promptId, lastPromptId) != 0) {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId)-1);
    lastPromptId[sizeof(lastPromptId)-1] = 0;
    if (tama.promptId[0] == 0) {
      // Bridge clear snapshot arrived — allow future prompts normally.
      responseSent = false;
      applyDisplayMode();               // 彻底擦除屏幕上的弹窗残影
      if (buddyMode) buddyInvalidate(); // 强制下一次循环重绘宠物
    } else {
      responseSent = false;
      promptArrivedMs = millis();
      wake();
      beep(1200, 80);   // alert chirp
      // Jump to the approval screen no matter what was open — drawApproval
      // only runs from drawHUD which only runs in DISP_NORMAL.
      displayMode = DISP_NORMAL;
      menuOpen = settingsOpen = resetOpen = false;
      applyDisplayMode();
      characterInvalidate();
      if (buddyMode) buddyInvalidate();
    }
  }

  bool interactiveWaitingNow = isInteractiveWaiting(tama);
  if (interactiveWaitingNow && !interactiveAlertLatched) {
    wake();
    beepInteractiveAlert();
    interactiveAlertLatched = true;
  } else if (!interactiveWaitingNow) {
    interactiveAlertLatched = false;
  }

  if (interactiveWaitingNow && lastInteractiveId[0] == 0) {
    strncpy(lastInteractiveId, "active", sizeof(lastInteractiveId)-1);
    lastInteractiveId[sizeof(lastInteractiveId)-1] = 0;
    lastInteractiveQuestionIndex = 0;
    resetInteractiveAnswers();
    if (interactiveUiEnabled()) {
      wake();
      displayMode = DISP_NORMAL;
      menuOpen = settingsOpen = resetOpen = false;
      applyDisplayMode();
      characterInvalidate();
      if (buddyMode) buddyInvalidate();
    }
  } else if (!interactiveWaitingNow && lastInteractiveId[0] != 0) {
    lastInteractiveId[0] = 0;
    interactiveSubmitting = false;
  }

  bool inPrompt = promptActive();
  bool inInteractive = interactiveActive();

  // Button-press wake. Track which button woke the screen so its full
  // press cycle (including long-press) is swallowed — you don't want
  // BtnA-to-wake to also cycle displayMode or open the menu.
  if (M5.BtnA.isPressed() || M5.BtnB.isPressed()) {
    if (screenOff) {
      if (M5.BtnA.isPressed()) swallowBtnA = true;
      if (M5.BtnB.isPressed()) swallowBtnB = true;
    }
    wake();
  }

  // StickS3's side button is a PMIC power/reset key in hardware, not a safe
  // general-purpose UI key. Use an A+B chord for manual screen-off instead.
  if (M5.BtnA.isPressed() && M5.BtnB.isPressed()) {
    if (comboPressStartMs == 0) comboPressStartMs = now;
    if (!screenOff && !comboScreenHandled && (now - comboPressStartMs) >= 350) {
      compat::screenPower(false);
      screenOff = true;
      if (dimmed) dimmed = false;
      swallowBtnA = true;
      swallowBtnB = true;
      comboScreenHandled = true;
    }
  } else {
    comboPressStartMs = 0;
    comboScreenHandled = false;
  }

  if (M5.BtnA.pressedFor(600) && !btnALong && !swallowBtnA && !M5.BtnB.isPressed()) {
    btnALong = true;
    beep(800, 60);
    if (resetOpen) { resetOpen = false; }
    else if (settingsOpen) { settingsOpen = false; characterInvalidate(); }
    else {
      menuOpen = !menuOpen;
      menuSel = 0;
      if (!menuOpen) characterInvalidate();
    }
    Serial.println(menuOpen ? "menu open" : "menu close");
  }
  if (M5.BtnA.wasReleased()) {
    if (!btnALong && !swallowBtnA) {
      if (inPrompt) {
        char cmd[96];
        snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}", tama.promptId);
        sendCmd(cmd);
        strncpy(lastHandledPromptId, tama.promptId, sizeof(lastHandledPromptId)-1);
        lastHandledPromptId[sizeof(lastHandledPromptId)-1] = 0;
        responseSent = true;
        tama.promptId[0] = 0;
        tama.promptTool[0] = 0;
        tama.promptHint[0] = 0;
        tama.sessionsWaiting = 0;  // sync with UI so derive() stops showing attention
        uint32_t tookS = (millis() - promptArrivedMs) / 1000;
        statsOnApproval(tookS);
        beep(2400, 60);
        if (tookS < 5) triggerOneShot(P_HEART, 2000);
      } else if (inInteractive) {
        beep(1200, 20);
      } else if (resetOpen) {
        beep(1800, 30);
        resetSel = (resetSel + 1) % RESET_N;
        resetConfirmIdx = 0xFF;
      } else if (settingsOpen) {
        beep(1800, 30);
        settingsSel = (settingsSel + 1) % SETTINGS_N;
      } else if (menuOpen) {
        beep(1800, 30);
        menuSel = (menuSel + 1) % MENU_N;
      } else {
        beep(1800, 30);
        displayMode = (displayMode + 1) % DISP_COUNT;
        applyDisplayMode();
      }
    }
    btnALong = false;
    swallowBtnA = false;
  }

  // BtnB: prompt/settings/info/pet keep their existing actions.
  // On the normal page, B is reserved for manual screen-off.
  if (M5.BtnB.wasPressed()) {
    if (swallowBtnB) { swallowBtnB = false; }
    else if (M5.BtnA.isPressed()) {
      // A+B is reserved for manual screen-off.
    }
    else
    if (inPrompt) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}", tama.promptId);
      sendCmd(cmd);
      strncpy(lastHandledPromptId, tama.promptId, sizeof(lastHandledPromptId)-1);
      lastHandledPromptId[sizeof(lastHandledPromptId)-1] = 0;
      responseSent = true;
      tama.promptId[0] = 0;
       tama.promptTool[0] = 0;
       tama.promptHint[0] = 0;
       tama.sessionsWaiting = 0;  // sync with UI so derive() stops showing attention
       statsOnDenial();
      beep(600, 60);
    } else if (inInteractive) {
      beep(1200, 20);
    } else if (resetOpen) {
      beep(2400, 30);
      applyReset(resetSel);
    } else if (settingsOpen) {
      beep(2400, 30);
      applySetting(settingsSel);
    } else if (menuOpen) {
      beep(2400, 30);
      menuConfirm();
    } else if (displayMode == DISP_INFO) {
      beep(2400, 30);
      infoPage = (infoPage + 1) % INFO_PAGES;
      applyDisplayMode();
    } else if (displayMode == DISP_PET) {
      beep(2400, 30);
      petPage = (petPage + 1) % PET_PAGES;
      applyDisplayMode();
    } else if (displayMode == DISP_NORMAL) {
      compat::screenPower(false);
      screenOff = true;
      if (dimmed) dimmed = false;
      swallowBtnB = true;
    } else {
      beep(2400, 30);
    }
  }

  // blink bookkeeping

  // Charging clock: takes over the home screen when on USB power, no
  // overlays, no prompt, no live Claude data, and the RTC has been set
  // by the bridge. Pet sleeps underneath. Exit restores Y via
  // applyDisplayMode() so the next mode-switch isn't visually offset.
  clockRefreshRtc();   // 1Hz internal throttle; also caches _onUsb
  bool usbIdleSleep = _onUsb && idleStartedMs != 0 && (now - idleStartedMs) >= USB_IDLE_SLEEP_MS;
  bool usbIdleRecover = _onUsb && baseState == P_IDLE && idleStartedMs != 0 && !inPrompt;
  statsPollUsbIdleRecovery(usbIdleRecover);
  if (baseState == P_IDLE && usbIdleSleep) baseState = P_SLEEP;
  // Show the clock when nothing is happening — bridge heartbeat alone
  // doesn't count as activity (it's the only way to get the RTC synced).
  bool clocking = displayMode == DISP_NORMAL
               && !menuOpen && !settingsOpen && !resetOpen && !inPrompt && !inInteractive
               && tama.sessionsRunning == 0 && tama.sessionsWaiting == 0
               && dataRtcValid() && _onUsb;
  if (clocking) clockUpdateOrient();
  else { clockOrient = 0; orientFrames = 0; paintedOrient = 0; }
  bool landscapeClock = false;

  static bool wasClocking = false;
  static bool wasLandscape = false;
  if (clocking != wasClocking || landscapeClock != wasLandscape) {
    if (clocking && !landscapeClock) characterSetPeek(false);
    else applyDisplayMode();
    characterInvalidate();
    if (buddyMode) buddyInvalidate();
    wasClocking = clocking;
    wasLandscape = landscapeClock;
  }
  if (clocking) {
    uint8_t h = _clkTm.hours;
    activeState = (h < 8 || usbIdleSleep) ? P_SLEEP : P_IDLE;
  }

  bool sleepingNow = activeState == P_SLEEP;
  if (sleepingNow && !sleepStateActive) {
    sleepStateActive = true;
    sleepStateStartedMs = now;
  } else if (!sleepingNow && sleepStateActive) {
    uint32_t sleptMs = now - sleepStateStartedMs;
    sleepStateActive = false;
    if (sleptMs >= ENERGY_RESTORE_SLEEP_MS) statsOnWake();
  }

  static uint32_t lastPasskey = 0;
  uint32_t pk = blePasskey();
  if (pk && !lastPasskey) { wake(); beep(1800, 60); }
  lastPasskey = pk;

  if (napping || screenOff) {
    // skip sprite render — face-down, powered off, or landscape clock
    // (which draws direct-to-LCD below)
  } else if (buddyMode) {
    buddyTick(activeState);
  } else if (characterLoaded()) {
    characterSetState(activeState);
    characterTick();
  } else {
    const Palette& p = characterPalette();
    spr.fillSprite(p.bg);
    spr.setTextColor(p.textDim, p.bg);
    spr.setTextSize(1);
    if (xferActive()) {
      uint32_t done = xferProgress(), total = xferTotal();
      spr.setCursor(8, 90);
      spr.print("installing");
      spr.setCursor(8, 102);
      spr.printf("%luK / %luK", done/1024, total/1024);
      int barW = W - 16;
      spr.drawRect(8, 116, barW, 8, p.textDim);
      if (total > 0) {
        int fill = (int)((uint64_t)barW * done / total);
        if (fill > 1) spr.fillRect(9, 117, fill - 1, 6, p.body);
      }
    } else {
      spr.setCursor(8, 100);
      spr.print("no character loaded");
    }
  }
  if (!napping && !screenOff) {
    if (blePasskey()) drawPasskey();
    else if (clocking) drawClock();
    else if (displayMode == DISP_INFO) drawInfo();
    else if (displayMode == DISP_PET) drawPet();
    else if (settings().hud) drawHUD();
    if (resetOpen) drawReset();
    else if (settingsOpen) drawSettings();
    else if (menuOpen) drawMenu();
    spr.pushSprite(0, 0);
  }

  // Face-down nap: dim immediately, pause animations, accumulate sleep time.
  // Skipped during approval — you're holding it to read, not sleeping it.
  // Exit needs sustained not-down so IMU noise at the threshold doesn't
  // bounce brightness between 8 and full every few frames.
  static int8_t faceDownFrames = 0;
  if (!inPrompt) {
    bool down = isFaceDown();
    if (down)       { if (faceDownFrames < 20) faceDownFrames++; }
    else            { if (faceDownFrames > -10) faceDownFrames--; }
  }

  if (!napping && faceDownFrames >= 15) {
    napping = true;
    napStartMs = now;
    compat::setScreenBrightness0_100(8);
    dimmed = true;
  } else if (napping && faceDownFrames <= -8) {
    napping = false;
    statsOnNapEnd((now - napStartMs) / 1000);
    wake();
  }

  // Battery-only display power policy:
  // - idle: 30s inactivity -> screen off
  // - busy / approval: 30s inactivity -> minimum visible brightness
  // - sleep: 10s inactivity -> screen off
  // USB power forces the display to stay awake at normal brightness.
  if (_onUsb) {
    if (screenOff) {
      compat::screenPower(true);
      screenOff = false;
    }
    if (dimmed) {
      applyBrightness();
      dimmed = false;
    }
  } else {
    uint32_t inactiveMs = millis() - lastInteractMs;
    bool wantsDim = false;
    bool wantsOff = false;

    if (inPrompt || activeState == P_BUSY || activeState == P_ATTENTION) {
      wantsDim = inactiveMs >= BUSY_DIM_MS;
    } else if (baseState == P_IDLE) {
      wantsOff = inactiveMs >= IDLE_SCREEN_OFF_MS;
    } else if (baseState == P_SLEEP) {
      wantsOff = inactiveMs >= SLEEP_SCREEN_OFF_MS;
    }

    if (!screenOff && wantsOff) {
      compat::screenPower(false);
      screenOff = true;
      if (dimmed) dimmed = false;
    } else if (!screenOff && wantsDim) {
      if (!dimmed) {
        compat::setScreenBrightness0_100(MIN_VISIBLE_BRIGHTNESS);
        dimmed = true;
      }
    } else if (!screenOff && dimmed) {
      applyBrightness();
      dimmed = false;
    }
  }

  bool wantsAdvertising = true;
  if (!_onUsb && screenOff && !bleConnected()) {
    uint32_t inactiveMs = millis() - lastInteractMs;
    uint32_t periodMs = SHORT_ADV_PERIOD_MS;
    uint32_t onMs = SHORT_ADV_ON_MS;
    if (inactiveMs >= SHORT_DUTY_WINDOW_MS) {
      periodMs = LONG_ADV_PERIOD_MS;
      onMs = LONG_ADV_ON_MS;
    }
    wantsAdvertising = (inactiveMs % periodMs) < onMs;
  }
  bleSetAdvertising(wantsAdvertising);

  lastBaseState = baseState;

  delay(screenOff ? 200 : 16);
}
