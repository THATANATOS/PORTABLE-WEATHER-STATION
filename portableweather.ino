/****************************************************************************************
/*
  ============================================================
  PORTABLE WEATHER STATION – PIN DEFINITIONS & WIRING NOTES
  ============================================================

  PROJECT SUMMARY:
  - Target: ESP32 / ESP32-S3
  - Display: SPI TFT (ILI9341)
  - Sensor: BMP280 (I2C)
  - Input: 4 physical buttons (UP / DOWN / SELECT / BACK)
  - UI: On-screen digital keyboard navigated using buttons
  - Network: Wi-Fi with persistent credential storage (Preferences)
  - Weather Source:
      * Local: BMP280 (temperature / pressure)
      * Online: Open-Meteo API (Mindanao region)
  - Architecture:
      * Non-blocking Wi-Fi connection state machine
      * Modular code structure
      * Fixed redraw logic to avoid BMP280 screen glitches
      * On-screen control guide for usability

  ------------------------------------------------------------
                      PIN ASSIGNMENTS
  ------------------------------------------------------------

  TFT DISPLAY (SPI):
  - TFT_CS   -> GPIO5   : Chip Select
  - TFT_DC   -> GPIO2   : Data / Command
  - TFT_RST  -> GPIO4   : Reset
  - TFT_SDA  -> GPIO23  : SDA
  - TFT_SCL  -> GPIO18  : SCL

  I2C BUS (BMP280):
  - I2C_SDA  -> GPIO27  : SDA
  - I2C_SCL  -> GPIO26  : SCL

  BUTTON INPUTS:
  - BTN_UP_PIN    -> GPIO15 : Navigate up
  - BTN_DOWN_PIN  -> GPIO32 : Navigate down (rewired, input-only safe pin)
  - BTN_SEL_PIN   -> GPIO21 : Select / confirm
  - BTN_BACK_PIN  -> GPIO12 : Back / cancel

  ------------------------------------------------------------
  WIRING NOTES:
  - All logic is 3.3V ONLY.
  - Buttons are momentary, normally-open.
  - Buttons are wired between GPIO and GND
    (use internal pullups in code).
  - GPIO32 is input-only and ideal for button use.
  - All components must share a common ground.
  - Keep SPI and I2C wiring short to avoid noise.

  ------------------------------------------------------------
  VERSION INFO:
  - Date: 2025-11-08
  - Author / Creator: Joshua C. Godilos
  - Project: Portable Weather Station (ESP32 / ESP32-S3)

  ============================================================
*/
****************************************************************************************/

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Adafruit_BMP280.h>
#include <Preferences.h>

/* ============================
   USER CONFIG
   ============================ */

// Default credentials - used only for quick auto-connect attempt
// If you want no auto-try, set these to empty strings ("")
#define DEFAULT_WIFI_SSID     " WIFI SSID"
#define DEFAULT_WIFI_PASSWORD "WIFI PASSWORD"

// Pin assignments (updated down button to GPIO32 as requested)
#define TFT_CS    5
#define TFT_DC    2
#define TFT_RST   4

#define I2C_SDA   27
#define I2C_SCL   26

#define BTN_UP_PIN     15
#define BTN_DOWN_PIN   32   // <-- rewired to GPIO32 as requested
#define BTN_SEL_PIN    21
#define BTN_BACK_PIN   12

// Screen
const uint16_t SCREEN_W = 160;
const uint16_t SCREEN_H = 128;

/* ============================
   Libraries / Hardware objects
   ============================ */

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);
Adafruit_BMP280 bmp;
Preferences preferences; // for persistent WiFi credentials

/* ============================
   UI Colors
   ============================ */

const uint16_t COL_BG = ST77XX_BLACK;
const uint16_t COL_TEXT = ST77XX_WHITE;
const uint16_t COL_ACCENT = ST77XX_CYAN;
const uint16_t COL_HIGHLIGHT = ST77XX_CYAN;
const uint16_t COL_VALUE = ST77XX_YELLOW;
const uint16_t COL_ERROR = ST77XX_RED;
const uint16_t COL_GOOD = ST77XX_GREEN;
const uint16_t COL_DIM = 0x39E7; // light gray-ish

/* ============================
   Timing / debounce constants
   ============================ */

const unsigned long DEBOUNCE_MS = 30;    // button debounce
const unsigned long LONGPRESS_MS = 1200; // for long press actions
const unsigned long UI_REFRESH_MS = 700; // periodic UI refresh
const unsigned long LOCAL_WEATHER_INTERVAL = 1500; // bmp update interval

/* ============================
   Button struct
   ============================ */

struct Btn {
  uint8_t pin;
  bool stable;           // debounced stable state (HIGH or LOW)
  bool lastRaw;          // last raw reading
  unsigned long lastChange;
  unsigned long pressedAt; // millis when became pressed (LOW)
  bool prevStable;       // previous stable state for edge detection
  unsigned int toggles;
};

Btn btnUp, btnDown, btnSel, btnBack;

/* Forward declarations for button helpers */
void setupButtons();
void updateButtons();
bool pressedEdge(Btn &b);
bool releasedEdge(Btn &b);
bool isPressed(Btn &b);

/* ============================
   App screens and states
   ============================ */

enum Screen {
  SCREEN_LOADING = 0,
  SCREEN_MENU,
  SCREEN_LOCAL_WEATHER,
  SCREEN_API_WEATHER,
  SCREEN_GEOLOCATION,
  SCREEN_WIFI_SCAN,
  SCREEN_WIFI_PASSWORD,
  SCREEN_WIFI_CONNECTING,
  SCREEN_WIFI_RESULT,
  SCREEN_INFO,  
  SCREEN_WIFI_KEYBOARD       // new: the on-screen navigable keyboard
};

