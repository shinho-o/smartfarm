#include <Arduino.h>
#include <math.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Wire.h>
#include <Adafruit_SHT31.h>
#include <ESP32Servo.h>
#include "esp_camera.h"
#include "esp_http_server.h"
#include "secrets.h"

// ===================== MQTT 토픽 =====================
#define MQTT_TOPIC       "smartfarm/sensors"
#define MQTT_CONTROL     "smartfarm/control"

// ===================== 센서 핀 설정 (카메라와 충돌 방지) =====================
#define DS18B20_PIN  2       // 수온 센서 (OneWire)
#define CDS_PIN      1       // 광센서 (ADC)
#define PH_PIN       3       // pH 센서 (ADC)
#define SHT31_SDA    38      // SHT31 I2C SDA
#define SHT31_SCL    39      // SHT31 I2C SCL
#define SERVO_PIN    14      // 서보모터

// ===================== 서보 설정 =====================
// 현재(180°)=켜진 상태/대기, 반시계 30도(150°)=OFF
#define SERVO_CENTER     180  // 대기 위치 (켜진 상태)
#define SERVO_ON_ANGLE   180  // ON (현재 위치 유지)
#define SERVO_OFF_ANGLE  150  // OFF (반시계 30도)
#define SERVO_PUSH_TIME  1000  // 밀고 복귀하는 시간 (ms)

// ===================== OV2640 카메라 핀 (Freenove ESP32-S3) =====================
#define CAM_PIN_PWDN   -1
#define CAM_PIN_RESET  -1
#define CAM_PIN_XCLK   15
#define CAM_PIN_SIOD   4
#define CAM_PIN_SIOC   5
#define CAM_PIN_D7     16
#define CAM_PIN_D6     17
#define CAM_PIN_D5     18
#define CAM_PIN_D4     12
#define CAM_PIN_D3     10
#define CAM_PIN_D2     8
#define CAM_PIN_D1     9
#define CAM_PIN_D0     11
#define CAM_PIN_VSYNC  6
#define CAM_PIN_HREF   7
#define CAM_PIN_PCLK   13

// ===================== 샘플링 설정 =====================
#define SAMPLE_COUNT    10
#define SAMPLE_INTERVAL 500     // 0.5초마다 샘플 수집
#define SEND_INTERVAL   5000    // 5초마다 전송

// ===================== CDS 캘리브레이션 =====================
#define CDS_BRIGHT   103
#define CDS_DARK     3100

// ===================== pH 캘리브레이션 =====================
// 생수(pH≈7.0) 기준: raw ≈ 3970
// 1점 캘리브레이션 (2점은 기준액 있을 때 추가)
#define PH_RAW_NEUTRAL  4095    // pH 7.0일 때 ADC raw 값 (생수 기준)
#define PH_RAW_PER_PH   -180.0f // raw 변화량 per pH 1.0 (음수: raw↑ = pH↓)

// ===================== 전역 변수 =====================
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

OneWire oneWire(DS18B20_PIN);
DallasTemperature tempSensor(&oneWire);
Adafruit_SHT31 sht31 = Adafruit_SHT31();
Servo ledServo;
bool ledState = false;

httpd_handle_t streamServer = NULL;

float cdsBuffer[SAMPLE_COUNT]     = {0};
float phBuffer[SAMPLE_COUNT]      = {0};
float tmpBuffer[SAMPLE_COUNT]     = {0};
float airTempBuffer[SAMPLE_COUNT] = {0};
float humBuffer[SAMPLE_COUNT]     = {0};
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
    float ph = 7.0f + (float)(PH_RAW_NEUTRAL - raw) / PH_RAW_PER_PH;
    return constrain(ph, 0.0f, 14.0f);
}

// 중앙값 필터 (튀는 값 제거)
float getMedian(float* buf, int count) {
    float sorted[SAMPLE_COUNT];
    memcpy(sorted, buf, count * sizeof(float));

    // 버블 정렬
    for (int i = 0; i < count - 1; i++) {
        for (int j = 0; j < count - i - 1; j++) {
            if (sorted[j] > sorted[j + 1]) {
                float tmp = sorted[j];
                sorted[j] = sorted[j + 1];
                sorted[j + 1] = tmp;
            }
        }
    }
    return (count % 2 == 0)
        ? (sorted[count / 2 - 1] + sorted[count / 2]) / 2.0f
        : sorted[count / 2];
}

// 이상치 제거 후 평균 (중앙값 기준 ±편차 벗어나면 제외)
float filteredAverage(float* buf, int count) {
    float median = getMedian(buf, count);

    // MAD (Median Absolute Deviation) 계산
    float deviations[SAMPLE_COUNT];
    for (int i = 0; i < count; i++) {
        deviations[i] = fabs(buf[i] - median);
    }
    float mad = getMedian(deviations, count);
    float threshold = (mad < 0.01f) ? 1.0f : mad * 3.0f;

    float sum = 0;
    int valid = 0;
    for (int i = 0; i < count; i++) {
        if (buf[i] > -900 && fabs(buf[i] - median) <= threshold) {
            sum += buf[i];
            valid++;
        }
    }
    return (valid > 0) ? sum / valid : median;
}

