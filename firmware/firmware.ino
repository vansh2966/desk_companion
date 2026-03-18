#include <TFT_eSPI.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <WiFi.h>
#include "time.h"


TFT_eSPI tft = TFT_eSPI();
Adafruit_BME280 bme;

!)
const char* ssid     = "Airtel_9722576061_5GHz";
const char* password = "air55665";
const char* ntpServer = "pool.ntp.org";

void setup() {
  Serial.begin(115200);

  // Initialize Display
  tft.init();
  tft.setRotation(1); 
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Connecting to WiFi...", 10, 10, 2);

  // Initialize WiFi & Time
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); }
  configTime(0, 0, ntpServer); 

  // Initialize BME280 (
  if (!bme.begin(0x76)) { 
    tft.drawString("BME280 Error!", 10, 30, 2);
  }

  tft.fillScreen(TFT_BLACK);
}

void loop() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    tft.drawString("Time Error", 10, 10, 4);
    return;
  }

  // Display Time
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawRightString(String(timeinfo.tm_hour) + ":" + String(timeinfo.tm_min), 160, 40, 7);

  // Display Sensor Data
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("Temp: " + String(bme.readTemperature()) + " C", 20, 120, 4);
  tft.drawString("Humi: " + String(bme.readHumidity()) + " %", 20, 150, 4);

  delay(1000); 
}