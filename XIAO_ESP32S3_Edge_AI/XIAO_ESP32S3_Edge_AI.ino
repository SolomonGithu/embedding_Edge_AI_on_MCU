// Install the ESPAsyncWebServer library

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include "FS.h"
#include "SD.h"
#include "SPI.h"
#include "esp_camera.h"

// WiFi and Access Point(AP) credentials. Board switches to AP mode when WiFi connection fails
const char* wifi_SSID = "********";
const char* wifi_password = "********";
const char* ap_SSID = "XIAO_ESP32S3";   // put the name of the Access Point (Wi-Fi hotspot)
const char* ap_password = "1234567890"; // put the password of the Access point (Wi-Fi hotspot)

unsigned long wifi_connectTimeout = 10000;  // in ms
String ip_address;

AsyncWebServer server(80); // server port

// WARNING!!! PSRAM IC required for UXGA resolution and high JPEG quality
//            Ensure ESP32 Wrover Module or other board with PSRAM is selected
//            Partial images will be transmitted if image exceeds buffer size
//
//            You must select partition scheme from the board menu that has at least 3MB APP space.
//            Face Recognition is DISABLED for ESP32 and ESP32-S2, because it takes up from 15
//            seconds to process single frame. Face Detection is ENABLED if PSRAM is enabled as well

// ===================
// Select camera model
// ===================
//#define CAMERA_MODEL_WROVER_KIT // Has PSRAM
//#define CAMERA_MODEL_ESP_EYE // Has PSRAM
//#define CAMERA_MODEL_ESP32S3_EYE // Has PSRAM
//#define CAMERA_MODEL_M5STACK_PSRAM // Has PSRAM
//#define CAMERA_MODEL_M5STACK_V2_PSRAM // M5Camera version B Has PSRAM
//#define CAMERA_MODEL_M5STACK_WIDE // Has PSRAM
//#define CAMERA_MODEL_M5STACK_ESP32CAM // No PSRAM
//#define CAMERA_MODEL_M5STACK_UNITCAM // No PSRAM
//#define CAMERA_MODEL_AI_THINKER // Has PSRAM
//#define CAMERA_MODEL_TTGO_T_JOURNAL // No PSRAM
#define CAMERA_MODEL_XIAO_ESP32S3 // Has PSRAM
// ** Espressif Internal Boards **
//#define CAMERA_MODEL_ESP32_CAM_BOARD
//#define CAMERA_MODEL_ESP32S2_CAM_BOARD
//#define CAMERA_MODEL_ESP32S3_CAM_LCD
//#define CAMERA_MODEL_DFRobot_FireBeetle2_ESP32S3 // Has PSRAM
//#define CAMERA_MODEL_DFRobot_Romeo_ESP32S3 // Has PSRAM
#include "camera_pins.h"
#include "AsyncWebCamera.h"

void initSDCard() {
  if (!SD.begin(21)) {
    Serial.println("Card Mount Failed");
    return;
  }
  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("No SD card attached!");
    return;
  }
  Serial.print("SD Card Type: ");
  if (cardType == CARD_MMC) Serial.println("MMC");
  else if (cardType == CARD_SD) Serial.println("SDSC");
  else if (cardType == CARD_SDHC) Serial.println("SDHC");
  else Serial.println("UNKNOWN");

  uint64_t cardSize = SD.cardSize() / (1024 * 1024);
  Serial.printf("SD Card Size: %lluMB\n", cardSize);
}

void init_WiFi_or_AP() {
  /* *** First try to connect to WiFi *** */
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifi_SSID, wifi_password);
  WiFi.setSleep(false);
  Serial.print("Connecting to "); Serial.print(wifi_SSID); Serial.print(" ");
  unsigned long startAttemptTime = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < wifi_connectTimeout) {
    Serial.print(".");
    delay(100);
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected to "); Serial.print(wifi_SSID); Serial.println("!");
    ip_address = WiFi.localIP().toString();
  }

  /* **** Create an Access Point if joining a WiFi network failed **** */
  else {
    Serial.print("Failed to connect to "); Serial.print(wifi_SSID); Serial.println("! Starting Access Point...");
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ap_SSID, ap_password);
    ip_address = WiFi.softAPIP().toString();
  }

  Serial.print("Camera Ready! Use 'http://"); Serial.print(ip_address); Serial.println("' to connect");
}