// ===================== 카메라 초기화 =====================
bool initCamera() {
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0       = CAM_PIN_D0;
    config.pin_d1       = CAM_PIN_D1;
    config.pin_d2       = CAM_PIN_D2;
    config.pin_d3       = CAM_PIN_D3;
    config.pin_d4       = CAM_PIN_D4;
    config.pin_d5       = CAM_PIN_D5;
    config.pin_d6       = CAM_PIN_D6;
    config.pin_d7       = CAM_PIN_D7;
    config.pin_xclk     = CAM_PIN_XCLK;
    config.pin_pclk     = CAM_PIN_PCLK;
    config.pin_vsync    = CAM_PIN_VSYNC;
    config.pin_href     = CAM_PIN_HREF;
    config.pin_sccb_sda = CAM_PIN_SIOD;
    config.pin_sccb_scl = CAM_PIN_SIOC;
    config.pin_pwdn     = CAM_PIN_PWDN;
    config.pin_reset    = CAM_PIN_RESET;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size   = FRAMESIZE_VGA;    // 640x480
    config.jpeg_quality = 12;
    config.fb_count     = 2;
    config.fb_location  = CAMERA_FB_IN_PSRAM;
    config.grab_mode    = CAMERA_GRAB_LATEST;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("카메라 초기화 실패: 0x%x\n", err);
        return false;
    }
    Serial.println("카메라 초기화 완료");
    return true;
}

// ===================== MJPEG 스트리밍 핸들러 =====================
#define PART_BOUNDARY "123456789000000000000987654321"
static const char* STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t streamHandler(httpd_req_t *req) {
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
    char partBuf[64];

    res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (res != ESP_OK) return res;

    while (true) {
        fb = esp_camera_fb_get();
        if (!fb) {
            Serial.println("카메라 프레임 획득 실패");
            res = ESP_FAIL;
            break;
        }

        size_t hlen = snprintf(partBuf, 64, STREAM_PART, fb->len);
        res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
        if (res == ESP_OK)
            res = httpd_resp_send_chunk(req, partBuf, hlen);
        if (res == ESP_OK)
            res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);

        esp_camera_fb_return(fb);

        if (res != ESP_OK) break;
    }
    return res;
}

// 단일 캡처 핸들러 (정지 이미지)
static esp_err_t captureHandler(httpd_req_t *req) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    httpd_resp_send(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    return ESP_OK;
}

