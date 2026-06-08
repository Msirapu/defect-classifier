/*
 * =====================================================================
 * Visual Defect Classifier — ESP32-S3 Firmware (API Gateway Mode)
 * =====================================================================
 *
 * This variant replaces the S3 presigned URL with an AWS API Gateway
 * endpoint.  The gateway proxies the raw JPEG body into a Lambda
 * function which then puts the image into S3 and triggers inference.
 *
 * CHANGES vs. presigned-URL variant
 * -----------------------------------
 *  • s3PresignedUrl replaced by apiGatewayUrl
 *  • uploadToS3() replaced by uploadViaApiGateway() — uses HTTP POST
 *    with Content-Type: image/jpeg directly to APIGW
 *  • No PubSubClient changes; MQTT receive path is identical
 *  • All ESP32-S3 hardware changes (PSRAM, GPIO) carried forward
 *
 * API Gateway setup (brief)
 * -------------------------
 *  1. Create REST API in API Gateway.
 *  2. POST /upload resource → Lambda integration (proxy).
 *  3. Binary media types: image/jpeg
 *  4. Deploy to a stage (e.g. "prod").
 *  5. Paste the invoke URL below.
 * =====================================================================
 */

#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// ─── WiFi ─────────────────────────────────────────────────────────────────────
const char* ssid     = "";
const char* password = "";

// ─── API Gateway ──────────────────────────────────────────────────────────────
// Example: https://abc123.execute-api.us-east-2.amazonaws.com/prod/upload
const char* apiGatewayUrl = "";

// ─── AWS IoT Core ─────────────────────────────────────────────────────────────
const char* iotEndpoint = "a11261zvniicx8-ats.iot.us-east-2.amazonaws.com";
const char* mqttTopic   = "inspection/result";
const char* clientId    = "ESP32S3-Defect-Classifier-APIGW";
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
#define LED_RED    15
#define LED_GREEN  16
#define LED_YELLOW 17

// ─── Image Buffer (PSRAM) ─────────────────────────────────────────────────────
#define MAX_IMAGE_SIZE (200 * 1024)
uint8_t* imageBuffer = nullptr;
size_t   imageSize   = 0;

int  s3RetryCount   = 0;
#define MAX_RETRIES 3

bool     pendingUpload = false;
uint8_t* pendingBuffer = nullptr;
size_t   pendingSize   = 0;

enum SystemState {
  STATE_IDLE, STATE_RECEIVING, STATE_UPLOADING,
  STATE_WAITING_RESULT, STATE_SHOW_RESULT
};
SystemState   currentState = STATE_IDLE;
unsigned long resultTimer  = 0;

WebServer        server(80);
WiFiClientSecure mqttTlsClient;
PubSubClient     mqttClient(mqttTlsClient);

void ledsOff() {
  digitalWrite(LED_RED, LOW); digitalWrite(LED_GREEN, LOW); digitalWrite(LED_YELLOW, LOW);
}
void setYellow() { ledsOff(); digitalWrite(LED_YELLOW, HIGH); Serial.println("[LED] YELLOW — Processing..."); }
void setGreen()  { ledsOff(); digitalWrite(LED_GREEN,  HIGH); Serial.println("[LED] GREEN  — GOOD!"); }
void setRed()    { ledsOff(); digitalWrite(LED_RED,    HIGH); Serial.println("[LED] RED    — DEFECT!"); }

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];

  Serial.println("\n========================================");
  Serial.println("[IoT] Payload: " + message);

  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, message)) { Serial.println("[IoT] JSON error!"); return; }

  const char* prediction = doc["prediction"] | "unknown";
  float       confidence = doc["confidence"] | 0.0f;
  const char* image      = doc["image"]      | "unknown";
  long long   unix_ms    = doc["unix_ms"]    | 0LL;

  Serial.printf("  Image      : %s\n", image);
  Serial.printf("  Prediction : %s\n", prediction);
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

void handleStatus() {
  server.send(200, "text/plain", currentState == STATE_IDLE ? "READY" : "BUSY");
}

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
    } else {
      Serial.printf(" ❌ state=%d — retry in 5s\n", mqttClient.state());
      mqttTlsClient.setCACert(rootCA);
      mqttTlsClient.setCertificate(deviceCert);
      mqttTlsClient.setPrivateKey(privateKey);
      mqttTlsClient.setAlpnProtocols(alpnProtos);
      delay(5000);
    }
  }
}

