"""
Lambda handler — Defect Classifier
====================================
Trigger  : S3 ObjectCreated event (PUT from ESP32-S3 or API Gateway)
Runtime  : Python 3.11 (container image via ECS/ECR)

Storage  : AWS Timestream (replaces DynamoDB)
           Database : InspectionDB
           Table    : InspectionResults

Dashboard: Amazon Managed Grafana (connects to Timestream via the
           built-in Timestream data source — no extra connector needed).
           Alternative: Amazon QuickSight also supports Timestream via
           the Timestream connector, but Grafana gives real-time panels
           (auto-refresh ≥ 5 s) which is ideal for a live conveyor line.

IoT Core : Publishes to "inspection/result" topic.
           Payload now includes unix_ms so the ESP32-S3 can log it.
"""

import json
import time
import boto3
import numpy as np
import cv2
import onnxruntime as ort
from datetime import datetime, timezone

# ─── AWS Clients ──────────────────────────────────────────────────────────────
s3 = boto3.client("s3")
iot = boto3.client("iot-data", region_name="us-east-2")

# Timestream write client — use the write endpoint
timestream = boto3.client("timestream-write", region_name="us-east-2")

TIMESTREAM_DB    = "InspectionDB"
TIMESTREAM_TABLE = "InspectionResults"

# ─── ONNX Model ───────────────────────────────────────────────────────────────
session     = ort.InferenceSession("model11.onnx")
input_name  = session.get_inputs()[0].name
output_name = session.get_outputs()[0].name

# ─── Preprocessing (identical to training pipeline) ───────────────────────────
def preprocess(image_bytes: bytes) -> np.ndarray:
    nparr = np.frombuffer(image_bytes, np.uint8)
    img   = cv2.imdecode(nparr, cv2.IMREAD_GRAYSCALE)
    img   = cv2.cvtColor(img, cv2.COLOR_GRAY2RGB)
    img   = cv2.resize(img, (224, 224))
    img   = img.astype(np.float32) / 255.0

    mean = np.array([0.485, 0.456, 0.406], dtype=np.float32)
    std  = np.array([0.229, 0.224, 0.225], dtype=np.float32)
    img  = (img - mean) / std

    img  = np.transpose(img, (2, 0, 1))      # HWC → CHW
    img  = np.expand_dims(img, axis=0)        # add batch dim
    return img

# ─── Write to Timestream ──────────────────────────────────────────────────────
def write_to_timestream(image_key: str, prediction: str, confidence: float, unix_ms: int) -> None:
    """
    Timestream record layout
    ------------------------
    Dimensions (metadata / low-cardinality):
        image_id  — S3 object key (acts as the primary identifier)
        result    — "GOOD" or "DEFECT"

    Measure name  : confidence
    Measure value : float (0.0–1.0)
    Time          : unix_ms (millisecond precision)

    You can add more measures (e.g. inference_latency_ms) by extending
    the Records list below.

    Timestream → Grafana tip
    -------------------------
    In Grafana, use the Timestream data source and this query to build
    a live defect-rate panel:

        SELECT BIN(time, 1m) AS binned_time,
               COUNT(CASE WHEN result = 'DEFECT' THEN 1 END) AS defects,
               COUNT(*) AS total
        FROM "InspectionDB"."InspectionResults"
        WHERE time > ago(1h)
        GROUP BY BIN(time, 1m)
        ORDER BY binned_time ASC
    """
    timestream.write_records(
        DatabaseName=TIMESTREAM_DB,
        TableName=TIMESTREAM_TABLE,
        Records=[
            {
                "Dimensions": [
                    {"Name": "image_id", "Value": image_key},
                    {"Name": "result",   "Value": prediction},
                ],
                "MeasureName":      "confidence",
                "MeasureValue":     str(round(confidence, 6)),
                "MeasureValueType": "DOUBLE",
                "Time":             str(unix_ms),
                "TimeUnit":         "MILLISECONDS",
            }
        ],
        CommonAttributes={},
    )

# ─── Lambda Handler ───────────────────────────────────────────────────────────
def lambda_handler(event, context):
    record = event["Records"][0]
    bucket = record["s3"]["bucket"]["name"]
    key    = record["s3"]["object"]["key"]

    # ── 1. Fetch image ────────────────────────────────────────────────────────
    response    = s3.get_object(Bucket=bucket, Key=key)
    image_bytes = response["Body"].read()

    # ── 2. Inference ──────────────────────────────────────────────────────────
    x         = preprocess(image_bytes)
    pred      = session.run([output_name], {input_name: x})[0]
    logit     = pred[0][0]
    prob      = float(1 / (1 + np.exp(-logit)))   # sigmoid
    result    = "DEFECT" if prob > 0.4 else "GOOD"
    unix_ms   = int(time.time() * 1000)            # millisecond epoch timestamp

    print(f"[Lambda] key={key}  result={result}  confidence={prob:.4f}  unix_ms={unix_ms}")

    # ── 3. Persist to Timestream ──────────────────────────────────────────────
    write_to_timestream(key, result, prob, unix_ms)

    # ── 4. Publish to IoT Core → ESP32-S3 ────────────────────────────────────
    iot.publish(
        topic="inspection/result",
        qos=1,
        payload=json.dumps({
            "image":      key,
            "prediction": result,
            "confidence": prob,
            "unix_ms":    unix_ms,          # ← new: used by ESP32-S3 for logging
            "timestamp":  datetime.now(timezone.utc).isoformat(),
        }),
    )

    return {
        "statusCode": 200,
        "body": json.dumps({"prediction": result, "confidence": prob}),
    }
