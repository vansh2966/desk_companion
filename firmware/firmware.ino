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

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans18pt7b.h>

#define AESTHETIC_GOLD 0xCE40
#define SPOTIFY_GREEN  0x1EDB
#define MENU_BG        0x2104
#define UI_BG          0x0008
#define UI_PANEL       0x1084
#define UI_LINE        0x4208
#define UI_ORANGE      0xFD20

// Wi-Fi Credentials 
const char* ssid     = "";
const char* password = "";

//  API Keys 
String weatherApiKey = "";
String city = "";
String countryCode = "";

// SPOTIFY CREDENTIALS 
String spotifyRefreshToken = "";
String spotifyBase64Auth = "";

// TODOISD
String todoistToken = ""; // Paste your Todoist API token

//Pins
#define SPI_SCK  7
#define SPI_MISO 44
#define SPI_MOSI 9
#define TFT_CS   2
#define TFT_DC   4
#define TFT_RST  1
#define TFT_BL   43
#define TOUCH_CS 5

// --- App State ---
enum AppState { DASHBOARD, TIMER, STOPWATCH, SPOTIFY, PC_STATS, TASKS };
volatile AppState currentApp = DASHBOARD;
bool isMenuOpen = false;

enum FetchState { FETCH_IDLE, FETCH_SPOTIFY, FETCH_WEATHER };
volatile bool spotifyFetchPending = false;
volatile bool weatherFetchPending = false;
volatile unsigned long spotifyFetchReadyAt = 0;

// --- Hardware ---
Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);
XPT2046_Touchscreen touch(TOUCH_CS);
WiFiClientSecure secureClient;
WiFiUDP telemetryUdp;
SemaphoreHandle_t dataMutex;
TaskHandle_t networkTaskHandle = NULL;

// --- Spotify State ---
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

// Album Art Buffer
#define ALBUM_ART_MAX_SIZE 12000
static uint8_t imgBuff[ALBUM_ART_MAX_SIZE];
static uint8_t imgDownloadBuff[ALBUM_ART_MAX_SIZE];
size_t imgBuffSize = 0;
String cachedAlbumUrl = "";

// Weather State
float currentTemp = 0.0;
int currentHumidity = 0;
String currentWeather = "--";
volatile bool weatherDirty = false;
unsigned long lastWeatherUpdate = 0;

// PC Telemetry State 
float pcCpuUsage = 0.0;
float pcGpuTemp = -1.0;
float pcRamUsage = 0.0;
float pcWifiMbps = 0.0;
unsigned long lastTelemetryMillis = 0;
volatile bool telemetryDirty = true;

// --- Task State ---
String taskItems[3] = {"Add Todoist token", "Open Tasks", "Stay focused"};
String taskIds[3] = {"", "", ""};
int taskCount = 3;
volatile bool tasksDirty = true;
volatile bool taskFetchPending = false;
volatile bool taskCompletePending = false;
String taskCompleteId = "";
unsigned long lastTaskFetch = 0;

//Time
const char* ntpServer  = "pool.ntp.org";
const char* ntpServer2 = "time.google.com";
const char* ntpServer3 = "time.cloudflare.com";
const long  gmtOffset_sec = 19800;
const int   daylightOffset_sec = 0;
unsigned long lastNTPSync = 0;
char lastTopClock[10] = "";
int lastDashboardMinute = -1;

// Power / Display
int currentBrightness = 155;
const int MIN_BRIGHTNESS = 20;
const int MAX_BRIGHTNESS = 230;
const int DIM_BRIGHTNESS = 28;
volatile bool screenAwake = true;
bool screenDimmed = false;
unsigned long lastInteractionMillis = 0;
unsigned long lastTouchTime = 0;

// Timer/SW Variables
long timerSeconds = 25 * 60;
bool timerRunning = false;
unsigned long lastTimerTick = 0;
unsigned long swStartMillis = 0;
unsigned long swElapsedMillis = 0;
bool swRunning = false;

//Marquee
GFXcanvas16* marqueeCanvas;
int marqueeX = 0;
int songPixelWidth = 0;
bool needsScrolling = false;
unsigned long lastMarqueeUpdate = 0;
unsigned long lastBeatUpdate = 0;
char marqueeSong[96] = "Loading...";