Screen currentScreen = SCREEN_LOADING;
Screen previousScreen = SCREEN_LOADING;

/* Menu */
const char* MENU_ITEMS[] = {
  "Local Weather (BMP)",
  "API Weather (Mindanao)",
  "Geolocation",
  "Connect to Wi-Fi",
  "Info / About"
};
const int MENU_COUNT = sizeof(MENU_ITEMS) / sizeof(MENU_ITEMS[0]);
int menuIndex = 0;

/* Wi-Fi scan results */
String scannedSSIDs[32];
int scannedCount = 0;
int wifiListIndex = 0;

/* Wi-Fi manual setup buffers */
String manualSSID = "";
String manualPassword = "";
bool editingSSID = true; // true = editing SSID, false = editing password

/* Wi-Fi connect flags */
bool wifiConnected = false;
String wifiStatusText = "Not connected";

/* BMP280 present flag */
bool bmpPresent = false;

/* Local weather UI flags */
bool localWeatherInitialized = false;
unsigned long localWeatherLastUpdate = 0;

/* WiFi connect state machine */
enum WiFiConnectState { WFC_IDLE, WFC_CONNECTING, WFC_CONNECTED, WFC_FAILED };
WiFiConnectState wifiConnectState = WFC_IDLE;
unsigned long wifiConnectStart = 0;
const unsigned long WIFI_CONNECT_TIMEOUT = 20000; // 20s

/* UI refresh */
unsigned long lastUiRefresh = 0;

/* Open-Meteo config */
const double OPEN_METEO_LAT = 8.5;
const double OPEN_METEO_LON = 125.97;
const char* OPEN_METEO_BASE = "https://api.open-meteo.com/v1/forecast";

/* ============================
   Keyboard layout configuration
   ============================ */

/*
  Keyboard is a grid navigable with 4 buttons:

  - UP / DOWN: move vertical in the grid
  - SELECT (short): move RIGHT
  - BACK   (short): move LEFT
  - SELECT (LONG): insert current highlighted char into active field
  - BACK   (LONG): delete last char from active field
  - SELECT + BACK (both held long): confirm and save credentials (exit keyboard)

  The grid is wide enough for letters, numbers, and a few symbols.
*/

const char KEYBOARD_ROWS = 6;
const char KEYBOARD_COLS = 10;
// We'll produce a 6x10 grid (60 cells) covering most chars needed
const char keyboardCells[KEYBOARD_ROWS][KEYBOARD_COLS + 1] = {
  "abcdefghiJ", // note mix to help variety; user can change if desired
  "klmnopqrst",
  "uvwxyzABCD",
  "EFGHIJKLMN",
  "OPQRSTUVWX",
  "YZ01234567"  // last row will include some digits and symbols via overrides
};

// We'll expose some additional useful symbols in a second 'page' mapping if needed.
// For simplicity, we'll map a few symbol hotspots when cursor reaches certain end cells.
const char extraSymbols[] = "!@#.-_/:*?&";

/* Keyboard cursor state */
int kbRow = 0;
int kbCol = 0;

/* ============================
   Preferences keys
   ============================ */

const char* PREF_NAMESPACE = "pws_prefs";
const char* PREF_SSID_KEY = "wifi_ssid";
const char* PREF_PWD_KEY = "wifi_pwd";

/* ============================
   Forward declarations (UI, screens)
   ============================ */

void drawHeader(const char* title);
void drawFooter(const char* footer);
void clearArea(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
void drawCenteredText(const char* txt, uint8_t size, uint16_t y, uint16_t color);

void showLoading();
void showMenu();
void showLocalWeather();
void showAPISummaryWeather();
void showGeolocation();
void startWifiScanAndShow();
void showWiFiScan();
void showWiFiPasswordEntry();  // previous simple password entry
void showWiFiManualSetup();    // new: show SSID/password fields & enter keyboard
void showKeyboard();           // the actual navigable keyboard UI
void startWiFiConnect(const String &ssid, const String &pwd);
void showWiFiConnectResult(bool ok, const String &msg);
void showInfo();

String weatherCodeToStatus(int code);

/* ============================
   Setup
   ============================ */

void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println("\n--- Portable Weather Station (keyboard + prefs) starting ---");

  // Buttons
  setupButtons();

  // I2C for BMP
  Wire.begin(I2C_SDA, I2C_SCL);
  Serial.printf("I2C started SDA=%d SCL=%d\n", I2C_SDA, I2C_SCL);

  // TFT init
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);
  tft.fillScreen(COL_BG);

  // BMP init
  if (bmp.begin(0x76)) {
    bmpPresent = true;
    Serial.println("BMP280 at 0x76");
  } else if (bmp.begin(0x77)) {
    bmpPresent = true;
    Serial.println("BMP280 at 0x77");
  } else {
    bmpPresent = false;
    Serial.println("BMP280 not found");
  }

  // Preferences init
  preferences.begin(PREF_NAMESPACE, false); // read/write
  // Load saved creds if present into manual buffers (non-empty -> will be used)
  String savedSsid = preferences.getString(PREF_SSID_KEY, "");
  String savedPwd = preferences.getString(PREF_PWD_KEY, "");
  if (savedSsid.length() > 0) {
    manualSSID = savedSsid;
    manualPassword = savedPwd;
    Serial.printf("Loaded saved SSID: %s (pwd length %d)\n", manualSSID.c_str(), manualPassword.length());
  } else {
    Serial.println("No saved Wi-Fi credentials found in Preferences");
  }

  // Show loading and quick auto connect attempt (with defaults or saved creds)
  showLoading();
  // Attempt auto connect: prefer saved creds, else default constants
  String trySsid = savedSsid.length() ? savedSsid : String(DEFAULT_WIFI_SSID);
  String tryPwd = savedSsid.length() ? savedPwd : String(DEFAULT_WIFI_PASSWORD);
  if (trySsid.length() > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(trySsid.c_str(), tryPwd.c_str());
    unsigned long st = millis();
    const unsigned long tmo = 6000;
    while (WiFi.status() != WL_CONNECTED && millis() - st < tmo) {
      delay(200);
    }
    if (WiFi.status() == WL_CONNECTED) {
      wifiConnected = true;
      wifiStatusText = "Connected";
      Serial.println("Auto-connected to WiFi at boot");
    } else {
      wifiConnected = false;
      wifiStatusText = "Not connected";
      WiFi.disconnect(true);
      Serial.println("Auto-connect failed");
    }
  }

  // Start at menu
  currentScreen = SCREEN_MENU;
  showMenu();
}