// ─── Upload via API Gateway ───────────────────────────────────────────────────
bool uploadViaApiGateway(uint8_t* data, size_t length) {
  Serial.println("\n[APIGW] Starting upload...");

  WiFiClientSecure apigwTls;
  apigwTls.setInsecure();
  apigwTls.setTimeout(20);

  HTTPClient http;
  http.begin(apigwTls, apiGatewayUrl);
  http.addHeader("Content-Type", "image/jpeg");
  http.setTimeout(20000);

  int code = http.POST(data, length);
  String body = (code < 0) ? http.errorToString(code) : http.getString();
  http.end();

  if (code == 200 || code == 202) {
    Serial.printf("[APIGW] ✅ Success, HTTP %d\n", code);
    return true;
  }
  Serial.printf("[APIGW] ❌ Failed, HTTP %d — %s\n", code, body.c_str());
  return false;
}

void handleUpload() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    if (currentState != STATE_IDLE) { Serial.println("[ESP32-S3] Busy — reject"); return; }
    imageSize = 0;
    if (imageBuffer) { free(imageBuffer); imageBuffer = nullptr; }
    imageBuffer = (uint8_t*)ps_malloc(MAX_IMAGE_SIZE);
    if (!imageBuffer) { Serial.println("[ESP32-S3] ❌ ps_malloc failed!"); return; }
    currentState = STATE_RECEIVING;
    setYellow();
    Serial.printf("[ESP32-S3] Receiving: %s\n", upload.filename.c_str());
  }
  else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!imageBuffer) return;
    if (imageSize + upload.currentSize <= MAX_IMAGE_SIZE) {
      memcpy(imageBuffer + imageSize, upload.buf, upload.currentSize);
      imageSize += upload.currentSize;
    }
  }
  else if (upload.status == UPLOAD_FILE_END) {
    Serial.printf("[ESP32-S3] ✅ Received: %u bytes\n", upload.totalSize);
    pendingBuffer = imageBuffer;
    pendingSize   = imageSize;
    pendingUpload = true;
    imageBuffer   = nullptr;
    imageSize     = 0;
    currentState  = STATE_UPLOADING;
  }
}

void setup() {
  Serial.begin(115200);
  delay(2000);

  if (!psramFound()) {
    Serial.println("⚠️  PSRAM NOT found! Go to Tools → PSRAM → OPI PSRAM");
  } else {
    Serial.printf("[PSRAM] ✅ %u KB free\n", ESP.getFreePsram() / 1024);
  }

  pinMode(LED_RED, OUTPUT); pinMode(LED_GREEN, OUTPUT); pinMode(LED_YELLOW, OUTPUT);
  ledsOff();

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("[WiFi] Connecting");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\n[WiFi] ✅ " + WiFi.localIP().toString());
  delay(2000);

  connectMQTT();

  server.on("/upload", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", "OK");
  }, handleUpload);
  server.on("/status", HTTP_GET, handleStatus);
  server.begin();

  Serial.println("\n========================================");
  Serial.println("✅ System Ready! (API Gateway Mode)");
  Serial.printf("   Upload : http://%s/upload\n", WiFi.localIP().toString().c_str());
  Serial.printf("   Status : http://%s/status\n", WiFi.localIP().toString().c_str());
  Serial.println("========================================\n");
}

void loop() {
  if (!mqttClient.connected()) connectMQTT();
  mqttClient.loop();
  server.handleClient();

  if (pendingUpload) {
    pendingUpload = false;
    bool ok       = false;
    s3RetryCount  = 0;

    while (!ok && s3RetryCount < MAX_RETRIES) {
      if (s3RetryCount > 0) { delay(2000); mqttClient.loop(); }
      ok = uploadViaApiGateway(pendingBuffer, pendingSize);
      s3RetryCount++;
    }

    free(pendingBuffer); pendingBuffer = nullptr; pendingSize = 0;

    if (ok) {
      currentState = STATE_WAITING_RESULT;
      Serial.println("[LOOP] ✅ Upload done — Waiting for MQTT result...");
    } else {
      Serial.println("[LOOP] ❌ Upload failed — going IDLE");
      ledsOff(); currentState = STATE_IDLE;
    }
  }

  if (currentState == STATE_SHOW_RESULT) {
    if (millis() - resultTimer >= 5000) {
      ledsOff();
      // PLC integration: digitalWrite(PLC_REJECT_PIN, LOW); // de-assert sort signal before resuming belt
      // PLC integration: digitalWrite(PLC_ACCEPT_PIN, LOW);
      // PLC integration: digitalWrite(PLC_RESUME_PIN, HIGH); // signal PLC to resume conveyor
      currentState = STATE_IDLE;
      Serial.println("[STATE] ✅ Done — Ready for next image!");
    }
  }
}