// Timing constants 
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

//Forward declaration
bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap);
static inline bool elapsedSince(unsigned long now, unsigned long last, unsigned long interval);
static inline bool timeReached(unsigned long now, unsigned long target);
void lockData();
void unlockData();
void queueFetch(uint8_t fetchType, unsigned long delayMs);
void queueFetch(uint8_t fetchType);
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

void queueFetch(uint8_t fetchType, unsigned long delayMs) {
  if (fetchType == FETCH_SPOTIFY) {
    spotifyFetchPending = true;
    spotifyFetchReadyAt = (delayMs > 0) ? millis() + delayMs : 0;
  } else if (fetchType == FETCH_WEATHER) {
    weatherFetchPending = true;
  }
}

void queueFetch(uint8_t fetchType) {
  queueFetch(fetchType, 0);
}

void queueSpotifyControl(const char* action) {
  lockData();
  strncpy(spotifyControlAction, action, sizeof(spotifyControlAction) - 1);
  spotifyControlAction[sizeof(spotifyControlAction) - 1] = '\0';
  spotifyControlPending = true;
  unlockData();
}

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

  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(ILI9341_CYAN);
  tft.setCursor(20, 106);
  tft.print("Connecting Wi-Fi...");

  secureClient.setInsecure();
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) delay(250);
  telemetryUdp.begin(TELEMETRY_PORT);

  tft.fillScreen(UI_BG);
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
  redrawEntireUI();

  xTaskCreatePinnedToCore(
    networkTask,
    "NetworkTask",
    12288,
    NULL,
    1,
    &networkTaskHandle,
    0
  );
}

void loop() {
  unsigned long currentMillis = millis();

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

  static bool touchWasDown = false;
  bool touching = touch.touched();
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
  touchWasDown = touching;
  vTaskDelay(pdMS_TO_TICKS(2));
}


// BACKGROUND NETWORK

