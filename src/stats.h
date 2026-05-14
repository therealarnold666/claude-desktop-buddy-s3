#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <time.h>

// Header-only with file-static state: include from exactly one translation
// unit (main.cpp). Including from a second .cpp produces duplicate symbols.

// Persistent stats backed by NVS. Load once at boot; save sparingly
// (NVS sectors have ~100K write cycles). We save on significant events
// only — approval, denial, nap end — never on a timer.

static const uint32_t TOKENS_PER_LEVEL = 50000;
static const uint32_t USAGE_5H_TARGET_TOKENS = 20000;
static const uint32_t USAGE_WEEK_TARGET_TOKENS = 150000;
static const uint32_t USAGE_WEEK_SECONDS = 7UL * 24UL * 3600UL;
static const uint8_t  USAGE_5H_BUCKETS = 5;

struct Stats {
  uint32_t napSeconds;       // cumulative face-down time
  uint16_t approvals;
  uint16_t denials;
  uint16_t velocity[8];      // ring buffer: seconds-to-respond per approval
  uint8_t  velIdx;
  uint8_t  velCount;
  uint8_t  level;
  uint32_t tokens;          // cumulative output tokens, drives level
  uint32_t usage5hHour[USAGE_5H_BUCKETS];
  uint32_t usage5hTokens[USAGE_5H_BUCKETS];
  uint32_t usageWeekStart;
  uint32_t usageWeekTokens;
};

static Stats _stats;
static Preferences _prefs;
static bool _dirty = false;

inline uint32_t statsNowLocalEpoch() {
  extern time_t _clkEpochLocal;
  extern uint32_t _clkEpochSetMs;
  extern bool _clkEpochValid;
  if (!_clkEpochValid) return 0;
  return (uint32_t)(_clkEpochLocal + (time_t)((millis() - _clkEpochSetMs) / 1000));
}

inline uint32_t statsWeekStartForEpoch(uint32_t epochLocal) {
  if (epochLocal == 0) return 0;
  time_t t = (time_t)epochLocal;
  struct tm lt;
  gmtime_r(&t, &lt);
  uint32_t dayStart = epochLocal - (uint32_t)(lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec);
  uint8_t daysSinceMonday = (uint8_t)((lt.tm_wday + 6) % 7);
  return dayStart - (uint32_t)daysSinceMonday * 86400UL;
}

inline void statsRefreshWeekUsage(uint32_t nowLocal) {
  if (nowLocal == 0) return;
  uint32_t weekStart = statsWeekStartForEpoch(nowLocal);
  if (_stats.usageWeekStart != weekStart) {
    _stats.usageWeekStart = weekStart;
    _stats.usageWeekTokens = 0;
    _dirty = true;
  }
}

inline void statsLoad() {
  _prefs.begin("buddy", true);
  _stats.napSeconds = _prefs.getUInt("nap", 0);
  _stats.approvals  = _prefs.getUShort("appr", 0);
  _stats.denials    = _prefs.getUShort("deny", 0);
  _stats.velIdx     = _prefs.getUChar("vidx", 0);
  _stats.velCount   = _prefs.getUChar("vcnt", 0);
  _stats.level      = _prefs.getUChar("lvl", 0);
  _stats.tokens     = _prefs.getUInt("tok", 0);
  _stats.usageWeekStart = _prefs.getUInt("uws", 0);
  _stats.usageWeekTokens = _prefs.getUInt("uwt", 0);
  size_t got = _prefs.getBytes("vel", _stats.velocity, sizeof(_stats.velocity));
  if (got != sizeof(_stats.velocity)) memset(_stats.velocity, 0, sizeof(_stats.velocity));
  got = _prefs.getBytes("u5hh", _stats.usage5hHour, sizeof(_stats.usage5hHour));
  if (got != sizeof(_stats.usage5hHour)) memset(_stats.usage5hHour, 0, sizeof(_stats.usage5hHour));
  got = _prefs.getBytes("u5ht", _stats.usage5hTokens, sizeof(_stats.usage5hTokens));
  if (got != sizeof(_stats.usage5hTokens)) memset(_stats.usage5hTokens, 0, sizeof(_stats.usage5hTokens));
  _prefs.end();
  // Level is derived from tokens; if NVS has level set but tokens at 0,
  // backfill so the derivation holds.
  if (_stats.tokens == 0 && _stats.level > 0) {
    _stats.tokens = (uint32_t)_stats.level * TOKENS_PER_LEVEL;
  }
}

inline void statsSave() {
  if (!_dirty) return;
  _prefs.begin("buddy", false);
  _prefs.putUInt("nap", _stats.napSeconds);
  _prefs.putUShort("appr", _stats.approvals);
  _prefs.putUShort("deny", _stats.denials);
  _prefs.putUChar("vidx", _stats.velIdx);
  _prefs.putUChar("vcnt", _stats.velCount);
  _prefs.putUChar("lvl", _stats.level);
  _prefs.putUInt("tok", _stats.tokens);
  _prefs.putUInt("uws", _stats.usageWeekStart);
  _prefs.putUInt("uwt", _stats.usageWeekTokens);
  _prefs.putBytes("vel", _stats.velocity, sizeof(_stats.velocity));
  _prefs.putBytes("u5hh", _stats.usage5hHour, sizeof(_stats.usage5hHour));
  _prefs.putBytes("u5ht", _stats.usage5hTokens, sizeof(_stats.usage5hTokens));
  _prefs.end();
  _dirty = false;
}

