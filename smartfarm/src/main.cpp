#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "secrets.h"

// ===================== MQTT 토픽 =====================
#define MQTT_TOPIC   "smartfarm/sensors"

// ===================== 핀 설정 =====================
#define DS18B20_PIN  4
#define CDS_PIN      1
#define PH_PIN       8

// ===================== 샘플링 설정 =====================
#define SAMPLE_COUNT    10
#define SAMPLE_INTERVAL 500     // 0.5초마다 샘플 수집
#define SEND_INTERVAL   5000    // 5초마다 전송

// ===================== CDS 캘리브레이션 =====================
#define CDS_BRIGHT   103
#define CDS_DARK     3100

// ===================== pH 캘리브레이션 =====================
#define PH_VREF         3.3f
#define PH_ADC_MAX      4095
#define PH_DIVIDER      2.0f
#define PH_NEUTRAL_V    3.18f   // 수돗물 기준 중성 전압 (실측값)
#define PH_SLOPE        0.18f
#define PH_OFFSET       0.0f

// ===================== 전역 변수 =====================
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

OneWire oneWire(DS18B20_PIN);
DallasTemperature tempSensor(&oneWire);

float cdsBuffer[SAMPLE_COUNT] = {0};
float phBuffer[SAMPLE_COUNT]  = {0};
float tmpBuffer[SAMPLE_COUNT] = {0};
int bufferIndex = 0;

int rawCDS = 0;
int rawPH  = 0;

unsigned long lastSampleTime = 0;
unsigned long lastSendTime   = 0;

// ===================== 함수 =====================
float getCDSBrightness(int raw) {
    int clamped = constrain(raw, CDS_BRIGHT, CDS_DARK);
    return (float)(CDS_DARK - clamped) / (float)(CDS_DARK - CDS_BRIGHT) * 100.0f;
}

float getPH(int raw) {
    float voltage = raw * (PH_VREF / PH_ADC_MAX) * PH_DIVIDER;
    float ph = 7.0f + ((PH_NEUTRAL_V - voltage) / PH_SLOPE) + PH_OFFSET;
    return constrain(ph, 0.0f, 14.0f);
}

float movingAverage(float* buf, int count) {
    float sum = 0;
    for (int i = 0; i < count; i++) sum += buf[i];
    return sum / count;
}

void connectWiFi() {
    Serial.print("WiFi 연결 중");
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.print(" 연결 완료: ");
    Serial.println(WiFi.localIP());
}

void connectMQTT() {
    mqtt.setServer(MQTT_SERVER, MQTT_PORT);
    while (!mqtt.connected()) {
        Serial.print("MQTT 연결 중...");
        if (mqtt.connect("ESP32_Smartfarm")) {
            Serial.println("완료");
        } else {
            Serial.print("실패(");
            Serial.print(mqtt.state());
            Serial.println(") 5초 후 재시도");
            delay(5000);
        }
    }
}

void setup() {
    Serial.begin(115200);
    analogReadResolution(12);
    tempSensor.begin();

    connectWiFi();
    connectMQTT();

    // 버퍼 초기화
    for (int i = 0; i < SAMPLE_COUNT; i++) {
        cdsBuffer[i] = getCDSBrightness(analogRead(CDS_PIN));
        phBuffer[i]  = getPH(analogRead(PH_PIN));
        tempSensor.requestTemperatures();
        float t = tempSensor.getTempCByIndex(0);
        tmpBuffer[i] = (t == DEVICE_DISCONNECTED_C) ? 0.0f : t;
        delay(50);
    }

    Serial.println("준비 완료");
}

void loop() {
    // WiFi / MQTT 재연결
    if (WiFi.status() != WL_CONNECTED) connectWiFi();
    if (!mqtt.connected()) connectMQTT();
    mqtt.loop();

    unsigned long now = millis();

    // 0.5초마다 샘플 수집
    if (now - lastSampleTime >= SAMPLE_INTERVAL) {
        rawCDS = analogRead(CDS_PIN);
        rawPH  = analogRead(PH_PIN);

        cdsBuffer[bufferIndex] = getCDSBrightness(rawCDS);
        phBuffer[bufferIndex]  = getPH(rawPH);

        tempSensor.requestTemperatures();
        float t = tempSensor.getTempCByIndex(0);
        tmpBuffer[bufferIndex] = (t == DEVICE_DISCONNECTED_C) ? -999.0f : t;

        bufferIndex = (bufferIndex + 1) % SAMPLE_COUNT;
        lastSampleTime = now;
    }

    // 5초마다 MQTT 전송
    if (now - lastSendTime >= SEND_INTERVAL) {
        float avgCDS  = movingAverage(cdsBuffer, SAMPLE_COUNT);
        float avgPH   = movingAverage(phBuffer,  SAMPLE_COUNT);
        float avgTemp = movingAverage(tmpBuffer, SAMPLE_COUNT);

        char payload[200];
        snprintf(payload, sizeof(payload),
            "{\"ts\":%lu,\"raw_cds\":%d,\"raw_ph\":%d,\"light\":%.1f,\"ph\":%.2f,\"water_temp\":%.2f}",
            now, rawCDS, rawPH, avgCDS, avgPH, avgTemp
        );

        mqtt.publish(MQTT_TOPIC, payload);
        Serial.println(payload);

        lastSendTime = now;
    }
}