/* ============================
   Loop
   ============================ */

void loop() {
  updateButtons();

  // Navigation & interactions
  if (pressedEdge(btnUp)) {
    if (currentScreen == SCREEN_MENU) {
      menuIndex = (menuIndex - 1 + MENU_COUNT) % MENU_COUNT;
      showMenu();
    } else if (currentScreen == SCREEN_WIFI_SCAN) {
      if (scannedCount > 0) {
        wifiListIndex = max(0, wifiListIndex - 1);
        showWiFiScan();
      }
    } else if (currentScreen == SCREEN_WIFI_KEYBOARD) {
      // Move cursor up
      kbRow = max(0, kbRow - 1);
      showKeyboard();
    } else {
      // other screens: optional actions
    }
  }

  if (pressedEdge(btnDown)) {
    if (currentScreen == SCREEN_MENU) {
      menuIndex = (menuIndex + 1) % MENU_COUNT;
      showMenu();
    } else if (currentScreen == SCREEN_WIFI_SCAN) {
      if (scannedCount > 0) {
        wifiListIndex = min(scannedCount - 1, wifiListIndex + 1);
        showWiFiScan();
      }
    } else if (currentScreen == SCREEN_WIFI_KEYBOARD) {
      // move cursor down
      kbRow = min((int)KEYBOARD_ROWS - 1, kbRow + 1);
      showKeyboard();
    }
  }

  if (pressedEdge(btnSel)) {
    // SELECT short: interpreted differently on each screen
    if (currentScreen == SCREEN_MENU) {
      previousScreen = currentScreen;
      switch (menuIndex) {
        case 0:
          currentScreen = SCREEN_LOCAL_WEATHER;
          localWeatherInitialized = false;
          showLocalWeather();
          break;
        case 1:
          currentScreen = SCREEN_API_WEATHER;
          showAPISummaryWeather();
          break;
        case 2:
          currentScreen = SCREEN_GEOLOCATION;
          showGeolocation();
          break;
        case 3:
          currentScreen = SCREEN_WIFI_SCAN;
          startWifiScanAndShow();
          break;
        case 4:
          currentScreen = SCREEN_WIFI_MANUAL_SETUP;
          // if manual fields were empty but we loaded saved credentials, we prefill
          showWiFiManualSetup();
          break;
        case 5:
          currentScreen = SCREEN_INFO;
          showInfo();
          break;
      }
    } else if (currentScreen == SCREEN_LOCAL_WEATHER) {
      currentScreen = SCREEN_MENU;
      showMenu();
    } else if (currentScreen == SCREEN_API_WEATHER) {
      currentScreen = SCREEN_MENU;
      showMenu();
    } else if (currentScreen == SCREEN_GEOLOCATION) {
      currentScreen = SCREEN_MENU;
      showMenu();
    } else if (currentScreen == SCREEN_WIFI_SCAN) {
      if (scannedCount > 0) {
        // choose scanned SSID and go to keyboard-based password entry in manual mode
        manualSSID = scannedSSIDs[wifiListIndex];
        manualPassword = "";
        editingSSID = false; // since SSID already chosen from scan, jump to password editing
        currentScreen = SCREEN_WIFI_KEYBOARD;
        // position keyboard cursor to 'a' default
        kbRow = 0; kbCol = 0;
        showKeyboard();
      }
    } else if (currentScreen == SCREEN_WIFI_PASSWORD) {
      // older password screen; leave as is (rare)
      showWiFiPasswordEntry();
    } else if (currentScreen == SCREEN_WIFI_MANUAL_SETUP) {
      // Short SELECT in manual setup: move editing focus to next field (SSID -> PWD or vice versa)
      editingSSID = !editingSSID;
      showWiFiManualSetup();
    } else if (currentScreen == SCREEN_WIFI_KEYBOARD) {
      // Short SELECT = move RIGHT
      kbCol = min((int)KEYBOARD_COLS - 1, kbCol + 1);
      showKeyboard();
    } else if (currentScreen == SCREEN_WIFI_CONNECTING) {
      // nothing (we allow BACK to cancel)
    } else if (currentScreen == SCREEN_WIFI_RESULT) {
      // go back to menu
      currentScreen = SCREEN_MENU;
      showMenu();
    } else if (currentScreen == SCREEN_INFO) {
      currentScreen = SCREEN_MENU;
      showMenu();
    }
  }

  if (pressedEdge(btnBack)) {
    // BACK short:
    if (currentScreen == SCREEN_MENU) {
      // nothing
    } else if (currentScreen == SCREEN_WIFI_KEYBOARD) {
      // short BACK = move LEFT
      kbCol = max(0, kbCol - 1);
      showKeyboard();
    } else if (currentScreen == SCREEN_LOCAL_WEATHER) {
      currentScreen = SCREEN_MENU;
      showMenu();
    } else if (currentScreen == SCREEN_API_WEATHER) {
      currentScreen = SCREEN_MENU;
      showMenu();
    } else if (currentScreen == SCREEN_GEOLOCATION) {
      currentScreen = SCREEN_MENU;
      showMenu();
    } else if (currentScreen == SCREEN_WIFI_SCAN) {
      currentScreen = SCREEN_MENU;
      showMenu();
    } else if (currentScreen == SCREEN_WIFI_MANUAL_SETUP) {
      // Back to menu, don't save
      currentScreen = SCREEN_MENU;
      showMenu();
    } else if (currentScreen == SCREEN_WIFI_CONNECTING) {
      // cancel connect
      WiFi.disconnect(true);
      wifiConnectState = WFC_FAILED;
      showWiFiConnectResult(false, "Cancelled by user");
    } else {
      currentScreen = SCREEN_MENU;
      showMenu();
    }
  }

  // Long press handling: insert/delete/confirm keyboard or confirm manual connect
  // SELECT long: insert highlighted char into current field when in keyboard
  if (currentScreen == SCREEN_WIFI_KEYBOARD) {
    if (isPressed(btnSel) && btnSel.pressedAt != 0 && (millis() - btnSel.pressedAt) > LONGPRESS_MS) {
      // Insert highlighted char
      // Get char under cursor (with edge cases)
      char ch = keyboardCells[kbRow][kbCol];
      // Map last row special columns to extraSymbols if necessary
      // (we'll map if kbRow reaches bottom-right zone)
      if (kbRow == KEYBOARD_ROWS - 1 && kbCol >= 8) {
        // try to index into extraSymbols if present
        int symIndex = kbCol - 8;
        if (symIndex < (int)strlen(extraSymbols)) ch = extraSymbols[symIndex];
      }
      if (editingSSID) {
        if (manualSSID.length() < 32) manualSSID += ch;
      } else {
        if (manualPassword.length() < 64) manualPassword += ch;
      }
      // small debounce to prevent repeated long insertions - wait until button released
      // We'll busy-wait until release but keep it short to avoid blocking UI too long
      while (isPressed(btnSel)) { delay(10); updateButtons(); }
      showKeyboard();
    }
    // BACK long: delete last char
    if (isPressed(btnBack) && btnBack.pressedAt != 0 && (millis() - btnBack.pressedAt) > LONGPRESS_MS) {
      if (editingSSID) {
        if (manualSSID.length() > 0) manualSSID.remove(manualSSID.length() - 1);
      } else {
        if (manualPassword.length() > 0) manualPassword.remove(manualPassword.length() - 1);
      }
      // wait for release
      while (isPressed(btnBack)) { delay(10); updateButtons(); }
      showKeyboard();
    }
    // SELECT + BACK long together: confirm save and attempt connect
    if (isPressed(btnSel) && isPressed(btnBack) && btnSel.pressedAt != 0 && btnBack.pressedAt != 0) {
      unsigned long heldSel = millis() - btnSel.pressedAt;
      unsigned long heldBack = millis() - btnBack.pressedAt;
      if (heldSel > LONGPRESS_MS && heldBack > LONGPRESS_MS) {
        // Confirm & save to Preferences, then try connect
        preferences.putString(PREF_SSID_KEY, manualSSID);
        preferences.putString(PREF_PWD_KEY, manualPassword);
        Serial.println("Saved Wi-Fi credentials to Preferences.");
        // Transition to connecting UI and start connect
        startWiFiConnect(manualSSID, manualPassword);
        // small delay to let UI update
        delay(40);
      }
    }
  }

  // Also support long SELECT in manual setup screen to open keyboard when navigating fields
  if (currentScreen == SCREEN_WIFI_MANUAL_SETUP) {
    if (isPressed(btnSel) && btnSel.pressedAt != 0 && (millis() - btnSel.pressedAt) > LONGPRESS_MS) {
      // Open keyboard to edit currently selected field
      currentScreen = SCREEN_WIFI_KEYBOARD;
      // preposition keyboard cursor to 'a' default
      kbRow = 0; kbCol = 0;
      showKeyboard();
      // wait for release
      while (isPressed(btnSel)) { delay(10); updateButtons(); }
    }
  }

  // Periodic refresh tasks
  unsigned long now = millis();
  if (now - lastUiRefresh > UI_REFRESH_MS) {
    lastUiRefresh = now;
    if (currentScreen == SCREEN_LOCAL_WEATHER) {
      showLocalWeather(); // refresh sensor readings
    } else if (currentScreen == SCREEN_WIFI_CONNECTING && wifiConnectState == WFC_CONNECTING) {
      // poll WiFi status
      if (WiFi.status() == WL_CONNECTED) {
        wifiConnectState = WFC_CONNECTED;
        wifiConnected = true;
        String ip = WiFi.localIP().toString();
        showWiFiConnectResult(true, "Connected: " + ip);
      } else if (millis() - wifiConnectStart > WIFI_CONNECT_TIMEOUT) {
        WiFi.disconnect(true);
        wifiConnectState = WFC_FAILED;
        wifiConnected = false;
        showWiFiConnectResult(false, "Timeout");
      } else {
        // small animated dot or progress square could be toggled here
      }
    }
  }

  // tiny yield
  delay(8);
}

