#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "ble_bridge.h"
#include "xfer.h"

struct TamaState {
  static constexpr uint8_t INTERACTIVE_Q_MAX = 4;
  static constexpr uint8_t INTERACTIVE_OPT_MAX = 4;
  static constexpr uint8_t INTERACTIVE_HEADER_BYTES = 48;
  static constexpr uint16_t INTERACTIVE_QUESTION_BYTES = 256;
  static constexpr uint16_t INTERACTIVE_OPTION_BYTES = 160;
  uint8_t  sessionsTotal;
  uint8_t  sessionsRunning;
  uint8_t  sessionsWaiting;
  bool     recentlyCompleted;
  uint32_t tokensToday;
  uint32_t lastUpdated;
  char     msg[24];
  bool     connected;
  char     lines[8][92];
  uint8_t  nLines;
  uint16_t lineGen;          // bumps when lines change — lets UI reset scroll
  char     promptId[40];     // pending permission request ID; empty = no prompt
  char     promptTool[20];
  char     promptHint[44];
  char     interactiveId[32];
  char     interactiveCallId[32];
  char     interactiveTurnId[40];
  char     interactiveStatus[16];
  uint8_t  interactiveQuestionIndex;
  uint8_t  interactiveQuestionTotal;
  uint8_t  interactiveQuestionCount;
  char     interactiveQuestionIds[INTERACTIVE_Q_MAX][32];
  char     interactiveHeaders[INTERACTIVE_Q_MAX][INTERACTIVE_HEADER_BYTES];
  char     interactiveQuestions[INTERACTIVE_Q_MAX][INTERACTIVE_QUESTION_BYTES];
  uint8_t  interactiveOptionCounts[INTERACTIVE_Q_MAX];
  char     interactiveOptions[INTERACTIVE_Q_MAX][INTERACTIVE_OPT_MAX][INTERACTIVE_OPTION_BYTES];
};

// ---------------------------------------------------------------------------
// Three modes, checked in priority order:
//   demo   → auto-cycle fake scenarios every 8s, ignore live data
//   live   → host has pushed JSON recently enough to count as connected
//   asleep → no data, all zeros, "No Codex connected"
// ---------------------------------------------------------------------------

static uint32_t _lastLiveMs = 0;
static uint32_t _lastBtByteMs = 0;   // hasClient() lies; track actual BT traffic
static constexpr uint32_t LIVE_TIMEOUT_MS = 20UL * 60UL * 1000UL;
static bool     _demoMode   = false;
static uint8_t  _demoIdx    = 0;
static uint32_t _demoNext   = 0;

struct _Fake { const char* n; uint8_t t,r,w; bool c; uint32_t tok; };
static const _Fake _FAKES[] = {
  {"asleep",0,0,0,false,0}, {"one idle",1,0,0,false,12000},
  {"busy",4,3,0,false,89000}, {"attention",2,1,1,false,45000},
  {"completed",1,0,0,true,142000},
};

inline void dataSetDemo(bool on) {
  _demoMode = on;
  if (on) { _demoIdx = 0; _demoNext = millis(); }
}
inline bool dataDemo() { return _demoMode; }

inline bool dataConnected() {
  return _lastLiveMs != 0 && (millis() - _lastLiveMs) <= LIVE_TIMEOUT_MS;
}

inline bool dataBtActive() {
  // Desktop's idle keepalive is ~10s; give it 1.5x headroom.
  return _lastBtByteMs != 0 && (millis() - _lastBtByteMs) <= 15000;
}

inline const char* dataScenarioName() {
  if (_demoMode) return _FAKES[_demoIdx].n;
  if (dataConnected()) return dataBtActive() ? "bt" : "usb";
  return "none";
}

// Set true once the bridge sends a time sync — until then the RTC may
// hold whatever was on the coin cell (or 2000-01-01 if it lost power).
static bool _rtcValid = false;
inline bool dataRtcValid() { return _rtcValid; }

