/**
 * ============================================================================
 * Project: Smart Flammable Material Storage Monitor
 * Platform: ESP32 (Dual-Core 240MHz) with FreeRTOS & NETPIE 2020 (MQTT)
 * ============================================================================
 */

#include <DHT.h>
#include <MQUnifiedsensor.h>
#include <PubSubClient.h>
#include <WiFi.h>

// ==========================================
// Configuration (Credentials & Endpoints)
// ==========================================
#if __has_include("config.h")
  #include "config.h"
#elif __has_include("config.example.h")
  #include "config.example.h"
#else
  #define WIFI_SSID     "YOUR_WIFI_SSID"
  #define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
  #define MQTT_BROKER    "mqtt.netpie.io"
  #define MQTT_PORT      1883
  #define MQTT_CLIENT_ID "YOUR_NETPIE_CLIENT_ID"
  #define MQTT_TOKEN     "YOUR_NETPIE_TOKEN"
  #define MQTT_SECRET    "YOUR_NETPIE_SECRET"
#endif

#ifndef MQTT_PORT
  #define MQTT_PORT 1883
#endif

// Networking Clients
WiFiClient espClient;
PubSubClient client(espClient);

// ==========================================
// Pin Configuration for Sensors and Actuators
// ==========================================

// 1. Ultrasonic Sensor (SR04M-2 / JSN-SR04T)
const int TRIG_PIN = 5;  // Trigger Output Pulse
const int ECHO_PIN = 18; // Echo Input (Connected via 10k/20k voltage divider to protect 3.3V GPIO)

// 2. DHT22 Temperature & Humidity Sensor
#define DHTPIN 14
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

// 3. MQ-5 Combustible Gas / LPG Sensor
#define BOARD "ESP-32"
#define PIN_MQ5 34
#define TYPE_MQ5 "MQ-5"
#define VOLTAGE_RESOLUTION 3.3
#define ADC_BIT_RESOLUTION 12
#define RATIO_MQ5_CLEAN_AIR 6.5
MQUnifiedsensor MQ5(BOARD, VOLTAGE_RESOLUTION, ADC_BIT_RESOLUTION, PIN_MQ5, TYPE_MQ5);

// 4. Relay Actuators (Active HIGH)
#define PUMP_PIN 19 // Relay Channel 1: Submersible Cooling Water Pump
#define FAN_PIN 17  // Relay Channel 2: Ventilation / Exhaust Fan

// ==========================================
// Shared Resources & Mutex Semaphores
// ==========================================
SemaphoreHandle_t xMutexTempHum;
SemaphoreHandle_t xMutexDistance;
SemaphoreHandle_t xMutexGas;

// Global Shared State (Protected by Mutexes)
float _temp = 0.0;
float _hum = 0.0;
bool _isSensorError = false;

int _averageDistance = 0;
float _gasLPG = 0.0;

// ==========================================
// Operational & Safety Thresholds
// ==========================================

// Temperature Mist Control (Hysteresis Band)
const float TEMP_ON  = 27.0; // Turn ON pump when temperature >= 27.0 C
const float TEMP_OFF = 26.9; // Turn OFF pump when temperature <= 26.9 C

// Safety Thresholds
const float GAS_THRESHOLD      = 500.0; // Danger level for LPG / Flammable Gas (PPM)
const float HUMIDITY_THRESHOLD = 55.0;  // Forced ventilation threshold (%RH)
const int WATER_DEPTH_LIMIT    = 80;    // Dry-run protection threshold (Distance in cm)

// Water Tank Dimensions (for Percentage Calculation)
const int TANK_HEIGHT = 100; // Total tank depth (cm)
const int DEAD_ZONE   = 30;  // Top dead zone / sensor blind spot (cm)

