/*
 * ============================================================
 *  라벤더 정원 IoT 모니터 — ESP32 메인 코드
 *  서버 : https://eyiot.vercel.app
 *  센서 : DHT11 (온도·습도)
 *  출력 : SSD1306 OLED 128×64 (I2C)
 *         선풍기 릴레이 (Digital, GPIO 18)
 *         물펌프 릴레이 (Digital, GPIO 19)
 *         상태 LED (GPIO 2)
 *  주기 : 5분마다 서버 전송 & AI 응답 수신
 * ============================================================
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <U8g2lib.h>
#include <Wire.h>

// ===== Wi-Fi 설정 =====
const char* WIFI_SSID     = "bugs";
const char* WIFI_PASSWORD = "bugs1234";

// ===== 서버 설정 =====
const char* SERVER_URL = "https://eyiot.vercel.app/sensor";

// ===== 핀 설정 =====
const int DHT_PIN      = 4;   // DHT11 데이터
const int LED_PIN      = 2;   // 내장 LED (상태 표시)
const int FAN_PIN      = 18;  // 선풍기 릴레이 (디지털)
const int PUMP_PIN     = 19;  // 물펌프 릴레이 (디지털)

// ===== 릴레이 활성 레벨 =====
// 대부분의 릴레이 모듈은 LOW 신호로 동작(활성 LOW).
// 활성 HIGH 릴레이라면 RELAY_ON = HIGH, RELAY_OFF = LOW 로 변경하세요.
const int RELAY_ON  = LOW;
const int RELAY_OFF = HIGH;

// ===== 타이머 =====
const unsigned long SEND_INTERVAL_MS = 5UL * 60UL * 1000UL;  // 5분
unsigned long lastSendMs = 0;

// ===== DHT 센서 =====
DHT dht(DHT_PIN, DHT11);

// ===== OLED 디스플레이 (I2C, SDA=21, SCL=22) =====
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/U8X8_PIN_NONE);

// ===== 전역 상태 =====
float   g_temp   = NAN;
float   g_hum    = NAN;
bool    g_fanOn  = false;
bool    g_pumpOn = false;
String  g_statusMsg = "부팅 중...";
bool    g_sending   = false;

// ============================================================
//  유틸리티
// ============================================================

/** LED 블링크 (비차단 — 짧게 n번) */
void blinkLed(int times, int ms = 120) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(ms);
    digitalWrite(LED_PIN, LOW);
    delay(ms);
  }
}

/** WiFi 연결 (재시도 포함) */
bool ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;

  Serial.print("[WiFi] 연결 시도: ");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED && attempt < 20) {
    delay(500);
    Serial.print(".");
    attempt++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WiFi] 연결됨: ");
    Serial.println(WiFi.localIP());
    blinkLed(3, 80);
    return true;
  }
  Serial.println("[WiFi] 연결 실패");
  return false;
}

// ============================================================
//  모터 제어
// ============================================================

void setFan(bool on) {
  g_fanOn = on;
  digitalWrite(FAN_PIN, on ? RELAY_ON : RELAY_OFF);
  Serial.printf("[릴레이] 선풍기 %s\n", on ? "ON" : "OFF");
}

void setPump(bool on) {
  g_pumpOn = on;
  digitalWrite(PUMP_PIN, on ? RELAY_ON : RELAY_OFF);
  Serial.printf("[릴레이] 물펌프 %s\n", on ? "ON" : "OFF");
}

// ============================================================
//  AI 응답 파싱 — JSON 블록에서 fan/pump 상태 추출
// ============================================================

/**
 * Gemini 응답 문자열에서 첫 번째 { } JSON 블록을 찾아
 * fan_motor / pump_motor(water_pump) 값을 파싱합니다.
 * 파싱 성공 시 true 반환.
 */