/* ============================
   Button Helpers
   ============================ */

void setupButtons() {
  pinMode(BTN_UP_PIN, INPUT_PULLUP);
  pinMode(BTN_DOWN_PIN, INPUT_PULLUP); // remapped to 32 by user request
  pinMode(BTN_SEL_PIN, INPUT_PULLUP);
  pinMode(BTN_BACK_PIN, INPUT_PULLUP);

  auto initB = [](Btn &b, uint8_t pin){
    b.pin = pin;
    b.stable = digitalRead(pin);
    b.lastRaw = b.stable;
    b.lastChange = millis();
    b.pressedAt = 0;
    b.prevStable = b.stable;
    b.toggles = 0;
  };

  initB(btnUp, BTN_UP_PIN);
  initB(btnDown, BTN_DOWN_PIN);
  initB(btnSel, BTN_SEL_PIN);
  initB(btnBack, BTN_BACK_PIN);

  Serial.printf("Buttons assigned: UP=%d DOWN=%d SEL=%d BACK=%d\n", BTN_UP_PIN, BTN_DOWN_PIN, BTN_SEL_PIN, BTN_BACK_PIN);
}

void updateButtons() {
  unsigned long t = millis();

  auto updateOne = [&](Btn &b) {
    bool raw = digitalRead(b.pin);
    if (raw != b.lastRaw) {
      b.lastChange = t;
      b.lastRaw = raw;
    } else {
      if ((t - b.lastChange) > DEBOUNCE_MS && b.stable != raw) {
        b.stable = raw;
        if (raw == LOW) {
          b.pressedAt = t;
        } else {
          b.pressedAt = 0;
        }
        b.toggles++;
      }
    }
  };

  updateOne(btnUp);
  updateOne(btnDown);
  updateOne(btnSel);
  updateOne(btnBack);
}

