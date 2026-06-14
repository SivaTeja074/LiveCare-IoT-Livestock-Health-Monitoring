# LiveCare Project

AI-powered livestock health monitoring system. An ESP32 smart collar streams sensor data to Firebase; a Streamlit dashboard runs Isolation Forest anomaly detection on live and manual test packets.

## Project Structure

```
LiveCare_Project/
├── dashboard/              # Streamlit web dashboard
│   └── app.py
├── ml/
│   ├── data/               # Training datasets
│   │   └── livecare_production_dataset.csv
│   ├── models/             # Trained model artifacts
│   │   ├── livecare_isolation_forest.pkl
│   │   └── scaler.pkl
│   └── notebooks/          # ML training pipeline
│       └── training_ai.ipynb
├── firmware/               # ESP32 smart collar code
│   └── LiveCare_ESP32.ino
├── config/
│   └── firebase/           # Firebase Admin SDK credentials (gitignored)
├── requirements.txt
└── README.md
```

## Setup
1. Create a virtual environment and install dependencies:

   ```bash
   python -m venv .venv
   .venv\Scripts\activate
   pip install -r requirements.txt
   ```

2. Place your Firebase Admin SDK JSON key in `config/firebase/`:

   ```
   config/firebase/livecare-18328-firebase-adminsdk-fbsvc-5a436cf629.json
   ```

## Usage :

### Run the dashboard

```bash
streamlit run dashboard/app.py
```

Modes:
- **Manual injection** — test healthy vs unhealthy sensor packets
- **Live Firebase stream** — pull the latest ESP32 payload and score it

### Retrain the model

Open and run `ml/notebooks/training_ai.ipynb`. It reads from `ml/data/`, trains an Isolation Forest on healthy baseline data, and saves artifacts to `ml/models/`.

### Flash the ESP32 collar

Open `firmware/LiveCare_ESP32.ino` in the Arduino IDE, set WiFi/Firebase/Blynk credentials, and upload to your ESP32.

## Sensor Features

| Feature | Description |
|---------|-------------|
| `heart_rate` | Pulse oximeter heart rate |
| `temperature_body` | Body/skin temperature |
| `spo2` | Blood oxygen saturation |
| `accel_x/y/z` | Accelerometer axes |
| `gyro_pitch` | Orientation / movement |
| `temperature_ambient` | Environmental temperature |
| `env_humidity` | Environmental humidity |

## Model

- **Algorithm:** Isolation Forest (unsupervised, trained on healthy data only)
- **Scaler:** StandardScaler (required before inference)
- **Output:** `1` = healthy, `-1` = anomaly
