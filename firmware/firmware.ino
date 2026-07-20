#include <WiFi.h>
#include <WiFiUdp.h>
#include <time.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <XPT2046_Touchscreen.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <TJpg_Decoder.h>
#include <string.h>
#include <Preferences.h>
#include <LittleFS.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans18pt7b.h>

// ==========================================
//              DEFINES & COLORS
// ==========================================
#define AESTHETIC_GOLD 0xCE40
#define SPOTIFY_GREEN  0x1EDB
#define MENU_BG        0x2104
#define UI_BG          0x0008
#define UI_PANEL       0x1084
#define UI_LINE        0x4208
#define UI_ORANGE      0xFD20
#define CANVAS_BG      0xFFFF

#define SPI_SCK  7
#define SPI_MISO 44
#define SPI_MOSI 9
#define TFT_CS   2
#define TFT_DC   4
#define TFT_RST  1
#define TFT_BL   43
#define TOUCH_CS 5

// ==========================================
//              APP STATE
// ==========================================
enum AppState { DASHBOARD, TIMER, STOPWATCH, SPOTIFY, PC_STATS, TASKS, WIFI_SCAN, WIFI_PASSWORD, WIFI_SAVED_ACTION, DRAWING, TOUCH_CALIB };
volatile AppState currentApp = DASHBOARD;
bool isMenuOpen = false;
int settingsScrollOff = 0;

enum FetchState : uint8_t { FETCH_IDLE, FETCH_SPOTIFY, FETCH_WEATHER };
volatile bool spotifyFetchPending = false;
volatile bool weatherFetchPending = false;
volatile unsigned long spotifyFetchReadyAt = 0;

// ==========================================
//              HARDWARE
// ==========================================
Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);
XPT2046_Touchscreen touch(TOUCH_CS);
WiFiUDP telemetryUdp;
SemaphoreHandle_t dataMutex;
TaskHandle_t networkTaskHandle = NULL;
Preferences prefs;

// ==========================================
//              WIFI MANAGER STATE
// ==========================================
#define MAX_SAVED_NETWORKS 3
#define MAX_SCAN_RESULTS 10

struct SavedNetwork { char ssid[33]; char password[65]; };
SavedNetwork savedNets[MAX_SAVED_NETWORKS];
int savedNetCount = 0;

struct ScanResult { char ssid[33]; int32_t rssi; bool encrypted; };
ScanResult scanResults[MAX_SCAN_RESULTS];
int scanCount = 0;
int wifiSelIdx = -1;
char wifiPwdBuf[65] = "";
int wifiPwdLen = 0;
bool kbShift = false;
bool kbSymbol = false;
bool kbShowPwd = false;
int wifiScrollOff = 0;
bool wifiFirstBoot = false;

// ==========================================
//              TOUCH CALIBRATION
// ==========================================
int calRawMinX = 3900, calRawMaxX = 120;
int calRawMinY = 3900, calRawMaxY = 120;
int calStep = 0;
long calAccX = 0, calAccY = 0;
int calSamples = 0;

// ==========================================
//              DRAWING STATE
// ==========================================
#define CANVAS_XOFF 0
#define CANVAS_YOFF 36
#define CANVAS_W 320
#define CANVAS_H 164
#define DRAW_TB_Y 200
#define DRAW_TB_H 40

const uint16_t DRAW_COLORS[] = {
  ILI9341_BLACK, ILI9341_WHITE, 0xF800, 0x001F,
  0x07E0, 0xFFE0, UI_ORANGE, ILI9341_MAGENTA
};
#define DRAW_NCOLORS 8
const int DRAW_BRUSH_R[] = {2, 5, 10};
#define DRAW_NBRUSH 3

int drawColIdx = 0;
int drawBrushIdx = 0;
bool drawEraser = false;
bool drawMenuVisible = false;
int drawLastTX = -1, drawLastTY = -1;
uint16_t* canvasBuf = NULL;
bool canvasSaveOk = false;

// ==========================================
//              API CREDENTIALS
// ==========================================
String weatherApiKey = "028d03bd0ba9f42113631862f8ad1702";
String city = "Ahmedabad";
String countryCode = "IN";

String spotifyRefreshToken = "AQA2on0_-AbgkmolE3XrMppYDFaOGF85dztSkV8ofIkcCDEogG5VYhMXtPssBPMqmE4-fxR2WxIsRrvyMN_f6op3EBHQBUTKbLk-BEa5wuslFAPX-TLICcqNm0XH8y_eW0A";
String spotifyBase64Auth = "MWFkZDI4MGZlY2M1NGU0YWFiYWM5N2UzY2I4YTAzNmI6YjE3M2JiZTRhNzkwNGIzYmE3YzM1MDJiYTUzYThmOTE=";

String todoistToken = "aa48cfbec036a743802939b523f3c3fd95f7fa07";

// ==========================================
//              SPOTIFY STATE
// ==========================================
String spotifyAccessToken = "";
String currentSong = "Loading...";
String currentArtist = "";
String currentAlbumUrl = "";
bool isPlaying = false;

volatile bool spotifyContentDirty = true;
volatile bool spotifyControlsDirty = true;
volatile bool spotifyAlbumDirty = false;
volatile bool spotifyControlPending = false;
char spotifyControlAction[12] = "";
unsigned long lastSpotifyTokenRefresh = 0;
unsigned long lastSpotifyDataFetch = 0;

#define ALBUM_ART_MAX_SIZE 12000
static uint8_t imgBuff[ALBUM_ART_MAX_SIZE];
static uint8_t imgDownloadBuff[ALBUM_ART_MAX_SIZE];
size_t imgBuffSize = 0;
String cachedAlbumUrl = "";

// ==========================================
//              WEATHER / TELEMETRY / TASKS
// ==========================================
float currentTemp = 0.0;
int currentHumidity = 0;
String currentWeather = "--";
volatile bool weatherDirty = false;
unsigned long lastWeatherUpdate = 0;

float pcCpuUsage = 0.0;
float pcGpuTemp = -1.0;
float pcRamUsage = 0.0;
float pcWifiMbps = 0.0;
unsigned long lastTelemetryMillis = 0;
volatile bool telemetryDirty = true;

String taskItems[3] = {"Add Todoist token", "Open Tasks", "Stay focused"};
String taskIds[3] = {"", "", ""};
int taskCount = 3;
volatile bool tasksDirty = true;
volatile bool taskFetchPending = false;
volatile bool taskCompletePending = false;
String taskCompleteId = "";
unsigned long lastTaskFetch = 0;

// ==========================================
//              TIME / POWER / TIMER
// ==========================================
const char* ntpServer  = "pool.ntp.org";
const char* ntpServer2 = "time.google.com";
const char* ntpServer3 = "time.cloudflare.com";
const long  gmtOffset_sec = 19800;
const int   daylightOffset_sec = 0;
unsigned long lastNTPSync = 0;
char lastTopClock[10] = "";
int lastDashboardMinute = -1;

int currentBrightness = 155;
const int MIN_BRIGHTNESS = 20;
const int MAX_BRIGHTNESS = 230;
const int DIM_BRIGHTNESS = 28;
volatile bool screenAwake = true;
bool screenDimmed = false;
unsigned long lastInteractionMillis = 0;
unsigned long lastTouchTime = 0;

long timerSeconds = 25 * 60;
bool timerRunning = false;
unsigned long lastTimerTick = 0;
unsigned long swStartMillis = 0;
unsigned long swElapsedMillis = 0;
bool swRunning = false;

// ==========================================
//              MARQUEE
// ==========================================
GFXcanvas16* marqueeCanvas;
int marqueeX = 0;
int songPixelWidth = 0;
bool needsScrolling = false;
unsigned long lastMarqueeUpdate = 0;
unsigned long lastBeatUpdate = 0;
char marqueeSong[96] = "Loading...";

// ==========================================
//              TIMING CONSTANTS
// ==========================================
const unsigned long NTP_SYNC_INTERVAL_MS = 900000UL;
const unsigned long SPOTIFY_TOKEN_INTERVAL_MS = 3000000UL;
const unsigned long SPOTIFY_FETCH_INTERVAL_MS = 4500;
const unsigned long WEATHER_FETCH_INTERVAL_MS = 600000UL;
const unsigned long TASK_FETCH_INTERVAL_MS = 900000UL;
const unsigned long MARQUEE_INTERVAL_MS = 24;
const unsigned long BEAT_VISUALIZER_INTERVAL_MS = 90;
const unsigned long TOUCH_DEBOUNCE_MS = 45;
const unsigned long CLOCK_CHECK_INTERVAL_MS = 250;
const unsigned long STOPWATCH_DRAW_INTERVAL_MS = 100;
const unsigned long SPOTIFY_CONTROL_SETTLE_MS = 250;
const unsigned long SCREEN_DIM_TIMEOUT_MS = 30000UL;
const unsigned long SCREEN_SLEEP_TIMEOUT_MS = 120000UL;
const uint16_t API_HTTP_TIMEOUT_MS = 4500;
const uint16_t IMAGE_HTTP_TIMEOUT_MS = 7000;
const uint16_t TELEMETRY_PORT = 4210;
const unsigned long TELEMETRY_STALE_MS = 10000UL;
const int MARQUEE_X = 12;
const int MARQUEE_Y = 72;
const int MARQUEE_VIEW_WIDTH = 210;
const int MARQUEE_VIEW_HEIGHT = 35;
const int MARQUEE_GAP_PX = 48;
const int MARQUEE_TEXT_PADDING_PX = 18;

// ==========================================
//              FORWARD DECLARATIONS
// ==========================================
bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap);
static inline bool elapsedSince(unsigned long now, unsigned long last, unsigned long interval);
static inline bool timeReached(unsigned long now, unsigned long target);
void lockData();
void unlockData();
void queueFetch(FetchState fetchType, unsigned long delayMs);
void queueFetch(FetchState fetchType);
void queueSpotifyControl(const char* action);
void networkTask(void* pvParameters);
void refreshSpotifyToken();
void fetchSpotifyData();
void sendSpotifyControl(const char* action);
void fetchAlbumArtToCache(const String& url);
void fetchWeatherData();
void pollTelemetryUdp();
void fetchTodoistTasks();
void completeTodoistTask(const String& taskId);
void applyNetworkUpdates();
void controlSpotify(const char* action);
bool syncTimeNow(bool waitForSync);
void initDisplaySafely();
void setBacklight(int value);
void fadeBacklightTo(int target);
void wakeScreen();
void sleepScreen();
void updatePowerState(unsigned long now);
bool shouldKeepScreenLive();
bool readTouchPoint(int& x, int& y);
void handleTouch(int x, int y);
void drawTopBar(const char* title, uint16_t accent, bool showBack);
void updateTopClock(bool force);
void drawCenteredText(const char* text, int16_t x, int16_t y, int16_t w, int16_t h, const GFXfont* font, uint16_t color);
void fitTextToWidth(char* text, uint16_t maxWidth, const GFXfont* font);
bool hitRect(int x, int y, int rx, int ry, int rw, int rh);
void refreshTelemetryStaleUi(unsigned long now);
void redrawCurrentApp();
void redrawEntireUI();
void drawDashboardUI();
void drawStaticUI();
void drawPcStatsPreview(bool force);
void drawTasksPreview(bool force);
void updateDashboardClock(struct tm* timeinfo, bool force);
void updateDateDisplay(struct tm* timeinfo);
void updateWeatherDisplay();
void openSettingsMenu();
void closeSettingsMenu();
void drawSpotifyUI();
void drawSpotifyControls();
void drawAlbumArtPlaceholder();
void drawCachedAlbumArt();
void updateSongMetrics();
void updateSpotifyText(bool force);
void drawMarquee();
void drawBeatVisualizer(bool force);
void drawFocusAppUI();
void updateTimerDisplay();
void updateStopwatchDisplay();
void drawPcStatsUI();
void drawPcStatsValues(bool force);
void drawTasksUI();
void requestTaskComplete(int index);
void removeLocalTaskAt(int index);
// WiFi Manager
void loadSavedNetworks();
void saveSavedNetworks();
bool tryConnectSaved(bool drawUI = true);
void startWifiScan();
void drawWifiScanUI();
void drawWifiSavedActionUI();
void handleWifiSavedActionTouch(int x, int y);
void drawWifiKeyboardUI();
void drawWifiPasswordField();
void drawWifiKeyboard();
char getKeyAt(int row, int col);
void handleWifiScanTouch(int x, int y);
void handleWifiKbTouch(int x, int y);
void wifiTypeChar(char c);
void wifiBackspace();
void wifiConnect();
void onWifiConnected();
// Drawing
void initCanvasBuffer();
void drawDrawingUI();
void drawDrawingToolbar();
void drawDrawingMenuOverlay();
void canvasDrawDot(int cx, int cy, int r, uint16_t color);
void canvasDrawLine(int x0, int y0, int x1, int y1, int r, uint16_t color);
void canvasClear();
void saveDrawing(int slot);
void loadDrawing(int slot);
bool drawingSlotExists(int slot);
void handleDrawingTouch(int x, int y);
void handleDrawingDrag(int x, int y);
void handleDrawingMenuTouch(int x, int y);
// Touch Calibration
void loadCalibration();
void saveCalibration();
void drawCalibrationUI();
void handleCalibTouch();

// ==========================================
//              UTILITY FUNCTIONS
// ==========================================
bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  if (y >= tft.height()) return 0;
  tft.drawRGBBitmap(x, y, bitmap, w, h);
  return 1;
}

static inline bool elapsedSince(unsigned long now, unsigned long last, unsigned long interval) {
  return (unsigned long)(now - last) >= interval;
}

static inline bool timeReached(unsigned long now, unsigned long target) {
  return target == 0 || (long)(now - target) >= 0;
}

void drawCenteredText(const char* text, int16_t x, int16_t y, int16_t w, int16_t h, const GFXfont* font, uint16_t color) {
  tft.setFont(font);
  tft.setTextColor(color);
  int16_t x1, y1;
  uint16_t tw, th;
  tft.getTextBounds(text, 0, 0, &x1, &y1, &tw, &th);
  int16_t cx = x + (w - (int16_t)tw) / 2 - x1;
  int16_t cy = y + (h - (int16_t)th) / 2 - y1;
  tft.setCursor(cx, cy);
  tft.print(text);
}

void fitTextToWidth(char* text, uint16_t maxWidth, const GFXfont* font) {
  tft.setFont(font);
  size_t len = strlen(text);
  while (len > 0) {
    int16_t x1, y1;
    uint16_t tw, th;
    tft.getTextBounds(text, 0, 0, &x1, &y1, &tw, &th);
    if (tw <= maxWidth) return;
    text[--len] = '\0';
  }
}

