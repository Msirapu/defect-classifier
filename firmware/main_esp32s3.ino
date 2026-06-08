/*
 * =====================================================================
 * Visual Defect Classifier — ESP32-S3 Firmware (Presigned URL Mode)
 * =====================================================================
 *
 * TARGET BOARD : ESP32-S3 (e.g. Espressif ESP32-S3-DevKitC-1)
 *                Board: "ESP32S3 Dev Module"  in Arduino IDE
 *
 * WHAT CHANGED vs. the original ESP32 sketch
 * -------------------------------------------
 *  1. Board headers: WiFiClientSecure & PubSubClient are identical;
 *     the ESP32-S3 chip is fully supported by Arduino-ESP32 v2.x+.
 *  2. PSRAM support: ESP32-S3 has octal PSRAM.  The image buffer now
 *     uses ps_malloc() so you can handle images up to ~200 KB instead
 *     of being capped at ~50 KB in internal SRAM.
 *  3. MAX_IMAGE_SIZE raised to 200 KB accordingly.
 *  4. LED GPIO remapped to pins that are safe on the ESP32-S3 DevKit
 *     (GPIO 1, 2, 3 share with UART on S3 — we use 15, 16, 17).
 *  5. Serial.begin() baud left at 115200; no other UART changes needed.
 *  6. Added PSRAM init check in setup() with a clear error if PSRAM is
 *     not enabled in the board settings (Partition scheme / PSRAM).
 *  7. uploadToS3 scoped TLS client unchanged — still avoids touching
 *     the MQTT TLS stack during HTTP upload.
 *  8. MQTT payload extended: Lambda now writes to AWS Timestream, so
 *     the IoT payload carries a numeric unix timestamp alongside the
 *     ISO-8601 string (Timestream ingestion uses epoch milliseconds).
 *  9. State-machine enum and logic are identical to the original.
 * =====================================================================
 */

#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// ─── WiFi ─────────────────────────────────────────────────────────────────────
const char* ssid     = "";   // ← fill in
const char* password = "";   // ← fill in

// ─── S3 Pre-signed URL ────────────────────────────────────────────────────────
// Generate with: aws s3 presign s3://<bucket>/<key> --expires-in 3600
const char* s3PresignedUrl = "";  // ← fill in

// ─── AWS IoT Core ─────────────────────────────────────────────────────────────
const char* iotEndpoint = "a11261zvniicx8-ats.iot.us-east-2.amazonaws.com"; // ← your endpoint
const char* mqttTopic   = "inspection/result";
const char* clientId    = "ESP32S3-Defect-Classifier";
const char* alpnProtos[]= {"x-amzn-mqtt-ca", NULL};

// ─── Certificates ─────────────────────────────────────────────────────────────
const char* rootCA = R"EOF(
-----BEGIN CERTIFICATE-----

-----END CERTIFICATE-----
)EOF";

const char* deviceCert = R"EOF(
-----BEGIN CERTIFICATE-----

-----END CERTIFICATE-----
)EOF";

const char* privateKey = R"EOF(
-----BEGIN RSA PRIVATE KEY-----

-----END RSA PRIVATE KEY-----
)EOF";

// ─── LED Pins (ESP32-S3 safe GPIOs) ──────────────────────────────────────────
// Avoid GPIO 0 (boot), 3 (JTAG), 45/46 (strapping).
// GPIO 15/16/17 are free on S3-DevKitC-1 when PSRAM is octal (uses 35-40).
#define LED_RED    15
#define LED_GREEN  16
#define LED_YELLOW 17

// ─── Image Buffer ─────────────────────────────────────────────────────────────
// ESP32-S3 has PSRAM — raise cap to 200 KB.
// Requires: Tools → PSRAM → "OPI PSRAM" in Arduino IDE.
#define MAX_IMAGE_SIZE (200 * 1024)

uint8_t* imageBuffer = nullptr;
size_t   imageSize   = 0;

// ─── S3 Retry ─────────────────────────────────────────────────────────────────
int s3RetryCount = 0;
#define MAX_S3_RETRIES 3

// ─── Pending upload flag ──────────────────────────────────────────────────────
bool     pendingUpload = false;
uint8_t* pendingBuffer = nullptr;
size_t   pendingSize   = 0;

// ─── State Machine ────────────────────────────────────────────────────────────
enum SystemState {
  STATE_IDLE,
  STATE_RECEIVING,
  STATE_UPLOADING,
  STATE_WAITING_RESULT,
  STATE_SHOW_RESULT
};

SystemState   currentState = STATE_IDLE;
unsigned long resultTimer  = 0;

// ─── Networking Objects ───────────────────────────────────────────────────────
WebServer        server(80);
WiFiClientSecure mqttTlsClient;   // ONLY used for MQTT — never reused for HTTP
PubSubClient     mqttClient(mqttTlsClient);