bool pressedEdge(Btn &b) {
  bool res = false;
  if (b.prevStable && b.stable == LOW) res = true;
  b.prevStable = b.stable;
  return res;
}

bool releasedEdge(Btn &b) {
  bool res = false;
  if (!b.prevStable && b.stable == HIGH) res = true;
  b.prevStable = b.stable;
  return res;
}

bool isPressed(Btn &b) {
  return b.stable == LOW;
}

/* ============================
   UI Helpers
   ============================ */

void drawHeader(const char* title) {
  tft.fillRect(0, 0, SCREEN_W, 20, COL_BG);
  tft.setTextSize(1);
  tft.setTextColor(COL_ACCENT);
  tft.setCursor(6, 4);
  tft.print(title);
  tft.drawLine(4, 20, SCREEN_W - 4, 20, COL_ACCENT);
}

void drawFooter(const char* footer) {
  tft.fillRect(0, SCREEN_H - 14, SCREEN_W, 14, COL_BG);
  tft.setTextSize(1);
  tft.setTextColor(COL_TEXT);
  tft.setCursor(6, SCREEN_H - 12);
  tft.print(footer);
}

void clearArea(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color) {
  tft.fillRect(x, y, w, h, color);
}

void drawCenteredText(const char* txt, uint8_t size, uint16_t y, uint16_t color) {
  tft.setTextSize(size);
  int16_t x1, y1;
  uint16_t w, h;
  tft.getTextBounds(txt, 0, y, &x1, &y1, &w, &h);
  int16_t x = (SCREEN_W - w) / 2;
  if (x < 0) x = 0;
  tft.setTextColor(color);
  tft.setCursor(x, y);
  tft.print(txt);
}

/* ============================
   Screen: Loading
   ============================ */

void showLoading() {
  tft.fillScreen(COL_BG);
  drawHeader("LOADING - Weather Station");
  tft.setTextSize(1);
  tft.setTextColor(COL_TEXT);
  tft.setCursor(6, 28);
  tft.print("Initializing network...");
  // progress bar
  tft.drawRect(10, 60, SCREEN_W - 20, 10, COL_TEXT);
  for (int i = 0; i < 12; ++i) {
    tft.fillRect(12 + i * 10, 62, 8, 6, COL_ACCENT);
    delay(60);
  }
  drawFooter("Booting...");
}

/* ============================
   Screen: Main Menu
   ============================ */

void showMenu() {
  localWeatherInitialized = false; // ensure local screen fully redrawn next time
  tft.fillScreen(COL_BG);
  drawHeader("MAIN MENU");
  tft.setTextSize(1);
  int baseY = 24;
  for (int i = 0; i < MENU_COUNT; ++i) {
    int y = baseY + i * 20;
    if (i == menuIndex) {
      tft.fillRect(4, y - 2, SCREEN_W - 8, 18, COL_HIGHLIGHT);
      tft.setTextColor(ST77XX_BLACK);
    } else {
      tft.fillRect(4, y - 2, SCREEN_W - 8, 18, COL_BG);
      tft.setTextColor(COL_TEXT);
    }
    tft.setCursor(10, y + 0);
    tft.print(MENU_ITEMS[i]);
  }
  
}

/* ============================
   Screen: Local Weather (BMP280) - Fixed
   ============================ */

