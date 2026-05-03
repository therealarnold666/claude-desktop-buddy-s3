#pragma once

#if defined(ARDUINO_M5Stack_StickS3) || defined(CONFIG_IDF_TARGET_ESP32S3)
#include <M5Unified.h>
#include <M5GFX.h>

using BuddySurface = lgfx::LGFXBase;
using BuddySprite = M5Canvas;

#ifndef Lcd
#define Lcd Display
#endif
#ifndef Beep
#define Beep Speaker
#endif

struct RTC_TimeTypeDef {
  uint8_t Hours;
  uint8_t Minutes;
  uint8_t Seconds;
};

struct RTC_DateTypeDef {
  uint8_t WeekDay;
  uint8_t Month;
  uint8_t Date;
  uint16_t Year;
};

inline void compatScreenBreath(uint8_t level) {
  int b = (int)level * 255 / 100;
  if (b < 0) b = 0;
  if (b > 255) b = 255;
  M5.Display.setBrightness((uint8_t)b);
}

inline void compatSetLDO2(bool on) {
  if (on) M5.Display.wakeup();
  else M5.Display.sleep();
}

inline uint8_t compatGetBtnPress(void) {
  return M5.BtnPWR.wasClicked() ? 0x02 : 0;
}

inline void compatPowerOff(void) { M5.Power.powerOff(); }
inline float compatGetVBusVoltage(void) { return M5.Power.getVBUSVoltage(); }
inline float compatGetBatVoltage(void) { return M5.Power.getBatteryVoltage(); }
inline float compatGetBatCurrent(void) { return 0.0f; }
inline float compatGetAxpTemp(void) { return 0.0f; }

inline void compatRtcGetTime(RTC_TimeTypeDef* t) {
  if (!t) return;
  auto dt = M5.Rtc.getDateTime();
  t->Hours = dt.time.hours;
  t->Minutes = dt.time.minutes;
  t->Seconds = dt.time.seconds;
}

inline void compatRtcGetDate(RTC_DateTypeDef* d) {
  if (!d) return;
  auto dt = M5.Rtc.getDateTime();
  d->WeekDay = dt.date.weekDay;
  d->Month = dt.date.month;
  d->Date = dt.date.date;
  d->Year = dt.date.year;
}

inline void compatRtcSetTime(const RTC_TimeTypeDef* t) {
  if (!t) return;
  auto dt = M5.Rtc.getDateTime();
  dt.time.hours = t->Hours;
  dt.time.minutes = t->Minutes;
  dt.time.seconds = t->Seconds;
  M5.Rtc.setDateTime(dt);
}

inline void compatRtcSetDate(const RTC_DateTypeDef* d) {
  if (!d) return;
  auto dt = M5.Rtc.getDateTime();
  dt.date.weekDay = d->WeekDay;
  dt.date.month = d->Month;
  dt.date.date = d->Date;
  dt.date.year = d->Year;
  M5.Rtc.setDateTime(dt);
}

inline void compatImuInit(void) { M5.Imu.init(); }
inline void compatBeepUpdate(void) {}

#else
#include <M5StickCPlus.h>

using BuddySurface = TFT_eSPI;
using BuddySprite = TFT_eSprite;

inline void compatScreenBreath(uint8_t level) { M5.Axp.ScreenBreath(level); }
inline void compatSetLDO2(bool on) { M5.Axp.SetLDO2(on); }
inline uint8_t compatGetBtnPress(void) { return M5.Axp.GetBtnPress(); }
inline void compatPowerOff(void) { M5.Axp.PowerOff(); }
inline float compatGetVBusVoltage(void) { return M5.Axp.GetVBusVoltage(); }
inline float compatGetBatVoltage(void) { return M5.Axp.GetBatVoltage(); }
inline float compatGetBatCurrent(void) { return M5.Axp.GetBatCurrent(); }
inline float compatGetAxpTemp(void) { return M5.Axp.GetTempInAXP192(); }
inline void compatRtcGetTime(RTC_TimeTypeDef* t) { M5.Rtc.GetTime(t); }
inline void compatRtcGetDate(RTC_DateTypeDef* d) { M5.Rtc.GetDate(d); }
inline void compatRtcSetTime(const RTC_TimeTypeDef* t) { M5.Rtc.SetTime(t); }
inline void compatRtcSetDate(const RTC_DateTypeDef* d) { M5.Rtc.SetDate(d); }
inline void compatImuInit(void) { M5.Imu.Init(); }
inline void compatBeepUpdate(void) { M5.Beep.update(); }

#endif