bool hitRect(int x, int y, int rx, int ry, int rw, int rh) {
  const int pad = 10;
  return x >= rx - pad && x <= rx + rw + pad && y >= ry - pad && y <= ry + rh + pad;
}

void lockData() {
  if (dataMutex) xSemaphoreTake(dataMutex, portMAX_DELAY);
}

void unlockData() {
  if (dataMutex) xSemaphoreGive(dataMutex);
}

void queueFetch(FetchState fetchType, unsigned long delayMs) {
  if (fetchType == FETCH_SPOTIFY) {
    spotifyFetchPending = true;
    spotifyFetchReadyAt = (delayMs > 0) ? millis() + delayMs : 0;
  } else if (fetchType == FETCH_WEATHER) {
    weatherFetchPending = true;
  }
}

void queueFetch(FetchState fetchType) {
  queueFetch(fetchType, 0);
}

void queueSpotifyControl(const char* action) {
  lockData();
  strncpy(spotifyControlAction, action, sizeof(spotifyControlAction) - 1);
  spotifyControlAction[sizeof(spotifyControlAction) - 1] = '\0';
  spotifyControlPending = true;
  unlockData();
}

// ==========================================
//              WIFI MANAGER
// ==========================================
void loadSavedNetworks() {
  prefs.begin("wifi", true);
  savedNetCount = prefs.getInt("count", 0);
  if (savedNetCount > MAX_SAVED_NETWORKS) savedNetCount = MAX_SAVED_NETWORKS;
  for (int i = 0; i < savedNetCount; i++) {
    char ks[8], kp[8];
    snprintf(ks, sizeof(ks), "s%d", i);
    snprintf(kp, sizeof(kp), "p%d", i);
    String s = prefs.getString(ks, "");
    String p = prefs.getString(kp, "");
    s.toCharArray(savedNets[i].ssid, 33);
    p.toCharArray(savedNets[i].password, 65);
  }
  prefs.end();
}

void saveSavedNetworks() {
  prefs.begin("wifi", false);
  prefs.putInt("count", savedNetCount);
  for (int i = 0; i < MAX_SAVED_NETWORKS; i++) {
    char ks[8], kp[8];
    snprintf(ks, sizeof(ks), "s%d", i);
    snprintf(kp, sizeof(kp), "p%d", i);
    if (i < savedNetCount) {
      prefs.putString(ks, savedNets[i].ssid);
      prefs.putString(kp, savedNets[i].password);
    } else {
      prefs.remove(ks);
      prefs.remove(kp);
    }
  }
  prefs.end();
}

bool tryConnectSaved(bool drawUI) {
  for (int i = 0; i < savedNetCount; i++) {
    if (drawUI) {
      tft.fillRect(20, 100, 280, 30, UI_BG);
      tft.setFont(&FreeSans9pt7b);
      tft.setTextColor(ILI9341_CYAN);
      tft.setCursor(20, 118);
      tft.print("Trying: ");
      tft.print(savedNets[i].ssid);
    }

    WiFi.disconnect();
    delay(100);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(savedNets[i].ssid, savedNets[i].password);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
      delay(300);
      attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) return true;
    WiFi.disconnect();
  }
  return false;
}

int getSavedNetworkIndex(const char* ssid) {
  for (int i = 0; i < savedNetCount; i++) {
    if (strcmp(savedNets[i].ssid, ssid) == 0) return i;
  }
  return -1;
}

bool isSavedNetwork(const char* ssid) {
  return getSavedNetworkIndex(ssid) != -1;
}

void removeSavedNetwork(int idx) {
  if (idx < 0 || idx >= savedNetCount) return;
  for (int i = idx; i < savedNetCount - 1; i++) {
    strcpy(savedNets[i].ssid, savedNets[i+1].ssid);
    strcpy(savedNets[i].password, savedNets[i+1].password);
  }
  savedNetCount--;
  saveSavedNetworks();
}

void addSavedNetwork(const char* ssid, const char* pwd) {
  // Check if already saved, move to front
  for (int i = 0; i < savedNetCount; i++) {
    if (strcmp(savedNets[i].ssid, ssid) == 0) {
      SavedNetwork tmp = savedNets[i];
      for (int j = i; j > 0; j--) savedNets[j] = savedNets[j - 1];
      savedNets[0] = tmp;
      strncpy(savedNets[0].password, pwd, 64);
      savedNets[0].password[64] = '\0';
      saveSavedNetworks();
      return;
    }
  }
  // Shift down, add at front
  if (savedNetCount < MAX_SAVED_NETWORKS) savedNetCount++;
  for (int i = savedNetCount - 1; i > 0; i--) savedNets[i] = savedNets[i - 1];
  strncpy(savedNets[0].ssid, ssid, 32);
  savedNets[0].ssid[32] = '\0';
  strncpy(savedNets[0].password, pwd, 64);
  savedNets[0].password[64] = '\0';
  saveSavedNetworks();
}

void startWifiScan() {
  tft.fillScreen(UI_BG);
  drawTopBar("WiFi", ILI9341_CYAN, true);
  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(ILI9341_DARKGREY);
  tft.setCursor(100, 140);
  tft.print("Scanning...");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  int n = WiFi.scanNetworks(false, true);
  if (n <= 0) {
    delay(500);
    n = WiFi.scanNetworks(false, true);
  }
  scanCount = 0;
  for (int i = 0; i < n && scanCount < MAX_SCAN_RESULTS; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue;
    // Deduplicate
    bool dup = false;
    for (int j = 0; j < scanCount; j++) {
      if (strcmp(scanResults[j].ssid, s.c_str()) == 0) { dup = true; break; }
    }
    if (dup) continue;
    s.toCharArray(scanResults[scanCount].ssid, 33);
    scanResults[scanCount].rssi = WiFi.RSSI(i);
    scanResults[scanCount].encrypted = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    scanCount++;
  }
  WiFi.scanDelete();
  wifiScrollOff = 0;
}

void drawWifiScanUI() {
  tft.fillScreen(UI_BG);
  drawTopBar("WiFi", ILI9341_CYAN, true);

  tft.setFont(&FreeSans9pt7b);
  tft.setCursor(16, 56);
  if (WiFi.status() == WL_CONNECTED) {
    tft.setTextColor(SPOTIFY_GREEN);
    tft.print("Connected: ");
    String ssidStr = WiFi.SSID();
    if (ssidStr.length() > 14) ssidStr = ssidStr.substring(0, 11) + "...";
    tft.print(ssidStr);
  } else {
    tft.setTextColor(ILI9341_DARKGREY);
    tft.print("Select a network");
  }

  int maxVisible = 4;
  for (int i = 0; i < maxVisible && (i + wifiScrollOff) < scanCount; i++) {
    int idx = i + wifiScrollOff;
    int y = 62 + i * 32;
    uint16_t bg = (idx % 2 == 0) ? 0x1084 : 0x0861;
    tft.fillRoundRect(10, y, 300, 28, 6, bg);

    // Signal strength bars
    int bars = 1;
    if (scanResults[idx].rssi > -50) bars = 4;
    else if (scanResults[idx].rssi > -65) bars = 3;
    else if (scanResults[idx].rssi > -80) bars = 2;
    for (int b = 0; b < 4; b++) {
      uint16_t c = (b < bars) ? ILI9341_CYAN : 0x2104;
      int bh = 6 + b * 4;
      tft.fillRect(16 + b * 6, y + 24 - bh, 4, bh, c);
    }

    // Lock icon for encrypted
    if (scanResults[idx].encrypted) {
      tft.drawRect(42, y + 10, 8, 8, AESTHETIC_GOLD);
      tft.drawRect(44, y + 6, 4, 6, AESTHETIC_GOLD);
    }

    // SSID name
    bool isSaved = isSavedNetwork(scanResults[idx].ssid);
    char ssidBuf[36];
    strncpy(ssidBuf, scanResults[idx].ssid, 27);
    ssidBuf[27] = '\0';
    if (isSaved) strcat(ssidBuf, " [Saved]");
    fitTextToWidth(ssidBuf, 220, &FreeSans9pt7b);
    tft.setFont(&FreeSans9pt7b);
    tft.setTextColor(isSaved ? SPOTIFY_GREEN : ILI9341_WHITE);
    tft.setCursor(56, y + 20);
    tft.print(ssidBuf);
  }

  if (scanCount == 0) {
    tft.setFont(&FreeSans9pt7b);
    tft.setTextColor(ILI9341_DARKGREY);
    tft.setCursor(80, 130);
    tft.print("No networks found");
  }

  // Massive UP/DOWN buttons
  tft.fillRoundRect(10, 196, 140, 36, 8, 0x2104);
  tft.fillTriangle(80, 204, 70, 220, 90, 220, ILI9341_WHITE);

  tft.fillRoundRect(170, 196, 140, 36, 8, 0x2104);
  tft.fillTriangle(240, 220, 230, 204, 250, 204, ILI9341_WHITE);
}

void drawWifiKeyboardUI() {
  tft.fillScreen(UI_BG);
  drawTopBar("WiFi", ILI9341_CYAN, true);

  // Show selected SSID
  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(ILI9341_DARKGREY);
  tft.setCursor(12, 56);
  tft.print("Network:");
  if (wifiSelIdx >= 0 && wifiSelIdx < scanCount) {
    tft.setTextColor(ILI9341_WHITE);
    tft.setCursor(100, 56);
    char ssidBuf[24];
    strncpy(ssidBuf, scanResults[wifiSelIdx].ssid, 23);
    ssidBuf[23] = '\0';
    tft.print(ssidBuf);
  }

  drawWifiPasswordField();
  drawWifiKeyboard();
}

void drawWifiPasswordField() {
  tft.fillRoundRect(10, 62, 300, 28, 6, 0x1084);
  tft.drawRoundRect(10, 62, 300, 28, 6, ILI9341_CYAN);
  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(16, 82);
  if (wifiPwdLen == 0) {
    tft.setTextColor(ILI9341_DARKGREY);
    tft.print("Enter password...");
  } else {
    char display[33];
    int showLen = wifiPwdLen > 30 ? 30 : wifiPwdLen;
    for (int i = 0; i < showLen; i++) {
      display[i] = kbShowPwd ? wifiPwdBuf[i] : '*';
    }
    display[showLen] = '\0';
    tft.print(display);
  }
  // Show/hide toggle
  tft.fillRoundRect(280, 64, 26, 24, 4, kbShowPwd ? ILI9341_CYAN : 0x3186);
  tft.setFont(NULL);
  tft.setTextColor(ILI9341_BLACK);
  tft.setCursor(286, 72);
  tft.print(kbShowPwd ? "H" : "S");
}

void drawWifiKeyboard() {
  // Keyboard background
  tft.fillRect(0, 92, 320, 148, 0x0861);

  const char* row1;
  const char* row2;
  const char* row3;
  int len3;

  if (kbSymbol) {
    row1 = "1234567890";
    row2 = "@#$_&-+()";
    row3 = "=*!?/:;,.";
    len3 = 9;
  } else if (kbShift) {
    row1 = "QWERTYUIOP";
    row2 = "ASDFGHJKL";
    row3 = "ZXCVBNM";
    len3 = 7;
  } else {
    row1 = "qwertyuiop";
    row2 = "asdfghjkl";
    row3 = "zxcvbnm";
    len3 = 7;
  }

  int kw = 28, kh = 28, px = 30, py = 32;
  int yBase = 96;

  // Row 1: 10 keys
  int x0 = (320 - 10 * px) / 2;
  for (int i = 0; i < 10; i++) {
    int kx = x0 + i * px;
    tft.fillRoundRect(kx, yBase, kw, kh, 4, 0x3186);
    char c[2] = {row1[i], 0};
    drawCenteredText(c, kx, yBase, kw, kh, &FreeSans9pt7b, ILI9341_WHITE);
  }

  // Row 2: 9 keys
  int y2 = yBase + py;
  x0 = (320 - 9 * px) / 2;
  for (int i = 0; i < 9; i++) {
    int kx = x0 + i * px;
    tft.fillRoundRect(kx, y2, kw, kh, 4, 0x3186);
    char c[2] = {row2[i], 0};
    drawCenteredText(c, kx, y2, kw, kh, &FreeSans9pt7b, ILI9341_WHITE);
  }

  // Row 3: shift + keys + backspace
  int y3 = yBase + 2 * py;
  tft.fillRoundRect(4, y3, 42, kh, 4, kbShift ? AESTHETIC_GOLD : 0x2925);
  drawCenteredText("^", 4, y3, 42, kh, &FreeSans9pt7b, kbShift ? ILI9341_BLACK : ILI9341_WHITE);

  int r3x0 = 50;
  for (int i = 0; i < len3; i++) {
    int kx = r3x0 + i * px;
    tft.fillRoundRect(kx, y3, kw, kh, 4, 0x3186);
    char c[2] = {row3[i], 0};
    drawCenteredText(c, kx, y3, kw, kh, &FreeSans9pt7b, ILI9341_WHITE);
  }
  tft.fillRoundRect(274, y3, 42, kh, 4, 0x480C);
  drawCenteredText("<", 274, y3, 42, kh, &FreeSans9pt7b, ILI9341_WHITE);

  // Row 4: mode + space + connect
  int y4 = yBase + 3 * py;
  tft.fillRoundRect(4, y4, 56, kh, 4, 0x2925);
  drawCenteredText(kbSymbol ? "ABC" : "123", 4, y4, 56, kh, &FreeSans9pt7b, ILI9341_WHITE);

  tft.fillRoundRect(64, y4, 146, kh, 4, 0x3186);
  drawCenteredText("SPACE", 64, y4, 146, kh, &FreeSans9pt7b, ILI9341_DARKGREY);

  tft.fillRoundRect(214, y4, 102, kh, 4, SPOTIFY_GREEN);
  drawCenteredText("ENTER", 214, y4, 102, kh, &FreeSans9pt7b, ILI9341_BLACK);
}

char getKeyAt(int row, int col) {
  const char* r1 = kbSymbol ? "1234567890" : (kbShift ? "QWERTYUIOP" : "qwertyuiop");
  const char* r2 = kbSymbol ? "@#$_&-+()" : (kbShift ? "ASDFGHJKL" : "asdfghjkl");
  const char* r3 = kbSymbol ? "=*!?/:;,." : (kbShift ? "ZXCVBNM" : "zxcvbnm");
  if (row == 0 && col >= 0 && col < 10) return r1[col];
  if (row == 1 && col >= 0 && col < 9) return r2[col];
  int len3 = kbSymbol ? 9 : 7;
  if (row == 2 && col >= 0 && col < len3) return r3[col];
  return 0;
}