void setup() {
  Serial.begin(115200);
  init_WiFi_or_AP();    // initialize the Wi-Fi or AP connection
  initSDCard();         // initialize the SD card
  if (!initCamera()) {  // initialize the on-board camera
    Serial.println("Camera init failed");
    return;
  }

  /* ***** defining URL routes ***** */
  // 1. URL for serving HTML file (with JS scripts) and images
  server.on("/", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send(SD, "/web_ai/index.html", "text/html");
  });
  server.serveStatic("/seeed_studio_favicon", SD, "/web_ai/seeed_studio_favicon.png").setCacheControl("public, max-age=86400"); // 1 day caching
  server.serveStatic("/seeed_studio_logo", SD, "/web_ai/seeed_studio_logo.jpg").setCacheControl("public, max-age=86400"); // 1 day caching
  server.serveStatic("/tfjs", SD, "/web_ai/tfjs_4.13.0.js").setCacheControl("public, max-age=86400"); // 1 day caching
  server.on("/camera_feed", HTTP_GET, StreamJpg); // URL for sending live feed from the camera sensor

  // 2. URL for serving model files if they exist. Model files are stored in root of the SD card.
  // DO NOT CHANGE THE model.weights.bin URL naming!!
  // The WeightsManifest in the model.json contains this path and it will automatically fetch it.
  server.on("/model.json", HTTP_GET, [](AsyncWebServerRequest * request) {
    Serial.println("Request received: /model.json");
    if (SD.exists("/model.json")) { // check if the model.json file exists on the SD card
      request->send(SD, "/model.json", "application/json");
    } else {
      request->send(404, "application/json", "{\"error\":\"model.json not found\"}");
    }
  });
  server.on("/model.weights.bin", HTTP_GET, [](AsyncWebServerRequest * request) {
    Serial.println("Request received: /model.weights.bin");
    if (SD.exists("/model.weights.bin")) {  // check if the model.weights.bin file exists on the SD card
      request->send(SD, "/model.weights.bin", "application/octet-stream");
    } else {
      request->send(404, "application/json", "{\"error\":\"model.weights.bin not found\"}");
    }
  });

  // 3. URL to save model files to SD card. The model is received by a HTTP POST request
  server.on("/save_model", HTTP_POST, [](AsyncWebServerRequest * request) {
    request->send(200, "application/json", "{\"message\": \"Successful save of model to SD card.\"}");
  }, [](AsyncWebServerRequest * request,
        const String & filename,
        size_t index,
        uint8_t *data,
        size_t len,
        bool final) {
    static File file_to_save;

    if (index == 0) {
      Serial.printf("Saving to SD card started: %s\n", filename.c_str());

      String fullPath = "/" + filename;
      if (SD.exists(fullPath)) {
        SD.remove(fullPath);
      }

      file_to_save = SD.open(fullPath, FILE_WRITE);
      if (!file_to_save) {
        Serial.println("Failed to open file for writing");
        return;
      }
    }

    if (file_to_save) {
      file_to_save.write(data, len);
    }

    if (final) {
      Serial.printf("Saving to SD card complete: %s, size: %u bytes\n", filename.c_str(), index + len);
      if (file_to_save) file_to_save.close();
    }
  });

  server.onNotFound(error_404); // 4. 404 handler for routes that are not found
  server.serveStatic("/", SD, "/");
  server.begin();
}

void loop() { // Nothing here
}

// Returns error 404 :)
void error_404(AsyncWebServerRequest *request) {
  request->send(404, "text/plain", "404. The requested URL was not found!");
}

// stream jpg images using async web server
void StreamJpg(AsyncWebServerRequest *request) {
  AsyncJpegStreamResponse *response = new AsyncJpegStreamResponse();
  if (!response) {
    // If the response object could not be created, send 501 (Not Implemented)
    request->send(501);
    return;
  }
  // adding headers
  response->addHeader("Access-Control-Allow-Origin", "*");
  response->addHeader("X-Framerate", "60");
  response->addHeader("Cache-Control", "no-store"); // Do not cache the stream

  request->send(response);
}