// ─── LED Helpers ──────────────────────────────────────────────────────────────
void ledsOff() {
  digitalWrite(LED_RED,    LOW);
  digitalWrite(LED_GREEN,  LOW);
  digitalWrite(LED_YELLOW, LOW);
}
void setYellow() { ledsOff(); digitalWrite(LED_YELLOW, HIGH); Serial.println("[LED] YELLOW — Processing..."); }
void setGreen()  { ledsOff(); digitalWrite(LED_GREEN,  HIGH); Serial.println("[LED] GREEN  — GOOD!"); }
void setRed()    { ledsOff(); digitalWrite(LED_RED,    HIGH); Serial.println("[LED] RED    — DEFECT!"); }

// ─── MQTT Callback ────────────────────────────────────────────────────────────
void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];

  Serial.println("\n========================================");
  Serial.println("[IoT] Payload: " + message);

  // Lambda now includes unix_ms for Timestream; we log it here.
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, message)) { Serial.println("[IoT] JSON parse error!"); return; }

  const char* prediction = doc["prediction"] | "unknown";
  float       confidence = doc["confidence"] | 0.0f;
  const char* image      = doc["image"]      | "unknown";
  long long   unix_ms    = doc["unix_ms"]    | 0LL;   // ← new field from Lambda

  Serial.printf("  Image      : %s\n",  image);
  Serial.printf("  Prediction : %s\n",  prediction);
  Serial.printf("  Confidence : %.2f%%\n", confidence * 100);
  Serial.printf("  Timestamp  : %lld ms\n", unix_ms);
  Serial.println("========================================");

  if (strcmp(prediction, "DEFECT") == 0) {
    setRed();
    // PLC integration: digitalWrite(PLC_REJECT_PIN, HIGH); // signal PLC to divert part to reject bin
  } else {
    setGreen();
    // PLC integration: digitalWrite(PLC_ACCEPT_PIN, HIGH); // signal PLC to allow part through
  }

  resultTimer  = millis();
  currentState = STATE_SHOW_RESULT;
}

// ─── /status Endpoint ─────────────────────────────────────────────────────────
void handleStatus() {
  server.send(200, "text/plain",
    currentState == STATE_IDLE ? "READY" : "BUSY");
}

// ─── Connect MQTT ─────────────────────────────────────────────────────────────
void connectMQTT() {
  mqttTlsClient.setCACert(rootCA);
  mqttTlsClient.setCertificate(deviceCert);
  mqttTlsClient.setPrivateKey(privateKey);
  mqttTlsClient.setAlpnProtocols(alpnProtos);

  mqttClient.setServer(iotEndpoint, 443);
  mqttClient.setCallback(onMqttMessage);
  mqttClient.setKeepAlive(60);
  mqttClient.setSocketTimeout(30);
  mqttClient.setBufferSize(2048);

  while (!mqttClient.connected()) {
    Serial.print("[MQTT] Connecting...");
    if (mqttClient.connect(clientId)) {
      Serial.println(" ✅ Connected!");
      mqttClient.subscribe(mqttTopic);
      Serial.printf("[MQTT] ✅ Subscribed to: %s\n", mqttTopic);
    } else {
      Serial.printf(" ❌ state=%d — retrying in 5s\n", mqttClient.state());
      // Re-apply certs after failed attempt (ESP-IDF TLS sometimes resets them)
      mqttTlsClient.setCACert(rootCA);
      mqttTlsClient.setCertificate(deviceCert);
      mqttTlsClient.setPrivateKey(privateKey);
      mqttTlsClient.setAlpnProtocols(alpnProtos);
      delay(5000);
    }
  }
}

// ─── Upload to S3 (scoped TLS — never touches mqttTlsClient) ─────────────────
bool uploadToS3(uint8_t* data, size_t length) {
  Serial.println("\n[S3] Starting upload...");

  WiFiClientSecure s3Tls;
  s3Tls.setInsecure();   // Pre-signed URL carries request-level auth; TLS only for transport
  s3Tls.setTimeout(20);

  HTTPClient http;
  http.begin(s3Tls, s3PresignedUrl);
  http.addHeader("Content-Type", "image/jpeg");
  http.setTimeout(20000);

  int code = http.PUT(data, length);
  String body = (code < 0) ? http.errorToString(code) : http.getString();
  http.end();

  if (code == 200 || code == 204) {
    Serial.printf("[S3] ✅ Upload OK, HTTP %d\n", code);
    return true;
  }
  Serial.printf("[S3] ❌ Failed, HTTP %d — %s\n", code, body.c_str());
  return false;
}