void wifiTypeChar(char c) {
  if (c == 0 || wifiPwdLen >= 63) return;
  wifiPwdBuf[wifiPwdLen++] = c;
  wifiPwdBuf[wifiPwdLen] = '\0';
  drawWifiPasswordField();
}

void wifiBackspace() {
  if (wifiPwdLen > 0) {
    wifiPwdBuf[--wifiPwdLen] = '\0';
    drawWifiPasswordField();
  }
}

void handleWifiScanTouch(int x, int y) {
  // Back button
  if (y < 36 && x < 60) {
    if (wifiFirstBoot) return; // Can't go back on first boot
    currentApp = DASHBOARD;
    redrawEntireUI();
    return;
  }

  // REFRESH Button
  if (y < 36 && x > 230) {
    startWifiScan();
    drawWifiScanUI();
    return;
  }

  // UP Button
  if (hitRect(x, y, 10, 196, 140, 36)) {
    if (wifiScrollOff > 0) wifiScrollOff--;
    drawWifiScanUI();
    return;
  }
  
  // DOWN Button
  if (hitRect(x, y, 170, 196, 140, 36)) {
    if (wifiScrollOff + 4 < scanCount) wifiScrollOff++;
    drawWifiScanUI();
    return;
  }

  // Network list
  for (int i = 0; i < 4; i++) {
    int idx = i + wifiScrollOff;
    if (idx >= scanCount) break;
    int ny = 62 + i * 32;
    if (y >= ny && y < ny + 28) {
      wifiSelIdx = idx;
      wifiPwdLen = 0;
      wifiPwdBuf[0] = '\0';
      kbShift = false;
      kbSymbol = false;
      kbShowPwd = false;

      // Check if saved
      if (isSavedNetwork(scanResults[idx].ssid)) {
        currentApp = WIFI_SAVED_ACTION;
        drawWifiSavedActionUI();
      } else if (!scanResults[idx].encrypted) {
        wifiConnect();
      } else {
        currentApp = WIFI_PASSWORD;
        drawWifiKeyboardUI();
      }
      return;
    }
  }
}

void drawWifiSavedActionUI() {
  tft.fillScreen(UI_BG);
  drawTopBar("NETWORK", ILI9341_CYAN, true);

  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(20, 80);
  tft.print("Saved Network:");
  
  tft.setTextColor(SPOTIFY_GREEN);
  tft.setCursor(20, 110);
  tft.print(scanResults[wifiSelIdx].ssid);

  // CONNECT button
  tft.fillRoundRect(20, 140, 130, 48, 8, SPOTIFY_GREEN);
  drawCenteredText("CONNECT", 20, 140, 130, 48, &FreeSans9pt7b, ILI9341_BLACK);

  // FORGET button
  tft.fillRoundRect(170, 140, 130, 48, 8, ILI9341_RED);
  drawCenteredText("FORGET", 170, 140, 130, 48, &FreeSans9pt7b, ILI9341_WHITE);
}

void handleWifiSavedActionTouch(int x, int y) {
  // Back
  if (y < 36 && x < 60) {
    currentApp = WIFI_SCAN;
    drawWifiScanUI();
    return;
  }
  // Connect
  if (hitRect(x, y, 20, 140, 130, 48)) {
    int savedIdx = getSavedNetworkIndex(scanResults[wifiSelIdx].ssid);
    if (savedIdx != -1) {
      strcpy(wifiPwdBuf, savedNets[savedIdx].password);
      wifiConnect();
    }
    return;
  }
  // Forget
  if (hitRect(x, y, 170, 140, 130, 48)) {
    int savedIdx = getSavedNetworkIndex(scanResults[wifiSelIdx].ssid);
    if (savedIdx != -1) {
      removeSavedNetwork(savedIdx);
    }
    currentApp = WIFI_SCAN;
    drawWifiScanUI();
    return;
  }
}

void handleWifiKbTouch(int x, int y) {
  // Back button (top bar)
  if (y < 36 && x < 60) {
    currentApp = WIFI_SCAN;
    drawWifiScanUI();
    return;
  }

  // Show/Hide password toggle
  if (hitRect(x, y, 260, 50, 60, 50)) {
    kbShowPwd = !kbShowPwd;
    drawWifiPasswordField();
    return;
  }

  int kw = 28, px = 30, py = 32;
  int yBase = 96;

  // Row 1
  if (y >= yBase && y < yBase + 28) {
    int x0 = (320 - 10 * px) / 2;
    int col = (x - x0) / px;
    if (col >= 0 && col < 10 && x >= x0) {
      char c = getKeyAt(0, col);
      wifiTypeChar(c);
      return;
    }
  }

  // Row 2
  int y2 = yBase + py;
  if (y >= y2 && y < y2 + 28) {
    int x0 = (320 - 9 * px) / 2;
    int col = (x - x0) / px;
    if (col >= 0 && col < 9 && x >= x0) {
      char c = getKeyAt(1, col);
      wifiTypeChar(c);
      return;
    }
  }

  // Row 3
  int y3 = yBase + 2 * py;
  if (y >= y3 && y < y3 + 28) {
    // Shift
    if (x < 46) {
      kbShift = !kbShift;
      kbSymbol = false;
      drawWifiKeyboard();
      return;
    }
    // Backspace
    if (x >= 274) {
      wifiBackspace();
      return;
    }
    // Keys
    int len3 = kbSymbol ? 9 : 7;
    int col = (x - 50) / px;
    if (col >= 0 && col < len3) {
      char c = getKeyAt(2, col);
      wifiTypeChar(c);
      return;
    }
  }

  // Row 4
  int y4 = yBase + 3 * py;
  if (y >= y4 && y < y4 + 28) {
    // Mode toggle
    if (x < 60) {
      kbSymbol = !kbSymbol;
      kbShift = false;
      drawWifiKeyboard();
      return;
    }
    // Space
    if (x >= 64 && x < 210) {
      wifiTypeChar(' ');
      return;
    }
    // Connect
    if (x >= 214) {
      wifiConnect();
      return;
    }
  }
}

void wifiConnect() {
  if (wifiSelIdx < 0 || wifiSelIdx >= scanCount) return;

  tft.fillScreen(UI_BG);
  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(ILI9341_CYAN);
  tft.setCursor(60, 120);
  tft.print("Connecting to ");
  tft.print(scanResults[wifiSelIdx].ssid);
  tft.print("...");

  WiFi.disconnect();
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(scanResults[wifiSelIdx].ssid, wifiPwdBuf);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(250);
    attempts++;
    // Draw progress dots
    tft.setFont(NULL);
    tft.setTextColor(ILI9341_CYAN);
    tft.setCursor(60 + (attempts % 20) * 6, 140);
    tft.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    addSavedNetwork(scanResults[wifiSelIdx].ssid, wifiPwdBuf);
    onWifiConnected();
  } else {
    WiFi.disconnect();
    tft.fillScreen(UI_BG);
    drawTopBar("WiFi", ILI9341_RED, true);
    tft.setFont(&FreeSans9pt7b);
    tft.setTextColor(ILI9341_RED);
    tft.setCursor(60, 120);
    tft.print("Connection failed!");
    tft.setTextColor(ILI9341_DARKGREY);
    tft.setCursor(50, 150);
    tft.print("Check password & try again");
    delay(2000);
    currentApp = WIFI_SCAN;
    startWifiScan();
    drawWifiScanUI();
  }
}

void onWifiConnected() {
  tft.fillScreen(UI_BG);
  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(SPOTIFY_GREEN);
  tft.setCursor(80, 106);
  tft.print("WiFi Connected!");
  delay(600);

  telemetryUdp.begin(TELEMETRY_PORT);

  tft.fillScreen(UI_BG);
  tft.setTextColor(ILI9341_CYAN);
  tft.setCursor(20, 106);
  tft.print("Syncing time...");
  syncTimeNow(true);

  tft.fillScreen(UI_BG);
  tft.setTextColor(SPOTIFY_GREEN);
  tft.setCursor(20, 106);
  tft.print("Authorizing Spotify...");
  refreshSpotifyToken();
  lastSpotifyTokenRefresh = millis();

  fetchWeatherData();
  lastWeatherUpdate = millis();

  lastInteractionMillis = millis();
  wifiFirstBoot = false;
  currentApp = DASHBOARD;
  redrawEntireUI();

  if (networkTaskHandle == NULL) {
    xTaskCreatePinnedToCore(networkTask, "NetworkTask", 12288, NULL, 1, &networkTaskHandle, 0);
  }
}

// ==========================================
//              TOUCH CALIBRATION
// ==========================================
void loadCalibration() {
  prefs.begin("tcal", true);
  if (prefs.isKey("minx")) {
    int tempMinX = prefs.getInt("minx", 3900);
    int tempMaxX = prefs.getInt("maxx", 120);
    int tempMinY = prefs.getInt("miny", 3900);
    int tempMaxY = prefs.getInt("maxy", 120);
    
    // Validate calibration to prevent erratic touch if double-tapped
    if (abs(tempMaxX - tempMinX) > 500 && abs(tempMaxY - tempMinY) > 500) {
      calRawMinX = tempMinX;
      calRawMaxX = tempMaxX;
      calRawMinY = tempMinY;
      calRawMaxY = tempMaxY;
    }
  }
  prefs.end();
}

void saveCalibration() {
  prefs.begin("tcal", false);
  prefs.putInt("minx", calRawMinX);
  prefs.putInt("maxx", calRawMaxX);
  prefs.putInt("miny", calRawMinY);
  prefs.putInt("maxy", calRawMaxY);
  prefs.end();
}

void drawCalibrationUI() {
  tft.fillScreen(ILI9341_BLACK);
  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(ILI9341_WHITE);

  if (calStep == 0) {
    tft.setCursor(50, 120);
    tft.print("Tap the crosshair");
    tft.setCursor(50, 145);
    tft.print("at the TOP-LEFT corner");
    // Draw crosshair at (20, 20)
    tft.drawLine(10, 20, 30, 20, ILI9341_RED);
    tft.drawLine(20, 10, 20, 30, ILI9341_RED);
    tft.fillCircle(20, 20, 3, ILI9341_RED);
  } else if (calStep == 1) {
    tft.setCursor(30, 100);
    tft.print("Now tap the crosshair at");
    tft.setCursor(30, 125);
    tft.print("the BOTTOM-RIGHT corner");
    // Draw crosshair at (299, 219)
    tft.drawLine(289, 219, 309, 219, ILI9341_RED);
    tft.drawLine(299, 209, 299, 229, ILI9341_RED);
    tft.fillCircle(299, 219, 3, ILI9341_RED);
  } else {
    tft.setCursor(60, 120);
    tft.setTextColor(SPOTIFY_GREEN);
    tft.print("Calibration saved!");
    delay(1000);
    currentApp = DASHBOARD;
    redrawEntireUI();
  }
}

void handleCalibTouch() {
  if (!touch.touched()) return;
  delay(50); // Let touch settle
  if (!touch.touched()) return;

  // Accumulate samples
  long rawX = 0, rawY = 0;
  int samples = 10;
  for (int i = 0; i < samples; i++) {
    TS_Point p = touch.getPoint();
    rawX += p.x;
    rawY += p.y;
    delay(5);
  }
  rawX /= samples;
  rawY /= samples;

  if (calStep == 0) {
    calRawMinX = rawX;
    calRawMinY = rawY;
    calStep = 1;
    // Wait for release with debounce
    unsigned long untouchedStart = millis();
    while (millis() - untouchedStart < 500) {
      if (touch.touched()) untouchedStart = millis();
      delay(10);
    }
    drawCalibrationUI();
  } else if (calStep == 1) {
    calRawMaxX = rawX;
    calRawMaxY = rawY;
    calStep = 2;
    saveCalibration();
    // Wait for release with debounce
    unsigned long untouchedStart = millis();
    while (millis() - untouchedStart < 500) {
      if (touch.touched()) untouchedStart = millis();
      delay(10);
    }
    drawCalibrationUI();
  }
}

// ==========================================
//              DRAWING APP
// ==========================================
void initCanvasBuffer() {
  canvasBuf = (uint16_t*)ps_malloc(CANVAS_W * CANVAS_H * sizeof(uint16_t));
  if (!canvasBuf) canvasBuf = (uint16_t*)malloc(CANVAS_W * CANVAS_H * sizeof(uint16_t));
  canvasSaveOk = (canvasBuf != NULL);
  if (canvasSaveOk) {
    for (int i = 0; i < CANVAS_W * CANVAS_H; i++) canvasBuf[i] = CANVAS_BG;
  }
}

void canvasDrawDot(int cx, int cy, int r, uint16_t color) {
  // Clip to canvas area
  int scrY = cy + CANVAS_YOFF;
  if (cy < 0 || cy >= CANVAS_H || cx < 0 || cx >= CANVAS_W) return;
  tft.fillCircle(cx, scrY, r, color);
  // Update buffer
  if (canvasSaveOk) {
    int x0 = max(0, cx - r), x1 = min(CANVAS_W - 1, cx + r);
    int y0 = max(0, cy - r), y1 = min(CANVAS_H - 1, cy + r);
    int r2 = r * r;
    for (int py = y0; py <= y1; py++) {
      for (int px = x0; px <= x1; px++) {
        int dx = px - cx, dy = py - cy;
        if (dx * dx + dy * dy <= r2) {
          canvasBuf[py * CANVAS_W + px] = color;
        }
      }
    }
  }
}

void canvasDrawLine(int x0, int y0, int x1, int y1, int r, uint16_t color) {
  int dx = abs(x1 - x0), dy = abs(y1 - y0);
  int sx = x0 < x1 ? 1 : -1;
  int sy = y0 < y1 ? 1 : -1;
  int err = dx - dy;
  int step = max(1, r / 2); // Skip some dots for large brushes
  int count = 0;
  while (true) {
    if (count % step == 0) canvasDrawDot(x0, y0, r, color);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 > -dy) { err -= dy; x0 += sx; }
    if (e2 < dx) { err += dx; y0 += sy; }
    count++;
  }
  canvasDrawDot(x1, y1, r, color); // Ensure endpoint
}

void canvasClear() {
  tft.fillRect(CANVAS_XOFF, CANVAS_YOFF, CANVAS_W, CANVAS_H, CANVAS_BG);
  if (canvasSaveOk) {
    for (int i = 0; i < CANVAS_W * CANVAS_H; i++) canvasBuf[i] = CANVAS_BG;
  }
}

