// AppConfig.h
#pragma once

enum class DateLocale { EN, DE };

struct AppConfig {
  DateLocale dateLocale = DateLocale::DE; // default to German
  bool use24h = true;                     // keep 24h clock
};

extern AppConfig gConfig;
