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
#include "fonts.h"       // ensure Font16 / Font20 exist
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>

// Calendar module
#include "Calendar.h"

#ifndef DBG
  #define DBG(...) printf(__VA_ARGS__)   // use printf, not Serial
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
static const char* CAL_URL   = SECRET_CAL_URL;


// ---------- Layout constants ----------
#define W   EPD_7IN5_V2_WIDTH     // 800
#define H   EPD_7IN5_V2_HEIGHT    // 480

// Calendar row layout (time + title)
#define CAL_TIME_X   0
#define CAL_TITLE_X  140 


// Header
#define HDR_X      0
#define HDR_Y      0
#define HDR_W      W
#define HDR_H      46

// Left column (Air Quality)
#define GUTTER     8
#define COL_L_X    GUTTER
#define COL_L_Y    (HDR_Y + HDR_H + 4)
#define COL_L_W    360
#define COL_L_H    260

// Right column (Calendar)
#define COL_R_X    (COL_L_X + COL_L_W + 12)
#define COL_R_Y    COL_L_Y
#define COL_R_W    (W - COL_R_X - GUTTER)
#define COL_R_H    COL_L_H

// Bottom row (Plants)
#define BOT_X      GUTTER
#define BOT_Y      (COL_L_Y + COL_L_H + 12)
#define BOT_W      (W - 2*GUTTER)
#define BOT_H      (H - BOT_Y - GUTTER)

// Partial update rectangles
#define PRT_CLK_X  (W - 270)
#define PRT_CLK_Y  (HDR_Y + 6)
#define PRT_CLK_W  260
#define PRT_CLK_H  (HDR_H - 12)

#define PRT_AIR_X  (COL_L_X + 6)
#define PRT_AIR_Y  (COL_L_Y + 26)
#define PRT_AIR_W  (COL_L_W - 12)
#define PRT_AIR_H  (COL_L_H - 36)

#define PRT_CAL_X  (COL_R_X + 6)
#define PRT_CAL_Y  (COL_R_Y + 26)
#define PRT_CAL_W  (COL_R_W - 12)
#define PRT_CAL_H  (COL_R_H - 36)

#define PRT_PLT_X  (BOT_X + 6)
#define PRT_PLT_Y  (BOT_Y + 26)
#define PRT_PLT_W  (BOT_W - 12)
#define PRT_PLT_H  (BOT_H - 36)

// ---------- Data types ----------
typedef struct {
  int pm25;     // µg/m³
  int pm10;     // µg/m³
  int co2;      // ppm
  float voc;    // mg/m³ (placeholder)
  int nox;      // ppb (placeholder)
  float tC;     // °C
  int rh;       // %
} AirData;

typedef struct {
  char name[18];
  int moisture_pct; // 0..100
} PlantItem;

// ---------- Globals ----------
static UBYTE *FBFull = NULL;     // full-screen framebuffer (1-bit)
static UBYTE *FBPart = NULL;     // partial framebuffer (reused per region)
static ICalendarProvider* gCal = nullptr;  // calendar provider

// --- Helpers to size framebuffers safely ---
static inline UWORD bytesForMono1bpp(UWORD w, UWORD h) {
  UWORD rowBytes = (w % 8 == 0) ? (w / 8) : (w / 8 + 1);
  return rowBytes * h;
}

// ---------- Sensor mock (replace with real sensors/APIs) ----------
static void readAir(AirData* a) {
  a->pm25 = 12;  a->pm10 = 28;  a->co2 = 950;
  a->voc  = 0.40f; a->nox = 12; a->tC = 22.4f; a->rh = 46;
}

static void readPlants(PlantItem* p, int n) {
  const char* names[5] = {"Monstera","Basil","Ficus","Fern","Aloe"};
  int vals[5] = {34,52,63,28,58};
  for (int i=0;i<n;i++) {
    snprintf(p[i].name, sizeof(p[i].name), "%s", names[i]);
    p[i].moisture_pct = vals[i];
  }
}

// ---------- AQI helpers ----------
static int level_pm(int val) {
  if (val <= 15) return 0;      // Good
  if (val <= 35) return 1;      // Ok
  return 2;                     // Bad
}
static int level_co2(int ppm) {
  if (ppm < 800) return 0;
  if (ppm < 1200) return 1;
  return 2;
}
static int level_generic(float v, float good_max, float ok_max) {
  if (v <= good_max) return 0;
  if (v <= ok_max)   return 1;
  return 2;
}
static const char* lvlText(int lvl) {
  return (lvl==0) ? "Good" : (lvl==1) ? "Ok" : "Bad";
}

