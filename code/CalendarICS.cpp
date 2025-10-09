// CalendarICS.cpp
// ICS-over-HTTPS calendar provider for ESP32 (Arduino)
// - Fast on large ICS: tail fetch + early-exit once UI is filled
// - Robust local time: converts UTC -> local via measured offset (no TZ propagation issues)
// - Formats times as "HH:MM - HH:MM"

#include "Calendar.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

#ifndef DBG
  #define DBG(...) printf(__VA_ARGS__)
#endif

// ---- Tunables ----
static const int     kUiNeededItems = 6;       // how many rows your UI normally shows
static const size_t  kTailBytesTry  = 200000;  // first try: last ~200 KB of the ICS
static const size_t  kLineBuf       = 1024;    // buffer for one physical line

// ---- Portable timegm() (define early so we can use it everywhere) ----
static time_t timegm_compat(struct tm *tm) {
#if defined(__USE_BSD) || defined(_BSD_SOURCE) || defined(__USE_MISC)
  return timegm(tm);
#else
  char *old_tz = getenv("TZ");
  setenv("TZ", "", 1); tzset();
  time_t t = mktime(tm);
  if (old_tz) setenv("TZ", old_tz, 1); else unsetenv("TZ");
  tzset();
  return t;
#endif
}

// ---- Small helpers ----

// Unescape common ICS sequences (\, \n, \;)
static String icsUnescape(const String& s) {
  String o; o.reserve(s.length());
  for (size_t i=0;i<s.length();++i) {
    if (s[i]=='\\' && i+1<s.length()) {
      char n = s[i+1];
      if (n=='n' || n=='N') { o += ' '; i++; continue; }          // replace \n with space (single-line UI)
      if (n=='\\' || n==',' || n==';') { o += n; i++; continue; } // unescape \\, \, and \;
    }
    o += s[i];
  }
  return o;
}

// Compute local offset from UTC (seconds) without relying on TZ propagation in this TU.
// offset = now_local_epoch - now_utc_epoch
static long currentLocalOffsetSeconds() {
  time_t now; time(&now);
  struct tm localTm; localtime_r(&now, &localTm);
  // Treat the local time struct as if it were UTC to recover the offset
  time_t local_as_utc = timegm_compat(&localTm);
  return (long)(local_as_utc - now);
}

// Format "HH:MM" for a UTC epoch using our measured local offset (tz-agnostic)
static void fmtHHMM_local_fromUTC(char* out, size_t outsz, time_t tUTC, long ofsSec) {
  if (tUTC <= 0) { snprintf(out, outsz, "--:--"); return; }
  time_t tl = tUTC + ofsSec;            // convert to local epoch
  struct tm tmL; gmtime_r(&tl, &tmL);   // format via gmtime to avoid TZ dependency
  snprintf(out, outsz, "%02d:%02d", tmL.tm_hour, tmL.tm_min);
}

// ---- Provider implementation ----

class IcsCalendarProvider : public ICalendarProvider {
public:
  explicit IcsCalendarProvider(bool insecure) : insecureTLS_(insecure) {}

  bool begin() override {
    // If TZ was set in the app, tzset() here helps when libc needs it per TU.
    tzset();
    return true;
  }

  void setUrl(const char* url) override {
    url_ = url ? String(url) : String();
  }

  int readToday(CalItem* out, int maxn) override {
    if (!out || maxn <= 0 || WiFi.status() != WL_CONNECTED || url_.isEmpty())
      return 0;

    tzset(); // try to honor app TZ if libc supports it; we still do offset math below

    // 1) Tail-first: newest events are typically near the end for Google ICS
    int filled = fetchAndParse(out, maxn, /*useRangeTail=*/true);
    if (filled >= 0) {
      if (filled > 0) return filled;

      // 2) Fallback: full GET only if tail gave nothing (or not enough)
      filled = fetchAndParse(out, maxn, /*useRangeTail=*/false);
      return (filled < 0) ? 0 : filled;
    }
    return 0;
  }

private:
  // Compare local calendar days via offsetted gmtime (tz-agnostic)
  static bool isSameLocalDay(time_t aUTC, time_t bUTC, long ofsSec) {
    auto dayKey = [&](time_t t)->long {
      time_t tl = t + ofsSec;
      struct tm tmL; gmtime_r(&tl, &tmL);
      return (tmL.tm_year * 1000L + tmL.tm_yday);
    };
    return dayKey(aUTC) == dayKey(bUTC);
  }