static void _applyJson(const char* line, TamaState* out) {
  JsonDocument doc;
  if (deserializeJson(doc, line)) return;
  if (xferCommand(doc)) { _lastLiveMs = millis(); return; }

  // Bridge sends {"time":[epoch_sec, tz_offset_sec]}; gmtime_r on the
  // adjusted epoch yields local components including weekday.
  JsonArray t = doc["time"];
  if (!t.isNull() && t.size() == 2) {
    time_t local = (time_t)t[0].as<uint32_t>() + (int32_t)t[1];
    struct tm lt; gmtime_r(&local, &lt);
    // Use the tm-based API path to avoid occasional struct-field mismatch
    // symptoms seen on some boots (clock shows 00:00 / Jan 01).
    M5.Rtc.setDateTime(&lt);

    extern uint32_t _clkLastRead;
    extern time_t _clkEpochLocal;
    extern uint32_t _clkEpochSetMs;
    extern bool _clkEpochValid;
    _clkLastRead = 0;   // force re-read so _clkDt and _rtcValid agree
    _clkEpochLocal = local;
    _clkEpochSetMs = millis();
    _clkEpochValid = true;
    // Host has provided a trusted timestamp.
    _rtcValid = true;
    _lastLiveMs = millis();
    return;
  }

  // 解析状态快照
  if (!doc["total"].isNull() || !doc["msg"].isNull() || !doc["waiting"].isNull() || !doc["running"].isNull() || !doc["prompt"].isNull()) {
    // 如果宿主机没有下发 total 和 running，保留屏幕上的现有数字（恢复记忆功能）
    if (!doc["total"].isNull())   out->sessionsTotal   = doc["total"].as<uint8_t>();
    if (!doc["running"].isNull()) out->sessionsRunning = doc["running"].as<uint8_t>();
    
    // waiting 仍保持强制同步，防止弹窗状态卡死
    out->sessionsWaiting = doc["waiting"] | 0;
  }

  out->recentlyCompleted = doc["completed"] | false;
  uint32_t bridgeTokens = doc["tokens"] | 0;
  if (doc["tokens"].is<uint32_t>()) statsOnBridgeTokens(bridgeTokens);
  out->tokensToday = doc["tokens_today"] | out->tokensToday;
  const char* m = doc["msg"];
  if (m) { strncpy(out->msg, m, sizeof(out->msg)-1); out->msg[sizeof(out->msg)-1]=0; }
  JsonArray la = doc["entries"];
  if (!la.isNull()) {
    uint8_t n = 0;
    for (JsonVariant v : la) {
      if (n >= 8) break;
      const char* s = v.as<const char*>();
      strncpy(out->lines[n], s ? s : "", 91); out->lines[n][91]=0;
      n++;
    }
    if (n != out->nLines || (n > 0 && strcmp(out->lines[n-1], out->msg) != 0)) {
      out->lineGen++;
    }
    out->nLines = n;
  }
  JsonObject pr = doc["prompt"];
  if (!pr.isNull()) {
    const char* pid = pr["id"]; const char* pt = pr["tool"]; const char* ph = pr["hint"];
    
    extern char lastHandledPromptId[40];
    if (pid && lastHandledPromptId[0] != 0 && strcmp(pid, lastHandledPromptId) == 0) {
      // 如果这是刚刚才审批过的旧包，直接在解析层丢弃它，防止污染主循环状态机
      out->promptId[0] = 0; out->promptTool[0] = 0; out->promptHint[0] = 0;
      out->sessionsWaiting = 0; 
    } else {
      strncpy(out->promptId,   pid ? pid : "", sizeof(out->promptId)-1);   out->promptId[sizeof(out->promptId)-1]=0;
      strncpy(out->promptTool, pt  ? pt  : "", sizeof(out->promptTool)-1); out->promptTool[sizeof(out->promptTool)-1]=0;
      strncpy(out->promptHint, ph  ? ph  : "", sizeof(out->promptHint)-1); out->promptHint[sizeof(out->promptHint)-1]=0;
    }
  } else {
    out->promptId[0] = 0; out->promptTool[0] = 0; out->promptHint[0] = 0;
  }
  JsonObject ir = doc["interactive"];
  if (!ir.isNull()) {
    const char* iid = ir["id"];
    const char* callId = ir["call_id"];
    const char* turnId = ir["turn_id"];
    const char* status = ir["status"];
    strncpy(out->interactiveId, iid ? iid : "", sizeof(out->interactiveId)-1); out->interactiveId[sizeof(out->interactiveId)-1]=0;
    strncpy(out->interactiveCallId, callId ? callId : "", sizeof(out->interactiveCallId)-1); out->interactiveCallId[sizeof(out->interactiveCallId)-1]=0;
    strncpy(out->interactiveTurnId, turnId ? turnId : "", sizeof(out->interactiveTurnId)-1); out->interactiveTurnId[sizeof(out->interactiveTurnId)-1]=0;
    strncpy(out->interactiveStatus, status ? status : "", sizeof(out->interactiveStatus)-1); out->interactiveStatus[sizeof(out->interactiveStatus)-1]=0;
    out->interactiveQuestionIndex = ir["question_index"] | 0;
    out->interactiveQuestionTotal = ir["question_count"] | 0;
    out->interactiveQuestionCount = 0;
    JsonArray qs = ir["questions"];
    if (!qs.isNull()) {
      uint8_t qi = 0;
      for (JsonVariant qv : qs) {
        if (qi >= TamaState::INTERACTIVE_Q_MAX) break;
        JsonObject q = qv.as<JsonObject>();
        const char* qid = q["id"];
        const char* hdr = q["header"];
        const char* txt = q["question"];
        strncpy(out->interactiveQuestionIds[qi], qid ? qid : "", sizeof(out->interactiveQuestionIds[qi])-1);
        out->interactiveQuestionIds[qi][sizeof(out->interactiveQuestionIds[qi])-1] = 0;
        strncpy(out->interactiveHeaders[qi], hdr ? hdr : "", sizeof(out->interactiveHeaders[qi])-1);
        out->interactiveHeaders[qi][sizeof(out->interactiveHeaders[qi])-1] = 0;
        strncpy(out->interactiveQuestions[qi], txt ? txt : "", sizeof(out->interactiveQuestions[qi])-1);
        out->interactiveQuestions[qi][sizeof(out->interactiveQuestions[qi])-1] = 0;
        out->interactiveOptionCounts[qi] = 0;
        JsonArray opts = q["options"];
        if (!opts.isNull()) {
          uint8_t oi = 0;
          for (JsonVariant ov : opts) {
            if (oi >= TamaState::INTERACTIVE_OPT_MAX) break;
            const char* opt = ov.as<const char*>();
            strncpy(out->interactiveOptions[qi][oi], opt ? opt : "", sizeof(out->interactiveOptions[qi][oi])-1);
            out->interactiveOptions[qi][oi][sizeof(out->interactiveOptions[qi][oi])-1] = 0;
            oi++;
          }
          out->interactiveOptionCounts[qi] = oi;
        }
        qi++;
      }
      out->interactiveQuestionCount = qi;
    }
  } else {
    out->interactiveId[0] = 0; out->interactiveCallId[0] = 0; out->interactiveTurnId[0] = 0;
    out->interactiveStatus[0] = 0;
    out->interactiveQuestionIndex = 0; out->interactiveQuestionTotal = 0;
    out->interactiveQuestionCount = 0;
    for (uint8_t qi = 0; qi < TamaState::INTERACTIVE_Q_MAX; qi++) {
      out->interactiveQuestionIds[qi][0] = 0;
      out->interactiveHeaders[qi][0] = 0;
      out->interactiveQuestions[qi][0] = 0;
      out->interactiveOptionCounts[qi] = 0;
      for (uint8_t oi = 0; oi < TamaState::INTERACTIVE_OPT_MAX; oi++) out->interactiveOptions[qi][oi][0] = 0;
    }
  }
  out->lastUpdated = millis();
  _lastLiveMs = millis();
}