bool parseMotorJson(const String& text, bool& fanOn, bool& pumpOn) {
  int start = text.indexOf('{');
  int end   = text.lastIndexOf('}');
  if (start < 0 || end <= start) return false;

  String jsonBlock = text.substring(start, end + 1);
  Serial.print("[JSON 블록] ");
  Serial.println(jsonBlock);

  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, jsonBlock);
  if (err) {
    Serial.print("[JSON 파싱 실패] ");
    Serial.println(err.c_str());
    return false;
  }

  // fan_motor 키 탐색 (다양한 키명 대응)
  const char* fanKeys[]  = {"fan_motor", "fan", "선풍기", "fan_status"};
  const char* pumpKeys[] = {"water_pump", "pump_motor", "pump", "물펌프", "pump_status"};

  fanOn  = false;
  pumpOn = false;

  for (const char* k : fanKeys) {
    if (doc.containsKey(k)) {
      JsonVariant v = doc[k];
      if (v.is<bool>())        fanOn = v.as<bool>();
      else if (v.is<int>())    fanOn = v.as<int>() != 0;
      else {
        String s = v.as<String>();
        s.toLowerCase();
        fanOn = (s == "on" || s == "true" || s == "1");
      }
      break;
    }
  }
  for (const char* k : pumpKeys) {
    if (doc.containsKey(k)) {
      JsonVariant v = doc[k];
      if (v.is<bool>())        pumpOn = v.as<bool>();
      else if (v.is<int>())    pumpOn = v.as<int>() != 0;
      else {
        String s = v.as<String>();
        s.toLowerCase();
        pumpOn = (s == "on" || s == "true" || s == "1");
      }
      break;
    }
  }
  return true;
}

// ============================================================
//  OLED 렌더링
// ============================================================

void drawDisplay() {
  u8g2.clearBuffer();

  // ── 제목 줄 ──
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 10, "Lavender Garden IoT");
  u8g2.drawHLine(0, 12, 128);

  // ── 센서 값 ──
  char buf[32];
  if (!isnan(g_temp) && !isnan(g_hum)) {
    u8g2.setFont(u8g2_font_7x14B_tf);
    snprintf(buf, sizeof(buf), "T:%.1f C  H:%.1f%%", g_temp, g_hum);
    u8g2.drawStr(0, 30, buf);
  } else {
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 28, "센서 읽기 중...");
  }

  // ── 모터 상태 ──
  u8g2.setFont(u8g2_font_6x10_tf);
  snprintf(buf, sizeof(buf), "Fan:%s  Pump:%s",
           g_fanOn  ? "ON " : "OFF",
           g_pumpOn ? "ON " : "OFF");
  u8g2.drawStr(0, 46, buf);

  // ── 상태 메시지 ──
  u8g2.drawHLine(0, 50, 128);
  // 상태 메시지는 최대 21자 (6px 폰트 기준)
  String msg = g_statusMsg;
  if (msg.length() > 21) msg = msg.substring(0, 21);
  u8g2.drawStr(0, 62, msg.c_str());

  u8g2.sendBuffer();
}

// ============================================================
//  서버 전송 & AI 응답 처리
// ============================================================