  // Does [startUTC, endUTC] (or instant at startUTC if no end) overlap "today" in local time?
  static bool spansToday(time_t startUTC, time_t endUTC, time_t refUTC, long ofsSec) {
    // Build local day start/end for 'refUTC'
    time_t tl = refUTC + ofsSec;
    struct tm tmL; gmtime_r(&tl, &tmL);

    struct tm d0 = tmL; d0.tm_hour = 0;  d0.tm_min = 0;  d0.tm_sec = 0;
    struct tm d1 = tmL; d1.tm_hour = 23; d1.tm_min = 59; d1.tm_sec = 59;

    // Convert local day start/end back to UTC epochs by reversing the offset
    time_t dayStartLocalUTC = timegm_compat(&d0) - ofsSec;  // << fixed: use timegm_compat
    time_t dayEndLocalUTC   = timegm_compat(&d1) - ofsSec;  // << fixed: use timegm_compat

    time_t eEnd = (endUTC > startUTC) ? endUTC : startUTC; // zero-duration events
    return !(eEnd < dayStartLocalUTC || startUTC > dayEndLocalUTC);
  }

  // Parse DTSTART/DTEND variants:
  // - "DTSTART:YYYYMMDDTHHMMSSZ"   (UTC)
  // - "DTSTART;TZID=Europe/Berlin:YYYYMMDDTHHMMSS" (local)
  // - "DTSTART;VALUE=DATE:YYYYMMDD" (all-day local)
  static bool parseICSTime(const String& line, time_t* out) {
    int colon = line.indexOf(':'); if (colon < 0) return false;

    String meta = line.substring(0, colon);   // includes TZID/parameters if present
    String v    = line.substring(colon+1); v.trim();

    bool zulu = false;
    if (v.length() && (v[v.length()-1]=='Z' || v[v.length()-1]=='z')) { zulu = true; v.remove(v.length()-1); }

    int tpos = v.indexOf('T');

    // All-day → treat as local midnight
    if (tpos < 0) {
      if (v.length() < 8) return false;
      struct tm tmv{}; tmv.tm_year = v.substring(0,4).toInt() - 1900;
      tmv.tm_mon  = v.substring(4,6).toInt() - 1;
      tmv.tm_mday = v.substring(6,8).toInt();
      tmv.tm_hour = 0; tmv.tm_min = 0; tmv.tm_sec = 0;
      *out = mktime(&tmv); 
      return (*out > 0);
    }

    if (tpos < 8) return false;
    String d = v.substring(0, 8);
    String t = v.substring(tpos+1);

    // Keep only digits from time and normalize to HHMMSS
    String td; td.reserve(6);
    for (int i=0;i<t.length();++i) if (t[i]>='0' && t[i]<='9') td += t[i];
    while (td.length() < 6) td += '0';
    if (td.length() > 6) td.remove(6);

    int YY = d.substring(0,4).toInt();
    int MO = d.substring(4,6).toInt();
    int DD = d.substring(6,8).toInt();
    int HH = td.substring(0,2).toInt();
    int MM = td.substring(2,4).toInt();
    int SS = td.substring(4,6).toInt();

    if (MO<1 || MO>12 || DD<1 || DD>31 || HH>23 || MM>59 || SS>60) return false;

    struct tm tmv{}; 
    tmv.tm_year = YY - 1900; tmv.tm_mon = MO - 1; tmv.tm_mday = DD;
    tmv.tm_hour = HH;         tmv.tm_min = MM;    tmv.tm_sec  = SS;

    // If 'Z' present → UTC via timegm_compat(); else local via mktime()
    *out = zulu ? timegm_compat(&tmv) : mktime(&tmv);
    return (*out > 0);
  }

  // Read one CRLF-terminated physical line into buf (without CRLF). Returns false on EOF.
  static bool readPhysLine(Stream& s, char* buf, size_t bufsz) {
    size_t n = s.readBytesUntil('\n', buf, bufsz - 1);
    if (n == 0 && !s.available()) return false;
    buf[n] = 0;
    if (n>0 && buf[n-1]=='\r') buf[n-1] = 0;
    return true;
  }