// Tiny 3-segment scale like [■□□]
static void drawScale3(int x, int y, int boxW, int boxH, int lvl) {
  int gap = 2;
  for (int i=0;i<3;i++) {
    int bx = x + i*(boxW+gap);
    Paint_DrawRectangle(bx, y, bx+boxW, y+boxH, BLACK, DOT_PIXEL_1X1,
                        (i==lvl)?DRAW_FILL_FULL:DRAW_FILL_EMPTY);
  }
}

// ---------- UI drawing ----------
static void drawSectionTitle(int x, int y, const char* txt) {
  Paint_DrawString_EN(x, y, (char*)txt, &Font20, WHITE, BLACK);
}

static void drawStaticUI(void) {
  Paint_SelectImage(FBFull);
  Paint_Clear(WHITE);

  // Header bar
  Paint_DrawRectangle(HDR_X, HDR_Y, HDR_X+HDR_W-1, HDR_Y+HDR_H-1,
                      BLACK, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
  Paint_DrawString_EN(HDR_X+10, HDR_Y+12, (char*)"Niklas Dathe", &Font20, WHITE, BLACK);
  // Clock box outline
  Paint_DrawRectangle(PRT_CLK_X-6, PRT_CLK_Y-4, PRT_CLK_X+PRT_CLK_W+6, PRT_CLK_Y+PRT_CLK_H+4,
                      BLACK, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);

  // Left column
  Paint_DrawRectangle(COL_L_X, COL_L_Y, COL_L_X+COL_L_W, COL_L_Y+COL_L_H,
                      BLACK, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
  drawSectionTitle(COL_L_X+10, COL_L_Y+4, "AIR QUALITY");

  // Right column
  Paint_DrawRectangle(COL_R_X, COL_R_Y, COL_R_X+COL_R_W, COL_R_Y+COL_R_H,
                      BLACK, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
  drawSectionTitle(COL_R_X+10, COL_R_Y+4, "GOOGLE CALENDAR");

  // Bottom
  Paint_DrawRectangle(BOT_X, BOT_Y, BOT_X+BOT_W, BOT_Y+BOT_H,
                      BLACK, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
  drawSectionTitle(BOT_X+10, BOT_Y+4, "PLANT STATUS");

  // Push static frame
  EPD_7IN5_V2_Display(FBFull);
}

// Right-header time/date (partial)
static void updateClockPart(void) {
  Paint_SelectImage(FBPart);
  Paint_NewImage(FBPart, PRT_CLK_W, PRT_CLK_H, 0, WHITE);
  Paint_Clear(WHITE);

  time_t now; time(&now);
  struct tm lt; localtime_r(&now, &lt);

  static const char* WD[7]  = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
  static const char* MO[12] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};

  char line1[32], line2[32];
  snprintf(line1, sizeof(line1), "%s %02d %s", WD[lt.tm_wday], lt.tm_mday, MO[lt.tm_mon]);
  snprintf(line2, sizeof(line2), "%02d:%02d", lt.tm_hour, lt.tm_min);

  Paint_DrawString_EN(4, 2,  line1, &Font16, WHITE, BLACK);
  Paint_DrawString_EN(4, 22, line2, &Font20, WHITE, BLACK);

  EPD_7IN5_V2_Display_Part(FBPart, PRT_CLK_X, PRT_CLK_Y, PRT_CLK_X+PRT_CLK_W, PRT_CLK_Y+PRT_CLK_H);
}

// Air block (partial)
static void updateAirPart(const AirData* a) {
  Paint_SelectImage(FBPart);
  Paint_NewImage(FBPart, PRT_AIR_W, PRT_AIR_H, 0, WHITE);
  Paint_Clear(WHITE);

  int y = 0;
  int line = 28;
  int labelX = 0;
  int scaleX = 88;
  int scaleW = 18, scaleH = 12;
  int textX  = scaleX + 3* (scaleW+2) + 10;

  char buf[40];

  // PM2.5
  int lvl = level_pm(a->pm25);
  Paint_DrawString_EN(labelX, y, (char*)"PM2.5", &Font16, WHITE, BLACK);
  drawScale3(scaleX, y+2, scaleW, scaleH, lvl);
  snprintf(buf, sizeof(buf), "%-3s  %d ug/m3", lvlText(lvl), a->pm25);
  Paint_DrawString_EN(textX, y, buf, &Font16, WHITE, BLACK); y += line;

  // PM10
  lvl = level_pm(a->pm10);
  Paint_DrawString_EN(labelX, y, (char*)"PM10 ", &Font16, WHITE, BLACK);
  drawScale3(scaleX, y+2, scaleW, scaleH, lvl);
  snprintf(buf, sizeof(buf), "%-3s  %d ug/m3", lvlText(lvl), a->pm10);
  Paint_DrawString_EN(textX, y, buf, &Font16, WHITE, BLACK); y += line;

  // CO2
  lvl = level_co2(a->co2);
  Paint_DrawString_EN(labelX, y, (char*)"CO2  ", &Font16, WHITE, BLACK);
  drawScale3(scaleX, y+2, scaleW, scaleH, lvl);
  snprintf(buf, sizeof(buf), "%-3s  %d ppm", lvlText(lvl), a->co2);
  Paint_DrawString_EN(textX, y, buf, &Font16, WHITE, BLACK); y += line;

  // VOCx
  lvl = level_generic(a->voc, 0.3f, 0.6f);
  Paint_DrawString_EN(labelX, y, (char*)"VOCx ", &Font16, WHITE, BLACK);
  drawScale3(scaleX, y+2, scaleW, scaleH, lvl);
  snprintf(buf, sizeof(buf), "%-3s  %.2f mg/m3", lvlText(lvl), a->voc);
  Paint_DrawString_EN(textX, y, buf, &Font16, WHITE, BLACK); y += line;

  // NOx
  lvl = level_generic((float)a->nox, 20.f, 40.f);
  Paint_DrawString_EN(labelX, y, (char*)"NOx  ", &Font16, WHITE, BLACK);
  drawScale3(scaleX, y+2, scaleW, scaleH, lvl);
  snprintf(buf, sizeof(buf), "%-3s  %d ppb", lvlText(lvl), a->nox);
  Paint_DrawString_EN(textX, y, buf, &Font16, WHITE, BLACK); y += (line+6);

  // Temp/Humidity
  snprintf(buf, sizeof(buf), "Temp   %.1f C", a->tC);
  Paint_DrawString_EN(labelX, y, buf, &Font16, WHITE, BLACK); y += line;
  snprintf(buf, sizeof(buf), "Hum    %d %%", a->rh);
  Paint_DrawString_EN(labelX, y, buf, &Font16, WHITE, BLACK);

  EPD_7IN5_V2_Display_Part(FBPart, PRT_AIR_X, PRT_AIR_Y, PRT_AIR_X+PRT_AIR_W, PRT_AIR_Y+PRT_AIR_H);
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
    Paint_DrawString_EN(CAL_TIME_X,  y, (char*)items[i].time,  &Font16, WHITE, BLACK);
    // title pushed further right so it never overlaps time
    Paint_DrawString_EN(CAL_TITLE_X, y, (char*)items[i].title, &Font16, WHITE, BLACK);
    y += rowH;
  }
  if (n == 0) {
    Paint_DrawString_EN(CAL_TIME_X, 0, (char*)"No events", &Font16, WHITE, BLACK);
  }

  EPD_7IN5_V2_Display_Part(FBPart, PRT_CAL_X, PRT_CAL_Y, PRT_CAL_X+PRT_CAL_W, PRT_CAL_Y+PRT_CAL_H);
}


// Plants (partial)
static void updatePlantsPart(const PlantItem* p, int n) {
  Paint_SelectImage(FBPart);
  Paint_NewImage(FBPart, PRT_PLT_W, PRT_PLT_H, 0, WHITE);
  Paint_Clear(WHITE);

  int y = 0;
  for (int i=0;i<n && i<5;i++) {
    int needs = (p[i].moisture_pct < 40); // threshold
    // Bullet: filled if needs water
    if (needs) {
      Paint_DrawCircle(6, y+10, 6, BLACK, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    } else {
      Paint_DrawCircle(6, y+10, 6, BLACK, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
    }
    Paint_DrawString_EN(20, y, (char*)p[i].name, &Font16, WHITE, BLACK);

    char buf[32];
    snprintf(buf, sizeof(buf), "%d %%", p[i].moisture_pct);
    int px = PRT_PLT_W - 60; // right align %
    Paint_DrawString_EN(px, y, buf, &Font16, WHITE, BLACK);

    if (needs) {
      Paint_DrawString_EN(px+44, y, (char*)"!", &Font16, WHITE, BLACK);
    }
    y += 28;
  }

  EPD_7IN5_V2_Display_Part(FBPart, PRT_PLT_X, PRT_PLT_Y, PRT_PLT_X+PRT_PLT_W, PRT_PLT_Y+PRT_PLT_H);
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
  time_t now=0;
  for (int i=0;i<100;i++){ time(&now); if (now>1700000000UL) break; delay(100); }
  struct tm lt; localtime_r(&now, &lt);
  DBG("[Time] now=%ld  %04d-%02d-%02d %02d:%02d:%02d (local)\n",
      (long)now, lt.tm_year+1900, lt.tm_mon+1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);

  // EPD init + clear
  EPD_7IN5_V2_Init();
  EPD_7IN5_V2_Clear();
  DEV_Delay_ms(200);

  // === Allocate full framebuffer (1-bit) ===
  const UWORD fullSize = bytesForMono1bpp(W, H);
  FBFull = (UBYTE*)malloc(fullSize);
  if (!FBFull) { printf("OOM full (%u bytes)\r\n", fullSize); while(1); }

  // === Allocate partial framebuffer: pick the LARGEST partial region ===
  const UWORD partSizeClk = bytesForMono1bpp(PRT_CLK_W, PRT_CLK_H);   // 260 x ~28
  const UWORD partSizeAir = bytesForMono1bpp(PRT_AIR_W, PRT_AIR_H);   // 348 x 224
  const UWORD partSizeCal = bytesForMono1bpp(PRT_CAL_W, PRT_CAL_H);   // 400 x 224
  const UWORD partSizePlt = bytesForMono1bpp(PRT_PLT_W, PRT_PLT_H);   // 772 x 114

  UWORD partSize = partSizeClk;
  if (partSizeAir > partSize) partSize = partSizeAir;
  if (partSizeCal > partSize) partSize = partSizeCal;  // <-- largest for current layout
  if (partSizePlt > partSize) partSize = partSizePlt;

  FBPart = (UBYTE*)malloc(partSize);
  if (!FBPart) { printf("OOM part (%u bytes)\r\n", partSize); while(1); }

  // Prepare canvas + draw static chrome
  Paint_SelectImage(FBFull);
  Paint_NewImage(FBFull, W, H, 0, WHITE);
  Paint_Clear(WHITE);
  drawStaticUI();

  // Switch to partial mode for dynamic areas
  EPD_7IN5_V2_Init_Part();

  // Instantiate calendar provider
  gCal = makeIcsCalendarProvider(/*insecureTLS=*/ (SECRET_INSECURE_TLS ? true : false));
  gCal->setUrl(CAL_URL);
  gCal->begin();


  // Initial dynamic fills
  updateClockPart();

  AirData a; readAir(&a); updateAirPart(&a);

  CalItem cal[6];
  int ncal = 0;
  if (now > 1700000000UL && gCal) {
    ncal = gCal->readToday(cal, 6);
    DBG("[CAL] ui count=%d\n", ncal);
  }
  updateCalendarPart(cal, (ncal>0)?ncal:0);

  PlantItem plants[5]; readPlants(plants, 5);
  updatePlantsPart(plants, 5);
}

void loop() {
  static int sensorTick = 0;
  static int calTick = 0;
  static int fullTick = 0;

  // CLOCK: every ~1s (partial)
  updateClockPart();
  DEV_Delay_ms(1000);

  // SENSORS: every 10s (partial)
  if (++sensorTick >= 10) {
    sensorTick = 0;
    AirData a; readAir(&a);
    updateAirPart(&a);

    PlantItem plants[5]; readPlants(plants, 5);
    updatePlantsPart(plants, 5);
  }

  // CALENDAR: every 60s (partial)
  if (++calTick >= 60) {
    calTick = 0;
    if (gCal) {
      CalItem cal[6];
      int ncal = gCal->readToday(cal, 6);
      DBG("[CAL] ui count=%d\n", ncal);
      updateCalendarPart(cal, (ncal>0)?ncal:0);
    }
  }

  // PERIODIC FULL REFRESH: every ~10 minutes
  if (++fullTick >= 600) {
    fullTick = 0;
    EPD_7IN5_V2_Init();
    EPD_7IN5_V2_Display(FBFull);      // re-push static frame to clean ghosting
    EPD_7IN5_V2_Init_Part();
    // immediately re-draw dynamic partials
    updateClockPart();
    AirData a; readAir(&a); updateAirPart(&a);
    if (gCal) {
      CalItem cal[6]; int ncal = gCal->readToday(cal, 6); updateCalendarPart(cal, (ncal>0)?ncal:0);
    }
    PlantItem plants[5]; readPlants(plants, 5); updatePlantsPart(plants, 5);
  }
}