void showLocalWeather() {
  if (!localWeatherInitialized) {
    tft.fillScreen(COL_BG);
    drawHeader("LOCAL WEATHER (BMP280)");
    tft.setTextSize(1);
    tft.setTextColor(COL_TEXT);
    tft.setCursor(8, 30); tft.print("Temperature:");
    tft.setCursor(8, 52); tft.print("Pressure:");
    tft.setCursor(8, 74); tft.print("Altitude:");
    // Value boxes to update later
    tft.fillRect(92, 28, 64, 16, COL_BG);
    tft.fillRect(92, 50, 64, 16, COL_BG);
    tft.fillRect(92, 72, 64, 16, COL_BG);
    
    localWeatherInitialized = true;
    localWeatherLastUpdate = 0;
  }

  // Throttle
  if (millis() - localWeatherLastUpdate < LOCAL_WEATHER_INTERVAL) return;
  localWeatherLastUpdate = millis();

  float tval = NAN, pval = NAN, aval = NAN;
  bool ok = bmpPresent;
  if (ok) {
    tval = bmp.readTemperature();
    pval = bmp.readPressure() / 100.0F; // hPa
    aval = bmp.readAltitude(1013.25F);
    if (isnan(tval) || isnan(pval) || isnan(aval)) ok = false;
  }

  uint16_t tcolor = COL_VALUE;
  if (ok) {
    if (tval >= 30.0) tcolor = ST77XX_RED;
    else if (tval >= 20.0) tcolor = ST77XX_GREEN;
    else tcolor = ST77XX_BLUE;
  }

  // Temperature
  clearArea(92, 28, 64, 16, COL_BG);
  tft.setTextSize(1);
  if (ok) {
    tft.setTextColor(tcolor);
    tft.setCursor(92, 30);
    char buf[24]; snprintf(buf, sizeof(buf), "%.1f C", tval);
    tft.print(buf);
  } else {
    tft.setTextColor(COL_ERROR);
    tft.setCursor(92, 30);
    tft.print("ERR");
  }

  // Pressure
  clearArea(92, 50, 64, 16, COL_BG);
  if (ok) {
    tft.setTextColor(COL_VALUE);
    tft.setCursor(92, 52);
    char buf[24]; snprintf(buf, sizeof(buf), "%.1f hPa", pval);
    tft.print(buf);
  } else {
    tft.setTextColor(COL_ERROR);
    tft.setCursor(92, 52);
    tft.print("ERR");
  }

  // Altitude
  clearArea(92, 72, 64, 16, COL_BG);
  if (ok) {
    tft.setTextColor(COL_VALUE);
    tft.setCursor(92, 74);
    char buf[24]; snprintf(buf, sizeof(buf), "%.0f m", aval);
    tft.print(buf);
  } else {
    tft.setTextColor(COL_ERROR);
    tft.setCursor(92, 74);
    tft.print("ERR");
  }
}

/* ============================
   Screen: API Weather (Open-Meteo)
   ============================ */

void showAPISummaryWeather() {
  tft.fillScreen(COL_BG);
  drawHeader("API WEATHER - Mindanao");
  tft.setTextSize(1);
  tft.setTextColor(COL_TEXT);
  tft.setCursor(8, 30);
  tft.print("Fetching current weather...");
  

  // Build URL
  char url[256];
  snprintf(url, sizeof(url), "%s?latitude=%.4f&longitude=%.4f&current_weather=true", OPEN_METEO_BASE, OPEN_METEO_LAT, OPEN_METEO_LON);
  Serial.printf("Open-Meteo URL: %s\n", url);

  HTTPClient http;
  http.begin(url);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("Open-Meteo HTTP error: %d\n", code);
    tft.fillScreen(COL_BG);
    drawHeader("API WEATHER - Error");
    tft.setTextSize(1);
    tft.setTextColor(COL_ERROR);
    tft.setCursor(8, 36);
    tft.print("Failed to fetch API");
    drawFooter("SELECT: Back   BACK: Menu");
    http.end();
    return;
  }

  String payload = http.getString();
  http.end();

  StaticJsonDocument<1024> doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.println("Open-Meteo JSON parse error");
    tft.fillScreen(COL_BG);
    drawHeader("API WEATHER - Error");
    tft.setTextSize(1);
    tft.setTextColor(COL_ERROR);
    tft.setCursor(8, 36);
    tft.print("JSON parse error");
    drawFooter("SELECT: Back   BACK: Menu");
    return;
  }

  JsonObject cw = doc["current_weather"];
  if (!cw) {
    tft.fillScreen(COL_BG);
    drawHeader("API WEATHER - No Data");
    tft.setTextSize(1);
    tft.setTextColor(COL_ERROR);
    tft.setCursor(8, 36);
    tft.print("No current weather");
    drawFooter("SELECT: Back   BACK: Menu");
    return;
  }

  double temp = cw["temperature"] | NAN;
  int wcode = cw["weathercode"] | -1;
  double wspd = cw["windspeed"] | NAN;
  double wdir = cw["winddirection"] | NAN;

  String summary = weatherCodeToStatus(wcode);

  tft.fillScreen(COL_BG);
  drawHeader("API WEATHER - Mindanao");
  tft.setTextSize(1);
  tft.setTextColor(COL_TEXT);
  tft.setCursor(8, 28); tft.print("Summary:");
  tft.setCursor(8, 44); tft.print("Temp:");
  tft.setCursor(8, 60); tft.print("Wind:");
  tft.setTextColor(COL_ACCENT);
  tft.setCursor(68, 28); tft.print(summary);
  tft.setTextColor(COL_VALUE);
  tft.setCursor(68, 44);
  if (!isnan(temp)) {
    char buf[32]; snprintf(buf, sizeof(buf), "%.1f C( %d)", temp, wcode);
    tft.print(buf);
  } else tft.print("N/A");
  tft.setCursor(68, 60);
  if (!isnan(wspd)) {
    char buf[32]; snprintf(buf, sizeof(buf), "%.1f(%.0f deg)", wspd, wdir);
    tft.print(buf);
  } else tft.print("N/A");

 
}

