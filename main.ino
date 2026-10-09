#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <DHT.h>

/* ===================== PENGATURAN ===================== */

// Wi-Fi
const char* ssid     = "KOST TIYAH LT 2";
const char* password = "banjarmasin1";

// MQTT publik untuk pengujian (data real-time ke dashboard)
const char* mqtt_server = "test.mosquitto.org";
const int   mqtt_port   = 8883;

const char* topicJarak  = "monitoring/esp32/ihsanfdlh/distance";
const char* topicSuhu   = "monitoring/esp32/ihsanfdlh/temperature";
const char* topicLembap = "monitoring/esp32/ihsanfdlh/humidity";

// Supabase (riwayat data)
const char* SUPABASE_URL = "https://xwrghpljhmflqwglisjg.supabase.co";   // Project URL
const char* SUPABASE_KEY = "sb_publishable_5YmEiOShCOA_A1T4cqlp0Q_DZfFMBvl";           // anon / publishable key

// Interval
const unsigned long PUBLISH_INTERVAL = 2000UL;                // MQTT tiap 2 detik
const unsigned long LOG_INTERVAL     = 15UL * 60UL * 1000UL;  // Supabase tiap 15 menit
const unsigned long LOG_RETRY        = 60UL * 1000UL;         // ulangi 1 menit jika gagal
const unsigned long FIRST_LOG_DELAY  = 20UL * 1000UL;         // simpan pertama 20 detik setelah menyala

// Pin
#define TRIG_PIN 5
#define ECHO_PIN 18
#define DHT_PIN  4
#define DHT_TYPE DHT11

/* ====================================================== */

DHT dht(DHT_PIN, DHT_TYPE);
WiFiClientSecure secureClient;
PubSubClient mqtt(secureClient);

unsigned long lastPublish = 0;
unsigned long lastLog = 0;
unsigned long lastMqttTry = 0;
unsigned long lastWifiTry = 0;
bool firstLogDone = false;

// ---------- Sensor ----------
float bacaJarakSekali() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(3);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  unsigned long durasi = pulseIn(ECHO_PIN, HIGH, 30000);  // timeout 30 ms
  if (durasi == 0) return -1;
  return durasi * 0.0343 / 2.0;
}

// Rata-rata 3 kali pembacaan valid agar lebih stabil
float bacaJarak() {
  float total = 0;
  int valid = 0;
  for (int i = 0; i < 3; i++) {
    float j = bacaJarakSekali();
    if (j >= 0) { total += j; valid++; }
    delay(30);
  }
  if (valid == 0) return -1;
  return total / valid;
}

// ---------- Wi-Fi ----------
void sambungkanWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);

  Serial.print("Menghubungkan Wi-Fi");
  unsigned long mulai = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - mulai < 20000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWi-Fi terhubung!");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWi-Fi gagal, akan dicoba lagi...");
  }
}

// ---------- MQTT (tidak memblokir program) ----------
void cobaMQTT() {
  if (mqtt.connected() || WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastMqttTry < 5000) return;
  lastMqttTry = millis();

  Serial.print("Menghubungkan MQTT...");
  String clientId = "ESP32-";
  clientId += String((uint32_t)ESP.getEfuseMac(), HEX);

  if (mqtt.connect(clientId.c_str())) {
    Serial.println("terhubung!");
  } else {
    Serial.print("gagal, kode: ");
    Serial.println(mqtt.state());
  }
}

// ---------- Supabase ----------
// Nilai tidak valid dikirim sebagai null supaya baris tetap tersimpan
void tambahNilai(String &json, const char* nama, float v, int desimal, bool valid, bool koma) {
  if (koma) json += ",";
  json += "\"";
  json += nama;
  json += "\":";
  json += valid ? String(v, desimal) : String("null");
}