void saveDrawing(int slot) {
  if (!canvasSaveOk) return;
  char path[20];
  snprintf(path, sizeof(path), "/draw_%d.raw", slot);
  File f = LittleFS.open(path, "w");
  if (!f) return;

  // Show saving indicator
  tft.fillRoundRect(110, 90, 100, 30, 6, UI_PANEL);
  drawCenteredText("Saving...", 110, 90, 100, 30, &FreeSans9pt7b, AESTHETIC_GOLD);

  // Write in chunks for speed
  const int chunkRows = 4;
  for (int y = 0; y < CANVAS_H; y += chunkRows) {
    int rows = min(chunkRows, CANVAS_H - y);
    f.write((uint8_t*)&canvasBuf[y * CANVAS_W], rows * CANVAS_W * 2);
  }
  f.close();

  // Brief confirmation
  tft.fillRoundRect(110, 90, 100, 30, 6, 0x032C);
  drawCenteredText("Saved!", 110, 90, 100, 30, &FreeSans9pt7b, ILI9341_WHITE);
  delay(500);
  drawMenuVisible = false;
  drawDrawingUI();
}

void loadDrawing(int slot) {
  if (!canvasSaveOk) return;
  char path[20];
  snprintf(path, sizeof(path), "/draw_%d.raw", slot);
  File f = LittleFS.open(path, "r");
  if (!f) return;

  tft.fillRoundRect(110, 90, 100, 30, 6, UI_PANEL);
  drawCenteredText("Loading...", 110, 90, 100, 30, &FreeSans9pt7b, ILI9341_CYAN);

  const int chunkRows = 4;
  for (int y = 0; y < CANVAS_H; y += chunkRows) {
    int rows = min(chunkRows, CANVAS_H - y);
    f.read((uint8_t*)&canvasBuf[y * CANVAS_W], rows * CANVAS_W * 2);
    // Blit to TFT
    tft.drawRGBBitmap(CANVAS_XOFF, CANVAS_YOFF + y, &canvasBuf[y * CANVAS_W], CANVAS_W, rows);
  }
  f.close();
  drawMenuVisible = false;
  drawDrawingToolbar();
}

bool drawingSlotExists(int slot) {
  char path[20];
  snprintf(path, sizeof(path), "/draw_%d.raw", slot);
  return LittleFS.exists(path);
}

void drawDrawingUI() {
  tft.fillScreen(ILI9341_BLACK);
  drawTopBar("DRAW", ILI9341_MAGENTA, true);

  // Canvas area - white
  tft.fillRect(CANVAS_XOFF, CANVAS_YOFF, CANVAS_W, CANVAS_H, CANVAS_BG);

  // If buffer has content, blit it
  if (canvasSaveOk) {
    bool hasContent = false;
    for (int i = 0; i < CANVAS_W * CANVAS_H; i++) {
      if (canvasBuf[i] != CANVAS_BG) { hasContent = true; break; }
    }
    if (hasContent) {
      for (int y = 0; y < CANVAS_H; y += 4) {
        int rows = min(4, CANVAS_H - y);
        tft.drawRGBBitmap(CANVAS_XOFF, CANVAS_YOFF + y, &canvasBuf[y * CANVAS_W], CANVAS_W, rows);
      }
    }
  }

  drawDrawingToolbar();
  drawLastTX = -1;
  drawLastTY = -1;
}

void drawDrawingToolbar() {
  tft.fillRect(0, DRAW_TB_Y, 320, DRAW_TB_H, 0x0861);
  tft.drawFastHLine(0, DRAW_TB_Y, 320, 0x2104);

  // Back button
  tft.fillRoundRect(2, DRAW_TB_Y + 4, 26, 32, 4, UI_PANEL);
  drawCenteredText("<", 2, DRAW_TB_Y + 4, 26, 32, &FreeSans9pt7b, ILI9341_WHITE);

  // Color swatches
  for (int i = 0; i < DRAW_NCOLORS; i++) {
    int cx = 40 + i * 24;
    int cy = DRAW_TB_Y + 20;
    tft.fillCircle(cx, cy, 9, DRAW_COLORS[i]);
    if (i == drawColIdx && !drawEraser) {
      tft.drawCircle(cx, cy, 11, ILI9341_WHITE);
      tft.drawCircle(cx, cy, 12, ILI9341_WHITE);
    }
    // Border for white color visibility
    if (DRAW_COLORS[i] == ILI9341_WHITE || DRAW_COLORS[i] == CANVAS_BG) {
      tft.drawCircle(cx, cy, 9, ILI9341_DARKGREY);
    }
  }

  // Brush size indicator
  int bx = 236;
  int by = DRAW_TB_Y + 20;
  int br = DRAW_BRUSH_R[drawBrushIdx];
  tft.fillRoundRect(bx - 14, DRAW_TB_Y + 4, 28, 32, 4, 0x2104);
  tft.fillCircle(bx, by, min(br, 12), AESTHETIC_GOLD);

  // Eraser
  int ex = 266;
  tft.fillRoundRect(ex - 14, DRAW_TB_Y + 4, 28, 32, 4, drawEraser ? ILI9341_CYAN : 0x2104);
  drawCenteredText("E", ex - 14, DRAW_TB_Y + 4, 28, 32, &FreeSans9pt7b, drawEraser ? ILI9341_BLACK : ILI9341_WHITE);

  // Menu button
  int mx = 296;
  tft.fillRoundRect(mx - 12, DRAW_TB_Y + 4, 28, 32, 4, 0x2925);
  // Three dots menu icon
  for (int d = 0; d < 3; d++) {
    tft.fillCircle(mx + 2, DRAW_TB_Y + 10 + d * 8, 2, ILI9341_WHITE);
  }
}

void drawDrawingMenuOverlay() {
  drawMenuVisible = true;
  // Semi-transparent overlay effect
  tft.fillRoundRect(50, 40, 220, 170, 8, 0x0861);
  tft.drawRoundRect(50, 40, 220, 170, 8, AESTHETIC_GOLD);

  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(AESTHETIC_GOLD);
  tft.setCursor(110, 60);
  tft.print("DRAWING");

  const char* labels[] = {"SAVE 1", "SAVE 2", "SAVE 3", "LOAD 1", "LOAD 2", "LOAD 3", "CLEAR", "CANCEL"};
  uint16_t colors[] = {0x2925, 0x2925, 0x2925, 0x032C, 0x032C, 0x032C, 0x480C, UI_PANEL};

  // 2 columns, 4 rows
  for (int i = 0; i < 8; i++) {
    int col = i % 2;
    int row = i / 2;
    int bx = 56 + col * 112;
    int by = 68 + row * 34;
    tft.fillRoundRect(bx, by, 104, 28, 6, colors[i]);

    // Show slot status for load buttons
    uint16_t textCol = ILI9341_WHITE;
    if (i >= 3 && i <= 5) {
      if (!drawingSlotExists(i - 3)) textCol = ILI9341_DARKGREY;
    }
    drawCenteredText(labels[i], bx, by, 104, 28, &FreeSans9pt7b, textCol);
  }
}

void handleDrawingMenuTouch(int x, int y) {
  for (int i = 0; i < 8; i++) {
    int col = i % 2;
    int row = i / 2;
    int bx = 56 + col * 112;
    int by = 68 + row * 34;
    if (x >= bx && x < bx + 104 && y >= by && y < by + 28) {
      if (i < 3) {
        saveDrawing(i);
        return;
      } else if (i < 6) {
        int slot = i - 3;
        if (drawingSlotExists(slot)) loadDrawing(slot);
        else { drawMenuVisible = false; drawDrawingUI(); }
        return;
      } else if (i == 6) {
        canvasClear();
        drawMenuVisible = false;
        drawDrawingToolbar();
        return;
      } else {
        drawMenuVisible = false;
        drawDrawingUI();
        return;
      }
    }
  }
  // Tap outside = cancel
  if (x < 50 || x > 270 || y < 40 || y > 210) {
    drawMenuVisible = false;
    drawDrawingUI();
  }
}

void handleDrawingTouch(int x, int y) {
  // Menu overlay
  if (drawMenuVisible) {
    handleDrawingMenuTouch(x, y);
    return;
  }

  // Top bar back
  if (y < 36 && x < 50) {
    drawLastTX = -1;
    drawLastTY = -1;
    currentApp = DASHBOARD;
    redrawEntireUI();
    return;
  }

  // Toolbar
  if (y >= DRAW_TB_Y) {
    // Back
    if (x < 30) {
      drawLastTX = -1;
      drawLastTY = -1;
      currentApp = DASHBOARD;
      redrawEntireUI();
      return;
    }
    // Colors
    for (int i = 0; i < DRAW_NCOLORS; i++) {
      int cx = 40 + i * 24;
      if (abs(x - cx) < 13 && abs(y - (DRAW_TB_Y + 20)) < 16) {
        drawColIdx = i;
        drawEraser = false;
        drawDrawingToolbar();
        return;
      }
    }
    // Brush size
    if (x >= 222 && x < 250) {
      drawBrushIdx = (drawBrushIdx + 1) % DRAW_NBRUSH;
      drawDrawingToolbar();
      return;
    }
    // Eraser
    if (x >= 252 && x < 280) {
      drawEraser = !drawEraser;
      drawDrawingToolbar();
      return;
    }
    // Menu
    if (x >= 282) {
      drawDrawingMenuOverlay();
      return;
    }
    return;
  }

  // Canvas area - draw
  if (y >= CANVAS_YOFF && y < CANVAS_YOFF + CANVAS_H) {
    int canvasY = y - CANVAS_YOFF;
    int r = DRAW_BRUSH_R[drawBrushIdx];
    uint16_t color = drawEraser ? CANVAS_BG : DRAW_COLORS[drawColIdx];

    if (drawLastTX >= 0 && drawLastTY >= 0) {
      canvasDrawLine(drawLastTX, drawLastTY, x, canvasY, r, color);
    } else {
      canvasDrawDot(x, canvasY, r, color);
    }
    drawLastTX = x;
    drawLastTY = canvasY;
  }
}

void handleDrawingDrag(int x, int y) {
  if (drawMenuVisible) return;
  if (y < CANVAS_YOFF || y >= CANVAS_YOFF + CANVAS_H) return;

  int canvasY = y - CANVAS_YOFF;
  int r = DRAW_BRUSH_R[drawBrushIdx];
  uint16_t color = drawEraser ? CANVAS_BG : DRAW_COLORS[drawColIdx];

  if (drawLastTX >= 0 && drawLastTY >= 0) {
    canvasDrawLine(drawLastTX, drawLastTY, x, canvasY, r, color);
  } else {
    canvasDrawDot(x, canvasY, r, color);
  }
  drawLastTX = x;
  drawLastTY = canvasY;
}

// ==========================================
//              SETUP
// ==========================================
void setup() {
  setCpuFrequencyMhz(240);
  dataMutex = xSemaphoreCreateMutex();

  spotifyAccessToken.reserve(256);
  currentSong.reserve(96);
  currentArtist.reserve(96);
  currentAlbumUrl.reserve(192);
  currentWeather.reserve(24);
  cachedAlbumUrl.reserve(192);

  Serial.begin(115200);
  initDisplaySafely();

  // Init LittleFS
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS init failed");
  }

  // Init canvas buffer for drawing
  initCanvasBuffer();

  // Load touch calibration
  loadCalibration();

  // Load saved WiFi networks
  loadSavedNetworks();

  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(ILI9341_CYAN);
  tft.setCursor(20, 106);
  tft.print("BUBBLE");
  tft.setTextColor(ILI9341_DARKGREY);
  tft.setCursor(20, 130);
  tft.print("Starting up...");
  delay(400);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  if (savedNetCount > 0) {
    tft.fillScreen(UI_BG);
    tft.setFont(&FreeSans9pt7b);
    tft.setTextColor(ILI9341_CYAN);
    tft.setCursor(20, 80);
    tft.print("Connecting to WiFi...");

    if (tryConnectSaved()) {
      onWifiConnected();
      return;
    }
  }

  // No saved networks or connection failed — show WiFi scan
  wifiFirstBoot = true;
  currentApp = WIFI_SCAN;
  startWifiScan();
  drawWifiScanUI();
}

