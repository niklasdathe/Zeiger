/*
 * Polished E-Paper UI with partial updates for ESP32 + Waveshare 7.5" V2
 * - Uses ICalendarProvider (Calendar.h) + ICS backend (CalendarIcs.cpp)
 * - Memory-safe partial framebuffer sizing (max of all regions)
 * - Robust time init + guarded calendar rendering
 * - Partial updates: clock (1s), sensors/plants (10s), calendar (60s)
 * - Full refresh every ~10 min to mitigate ghosting
 */

#include "DEV_Config.h"
#include "EPD.h"
#include "GUI_Paint.h"
#include "fonts.h"  // ensure Font16 / Font20 exist
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>

#include "AppConfig.h"
#include "DateTimeFormatter.h"
#include "Clock.h"
#include "Calendar.h"


#ifndef DBG
#define DBG(...) printf(__VA_ARGS__)  // use printf, not Serial
#endif

// ---------- WiFi / Calendar secrets ----------
#include "secrets.h"

#ifndef SECRET_WIFI_SSID
#error "SECRET_WIFI_SSID missing. Create secrets.h (see secrets_example.h)."
#endif
#ifndef SECRET_WIFI_PASS
#error "SECRET_WIFI_PASS missing. Create secrets.h (see secrets_example.h)."
#endif
#ifndef SECRET_CAL_URL
#error "SECRET_CAL_URL missing. Create secrets.h (see secrets_example.h)."
#endif
#ifndef SECRET_INSECURE_TLS
#define SECRET_INSECURE_TLS 0
#endif

static const char* WIFI_SSID = SECRET_WIFI_SSID;
static const char* WIFI_PASS = SECRET_WIFI_PASS;
static const char* CAL_URL = SECRET_CAL_URL;


// ---------- Layout constants ----------
#define W EPD_7IN5_V2_WIDTH   // 800
#define H EPD_7IN5_V2_HEIGHT  // 480

// Calendar row layout (time + title)
#define CAL_TIME_X 0
#define CAL_TITLE_X 140


// Header
#define HDR_X 0
#define HDR_Y 0
#define HDR_W W
#define HDR_H 46

// Left column (Weather)
#define GUTTER 8
#define COL_L_X GUTTER
#define COL_L_Y (HDR_Y + HDR_H + 4)
#define COL_L_W 360
#define COL_L_H 260

// Right column (Calendar)
#define COL_R_X (COL_L_X + COL_L_W + 12)
#define COL_R_Y COL_L_Y
#define COL_R_W (W - COL_R_X - GUTTER)
#define COL_R_H COL_L_H

// Bottom row (Plants)
#define BOT_X GUTTER
#define BOT_Y (COL_L_Y + COL_L_H + 12)
#define BOT_W (W - 2 * GUTTER)
#define BOT_H (H - BOT_Y - GUTTER)

// Partial update rectangles
#define PRT_CLK_X (W - 270)
#define PRT_CLK_Y (HDR_Y + 6)
#define PRT_CLK_W 260
#define PRT_CLK_H (HDR_H - 12)

#define PRT_WTH_X (COL_L_X + 6)
#define PRT_WTH_Y (COL_L_Y + 26)
#define PRT_WTH_W (COL_L_W - 12)
#define PRT_WTH_H (COL_L_H - 36)

#define PRT_CAL_X (COL_R_X + 6)
#define PRT_CAL_Y (COL_R_Y + 26)
#define PRT_CAL_W (COL_R_W - 12)
#define PRT_CAL_H (COL_R_H - 36)

#define PRT_PLT_X (BOT_X + 6)
#define PRT_PLT_Y (BOT_Y + 26)
#define PRT_PLT_W (BOT_W - 12)
#define PRT_PLT_H (BOT_H - 36)

// ---------- Data types ----------
typedef enum {
  WeatherIcon_Sun = 0,
  WeatherIcon_Partly,
  WeatherIcon_Rain,
  WeatherIcon_Storm,
} WeatherIcon;

typedef struct {
  WeatherIcon icon;
  char condition[24];
  int tempNow;       // °C
  int feelsLike;     // °C
  int tempHigh;      // °C
  int tempLow;       // °C
  int humidity;      // %
  int precipChance;  // %
  int windKph;       // km/h
  char windDir[4];
  int uvIndex;
} WeatherData;

typedef struct {
  char name[18];
  int moisture_pct;  // 0..100
} PlantItem;