/* weatherCodeToStatus mapping */
String weatherCodeToStatus(int code) {
  if (code == 0) return "Clear";
  if (code == 1) return "Mainly Clear";
  if (code == 2) return "Partly Cloudy";
  if (code == 3) return "Overcast";
  if (code == 45 || code == 48) return "Fog";
  if (code == 51 || code == 53 || code == 55) return "Drizzle";
  if (code == 56 || code == 57) return "Freezing Drizzle";
  if (code == 61 || code == 63 || code == 65) return "Rain";
  if (code == 66 || code == 67) return "Freezing Rain";
  if (code == 71 || code == 73 || code == 75) return "Snow";
  if (code == 77) return "Snow Grains";
  if (code == 80 || code == 81 || code == 82) return "Showers";
  if (code == 85 || code == 86) return "Snow Showers";
  if (code == 95) return "Thunderstorm";
  if (code == 96 || code == 99) return "Thunder + Hail";
  return "Unknown";
}

/* ============================
   Geolocation (IP)
   ============================ */

void showGeolocation() {
  tft.fillScreen(COL_BG);
  drawHeader("GEOLOCATION");
  if (WiFi.status() != WL_CONNECTED) {
    tft.setTextSize(1);
    tft.setTextColor(COL_ERROR);
    tft.setCursor(8, 36);
    tft.print("Wi-Fi not connected");
    
    return;
  }

  tft.setTextSize(1);
  tft.setTextColor(COL_TEXT);
  tft.setCursor(8, 36);
  tft.print("Fetching location...");
  

  HTTPClient http;
  http.begin("https://geolocation-db.com/json/");
  int code = http.GET();
  if (code != 200) {
    Serial.printf("Geo HTTP error: %d\n", code);
    tft.fillScreen(COL_BG);
    drawHeader("GEOLOCATION");
    tft.setTextSize(1);
    tft.setTextColor(COL_ERROR);
    tft.setCursor(8, 36);
    tft.print("Failed to fetch location");
    
    http.end();
    return;
  }
  String payload = http.getString();
  http.end();

  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.println("Geo JSON parse error");
    tft.fillScreen(COL_BG);
    drawHeader("GEOLOCATION");
    tft.setTextSize(1);
    tft.setTextColor(COL_ERROR);
    tft.setCursor(8, 36);
    tft.print("JSON parse error");
  
    return;
  }

  const char* country = doc["country_name"] | "Unknown";
  const char* region = doc["region"] | doc["state"] | "Unknown";
  const char* city = doc["city"] | "Unknown";
  double lat = doc["latitude"] | 0.0;
  double lon = doc["longitude"] | 0.0;

  tft.fillScreen(COL_BG);
  drawHeader("GEOLOCATION");
  tft.setTextSize(1);
  tft.setTextColor(COL_TEXT);
  tft.setCursor(8, 28); tft.print("Country:");
  tft.setCursor(8, 44); tft.print("Region:");
  tft.setCursor(8, 60); tft.print("City:");
  tft.setCursor(8, 76); tft.print("Latitude:");
  tft.setCursor(8, 92); tft.print("Longitude:");

  tft.setTextColor(COL_VALUE);
  tft.setCursor(78, 28); tft.print(country);
  tft.setCursor(78, 44); tft.print(region);
  tft.setCursor(78, 60); tft.print(city);
  tft.setCursor(78, 76); tft.printf("%.4f", lat);
  tft.setCursor(78, 92); tft.printf("%.4f", lon);
 
}

/* ============================
   Wi-Fi Scan & Connect
   ============================ */

void startWifiScanAndShow() {
  tft.fillScreen(COL_BG);
  drawHeader("WIFI SCAN");
  tft.setTextSize(1);
  tft.setTextColor(COL_TEXT);
  tft.setCursor(8, 36);
  tft.print("Scanning networks...");
  
  delay(80);

  scannedCount = WiFi.scanNetworks(false, true);
  if (scannedCount > 31) scannedCount = 31;
  if (scannedCount <= 0) {
    scannedCount = 1;
    scannedSSIDs[0] = "No networks found";
  } else {
    for (int i = 0; i < scannedCount; ++i) scannedSSIDs[i] = WiFi.SSID(i);
  }
  wifiListIndex = 0;
  showWiFiScan();
}

void showWiFiScan() {
  tft.fillScreen(COL_BG);
  drawHeader("WIFI - Select Network");
  tft.setTextSize(1);
  int baseY = 26;
  const int maxRows = 4;
  int startIndex = wifiListIndex - maxRows / 2;
  if (startIndex < 0) startIndex = 0;
  if (startIndex + maxRows > scannedCount) startIndex = max(0, scannedCount - maxRows);

  for (int row = 0; row < maxRows; ++row) {
    int idx = startIndex + row;
    int y = baseY + row * 22;
    if (idx >= scannedCount) {
      clearArea(6, y - 1, SCREEN_W - 12, 18, COL_BG);
      continue;
    }
    if (idx == wifiListIndex) {
      tft.fillRect(6, y - 1, SCREEN_W - 12, 18, COL_HIGHLIGHT);
      tft.setTextColor(ST77XX_BLACK);
    } else {
      tft.fillRect(6, y - 1, SCREEN_W - 12, 18, COL_BG);
      tft.setTextColor(COL_TEXT);
    }
    tft.setCursor(12, y + 2);
    tft.print(scannedSSIDs[idx]);
  }
  drawFooter("Pssst");
}



/* ============================
   Keyboard UI
   ============================ */

