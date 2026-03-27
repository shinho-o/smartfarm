# SmartFarm 프로젝트 전체 정보

## 1. 하드웨어

### ESP32
- **보드:** Freenove ESP32-S3 WROOM N8R8
- **Flash:** 8MB / **PSRAM:** 8MB
- **USB 포트:** UART (펌웨어 업로드용), USB-OTG (사용 안 함)
- **로컬 IP:** 192.168.0.72
- **버튼:** RST (리셋), BOOT (업로드용)

### 라즈베리파이
- **호스트명:** farmer1
- **사용자:** theysh0312
- **로컬 IP:** 192.168.0.58
- **Tailscale IP:** 100.110.26.123
- **OS:** Linux (Debian Trixie, aarch64)
- **접속:** `ssh theysh0312@farmer1.local`

### 노트북 (개발 PC)
- **OS:** Windows 11 Home 10.0.26200
- **Tailscale IP:** 100.127.14.12
- **프로젝트 경로:** `c:\smartfarm\smartfarm`

---

## 2. 센서 연결 (ESP32 핀 배치)

| 센서 | 핀 | 프로토콜 | 연결 방법 |
|------|-----|----------|-----------|
| CDS 광센서 | GPIO 1 (AO) | ADC | VCC→3.3V, GND→GND, AO→GPIO1, DO→안씀 |
| pH 센서 | GPIO 3 (PO) | ADC | V+→3.3V, G→GND, PO→GPIO3, TO→안씀, DO→안씀 |
| DS18B20 수온 | GPIO 2 (DATA) | OneWire | VCC→3.3V, GND→GND, DATA→GPIO2, **10kΩ 풀업저항 (3.3V↔DATA)** |
| SHT31 온습도 | SDA: GPIO 38, SCL: GPIO 39 | I2C (0x44) | VIN→3.3V, GND→GND, SDA(초록)→GPIO38, SCL(노랑)→GPIO39 |
| OV2640 카메라 | GPIO 4~18 (FPC) | FPC 리본케이블 | 보드 뒷면 커넥터에 직접 연결 |

### 주의사항
- **GPIO 47/48은 SHT31 I2C에서 안 됨** → GPIO 38/39로 변경
- DS18B20은 **10kΩ 풀업 저항 필수** (저항 1개만, 방향 무관)
- 모든 센서 전원: **3.3V** (5V 아님)
- 카메라가 GPIO 4~18 대부분 사용하므로 센서는 GPIO 1, 2, 3, 38, 39 사용
- SHT31 선 색: **노랑=SCL, 초록=SDA** (이 보드 기준)
- CDS, pH는 모듈이라 저항 불필요 (내장)

### 저항 색띠 읽기
- 갈색-검정-주황-금색 = **10kΩ (±5%)**
- 4.7kΩ이 이상적이지만 10kΩ도 동작함

---

## 3. 소프트웨어 & 개발환경

### PlatformIO
- **PC 경로:** `~/.platformio/penv/Scripts/pio.exe`
- **RPi 경로:** `~/.platformio/penv/bin/pio`
- **보드 설정:** `freenove_esp32_s3_wroom`
- **프레임워크:** Arduino

### 라이브러리 (platformio.ini)
```
lib_deps =
    milesburton/DallasTemperature @ ^3.11.0
    paulstoffregen/OneWire @ ^2.3.8
    knolleary/PubSubClient @ ^2.8
    adafruit/Adafruit SHT31 Library @ ^2.2.2
    espressif/esp32-camera @ ^2.0.0
```

### 빌드 & 업로드 (라즈베리파이에서)
```bash
cd ~/smartfarm/smartfarm
git pull
~/.platformio/penv/bin/pio run -t upload      # 빌드 + 업로드
~/.platformio/penv/bin/pio device monitor -b 115200  # 시리얼 모니터
```

### 시리얼 포트 잠김 에러 해결
```bash
sudo fuser -k /dev/ttyACM0
```

---

## 4. GitHub

- **리포지토리:** https://github.com/shinho-o/smartfarm
- **공개 여부:** Public
- **secrets.h:** `.gitignore`에 포함 (업로드 안 됨)
- **RPi 클론 경로:** `~/smartfarm/smartfarm`