void networkTask(void* pvParameters) {
  for (;;) {
    unsigned long now = millis();

    if (WiFi.status() != WL_CONNECTED) {
      WiFi.reconnect();
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    pollTelemetryUdp();

    if (elapsedSince(now, lastSpotifyTokenRefresh, SPOTIFY_TOKEN_INTERVAL_MS)) {
      refreshSpotifyToken();
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
      fetchTodoistTasks();
      lastTaskFetch = millis();
    }

    if (spotifyControlPending) {
      char action[12];
      lockData();
      strncpy(action, spotifyControlAction, sizeof(action) - 1);
      action[sizeof(action) - 1] = '\0';
      spotifyControlPending = false;
      unlockData();
      sendSpotifyControl(action);
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
        completeTodoistTask(taskId);
        taskFetchPending = true;
      }
    }

    if (spotifyFetchPending && timeReached(now, spotifyFetchReadyAt)) {
      spotifyFetchPending = false;
      spotifyFetchReadyAt = 0;
      fetchSpotifyData();
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

void refreshSpotifyToken() {
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

void fetchSpotifyData() {
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

void sendSpotifyControl(const char* action) {
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

void fetchTodoistTasks() {
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

void completeTodoistTask(const String& taskId) {
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
    if (contentDirty || controlsDirty) {
      updateSpotifyText(false);
    }
    if (controlsDirty) drawSpotifyControls();
    if (albumDirty) {
      drawAlbumArtPlaceholder();
      drawCachedAlbumArt();
    }
  }

  if (currentApp == DASHBOARD && !isMenuOpen && weatherNeedsDraw) {
    updateWeatherDisplay();
  }
  if (currentApp == DASHBOARD && !isMenuOpen && telemetryNeedsDraw) {
    drawPcStatsPreview(false);
  } else if (currentApp == PC_STATS && telemetryNeedsDraw) {
    drawPcStatsValues(false);
  }
  if (currentApp == DASHBOARD && !isMenuOpen && tasksNeedDraw) {
    drawTasksPreview(false);
  } else if (currentApp == TASKS && tasksNeedDraw) {
    drawTasksUI();
  }
}

void refreshTelemetryStaleUi(unsigned long now) {
  static bool initialized = false;
  static bool lastFresh = false;
  unsigned long lastSeen;

  lockData();
  lastSeen = lastTelemetryMillis;
  unlockData();

  bool fresh = (lastSeen != 0 && (unsigned long)(now - lastSeen) <= TELEMETRY_STALE_MS);
  if (initialized && fresh != lastFresh && screenAwake && !isMenuOpen) {
    if (currentApp == DASHBOARD) drawPcStatsPreview(false);
    else if (currentApp == PC_STATS) drawPcStatsValues(false);
  }
  lastFresh = fresh;
  initialized = true;
}


// TIME / TOP BAR

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

// DISPLAY / POWER / TOUCH

void initDisplaySafely() {
  pinMode(TFT_BL, OUTPUT);
  analogWrite(TFT_BL, 0);

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
  analogWrite(TFT_BL, value);
}

void fadeBacklightTo(int target) {
  target = constrain(target, 0, MAX_BRIGHTNESS);
  int start = screenAwake ? currentBrightness : 0;
  if (target < start) {
    for (int b = start; b >= target; b -= 8) {
      setBacklight(b);
      delay(4);
    }
  } else {
    for (int b = start; b <= target; b += 8) {
      setBacklight(b);
      delay(4);
    }
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
  if (currentApp == SPOTIFY || currentApp == PC_STATS) return true;
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
    delay(1);
  }

  rawX /= samples;
  rawY /= samples;
  x = constrain(map(rawX, 3900, 120, 0, 319), 0, 319);
  y = constrain(map(rawY, 3900, 120, 0, 239), 0, 239);
  return true;
}


//UI FLOW

void redrawCurrentApp() {
  if (currentApp == DASHBOARD) redrawEntireUI();
  else if (currentApp == SPOTIFY) drawSpotifyUI();
  else if (currentApp == PC_STATS) drawPcStatsUI();
  else if (currentApp == TASKS) drawTasksUI();
  else drawFocusAppUI();
}

void redrawEntireUI() {
  isMenuOpen = false;
  drawDashboardUI();
}

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
  tft.fillRoundRect(10, 46, 300, 74, 8, UI_PANEL);
  tft.drawRoundRect(10, 46, 300, 74, 8, AESTHETIC_GOLD);

  tft.fillRoundRect(10, 126, 145, 56, 8, 0x032C);
  tft.fillRoundRect(165, 126, 145, 56, 8, 0x480C);
  tft.fillRoundRect(10, 188, 145, 36, 8, 0x2925);
  tft.fillRoundRect(165, 188, 145, 36, 8, 0x03A6);

  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(ILI9341_CYAN);
  tft.setCursor(22, 148);
  tft.print("PC");
  tft.setTextColor(ILI9341_MAGENTA);
  tft.setCursor(177, 148);
  tft.print("TASKS");
  drawCenteredText("TIMER", 10, 188, 145, 36, &FreeSans9pt7b, AESTHETIC_GOLD);
  drawCenteredText("SPOTIFY", 165, 188, 145, 36, &FreeSans9pt7b, SPOTIFY_GREEN);
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
  if (stale) {
    strncpy(line1, "Waiting", sizeof(line1));
    snprintf(line2, sizeof(line2), "UDP %u", TELEMETRY_PORT);
  } else {
    snprintf(line1, sizeof(line1), "CPU %d%%", (int)cpu);
    if (gpu >= 0) snprintf(line2, sizeof(line2), "RAM %d%% G%dC", (int)ram, (int)gpu);
    else snprintf(line2, sizeof(line2), "RAM %d%%", (int)ram);
  }
  line1[sizeof(line1) - 1] = '\0';
  line2[sizeof(line2) - 1] = '\0';

  static char lastLine1[18] = "";
  static char lastLine2[18] = "";
  if (force || strcmp(line1, lastLine1) != 0 || strcmp(line2, lastLine2) != 0) {
    tft.fillRect(20, 151, 126, 27, 0x032C);
    tft.setFont(&FreeSans9pt7b);
    tft.setTextColor(ILI9341_WHITE);
    tft.setCursor(22, 164);
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

  if (stale) {
    String localIp = WiFi.localIP().toString();
    snprintf(statusLine, sizeof(statusLine), "UDP %u @ %s", TELEMETRY_PORT, localIp.c_str());
    strncpy(cpuLine, "CPU --", sizeof(cpuLine));
    strncpy(ramLine, "RAM --", sizeof(ramLine));
    strncpy(gpuLine, "GPU --", sizeof(gpuLine));
    strncpy(wifiLine, "WiFi --", sizeof(wifiLine));
  } else {
    strncpy(statusLine, "Live from laptop", sizeof(statusLine));
    snprintf(cpuLine, sizeof(cpuLine), "CPU %d%%", (int)cpu);
    snprintf(ramLine, sizeof(ramLine), "RAM %d%%", (int)ram);
    if (gpu >= 0) snprintf(gpuLine, sizeof(gpuLine), "GPU %d C", (int)gpu);
    else strncpy(gpuLine, "GPU --", sizeof(gpuLine));
    snprintf(wifiLine, sizeof(wifiLine), "WiFi %.1fM", wifiMbps);
  }
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
    tft.setTextColor(stale ? UI_ORANGE : ILI9341_DARKGREY);
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

void openSettingsMenu() {
  isMenuOpen = true;
  tft.fillRect(0, 0, 190, 240, MENU_BG);

  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(16, 25);
  tft.print("SETTINGS");
  tft.drawLine(16, 36, 174, 36, UI_LINE);

  tft.fillRoundRect(15, 60, 160, 34, 8, 0x3186);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(56, 83);
  tft.print("SLEEP");

  tft.fillRoundRect(15, 108, 160, 34, 8, AESTHETIC_GOLD);
  tft.setTextColor(ILI9341_BLACK);
  tft.setCursor(58, 131);
  tft.print("TIMER");

  tft.fillRoundRect(15, 156, 160, 34, 8, SPOTIFY_GREEN);
  tft.setCursor(46, 179);
  tft.print("SPOTIFY");
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

void handleTouch(int x, int y) {
  if (currentApp == DASHBOARD) {
    if (!isMenuOpen) {
      if (x < 86 && y < 54) openSettingsMenu();
      else if (hitRect(x, y, 10, 128, 145, 52)) { currentApp = PC_STATS; drawPcStatsUI(); }
      else if (hitRect(x, y, 165, 128, 145, 52)) { currentApp = TASKS; drawTasksUI(); }
      else if (hitRect(x, y, 165, 188, 145, 36)) { currentApp = SPOTIFY; queueFetch(FETCH_SPOTIFY); drawSpotifyUI(); }
      else if (hitRect(x, y, 10, 188, 145, 36)) { currentApp = TIMER; drawFocusAppUI(); }
    } else {
      if (x > 180) closeSettingsMenu();
      else if (hitRect(x, y, 15, 60, 160, 34)) {
        closeSettingsMenu();
        sleepScreen();
      } else if (hitRect(x, y, 15, 108, 160, 34)) {
        isMenuOpen = false;
        currentApp = TIMER;
        drawFocusAppUI();
      } else if (hitRect(x, y, 15, 156, 160, 34)) {
        isMenuOpen = false;
        currentApp = SPOTIFY;
        queueFetch(FETCH_SPOTIFY);
        drawSpotifyUI();
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
  }
}


//SPOTIFY UI

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
    if (playing) {
      target = random(4, h - 3);
    } else {
      target = 3 + (i % 3);
    }
    if (force) heights[i] = target;
    else heights[i] = (heights[i] * 2 + target) / 3;

    int barX = x + 2 + i * (barWidth + gap);
    int barH = constrain((int)heights[i], 3, h - 4);
    uint16_t color = playing ? (i % 2 == 0 ? SPOTIFY_GREEN : ILI9341_CYAN) : UI_LINE;
    tft.fillRoundRect(barX, y + h - 3 - barH, barWidth, barH, 1, color);
  }
}


//TIMER / STOPWATCH UI

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
  sprintf(timeStr, "%02d:%02d", m, s);
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
  sprintf(timeStr, "%02d:%02d.%d", m, s, ms);
  drawCenteredText(timeStr, 24, 78, 272, 44, &FreeSans18pt7b, ILI9341_CYAN);
}
