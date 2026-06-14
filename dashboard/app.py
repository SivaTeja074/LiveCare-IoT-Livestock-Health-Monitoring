import streamlit as st
import pandas as pd
import joblib
import firebase_admin
import time
from pathlib import Path
from firebase_admin import credentials, db

PROJECT_ROOT = Path(__file__).resolve().parent.parent
MODELS_DIR = PROJECT_ROOT / "ml" / "models"
FIREBASE_CRED_PATH = PROJECT_ROOT / "config" / "firebase" / "livecare-18328-firebase-adminsdk-fbsvc-5a436cf629.json"

# ==========================================
# PAGE CONFIG
# ==========================================
st.set_page_config(page_title="LiveCare Status Dashboard", layout="centered", page_icon="🐄")

# ==========================================
# LOAD MODEL
# ==========================================
@st.cache_resource
def load_model():
    return joblib.load(MODELS_DIR / "livecare_isolation_forest.pkl")

@st.cache_resource
def load_scaler():
    return joblib.load(MODELS_DIR / "scaler.pkl")

model = load_model()
scaler = load_scaler()

# ==========================================
# FEATURE ORDER (CRITICAL)
# ==========================================
feature_order = [
    'heart_rate','temperature_body','spo2',
    'accel_x','accel_y','accel_z',
    'gyro_pitch','temperature_ambient','env_humidity'
]

# ==========================================
# FIREBASE INIT
# ==========================================
@st.cache_resource
def init_firebase():
    if not firebase_admin._apps:
        cred = credentials.Certificate(str(FIREBASE_CRED_PATH))
        firebase_admin.initialize_app(cred, {
            'databaseURL': 'https://livecare-18328-default-rtdb.asia-southeast1.firebasedatabase.app/'
        })
    return db.reference('/livestock_logs')

# ==========================================
# SIDEBAR
# ==========================================
st.sidebar.title("LiveCare AI Health Status")
page = st.sidebar.radio("Select Mode:", ["💉 Inject manual sensor packets", "🔴 Live Firebase Stream"])
st.sidebar.markdown("---")
st.sidebar.success("Isolation Forest Model: **ACTIVE**")

# ==========================================
# 1. MANUAL TEST
# ==========================================
if page == "💉 Inject manual sensor packets":
    st.title("Manual ML Simulation")
    st.markdown("Inject hardcoded sensor packets directly into the Isolation Forest to test the alarm logic.")
    
    st.markdown("---")
    
    col1, col2 = st.columns(2)

    injected_data = None
    packet_type = ""

    with col1:
        if st.button("🟢 INJECT HEALTHY PACKET", use_container_width=True):
            injected_data = {
                'heart_rate': 62.0,
                'temperature_body': 38.6,
                'spo2': 98.5,
                'accel_x': 0.05,
                'accel_y': 0.02,
                'accel_z': 9.81,
                'gyro_pitch': 2.0,
                'temperature_ambient': 26.0,
                'env_humidity': 55.0
            }
            packet_type = "Healthy Baseline"

    with col2:
        if st.button("🔴 INJECT UNHEALTHY PACKET", use_container_width=True):
            injected_data = {
                'heart_rate': 96.0,
                'temperature_body': 41.2,
                'spo2': 93.0,
                'accel_x': 0.35,
                'accel_y': 0.20,
                'accel_z': 9.60,
                'gyro_pitch': 8.5,
                'temperature_ambient': 40.0,
                'env_humidity': 85.0
            }
            packet_type = "Heat Stress / Fever Anomaly"

    if injected_data:
        st.markdown(f"### Processing: `{packet_type}`")
        st.json(injected_data)

        df = pd.DataFrame([injected_data])[feature_order]

        df_scaled = scaler.transform(df)

        with st.spinner("Running through Isolation Forest..."):
            time.sleep(0.5)
            prediction = model.predict(df_scaled)

        st.markdown("---")
        score = model.decision_function(df_scaled)

        st.write(f"### Anomaly Score: {score[0]:.4f}")

        if prediction[0] == 1:
            st.success("## AI HEALTH STATUS:✅ HEALTHY \nNo anomalies detected. Vitals are within normal ranges.")
        else:
            st.error("## AI HEALTH STATUS:🚨 ALARM TRIGGERED! \n**ANOMALY DETECTED:** The ML model has flagged this packet as highly abnormal.")


# ==========================================
# 2. LIVE FIREBASE
# ==========================================
elif page == "🔴 Live Firebase Stream":  
    st.title("Live Edge-AI Monitor")
    st.markdown("Listening for live JSON payloads from the ESP32 Smart Collar.")
    
    try:
        db_ref = init_firebase()
        
        if st.button("🔄 SCAN LATEST ESP32 DATA", type="primary", use_container_width=True):
            with st.spinner("Pulling from Cloud..."):
                live_data_dict = db_ref.order_by_key().limit_to_last(1).get()
                
                if live_data_dict:
                    for push_id, data in live_data_dict.items():
                        st.info(f"**Timestamp:** `{data.get('timestamp', 'Unknown')}`")
                        
                        live_payload = {
                            'heart_rate': float(data.get('heart_rate', 60.0)),
                            'temperature_body': float(data.get('temperature_body', 38.6)),
                            'spo2': float(data.get('spo2', 98.0)),
                            'accel_x': float(data.get('accel_x', 0.0)),
                            'accel_y': float(data.get('accel_y', 0.0)),
                            'accel_z': float(data.get('accel_z', 9.8)),
                            'gyro_pitch': float(data.get('gyro_pitch', 0.0)),
                            'temperature_ambient': float(data.get('temperature_ambient', 25.0)),
                            'env_humidity': float(data.get('env_humidity', 60.0))
                        }
                        
                        st.json(live_payload)  
                        df = pd.DataFrame([live_payload])[feature_order]  

                        df_scaled = scaler.transform(df)

                        prediction = model.predict(df_scaled)
                        score = model.decision_function(df_scaled)

                        st.write(f"### Anomaly Score: {score[0]:.4f}")
                        
                        if prediction[0] == 1:
                            st.success("## AI HEALTH STATUS:✅ HEALTHY \nThe ESP32 is reporting normal baseline activity.")
                        else:
                            st.error("## AI HEALTH STATUS:🚨 ALARM TRIGGERED! \n**ANOMALY DETECTED:** Immediate attention required.")
                else:
                    st.warning("No data found. Ensure ESP32 is transmitting.")
                    
    except Exception as e:
        st.error(f"Connection Error: {e}")
