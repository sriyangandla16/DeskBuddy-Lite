// ================================================================
//  secrets.h  —  YOUR settings (never share this file or post it online!)
// ================================================================
#pragma once

// ---- Wi-Fi (2.4 GHz) ----
#define WIFI_SSID      "your-wifi-name"
#define WIFI_PASSWORD  "your-wifi-password"

// ---- Time zone: seconds ahead of UTC (India = 5.5 h = 19800) ----
#define UTC_OFFSET_SECONDS  19800

// ---- Weather: free key from https://openweathermap.org/api ----
#define OWM_API_KEY  ""                 // e.g. "1a2b3c4d5e6f..."
#define OWM_CITY     "Mumbai,IN"        // your City,COUNTRY  (no spaces)

// ---- Instagram follower counter (see README) ----
// "IGAA..." token  = shows YOUR OWN professional account
// "EAA..."  token  = can show ANY Business/Creator account set in IG_TARGET
#define IG_TOKEN   ""
#define IG_TARGET  ""                   // e.g. "nasa" — only used with an "EAA..." token