bool simpanKeSupabase(float jarak, float suhu, float lembap) {
  bool okJarak = jarak >= 0;
  bool okSuhu  = !isnan(suhu);
  bool okLembap = !isnan(lembap);

  if (!okJarak && !okSuhu && !okLembap) {
    Serial.println("Supabase: semua sensor gagal, data tidak disimpan");
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Supabase: Wi-Fi belum terhubung");
    return false;
  }

  String json = "{";
  tambahNilai(json, "distance",    jarak, 2, okJarak,  false);
  tambahNilai(json, "temperature", suhu,  1, okSuhu,   true);
  tambahNilai(json, "humidity",    lembap, 1, okLembap, true);
  json += "}";

  WiFiClientSecure client;   // koneksi terpisah dari MQTT
  client.setInsecure();      // cukup untuk proyek hobi

  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(client, String(SUPABASE_URL) + "/rest/v1/sensor_logs")) {
    Serial.println("Supabase: gagal memulai koneksi");
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  // Key lama (JWT, diawali "eyJ") perlu header Authorization; key "sb_publishable_..." tidak
  if (String(SUPABASE_KEY).startsWith("eyJ")) {
    http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  }
  http.addHeader("Prefer", "return=minimal");

  int code = http.POST(json);
  Serial.printf("Supabase status: %d  %s\n", code, json.c_str());
  if (code != 201) {
    Serial.println(http.getString());   // pesan error dari Supabase
  }
  http.end();

  return code == 201;
}

void setup() {
  Serial.begin(115200);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  dht.begin();
  sambungkanWiFi();

  // Untuk pengujian saja: sertifikat TLS tidak diverifikasi
  secureClient.setInsecure();
  mqtt.setServer(mqtt_server, mqtt_port);
  mqtt.setBufferSize(512);
}

void loop() {
  // Sambungkan ulang Wi-Fi tanpa memblokir terlalu lama
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWifiTry > 10000) {
      lastWifiTry = millis();
      sambungkanWiFi();
    }
  }

  cobaMQTT();
  mqtt.loop();

  // ===== Kirim real-time lewat MQTT =====
  if (millis() - lastPublish >= PUBLISH_INTERVAL) {
    lastPublish = millis();

    float jarak = bacaJarak();
    float suhu = dht.readTemperature();
    float kelembapan = dht.readHumidity();
    char nilai[20];

    if (jarak >= 0) {
      snprintf(nilai, sizeof(nilai), "%.2f", jarak);
      if (mqtt.connected()) mqtt.publish(topicJarak, nilai);
      Serial.printf("Jarak: %.2f cm\n", jarak);
    } else {
      Serial.println("Pembacaan jarak gagal");
    }

    if (!isnan(suhu)) {
      snprintf(nilai, sizeof(nilai), "%.2f", suhu);
      if (mqtt.connected()) mqtt.publish(topicSuhu, nilai);
      Serial.printf("Suhu: %.2f C\n", suhu);
    } else {
      Serial.println("Pembacaan DHT11 gagal");
    }

    if (!isnan(kelembapan)) {
      snprintf(nilai, sizeof(nilai), "%.2f", kelembapan);
      if (mqtt.connected()) mqtt.publish(topicLembap, nilai);
      Serial.printf("Kelembapan: %.2f %%\n", kelembapan);
    }
  }

  // ===== Simpan ke Supabase tiap 15 menit =====
  bool waktuPertama = !firstLogDone && millis() >= FIRST_LOG_DELAY;
  bool waktuRutin   = firstLogDone && (millis() - lastLog >= LOG_INTERVAL);

  if (waktuPertama || waktuRutin) {
    float jarak = bacaJarak();
    float suhu = dht.readTemperature();
    float kelembapan = dht.readHumidity();

    if (simpanKeSupabase(jarak, suhu, kelembapan)) {
      firstLogDone = true;
      lastLog = millis();
    } else if (firstLogDone) {
      lastLog = millis() - (LOG_INTERVAL - LOG_RETRY);   // coba lagi 1 menit kemudian
    } else {
      // Belum pernah berhasil: coba lagi 1 menit kemudian
      firstLogDone = true;
      lastLog = millis() - (LOG_INTERVAL - LOG_RETRY);
    }
  }
}