// ---------- Globals ----------
static UBYTE* FBFull = NULL;               // full-screen framebuffer (1-bit)
UBYTE* FBPart = NULL;                      // remove static so Clock.cpp can extern it
static ICalendarProvider* gCal = nullptr;  // calendar provider

// --- Helpers to size framebuffers safely ---
static inline UWORD bytesForMono1bpp(UWORD w, UWORD h) {
  UWORD rowBytes = (w % 8 == 0) ? (w / 8) : (w / 8 + 1);
  return rowBytes * h;
}

// ---------- Sensor mock (replace with real sensors/APIs) ----------
static void readWeather(WeatherData* w) {
  w->icon = WeatherIcon_Partly;
  snprintf(w->condition, sizeof(w->condition), "Partly Cloudy");
  w->tempNow = 19;
  w->feelsLike = 18;
  w->tempHigh = 22;
  w->tempLow = 11;
  w->humidity = 58;
  w->precipChance = 35;
  w->windKph = 18;
  snprintf(w->windDir, sizeof(w->windDir), "SW");
  w->uvIndex = 5;
}

static void readPlants(PlantItem* p, int n) {
  const char* names[5] = { "Monstera", "Basil", "Ficus", "Fern", "Aloe" };
  int vals[5] = { 34, 52, 63, 28, 58 };
  for (int i = 0; i < n; i++) {
    snprintf(p[i].name, sizeof(p[i].name), "%s", names[i]);
    p[i].moisture_pct = vals[i];
  }
}

// ---------- Weather icon helpers ----------
static void drawSunIcon(int x, int y, int size) {
  int cx = x + size / 2;
  int cy = y + size / 2;
  int r = size / 3;
  int ray = r + size / 6;

  Paint_DrawCircle(cx, cy, r, BLACK, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);

  // cardinal rays (thin rectangles)
  Paint_DrawRectangle(cx - 1, cy - ray, cx + 1, cy - r - 2, BLACK, DOT_PIXEL_1X1,
                      DRAW_FILL_FULL);
  Paint_DrawRectangle(cx - 1, cy + r + 2, cx + 1, cy + ray, BLACK, DOT_PIXEL_1X1,
                      DRAW_FILL_FULL);
  Paint_DrawRectangle(cx - ray, cy - 1, cx - r - 2, cy + 1, BLACK, DOT_PIXEL_1X1,
                      DRAW_FILL_FULL);
  Paint_DrawRectangle(cx + r + 2, cy - 1, cx + ray, cy + 1, BLACK, DOT_PIXEL_1X1,
                      DRAW_FILL_FULL);

  // diagonal rays (small squares)
  int diag = (ray * 7) / 10;
  Paint_DrawRectangle(cx - diag, cy - diag, cx - diag + 2, cy - diag + 2, BLACK,
                      DOT_PIXEL_1X1, DRAW_FILL_FULL);
  Paint_DrawRectangle(cx + diag - 2, cy - diag, cx + diag, cy - diag + 2, BLACK,
                      DOT_PIXEL_1X1, DRAW_FILL_FULL);
  Paint_DrawRectangle(cx - diag, cy + diag - 2, cx - diag + 2, cy + diag, BLACK,
                      DOT_PIXEL_1X1, DRAW_FILL_FULL);
  Paint_DrawRectangle(cx + diag - 2, cy + diag - 2, cx + diag, cy + diag, BLACK,
                      DOT_PIXEL_1X1, DRAW_FILL_FULL);
}

