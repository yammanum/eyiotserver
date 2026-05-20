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
const unsigned long SEND_INTERVAL_MS = 3UL * 60UL * 1000UL;  // 3분
const unsigned long SENSOR_READ_INTERVAL_MS = 1000;           // 1초
unsigned long lastSendMs = 0;
unsigned long lastSensorReadMs = 0;

// ===== HTTP 통신 설정 =====
const int HTTP_CONNECT_TIMEOUT_MS = 12000;
const int HTTP_READ_TIMEOUT_MS    = 35000;
const int HTTP_MAX_RETRY          = 2;

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
String  g_advice = "조언을 기다리는 중...";
bool    g_sending   = false;

// ===== OLED 페이지 제어 =====
int     g_currentPage = 0;      // 0: 센서/모터, 1: 조언
const int PAGE_COUNT = 2;
unsigned long g_lastPageChangeMs = 0;
const unsigned long PAGE_CHANGE_INTERVAL_MS = 5000;  // 5초마다 페이지 전환
int g_adviceSubPage = 0;
int g_advicePageCount = 1;

const int ADVICE_CHARS_PER_LINE = 8;
const int ADVICE_LINES_PER_PAGE = 2;

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

/** UTF-8 시작 바이트 기준 문자 길이 반환 */
int utf8CharBytes(uint8_t c) {
  if ((c & 0x80) == 0x00) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1;
}

/** UTF-8 문자열의 문자 수(코드포인트 수) 계산 */
int utf8Length(const String& s) {
  int count = 0;
  int i = 0;
  while (i < s.length()) {
    uint8_t c = (uint8_t)s[i];
    i += utf8CharBytes(c);
    count++;
  }
  return count;
}

/** UTF-8 문자열을 문자 단위로 잘라 반환 */
String utf8Substring(const String& s, int startChar, int charCount) {
  if (charCount <= 0) return "";

  int i = 0;
  int charIdx = 0;
  int startByte = -1;
  int endByte = s.length();

  while (i < s.length()) {
    if (charIdx == startChar) {
      startByte = i;
    }
    if (charIdx == startChar + charCount) {
      endByte = i;
      break;
    }

    uint8_t c = (uint8_t)s[i];
    i += utf8CharBytes(c);
    charIdx++;
  }

  if (startByte < 0) return "";
  return s.substring(startByte, endByte);
}

int calcAdvicePageCount(const String& advice) {
  int totalChars = utf8Length(advice);
  int charsPerPage = ADVICE_CHARS_PER_LINE * ADVICE_LINES_PER_PAGE;
  if (charsPerPage <= 0) return 1;
  int pages = (totalChars + charsPerPage - 1) / charsPerPage;
  return max(1, pages);
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
 * fan / pump / advice 값을 파싱합니다.
 * 파싱 성공 시 true 반환.
 */
bool parseMotorJson(const String& text, bool& fanOn, bool& pumpOn, String& advice) {
  advice = "(조언 없음)";
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

  // 키는 fan, pump 고정 포맷만 처리
  const char* fanKeys[]  = {"fan"};
  const char* pumpKeys[] = {"pump"};

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

  // advice 파싱
  if (doc.containsKey("advice")) {
    advice = doc["advice"].as<String>();
  }

  return true;
}

// ============================================================
//  OLED 렌더링 — 페이지 기반
// ============================================================

void drawDisplay() {
  u8g2.clearBuffer();

  if (g_currentPage == 0) {
    // 페이지 0: 센서 값 & 모터 상태
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 10, "Lavender Garden IoT");
    u8g2.drawHLine(0, 12, 128);

    char buf[32];
    if (!isnan(g_temp) && !isnan(g_hum)) {
      u8g2.setFont(u8g2_font_7x14B_tf);
      snprintf(buf, sizeof(buf), "T:%.1f C  H:%.1f%%", g_temp, g_hum);
      u8g2.drawStr(0, 30, buf);
    } else {
      u8g2.setFont(u8g2_font_unifont_t_korean2);
      u8g2.setCursor(0, 30);
      u8g2.print("센서 읽기 중...");
    }

    u8g2.setFont(u8g2_font_6x10_tf);
    snprintf(buf, sizeof(buf), "Fan:%s  Pump:%s",
             g_fanOn  ? "ON " : "OFF",
             g_pumpOn ? "ON " : "OFF");
    u8g2.drawStr(0, 46, buf);

    u8g2.drawHLine(0, 50, 128);
    u8g2.setFont(u8g2_font_unifont_t_korean2);
    u8g2.setCursor(0, 62);
    String msg = g_statusMsg;
    u8g2.print(msg);
  }
  else if (g_currentPage == 1) {
    // 페이지 1: Gemini 조언
    u8g2.setFont(u8g2_font_unifont_t_korean2);
    u8g2.setCursor(0, 12);
    u8g2.print("AI 조언");
    u8g2.drawHLine(0, 12, 128);

    String advice = g_advice;
    g_advicePageCount = calcAdvicePageCount(advice);
    if (g_adviceSubPage >= g_advicePageCount) g_adviceSubPage = 0;

    u8g2.setFont(u8g2_font_unifont_t_korean2);
    for (int line = 0; line < ADVICE_LINES_PER_PAGE; line++) {
      int startChar = (g_adviceSubPage * ADVICE_LINES_PER_PAGE + line) * ADVICE_CHARS_PER_LINE;
      String chunk = utf8Substring(advice, startChar, ADVICE_CHARS_PER_LINE);
      if (chunk.length() == 0) break;
      int y = 30 + (line * 16);
      u8g2.setCursor(0, y);
      u8g2.print(chunk);
    }

    u8g2.drawHLine(0, 50, 128);
    u8g2.setFont(u8g2_font_6x10_tf);
    char pageBuf[16];
    snprintf(pageBuf, sizeof(pageBuf), "[%d/%d]", g_adviceSubPage + 1, g_advicePageCount);
    u8g2.drawStr(95, 62, pageBuf);
  }

  u8g2.sendBuffer();
}

