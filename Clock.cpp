// Clock.cpp
#include "Clock.h"
#include "GUI_Paint.h"
#include "EPD.h"
#include "fonts.h"
#include <Arduino.h>

// Helper: draw string as monospace by placing each char in a fixed cell.
// This avoids digit overlap/jitter on some font packs.
static void drawMonospaceString(int x, int y, const char* s, const sFONT* font, int cellW) {
  for (int i = 0; s[i]; ++i) {
    char buf[2] = { s[i], 0 };
    Paint_DrawString_EN(x + i*cellW, y, buf, (sFONT*)font, WHITE, BLACK);
  }
}

class EpdClockWidget : public IClockWidget {
public:
  EpdClockWidget(int x, int y, int w, int h, IDateTimeFormatter* fmt)
  : _x(x), _y(y), _w(w), _h(h), _fmt(fmt) {}

  void begin() override {
    _lastMinute = -1;
    _lastYday   = -1;
    // Optional: pre-draw empty box or keep your frame line. Clearing handled each update.
    updateNow(/*force*/true);
  }

  void tick() override {
    time_t now; time(&now);
    struct tm lt; localtime_r(&now, &lt);
    if (lt.tm_min != _lastMinute || lt.tm_yday != _lastYday) {
      updateNow(/*force*/false);
    }
  }

private:
  void updateNow(bool force) {
    time_t now; time(&now);
    struct tm lt; localtime_r(&now, &lt);

    // Format strings
    char dateStr[40];
    char timeStr[16];
    _fmt->formatDate(dateStr, sizeof(dateStr), lt);
    _fmt->formatTime(timeStr, sizeof(timeStr), lt);

    // Paint into the partial buffer region
    Paint_SelectImage(_scratchForPartial()); // see comment below
    Paint_NewImage(_scratchForPartial(), _w, _h, 0, WHITE);
    Paint_Clear(WHITE);

    // Two lines: date (Font16), time (Font20), with monospace time
    // date baseline y=2, time baseline y≈24 (tuned for your PRT_CLK_H)
    Paint_DrawString_EN(4, 2, dateStr, &Font16, WHITE, BLACK);

    // Use monospace draw to avoid glyph overlap issues in some font packs
    // Choose a conservative cell width for Font20; tune if you want tighter spacing
    const int cellW = 14;  // ~Font20 width per char on Waveshare packs; adjust if needed
    drawMonospaceString(4, 22, timeStr, &Font20, cellW);

    // Push partial
    EPD_7IN5_V2_Display_Part(_scratchForPartial(), _x, _y, _x+_w, _y+_h);

    _lastMinute = lt.tm_min;
    _lastYday   = lt.tm_yday;
  }

  // We reuse the globally allocated partial framebuffer FBPart.
  // Declare it here as extern (it’s in your main sketch).
  UBYTE* _scratchForPartial() {
    extern UBYTE* FBPart;
    return FBPart;
  }

  int _x, _y, _w, _h;
  IDateTimeFormatter* _fmt;
  int _lastMinute;
  int _lastYday;
};

IClockWidget* makeEpdClockWidget(int x, int y, int w, int h, IDateTimeFormatter* fmt) {
  static EpdClockWidget widget(x, y, w, h, fmt);
  return &widget;
}
