#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#define BLYNK_TEMPLATE_ID ""      // Enter your Template ID
#define BLYNK_TEMPLATE_NAME ""    // Enter your Template Name
#define BLYNK_AUTH_TOKEN ""       // Enter your Auth Token
//#define BLYNK_PRINT Serial

#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <TinyGPS++.h>
#include <time.h>  

// --- eFUSE CALIBRATION LIBRARY ---
#include "esp_adc_cal.h"
// ---------------------------------

// ==========================================
// FIREBASE CONFIGURATION
// ==========================================
#include <Firebase_ESP_Client.h>

// 1. Your Database URL (Remove "https://" and the trailing "/")
#define FIREBASE_HOST "" 

// 2. Your Database Secret 
#define FIREBASE_AUTH ""

// Define Firebase Objects
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;
FirebaseJson json;  // <--- FIXED: MOVED TO GLOBAL TO PREVENT CRASH

// ==========================================
// 1. THE THRESHOLD DEFINITIONS (ADULT DAIRY COW)
// ==========================================
// HEALTH THRESHOLDS
#define MIN_SPO2     90.0   // Hypoxia
#define FEVER_LIMIT  40.5   // Exterior Skin/Solar Adjusted Fever (Celsius)

// Heart Rate Limits
#define MIN_HR       40.0   // Bradycardia
#define CRITICAL_HR  30.0   // Cardiac Arrest Risk
#define MAX_HR       100.0  // Stress/Pain
#define MAX_LYING_TIME 300  // Max mins lying down (Lameness check)

// ==========================================
// 2. NOTIFICATION VARIABLES
// ==========================================
unsigned long lastNotifyTime = 0;       // Stores last time we sent an alert
const long notifyCooldown = 900000;     // 15 Minutes (900,000 ms) delay
String lastAlertMsg = "";               // Stores the last message to prevent duplicate repeats

char ssid[] = ""; // Enter your WiFi SSID
char pass[] = ""; // Enter your WiFi Password

// ------------------------------
// HARDWARE PINS
// ------------------------------
// I2C (MPU6050 & MAX30100)
#define I2C_SDA_PIN 32
#define I2C_SCL_PIN 33

// DS18B20 (Temp)
#define ONE_WIRE_BUS 4 

// DHT22 (Environmental Weather)
#include <DHT.h>
#define DHTPIN 15       // Connect the DHT22 Data pin to GPIO 15
#define DHTTYPE DHT22   // Specifically using the DHT22 (AM2302)
DHT dht(DHTPIN, DHTTYPE);

// --- GLOBAL ENVIRONMENTAL VARIABLES ---
float globalAmbientTemp = 0.0;
float globalHumidity = 0.0;
String globalHeatStress = "COMFORT ZONE";
unsigned long lastDhtRead = 0; // Timer for non-blocking DHT reads

// NEO-6M GPS
#define RXD2 16  // Connect to GPS TX
#define TXD2 17  // Connect to GPS RX
#define GPS_POWER_PIN 26 // Connects to MOSFET for GPS Power Control

// EXTRAS
#define BATTERY_PIN 34  // Voltage Divider Input

// ------------------------------
// MAIN SENSOR LIBRARIES (I2C)
// ------------------------------
#include <Wire.h>
#include <MPU6050_tockn.h>
#include "MAX30100_PulseOximeter.h"

// ------------------------------
// OBJECTS
// ------------------------------
MPU6050 mpu(Wire);
PulseOximeter pox;
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
TinyGPSPlus gps;
HardwareSerial neogps(2); // Use Serial 2 for GPS

// Shared PulseOximeter Data (Volatile)
volatile float sharedHeartRate = 0.0;
volatile float sharedSpO2 = 0.0;

// ===== MAX30100 FILTER (ADD HERE) =====
#define BUFFER_SIZE 10

float hrBuffer[BUFFER_SIZE];
float spo2Buffer[BUFFER_SIZE];

int bufferIndex = 0;
bool bufferFilled = false;

bool isFingerDetected = false;

// *** OPTIMIZATION: Global Temperature Variables ***
float globalTemp = 0.0;
unsigned long lastTempRead = 0;
bool requestTempFlag = false;          // For Non-Blocking Logic
unsigned long lastTempRequestTime = 0; // For Non-Blocking Logic

// *** GLOBAL TIMERS FOR DURATION LOGIC ***
unsigned long postureTimer = 0;        
String lastPostureState = "Standing";  