// ==========================================
//              MAIN LOOP
// ==========================================
void loop() {
  unsigned long currentMillis = millis();

  // Touch calibration mode
  if (currentApp == TOUCH_CALIB) {
    handleCalibTouch();
    vTaskDelay(pdMS_TO_TICKS(20));
    return;
  }

  // WiFi screens don't need the standard loop logic
  if (currentApp == WIFI_SCAN || currentApp == WIFI_PASSWORD || currentApp == WIFI_SAVED_ACTION) {
    // Just handle touch
    static bool wifiTouchWasDown = false;
    bool touching = touch.touched();
    if (touching && !wifiTouchWasDown) {
      int x, y;
      if (readTouchPoint(x, y)) {
        if (currentApp == WIFI_SCAN) handleWifiScanTouch(x, y);
        else if (currentApp == WIFI_SAVED_ACTION) handleWifiSavedActionTouch(x, y);
        else handleWifiKbTouch(x, y);
      }
    }
    wifiTouchWasDown = touching;
    vTaskDelay(pdMS_TO_TICKS(10));
    return;
  }

  updatePowerState(currentMillis);
  applyNetworkUpdates();

  if (elapsedSince(currentMillis, lastNTPSync, NTP_SYNC_INTERVAL_MS)) {
    syncTimeNow(false);
  }

  static unsigned long lastTelemetryStaleCheck = 0;
  if (elapsedSince(currentMillis, lastTelemetryStaleCheck, 1000)) {
    refreshTelemetryStaleUi(currentMillis);
    lastTelemetryStaleCheck = currentMillis;
  }

  static unsigned long lastClockCheck = 0;
  if (elapsedSince(currentMillis, lastClockCheck, CLOCK_CHECK_INTERVAL_MS)) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 25)) {
      updateTopClock(false);
      if (currentApp == DASHBOARD && !isMenuOpen && timeinfo.tm_min != lastDashboardMinute) {
        updateDashboardClock(&timeinfo, false);
        lastDashboardMinute = timeinfo.tm_min;
      }
    }
    lastClockCheck = currentMillis;
  }

  if (currentApp == SPOTIFY && needsScrolling && screenAwake && !isMenuOpen) {
    if (elapsedSince(currentMillis, lastMarqueeUpdate, MARQUEE_INTERVAL_MS)) {
      marqueeX -= 2;
      int marqueeCycleWidth = songPixelWidth + MARQUEE_GAP_PX;
      if (marqueeX <= -marqueeCycleWidth) marqueeX = MARQUEE_VIEW_WIDTH + MARQUEE_GAP_PX;
      drawMarquee();
      lastMarqueeUpdate = currentMillis;
    }
  }

  if (currentApp == SPOTIFY && screenAwake && !isMenuOpen) {
    if (elapsedSince(currentMillis, lastBeatUpdate, BEAT_VISUALIZER_INTERVAL_MS)) {
      drawBeatVisualizer(false);
      lastBeatUpdate = currentMillis;
    }
  }

  if (timerRunning && elapsedSince(currentMillis, lastTimerTick, 1000)) {
    unsigned long elapsedSeconds = (currentMillis - lastTimerTick) / 1000;
    lastTimerTick += elapsedSeconds * 1000;
    if (elapsedSeconds >= (unsigned long)timerSeconds) {
      timerSeconds = 0;
      timerRunning = false;
    } else {
      timerSeconds -= elapsedSeconds;
    }
    if (currentApp == TIMER) updateTimerDisplay();
  }

  if (swRunning && currentApp == STOPWATCH) {
    swElapsedMillis = currentMillis - swStartMillis;
    static unsigned long lastSwDraw = 0;
    if (elapsedSince(currentMillis, lastSwDraw, STOPWATCH_DRAW_INTERVAL_MS)) {
      updateStopwatchDisplay();
      lastSwDraw = currentMillis;
    }
  }

  // Drawing: handle continuous touch drag
  if (currentApp == DRAWING && screenAwake && !drawMenuVisible) {
    bool touching = touch.touched();
    if (touching) {
      int x, y;
      if (readTouchPoint(x, y)) {
        lastInteractionMillis = currentMillis;
        if (y >= CANVAS_YOFF && y < CANVAS_YOFF + CANVAS_H) {
          handleDrawingDrag(x, y);
        }
      }
    } else {
      drawLastTX = -1;
      drawLastTY = -1;
    }
  }

  // Touch handling (tap detection for non-drawing modes)
  static bool touchWasDown = false;
  bool touching = touch.touched();
  if (currentApp != DRAWING) {
    if (touching && !touchWasDown && elapsedSince(currentMillis, lastTouchTime, TOUCH_DEBOUNCE_MS)) {
      int x, y;
      if (readTouchPoint(x, y)) {
        lastTouchTime = currentMillis;
        lastInteractionMillis = currentMillis;
        if (!screenAwake || screenDimmed) {
          wakeScreen();
        } else {
          handleTouch(x, y);
        }
      }
    }
  } else {
    // Drawing mode: handle toolbar/menu taps on press
    if (touching && !touchWasDown && elapsedSince(currentMillis, lastTouchTime, TOUCH_DEBOUNCE_MS)) {
      int x, y;
      if (readTouchPoint(x, y)) {
        lastTouchTime = currentMillis;
        lastInteractionMillis = currentMillis;
        if (!screenAwake || screenDimmed) {
          wakeScreen();
        } else if (y >= DRAW_TB_Y || y < CANVAS_YOFF || drawMenuVisible) {
          handleDrawingTouch(x, y);
        } else {
          // First dot
          handleDrawingDrag(x, y);
        }
      }
    }
  }
  touchWasDown = touching;
  if (!touching && currentApp == DRAWING) {
    drawLastTX = -1;
    drawLastTY = -1;
  }

  vTaskDelay(pdMS_TO_TICKS(2));
}

// ==========================================
//              BACKGROUND NETWORK
// ==========================================
void networkTask(void* pvParameters) {
  WiFiClientSecure secureClient;
  secureClient.setInsecure();

  for (;;) {
    unsigned long now = millis();

    if (WiFi.status() != WL_CONNECTED) {
      if (currentApp != WIFI_SCAN && currentApp != WIFI_PASSWORD && currentApp != WIFI_SAVED_ACTION) {
        if (tryConnectSaved(false)) {
          telemetryUdp.begin(TELEMETRY_PORT);
        } else {
          vTaskDelay(pdMS_TO_TICKS(5000));
        }
      } else {
        vTaskDelay(pdMS_TO_TICKS(1000));
      }
      continue;
    }
    pollTelemetryUdp();

    if (elapsedSince(now, lastSpotifyTokenRefresh, SPOTIFY_TOKEN_INTERVAL_MS)) {
      refreshSpotifyTokenWith(secureClient);
      lastSpotifyTokenRefresh = millis();
    }

    if (currentApp == SPOTIFY && elapsedSince(now, lastSpotifyDataFetch, SPOTIFY_FETCH_INTERVAL_MS)) {
      queueFetch(FETCH_SPOTIFY);
      lastSpotifyDataFetch = now;
    }

    if (elapsedSince(now, lastWeatherUpdate, WEATHER_FETCH_INTERVAL_MS)) {
      queueFetch(FETCH_WEATHER);
      lastWeatherUpdate = now;
    }

    if (todoistToken != "" && (taskFetchPending || elapsedSince(now, lastTaskFetch, TASK_FETCH_INTERVAL_MS))) {
      taskFetchPending = false;
      fetchTodoistTasksWith(secureClient);
      lastTaskFetch = millis();
    }

    if (spotifyControlPending) {
      char action[12];
      lockData();
      strncpy(action, spotifyControlAction, sizeof(action) - 1);
      action[sizeof(action) - 1] = '\0';
      spotifyControlPending = false;
      unlockData();
      sendSpotifyControlWith(secureClient, action);
      queueFetch(FETCH_SPOTIFY, SPOTIFY_CONTROL_SETTLE_MS);
    }

    if (taskCompletePending) {
      String taskId;
      lockData();
      taskId = taskCompleteId;
      taskCompleteId = "";
      taskCompletePending = false;
      unlockData();
      if (taskId != "") {
        completeTodoistTaskWith(secureClient, taskId);
        taskFetchPending = true;
      }
    }

    if (spotifyFetchPending && timeReached(now, spotifyFetchReadyAt)) {
      spotifyFetchPending = false;
      spotifyFetchReadyAt = 0;
      fetchSpotifyDataWith(secureClient);
      lastSpotifyDataFetch = millis();
    }

    if (weatherFetchPending) {
      weatherFetchPending = false;
      fetchWeatherData();
      lastWeatherUpdate = millis();
    }

    vTaskDelay(pdMS_TO_TICKS(30));
  }
}

// ==========================================
//              API FUNCTIONS
// ==========================================

// Thread-safe wrappers — each uses the task-local secureClient
void refreshSpotifyTokenWith(WiFiClientSecure& secureClient) {
  if (spotifyRefreshToken == "" || WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin(secureClient, "https://accounts.spotify.com/api/token");
  http.setTimeout(API_HTTP_TIMEOUT_MS);
  http.useHTTP10(true);

  String authHeader;
  authHeader.reserve(6 + spotifyBase64Auth.length());
  authHeader = "Basic ";
  authHeader += spotifyBase64Auth;
  http.addHeader("Authorization", authHeader);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String payload;
  payload.reserve(39 + spotifyRefreshToken.length());
  payload = "grant_type=refresh_token&refresh_token=";
  payload += spotifyRefreshToken;

  if (http.POST(payload) == 200) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, http.getStream());
    if (!error) {
      lockData();
      spotifyAccessToken = doc["access_token"].as<String>();
      unlockData();
    }
  }
  http.end();
}

// Keep old name for setup() calls (uses a temporary client)
void refreshSpotifyToken() {
  WiFiClientSecure sc;
  sc.setInsecure();
  refreshSpotifyTokenWith(sc);
}

void fetchSpotifyDataWith(WiFiClientSecure& secureClient) {
  String token;
  lockData();
  token = spotifyAccessToken;
  unlockData();

  if (token == "" || WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin(secureClient, "https://api.spotify.com/v1/me/player/currently-playing");
  http.setTimeout(API_HTTP_TIMEOUT_MS);
  http.useHTTP10(true);

  String authHeader;
  authHeader.reserve(7 + token.length());
  authHeader = "Bearer ";
  authHeader += token;
  http.addHeader("Authorization", authHeader);

  int httpResponseCode = http.GET();
  String albumUrlToFetch = "";

  if (httpResponseCode == 200) {
    static JsonDocument filter;
    static bool filterReady = false;
    if (!filterReady) {
      filter["item"]["name"] = true;
      filter["item"]["artists"][0]["name"] = true;
      filter["item"]["album"]["images"][0]["url"] = true;
      filter["is_playing"] = true;
      filterReady = true;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    if (!error) {
      String newSong = doc["item"]["name"].as<String>();
      String newArtist = doc["item"]["artists"][0]["name"].as<String>();
      bool newPlayState = doc["is_playing"];
      String newAlbumUrl = "";

      JsonArray images = doc["item"]["album"]["images"];
      if (images.size() > 0) newAlbumUrl = images[images.size() - 1]["url"].as<String>();
      if (newAlbumUrl == "null") newAlbumUrl = "";

      lockData();
      bool songChanged = (newSong != currentSong);
      bool artistChanged = (newArtist != currentArtist);
      bool playStateChanged = (newPlayState != isPlaying);
      bool albumChanged = (newAlbumUrl != currentAlbumUrl);

      if (songChanged || artistChanged || playStateChanged || albumChanged) {
        currentSong = newSong;
        currentArtist = newArtist;
        isPlaying = newPlayState;
        currentAlbumUrl = newAlbumUrl;
        if (songChanged || artistChanged) spotifyContentDirty = true;
        if (playStateChanged) spotifyControlsDirty = true;
        if (albumChanged) {
          if (newAlbumUrl != "") {
            albumUrlToFetch = newAlbumUrl;
          } else {
            cachedAlbumUrl = "";
            imgBuffSize = 0;
            spotifyAlbumDirty = true;
          }
        }
      }
      unlockData();
    }
  } else if (httpResponseCode == 204) {
    lockData();
    currentSong = "No Music Playing";
    currentArtist = "";
    currentAlbumUrl = "";
    cachedAlbumUrl = "";
    imgBuffSize = 0;
    isPlaying = false;
    spotifyContentDirty = true;
    spotifyControlsDirty = true;
    spotifyAlbumDirty = true;
    unlockData();
  }
  http.end();

  if (albumUrlToFetch != "") fetchAlbumArtToCache(albumUrlToFetch);
}

void fetchSpotifyData() {
  WiFiClientSecure sc;
  sc.setInsecure();
  fetchSpotifyDataWith(sc);
}

void sendSpotifyControlWith(WiFiClientSecure& secureClient, const char* action) {
  String token;
  lockData();
  token = spotifyAccessToken;
  unlockData();

  if (token == "" || WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String endpoint;
  endpoint.reserve(45 + strlen(action));
  endpoint = "https://api.spotify.com/v1/me/player/";
  endpoint += action;
  http.begin(secureClient, endpoint);
  http.setTimeout(API_HTTP_TIMEOUT_MS);

  String authHeader;
  authHeader.reserve(7 + token.length());
  authHeader = "Bearer ";
  authHeader += token;
  http.addHeader("Authorization", authHeader);
  http.addHeader("Content-Length", "0");

  if (strcmp(action, "play") == 0 || strcmp(action, "pause") == 0) http.PUT(String(""));
  else http.POST(String(""));
  http.end();
}

void sendSpotifyControl(const char* action) {
  WiFiClientSecure sc;
  sc.setInsecure();
  sendSpotifyControlWith(sc, action);
}

void fetchAlbumArtToCache(const String& url) {
  if (url == "" || WiFi.status() != WL_CONNECTED) {
    lockData();
    imgBuffSize = 0;
    cachedAlbumUrl = "";
    spotifyAlbumDirty = true;
    unlockData();
    return;
  }

  lockData();
  bool alreadyCached = (url == cachedAlbumUrl && imgBuffSize > 0);
  unlockData();
  if (alreadyCached) return;

  WiFiClientSecure imgClient;
  imgClient.setInsecure();
  HTTPClient http;
  http.begin(imgClient, url);
  http.setTimeout(IMAGE_HTTP_TIMEOUT_MS);

  int httpCode = http.GET();
  if (httpCode == 200) {
    int len = http.getSize();
    if (len > 0 && len <= ALBUM_ART_MAX_SIZE) {
      WiFiClient* stream = http.getStreamPtr();
      size_t size = stream->readBytes(imgDownloadBuff, len);
      lockData();
      memcpy(imgBuff, imgDownloadBuff, size);
      imgBuffSize = size;
      cachedAlbumUrl = url;
      spotifyAlbumDirty = true;
      unlockData();
    }
  }
  http.end();
}

void fetchWeatherData() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  char weatherUrl[220];
  snprintf(weatherUrl, sizeof(weatherUrl),
           "http://api.openweathermap.org/data/2.5/weather?q=%s,%s&appid=%s&units=metric",
           city.c_str(), countryCode.c_str(), weatherApiKey.c_str());

  http.begin(weatherUrl);
  http.setTimeout(API_HTTP_TIMEOUT_MS);
  http.useHTTP10(true);

  if (http.GET() == 200) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, http.getStream());
    if (!error) {
      lockData();
      currentTemp = doc["main"]["temp"];
      currentHumidity = doc["main"]["humidity"];
      currentWeather = doc["weather"][0]["main"].as<String>();
      weatherDirty = true;
      unlockData();
    }
  }
  http.end();
}

void pollTelemetryUdp() {
  int packetSize = telemetryUdp.parsePacket();
  if (packetSize <= 0) return;

  char packet[96];
  int length = telemetryUdp.read(packet, sizeof(packet) - 1);
  if (length <= 0) return;
  packet[length] = '\0';

  float cpu, gpu, ram, wifiMbps;
  int parsed = sscanf(packet, "cpu=%f,gpu=%f,ram=%f,wifi=%f", &cpu, &gpu, &ram, &wifiMbps);
  if (parsed == 4) {
    lockData();
    pcCpuUsage = cpu;
    pcGpuTemp = gpu;
    pcRamUsage = ram;
    pcWifiMbps = wifiMbps;
    lastTelemetryMillis = millis();
    telemetryDirty = true;
    unlockData();
  }
}

void fetchTodoistTasksWith(WiFiClientSecure& secureClient) {
  if (todoistToken == "" || WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin(secureClient, "https://api.todoist.com/rest/v2/tasks?filter=today&limit=3");
  http.setTimeout(API_HTTP_TIMEOUT_MS);
  http.useHTTP10(true);

  String authHeader;
  authHeader.reserve(7 + todoistToken.length());
  authHeader = "Bearer ";
  authHeader += todoistToken;
  http.addHeader("Authorization", authHeader);

  if (http.GET() == 200) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, http.getStream());
    if (!error) {
      lockData();
      taskCount = 0;
      JsonArray tasks = doc.as<JsonArray>();
      for (JsonObject task : tasks) {
        if (taskCount >= 3) break;
        taskItems[taskCount] = task["content"].as<String>();
        taskIds[taskCount] = task["id"].as<String>();
        taskCount++;
      }
      if (taskCount == 0) {
        taskItems[0] = "No tasks today";
        taskIds[0] = "";
        taskCount = 1;
      }
      for (int i = taskCount; i < 3; i++) {
        taskItems[i] = "";
        taskIds[i] = "";
      }
      tasksDirty = true;
      unlockData();
    }
  }
  http.end();
}

