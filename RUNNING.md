# Building and Running `lpr`

## Build

### Windows (Visual Studio 2026 + vcpkg)
```
cmake --preset full-vcpkg            # configure (uses your vcpkg toolchain)
cmake --build --preset full-vcpkg --config Release
```
The executable is `lpr.exe`.

### Linux/macOS (system OpenCV)
```
cmake --preset full
cmake --build --preset full
```
The executable is `build/full/lpr`.

> First build is heavy (OpenVINO/ONNX/TensorRT headers). Backends are opt-in via
> `-DWITH_OPENVINO=ON -DWITH_TENSORRT=ON -DWITH_ONNXRUNTIME=ON -DWITH_HAILO=ON`.
> Vendor SDK paths: `-DOpenVINO_DIR=...`, `-DONNXRUNTIME_ROOT=...`, etc.

## Run

### Offline (no broker) — load settings from a file and run the pipeline
```
lpr --settings settings.json --config config_file.txt --log-console
```
`settings.json` shape (LPR settings and each camera's settings are arrays of {name,value}):
```json
{
  "settings": [
    { "name": "model_type",     "value": "openvino" },
    { "name": "car_detection",  "value": 1 },
    { "name": "track_plates",   "value": 0 },
    { "name": "plate_detector_model", "value": "/models/east.xml" },
    { "name": "ocr_model",            "value": "/models/ocr.xml" },
    { "name": "car_model",            "value": "/models/yolov5.xml" },
    { "name": "ocr_alphabet",         "value": "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ" },
    { "name": "device",               "value": "CPU" },
    { "name": "heartbeat_interval",   "value": 30000 }
  ],
  "cameras_data": [
    { "camera_id": 1, "settings": [
        { "name": "type_of_link",      "value": "rtsp" },
        { "name": "CameraAddress",     "value": "rtsp://user:pass@host/stream" },
        { "name": "MonoCameraAddress", "value": "" },
        { "name": "CameraDelayTime",   "value": 10 }
    ] }
  ]
}
```

### Production (NATS) — connect, send token, receive settings, run
The client connects, subscribes to the settings subject, and bootstraps when the backend pushes
settings. Connection details come from the environment (CLI flags override):

| Env var          | CLI flag            | Meaning                          |
|------------------|---------------------|----------------------------------|
| `NATS_SERVER_URL`| `--nats <url>`      | server URL (enables NATS mode)   |
| `AUTH_TOKEN`     | `--token <t>`       | auth token                       |
| `NATS_USER`      | `--user <u>`        | username                         |
| `NATS_PASSWORD`  | `--password <p>`    | password                         |
| `NATS_CERT_FILE` | `--cert <path>`     | client cert (mutual TLS)         |
| `NATS_KEY_FILE`  | `--key <path>`      | client key  (mutual TLS)         |
| `NATS_CA_FILE`   | `--ca <path>`       | CA cert     (mutual TLS)         |
|                  | `--settings-subject`| settings subject (default `lpr.settings`) |

Log control: `LOG_DIR` (default `%PROGRAMDATA%\Hoshyar\ALPR\logs` / `/var/log/Hoshyar/ALPR/logs`),
`LOG_RETENTION_DAYS` (default 30), `--log-console` to also echo to the terminal.

Example (token + TLS, like the original):
```
set NATS_SERVER_URL=nats://host:4222
set AUTH_TOKEN=your-token
set NATS_CERT_FILE=client-cert.pem
set NATS_KEY_FILE=client-key.pem
set NATS_CA_FILE=ca.pem
lpr
```

> NATS requires building with the cnats SDK: `-DWITH_NATS=ON`. If headers/libs aren't on the
> default path, point CMake at them: `-DNATS_INC=<cnats/include> -DNATS_LIB=<path/to/libnats>`.
> Without `-DWITH_NATS=ON` the client builds the in-memory transport (offline mode only).

## Supported NATS auth
- **Token** (`natsOptions_SetToken`)
- **User / password** (`natsOptions_SetUserInfo`)
- **Mutual TLS** — cert + key (+ optional CA): `natsOptions_SetSecure` + `LoadCertificatesChain` + `LoadCATrustedCertificates`
- **NKEYS / JWT `.creds`** — NOT yet wired (would be `natsOptions_SetUserCredentialsFromFiles`); ask if you need it.

## Quick local NATS test
```
# 1) start a broker with a token
nats-server -DV --auth your-token

# 2) run the client
AUTH_TOKEN=your-token NATS_SERVER_URL=nats://127.0.0.1:4222 lpr --log-console

# 3) push settings so it bootstraps (NATS CLI), using the settings.json above
nats pub lpr.settings "$(cat settings.json)" --server nats://127.0.0.1:4222 --auth your-token
```
The client logs `bootstrapping from settings`, loads models, creates cameras, and starts detection.
Subjects it publishes/subscribes are listed in `MESSAGE_FORMATS.md`.


## Local config file (model paths)
Model/training-data paths come from a local key=value file (the original's Config), given with
`--config <path>` or env `LPR_CONFIG`. NATS settings only carry runtime toggles. Example:
```
config_file = /models/alpr          # directory prefix for the model files below
plate_detection_file = east.xml     # -> /models/alpr/east.xml
ocr_file = ocr.xml                  # -> /models/alpr/ocr.xml
car_file = yolov5.xml               # -> /models/alpr/yolov5.xml
deep_detect_prob = 0.55             # EAST score threshold
nation_alpr = 1
serial = CAM-SERIAL-123
```
Without `--config`, model paths fall back to the NATS settings keys (model_type/plate_detector_model/
ocr_model/car_model).