// Level is token-driven now; approvals only feed mood/velocity.
inline void statsOnApproval(uint32_t secondsToRespond) {
  _stats.approvals++;
  _stats.velocity[_stats.velIdx] = (uint16_t)min(secondsToRespond, 65535u);
  _stats.velIdx = (_stats.velIdx + 1) % 8;
  if (_stats.velCount < 8) _stats.velCount++;
  _dirty = true; statsSave();
}

// Tokens feed the pet. Bridge sends the incremental output-token delta earned
// by a completed turn. We accumulate and persist locally so the stick owns the
// displayed lifetime total across reboots.
static bool _levelUpPending = false;

inline void statsOnBridgeTokens(uint32_t delta) {
  if (delta == 0) return;

  uint8_t lvlBefore = (uint8_t)(_stats.tokens / TOKENS_PER_LEVEL);
  _stats.tokens += delta;
  uint8_t lvlAfter = (uint8_t)(_stats.tokens / TOKENS_PER_LEVEL);
  if (lvlAfter > lvlBefore) _levelUpPending = true;
  _stats.level = lvlAfter;

  uint32_t nowLocal = statsNowLocalEpoch();
  if (nowLocal != 0) {
    statsRefreshWeekUsage(nowLocal);
    _stats.usageWeekTokens += delta;

    uint32_t hourSlot = nowLocal / 3600UL;
    uint8_t idx = (uint8_t)(hourSlot % USAGE_5H_BUCKETS);
    if (_stats.usage5hHour[idx] != hourSlot) {
      _stats.usage5hHour[idx] = hourSlot;
      _stats.usage5hTokens[idx] = 0;
    }
    _stats.usage5hTokens[idx] += delta;
  }
  _dirty = true; statsSave();
}

inline void statsSyncLifetimeTokens(uint32_t total) {
  if (total <= _stats.tokens) return;
  uint8_t lvlBefore = (uint8_t)(_stats.tokens / TOKENS_PER_LEVEL);
  _stats.tokens = total;
  uint8_t lvlAfter = (uint8_t)(_stats.tokens / TOKENS_PER_LEVEL);
  if (lvlAfter > lvlBefore) _levelUpPending = true;
  _stats.level = lvlAfter;
  _dirty = true; statsSave();
}

inline bool statsPollLevelUp() {
  bool r = _levelUpPending;
  _levelUpPending = false;
  return r;
}

inline void statsOnDenial() { _stats.denials++; _dirty = true; statsSave(); }

inline void statsMarkDirty() { _dirty = true; }

inline void statsOnNapEnd(uint32_t seconds) {
  _stats.napSeconds += seconds;
  _dirty = true; statsSave();
}

// Median of the velocity ring buffer. 0 if empty.
inline uint16_t statsMedianVelocity() {
  if (_stats.velCount == 0) return 0;
  uint16_t tmp[8];
  memcpy(tmp, _stats.velocity, sizeof(tmp));
  uint8_t n = _stats.velCount;
  // insertion sort, n ≤ 8
  for (uint8_t i = 1; i < n; i++) {
    uint16_t k = tmp[i]; int8_t j = i - 1;
    while (j >= 0 && tmp[j] > k) { tmp[j+1] = tmp[j]; j--; }
    tmp[j+1] = k;
  }
  return tmp[n/2];
}

// 0..4 tier. Velocity sets the base; heavy denial ratio drags it down.
inline uint8_t statsMoodTier() {
  uint16_t vel = statsMedianVelocity();
  int8_t tier;
  if (vel == 0) tier = 2;              // no data: neutral
  else if (vel < 15) tier = 4;
  else if (vel < 30) tier = 3;
  else if (vel < 60) tier = 2;
  else if (vel < 120) tier = 1;
  else tier = 0;
  uint16_t a = _stats.approvals, d = _stats.denials;
  if (a + d >= 3) {                    // need a few decisions before judging
    if (d > a) tier -= 2;
    else if (d * 2 > a) tier -= 1;     // deny rate > 33%
  }
  if (tier < 0) tier = 0;
  return (uint8_t)tier;
}

inline void statsOnWake() {}
inline uint8_t statsEnergyTier() { return 0; }
inline void statsResetUsbIdleRecovery() {}
inline bool statsPollUsbIdleRecovery(bool active) { (void)active; return false; }

inline uint8_t statsFedProgress() {
  return (uint8_t)((_stats.tokens % TOKENS_PER_LEVEL) / (TOKENS_PER_LEVEL / 10));
}