void fetchTodoistTasks() {
  WiFiClientSecure sc;
  sc.setInsecure();
  fetchTodoistTasksWith(sc);
}

void completeTodoistTaskWith(WiFiClientSecure& secureClient, const String& taskId) {
  if (todoistToken == "" || taskId == "" || WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = "https://api.todoist.com/rest/v2/tasks/";
  url += taskId;
  url += "/close";
  http.begin(secureClient, url);
  http.setTimeout(API_HTTP_TIMEOUT_MS);
  http.useHTTP10(true);

  String authHeader;
  authHeader.reserve(7 + todoistToken.length());
  authHeader = "Bearer ";
  authHeader += todoistToken;
  http.addHeader("Authorization", authHeader);
  http.addHeader("Content-Length", "0");
  http.POST(String(""));
  http.end();
}

void completeTodoistTask(const String& taskId) {
  WiFiClientSecure sc;
  sc.setInsecure();
  completeTodoistTaskWith(sc, taskId);
}

void applyNetworkUpdates() {
  bool contentDirty;
  bool controlsDirty;
  bool albumDirty;
  bool weatherNeedsDraw;
  bool telemetryNeedsDraw;
  bool tasksNeedDraw;

  lockData();
  contentDirty = spotifyContentDirty;
  controlsDirty = spotifyControlsDirty;
  albumDirty = spotifyAlbumDirty;
  weatherNeedsDraw = weatherDirty;
  telemetryNeedsDraw = telemetryDirty;
  tasksNeedDraw = tasksDirty;
  spotifyContentDirty = false;
  spotifyControlsDirty = false;
  spotifyAlbumDirty = false;
  weatherDirty = false;
  telemetryDirty = false;
  tasksDirty = false;
  if (currentSong.length() > 0) currentSong.toCharArray(marqueeSong, sizeof(marqueeSong));
  unlockData();

  if (contentDirty) updateSongMetrics();

  if (currentApp == SPOTIFY && screenAwake && !isMenuOpen) {
    if (contentDirty || controlsDirty) updateSpotifyText(false);
    if (controlsDirty) drawSpotifyControls();
    if (albumDirty) {
      drawAlbumArtPlaceholder();
      drawCachedAlbumArt();
    }
  }

  if (currentApp == DASHBOARD && !isMenuOpen && weatherNeedsDraw) updateWeatherDisplay();
  if (currentApp == DASHBOARD && !isMenuOpen && telemetryNeedsDraw) drawPcStatsPreview(false);
  else if (currentApp == PC_STATS && telemetryNeedsDraw) drawPcStatsValues(false);
  if (currentApp == DASHBOARD && !isMenuOpen && tasksNeedDraw) drawTasksPreview(false);
  else if (currentApp == TASKS && tasksNeedDraw) drawTasksUI();
}

void refreshTelemetryStaleUi(unsigned long now) {
  static bool initialized = false;
  static bool lastFresh = false;
  static unsigned long lastMockUpdate = 0;
  unsigned long lastSeen;

  lockData();
  lastSeen = lastTelemetryMillis;
  unlockData();

  bool fresh = (lastSeen != 0 && (unsigned long)(now - lastSeen) <= TELEMETRY_STALE_MS);
  
  if (!fresh && (unsigned long)(now - lastMockUpdate) > 2000) {
    lockData();
    pcCpuUsage = random(5, 60);
    pcRamUsage = random(30, 85);
    pcGpuTemp = random(45, 80);
    pcWifiMbps = random(10, 100);
    telemetryDirty = true;
    unlockData();
    lastMockUpdate = now;
  }

  if (initialized && fresh != lastFresh && screenAwake && !isMenuOpen) {
    if (currentApp == DASHBOARD) drawPcStatsPreview(false);
    else if (currentApp == PC_STATS) drawPcStatsValues(false);
  }
  lastFresh = fresh;
  initialized = true;
}

// ==========================================
//              TIME / TOP BAR
// ==========================================
bool syncTimeNow(bool waitForSync) {
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer, ntpServer2, ntpServer3);
  lastNTPSync = millis();

  if (!waitForSync) return true;

  struct tm timeinfo;
  for (int i = 0; i < 24; i++) {
    if (getLocalTime(&timeinfo, 250)) return true;
    delay(250);
  }
  return false;
}

void drawTopBar(const char* title, uint16_t accent, bool showBack) {
  tft.fillRect(0, 0, 320, 36, 0x0861);
  tft.fillRect(0, 34, 320, 2, accent);

  tft.setFont(&FreeSans9pt7b);
  if (showBack) {
    tft.setTextColor(accent);
    tft.setCursor(10, 24);
    tft.print("<");
  } else {
    tft.fillRect(12, 10, 24, 3, ILI9341_WHITE);
    tft.fillRect(12, 17, 24, 3, ILI9341_WHITE);
    tft.fillRect(12, 24, 24, 3, ILI9341_WHITE);
  }

  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(showBack ? 40 : 54, 24);
  tft.print(title);

  // WiFi indicator
  bool wifiConnected = (WiFi.status() == WL_CONNECTED);
  uint16_t wifiColor = wifiConnected ? SPOTIFY_GREEN : ILI9341_RED;
  tft.fillCircle(210, 18, 4, wifiColor);

  lastTopClock[0] = '\0';
  updateTopClock(true);
}

void updateTopClock(bool force) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 25)) return;

  char clockStr[10];
  strftime(clockStr, sizeof(clockStr), "%I:%M %p", &timeinfo);
  if (!force && strcmp(clockStr, lastTopClock) == 0) return;

  strcpy(lastTopClock, clockStr);
  if (isMenuOpen) return;

  tft.fillRect(220, 8, 98, 20, 0x0861);
  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(222, 24);
  tft.print(clockStr);
}

// ==========================================
//              DISPLAY / POWER / TOUCH
// ==========================================
void initDisplaySafely() {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  pinMode(TFT_BL, OUTPUT);
  analogWrite(TFT_BL, 0);
#else
  ledcSetup(0, 5000, 8);
  ledcAttachPin(TFT_BL, 0);
  ledcWrite(0, 0);
#endif

  pinMode(TFT_RST, OUTPUT);
  digitalWrite(TFT_RST, LOW);
  delay(80);
  digitalWrite(TFT_RST, HIGH);
  delay(180);

  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
  tft.begin();
  tft.setRotation(1);
  tft.fillScreen(ILI9341_BLACK);

  marqueeCanvas = new GFXcanvas16(MARQUEE_VIEW_WIDTH, MARQUEE_VIEW_HEIGHT);
  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(false);
  TJpgDec.setCallback(tft_output);

  touch.begin();
  touch.setRotation(1);

  fadeBacklightTo(currentBrightness);
}

void setBacklight(int value) {
  value = constrain(value, 0, MAX_BRIGHTNESS);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  analogWrite(TFT_BL, value);
#else
  ledcSetup(0, 5000, 8);
  ledcAttachPin(TFT_BL, 0);
  ledcWrite(0, value);
#endif
}

void fadeBacklightTo(int target) {
  target = constrain(target, 0, MAX_BRIGHTNESS);
  int start = screenAwake ? currentBrightness : 0;
  if (target < start) {
    for (int b = start; b >= target; b -= 8) { setBacklight(b); delay(4); }
  } else {
    for (int b = start; b <= target; b += 8) { setBacklight(b); delay(4); }
  }
  setBacklight(target);
}

void wakeScreen() {
  screenAwake = true;
  screenDimmed = false;
  fadeBacklightTo(currentBrightness);
  redrawCurrentApp();
}

void sleepScreen() {
  if (!screenAwake) return;
  screenAwake = false;
  screenDimmed = false;
  fadeBacklightTo(0);
}

void updatePowerState(unsigned long now) {
  if (!screenAwake) return;

  if (shouldKeepScreenLive()) {
    lastInteractionMillis = now;
    if (screenDimmed) {
      screenDimmed = false;
      setBacklight(currentBrightness);
    }
    return;
  }

  unsigned long idleFor = now - lastInteractionMillis;

  if (idleFor > SCREEN_SLEEP_TIMEOUT_MS) {
    sleepScreen();
  } else if (!screenDimmed && idleFor > SCREEN_DIM_TIMEOUT_MS) {
    screenDimmed = true;
    setBacklight(min(DIM_BRIGHTNESS, currentBrightness));
  }
}

bool shouldKeepScreenLive() {
  if (isMenuOpen) return false;
  if (currentApp == SPOTIFY || currentApp == PC_STATS || currentApp == DRAWING) return true;
  if (currentApp == TIMER && timerRunning) return true;
  if (currentApp == STOPWATCH && swRunning) return true;
  return false;
}

bool readTouchPoint(int& x, int& y) {
  if (!touch.touched()) return false;

  long rawX = 0;
  long rawY = 0;
  const int samples = 5;
  for (int i = 0; i < samples; i++) {
    TS_Point p = touch.getPoint();
    rawX += p.x;
    rawY += p.y;
    delayMicroseconds(500);
  }

  rawX /= samples;
  rawY /= samples;

  if (calRawMaxX == calRawMinX) calRawMaxX++;
  if (calRawMaxY == calRawMinY) calRawMaxY++;
  x = constrain(map(rawX, calRawMinX, calRawMaxX, 20, 299), 0, 319);
  y = constrain(map(rawY, calRawMinY, calRawMaxY, 20, 219), 0, 239);
  return true;
}

// ==========================================
//              UI FLOW
// ==========================================
void redrawCurrentApp() {
  if (currentApp == DASHBOARD) redrawEntireUI();
  else if (currentApp == SPOTIFY) drawSpotifyUI();
  else if (currentApp == PC_STATS) drawPcStatsUI();
  else if (currentApp == TASKS) drawTasksUI();
  else if (currentApp == DRAWING) drawDrawingUI();
  else if (currentApp == WIFI_SCAN) drawWifiScanUI();
  else if (currentApp == WIFI_PASSWORD) drawWifiKeyboardUI();
  else if (currentApp == TOUCH_CALIB) drawCalibrationUI();
  else drawFocusAppUI();
}

void redrawEntireUI() {
  isMenuOpen = false;
  drawDashboardUI();
}

// ==========================================
//              DASHBOARD
// ==========================================
void drawDashboardUI() {
  tft.fillScreen(UI_BG);
  drawTopBar("BUBBLE", AESTHETIC_GOLD, false);
  drawStaticUI();

  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 25)) {
    updateDashboardClock(&timeinfo, true);
    updateDateDisplay(&timeinfo);
  }
  drawPcStatsPreview(true);
  drawTasksPreview(true);
}

void drawStaticUI() {
  // Clock + weather card
  tft.fillRoundRect(10, 46, 300, 74, 8, UI_PANEL);
  tft.drawRoundRect(10, 46, 300, 74, 8, AESTHETIC_GOLD);

  // PC Stats & Tasks cards
  tft.fillRoundRect(10, 126, 145, 56, 8, 0x032C);
  tft.fillRoundRect(165, 126, 145, 56, 8, 0x480C);

  // Bottom row: TIMER | DRAW | SPOTIFY
  tft.fillRoundRect(10, 188, 96, 36, 8, 0x2925);
  tft.fillRoundRect(112, 188, 96, 36, 8, 0x3186);
  tft.fillRoundRect(214, 188, 96, 36, 8, 0x03A6);

  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(ILI9341_CYAN);
  tft.setCursor(22, 148);
  tft.print("PC");
  tft.setTextColor(ILI9341_MAGENTA);
  tft.setCursor(177, 148);
  tft.print("TASKS");

  drawCenteredText("TIMER", 10, 188, 96, 36, &FreeSans9pt7b, AESTHETIC_GOLD);
  drawCenteredText("DRAW", 112, 188, 96, 36, &FreeSans9pt7b, ILI9341_MAGENTA);
  drawCenteredText("SPOTIFY", 214, 188, 96, 36, &FreeSans9pt7b, SPOTIFY_GREEN);
}

void drawPcStatsPreview(bool force) {
  float cpu, ram, gpu;
  unsigned long lastSeen;

  lockData();
  cpu = pcCpuUsage;
  ram = pcRamUsage;
  gpu = pcGpuTemp;
  lastSeen = lastTelemetryMillis;
  unlockData();

  if (currentApp != DASHBOARD || isMenuOpen) return;

  bool stale = (lastSeen == 0 || (unsigned long)(millis() - lastSeen) > TELEMETRY_STALE_MS);
  char line1[18];
  char line2[18];
  snprintf(line1, sizeof(line1), "C:%d%% R:%d%%", (int)cpu, (int)ram);
  if (gpu >= 0) snprintf(line2, sizeof(line2), "G:%d%% W:%d%%", (int)gpu, (int)pcWifiMbps);
  else snprintf(line2, sizeof(line2), "G:--%% W:%d%%", (int)pcWifiMbps);
  line1[sizeof(line1) - 1] = '\0';
  line2[sizeof(line2) - 1] = '\0';

  static char lastLine1[18] = "";
  static char lastLine2[18] = "";
  if (force || strcmp(line1, lastLine1) != 0 || strcmp(line2, lastLine2) != 0) {
    tft.fillRect(12, 150, 140, 31, 0x032C);
    tft.setFont(&FreeSans9pt7b);
    tft.setTextColor(ILI9341_WHITE);
    tft.setCursor(22, 162);
    tft.print(line1);
    tft.setCursor(22, 178);
    tft.print(line2);
    strncpy(lastLine1, line1, sizeof(lastLine1));
    lastLine1[sizeof(lastLine1) - 1] = '\0';
    strncpy(lastLine2, line2, sizeof(lastLine2));
    lastLine2[sizeof(lastLine2) - 1] = '\0';
  }
}

void drawTasksPreview(bool force) {
  char firstTask[32];
  int count;

  lockData();
  count = taskCount;
  if (count > 0) taskItems[0].toCharArray(firstTask, sizeof(firstTask));
  else strncpy(firstTask, "No tasks", sizeof(firstTask));
  firstTask[sizeof(firstTask) - 1] = '\0';
  unlockData();

  if (currentApp != DASHBOARD || isMenuOpen) return;

  fitTextToWidth(firstTask, 120, &FreeSans9pt7b);
  static char lastTaskLine[32] = "";
  if (!force && strcmp(firstTask, lastTaskLine) == 0) return;

  tft.fillRect(175, 154, 126, 24, 0x480C);
  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(177, 172);
  tft.print(firstTask);
  strncpy(lastTaskLine, firstTask, sizeof(lastTaskLine));
  lastTaskLine[sizeof(lastTaskLine) - 1] = '\0';
}

