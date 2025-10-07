// Clock.h
#pragma once
#include <time.h>
#include "DateTimeFormatter.h"

// Simple interface so you can swap renderers later if needed.
class IClockWidget {
public:
  virtual ~IClockWidget() {}
  virtual void begin() = 0;
  // Call frequently (e.g., each loop); it internally updates only if minute/day changed
  virtual void tick() = 0;
};

// Create an EPD-backed clock widget that renders into a partial region.
// It owns no buffers; it reuses your global FBPart via Paint_* APIs.
IClockWidget* makeEpdClockWidget(int x, int y, int w, int h, IDateTimeFormatter* fmt);
