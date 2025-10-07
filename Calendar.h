// Calendar.h
#pragma once
#include <Arduino.h>
#include <time.h>

struct CalItem {
  char title[40];
  char time[18];   // "HH:MM - HH:MM" needs 14 incl. NUL; give some headroom
};

struct CalendarEvent {
  time_t start{};
  time_t end{};
  String summary;
  String location;
  bool cancelled{false};
};

class ICalendarProvider {
public:
  virtual ~ICalendarProvider() {}
  virtual bool begin() = 0;
  virtual void setUrl(const char* url) = 0;
  virtual int readToday(CalItem* out, int maxn) = 0;
};

ICalendarProvider* makeIcsCalendarProvider(bool insecureTLS = true);