static void drawCloudIcon(int x, int y, int size) {
  int baseY = y + size / 2 + 6;
  int rSmall = size / 5;
  int rMed = rSmall + 4;
  int rLarge = rMed + 4;
  int left = x + 8;

  Paint_DrawCircle(left, baseY, rMed, BLACK, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
  Paint_DrawCircle(left + rMed + rSmall, baseY - rSmall, rLarge, BLACK,
                   DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
  Paint_DrawCircle(left + rMed + rSmall + rLarge, baseY, rMed, BLACK,
                   DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
  Paint_DrawRectangle(left - rSmall, baseY, left + rMed + rSmall + rLarge + rMed,
                      baseY + rSmall, BLACK, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
}

static void drawWeatherConditionIcon(int x, int y, int size, WeatherIcon icon) {
  switch (icon) {
    case WeatherIcon_Sun:
      drawSunIcon(x, y, size);
      break;
    case WeatherIcon_Partly:
      drawSunIcon(x + size / 5, y, size * 3 / 4);
      drawCloudIcon(x, y + size / 4, size);
      break;
    case WeatherIcon_Rain:
      drawCloudIcon(x, y + size / 5, size);
      for (int i = 0; i < 3; i++) {
        int dropX = x + 16 + i * 12;
        int top = y + size - 28;
        Paint_DrawRectangle(dropX, top, dropX + 2, top + 10, BLACK,
                            DOT_PIXEL_1X1, DRAW_FILL_FULL);
      }
      break;
    case WeatherIcon_Storm:
      drawCloudIcon(x, y + size / 5, size);
      Paint_DrawLine(x + size / 2, y + size - 26, x + size / 2 - 10,
                     y + size - 10, BLACK, DOT_PIXEL_1X1, LINE_STYLE_SOLID);
      Paint_DrawLine(x + size / 2 - 10, y + size - 10, x + size / 2 + 2,
                     y + size - 10, BLACK, DOT_PIXEL_1X1, LINE_STYLE_SOLID);
      Paint_DrawLine(x + size / 2 + 2, y + size - 10, x + size / 2 - 8,
                     y + size, BLACK, DOT_PIXEL_1X1, LINE_STYLE_SOLID);
      break;
  }
}

static void drawThermometerSymbol(int x, int y) {
  int bulbR = 6;
  int stemTop = y;
  int stemBottom = y + 16;
  int cx = x + bulbR;

  Paint_DrawRectangle(cx - 2, stemTop, cx + 2, stemBottom, BLACK, DOT_PIXEL_1X1,
                      DRAW_FILL_EMPTY);
  Paint_DrawCircle(cx, stemBottom + bulbR, bulbR, BLACK, DOT_PIXEL_1X1,
                   DRAW_FILL_EMPTY);
  Paint_DrawRectangle(cx - 4, stemBottom - 4, cx + 4, stemBottom, BLACK,
                      DOT_PIXEL_1X1, DRAW_FILL_FULL);
}

static void drawWindSymbol(int x, int y) {
  for (int i = 0; i < 3; i++) {
    int yLine = y + 4 + i * 6;
    Paint_DrawLine(x, yLine, x + 16, yLine, BLACK, DOT_PIXEL_1X1,
                   LINE_STYLE_SOLID);
    Paint_DrawLine(x + 16, yLine, x + 20, yLine - 2, BLACK, DOT_PIXEL_1X1,
                   LINE_STYLE_SOLID);
  }
}

static void drawHumiditySymbol(int x, int y) {
  int cx = x + 8;
  Paint_DrawLine(cx, y, cx - 6, y + 10, BLACK, DOT_PIXEL_1X1, LINE_STYLE_SOLID);
  Paint_DrawLine(cx, y, cx + 6, y + 10, BLACK, DOT_PIXEL_1X1, LINE_STYLE_SOLID);
  Paint_DrawCircle(cx, y + 16, 6, BLACK, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
}

static void drawPrecipSymbol(int x, int y) {
  int top = y + 6;
  Paint_DrawLine(x + 2, top, x + 20, top, BLACK, DOT_PIXEL_1X1,
                 LINE_STYLE_SOLID);
  Paint_DrawLine(x + 2, top, x + 6, top - 6, BLACK, DOT_PIXEL_1X1,
                 LINE_STYLE_SOLID);
  Paint_DrawLine(x + 16, top - 6, x + 20, top, BLACK, DOT_PIXEL_1X1,
                 LINE_STYLE_SOLID);
  Paint_DrawLine(x + 11, top, x + 11, top + 14, BLACK, DOT_PIXEL_1X1,
                 LINE_STYLE_SOLID);
  Paint_DrawLine(x + 8, top + 4, x + 8, top + 10, BLACK, DOT_PIXEL_1X1,
                 LINE_STYLE_SOLID);
  Paint_DrawLine(x + 14, top + 4, x + 14, top + 10, BLACK, DOT_PIXEL_1X1,
                 LINE_STYLE_SOLID);
}

static void drawUvSymbol(int x, int y) {
  drawSunIcon(x, y, 22);
  Paint_DrawString_EN(x + 4, y + 24, (char*)"UV", &Font16, WHITE, BLACK);
}

// ---------- UI drawing ----------
static void drawSectionTitle(int x, int y, const char* txt) {
  Paint_DrawString_EN(x, y, (char*)txt, &Font20, WHITE, BLACK);
}

static void drawStaticUI(void) {
  Paint_SelectImage(FBFull);
  Paint_Clear(WHITE);

  // Header bar
  Paint_DrawRectangle(HDR_X, HDR_Y, HDR_X + HDR_W - 1, HDR_Y + HDR_H - 1,
                      BLACK, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
  Paint_DrawString_EN(HDR_X + 10, HDR_Y + 12, (char*)"Niklas Dathe", &Font20, WHITE, BLACK);
  // Clock box outline
  Paint_DrawRectangle(PRT_CLK_X - 6, PRT_CLK_Y - 4, PRT_CLK_X + PRT_CLK_W + 6, PRT_CLK_Y + PRT_CLK_H + 4,
                      BLACK, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);

  // Left column
  Paint_DrawRectangle(COL_L_X, COL_L_Y, COL_L_X + COL_L_W, COL_L_Y + COL_L_H,
                      BLACK, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
  drawSectionTitle(COL_L_X + 10, COL_L_Y + 4, "TODAY'S WEATHER");

  // Right column
  Paint_DrawRectangle(COL_R_X, COL_R_Y, COL_R_X + COL_R_W, COL_R_Y + COL_R_H,
                      BLACK, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
  drawSectionTitle(COL_R_X + 10, COL_R_Y + 4, "GOOGLE CALENDAR");

  // Bottom
  Paint_DrawRectangle(BOT_X, BOT_Y, BOT_X + BOT_W, BOT_Y + BOT_H,
                      BLACK, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
  drawSectionTitle(BOT_X + 10, BOT_Y + 4, "PLANT STATUS");

  // Push static frame
  EPD_7IN5_V2_Display(FBFull);
}


// Weather block (partial)
static void updateWeatherPart(const WeatherData* w) {
  Paint_SelectImage(FBPart);
  Paint_NewImage(FBPart, PRT_WTH_W, PRT_WTH_H, 0, WHITE);
  Paint_Clear(WHITE);

  const int iconSize = 64;
  drawWeatherConditionIcon(4, 4, iconSize, w->icon);

  char buf[64];
  int textX = iconSize + 16;

  Paint_DrawString_EN(textX, 6, (char*)w->condition, &Font20, WHITE, BLACK);
  snprintf(buf, sizeof(buf), "Now %d%cC", w->tempNow, 0xB0);
  Paint_DrawString_EN(textX, 34, buf, &Font16, WHITE, BLACK);

  snprintf(buf, sizeof(buf), "Feels like %d%cC", w->feelsLike, 0xB0);
  Paint_DrawString_EN(textX, 52, buf, &Font16, WHITE, BLACK);

  int rowY = 84;
  const int rowStep = 32;
  const int metricTextX = 48;

  drawThermometerSymbol(8, rowY - 18);
  snprintf(buf, sizeof(buf), "Temperature  High %d%c / Low %d%c", w->tempHigh, 0xB0, w->tempLow, 0xB0);
  Paint_DrawString_EN(metricTextX, rowY, buf, &Font16, WHITE, BLACK);

  rowY += rowStep;
  drawWindSymbol(8, rowY - 16);
  snprintf(buf, sizeof(buf), "Wind  %d km/h %s", w->windKph, w->windDir);
  Paint_DrawString_EN(metricTextX, rowY, buf, &Font16, WHITE, BLACK);

  rowY += rowStep;
  drawHumiditySymbol(8, rowY - 18);
  snprintf(buf, sizeof(buf), "Humidity  %d%%", w->humidity);
  Paint_DrawString_EN(metricTextX, rowY, buf, &Font16, WHITE, BLACK);

  rowY += rowStep;
  drawPrecipSymbol(6, rowY - 18);
  snprintf(buf, sizeof(buf), "Precipitation  %d%% chance", w->precipChance);
  Paint_DrawString_EN(metricTextX, rowY, buf, &Font16, WHITE, BLACK);

  rowY += rowStep;
  drawUvSymbol(4, rowY - 24);
  snprintf(buf, sizeof(buf), "UV Index  %d", w->uvIndex);
  Paint_DrawString_EN(metricTextX, rowY, buf, &Font16, WHITE, BLACK);

  EPD_7IN5_V2_Display_Part(FBPart, PRT_WTH_X, PRT_WTH_Y, PRT_WTH_X + PRT_WTH_W,
                           PRT_WTH_Y + PRT_WTH_H);
}

// Calendar (partial) — fed by ICalendarProvider
static void updateCalendarPart(const CalItem* items, int n) {
  Paint_SelectImage(FBPart);
  Paint_NewImage(FBPart, PRT_CAL_W, PRT_CAL_H, 0, WHITE);
  Paint_Clear(WHITE);

  int y = 0;
  const int rowH = 26;

  // optional thin separator before title block (visual cue)
  // Paint_DrawLine(CAL_TITLE_X - 8, 0, CAL_TITLE_X - 8, PRT_CAL_H - 1, BLACK, DOT_PIXEL_1X1, LINE_STYLE_SOLID);

  for (int i = 0; i < n && i < 6; i++) {
    // time (e.g., "20:00 - 21:00")
    Paint_DrawString_EN(CAL_TIME_X, y, (char*)items[i].time, &Font16, WHITE, BLACK);
    // title pushed further right so it never overlaps time
    Paint_DrawString_EN(CAL_TITLE_X, y, (char*)items[i].title, &Font16, WHITE, BLACK);
    y += rowH;
  }
  if (n == 0) {
    Paint_DrawString_EN(CAL_TIME_X, 0, (char*)"No events", &Font16, WHITE, BLACK);
  }

  EPD_7IN5_V2_Display_Part(FBPart, PRT_CAL_X, PRT_CAL_Y, PRT_CAL_X + PRT_CAL_W, PRT_CAL_Y + PRT_CAL_H);
}


// Plants (partial)
static void updatePlantsPart(const PlantItem* p, int n) {
  Paint_SelectImage(FBPart);
  Paint_NewImage(FBPart, PRT_PLT_W, PRT_PLT_H, 0, WHITE);
  Paint_Clear(WHITE);

  int y = 0;
  for (int i = 0; i < n && i < 5; i++) {
    int needs = (p[i].moisture_pct < 40);  // threshold
    // Bullet: filled if needs water
    if (needs) {
      Paint_DrawCircle(6, y + 10, 6, BLACK, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    } else {
      Paint_DrawCircle(6, y + 10, 6, BLACK, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
    }
    Paint_DrawString_EN(20, y, (char*)p[i].name, &Font16, WHITE, BLACK);

    char buf[32];
    snprintf(buf, sizeof(buf), "%d %%", p[i].moisture_pct);
    int px = PRT_PLT_W - 60;  // right align %
    Paint_DrawString_EN(px, y, buf, &Font16, WHITE, BLACK);

    if (needs) {
      Paint_DrawString_EN(px + 44, y, (char*)"!", &Font16, WHITE, BLACK);
    }
    y += 28;
  }

  EPD_7IN5_V2_Display_Part(FBPart, PRT_PLT_X, PRT_PLT_Y, PRT_PLT_X + PRT_PLT_W, PRT_PLT_Y + PRT_PLT_H);
}

// ---------- App ----------
void setup() {
  setvbuf(stdout, NULL, _IONBF, 0);  // unbuffered printf
  DBG("\r\n[BOOT] E-Paper dashboard starting...\r\n");

  Serial.begin(115200);
  delay(300);

  // EPD low-level init
  DEV_Module_Init();

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  DBG("[WiFi] Connecting to %s ...\n", WIFI_SSID);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(200);
    DBG(".");
  }
  DBG("\n[WiFi] Status=%d, IP=%s, RSSI=%d dBm\n",
      WiFi.status(), WiFi.localIP().toString().c_str(), WiFi.RSSI());

  // TZ + NTP (Europe/Berlin)
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();
  configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");

  // Robust time readiness loop
  time_t now = 0;
  for (int i = 0; i < 100; i++) {
    time(&now);
    if (now > 1700000000UL) break;
    delay(100);
  }
  struct tm lt;
  localtime_r(&now, &lt);
  DBG("[Time] now=%ld  %04d-%02d-%02d %02d:%02d:%02d (local)\n",
      (long)now, lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);

  // EPD init + clear
  EPD_7IN5_V2_Init();
  EPD_7IN5_V2_Clear();
  DEV_Delay_ms(200);

  // === Allocate full framebuffer (1-bit) ===
  const UWORD fullSize = bytesForMono1bpp(W, H);
  FBFull = (UBYTE*)malloc(fullSize);
  if (!FBFull) {
    printf("OOM full (%u bytes)\r\n", fullSize);
    while (1)
      ;
  }

  // === Allocate partial framebuffer: pick the LARGEST partial region ===
  const UWORD partSizeClk = bytesForMono1bpp(PRT_CLK_W, PRT_CLK_H);  // 260 x ~28
  const UWORD partSizeWeather = bytesForMono1bpp(PRT_WTH_W, PRT_WTH_H);  // 348 x 224
  const UWORD partSizeCal = bytesForMono1bpp(PRT_CAL_W, PRT_CAL_H);  // 400 x 224
  const UWORD partSizePlt = bytesForMono1bpp(PRT_PLT_W, PRT_PLT_H);  // 772 x 114

  UWORD partSize = partSizeClk;
  if (partSizeWeather > partSize) partSize = partSizeWeather;
  if (partSizeCal > partSize) partSize = partSizeCal;  // <-- largest for current layout
  if (partSizePlt > partSize) partSize = partSizePlt;

  FBPart = (UBYTE*)malloc(partSize);
  if (!FBPart) {
    printf("OOM part (%u bytes)\r\n", partSize);
    while (1)
      ;
  }

  // Prepare canvas + draw static chrome
  Paint_SelectImage(FBFull);
  Paint_NewImage(FBFull, W, H, 0, WHITE);
  Paint_Clear(WHITE);
  drawStaticUI();

  // Switch to partial mode for dynamic areas
  EPD_7IN5_V2_Init_Part();

  // --- Config (choose date locale here or in AppConfig.cpp) ---
  gConfig.dateLocale = DateLocale::DE;  // DE for "Dienstag, 07.10.2025"
  // gConfig.dateLocale = DateLocale::EN; // alternative
  gConfig.use24h = true;

  // --- Clock widget ---
  IDateTimeFormatter* fmt = makeFormatterStatic();
  static IClockWidget* clockWidget =
    makeEpdClockWidget(PRT_CLK_X, PRT_CLK_Y, PRT_CLK_W, PRT_CLK_H, fmt);
  clockWidget->begin();

  // Calendar provider
  gCal = makeIcsCalendarProvider(/*insecureTLS=*/(SECRET_INSECURE_TLS ? true : false));
  gCal->setUrl(CAL_URL);
  gCal->begin();

  // Initial dynamic fills (clock already drew once)
  WeatherData weather;
  readWeather(&weather);
  updateWeatherPart(&weather);

  CalItem cal[6];
  int ncal = 0;
  if (now > 1700000000UL && gCal) {
    ncal = gCal->readToday(cal, 6);
    DBG("[CAL] ui count=%d\n", ncal);
  }
  updateCalendarPart(cal, (ncal > 0) ? ncal : 0);

  PlantItem plants[5];
  readPlants(plants, 5);
  updatePlantsPart(plants, 5);
}

void loop() {
  static int sensorTick = 0;
  static int calTick = 0;
  static int fullTick = 0;

  // CLOCK: call often; it only updates when minute/day changed
  extern IClockWidget* makeEpdClockWidget(int, int, int, int, IDateTimeFormatter*);
  static IClockWidget* clk = nullptr;
  if (!clk) {
    IDateTimeFormatter* fmt = makeFormatterStatic();
    clk = makeEpdClockWidget(PRT_CLK_X, PRT_CLK_Y, PRT_CLK_W, PRT_CLK_H, fmt);
  }
  clk->tick();
  DEV_Delay_ms(1000);

  // SENSORS: every 10s (partial)
  if (++sensorTick >= 10) {
    sensorTick = 0;
    WeatherData weather;
    readWeather(&weather);
    updateWeatherPart(&weather);

    PlantItem plants[5];
    readPlants(plants, 5);
    updatePlantsPart(plants, 5);
  }

  // CALENDAR: every 60s (partial)
  if (++calTick >= 60) {
    calTick = 0;
    if (gCal) {
      CalItem cal[6];
      int ncal = gCal->readToday(cal, 6);
      DBG("[CAL] ui count=%d\n", ncal);
      updateCalendarPart(cal, (ncal > 0) ? ncal : 0);
    }
  }

  // PERIODIC FULL REFRESH: every ~10 minutes
  if (++fullTick >= 600) {
    fullTick = 0;
    EPD_7IN5_V2_Init();
    EPD_7IN5_V2_Display(FBFull);
    EPD_7IN5_V2_Init_Part();
    // Repaint dynamic sections
    if (clk) clk->begin();  // ensure clock region is clean after full refresh
    WeatherData weather;
    readWeather(&weather);
    updateWeatherPart(&weather);
    if (gCal) {
      CalItem cal[6];
      int ncal = gCal->readToday(cal, 6);
      updateCalendarPart(cal, (ncal > 0) ? ncal : 0);
    }
    PlantItem plants[5];
    readPlants(plants, 5);
    updatePlantsPart(plants, 5);
  }
}