// ===================== 웹 페이지 (카메라 ON/OFF) =====================
static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width,initial-scale=1">
    <title>SmartFarm Monitor</title>
    <style>
        *{margin:0;padding:0;box-sizing:border-box}
        body{font-family:Arial,sans-serif;background:#1a1a2e;color:#eee;
             display:flex;flex-direction:column;align-items:center;padding:20px}
        h1{margin-bottom:20px;color:#0f3460}
        .cam-box{background:#16213e;border-radius:12px;padding:16px;
                 max-width:660px;width:100%;text-align:center}
        .cam-box img{width:100%;border-radius:8px;display:none}
        .cam-box .off-msg{color:#888;padding:60px 0;font-size:1.2em}
        .btn{padding:12px 32px;font-size:1.1em;border:none;border-radius:8px;
             cursor:pointer;margin:16px 8px;transition:0.2s}
        .btn-on{background:#0f3460;color:#fff}
        .btn-on:hover{background:#1a5276}
        .btn-off{background:#c0392b;color:#fff}
        .btn-off:hover{background:#e74c3c}
        .btn-cap{background:#27ae60;color:#fff}
        .btn-cap:hover{background:#2ecc71}
    </style>
</head>
<body>
    <h1>SmartFarm Camera</h1>
    <div class="cam-box">
        <img id="stream" alt="Camera Stream">
        <div id="offMsg" class="off-msg">Camera OFF</div>
    </div>
    <div>
        <button class="btn btn-on" onclick="startStream()">ON</button>
        <button class="btn btn-off" onclick="stopStream()">OFF</button>
        <button class="btn btn-cap" onclick="capture()">Capture</button>
    </div>
    <script>
        const img=document.getElementById('stream');
        const msg=document.getElementById('offMsg');
        function startStream(){
            img.src='/stream?'+Date.now();
            img.style.display='block';msg.style.display='none';
        }
        function stopStream(){
            img.src='';img.style.display='none';msg.style.display='block';
        }
        function capture(){
            window.open('/capture?'+Date.now(),'_blank');
        }
    </script>
</body>
</html>
)rawliteral";

static esp_err_t indexHandler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
}

void startStreamServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 81;

    if (httpd_start(&streamServer, &config) == ESP_OK) {
        httpd_uri_t indexUri = {
            .uri       = "/",
            .method    = HTTP_GET,
            .handler   = indexHandler,
            .user_ctx  = NULL
        };
        httpd_uri_t streamUri = {
            .uri       = "/stream",
            .method    = HTTP_GET,
            .handler   = streamHandler,
            .user_ctx  = NULL
        };
        httpd_uri_t captureUri = {
            .uri       = "/capture",
            .method    = HTTP_GET,
            .handler   = captureHandler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(streamServer, &indexUri);
        httpd_register_uri_handler(streamServer, &streamUri);
        httpd_register_uri_handler(streamServer, &captureUri);
        Serial.println("스트리밍 서버 시작 (포트 81)");
        Serial.print("  웹:     http://");
        Serial.print(WiFi.localIP());
        Serial.println(":81/");
        Serial.print("  스트림: http://");
        Serial.print(WiFi.localIP());
        Serial.println(":81/stream");
        Serial.print("  캡처:   http://");
        Serial.print(WiFi.localIP());
        Serial.println(":81/capture");
    }
}

// ===================== WiFi / MQTT =====================
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

// MQTT 수신 콜백 (서보 제어)
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    char msg[64];
    int len = min((int)length, 63);
    memcpy(msg, payload, len);
    msg[len] = '\0';

    if (strcmp(topic, MQTT_CONTROL) == 0) {
        if (strstr(msg, "\"led\":\"on\"") || strstr(msg, "\"led\": \"on\"")) {
            ledServo.write(SERVO_ON_ANGLE);
            delay(SERVO_PUSH_TIME);
            ledServo.write(SERVO_CENTER);
            ledState = true;
            Serial.println("LED ON (push → return)");
        } else if (strstr(msg, "\"led\":\"off\"") || strstr(msg, "\"led\": \"off\"")) {
            ledServo.write(SERVO_OFF_ANGLE);
            delay(SERVO_PUSH_TIME);
            ledServo.write(SERVO_CENTER);
            ledState = false;
            Serial.println("LED OFF (push → return)");
        }
    }
}

void connectMQTT() {
    mqtt.setServer(MQTT_SERVER, MQTT_PORT);
    mqtt.setCallback(mqttCallback);
    while (!mqtt.connected()) {
        Serial.print("MQTT 연결 중...");
        if (mqtt.connect("ESP32_Smartfarm")) {
            mqtt.subscribe(MQTT_CONTROL);
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

    ledServo.attach(SERVO_PIN);
    ledServo.write(SERVO_CENTER);
    Serial.println("서보모터 초기화 완료");

    Wire.begin(SHT31_SDA, SHT31_SCL);
    if (!sht31.begin(0x44)) {
        Serial.println("SHT31 센서를 찾을 수 없습니다!");
    } else {
        Serial.println("SHT31 연결 완료");
    }

    connectWiFi();
    connectMQTT();

    // 카메라 초기화 및 스트리밍 서버 시작
    if (initCamera()) {
        startStreamServer();
    }

    // 버퍼 초기화
    for (int i = 0; i < SAMPLE_COUNT; i++) {
        cdsBuffer[i] = getCDSBrightness(analogRead(CDS_PIN));
        phBuffer[i]  = getPH(analogRead(PH_PIN));
        tempSensor.requestTemperatures();
        float t = tempSensor.getTempCByIndex(0);
        tmpBuffer[i] = (t == DEVICE_DISCONNECTED_C) ? 0.0f : t;
        airTempBuffer[i] = sht31.readTemperature();
        humBuffer[i]     = sht31.readHumidity();
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

        float at = sht31.readTemperature();
        float ah = sht31.readHumidity();
        airTempBuffer[bufferIndex] = isnan(at) ? -999.0f : at;
        humBuffer[bufferIndex]     = isnan(ah) ? -999.0f : ah;

        bufferIndex = (bufferIndex + 1) % SAMPLE_COUNT;
        lastSampleTime = now;
    }

    // 5초마다 MQTT 전송
    if (now - lastSendTime >= SEND_INTERVAL) {
        float avgCDS     = filteredAverage(cdsBuffer, SAMPLE_COUNT);
        float avgPH      = filteredAverage(phBuffer,  SAMPLE_COUNT);
        float avgTemp    = filteredAverage(tmpBuffer, SAMPLE_COUNT);
        float avgAirTemp = filteredAverage(airTempBuffer, SAMPLE_COUNT);
        float avgHum     = filteredAverage(humBuffer, SAMPLE_COUNT);

        char payload[300];
        snprintf(payload, sizeof(payload),
            "{\"ts\":%lu,\"raw_cds\":%d,\"raw_ph\":%d,\"light\":%.1f,\"ph\":%.2f,\"water_temp\":%.2f,\"air_temp\":%.2f,\"humidity\":%.2f,\"led\":%s}",
            now, rawCDS, rawPH, avgCDS, avgPH, avgTemp, avgAirTemp, avgHum, ledState ? "true" : "false"
        );

        mqtt.publish(MQTT_TOPIC, payload);
        Serial.println(payload);

        lastSendTime = now;
    }
}