void showKeyboard() {
  tft.fillScreen(COL_BG);
  drawHeader("Keyboard");

  tft.setTextSize(1);
  tft.setTextColor(COL_TEXT);

  // ✅ single short instruction
  tft.setCursor(6, 22);
  tft.print(editingSSID ? "Edit SSID: SEL=add" : "Edit PASS: SEL=add");

  // ✅ Show current input above keyboard
  tft.setTextColor(COL_ACCENT);
  tft.setCursor(6, 36);
  if (editingSSID) {
    String s = "SSID: " + manualSSID;
    if (s.length() > 20) s = s.substring(s.length() - 20);
    tft.print(s);
  } else {
    String s = "PWD: " + manualPassword;
    if (s.length() > 20) s = s.substring(s.length() - 20);
    tft.print(s);
  }

  // ✅ Move keyboard higher (from y=78 → y=54)
  int gridX = 6;
  int gridY = 54;
  int cellW = (SCREEN_W - 12) / KEYBOARD_COLS;
  if (cellW < 10) cellW = 10;  // safe min
  int cellH = 12;

  for (int r = 0; r < KEYBOARD_ROWS; ++r) {
    for (int c = 0; c < KEYBOARD_COLS; ++c) {
      int x = gridX + c * cellW;
      int y = gridY + r * (cellH + 2);
      bool cursorHere = (r == kbRow && c == kbCol);
      uint16_t bg = cursorHere ? COL_HIGHLIGHT : COL_BG;
      tft.fillRect(x, y, cellW - 1, cellH, bg);
      tft.setTextSize(1);
      tft.setTextColor(cursorHere ? ST77XX_BLACK : COL_TEXT);

      // Determine which character to draw
      char ch = keyboardCells[r][c];
      if (r == KEYBOARD_ROWS - 1 && c >= 8) {
        int symIndex = c - 8;
        if (symIndex < (int)strlen(extraSymbols)) ch = extraSymbols[symIndex];
        else ch = ' ';
      }

      tft.setCursor(x + 2, y + 2);
      tft.print(ch);
    }
  }
 
}

/* ============================
   Start WiFi Connect (non-blocking)
   ============================ */

void startWiFiConnect(const String &ssid, const String &pwd) {
  currentScreen = SCREEN_WIFI_CONNECTING;
  wifiConnectState = WFC_CONNECTING;
  wifiConnectStart = millis();

  tft.fillScreen(COL_BG);
  drawHeader("CONNECTING - Wi-Fi");
  tft.setTextSize(1);
  tft.setTextColor(COL_TEXT);
  tft.setCursor(6, 28); tft.print("SSID:");
  tft.setTextColor(COL_ACCENT);
  tft.setCursor(6, 40); tft.print(ssid);
  tft.setTextColor(COL_TEXT);
  tft.setCursor(6, 60); tft.print("Attempting to connect...");
  drawFooter("BACK: Cancel");

  // Save to preferences (we also save when user explicitly confirmed keyboard but double-save is okay)
  preferences.putString(PREF_SSID_KEY, ssid);
  preferences.putString(PREF_PWD_KEY, pwd);
  Serial.println("Saved credentials to Preferences before connecting.");

  WiFi.disconnect(true);
  delay(120);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pwd.c_str());
  // non-blocking: main loop polls status and timeout
}

/* ============================
   Show WiFi Connect Result
   ============================ */

void showWiFiConnectResult(bool ok, const String &msg) {
  currentScreen = SCREEN_WIFI_RESULT;
  tft.fillScreen(COL_BG);
  drawHeader("WIFI RESULT");
  tft.setTextSize(1);
  tft.setTextColor(ok ? COL_GOOD : COL_ERROR);
  tft.setCursor(6, 30);
  tft.print(ok ? "Connected!" : "Connection Failed");
  tft.setTextColor(COL_TEXT);
  tft.setCursor(6, 46);
  tft.print(msg);
 
}

/* ============================
   Info Screen
   ============================ */

void showInfo() {
  tft.fillScreen(COL_BG);
  drawHeader("INFO / ABOUT");
  tft.setTextSize(1);
  tft.setTextColor(COL_TEXT);
  tft.setCursor(8, 28); tft.print("Portable Weather Station");
  tft.setCursor(8, 40); tft.print("Board: ESP32 / ESP32-S3");
  tft.setCursor(8, 52); tft.print("Sensor: BMP280 (I2C)");
  tft.setCursor(8, 64); tft.print("TFT: ST7735 160x128");
  tft.setCursor(8, 76); tft.print("API Weather: Open-Meteo");
  tft.setCursor(8, 88); tft.print("Thinker: Joshua C. Godilos");
  
}

/* ============================
   End of Sketch (notes & tips)
   ============================ */

/*
  Additional notes:
  - The keyboard layout and mapping are intentionally designed to use only 4 buttons.
    If you later add a 5th button (e.g., a dedicated RIGHT), mapping could become more intuitive.
  - The keyboard short/long press model:
       - Short SELECT = move right
       - Short BACK = move left
       - Long SELECT = insert char
       - Long BACK = delete char
       - Long SELECT + LONG BACK together = confirm (save & connect)
    This allows all editing operations using existing buttons.
  - Preferences is used to persist credentials. The manual keyboard confirm action saves credentials
    and the startWiFiConnect() also saves credentials preemptively.
  - Rewiring DOWN to GPIO32: ensure you rewire your physical DOWN button to GPIO32.
  - If you want the keyboard to toggle case or access more symbols, I can add:
       - a shift/paging mode (press both to toggle case)
       - slow auto-repeat of cursor movement
  - Performance: HTTP calls are blocking when fetching API data; they are short operations and acceptable here.
  - If you want completely non-blocking network fetches, I can restructure into asynchronous state machine.

  Add new Features
  1) Add "Delete saved Wi-Fi creds" menu item (Preferences clear)
  2) Add NEO-6M GPS integration and prefer GPS coords for Geolocation/API Weather
  3) Improve keyboard pages (symbols, uppercase separately), or add a dedicated RIGHT button mapping
  4) Add OTA / captive portal for easier Wi-Fi management


*/