void sendSensorData() {
  // 1. 센서 읽기
  float temp = dht.readTemperature();
  float hum  = dht.readHumidity();

  if (isnan(temp) || isnan(hum)) {
    Serial.println("[DHT] 읽기 실패 — 건너뜀");
    g_statusMsg = "센서 오류";
    drawDisplay();
    return;
  }
  g_temp = temp;
  g_hum  = hum;
  Serial.printf("[DHT] 온도=%.1f  습도=%.1f\n", temp, hum);

  // 2. WiFi 확인
  if (!ensureWiFi()) {
    g_statusMsg = "WiFi 오류";
    drawDisplay();
    return;
  }

  // 3. HTTP POST
  g_sending   = true;
  g_statusMsg = "서버 전송 중...";
  drawDisplay();
  digitalWrite(LED_PIN, HIGH);

  HTTPClient http;
  http.begin(SERVER_URL);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(15000);

  // 요청 본문
  StaticJsonDocument<128> reqDoc;
  reqDoc["temperature"] = temp;
  reqDoc["humidity"]    = hum;
  String reqBody;
  serializeJson(reqDoc, reqBody);

  Serial.print("[HTTP] POST → ");
  Serial.println(SERVER_URL);
  Serial.print("[HTTP] Body: ");
  Serial.println(reqBody);

  int httpCode = http.POST(reqBody);
  Serial.printf("[HTTP] 응답 코드: %d\n", httpCode);

  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    Serial.print("[HTTP] 응답: ");
    Serial.println(payload);

    // 4. 응답 파싱
    StaticJsonDocument<2048> resDoc;
    DeserializationError err = deserializeJson(resDoc, payload);

    if (!err && resDoc.containsKey("result")) {
      String aiText = resDoc["result"].as<String>();

      bool fanOn = false, pumpOn = false;
      if (parseMotorJson(aiText, fanOn, pumpOn)) {
        setFan(fanOn);
        setPump(pumpOn);
        g_statusMsg = String("AI OK F:") + (fanOn ? "ON" : "OF") +
                      " P:" + (pumpOn ? "ON" : "OF");
      } else {
        g_statusMsg = "AI 응답 파싱 실패";
      }
      blinkLed(2, 100);
    } else {
      Serial.print("[파싱 오류] ");
      Serial.println(err.c_str());
      g_statusMsg = "응답 파싱 오류";
      blinkLed(5, 60);
    }
  } else {
    Serial.printf("[HTTP] 오류 코드: %d\n", httpCode);
    g_statusMsg = String("HTTP ERR:") + httpCode;
    blinkLed(5, 60);
  }

  http.end();
  g_sending = false;
  digitalWrite(LED_PIN, LOW);
  drawDisplay();
}

// ============================================================
//  setup / loop
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n[BOOT] 라벤더 정원 IoT 시작");

  // 핀 초기화
  pinMode(LED_PIN,  OUTPUT);
  pinMode(FAN_PIN,  OUTPUT);
  pinMode(PUMP_PIN, OUTPUT);
  digitalWrite(LED_PIN,  LOW);
  // 릴레이 초기 상태 — OFF
  digitalWrite(FAN_PIN,  RELAY_OFF);
  digitalWrite(PUMP_PIN, RELAY_OFF);

  // DHT 시작
  dht.begin();

  // OLED 시작
  Wire.begin();          // SDA=21, SCL=22 (ESP32 기본)
  u8g2.begin();
  g_statusMsg = "WiFi 연결 중...";
  drawDisplay();

  // WiFi 연결
  WiFi.mode(WIFI_STA);
  ensureWiFi();

  g_statusMsg = "준비 완료";
  drawDisplay();

  // 부팅 후 즉시 1회 전송
  sendSensorData();
  lastSendMs = millis();
}

void loop() {
  // WiFi 끊김 감지 — 재연결
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] 재연결 시도...");
    g_statusMsg = "WiFi 재연결...";
    drawDisplay();
    ensureWiFi();
  }

  // 5분 주기 전송
  unsigned long now = millis();
  if (now - lastSendMs >= SEND_INTERVAL_MS) {
    lastSendMs = now;
    sendSensorData();
  }

  // 남은 시간을 OLED 하단에 표시 (전송 중 아닐 때)
  if (!g_sending) {
    unsigned long remaining = SEND_INTERVAL_MS - (millis() - lastSendMs);
    unsigned int  secLeft   = remaining / 1000;
    char buf[24];
    snprintf(buf, sizeof(buf), "Next:%02u:%02u", secLeft / 60, secLeft % 60);
    g_statusMsg = String(buf);
    drawDisplay();
  }

  delay(5000);  // 5초마다 OLED 갱신
}