// ============================================================
//  서버 전송 & AI 응답 처리
// ============================================================

bool updateSensorReadings() {
  float temp = dht.readTemperature();
  float hum  = dht.readHumidity();

  if (isnan(temp) || isnan(hum)) {
    Serial.println("[DHT] 읽기 실패");
    return false;
  }

  g_temp = temp;
  g_hum  = hum;
  Serial.printf("[DHT] 온도=%.1f  습도=%.1f\n", temp, hum);
  return true;
}

void sendSensorData() {
  // 1. 최신 센서값 확보 (실패 시 1회 재시도)
  if ((isnan(g_temp) || isnan(g_hum)) && !updateSensorReadings()) {
    Serial.println("[DHT] 읽기 실패 — 건너뜀");
    g_statusMsg = "센서 오류";
    drawDisplay();
    return;
  }
  float temp = g_temp;
  float hum  = g_hum;

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

  int httpCode = -1;
  String payload = "";

  for (int attempt = 1; attempt <= HTTP_MAX_RETRY; attempt++) {
    HTTPClient http;
    http.begin(SERVER_URL);
    http.addHeader("Content-Type", "application/json");
    http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
    http.setTimeout(HTTP_READ_TIMEOUT_MS);

    Serial.printf("[HTTP] 시도 %d/%d\n", attempt, HTTP_MAX_RETRY);
    httpCode = http.POST(reqBody);
    Serial.printf("[HTTP] 응답 코드: %d\n", httpCode);

    if (httpCode > 0) {
      payload = http.getString();
      http.end();
      break;
    }

    Serial.printf("[HTTP] 오류 상세: %s\n", http.errorToString(httpCode).c_str());
    http.end();

    if (attempt < HTTP_MAX_RETRY) {
      delay(1500);
    }
  }

  if (httpCode == HTTP_CODE_OK) {
    Serial.print("[HTTP] 응답: ");
    Serial.println(payload);

    // 4. 응답 파싱
    StaticJsonDocument<2048> resDoc;
    DeserializationError err = deserializeJson(resDoc, payload);

    if (!err && resDoc.containsKey("result")) {
      String aiText = resDoc["result"].as<String>();

      bool fanOn = false, pumpOn = false;
      String advice = "";
      if (parseMotorJson(aiText, fanOn, pumpOn, advice)) {
        setFan(fanOn);
        setPump(pumpOn);
        g_advice = advice;
        g_adviceSubPage = 0;
        g_advicePageCount = calcAdvicePageCount(g_advice);
        g_currentPage = 0;
        g_lastPageChangeMs = millis();
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
    Serial.printf("[HTTP] 오류 상세: %s\n", HTTPClient::errorToString(httpCode).c_str());
    g_statusMsg = String("HTTP ERR:") + httpCode;
    blinkLed(5, 60);
  }

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
  u8g2.enableUTF8Print();
  g_statusMsg = "WiFi 연결 중...";
  drawDisplay();

  // WiFi 연결
  WiFi.mode(WIFI_STA);
  ensureWiFi();

  g_statusMsg = "준비 완료";
  drawDisplay();

  // 부팅 직후 센서 1회 읽기
  if (!updateSensorReadings()) {
    g_statusMsg = "센서 초기화 실패";
    drawDisplay();
  }
  lastSensorReadMs = millis();

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

  // 센서 주기 읽기 (전송과 분리)
  unsigned long now = millis();
  if (!g_sending && now - lastSensorReadMs >= SENSOR_READ_INTERVAL_MS) {
    if (updateSensorReadings()) {
      // 정상적으로 갱신되면 상태 메시지는 카운트다운 로직에서 갱신됨
    } else if (g_currentPage == 0) {
      g_statusMsg = "센서 읽기 실패";
    }
    lastSensorReadMs = now;
  }

  // 3분 주기 전송
  if (now - lastSendMs >= SEND_INTERVAL_MS) {
    lastSendMs = now;
    sendSensorData();
  }

  // 페이지 자동 전환 (5초 주기)
  if (!g_sending && now - g_lastPageChangeMs >= PAGE_CHANGE_INTERVAL_MS) {
    if (g_currentPage == 0) {
      g_currentPage = 1;
      g_adviceSubPage = 0;
    } else {
      if (g_adviceSubPage + 1 < g_advicePageCount) {
        g_adviceSubPage++;
      } else {
        g_currentPage = 0;
        g_adviceSubPage = 0;
      }
    }
    g_lastPageChangeMs = now;
  }

  // 상태 메시지 갱신 (페이지 0일 때만)
  if (!g_sending && g_currentPage == 0) {
    unsigned long remaining = SEND_INTERVAL_MS - (millis() - lastSendMs);
    unsigned int  secLeft   = remaining / 1000;
    char buf[24];
    snprintf(buf, sizeof(buf), "Next:%02u:%02u", secLeft / 60, secLeft % 60);
    g_statusMsg = String(buf);
  }

  drawDisplay();
  delay(1000);  // 1초마다 업데이트
}