### secrets.h 내용 (RPi에서 수동 생성 필요)
```c
#ifndef SECRETS_H
#define SECRETS_H

#define WIFI_SSID    "dotoribang"
#define WIFI_PASS    "hyunseo0312"
#define MQTT_SERVER  "192.168.0.58"
#define MQTT_PORT    1883

#endif
```

---

## 5. 데이터 파이프라인

```
ESP32 → MQTT (smartfarm/sensors) → Mosquitto → Telegraf → InfluxDB → Grafana
ESP32 → Camera (:81) → nginx (:82) → ngrok (공개 URL)
```

---

## 6. 서비스 정보 (라즈베리파이)

### Mosquitto (MQTT 브로커)
- **포트:** 1883
- **토픽:** `smartfarm/sensors`
- **상태 확인:** `sudo systemctl status mosquitto`
- **메시지 테스트:** `mosquitto_sub -t "smartfarm/sensors" -C 1`

### Telegraf (MQTT → InfluxDB)
- **설정 파일:** `/etc/telegraf/telegraf.conf`
- **재시작:** `sudo systemctl restart telegraf`
- **로그 확인:** `sudo journalctl -u telegraf --since "1 min ago" --no-pager`
- **설정 내용:**
```toml
[[inputs.mqtt_consumer]]
  servers = ["tcp://localhost:1883"]
  topics = ["smartfarm/sensors"]
  data_format = "json"
  json_time_key = ""
  name_override = "sensors"

[[outputs.influxdb_v2]]
  urls = ["http://localhost:8086"]
  token = "0IwjxaiQuv05Mv0P_FxFRoYKQ-rQzLC6_xIgOLEW-L6PL8sq1d1VMBM8UE44OVrK6fZls2AxWZ9yGgPZbeKCgA=="
  organization = "farmetry"
  bucket = "sensors"
```

### InfluxDB 2.x
- **URL:** http://farmer1.local:8086
- **Tailscale URL:** http://100.110.26.123:8086
- **Organization:** farmetry
- **Bucket:** sensors
- **API Token:** `0IwjxaiQuv05Mv0P_FxFRoYKQ-rQzLC6_xIgOLEW-L6PL8sq1d1VMBM8UE44OVrK6fZls2AxWZ9yGgPZbeKCgA==`
- **Measurement 이름:** smartfarm
- **필드:** light, ph, raw_cds, raw_ph, water_temp, air_temp, humidity

### Grafana OSS
- **URL:** http://farmer1.local:3000
- **Tailscale URL:** http://100.110.26.123:3000
- **로그인:** admin / 1234
- **비밀번호 초기화:** `sudo grafana-cli admin reset-admin-password 1234`
- **Data Source:** InfluxDB (Flux), organization: farmetry, bucket: sensors

### Grafana Flux 쿼리 예시
```flux
# 전체 센서
from(bucket: "sensors")
  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)
  |> filter(fn: (r) => r._measurement == "smartfarm")

# 공기 온도 + 습도만
from(bucket: "sensors")
  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)
  |> filter(fn: (r) => r._measurement == "smartfarm")
  |> filter(fn: (r) => r._field == "air_temp" or r._field == "humidity")

# 수온만
from(bucket: "sensors")
  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)
  |> filter(fn: (r) => r._measurement == "smartfarm")
  |> filter(fn: (r) => r._field == "water_temp")

# 사용 가능한 필드 목록 확인
from(bucket: "sensors")
  |> range(start: -1h)
  |> filter(fn: (r) => r._measurement == "smartfarm")
  |> keep(columns: ["_field"])
  |> distinct(column: "_field")
```

### Node-RED
- **URL:** http://farmer1.local:1880
- **상태:** 설치됨, 자동 시작 (현재 파이프라인에서는 미사용)

### nginx (카메라 프록시)
- **포트:** 82
- **설정:** `/etc/nginx/sites-available/camera`
- **역할:** RPi:82 → ESP32:81 (카메라 스트림 중계)
- **재시작:** `sudo nginx -t && sudo systemctl restart nginx`

---

## 7. 카메라 (ESP32 웹서버)

