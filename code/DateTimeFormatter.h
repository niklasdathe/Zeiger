// DateTimeFormatter.h
#pragma once
#include <time.h>
#include <stdio.h>
#include "AppConfig.h"

class IDateTimeFormatter {
public:
  virtual ~IDateTimeFormatter() {}
  virtual void formatDate(char* out, size_t n, const struct tm& lt) const = 0;
  virtual void formatTime(char* out, size_t n, const struct tm& lt) const = 0;
};

class GermanDateTimeFormatter : public IDateTimeFormatter {
public:
  void formatDate(char* out, size_t n, const struct tm& lt) const override {
    static const char* WD[] = {"Sonntag","Montag","Dienstag","Mittwoch","Donnerstag","Freitag","Samstag"};
    // (Dienstag, 07.10.2025)
    snprintf(out, n, "%s, %02d.%02d.%04d",
             WD[lt.tm_wday], lt.tm_mday, lt.tm_mon+1, lt.tm_year+1900);
  }
  void formatTime(char* out, size_t n, const struct tm& lt) const override {
    // always 24h
    snprintf(out, n, "%02d:%02d", lt.tm_hour, lt.tm_min);
  }
};

class EnglishDateTimeFormatter : public IDateTimeFormatter {
public:
  void formatDate(char* out, size_t n, const struct tm& lt) const override {
    static const char* WD[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char* MO[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    // e.g., Tue, 07 Oct 2025
    snprintf(out, n, "%s, %02d %s %04d",
             WD[lt.tm_wday], lt.tm_mday, MO[lt.tm_mon], lt.tm_year+1900);
  }
  void formatTime(char* out, size_t n, const struct tm& lt) const override {
    // 24h vs 12h based on gConfig
    if (gConfig.use24h) {
      snprintf(out, n, "%02d:%02d", lt.tm_hour, lt.tm_min);
    } else {
      int h = lt.tm_hour % 12; if (h == 0) h = 12;
      snprintf(out, n, "%2d:%02d", h, lt.tm_min);
    }
  }
};

// Factory depending on config
inline IDateTimeFormatter* makeFormatterStatic() {
  static GermanDateTimeFormatter de;
  static EnglishDateTimeFormatter en;
  return (gConfig.dateLocale == DateLocale::DE) ? (IDateTimeFormatter*)&de
                                                : (IDateTimeFormatter*)&en;
}