inline uint32_t statsUsage5hTokens() {
  uint32_t nowLocal = statsNowLocalEpoch();
  if (nowLocal == 0) return 0;
  uint32_t nowHour = nowLocal / 3600UL;
  uint32_t total = 0;
  for (uint8_t i = 0; i < USAGE_5H_BUCKETS; i++) {
    uint32_t bucketHour = _stats.usage5hHour[i];
    if (bucketHour == 0 || bucketHour > nowHour) continue;
    if (nowHour - bucketHour < USAGE_5H_BUCKETS) total += _stats.usage5hTokens[i];
  }
  return total;
}

inline uint32_t statsUsageWeekTokens() {
  uint32_t nowLocal = statsNowLocalEpoch();
  if (nowLocal == 0) return _stats.usageWeekTokens;
  uint32_t weekStart = statsWeekStartForEpoch(nowLocal);
  if (_stats.usageWeekStart != weekStart) return 0;
  return _stats.usageWeekTokens;
}

inline uint8_t statsUsagePercent(uint32_t value, uint32_t target) {
  if (target == 0) return 0;
  uint32_t pct = (value * 100UL) / target;
  if (pct > 100UL) pct = 100UL;
  return (uint8_t)pct;
}

inline uint8_t statsUsage5hPercent() {
  return statsUsagePercent(statsUsage5hTokens(), USAGE_5H_TARGET_TOKENS);
}

inline uint8_t statsUsageWeekPercent() {
  return statsUsagePercent(statsUsageWeekTokens(), USAGE_WEEK_TARGET_TOKENS);
}

inline uint32_t statsUsageWeekStart() {
  uint32_t nowLocal = statsNowLocalEpoch();
  if (nowLocal != 0) return statsWeekStartForEpoch(nowLocal);
  return _stats.usageWeekStart;
}

inline uint32_t statsUsageNextWeekResetEpoch() {
  uint32_t weekStart = statsUsageWeekStart();
  if (weekStart == 0) return 0;
  return weekStart + USAGE_WEEK_SECONDS;
}

// --- Settings --------------------------------------------------------------

struct Settings {
  bool sound;
  bool bt;
  bool wifi;     // placeholder — no WiFi stack linked yet, just stores the pref
  bool led;
  bool hud;
  bool interactiveQA;
  uint8_t clockRot;  // 0=portrait 1=landscape
};

static Settings _settings = { true, true, false, true, true, true, 0 };

inline void settingsLoad() {
  _prefs.begin("buddy", true);
  _settings.sound = _prefs.getBool("s_snd", true);
  _settings.bt    = _prefs.getBool("s_bt",  true);
  _settings.wifi  = _prefs.getBool("s_wifi",false);
  _settings.led   = _prefs.getBool("s_led", true);
  _settings.hud      = _prefs.getBool("s_hud", true);
  _settings.interactiveQA = _prefs.getBool("s_iqa", true);
  _settings.clockRot = _prefs.getUChar("s_crot", 0);
  if (_settings.clockRot > 1) _settings.clockRot = 0;
  _prefs.end();
}

inline void settingsSave() {
  _prefs.begin("buddy", false);
  _prefs.putBool("s_snd", _settings.sound);
  _prefs.putBool("s_bt",  _settings.bt);
  _prefs.putBool("s_wifi",_settings.wifi);
  _prefs.putBool("s_led", _settings.led);
  _prefs.putBool("s_hud", _settings.hud);
  _prefs.putBool("s_iqa", _settings.interactiveQA);
  _prefs.putUChar("s_crot", _settings.clockRot);
  _prefs.end();
}

static char _petName[24] = "Buddy";
static char _ownerName[32] = "";

inline void petNameLoad() {
  _prefs.begin("buddy", true);
  _prefs.getString("petname", _petName, sizeof(_petName));
  _prefs.getString("owner", _ownerName, sizeof(_ownerName));
  _prefs.end();
}

// Strip JSON-breaking chars — these names go into a printf'd JSON string
// unescaped (xfer.h status response). A quote persists to NVS and breaks
// the status endpoint until the name is re-set.
static void _safeCopy(char* dst, size_t dstLen, const char* src) {
  size_t j = 0;
  for (size_t i = 0; src[i] && j < dstLen - 1; i++) {
    char c = src[i];
    if (c != '"' && c != '\\' && c >= 0x20) dst[j++] = c;
  }
  dst[j] = 0;
}

inline void petNameSet(const char* name) {
  _safeCopy(_petName, sizeof(_petName), name);
  _prefs.begin("buddy", false);
  _prefs.putString("petname", _petName);
  _prefs.end();
}

inline const char* petName() { return _petName; }

inline void ownerSet(const char* name) {
  _safeCopy(_ownerName, sizeof(_ownerName), name);
  _prefs.begin("buddy", false);
  _prefs.putString("owner", _ownerName);
  _prefs.end();
}

inline const char* ownerName() { return _ownerName; }

inline uint8_t speciesIdxLoad() {
  _prefs.begin("buddy", true);
  uint8_t v = _prefs.getUChar("species", 0xFF);
  _prefs.end();
  return v;
}

inline void speciesIdxSave(uint8_t idx) {
  _prefs.begin("buddy", false);
  _prefs.putUChar("species", idx);
  _prefs.end();
}

inline Settings& settings() { return _settings; }

inline const Stats& stats() { return _stats; }
