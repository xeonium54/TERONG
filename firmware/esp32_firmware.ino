/*
  ESP32 Water & Air Quality Monitor — Firebase Firestore version
  Reads TDS, MQ135, and battery voltage (2S Li-ion via voltage divider),
  sends data directly to Firestore over the internet (no local server needed).

  Wiring:
    TDS sensor analog out   -> GPIO34 (ADC1_CH6)
    MQ135 analog out        -> GPIO35 (ADC1_CH7)
    Battery voltage divider -> GPIO32 (ADC1_CH4)
      2S Li-ion pack (~6.0V empty - 8.4V full)
      Divider: 100k (top, battery+ side) and 47k (bottom, to GND)
      Vgpio32 = Vbat * 47k / (100k + 47k) = Vbat * 0.3197
      This keeps GPIO32 voltage under ESP32's ~3.3V max even at full charge
      (8.4V * 0.3197 = 2.69V, safely within range with ADC_11db attenuation)

  Libraries needed (Arduino Library Manager):
    - WiFi (built-in for ESP32)
    - HTTPClient (built-in for ESP32)
    - ArduinoJson

  IMPORTANT: This uses Firestore's REST API in "test mode" (no auth token),
  meaning your Firestore security rules must allow open read/write.
  Fine for prototyping; NOT safe for a production/public deployment.
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ---------- STRUCTS (must come before any function that uses them —
// Arduino auto-generates prototypes at the top of the file, so these
// need to be defined before that point) ----------
struct AirReading {
  int raw;
  float estimatedPPM;
};

struct BatteryReading {
  float voltage;
  int percent;
};

// ---------- CONFIG ----------
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// From Firebase Console > Project Settings > General
const char* FIREBASE_PROJECT_ID = "terong-1744e";
const char* FIREBASE_API_KEY    = "AIzaSyC3GpNixDSqQ5DU8_7jb86fcDFwb_5-lpU";

const char* DEVICE_ID = "esp32-01";     // MUST be unique per device, must match a pin in the dashboard
const char* DEVICE_NAME = "Kitchen Tank";

const int TDS_PIN     = 34;
const int MQ135_PIN   = 35;
const int BATTERY_PIN = 32;

const unsigned long SEND_INTERVAL_MS = 10000; // send every 10 seconds

#define ADC_VREF 3.3
#define ADC_RESOLUTION 4096.0

// Voltage divider ratio: 47k / (100k + 47k)
#define DIVIDER_RATIO 0.3197

// 2S Li-ion voltage range for rough percentage estimate — calibrate to your pack
#define BATTERY_MIN_V 6.0   // ~0%
#define BATTERY_MAX_V 8.4   // 100% (fully charged)

// ---------- STATE ----------
unsigned long lastSend = 0;

void connectWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected! IP: ");
  Serial.println(WiFi.localIP());
}

float readTDS() {
  int raw = analogRead(TDS_PIN);
  float voltage = raw * (ADC_VREF / ADC_RESOLUTION);
  float tdsValue = (133.42 * pow(voltage, 3)
                   - 255.86 * pow(voltage, 2)
                   + 857.39 * voltage) * 0.5;
  return tdsValue < 0 ? 0 : tdsValue;
}

AirReading readMQ135() {
  int raw = analogRead(MQ135_PIN);
  float voltage = raw * (ADC_VREF / ADC_RESOLUTION);
  float estimatedPPM = voltage * 200.0; // placeholder, calibrate RZERO for real ppm
  return { raw, estimatedPPM };
}

BatteryReading readBattery() {
  int raw = analogRead(BATTERY_PIN);
  float pinVoltage = raw * (ADC_VREF / ADC_RESOLUTION);
  float batteryVoltage = pinVoltage / DIVIDER_RATIO;

  int percent = (int)(((batteryVoltage - BATTERY_MIN_V) / (BATTERY_MAX_V - BATTERY_MIN_V)) * 100.0);
  percent = constrain(percent, 0, 100);

  return { batteryVoltage, percent };
}

// Builds a Firestore REST document body and POSTs it as a new document
// in the "readings" collection. Firestore auto-generates the document ID.
void sendToFirestore(float tds, AirReading air, BatteryReading battery) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected, attempting reconnect...");
    connectWiFi();
    return;
  }

  HTTPClient http;
  String url = "https://firestore.googleapis.com/v1/projects/" + String(FIREBASE_PROJECT_ID)
             + "/databases/(default)/documents/readings?key=" + String(FIREBASE_API_KEY);

  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  // Firestore REST format requires each field typed, e.g. {"stringValue": "..."}
  StaticJsonDocument<768> doc;
  JsonObject fields = doc.createNestedObject("fields");

  fields["device_id"]["stringValue"] = DEVICE_ID;
  fields["device_name"]["stringValue"] = DEVICE_NAME;
  fields["tds_ppm"]["doubleValue"] = tds;
  fields["air_quality_raw"]["integerValue"] = String(air.raw);
  fields["air_quality_ppm"]["doubleValue"] = air.estimatedPPM;
  fields["battery_voltage"]["doubleValue"] = battery.voltage;
  fields["battery_percent"]["integerValue"] = String(battery.percent);
  fields["timestamp"]["timestampValue"] = getISOTimestamp();

  String payload;
  serializeJson(doc, payload);

  int httpCode = http.POST(payload);
  if (httpCode > 0) {
    Serial.printf("Firestore POST response: %d\n", httpCode);
    if (httpCode >= 400) {
      Serial.println(http.getString()); // print error body for debugging
    }
  } else {
    Serial.printf("POST failed: %s\n", http.errorToString(httpCode).c_str());
  }
  http.end();

  // Also update a "devices" doc with latest status/last_seen, using the
  // device_id as the fixed document ID so it always overwrites (PATCH).
  updateDeviceStatus(battery);
}

void updateDeviceStatus(BatteryReading battery) {
  HTTPClient http;
  // PATCH to a fixed doc ID = DEVICE_ID under "devices" collection
  String url = "https://firestore.googleapis.com/v1/projects/" + String(FIREBASE_PROJECT_ID)
             + "/databases/(default)/documents/devices/" + String(DEVICE_ID)
             + "?key=" + String(FIREBASE_API_KEY);

  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<384> doc;
  JsonObject fields = doc.createNestedObject("fields");
  fields["device_id"]["stringValue"] = DEVICE_ID;
  fields["device_name"]["stringValue"] = DEVICE_NAME;
  fields["battery_percent"]["integerValue"] = String(battery.percent);
  fields["last_seen"]["timestampValue"] = getISOTimestamp();
  fields["online"]["booleanValue"] = true;

  String payload;
  serializeJson(doc, payload);

  // PATCH creates the doc if it doesn't exist, or overwrites it if it does
  int httpCode = http.sendRequest("PATCH", payload);
  if (httpCode < 200 || httpCode >= 300) {
    Serial.printf("Device status update failed: %d\n", httpCode);
  }
  http.end();
}

// Firestore requires RFC3339 timestamps. ESP32 has no RTC by default, so this
// uses NTP time synced at boot. Make sure configTime() succeeds in setup().
String getISOTimestamp() {
  time_t now;
  struct tm timeinfo;
  time(&now);
  gmtime_r(&now, &timeinfo);
  char buf[30];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  return String(buf);
}

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db); // allows reading up to ~3.3V on ADC pins

  connectWiFi();

  // Sync time via NTP — required for valid Firestore timestamps
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.println("Waiting for NTP time sync...");
  time_t now = time(nullptr);
  while (now < 100000) { // wait until time looks valid (not epoch 0)
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println("\nTime synced.");
}

void loop() {
  if (millis() - lastSend >= SEND_INTERVAL_MS) {
    lastSend = millis();

    float tds = readTDS();
    AirReading air = readMQ135();
    BatteryReading battery = readBattery();

    Serial.printf("TDS: %.1f ppm | Air: %.1f ppm | Battery: %.2fV (%d%%)\n",
                  tds, air.estimatedPPM, battery.voltage, battery.percent);

    sendToFirestore(tds, air, battery);
  }
}