// *** GLOBAL TIMERS FOR GPS POWER CONTROL ***
unsigned long gpsPowerTimer = 0;
bool isGpsPoweredOn = true;
const unsigned long GPS_ON_DURATION = 300000;   // 5 Minutes in milliseconds
const unsigned long GPS_OFF_DURATION = 1500000; // 25 Minutes in milliseconds

// =============== GLOBAL MUTEX FOR I2C ===============
SemaphoreHandle_t i2cMutex;

// --- eFUSE CALIBRATION OBJECT ---
esp_adc_cal_characteristics_t adc_chars;
// --------------------------------
 
// ------------------------------
// POX TASK (Runs on Core 0)
// ------------------------------
void poxTask(void *pvParameters) {
  //Serial.println("poxTask running on Core 0");
  for (;;) {
    // Try taking mutex for 10ms only
    if (xSemaphoreTake(i2cMutex, 10 / portTICK_PERIOD_MS)) {
      pox.update();
      float hr = pox.getHeartRate();
      float sp = pox.getSpO2();

      // Finger detection for human
      if (hr > 30 && hr < 200 && sp > 70 && sp <= 100) {
          sharedHeartRate = hr;
          sharedSpO2 = sp;
          isFingerDetected = true;
      } else {
          isFingerDetected = false;
      }
      xSemaphoreGive(i2cMutex);
    }
    // 2 ms is the safe limit. Do not reduce.
    //vTaskDelay(2 / portTICK_PERIOD_MS);
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

// ------------------------------
// CALLBACKS & LOGIC
// ------------------------------
void onBeatDetected() {
  //Serial.println("Beat Detected!");
}

// ===== AVERAGE FUNCTION (ADD HERE) =====
float calculateAverage(float *buffer, int size) {
    float sum = 0;
    for (int i = 0; i < size; i++) {
        sum += buffer[i];
    }
    return sum / size;
}
 // ==========================================
// SIDE-MOUNT ACTIVITY & RUMINATION LOGIC
// ==========================================
String getActivity(float ax, float ay, float az, float pitch) {
  float mag = sqrt(ax * ax + ay * ay + az * az);
  float movementForce = abs(mag - 1.0); 

  // 1. High Movement
  if (movementForce > 0.8) return "Running";

  // 2. Moderate Movement (Grazing or Walking)
  if (movementForce > 0.25) { 
    // SIDE-MOUNT FIX: Use abs() so it works on either the left or right side of the neck
    if (abs(pitch) > 25) { 
        return "Grazing"; // Moving + Neck is bent down
    } else {
        return "Walking"; // Moving + Neck is level/up
    }
  }

  // 3. Micro-Movement (Chewing/Cud)
  if (movementForce >= 0.05 && movementForce <= 0.25) {
      return "Rumination"; 
  }

  // 4. Zero Movement
  return "Resting";
}

// Rumination Level
String getRuminationLevel(String activity) {
    if (activity == "Rumination") return "HIGH";     // Healthy
    if (activity == "Grazing" || activity == "Walking") return "NORMAL"; // Active
    if (activity == "Resting") return "LOW";         // Idle/Sleeping
    return "LOW";
}

// ==========================================
// ==========================================
// 2.5 VETERINARY SLEEP TRACKING LOGIC
// ==========================================
String getSleepStatus(String posture, String activity, float hr, long durationMins) {
  // A cow is asleep when she has been lying down for at least 5 minutes (settled),
  // is completely physically resting (not ruminating), and has a relaxed HR.
  if (posture == "Lying Down" && durationMins > 5 && activity == "Resting" && hr > 0 && hr < 75.0) {
      return "Sleeping";
  }
  return "Awake";
}

// ==========================================
// RESEARCH-BACKED POSTURE LOGIC (SIDE-MOUNT)
// ==========================================
String getPosture(float pitch, String activity) {
  float p = abs(pitch);

  // 1. If the cow is actively moving around, she is physically Standing.
  if (activity == "Running" || activity == "Walking" || activity == "Grazing") {
      return "Standing";
  }
  
  // 2. If the cow is completely still (Resting) and her neck is flat/tucked (angle > 45), she is Lying Down.
  if (activity == "Resting" && p >= 45) {
      return "Lying Down";
  }

  // 3. For ambiguous states (Resting with head up, or Ruminating), use the neck angle as the final tie-breaker.
  if (p < 45) {
    return "Standing"; // Neck is mostly upright
  } else {
    return "Lying Down"; // Neck is dropped/tucked
  }
}

// ==========================================
// ==========================================
// 3. VETERINARY HEALTH STATUS LOGIC
// ==========================================
String getHealthStatus(float spo2, float hr, float temp, String posture, String activity, long durationMins) {
  
  // CATEGORY 1: SPECIFIC DISEASE DIAGNOSTICS (Highest Priority)
  // Mastitis / Metritis (Severe Bacterial Infection: High Fever + Lethargy + Refusal to stand for 2+ hours)
  if (temp > FEVER_LIMIT && posture == "Lying Down" && activity == "Resting" && durationMins > 120) { 
      return "MASTITIS/METRITIS RISK"; 
  }
  
  // CATEGORY 2: GENERAL ALARMS (Emergency)
  if (temp > FEVER_LIMIT) { return "ALARM (FEVER)"; }
  if (hr > 0 && hr < CRITICAL_HR) { return "ALARM (LOW HR)"; }
  if (posture == "Lying Down" && activity == "Running") { return "ALARM (SEIZURE/THRASHING)"; } 
  if (spo2 > 0 && spo2 < MIN_SPO2) { return "ALARM (HYPOXIA)"; }

  // CATEGORY 3: WARNINGS
  if (posture == "Lying Down" && hr > 0 && hr < MIN_HR) { return "WARNING"; }
  if (posture == "Lying Down" && durationMins > MAX_LYING_TIME) { return "WARNING (LAMENESS)"; } 
  if (hr > MAX_HR && activity != "Running") { return "WARNING (STRESS)"; } 
  if (posture == "Standing" && hr > 0 && hr < MIN_HR) { return "WARNING"; }

  // CATEGORY 4: ADVISORY
  if (posture == "Standing" && durationMins > 180) { return "ADVISORY (FATIGUE)"; } 

  // CATEGORY 5: NORMAL
  if (activity == "Running" || activity == "Walking" || activity == "Grazing") { return "NORMAL"; }
  if (posture == "Standing" && activity == "Resting") { return "NORMAL"; }
  if (posture == "Lying Down" && activity == "Resting") { return "NORMAL"; } 

  return "NORMAL"; 
}

// ==========================================
// 2.8 ENVIRONMENTAL HEAT STRESS (THI)
// ==========================================
String getHeatStressLevel(float airTempC, float humidity) {
    // Safety check: Prevent 'Not a Number' (NaN) errors from crashing the math processor
    if (isnan(airTempC) || isnan(humidity)) return "SENSOR ERROR";
    
    // Calculate the THI Score using the veterinary formula
    float thi = (1.8 * airTempC + 32) - ((0.55 - 0.0055 * humidity) * (1.8 * airTempC - 26));

    // Return the scientific stress bracket
    if (thi > 89.0) { return "SEVERE HEAT STRESS"; }
    if (thi >= 79.0 && thi <= 89.0) { return "MODERATE HEAT STRESS"; }
    if (thi >= 72.0 && thi < 79.0) { return "MILD HEAT STRESS"; }
    
    return "COMFORT ZONE";
}

// ==========================================
// 4. VETERINARY BREEDING / ESTRUS DETECTION
// ==========================================
String getBreedingStatus(String activity, float hr, float temp, String healthStatus, String posture, long durationMins) {
  if (healthStatus.startsWith("ALARM") || healthStatus.startsWith("WARNING")) { return "Not Applicable (Sick)"; }
  
  // Estrus is flagged by prolonged restlessness (standing > 1 hour) AND active pacing AND elevated HR
  if (posture == "Standing" && durationMins > 60 && (activity == "Walking" || activity == "Running") && hr >= 80.0 && temp < FEVER_LIMIT) { 
      return "POSSIBLE ESTRUS (Heat)"; 
  }
  
  return "Normal Cycle";
}

// ==========================================
// ==========================================
// 5. NOTIFICATION LOGIC (SMART FILTERING)
// ==========================================
void sendEssentialNotifications(String health, String breeding, String ruminationLevel, String posture, float temp) {
  String currentMsg = "";
  bool urgent = false;
  
  // 1. SPECIFIC DISEASE ALERTS (Highest Priority)
  if (health == "MASTITIS/METRITIS RISK") {
    currentMsg = "🚨 INFECTION ALERT: High fever & lethargy detected. Inspect for Mastitis immediately.";
    urgent = true;
  }
  // 2. GENERAL CRITICAL ALARMS
  else if (health.startsWith("ALARM")) {
    if (temp > FEVER_LIMIT) {
        currentMsg = "⚠️ FEVER ALERT: High Temp (" + String(temp) + "C). Check Cow.";
    } else {
        currentMsg = "⚠️ CRITICAL EMERGENCY: Cow vitals are crashing! Check App.";
    }
    urgent = true;
  }
  // 3. BREEDING / ESTRUS
  else if (breeding == "POSSIBLE ESTRUS (Heat)") {
    currentMsg = "💕 ESTRUS ALERT: Cow showing signs of Heat. Ready for breeding.";
    urgent = true;
  }
  // 4. GENERAL WARNINGS (Lameness, Stress, etc.)
  else if (health.startsWith("WARNING")) {
     currentMsg = "⚠️ HEALTH WARNING: Abnormal vitals detected. Please inspect.";
     urgent = true;
  }
  // 5. DIGESTIVE ALERTS
  else if (ruminationLevel == "LOW" && posture == "Standing") {
     currentMsg = "⚠️ DIGESTIVE ALERT: Low Rumination Activity detected while standing. Check appetite.";
     urgent = true;
  }

  // --- SEND NOTIFICATION WITH COOLDOWN ---
  if (urgent) {
    if ((millis() - lastNotifyTime > notifyCooldown) || (currentMsg != lastAlertMsg)) {
       // Push to Blynk mobile app
       Blynk.logEvent("emergency_alert", currentMsg); 
       
       // Update timers and state
       lastNotifyTime = millis();
       lastAlertMsg = currentMsg;
    }
  }
}
// ------------------------------
// SETUP
// ------------------------------
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // Disable Brownout Detector
  Serial.begin(115200);
  analogReadResolution(12); // ESP32 default is 12-bit (0-4095)
  delay(500);

  // --- GET eFUSE CALIBRATION DATA ---
  esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, &adc_chars);
  // ----------------------------------

  // --- INITIALIZE GPS POWER CONTROL ---
  pinMode(GPS_POWER_PIN, OUTPUT);
  digitalWrite(GPS_POWER_PIN, HIGH); // Turn GPS ON initially
  gpsPowerTimer = millis();
  isGpsPoweredOn = true;

  // --- 2. INITIALIZE NEW HARDWARE ---
  sensors.begin(); // DS18B20
  dht.begin(); // Initialize DHT22 Environmental Sensor
  
  // <--- CRITICAL FIX: ENABLE NON-BLOCKING MODE --->
  sensors.setWaitForConversion(false); 

  // <--- AEROSPACE FIX 1: EXPAND UART BUFFER --->
  neogps.setRxBufferSize(1024); // Expand buffer to survive Firebase blocking delays
  neogps.begin(9600, SERIAL_8N1, RXD2, TXD2); // GPS

  i2cMutex = xSemaphoreCreateMutex();
  if (i2cMutex == NULL) {
    Serial.println("ERROR: Mutex not created!");
    while (1);
  }
 
  //Serial.println("Configuring WiFi and Blynk (Non-Blocking)...");
  WiFi.begin(ssid, pass); // Tell ESP32 to start looking for WiFi in the background
  Blynk.config(BLYNK_AUTH_TOKEN); // Configure Blynk without forcing it to connect immediately
  // The system will now instantly proceed to turn on the sensors, even in a dead zone!
  
  // <--- ADD THESE 2 LINES HERE --->
  //Serial.println("Configuring Network Time...");
  configTime(19800, 0, "pool.ntp.org"); 
  // <------------------------------>

  // ==========================================
  // FIREBASE SETUP
  // ==========================================
  Serial.printf("Firebase Client v%s\n", FIREBASE_CLIENT_VERSION);
  config.signer.tokens.legacy_token = FIREBASE_AUTH;
  config.database_url = FIREBASE_HOST;
  Firebase.reconnectWiFi(true);
  Firebase.begin(&config, &auth);
  //Serial.println("Firebase Initialized.");

  //Serial.println("Initializing I2C...");
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(400000); 
  // <--- AEROSPACE FIX 2: PREVENT PHYSICAL WIRE CRASHES --->
  Wire.setTimeOut(20); // If a wire breaks, give up after 20ms instead of freezing forever 

  //Serial.println("Initializing MPU6050...");
  mpu.begin();

  xSemaphoreTake(i2cMutex, portMAX_DELAY);
  // Replaced calculation with permanent hardware offsets
  mpu.setGyroOffsets(1.40, 2.61, 0.78);
  xSemaphoreGive(i2cMutex);

  //Serial.println("Initializing MAX30100...");
  xSemaphoreTake(i2cMutex, portMAX_DELAY);
  bool ok = pox.begin();
  delay(2000); // VERY IMPORTANT
  
  if (!ok) {
    xSemaphoreGive(i2cMutex); // Give it back if initialization fails
    Serial.println("ERROR: MAX30100 not detected! System continuing...");
  } else {
    // Keep the Mutex locked while configuring the sensor's power
    pox.setIRLedCurrent(MAX30100_LED_CURR_7_6MA);
    pox.setOnBeatDetectedCallback(onBeatDetected);
    xSemaphoreGive(i2cMutex); // Safely release the Mutex now that it is fully configured
  }

  xTaskCreatePinnedToCore(poxTask, "PoxTask", 4096, NULL, 1, NULL, 0);

  Serial.println("\nSystem Ready.\n");
}