void updateDashboardClock(struct tm* timeinfo, bool force) {
  static char lastBigClock[10] = "";
  char timeStr[6];
  char ampmStr[3];
  char fullTimeStr[10];
  strftime(timeStr, sizeof(timeStr), "%I:%M", timeinfo);
  strftime(ampmStr, sizeof(ampmStr), "%p", timeinfo);
  snprintf(fullTimeStr, sizeof(fullTimeStr), "%s %s", timeStr, ampmStr);
  if (!force && strcmp(fullTimeStr, lastBigClock) == 0) return;

  strcpy(lastBigClock, fullTimeStr);
  tft.fillRect(22, 58, 154, 36, UI_PANEL);
  tft.setFont(&FreeSans18pt7b);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(24, 88);
  tft.print(timeStr);

  int16_t x1, y1;
  uint16_t tw, th;
  tft.getTextBounds(timeStr, 24, 88, &x1, &y1, &tw, &th);
  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(AESTHETIC_GOLD);
  tft.setCursor(x1 + (int16_t)tw + 8, 88);
  tft.print(ampmStr);
}

void updateDateDisplay(struct tm* timeinfo) {
  char dateStr[18];
  char weatherText[18];
  float temp;
  int humidity;

  lockData();
  temp = currentTemp;
  humidity = currentHumidity;
  currentWeather.toCharArray(weatherText, sizeof(weatherText));
  unlockData();

  strftime(dateStr, sizeof(dateStr), "%a, %b %d", timeinfo);
  tft.fillRect(24, 96, 156, 18, UI_PANEL);
  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(AESTHETIC_GOLD);
  tft.setCursor(24, 112);
  tft.print(dateStr);

  tft.fillRect(194, 58, 100, 52, UI_PANEL);
  char tempLine[12];
  if (strcmp(weatherText, "--") == 0) {
    strncpy(tempLine, "-- C", sizeof(tempLine));
    strncpy(weatherText, "Weather", sizeof(weatherText));
  } else {
    snprintf(tempLine, sizeof(tempLine), "%.1f C", temp);
  }
  drawCenteredText(tempLine, 194, 58, 100, 24, &FreeSans9pt7b, 0x87FF);
  fitTextToWidth(weatherText, 90, &FreeSans9pt7b);
  drawCenteredText(weatherText, 194, 80, 100, 20, &FreeSans9pt7b, ILI9341_WHITE);
  char humidityLine[10];
  if (strcmp(tempLine, "-- C") == 0) strncpy(humidityLine, "-- RH", sizeof(humidityLine));
  else snprintf(humidityLine, sizeof(humidityLine), "%d%% RH", humidity);
  drawCenteredText(humidityLine, 194, 98, 100, 16, &FreeSans9pt7b, ILI9341_DARKGREY);
}

void updateWeatherDisplay() {
  if (currentApp != DASHBOARD || isMenuOpen) return;
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 25)) updateDateDisplay(&timeinfo);
}

// ==========================================
//              SETTINGS MENU
// ==========================================
void openSettingsMenu() {
  isMenuOpen = true;
  tft.fillRect(0, 0, 200, 240, MENU_BG);

  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(20, 25);
  tft.print("SETTINGS");
  tft.drawLine(16, 36, 184, 36, UI_LINE);

  // UP button
  tft.fillRoundRect(5, 46, 36, 83, 8, 0x2104);
  tft.fillTriangle(23, 72, 11, 96, 35, 96, ILI9341_WHITE);

  // DOWN button
  tft.fillRoundRect(5, 135, 36, 83, 8, 0x2104);
  tft.fillTriangle(11, 161, 35, 161, 23, 185, ILI9341_WHITE);

  const char* itemNames[5] = {"WiFi", "TIMER", "SPOTIFY", "CALIBRATE", "DRAW"};
  uint16_t itemColors[5] = {ILI9341_CYAN, AESTHETIC_GOLD, SPOTIFY_GREEN, 0x2925, 0x480C};
  uint16_t itemTextColors[5] = {ILI9341_BLACK, ILI9341_BLACK, ILI9341_BLACK, ILI9341_WHITE, ILI9341_WHITE};

  for (int i = 0; i < 3 && (i + settingsScrollOff) < 5; i++) {
    int idx = i + settingsScrollOff;
    int y = 46 + i * 62;
    tft.fillRoundRect(46, y, 144, 52, 8, itemColors[idx]);
    drawCenteredText(itemNames[idx], 46, y, 144, 52, &FreeSans9pt7b, itemTextColors[idx]);
  }
}

void closeSettingsMenu() {
  isMenuOpen = false;
  redrawEntireUI();
}

void controlSpotify(const char* action) {
  if (strcmp(action, "play") == 0 || strcmp(action, "pause") == 0) {
    lockData();
    isPlaying = (strcmp(action, "play") == 0);
    spotifyControlsDirty = true;
    unlockData();
    if (currentApp == SPOTIFY) {
      updateSpotifyText(false);
      drawSpotifyControls();
    }
  }
  queueSpotifyControl(action);
}

// ==========================================
//              TOUCH HANDLER
// ==========================================
void handleTouch(int x, int y) {
  if (currentApp == DASHBOARD) {
    if (!isMenuOpen) {
      // Hamburger menu
      if (x < 86 && y < 54) openSettingsMenu();
      // PC Stats card
      else if (hitRect(x, y, 10, 128, 145, 52)) { currentApp = PC_STATS; drawPcStatsUI(); }
      // Tasks card
      else if (hitRect(x, y, 165, 128, 145, 52)) { currentApp = TASKS; drawTasksUI(); }
      // Timer button
      else if (hitRect(x, y, 10, 188, 96, 36)) { currentApp = TIMER; drawFocusAppUI(); }
      // Draw button
      else if (hitRect(x, y, 112, 188, 96, 36)) { currentApp = DRAWING; drawDrawingUI(); }
      // Spotify button
      else if (hitRect(x, y, 214, 188, 96, 36)) { currentApp = SPOTIFY; queueFetch(FETCH_SPOTIFY); drawSpotifyUI(); }
    } else {
      // Settings menu
      if (x > 195) { closeSettingsMenu(); return; }

      // UP button click
      if (hitRect(x, y, 5, 46, 36, 83)) {
        if (settingsScrollOff > 0) settingsScrollOff--;
        openSettingsMenu();
        return;
      }
      
      // DOWN button click
      if (hitRect(x, y, 5, 135, 36, 83)) {
        if (settingsScrollOff < 2) settingsScrollOff++;
        openSettingsMenu();
        return;
      }

      // Check the 3 visible items
      for (int i = 0; i < 3; i++) {
        int idx = i + settingsScrollOff;
        int itemY = 46 + i * 62;
        if (hitRect(x, y, 46, itemY, 144, 52)) {
          if (idx == 0) { // WiFi
            isMenuOpen = false;
            currentApp = WIFI_SCAN;
            if (scanCount == 0) {
              startWifiScan();
            }
            drawWifiScanUI();
          } else if (idx == 1) { // Timer
            isMenuOpen = false;
            currentApp = TIMER;
            drawFocusAppUI();
          } else if (idx == 2) { // Spotify
            isMenuOpen = false;
            currentApp = SPOTIFY;
            queueFetch(FETCH_SPOTIFY);
            drawSpotifyUI();
          } else if (idx == 3) { // Calibrate
            isMenuOpen = false;
            currentApp = TOUCH_CALIB;
            drawCalibrationUI();
          } else if (idx == 4) { // Draw
            isMenuOpen = false;
            currentApp = DRAWING;
            drawDrawingUI();
          }
          break;
        }
      }
    }
  } else if (currentApp == SPOTIFY) {
    if (y < 54 && x < 92) {
      currentApp = DASHBOARD;
      redrawEntireUI();
    } else {
      if (hitRect(x, y, 15, 181, 82, 45)) controlSpotify("previous");
      else if (hitRect(x, y, 109, 181, 102, 45)) controlSpotify(isPlaying ? "pause" : "play");
      else if (hitRect(x, y, 223, 181, 82, 45)) controlSpotify("next");
    }
  } else if (currentApp == PC_STATS || currentApp == TASKS) {
    if (y < 54 && x < 92) {
      currentApp = DASHBOARD;
      redrawEntireUI();
    } else if (currentApp == TASKS) {
      if (hitRect(x, y, 12, 210, 132, 24)) {
        currentApp = DASHBOARD;
        redrawEntireUI();
      } else if (hitRect(x, y, 176, 210, 132, 24)) {
        taskFetchPending = true;
        drawTasksUI();
      } else {
        for (int i = 0; i < 3; i++) {
          if (hitRect(x, y, 12, 70 + i * 46, 296, 38)) {
            requestTaskComplete(i);
            break;
          }
        }
      }
    }
  } else if (currentApp == TIMER || currentApp == STOPWATCH) {
    if (y < 54) {
      if (x < 92) { currentApp = DASHBOARD; redrawEntireUI(); }
    } else if (hitRect(x, y, 36, 42, 116, 28) || hitRect(x, y, 168, 42, 116, 28)) {
      if (hitRect(x, y, 36, 42, 116, 28) && currentApp != TIMER) { currentApp = TIMER; drawFocusAppUI(); }
      else if (hitRect(x, y, 168, 42, 116, 28) && currentApp != STOPWATCH) { currentApp = STOPWATCH; drawFocusAppUI(); }
    } else if (currentApp == TIMER) {
      if (hitRect(x, y, 12, 184, 92, 38) || hitRect(x, y, 114, 184, 194, 38)) {
        if (hitRect(x, y, 12, 184, 92, 38)) { timerRunning = false; timerSeconds = 0; drawFocusAppUI(); }
        else if (hitRect(x, y, 114, 184, 194, 38)) {
          timerRunning = !timerRunning;
          if (timerRunning) lastTimerTick = millis();
          drawFocusAppUI();
        }
      } else if (!timerRunning && (hitRect(x, y, 12, 132, 92, 34) || hitRect(x, y, 114, 132, 92, 34) || hitRect(x, y, 216, 132, 92, 34))) {
        if (hitRect(x, y, 12, 132, 92, 34)) timerSeconds += 25 * 60;
        else if (hitRect(x, y, 114, 132, 92, 34)) timerSeconds += 5 * 60;
        else if (hitRect(x, y, 216, 132, 92, 34)) timerSeconds += 60;
        updateTimerDisplay();
      }
    } else if (currentApp == STOPWATCH) {
      if (hitRect(x, y, 12, 184, 140, 38) || hitRect(x, y, 168, 184, 140, 38)) {
        if (hitRect(x, y, 12, 184, 140, 38)) {
          swRunning = false;
          swElapsedMillis = 0;
          drawFocusAppUI();
        } else if (hitRect(x, y, 168, 184, 140, 38)) {
          swRunning = !swRunning;
          if (swRunning) swStartMillis = millis() - swElapsedMillis;
          drawFocusAppUI();
        }
      }
    }
  } else if (currentApp == DRAWING) {
    handleDrawingTouch(x, y);
  }
}

// ==========================================
//              SPOTIFY UI
// ==========================================
void drawSpotifyUI() {
  tft.fillScreen(ILI9341_BLACK);
  drawTopBar("NOW PLAYING", SPOTIFY_GREEN, true);

  tft.fillRoundRect(8, 48, 304, 116, 8, 0x0841);
  tft.drawRoundRect(8, 48, 304, 116, 8, 0x1C88);
  drawAlbumArtPlaceholder();
  drawCachedAlbumArt();
  updateSongMetrics();
  updateSpotifyText(true);
  drawBeatVisualizer(true);
  drawSpotifyControls();
}

void drawSpotifyControls() {
  tft.fillRect(0, 170, 320, 70, ILI9341_BLACK);
  tft.fillRoundRect(15, 181, 82, 45, 8, 0x3186);
  tft.fillRoundRect(109, 181, 102, 45, 8, SPOTIFY_GREEN);
  tft.fillRoundRect(223, 181, 82, 45, 8, 0x3186);

  tft.fillRect(40, 193, 4, 22, ILI9341_WHITE);
  tft.fillTriangle(66, 193, 66, 215, 49, 204, ILI9341_WHITE);

  bool playing;
  lockData();
  playing = isPlaying;
  unlockData();

  if (playing) {
    tft.fillRect(149, 193, 7, 25, ILI9341_BLACK);
    tft.fillRect(166, 193, 7, 25, ILI9341_BLACK);
  } else {
    tft.fillTriangle(151, 192, 151, 218, 179, 205, ILI9341_BLACK);
  }

  tft.fillTriangle(255, 193, 255, 215, 272, 204, ILI9341_WHITE);
  tft.fillRect(277, 193, 4, 22, ILI9341_WHITE);
}

void drawAlbumArtPlaceholder() {
  tft.fillRoundRect(232, 62, 66, 66, 8, 0x2104);
  tft.drawRoundRect(232, 62, 66, 66, 8, UI_LINE);
  tft.fillCircle(265, 95, 16, SPOTIFY_GREEN);
  tft.fillCircle(265, 95, 6, 0x2104);
}

void drawCachedAlbumArt() {
  bool canDraw;
  lockData();
  canDraw = (imgBuffSize > 0 && cachedAlbumUrl == currentAlbumUrl);
  if (canDraw) {
    tft.fillRect(233, 63, 64, 64, 0x0841);
    TJpgDec.drawJpg(233, 63, imgBuff, imgBuffSize);
  }
  unlockData();
}

void updateSongMetrics() {
  lockData();
  currentSong.toCharArray(marqueeSong, sizeof(marqueeSong));
  unlockData();

  tft.setFont(&FreeSans18pt7b);
  int16_t x1, y1;
  uint16_t w, h;
  tft.getTextBounds(marqueeSong, 0, 0, &x1, &y1, &w, &h);
  int visibleSongWidth = (int)w + max(0, (int)x1);
  songPixelWidth = visibleSongWidth + MARQUEE_TEXT_PADDING_PX;
  needsScrolling = (visibleSongWidth > MARQUEE_VIEW_WIDTH);
  marqueeX = 0;
}