// ============================================================================
// Task 1: Ultrasonic Water Level Reading (Core 1)
// Priority: 2 | Frequency: Every 200ms
// ============================================================================
void Task_Ultrasonic(void *pvParameters) {
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  const int numReadings = 5;
  int readings[numReadings] = {0};
  int readIndex = 0;
  long total = 0;

  for (;;) {
    // 1. Generate 15us ultrasonic pulse
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(5);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(15);
    digitalWrite(TRIG_PIN, LOW);

    // 2. Measure Echo pulse duration (timeout 30,000us ~ 5m)
    long duration = pulseIn(ECHO_PIN, HIGH, 30000);
    int currentDist = (duration * 0.034) / 2;

    // 3. Filter valid readings with 5-point Moving Average
    if (currentDist > 0 && currentDist < 500) {
      total = total - readings[readIndex];
      readings[readIndex] = currentDist;
      total = total + readings[readIndex];
      readIndex = (readIndex + 1) % numReadings;

      // 4. Update shared resource under Mutex protection
      if (xSemaphoreTake(xMutexDistance, portMAX_DELAY) == pdTRUE) {
        _averageDistance = total / numReadings;
        xSemaphoreGive(xMutexDistance);
      }
    }

    // Yield CPU time to lower priority tasks
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// ============================================================================
// Task 2: Temperature & Humidity Reading (DHT22) (Core 1)
// Priority: 1 | Frequency: Every 2500ms
// ============================================================================
void Task_DHT(void *pvParameters) {
  dht.begin();

  for (;;) {
    float h = dht.readHumidity();
    float t = dht.readTemperature();

    // Update shared temperature and humidity under Mutex protection
    if (xSemaphoreTake(xMutexTempHum, portMAX_DELAY) == pdTRUE) {
      if (isnan(h) || isnan(t)) {
        _isSensorError = true;
      } else {
        _isSensorError = false;
        _temp = t;
        _hum = h;
      }
      xSemaphoreGive(xMutexTempHum);
    }

    // DHT22 sampling period requires >= 2 seconds
    vTaskDelay(pdMS_TO_TICKS(2500));
  }
}

// ============================================================================
// Task 3: Combustible Gas Sensor Reading (MQ-5) (Core 1)
// Priority: 1 | Frequency: Every 1000ms
// ============================================================================
void Task_Gas(void *pvParameters) {
  MQ5.setRegressionMethod(1); // _PPM = a * ratio^b
  MQ5.setA(1163.8);
  MQ5.setB(-3.874);          // Equation parameters for LPG gas
  MQ5.init();

  // Baseline R0 calibration during sensor warm-up
  float calcR0 = 0;
  for (int i = 1; i <= 10; i++) {
    MQ5.update();
    calcR0 += MQ5.calibrate(RATIO_MQ5_CLEAN_AIR);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  MQ5.setR0(calcR0 / 10.0);

  for (;;) {
    MQ5.update();
    float lpg = MQ5.readSensor();

    // Update shared LPG gas PPM under Mutex protection
    if (xSemaphoreTake(xMutexGas, portMAX_DELAY) == pdTRUE) {
      _gasLPG = lpg;
      xSemaphoreGive(xMutexGas);
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// ============================================================================
// Task 4: Safety Interlocking & Actuator Control (Core 1)
// Priority: 3 (High) | Frequency: Every 500ms
// ============================================================================
void Task_Actuator(void *pvParameters) {
  pinMode(PUMP_PIN, OUTPUT);
  pinMode(FAN_PIN, OUTPUT);
  digitalWrite(PUMP_PIN, LOW);
  digitalWrite(FAN_PIN, LOW);

  bool pumpStatus = false;
  bool fanStatus = false;
  bool lastPumpStatus = false;
  bool lastFanStatus = false;

  for (;;) {
    float tempToCheck = 0.0;
    float humToCheck = 0.0;
    float gasToCheck = 0.0;
    int distToCheck = 0;
    bool sensorError = false;

    // 1. Safely retrieve latest shared sensor telemetry
    if (xSemaphoreTake(xMutexTempHum, portMAX_DELAY) == pdTRUE) {
      tempToCheck = _temp;
      humToCheck = _hum;
      sensorError = _isSensorError;
      xSemaphoreGive(xMutexTempHum);
    }
    if (xSemaphoreTake(xMutexGas, portMAX_DELAY) == pdTRUE) {
      gasToCheck = _gasLPG;
      xSemaphoreGive(xMutexGas);
    }
    if (xSemaphoreTake(xMutexDistance, portMAX_DELAY) == pdTRUE) {
      distToCheck = _averageDistance;
      xSemaphoreGive(xMutexDistance);
    }

    // 2. Safety Interlock Evaluation
    bool isGasDanger   = (gasToCheck > GAS_THRESHOLD);
    bool isHumDanger   = (humToCheck > HUMIDITY_THRESHOLD);
    bool isWaterEmpty  = (distToCheck > WATER_DEPTH_LIMIT);

    // 3. Ventilation Fan Control Logic
    // Runs when gas concentration exceeds safe threshold OR humidity is high
    fanStatus = (isGasDanger || isHumDanger);

    // 4. Water Pump Control Logic (Fail-Safe & Dry-run Protection)
    if (sensorError || isGasDanger || isWaterEmpty) {
      // Emergency cut-off:
      //  - Gas leak: Cut pump immediately to prevent relay spark ignition
      //  - Water empty: Prevent pump motor burnout (dry-run)
      //  - Sensor failure: Fail-safe mode
      pumpStatus = false;
    } else {
      // Normal temperature regulation with Hysteresis
      if (tempToCheck >= TEMP_ON) {
        pumpStatus = true;
      } else if (tempToCheck <= TEMP_OFF) {
        pumpStatus = false;
      }
    }

    // 5. Apply digital signals to relay hardware
    digitalWrite(PUMP_PIN, pumpStatus ? HIGH : LOW);
    digitalWrite(FAN_PIN, fanStatus ? HIGH : LOW);

    // 6. Serial state-change monitoring & emergency warnings
    if (pumpStatus != lastPumpStatus || fanStatus != lastFanStatus) {
      Serial.printf("[Actuator] PUMP: [%s] | FAN: [%s]\n",
                    pumpStatus ? "ON" : "OFF",
                    fanStatus ? "ON" : "OFF");
      if (isGasDanger)  Serial.println(F(">>> [SAFETY ALERT] GAS LEAK DETECTED! <<<"));
      if (isWaterEmpty) Serial.println(F(">>> [SAFETY ALERT] WATER LEVEL CRITICAL (DRY RUN)! <<<"));
      if (sensorError)  Serial.println(F(">>> [SAFETY ALERT] SENSOR READ ERROR! <<<"));

      lastPumpStatus = pumpStatus;
      lastFanStatus = fanStatus;
    }

    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

// ============================================================================
// Task 5: IoT Network Management & Cloud Telemetry (Core 0)
// Priority: 1 | Frequency: Every 2000ms publish (loop 100ms)
// ============================================================================
void Task_IoT(void *pvParameters) {
  client.setServer(MQTT_BROKER, MQTT_PORT);
  unsigned long lastPublish = 0;

  for (;;) {
    // 1. Maintain WiFi Connection
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println(F("[WiFi] Connecting..."));
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      int wifiRetries = 0;
      while (WiFi.status() != WL_CONNECTED && wifiRetries < 10) {
        vTaskDelay(pdMS_TO_TICKS(500));
        wifiRetries++;
      }
      if (WiFi.status() != WL_CONNECTED) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      } else {
        Serial.println(F("[WiFi] Connected successfully!"));
      }
    }

    // 2. Maintain NETPIE 2020 MQTT Connection
    if (!client.connected()) {
      Serial.print(F("[MQTT] Connecting to NETPIE Broker..."));
      if (client.connect(MQTT_CLIENT_ID, MQTT_TOKEN, MQTT_SECRET)) {
        Serial.println(F(" Connected!"));
      } else {
        Serial.printf(" Connection failed (state: %d). Retrying in 5s...\n", client.state());
        vTaskDelay(pdMS_TO_TICKS(5000));
        continue;
      }
    }

    client.loop(); // Handle incoming MQTT packets & keepalive

    // 3. Periodic Telemetry Publish (every 2 seconds)
    if (millis() - lastPublish > 2000) {
      float t = 0.0;
      float h = 0.0;
      float gas = 0.0;
      int dist = 0;

      if (xSemaphoreTake(xMutexTempHum, portMAX_DELAY) == pdTRUE) {
        t = _temp;
        h = _hum;
        xSemaphoreGive(xMutexTempHum);
      }
      if (xSemaphoreTake(xMutexDistance, portMAX_DELAY) == pdTRUE) {
        dist = _averageDistance;
        xSemaphoreGive(xMutexDistance);
      }
      if (xSemaphoreTake(xMutexGas, portMAX_DELAY) == pdTRUE) {
        gas = _gasLPG;
        xSemaphoreGive(xMutexGas);
      }

      // Calculate Water Percentage:
      // Formula: ((TankHeight - MeasuredDist) / (TankHeight - DeadZone)) * 100
      float waterPercent = ((float)(TANK_HEIGHT - dist) / (float)(TANK_HEIGHT - DEAD_ZONE)) * 100.0;
      waterPercent = constrain(waterPercent, 0.0, 100.0);

      // Construct JSON Payload for NETPIE shadow topic (@shadow/data/update)
      char payload[256];
      snprintf(payload, sizeof(payload),
               "{\"data\": {\"temp\": %.2f, \"hum\": %.2f, \"water_lv\": %.1f, \"dist\": %d, \"gas\": %.2f}}",
               t, h, waterPercent, dist, gas);

      Serial.print(F("[MQTT] Publishing: "));
      Serial.println(payload);

      client.publish("@shadow/data/update", payload);
      lastPublish = millis();
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ============================================================================
// System Initialization: FreeRTOS Mutex & Task Scheduling (No Polling)
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println(F("\n========================================================"));
  Serial.println(F("  Smart Flammable Material Storage Monitor (FreeRTOS)   "));
  Serial.println(F("========================================================"));

  // 1. Initialize Mutex Semaphores for thread-safe shared memory access
  xMutexTempHum  = xSemaphoreCreateMutex();
  xMutexDistance = xSemaphoreCreateMutex();
  xMutexGas      = xSemaphoreCreateMutex();

  if (xMutexTempHum == NULL || xMutexDistance == NULL || xMutexGas == NULL) {
    Serial.println(F("[ERROR] Failed to allocate Mutex Semaphores!"));
    while (1);
  }

  // 2. Spawn and Pin FreeRTOS Tasks to Dual Cores
  // Core 1: Sensor telemetry acquisition & hardware actuation
  xTaskCreatePinnedToCore(Task_Ultrasonic, "Task_Ultrasonic", 2048, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(Task_DHT,        "Task_DHT",        2048, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(Task_Gas,        "Task_Gas",        2048, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(Task_Actuator,   "Task_Actuator",   2048, NULL, 3, NULL, 1); // High priority safety task

  // Core 0: Wireless networking & Cloud IoT synchronization
  xTaskCreatePinnedToCore(Task_IoT,        "Task_IoT",        4096, NULL, 1, NULL, 0);

  Serial.println(F("[FreeRTOS] All tasks pinned and scheduled successfully."));

  // 3. Terminate Arduino setup/loop task to eliminate polling overhead
  vTaskDelete(NULL);
}

void loop() {
  // Empty - Pure FreeRTOS multitasking architecture without loop() polling
}