- **로컬 URL:** http://192.168.0.72:81/
- **nginx 프록시:** http://farmer1.local:82/
- **Tailscale:** http://100.110.26.123:82/

### 엔드포인트
| 경로 | 기능 |
|------|------|
| `/` | 웹 UI (ON/OFF/Capture 버튼) |
| `/stream` | MJPEG 실시간 스트리밍 |
| `/capture` | 정지 이미지 1장 (JPG) |

---

## 8. 외부 접속

### Tailscale (VPN)
- **RPi:** 100.110.26.123 (farmer1)
- **PC:** 100.127.14.12 (desktop-6nnivhn)
- **계정:** theysh01234@gmail.com
- Tailscale 앱이 설치된 기기에서만 접속 가능

### ngrok (공개 URL)
- **계정:** theysh01234@gmail.com
- **Authtoken:** `3BXUUp0pgyaU8OlN6zbNwXCWpCF_4DQXgd4TSTmaEBFtzr9U2`
- **현재 URL:** https://unsyncopated-unemotively-claris.ngrok-free.dev
- **주의:** ngrok 종료하면 URL 사라짐, 다시 켜면 새 URL 생성

### ngrok 실행 방법
```bash
ngrok http 82
```

### ngrok URL 확인 (터미널 잘릴 때)
```bash
curl -s http://127.0.0.1:4040/api/tunnels | grep -o '"public_url":"[^"]*"'
```

---

## 9. WiFi 정보
- **SSID:** dotoribang
- **Password:** hyunseo0312

---

## 10. JSON 페이로드 구조 (MQTT)

```json
{
  "ts": 12345,          // ESP32 uptime (ms)
  "raw_cds": 2048,      // CDS ADC raw value
  "raw_ph": 1500,       // pH ADC raw value
  "light": 75.3,        // 광량 (0-100%)
  "ph": 6.82,           // pH (0-14)
  "water_temp": 23.45,  // 수온 (°C)
  "air_temp": 25.30,    // 기온 (°C)
  "humidity": 60.50     // 습도 (%)
}
```

---

## 11. 유용한 명령어 모음

### 라즈베리파이
```bash
# 서비스 상태 확인
sudo systemctl status mosquitto
sudo systemctl status telegraf
sudo systemctl status influxdb
sudo systemctl status grafana-server
sudo systemctl status nginx

# 서비스 재시작
sudo systemctl restart telegraf
sudo systemctl restart grafana-server
sudo systemctl restart nginx

# MQTT 메시지 확인
mosquitto_sub -t "smartfarm/sensors" -C 1

# Telegraf 로그
sudo journalctl -u telegraf --since "1 min ago" --no-pager

# ESP32 업로드
cd ~/smartfarm/smartfarm
git pull
~/.platformio/penv/bin/pio run -t upload

# 시리얼 모니터
~/.platformio/penv/bin/pio device monitor -b 115200

# 시리얼 포트 잠김 해제
sudo fuser -k /dev/ttyACM0

# 저장 용량 확인
df -h

# 네트워크 장치 확인
arp -a

# ngrok 실행
ngrok http 82

# 라즈베리파이 끄기
sudo shutdown -h now
```

### PC (Windows)
```bash
# 빌드
cd c:/smartfarm/smartfarm
~/.platformio/penv/Scripts/pio.exe run

# GitHub 푸시
git add src/main.cpp platformio.ini
git commit -m "메시지"
git push origin main
```

---

## 12. 아직 안 한 것 (TODO)
- [ ] Grafana 대시보드 센서별 패널 만들기
- [ ] pH 캘리브레이션 (기준액으로 보정)
- [ ] 센서 노이즈 필터링 (중앙값 필터, 이상치 제거)
- [ ] Grafana에 카메라 임베드
- [ ] 알림 설정 (온도/습도 이상 시)
- [ ] ngrok 자동 실행 설정 (부팅 시)

---

## 13. 문서 파일 위치
- **배선도 HTML:** `docs/wiring-diagram.html`
- **회로도 SVG:** `docs/circuit-diagram.svg`
- **프로젝트 정보 (이 파일):** `docs/project-info.md`