void updateSpotifyText(bool force) {
  char artist[32];
  char statusText[10];
  bool playing;

  lockData();
  currentArtist.toCharArray(artist, sizeof(artist));
  playing = isPlaying;
  unlockData();

  strncpy(statusText, playing ? "Playing" : "Paused", sizeof(statusText));
  statusText[sizeof(statusText) - 1] = '\0';
  fitTextToWidth(artist, 205, &FreeSans9pt7b);

  static char lastSongLine[96] = "";
  static char lastArtistLine[32] = "";
  static char lastStatusLine[10] = "";

  if (force || strcmp(marqueeSong, lastSongLine) != 0) {
    drawMarquee();
    lastMarqueeUpdate = millis();
    strncpy(lastSongLine, marqueeSong, sizeof(lastSongLine));
    lastSongLine[sizeof(lastSongLine) - 1] = '\0';
  }

  if (force || strcmp(artist, lastArtistLine) != 0) {
    tft.fillRect(12, 116, 210, 22, 0x0841);
    tft.setFont(&FreeSans9pt7b);
    tft.setTextColor(SPOTIFY_GREEN);
    tft.setCursor(12, 134);
    tft.print(artist);
    strncpy(lastArtistLine, artist, sizeof(lastArtistLine));
    lastArtistLine[sizeof(lastArtistLine) - 1] = '\0';
  }

  if (force || strcmp(statusText, lastStatusLine) != 0) {
    tft.fillRect(12, 139, 120, 20, 0x0841);
    tft.setFont(&FreeSans9pt7b);
    tft.setTextColor(playing ? ILI9341_WHITE : ILI9341_DARKGREY);
    tft.setCursor(12, 156);
    tft.print(statusText);
    strncpy(lastStatusLine, statusText, sizeof(lastStatusLine));
    lastStatusLine[sizeof(lastStatusLine) - 1] = '\0';
  }
}

void drawMarquee() {
  marqueeCanvas->fillScreen(0x0841);
  marqueeCanvas->setFont(&FreeSans18pt7b);
  marqueeCanvas->setTextColor(ILI9341_WHITE);

  if (needsScrolling) {
    marqueeCanvas->setCursor(marqueeX, 28);
    marqueeCanvas->print(marqueeSong);
  } else {
    marqueeCanvas->setCursor(0, 28);
    marqueeCanvas->print(marqueeSong);
  }
  tft.drawRGBBitmap(MARQUEE_X, MARQUEE_Y, marqueeCanvas->getBuffer(), MARQUEE_VIEW_WIDTH, MARQUEE_VIEW_HEIGHT);
}

void drawBeatVisualizer(bool force) {
  bool playing;
  lockData();
  playing = isPlaying;
  unlockData();

  const int x = 236;
  const int y = 134;
  const int w = 58;
  const int h = 24;
  const int bars = 11;
  const int barWidth = 3;
  const int gap = 2;
  static uint8_t heights[bars] = {5, 8, 4, 12, 7, 15, 9, 5, 11, 6, 10};

  tft.fillRect(x, y, w, h, 0x0841);
  tft.drawFastHLine(x + 1, y + h - 2, w - 2, 0x1C88);

  for (int i = 0; i < bars; i++) {
    uint8_t target;
    if (playing) target = random(4, h - 3);
    else target = 3 + (i % 3);
    if (force) heights[i] = target;
    else heights[i] = (heights[i] * 2 + target) / 3;

    int barX = x + 2 + i * (barWidth + gap);
    int barH = constrain((int)heights[i], 3, h - 4);
    uint16_t color = playing ? (i % 2 == 0 ? SPOTIFY_GREEN : ILI9341_CYAN) : UI_LINE;
    tft.fillRoundRect(barX, y + h - 3 - barH, barWidth, barH, 1, color);
  }
}

// ==========================================
//              TIMER / STOPWATCH UI
// ==========================================
void drawFocusAppUI() {
  tft.fillScreen(ILI9341_BLACK);
  drawTopBar("FOCUS", currentApp == TIMER ? AESTHETIC_GOLD : ILI9341_CYAN, true);

  tft.fillRoundRect(36, 42, 116, 28, 8, currentApp == TIMER ? AESTHETIC_GOLD : UI_PANEL);
  tft.fillRoundRect(168, 42, 116, 28, 8, currentApp == STOPWATCH ? ILI9341_CYAN : UI_PANEL);
  drawCenteredText("TIMER", 36, 42, 116, 28, &FreeSans9pt7b, currentApp == TIMER ? ILI9341_BLACK : ILI9341_DARKGREY);
  drawCenteredText("WATCH", 168, 42, 116, 28, &FreeSans9pt7b, currentApp == STOPWATCH ? ILI9341_BLACK : ILI9341_DARKGREY);

  if (currentApp == TIMER) {
    updateTimerDisplay();
    tft.fillRoundRect(12, 132, 92, 34, 8, 0x2925);
    tft.fillRoundRect(114, 132, 92, 34, 8, 0x2925);
    tft.fillRoundRect(216, 132, 92, 34, 8, 0x2925);
    drawCenteredText("+25m", 12, 132, 92, 34, &FreeSans9pt7b, ILI9341_WHITE);
    drawCenteredText("+5m", 114, 132, 92, 34, &FreeSans9pt7b, ILI9341_WHITE);
    drawCenteredText("+1m", 216, 132, 92, 34, &FreeSans9pt7b, ILI9341_WHITE);

    tft.fillRoundRect(12, 184, 92, 38, 8, ILI9341_RED);
    tft.fillRoundRect(114, 184, 194, 38, 8, timerRunning ? UI_ORANGE : ILI9341_GREEN);
    drawCenteredText("RESET", 12, 184, 92, 38, &FreeSans9pt7b, ILI9341_WHITE);
    drawCenteredText(timerRunning ? "PAUSE" : "START", 114, 184, 194, 38, &FreeSans9pt7b, ILI9341_BLACK);
  } else {
    updateStopwatchDisplay();
    tft.fillRoundRect(12, 184, 140, 38, 8, ILI9341_RED);
    tft.fillRoundRect(168, 184, 140, 38, 8, swRunning ? UI_ORANGE : ILI9341_GREEN);
    drawCenteredText("RESET", 12, 184, 140, 38, &FreeSans9pt7b, ILI9341_WHITE);
    drawCenteredText(swRunning ? "PAUSE" : "START", 168, 184, 140, 38, &FreeSans9pt7b, ILI9341_BLACK);
  }
}

void updateTimerDisplay() {
  if (currentApp != TIMER) return;
  tft.fillRoundRect(24, 78, 272, 44, 8, UI_PANEL);
  int m = timerSeconds / 60;
  int s = timerSeconds % 60;
  char timeStr[10];
  snprintf(timeStr, sizeof(timeStr), "%02d:%02d", m, s);
  drawCenteredText(timeStr, 24, 78, 272, 44, &FreeSans18pt7b, AESTHETIC_GOLD);
}

void updateStopwatchDisplay() {
  if (currentApp != STOPWATCH) return;
  tft.fillRoundRect(24, 78, 272, 44, 8, UI_PANEL);
  unsigned long totalSeconds = swElapsedMillis / 1000;
  int m = totalSeconds / 60;
  int s = totalSeconds % 60;
  int ms = (swElapsedMillis % 1000) / 100;
  char timeStr[15];
  snprintf(timeStr, sizeof(timeStr), "%02d:%02d.%d", m, s, ms);
  drawCenteredText(timeStr, 24, 78, 272, 44, &FreeSans18pt7b, ILI9341_CYAN);
}

// ==========================================
//              PC STATS UI
// ==========================================
void drawPcStatsUI() {
  tft.fillScreen(ILI9341_BLACK);
  drawTopBar("PC STATS", ILI9341_CYAN, true);

  tft.fillRoundRect(12, 74, 142, 54, 8, 0x032C);
  tft.fillRoundRect(166, 74, 142, 54, 8, 0x480C);
  tft.fillRoundRect(12, 142, 142, 54, 8, 0x2925);
  tft.fillRoundRect(166, 142, 142, 54, 8, 0x03A6);
  drawPcStatsValues(true);
}

void drawPcStatsValues(bool force) {
  float cpu, gpu, ram, wifiMbps;
  unsigned long lastSeen;

  lockData();
  cpu = pcCpuUsage;
  gpu = pcGpuTemp;
  ram = pcRamUsage;
  wifiMbps = pcWifiMbps;
  lastSeen = lastTelemetryMillis;
  unlockData();

  if (currentApp != PC_STATS || isMenuOpen) return;

  bool stale = (lastSeen == 0 || (unsigned long)(millis() - lastSeen) > TELEMETRY_STALE_MS);
  char statusLine[32];
  char cpuLine[16];
  char ramLine[16];
  char gpuLine[16];
  char wifiLine[16];

  strncpy(statusLine, "Live from laptop", sizeof(statusLine));
  snprintf(cpuLine, sizeof(cpuLine), "CPU: %d%%", (int)cpu);
  snprintf(ramLine, sizeof(ramLine), "RAM: %d%%", (int)ram);
  if (gpu >= 0) snprintf(gpuLine, sizeof(gpuLine), "GPU: %d%%", (int)gpu);
  else strncpy(gpuLine, "GPU: --%", sizeof(gpuLine));
  snprintf(wifiLine, sizeof(wifiLine), "NET: %d%%", (int)wifiMbps);
  
  statusLine[sizeof(statusLine) - 1] = '\0';
  cpuLine[sizeof(cpuLine) - 1] = '\0';
  ramLine[sizeof(ramLine) - 1] = '\0';
  gpuLine[sizeof(gpuLine) - 1] = '\0';
  wifiLine[sizeof(wifiLine) - 1] = '\0';

  static char lastStatusLine[32] = "";
  static char lastCpuLine[16] = "";
  static char lastRamLine[16] = "";
  static char lastGpuLine[16] = "";
  static char lastWifiLine[16] = "";

  if (force || strcmp(statusLine, lastStatusLine) != 0) {
    tft.fillRect(12, 42, 296, 22, ILI9341_BLACK);
    tft.setFont(&FreeSans9pt7b);
    tft.setTextColor(ILI9341_DARKGREY);
    tft.setCursor(16, 58);
    tft.print(statusLine);
    strncpy(lastStatusLine, statusLine, sizeof(lastStatusLine));
    lastStatusLine[sizeof(lastStatusLine) - 1] = '\0';
  }

  if (force || strcmp(cpuLine, lastCpuLine) != 0) {
    tft.fillRoundRect(12, 74, 142, 54, 8, 0x032C);
    drawCenteredText(cpuLine, 12, 74, 142, 54, &FreeSans9pt7b, ILI9341_WHITE);
    strncpy(lastCpuLine, cpuLine, sizeof(lastCpuLine));
    lastCpuLine[sizeof(lastCpuLine) - 1] = '\0';
  }

  if (force || strcmp(ramLine, lastRamLine) != 0) {
    tft.fillRoundRect(166, 74, 142, 54, 8, 0x480C);
    drawCenteredText(ramLine, 166, 74, 142, 54, &FreeSans9pt7b, ILI9341_WHITE);
    strncpy(lastRamLine, ramLine, sizeof(lastRamLine));
    lastRamLine[sizeof(lastRamLine) - 1] = '\0';
  }

  if (force || strcmp(gpuLine, lastGpuLine) != 0) {
    tft.fillRoundRect(12, 142, 142, 54, 8, 0x2925);
    drawCenteredText(gpuLine, 12, 142, 142, 54, &FreeSans9pt7b, ILI9341_WHITE);
    strncpy(lastGpuLine, gpuLine, sizeof(lastGpuLine));
    lastGpuLine[sizeof(lastGpuLine) - 1] = '\0';
  }

  if (force || strcmp(wifiLine, lastWifiLine) != 0) {
    tft.fillRoundRect(166, 142, 142, 54, 8, 0x03A6);
    drawCenteredText(wifiLine, 166, 142, 142, 54, &FreeSans9pt7b, ILI9341_WHITE);
    strncpy(lastWifiLine, wifiLine, sizeof(lastWifiLine));
    lastWifiLine[sizeof(lastWifiLine) - 1] = '\0';
  }
}

// ==========================================
//              TASKS UI
// ==========================================
void drawTasksUI() {
  char tasks[3][38];
  char ids[3][16];
  int count;

  lockData();
  count = taskCount;
  for (int i = 0; i < 3; i++) {
    if (i < count) taskItems[i].toCharArray(tasks[i], sizeof(tasks[i]));
    else tasks[i][0] = '\0';
    taskIds[i].toCharArray(ids[i], sizeof(ids[i]));
  }
  unlockData();

  tft.fillScreen(ILI9341_BLACK);
  drawTopBar("TASKS", AESTHETIC_GOLD, true);

  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(ILI9341_DARKGREY);
  tft.setCursor(16, 58);
  tft.print(todoistToken == "" ? "Local list" : "Today");

  for (int i = 0; i < 3; i++) {
    uint16_t color = (i == 0) ? 0x2925 : ((i == 1) ? 0x032C : 0x480C);
    int y = 70 + i * 46;
    fitTextToWidth(tasks[i], 226, &FreeSans9pt7b);
    tft.fillRoundRect(12, y, 296, 38, 8, color);
    tft.drawRoundRect(20, y + 10, 18, 18, 4, ids[i][0] != '\0' ? AESTHETIC_GOLD : ILI9341_DARKGREY);
    tft.setTextColor(ILI9341_WHITE);
    tft.setCursor(50, y + 25);
    tft.print(tasks[i]);
  }

  tft.fillRoundRect(12, 210, 132, 24, 8, UI_PANEL);
  tft.fillRoundRect(176, 210, 132, 24, 8, UI_PANEL);
  drawCenteredText("HOME", 12, 210, 132, 24, &FreeSans9pt7b, ILI9341_WHITE);
  drawCenteredText("REFRESH", 176, 210, 132, 24, &FreeSans9pt7b, AESTHETIC_GOLD);
}

void requestTaskComplete(int index) {
  if (index < 0 || index >= taskCount) return;

  String id;
  lockData();
  id = taskIds[index];
  unlockData();

  if (id != "") {
    lockData();
    taskCompleteId = id;
    taskCompletePending = true;
    unlockData();
  }
  removeLocalTaskAt(index);
}

void removeLocalTaskAt(int index) {
  bool drawNow = (currentApp == TASKS);
  lockData();
  if (index >= 0 && index < taskCount) {
    for (int i = index; i < taskCount - 1; i++) {
      taskItems[i] = taskItems[i + 1];
      taskIds[i] = taskIds[i + 1];
    }
    taskCount--;
    if (taskCount <= 0) {
      taskItems[0] = "No tasks today";
      taskIds[0] = "";
      taskCount = 1;
    } else {
      taskItems[taskCount] = "";
      taskIds[taskCount] = "";
    }
    tasksDirty = !drawNow;
  }
  unlockData();
  if (drawNow) drawTasksUI();
}