template<size_t N>
struct _LineBuf {
  char buf[N];
  uint16_t len = 0;
  void feed(Stream& s, TamaState* out) {
    while (s.available()) {
      char c = s.read();
      if (c == '\n' || c == '\r') {
        if (len > 0) { buf[len]=0; if (buf[0]=='{') _applyJson(buf, out); len=0; }
      } else if (len < N-1) {
        buf[len++] = c;
      }
    }
  }
};

static _LineBuf<1024> _usbLine, _btLine;

inline void dataPoll(TamaState* out) {
  uint32_t now = millis();

  if (_demoMode) {
    if (now >= _demoNext) { _demoIdx = (_demoIdx + 1) % 5; _demoNext = now + 8000; }
    const _Fake& s = _FAKES[_demoIdx];
    out->sessionsTotal=s.t; out->sessionsRunning=s.r; out->sessionsWaiting=s.w;
    out->recentlyCompleted=s.c; out->tokensToday=s.tok; out->lastUpdated=now;
    out->connected = true;
    snprintf(out->msg, sizeof(out->msg), "demo: %s", s.n);
    return;
  }

#if !defined(ARDUINO_USB_MODE) || ARDUINO_USB_MODE == 0
  _usbLine.feed(Serial, out);
#endif
  // BLE ring buffer is drained manually since it's not a Stream.
  while (bleAvailable()) {
    int c = bleRead();
    if (c < 0) break;
    _lastBtByteMs = millis();
    if (c == '\n' || c == '\r') {
      if (_btLine.len > 0) {
        _btLine.buf[_btLine.len] = 0;
        if (_btLine.buf[0] == '{') _applyJson(_btLine.buf, out);
        _btLine.len = 0;
      }
    } else if (_btLine.len < sizeof(_btLine.buf) - 1) {
      _btLine.buf[_btLine.len++] = (char)c;
    }
  }

  out->connected = dataConnected();
  if (!out->connected) {
    out->sessionsTotal=0; out->sessionsRunning=0; out->sessionsWaiting=0;
    out->recentlyCompleted=false; out->lastUpdated=now;
    strncpy(out->msg, "No Codex connected", sizeof(out->msg)-1);
    out->msg[sizeof(out->msg)-1]=0;
  }
}
