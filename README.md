# Visual Defect Classifier — Edge-to-Cloud Inspection System

An end-to-end automated visual inspection system that uses a deep learning model (EfficientNet-B0, ONNX) deployed on AWS to classify manufactured parts as **GOOD** or **DEFECT** in real time.

The system was originally built for inspecting **motorcycle side covers** on a production conveyor. The architecture is intentionally generic and applies equally to any discrete-part conveyor inspection use case — see [Scalability](#scalability--use-cases) below.

---

## Table of contents

- [System overview](#system-overview)
- [Process flow](#process-flow)
- [Hardware](#hardware)
- [Firmware](#firmware)
- [AWS architecture](#aws-architecture)
- [Deep learning model](#deep-learning-model)
- [Dashboard](#dashboard)
- [Scalability / use cases](#scalability--use-cases)
- [Repository structure](#repository-structure)
- [Setup guide](#setup-guide)
- [FAQ](#faq)

---

## System overview

```
Part on conveyor  →  Sensor triggers PLC  →  PLC stops belt + signals ESP32-S3
→  Camera captures JPEG  →  ESP32-S3 uploads to S3  →  Lambda runs ONNX inference
→  Result stored in Timestream  →  Grafana dashboard  →  IoT Core MQTT publish
→  ESP32-S3 receives result  →  LED indicator + serial log  →  PLC sorts part
→  Conveyor resumes
```

The entire cycle — from shutter click to LED indication — completes in **under 3 seconds** on a stable WiFi link and a warm Lambda container.

---

## Process flow

> The diagram below shows the full pipeline for the **industry deployment**. In the GitHub showcase version the PLC and camera are replaced by a PC Python script that pushes a JPEG to the ESP32-S3 over HTTP.

```
Physical layer
──────────────────────────────────────────────────────────────────────────────
① Conveyor belt      →  ② Distance sensor    →  ③ PLC
   (part moving)          (detects part in FOV)   (stops belt, signals ESP32-S3)
                                ↑ stop signal ←────────────────────┘
④ Ring light         →  ⑤ Camera OV5640       →  ⑥ ESP32-S3
   (diffuse illum.)       (perpendicular mount)    (captures image)
                                                         │ presigned URL / API GW
AWS cloud layer                                          ↓
──────────────────────────────────────────────────────────────────────────────
⑦ AWS S3             →  ⑧ Lambda + ECS        →  ⑨ Timestream
   (image bucket)         (ONNX inference)          (time-series store)
                               │                          │
                               ↓                          ↓
                          ⑪ IoT Core              ⑩ Managed Grafana
                            (MQTT publish)            (live dashboard)
Result layer                   │
──────────────────────────────────────────────────────────────────────────────
                               ↓
⑫ ESP32-S3  →  ⑬ LED indicator  ⑭ Serial monitor  →  ⑮ PLC sorter
   (MQTT sub)    (R / G / Y)        (pred + conf.)       (accept / reject)
                                                               │
                          ←───────────────────────────────────┘
                               resume conveyor
```

### Lighting choice — ring light vs dome light

| | Ring light | Dome light |
|---|---|---|
| Illumination | Directional, creates shadows | Diffuse, shadowless |
| Cost | Lower (~$20–$80) | Higher (~$100–$400) |
| Best for | Surface texture, edges | Flat surfaces, glossy parts |
| **Chosen** | **✅ Ring light** | — |

A ring light was chosen for motorcycle side covers because edge and surface texture defects (cracks, sink marks, short shots) are more visible under directional light, and the cost is significantly lower. For highly polished or transparent parts (e.g. lenses, PCBs) a dome light is preferred.

---

## Hardware

| Component | Part | Notes |
|---|---|---|
| Microcontroller | ESP32-S3-DevKitC-1 | PSRAM required — see firmware notes |
| Camera | OV5640 (2 MP, 5 MP variant) | Mounted perpendicular to conveyor |
| Lighting | Ring light (LED, 5600K) | Mounted co-axial with camera |
| Indicators | 3× LEDs (red, green, yellow) | GPIO 15, 16, 17 on ESP32-S3 |
| Sensor | Ultrasonic / diffuse photoelectric | Managed by PLC (not by ESP32-S3) |
| PLC | Any brand (Siemens, Allen-Bradley, etc.) | Automation engineer's responsibility |

---

## Firmware

Two firmware variants are provided:

### `firmware/main_esp32s3.ino` — Presigned URL mode (recommended for simplicity)

- ESP32-S3 receives a JPEG from the camera (or PC script).
- Uploads directly to S3 using an AWS presigned PUT URL.
- Listens on MQTT (`inspection/result`) for the Lambda result.
- Lights correct LED and logs to serial monitor.

### `firmware/main_esp32s3_apigw.ino` — API Gateway mode

- Same as above, but image is POSTed to an API Gateway endpoint instead of S3.
- The Lambda function behind the gateway handles S3 storage.
- Useful when you want request-level auth (IAM / API key) on the upload path.

### Arduino IDE board settings

```
Board       : ESP32S3 Dev Module
PSRAM       : OPI PSRAM        ← REQUIRED
Flash mode  : QIO 80MHz
Upload speed: 921600
```

### LED mapping (ESP32-S3 safe GPIOs)

| LED | GPIO | Meaning |
|---|---|---|
| Yellow | 17 | Processing (image received, uploading) |
| Green | 16 | GOOD — part accepted |
| Red | 15 | DEFECT — part rejected |

---

## AWS architecture

```
┌─────────────────────────────────────────────────────────┐
│  AWS account                                            │
│                                                         │
│  S3 bucket ──(ObjectCreated)──► Lambda function         │
│                                      │                  │
│                                      ├──► Timestream    │
│                                      │    (write)       │
│                                      │                  │
│                                      └──► IoT Core      │
│                                           (MQTT pub)    │
│                                                         │
│  ECR ──(container image)──► Lambda (ECS container)      │
│                                                         │
│  Managed Grafana ──(Timestream datasource)──► Dashboard │
└─────────────────────────────────────────────────────────┘
```

### Services used

| Service | Purpose |
|---|---|
| **S3** | Image storage; triggers Lambda via S3 event notification |
| **ECR** | Hosts the Docker image containing the ONNX model |
| **Lambda** | Serverless inference entrypoint (runs inside the ECS container image) |
| **Timestream** | Time-series database for all inspection records |
| **IoT Core** | MQTT broker; delivers result to ESP32-S3 over TLS |
| **Managed Grafana** | Real-time dashboard connected to Timestream |
| **API Gateway** | (Optional) REST endpoint for image ingestion with IAM/API-key auth |

### Why Timestream instead of DynamoDB?

Inspection data is inherently time-series: every record has a timestamp, and the most useful queries are temporal (defect rate over the last hour, throughput per shift, trend detection). Timestream is purpose-built for this and gives you:

- Automatic data tiering (hot memory store → cold magnetic store).
- Native SQL queries over time windows (`ago(1h)`, `BIN(time, 1m)`).
- First-class integration with Managed Grafana — no Lambda adapter needed.
- Lower cost than DynamoDB for append-only time-series workloads.

### Why Managed Grafana instead of QuickSight?

| | Managed Grafana | QuickSight |
|---|---|---|
| Timestream support | Native datasource | Available via Athena connector |
| Refresh rate | As low as 5 s (live panels) | Minimum ~1 min (SPICE) |
| Alert routing | Built-in (PagerDuty, Slack, email) | Limited |
| Cost | Per-user/month | Per-user/month + SPICE capacity |
| **Best for** | **Live conveyor monitoring** | BI / executive reporting |

Grafana is the better fit for a factory floor dashboard where operators need to see the current state of the line in near-real-time.

---

## Deep learning model

| Attribute | Value |
|---|---|
| Backbone | EfficientNet-B0 (`tf_efficientnet_b0.ns_jft_in1k` via `timm`) |
| Input | 224 × 224 RGB (converted from grayscale) |
| Output | Single logit (binary classification) |
| Loss | Focal Loss (γ = 2) — handles class imbalance |
| Threshold | 0.4 (tuned on validation set, see notebook) |
| Export | ONNX opset 13, IR version 8 (for Lambda compatibility) |
| Training | 10 epochs, AdamW lr=1e-4, batch size 32 |
| Augmentation | HFlip, VFlip, Rotate, BrightnessContrast, GaussNoise, CoarseDropout |

The notebook (`DefectClassifier3.ipynb`) covers:

1. Data loading and 50/50 train/test split (stratified by class)
2. Augmentation pipeline (Albumentations)
3. EfficientNet-B0 fine-tuning with Focal Loss
4. Threshold search on validation F1
5. ONNX export → merge external weights → downgrade IR version
6. End-to-end ONNX Runtime inference test

**Preprocessing pipeline** (must match exactly between training and Lambda):

```python
img = cv2.imdecode(...)              # decode JPEG
img = cv2.cvtColor(img, COLOR_GRAY2RGB)
img = cv2.resize(img, (224, 224))
img = img.astype(np.float32) / 255.0
img = (img - [0.485,0.456,0.406]) / [0.229,0.224,0.225]   # ImageNet normalisation
img = np.transpose(img, (2,0,1))     # HWC → CHW
img = np.expand_dims(img, 0)         # batch dim
```

---

## Dashboard

Connect Amazon Managed Grafana to Timestream using the built-in Timestream datasource (no plugin install required).

**Example Timestream query — defect rate per minute:**

```sql
SELECT BIN(time, 1m)                                          AS time,
       COUNT(CASE WHEN result = 'DEFECT' THEN 1 END) * 100.0
           / COUNT(*)                                         AS defect_rate_pct,
       COUNT(*)                                               AS total_inspected
FROM "InspectionDB"."InspectionResults"
WHERE time > ago(1h)
GROUP BY BIN(time, 1m)
ORDER BY time ASC
```

Recommended panels:

- **Time-series** — defect rate % over the shift
- **Stat** — total inspected today
- **Gauge** — current hour defect rate
- **Bar chart** — GOOD vs DEFECT count by hour
- **Table** — last 50 inspection records with confidence scores

Set the dashboard auto-refresh to **5 s** for near-real-time conveyor monitoring.

---

## Scalability / use cases

This architecture is **product-agnostic**. Swap the training dataset and the same pipeline applies to:

| Industry | Part | Defect types |
|---|---|---|
| Automotive | Engine castings, brake discs | Porosity, cracks, surface pits |
| Electronics | PCBs, connectors | Solder bridges, missing components, burnt traces |
| Plastics | Injection-moulded housings | Sink marks, short shots, warping |
| Metal forming | Stamped sheet metal | Burrs, dents, incorrect punching |
| Packaging | Bottles, cans | Label misalignment, fill level, cap defects |
| Food processing | Baked goods, produce | Colour deviation, shape anomaly, contamination |
| Pharmaceuticals | Tablet blister packs | Missing tablets, seal defects |
| Textiles | Fabric rolls | Weave defects, colour spots, holes |

**To adapt for a new product:**

1. Collect images (≥ 200 per class recommended).
2. Retrain using the provided notebook — only `ok_path` / `def_path` need changing.
3. Export the new model as ONNX and replace `model11.onnx` in the container.
4. Rebuild and push the Docker image to ECR.
5. Update the Lambda function to use the new image. No firmware changes required.

**Multi-line scaling:** Deploy one S3 presigned URL (or API Gateway resource) and one ESP32-S3 per conveyor line. All lines share the same Lambda, Timestream table, and Grafana dashboard — add a `line_id` dimension to Timestream records to filter per line.

---

## Repository structure

```
defect-classifier/
├── firmware/
│   ├── main_esp32s3.ino           # Presigned URL mode (recommended)
│   └── main_esp32s3_apigw.ino     # API Gateway mode
├── lambda/
│   ├── app.py                     # Lambda handler (Timestream + IoT Core)
│   ├── Dockerfile                 # Container image for ECR / ECS
│   └── requirements.txt
├── notebooks/
│   └── DefectClassifier3.ipynb    # Training, evaluation, ONNX export
└── README.md
```

---

## Setup guide

### 1. AWS prerequisites

- Create an S3 bucket (e.g. `defect-inspection-images`).
- Create a Timestream database `InspectionDB` and table `InspectionResults`.
  - Memory store retention: 1 day (adjust for your needs).
  - Magnetic store retention: 1 year.
- Register an IoT Core Thing, download the certificate + private key.
- Create a Lambda execution role with permissions:
  - `s3:GetObject` on your bucket.
  - `timestream:WriteRecords` on your table.
  - `iot:Publish` on `inspection/result`.

### 2. Build and push the container

```bash
# Build
docker build -t defect-classifier ./lambda

# Tag and push to ECR
aws ecr create-repository --repository-name defect-classifier
docker tag defect-classifier:latest <account>.dkr.ecr.<region>.amazonaws.com/defect-classifier:latest
aws ecr get-login-password | docker login --username AWS --password-stdin <account>.dkr.ecr.<region>.amazonaws.com
docker push <account>.dkr.ecr.<region>.amazonaws.com/defect-classifier:latest
```

### 3. Create the Lambda function

- Runtime: container image (point to the ECR URI above).
- Trigger: S3 ObjectCreated event on your bucket.
- Memory: 1024 MB (model is ~20 MB; keep warm Lambda loaded).
- Timeout: 30 s.

### 4. Generate a presigned URL (for presigned URL firmware)

```bash
aws s3 presign s3://defect-inspection-images/latest.jpg --expires-in 86400
```

Paste the URL into `s3PresignedUrl` in `main_esp32s3.ino`.

> **Note:** Presigned URLs expire. For production use, have a separate Lambda or backend generate a fresh URL on each inspection cycle and push it to the ESP32-S3 via IoT Core.

### 5. Flash the firmware

1. Open `main_esp32s3.ino` in Arduino IDE 2.x.
2. Fill in `ssid`, `password`, `s3PresignedUrl`, `iotEndpoint`, and the three certificate strings.
3. Set board to `ESP32S3 Dev Module`, PSRAM to `OPI PSRAM`.
4. Flash and open Serial Monitor at 115200 baud.

### 6. Test

Send a JPEG to the ESP32-S3 from a PC:

```bash
curl -X POST http://<ESP32_IP>/upload \
  -F "file=@test_image.jpg" \
  -H "Content-Type: multipart/form-data"
```

Expected serial output:

```
[LED] YELLOW — Processing...
[S3] ✅ Upload OK, HTTP 200
[IoT] Payload: {"image":"latest.jpg","prediction":"GOOD","confidence":0.9213,...}
[LED] GREEN  — GOOD!
[STATE] ✅ Done — Ready for next image!
```
---

## FAQ

**Q: Why two TLS clients (mqttTlsClient and a scoped s3Tls inside uploadToS3)?**

A: The PubSubClient holds a long-lived TLS session to IoT Core. Starting a second TLS handshake on the *same* `WiFiClientSecure` object mid-session corrupts the MQTT state. Using a locally scoped `WiFiClientSecure` for the S3 upload keeps the two sessions completely isolated.

**Q: Why does the upload happen in `loop()` rather than in `handleUpload()`?**

A: The HTTP server callback runs inside the TCP stack. Starting a new TLS connection from inside that callback can deadlock the WiFi driver on ESP32. The `pendingUpload` flag hands the work off to `loop()` where it runs safely.

**Q: Can I use the OV5640 directly without a PC in the middle?**

A: Yes. The camera connects to the ESP32-S3 via SCCB (I2C) + DVP or MIPI-CSI. Use the `esp32-camera` Arduino library. The firmware already handles the upload; you just replace the `handleUpload()` call with your camera capture code. This is the full industry setup.

**Q: What is the inference threshold (0.4) and why not 0.5?**

A: The threshold was chosen by maximising F1 score on the validation set. Because the cost of missing a defect (false negative) is higher than incorrectly rejecting a good part (false positive), the threshold was shifted below 0.5 to increase recall on the DEFECT class.

**Q: How do I retrain for a different product?**

A: See [Scalability / use cases](#scalability--use-cases). The short answer: update `ok_path` and `def_path` in the notebook, retrain, export, rebuild the Docker image.