// ─── Handle Incoming Image Upload ─────────────────────────────────────────────
void handleUpload() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    if (currentState != STATE_IDLE) {
      Serial.println("[ESP32-S3] ⚠️ Busy — rejecting upload");
      return;
    }
    imageSize = 0;
    if (imageBuffer) { free(imageBuffer); imageBuffer = nullptr; }

    // Use ps_malloc() to allocate from PSRAM
    imageBuffer = (uint8_t*)ps_malloc(MAX_IMAGE_SIZE);
    if (!imageBuffer) {
      Serial.printf("[ESP32-S3] ❌ ps_malloc failed! PSRAM free: %d B\n",
                    ESP.getFreePsram());
      return;
    }
    currentState = STATE_RECEIVING;
    setYellow();
    Serial.printf("[ESP32-S3] Receiving: %s\n", upload.filename.c_str());
  }

  else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!imageBuffer) return;
    if (imageSize + upload.currentSize <= MAX_IMAGE_SIZE) {
      memcpy(imageBuffer + imageSize, upload.buf, upload.currentSize);
      imageSize += upload.currentSize;
    } else {
      Serial.println("[ESP32-S3] ❌ Image exceeds MAX_IMAGE_SIZE!");
    }
  }

  else if (upload.status == UPLOAD_FILE_END) {
    Serial.printf("[ESP32-S3] ✅ Received: %u bytes (heap free: %u, PSRAM free: %u)\n",
                  upload.totalSize, ESP.getFreeHeap(), ESP.getFreePsram());

    // Hand off to loop() — avoids TLS conflict with MQTT
    pendingBuffer = imageBuffer;
    pendingSize   = imageSize;
    pendingUpload = true;
    imageBuffer   = nullptr;
    imageSize     = 0;
    currentState  = STATE_UPLOADING;
  }
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(2000);

  // ── PSRAM check ──
  if (!psramFound()) {
    Serial.println("⚠️  PSRAM NOT found! Image buffer will be tiny.");
    Serial.println("    → In Arduino IDE: Tools → PSRAM → OPI PSRAM");
  } else {
    Serial.printf("[PSRAM] ✅ Found — %u KB free\n", ESP.getFreePsram() / 1024);
  }

  pinMode(LED_RED,    OUTPUT);
  pinMode(LED_GREEN,  OUTPUT);
  pinMode(LED_YELLOW, OUTPUT);
  ledsOff();

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("[WiFi] Connecting");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\n[WiFi] ✅ IP: " + WiFi.localIP().toString());
  delay(2000);

  connectMQTT();

  server.on("/upload", HTTP_POST,
    []() {
      server.sendHeader("Connection", "close");
      server.send(200, "text/plain", "OK");
    },
    handleUpload
  );
  server.on("/status", HTTP_GET, handleStatus);
  server.begin();

  Serial.println("\n========================================");
  Serial.println("✅ System Ready!");
  Serial.printf("   Upload : http://%s/upload\n",  WiFi.localIP().toString().c_str());
  Serial.printf("   Status : http://%s/status\n",  WiFi.localIP().toString().c_str());
  Serial.println("   Topic  : inspection/result");
  Serial.println("========================================\n");
}

// ─── Loop ─────────────────────────────────────────────────────────────────────
void loop() {

  // ── MQTT keepalive ────────────────────────────────────────────────────────
  if (!mqttClient.connected()) {
    Serial.println("[MQTT] Reconnecting...");
    connectMQTT();
  }
  mqttClient.loop();
  server.handleClient();

  // ── Pending S3 upload (runs in loop, not in HTTP handler) ─────────────────
  if (pendingUpload) {
    pendingUpload  = false;
    bool ok        = false;
    s3RetryCount   = 0;

    while (!ok && s3RetryCount < MAX_S3_RETRIES) {
      if (s3RetryCount > 0) {
        Serial.printf("[S3] Retry %d/%d...\n", s3RetryCount, MAX_S3_RETRIES);
        delay(2000);
        mqttClient.loop();  // keep MQTT alive between retries
      }
      ok = uploadToS3(pendingBuffer, pendingSize);
      s3RetryCount++;
    }

    free(pendingBuffer);
    pendingBuffer = nullptr;
    pendingSize   = 0;

    if (ok) {
      currentState = STATE_WAITING_RESULT;
      Serial.println("[LOOP] ✅ Upload done — Waiting for MQTT result...");
    } else {
      Serial.println("[LOOP] ❌ S3 upload failed after all retries — going IDLE");
      ledsOff();
      currentState = STATE_IDLE;
    }
  }

  // ── Result display timer (5-second LED hold) ───────────────────────────────
  if (currentState == STATE_SHOW_RESULT) {
    if (millis() - resultTimer >= 5000) {
      ledsOff();
      // PLC integration: digitalWrite(PLC_REJECT_PIN, LOW); // de-assert sort signal before resuming belt
      // PLC integration: digitalWrite(PLC_ACCEPT_PIN, LOW);
      // PLC integration: digitalWrite(PLC_RESUME_PIN, HIGH); // signal PLC to resume conveyor
      currentState = STATE_IDLE;
      Serial.println("[STATE] ✅ Done — Ready for next image!");
      Serial.println("========================================");
    }
  }
}