  // Read logical lines by folding continuation lines (leading space/tab), RFC 5545
  template<typename Fn>
  static void readLogicalLines(Stream& s, Fn onLine) {
    char line[kLineBuf];
    String logical; logical.reserve(256);
    bool haveLogical = false;

    while (readPhysLine(s, line, sizeof(line))) {
      bool isCont = (line[0]==' ' || line[0]=='\t');
      if (isCont) {
        if (!haveLogical) { logical = String(line+1); haveLogical = true; }
        else              { logical += (line+1); }
      } else {
        if (haveLogical) onLine(logical);
        logical = String(line);
        haveLogical = true;
      }
    }
    if (haveLogical) onLine(logical);
  }

  // Core fetch+parse. If useRangeTail, send Range header to fetch only the tail.
  // Returns number of UI items (0..maxn) or -1 on HTTP error.
  int fetchAndParse(CalItem* out, int maxn, bool useRangeTail) {
    WiFiClientSecure client; client.setTimeout(15000);
    if (insecureTLS_) client.setInsecure();

    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setUserAgent("ESP32-EPD/1.0");

    if (!http.begin(client, url_)) return -1;

    if (useRangeTail) {
      http.addHeader("Range", String("bytes=-") + String(kTailBytesTry));
    }

    int code = http.GET();
    DBG("[CAL] GET %s code=%d\n", useRangeTail ? "(tail)" : "(full)", code);
    if (!(code == 200 || code == 206)) { http.end(); return -1; }

    Stream* s = http.getStreamPtr();

    // Robust local offset (works even if TZ wasn't applied here)
    const long ofs = currentLocalOffsetSeconds();

    // State for parsing
    bool inEvent = false, inAlarm = false, cancelled = false;
    CalendarEvent cur;
    time_t nowUTC; time(&nowUTC);

    int filled = 0;

    auto flushLogical = [&](const String& ln) {
      // Skip alarms quickly
      if (ln.startsWith("BEGIN:VALARM")) { inAlarm = true;  return; }
      if (ln.startsWith("END:VALARM"))   { inAlarm = false; return; }
      if (inAlarm) return;

      if (ln == "BEGIN:VEVENT") { inEvent = true; cancelled = false; cur = CalendarEvent(); return; }
      if (ln == "END:VEVENT") {
        if (inEvent && !cancelled && cur.start > 0 && cur.summary.length()) {
          if (spansToday(cur.start, cur.end, nowUTC, ofs)) {
            // --- Map to UI item with "HH:MM - HH:MM"
            char hms[8], hme[8];
            fmtHHMM_local_fromUTC(hms, sizeof(hms), cur.start, ofs);
            time_t endUse = (cur.end > cur.start) ? cur.end : cur.start;
            fmtHHMM_local_fromUTC(hme, sizeof(hme), endUse, ofs);

            snprintf(out[filled].time, sizeof(out[filled].time), "%s-%s", hms, hme);

            String title = icsUnescape(cur.summary);
            String loc   = icsUnescape(cur.location);
            if (loc.length()) { title += " ("; title += loc; title += ")"; }

            if ((int)sizeof(out[filled].title) <= (int)title.length())
              title = title.substring(0, sizeof(out[filled].title) - 1);
            strncpy(out[filled].title, title.c_str(), sizeof(out[filled].title));
            out[filled].title[sizeof(out[filled].title)-1] = 0;

            filled++;
            // Early stop once UI is filled
            if (filled >= maxn || filled >= kUiNeededItems) { /* stop via outer flag */ }
          }
        }
        inEvent = false;
        return;
      }

      if (!inEvent) return;

      if (ln.startsWith("DTSTART"))         parseICSTime(ln, &cur.start);
      else if (ln.startsWith("DTEND"))      parseICSTime(ln, &cur.end);
      else if (ln.startsWith("STATUS:"))    { if (ln.indexOf("CANCELLED") >= 0) cancelled = true; }
      else if (ln.startsWith("SUMMARY:"))   cur.summary  = ln.substring(8);
      else if (ln.startsWith("LOCATION:"))  cur.location = ln.substring(9);
    };

    bool stopEarly = false;
    readLogicalLines(*s, [&](const String& l){
      if (!stopEarly) {
        flushLogical(l);
        if (filled >= maxn || filled >= kUiNeededItems) stopEarly = true;
      }
    });

    http.end();
    DBG("[CAL] ui filled=%d%s\n", filled, (useRangeTail && filled==0) ? " (tail empty, will fallback)" : "");
    return filled;
  }

private:
  String url_;
  bool insecureTLS_{true};
};

// Factory
ICalendarProvider* makeIcsCalendarProvider(bool insecureTLS) {
  return new IcsCalendarProvider(insecureTLS);
}