// ------------------------------
// LOOP (Runs on Core 1)
// ------------------------------
unsigned long previousWifiCheck = 0; // Global for WiFi Check

void loop() {
  // ==========================================
  // THE 12-HOUR HARDWARE FLUSH (PREVENTS FRAGMENTATION)
  // ==========================================
  // 43,200,000 milliseconds = exactly 12 hours
  if (millis() > 43200000) {
      //Serial.println("Performing 12-Hour Memory Flush...");
      ESP.restart(); 
  }

  // ==========================================
  // 0. WIFI AUTO-RECONNECTION (CRITICAL FIX)
  // ==========================================
  if (millis() - previousWifiCheck > 30000) {
    previousWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.disconnect(true);
      delay(200);
      WiFi.begin(ssid, pass);
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    Blynk.run();
  }
  
  // ==========================================
  // --- SMART GPS POWER CYCLE MANAGEMENT ---
  // ==========================================
  // 1. Check if we currently have a valid, fresh satellite lock (less than 2 seconds old)
  bool hasFreshFix = gps.location.isValid() && gps.location.age() < 2000;
  unsigned long timeSincePowerChange = millis() - gpsPowerTimer;

  if (isGpsPoweredOn) {
      // TURN OFF LOGIC:
      // Condition A: We got a satellite fix AND 5 minutes have passed (Normal farm operation)
      // Condition B: 20 Minutes have passed and still no fix. Turn off to save battery (Cow might be indoors).
      if ((hasFreshFix && timeSincePowerChange >= GPS_ON_DURATION) || (timeSincePowerChange >= 120000)) {
          digitalWrite(GPS_POWER_PIN, LOW); // Turn MOSFET OFF
          
          // --- ANTI-PARASITIC LEAKAGE CODE ---
          neogps.end();         
          pinMode(TXD2, INPUT); 
          pinMode(RXD2, INPUT); 
          // -----------------------------------

          isGpsPoweredOn = false;
          gpsPowerTimer = millis(); // Reset timer for the 25-minute sleep cycle
      }
  } 
  else if (!isGpsPoweredOn && (timeSincePowerChange >= GPS_OFF_DURATION)) {
      // TURN ON LOGIC (Wake up after 25 minutes of sleep)
      digitalWrite(GPS_POWER_PIN, HIGH); // Turn MOSFET ON
      
      // --- RESTORE SERIAL COMMUNICATION ---
      //delay(50); 
      neogps.setRxBufferSize(1024); 
      neogps.begin(9600, SERIAL_8N1, RXD2, TXD2); 
      // ------------------------------------

      isGpsPoweredOn = true;
      gpsPowerTimer = millis(); // Reset timer for the active cycle
  }

  // --- READ GPS CONSTANTLY ---
  while (neogps.available()) {
    gps.encode(neogps.read());
  }

  // ======== MPU6050 UPDATE (RATE-LIMITED TO 50ms) =========
  static unsigned long lastMpuUpdate = 0;
  if (millis() - lastMpuUpdate >= 50) {
      lastMpuUpdate = millis();
      
      xSemaphoreTake(i2cMutex, portMAX_DELAY);
      mpu.update(); // Fetch new data from I2C
      xSemaphoreGive(i2cMutex);
  }

  // 1. Get raw acceleration for Activity Logic (Running, Grazing, etc.)
  float ax = mpu.getAccX();
  float ay = mpu.getAccY();
  float az = mpu.getAccZ();

  // 2. SIDE-MOUNT FIX: Fetch the Y-Axis (Roll) instead of X-Axis (Pitch)
  // Because the box is mounted on the side of the neck, Head Up/Down motion 
  // is now captured by the Y-axis.
  float comp_pitch = mpu.getAngleY(); 

  // RESEARCH FIX: We must calculate Activity FIRST, then feed it into Posture
  String activity = getActivity(ax, ay, az, comp_pitch);
  String posture = getPosture(comp_pitch, activity);
  String ruminationLevel = getRuminationLevel(activity);

  // ============================================================
  // LANE 0: TEMPERATURE UPDATE (NON-BLOCKING ASYNC)
  // ============================================================
  // Step 1: Initiate Request every 15s
  if (millis() - lastTempRead > 15000 && !requestTempFlag) { 
      lastTempRead = millis();
      sensors.requestTemperatures(); 
      requestTempFlag = true;
      lastTempRequestTime = millis();
  }
  
   // Step 2: Read Value after 750ms delay (without blocking loop)
  if (requestTempFlag && (millis() - lastTempRequestTime >= 750)) {
      float t = sensors.getTempCByIndex(0);
      
      // --- THE BIOLOGICAL & HARDWARE FILTER ---
      // 1. Keep it within realistic biological limits (10C to 50C)
      // 2. Ignore exactly 85.00 (DS18B20 Power-On scratchpad error)
      if (t > 10.0 && t < 50.0 && t != 85.0) { 
          globalTemp = t; 
      } else if (t == -127.00) {
          //Serial.println("⚠️ Temp Sensor Error: Wire Disconnected!");
      }
      
      requestTempFlag = false; // Reset flag
  }

  // ============================================================
  // LANE 0.5: ENVIRONMENTAL WEATHER (NON-BLOCKING EVERY 90s)
  // ============================================================
  if (millis() - lastDhtRead > 90000) {
      lastDhtRead = millis();
      
      // Read humidity and temperature
      float h = dht.readHumidity();
      float t = dht.readTemperature();
      
      // Only update globals if the read was successful
      if (!isnan(h) && !isnan(t)) {
          globalAmbientTemp = t;
          globalHumidity = h;
          globalHeatStress = getHeatStressLevel(globalAmbientTemp, globalHumidity);
      }
  }
  // ============================================================
  // LANE 1: SMART BLYNK UPDATES (OPTIMIZED SENDING)
  // ============================================================
  static unsigned long lastFastPrint = 0;
  static String lastSentHealth = "";    
  static String lastSentBreeding = "";
  static String lastSentPosture = "";
  static String lastSentActivity = "";
  static String lastSentRumination = "";
  static String lastSentSleep = ""; // <--- ADD THIS LINE

  if (millis() - lastFastPrint >= 2000) { 
    lastFastPrint = millis();

    // 1. USE OPTIMIZED SENSOR VALUES
    float currentTemp = globalTemp; 

    bool locationFound = false;
    double currentLat = 0.0, currentLon = 0.0;
    
    // Only parse coordinates if the GPS is ON, valid, AND the data is fresh (< 2 seconds old)
    if (isGpsPoweredOn && gps.location.isValid() && gps.location.age() < 2000) {
       currentLat = gps.location.lat();
       currentLon = gps.location.lng();
       locationFound = true;
    }

    // 2. READ VITALS
    float heartRate = sharedHeartRate;
    float spo2      = sharedSpO2;
    
    // --- CALCULATE DURATION FOR HEALTH CHECK ---
    if (posture != lastPostureState) {
       postureTimer = millis();
       lastPostureState = posture;
    }
    long durationMins = (millis() - postureTimer) / 60000; 

    // 3. RUN HEALTH LOGIC
    String health = getHealthStatus(spo2, heartRate, currentTemp, posture, activity, durationMins);
    String breeding = getBreedingStatus(activity, heartRate, currentTemp, health, posture, durationMins);
    String sleep = getSleepStatus(posture, activity, heartRate, durationMins); // <--- Added durationMins
    
    // 4. SEND ALERTS IF URGENT
    sendEssentialNotifications(health, breeding, ruminationLevel, posture, currentTemp);

    // ========================================
    // --- OPTIMIZED BLYNK SENDING LOGIC ---
    // ========================================
    
    // 1. VITALS & GRAPHS (Always send every 2s for Smooth Graphs)
    Blynk.virtualWrite(V1, currentTemp); 

    // --- ENVIRONMENTAL BLYNK UPDATES ---
    Blynk.virtualWrite(V12, globalAmbientTemp); 
    Blynk.virtualWrite(V13, globalHumidity);
    
    // Only send the Heat Stress text if it changes (prevents flooding)
    static String lastSentHeatStress = "";
    if (globalHeatStress != lastSentHeatStress) {
        Blynk.virtualWrite(V14, globalHeatStress);
        
        // Color code the alert in Blynk
        if (globalHeatStress == "SEVERE HEAT STRESS") Blynk.setProperty(V14, "color", "#FF0000"); // Red
        else if (globalHeatStress == "MODERATE HEAT STRESS") Blynk.setProperty(V14, "color", "#FF8800"); // Orange
        else if (globalHeatStress == "MILD HEAT STRESS") Blynk.setProperty(V14, "color", "#FFFF00"); // Yellow
        else Blynk.setProperty(V14, "color", "#00FF00"); // Green
        
        lastSentHeatStress = globalHeatStress;
    }

    // ===== MAX30100 BUFFER LOGIC (ADD HERE) =====

    // Store values
    if (isFingerDetected) {
        hrBuffer[bufferIndex] = sharedHeartRate;
        spo2Buffer[bufferIndex] = sharedSpO2;

        bufferIndex++;

        if (bufferIndex >= BUFFER_SIZE) {
            bufferIndex = 0;
            bufferFilled = true;
        }
    }

    // Calculate average
    float finalHR = 0;
    float finalSpO2 = 0;

    if (bufferFilled) {
        finalHR = calculateAverage(hrBuffer, BUFFER_SIZE);
        finalSpO2 = calculateAverage(spo2Buffer, BUFFER_SIZE);
    }

    // Send to Blynk
    if (isFingerDetected && finalHR > 0) {
        Blynk.virtualWrite(V8, (int)finalHR);
        Blynk.virtualWrite(V9, (int)finalSpO2);
    } else {
        Blynk.virtualWrite(V8, 0);
        Blynk.virtualWrite(V9, 0);
    }

    // 2. GPS (Only if valid)
    if (locationFound) {
       // FIX FOR STRING V2 (IF YOU SELECTED STRING IN BLYNK)
       String gpsText = String(currentLat, 6) + ", " + String(currentLon, 6);
       Blynk.virtualWrite(V2, gpsText); 
    }
    
    // 3. STATUS STRINGS (Send ONLY ON CHANGE to prevent flooding)
    if (health != lastSentHealth) {
        Blynk.virtualWrite(V5, health);         
        
        // Color Logic inside Change Block
        String colorHex = "#00FF00";
        if (health.startsWith("ALARM"))    colorHex = "#FF0000";
        else if (health.startsWith("WARNING"))  colorHex = "#FF8800";
        else if (health.startsWith("ADVISORY")) colorHex = "#FFFF00";
        
        Blynk.setProperty(V5, "color", colorHex);
        lastSentHealth = health;
    }

    if (breeding != lastSentBreeding) {
        Blynk.virtualWrite(V4, breeding);
        lastSentBreeding = breeding;
    }

    if (posture != lastSentPosture) {
        Blynk.virtualWrite(V6, posture);
        lastSentPosture = posture;
    }

    if (activity != lastSentActivity) {
        Blynk.virtualWrite(V7, activity);
        lastSentActivity = activity;
    }

    if (ruminationLevel != lastSentRumination) {
        Blynk.virtualWrite(V10, ruminationLevel);
        lastSentRumination = ruminationLevel;
    }
    
    // <--- ADD THIS NEW BLOCK FOR SLEEP TRACKING --->
    if (sleep != lastSentSleep) {
        Blynk.virtualWrite(V11, sleep); 
        lastSentSleep = sleep;
    }

    // --- FULL RESTORED SERIAL MONITOR OUTPUT ---
    //Serial.println("====== LiveCare Sensor Output ======");
    //Serial.print("Temp: "); Serial.print(currentTemp); Serial.println(" C");
    
    //if (locationFound) {
       //Serial.print("GPS: "); Serial.print(currentLat, 6); Serial.print(", "); Serial.println(currentLon, 6);
    //} else if (!isGpsPoweredOn) {
       //Serial.println("GPS: Power OFF (Battery Saving)");
    //} else {
      // Serial.println("GPS: Searching...");
    //}

    //Serial.print("Posture: "); Serial.println(posture);
    //Serial.print("Activity: "); Serial.println(activity);
    //Serial.print("Rumination: "); Serial.println(ruminationLevel);
    //Serial.print("Health: "); Serial.println(health);
    //Serial.print("Breeding: "); Serial.println(breeding);
    //Serial.print("Sleep Status: "); Serial.println(sleep);

    //if (heartRate > 0) {
      // Serial.print("Heart Rate: "); Serial.print(heartRate); Serial.println(" bpm");
       //Serial.print("SpO2: "); Serial.print(spo2); Serial.println(" %");
    //} else {
      // Serial.println("Sensor: No Finger/Object Detected");
    //}
    //Serial.println("====================================\n");
  }

  // ============================================================
  // LANE 2: SLOW UPDATES (Every 30 Seconds - FIREBASE)
  // ============================================================
  static unsigned long lastSlowPrint = 0;
  if (millis() - lastSlowPrint >= 30000) { 
    lastSlowPrint = millis();

     // ==========================================
    // BATTERY MONITORING (eFUSE FACTORY CALIBRATED)
    // ==========================================
    // 1. Get raw ADC reading
    uint32_t adc_reading = analogRead(BATTERY_PIN);

    // 2. Use Factory eFuse data to get EXACT pin millivolts
    uint32_t voltage_mv = esp_adc_cal_raw_to_voltage(adc_reading, &adc_chars);
    
    // 3. Convert millivolts to Volts
    float pinVoltage = voltage_mv / 1000.0;

    // 4. Multiply by exact hardware ratio (470k / 470k + 1M internal impedance = ~2.47)
    float batVoltage = pinVoltage * 1.98; 

    // 5. Convert to Percentage (3.0V = 0%, 4.2V = 100%)
    int batPercentage = map((int)(batVoltage * 100), 300, 420, 0, 100);

    // 6. Clamp limits (Prevent -5% or 105%)
    if (batPercentage < 0) batPercentage = 0;
    if (batPercentage > 100) batPercentage = 100;

    // 7. Send Percentage to Blynk
    Blynk.virtualWrite(V3, batPercentage);

    // ==========================================
    // FIREBASE DATA UPLOAD (OPTIMIZED FOR ML)
    // ==========================================
    if (Firebase.ready()) {
        json.clear(); // CLEAR PREVIOUS DATA
        
        // --- GROUP 1: RAW SENSORS (THE ML INGREDIENTS) ---
        json.set("temperature_body", globalTemp); 
        json.set("temperature_ambient", globalAmbientTemp); 
        json.set("env_humidity", globalHumidity);           
        json.set("battery_voltage", batVoltage); 
        
        // --- GROUP 2: GPS ---
        // Only log GPS data if it's valid, actively powered, AND fresh (< 2 seconds old)
        if (isGpsPoweredOn && gps.location.isValid() && gps.location.age() < 2000) {
            json.set("gps_lat", gps.location.lat());
            json.set("gps_lon", gps.location.lng());
        }

        // --- GROUP 3: VITALS (CRITICAL) ---
        json.set("heart_rate", sharedHeartRate);
        json.set("spo2", sharedSpO2);
        
        // --- GROUP 4: RAW MOTION (CRITICAL FOR ML) ---
        // Fetch fresh raw acceleration for the ML pipeline
        float ml_ax = mpu.getAccX(); 
        float ml_ay = mpu.getAccY(); 
        float ml_az = mpu.getAccZ();
        
        json.set("accel_x", ml_ax);
        json.set("accel_y", ml_ay);
        json.set("accel_z", ml_az);
        json.set("gyro_pitch", comp_pitch);

        // --- GROUP 5: TRANSITIONAL BEHAVIOR LABELS ---
        // Keeping basic activity labels to help you read the database manually
        String currentActivity = getActivity(ml_ax, ml_ay, ml_az, comp_pitch);
        String currentPosture = getPosture(comp_pitch, currentActivity); 
        String currentRumination = getRuminationLevel(currentActivity);
        
        json.set("label_posture", currentPosture);
        json.set("label_activity", currentActivity); 
        json.set("rumination_activity", currentRumination);

        // --- GROUP 6: TIMESTAMP (CRITICAL FOR TIME-SERIES ML) ---
        struct tm timeinfo;
        if (getLocalTime(&timeinfo)) {
            char timeStringBuff[50];
            strftime(timeStringBuff, sizeof(timeStringBuff), "%Y-%m-%d %H:%M:%S", &timeinfo);
            json.set("timestamp", String(timeStringBuff));
        } else {
            json.set("timestamp", "Time_Sync_Error");
        }

        // Send to Firebase Path "/livestock_logs"
        if (Firebase.RTDB.pushJSON(&fbdo, "/livestock_logs", &json)) {
            //Serial.println("✅ FULL DATASET PUSHED TO FIREBASE");
        } else {
            Serial.println("❌ FIREBASE ERROR: " + fbdo.errorReason());
        }
    }
  }
  
  // FIX 2: Give WiFi stack time to breathe
  delay(1); 
}