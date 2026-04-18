/*
 * NRF52 Safety System - ESP32 Receiver (RX)
 * BLE iBeacon 수신 및 안전 제어 시스템
 * 페어링 기능 포함
 */

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <EEPROM.h>

// ===== 상수 정의 =====
// 페어링 관련
uint16_t SENDER_MAJOR = 0;                    // 페어링된 송신기 Major 값
#define PAIRING_BUTTON    4                   // 페어링 버튼 핀
#define PAIRING_MARKER_MAJOR 65535            // 페어링 광고용 Major 마커

// LED 핀 정의
#define LED_RED           5
#define LED_GRN           6
#define LED_BLU           7
#define LED_BUILTIN       8

// 출력 핀 정의
#define OUTPUT_S1         10                   // 메인 출력 신호 1
#define LOW_BAT_S2        3                    // 저전압 출력 신호 2

// 시스템 파라미터
#define LOW_BAT_THRESHOLD 4.0                  // 저전압 임계값 (V)
#define KEEP_ALIVE_TIME   3000                 // 비콘 수신 유지 시간 (ms)
#define PAIRING_HOLD_TIME 1000                 // 페어링 버튼 홀드 시간 (ms)

// ===== 전역 변수 =====
// 타겟 UUID (페어링 시 업데이트)
uint8_t TARGET_UUID[16] = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// BLE 스캔 객체
BLEScan* pBLEScan;

// 비콘 수신 상태 변수
volatile unsigned long lastBeaconTime = 0;     // 마지막 비콘 수신 시간
volatile float currentVoltage = 0.0;           // 현재 배터리 전압
volatile bool advReceived = false;             // 광고 수신 플래그
volatile bool emergencySignal = false;         // 비상 신호 플래그

// 타이머 변수
unsigned long lastClearTime = 0;               // BLE 스캔 결과 클리어 시간

// 페어링 상태 변수
bool pairingMode = false;                      // 페어링 모드 활성화 플래그
bool pairingCompleted = false;                 // 페어링 완료 플래그
unsigned long pairingBlinkStart = 0;           // 페어링 완료 깜빡임 시작 시간
int pairingBlinkCount = 0;                     // 페어링 완료 깜빡임 카운트

// ===== 함수 정의 =====

/**
 * RGB LED 제어 함수
 * @param r 빨강 LED (true=켜짐)
 * @param g 초록 LED (true=켜짐)
 * @param b 파랑 LED (true=켜짐)
 */
void setRGB(bool r, bool g, bool b) {
  digitalWrite(LED_RED, r);
  digitalWrite(LED_GRN, g);
  digitalWrite(LED_BLU, b);
}

/**
 * 모든 출력 신호 초기화 함수
 * @param reason 초기화 이유 (디버깅용)
 */
void resetOutputs(const char* reason) {
  if (digitalRead(OUTPUT_S1)) {
    digitalWrite(OUTPUT_S1, LOW);
    Serial.printf("Output Signal 1 OFF (%s)\n", reason);
  }
  if (digitalRead(LOW_BAT_S2)) {
    digitalWrite(LOW_BAT_S2, LOW);
    Serial.printf("Output Signal 2 OFF (%s)\n", reason);
  }
}

/**
 * BLE 광고 수신 콜백 클래스
 * iBeacon 광고를 처리하고 페어링 및 정상 신호를 구분
 */
class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    // 제조사 데이터 확인 (iBeacon 형식)
    if (!advertisedDevice.haveManufacturerData()) return;
    String md = advertisedDevice.getManufacturerData();

    // iBeacon 헤더 확인 (0x4C 0x00 0x02 0x15)
    if (md.length() < 25 ||
        (uint8_t)md[0] != 0x4C ||
        (uint8_t)md[1] != 0x00 ||
        (uint8_t)md[2] != 0x02 ||
        (uint8_t)md[3] != 0x15) return;

    // Major/Minor 값 추출
    uint16_t major = ((uint16_t)(uint8_t)md[20] << 8) | (uint8_t)md[21];
    uint16_t rawMinor = ((uint16_t)(uint8_t)md[22] << 8) | (uint8_t)md[23];

    // ===== 페어링 광고 처리 =====
    if (major == PAIRING_MARKER_MAJOR) {
      if (!pairingMode) return;  // 페어링 모드가 아니면 무시

      // 페어링 완료: minor에 실제 Major 값이 담겨 있음
      SENDER_MAJOR = rawMinor;
      memcpy(TARGET_UUID, &md[4], 16);  // UUID 복사
      EEPROM.put(0, TARGET_UUID);
      EEPROM.put(16, SENDER_MAJOR);
      EEPROM.commit();

      // 상태 업데이트
      pairingMode = false;
      pairingCompleted = true;
      pairingBlinkStart = millis();
      pairingBlinkCount = 0;

      Serial.printf("Paired UUID and Major=%u\n", SENDER_MAJOR);
      advReceived = false;
      return;
    }

    // ===== 정상 비콘 광고 처리 =====
    // UUID 일치 확인
    for (uint8_t i = 0; i < 16; i++) {
      if ((uint8_t)md[4 + i] != TARGET_UUID[i]) return;
    }

    // Major 일치 확인
    if (major != SENDER_MAJOR) return;

    // Minor 값에 따른 처리
    if (rawMinor == 9999) {
      emergencySignal = true;  // 비상 정지 신호
    } else {
      lastBeaconTime = millis();
      currentVoltage = rawMinor / 100.0;  // 배터리 전압 계산
      emergencySignal = false;
    }

    advReceived = true;
  }
};

/**
 * 시스템 초기화 함수
 */
void setup() {
  // 시리얼 통신 시작
  Serial.begin(115200);

  // EEPROM 초기화 및 저장된 페어링 정보 로드
  EEPROM.begin(512);
  uint8_t savedUUID[16];
  uint16_t savedMajor;
  EEPROM.get(0, savedUUID);
  EEPROM.get(16, savedMajor);

  // 유효한 페어링 정보가 있으면 로드
  if (savedMajor != 0xFFFF && savedMajor != 0) {
    memcpy(TARGET_UUID, savedUUID, 16);
    SENDER_MAJOR = savedMajor;
    Serial.printf("Loaded paired UUID and Major: %d\n", SENDER_MAJOR);
  }

  // 핀 모드 설정
  int outputPins[] = {OUTPUT_S1, LOW_BAT_S2, LED_RED, LED_GRN, LED_BLU, LED_BUILTIN};
  for (int pin : outputPins) {
    pinMode(pin, OUTPUT);
  }
  pinMode(PAIRING_BUTTON, INPUT_PULLUP);

  // 초기 LED 상태: 파란색 (대기)
  setRGB(0, 0, 1);

  // BLE 초기화 및 스캔 시작
  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks(), true);
  pBLEScan->start(0, nullptr, false);

  Serial.println("Beacon Scan Start");
}

/**
 * 메인 루프 함수
 * 버튼 입력, LED 상태, BLE 광고 처리
 */
void loop() {
  unsigned long now = millis();

  // ===== 정적 변수 선언 =====
  static bool lastS2State = false;           // 저전압 출력 이전 상태
  static bool s2FlashState = false;          // 저전압 깜빡임 상태
  static unsigned long lastS2ToggleTime = 0; // 저전압 깜빡임 토글 시간

  static unsigned long buttonPressStart = 0; // 버튼 누름 시작 시간
  static unsigned long lastBlinkTime = 0;    // LED 깜빡임 시간
  static int buttonLastState = HIGH;         // 버튼 이전 상태
  static bool buttonHandled = false;         // 버튼 처리 플래그

  // ===== 버튼 상태 읽기 =====
  int buttonState = digitalRead(PAIRING_BUTTON);

  // ===== 페어링 버튼 처리 =====
  if (buttonState == LOW && buttonLastState == HIGH) {
    // 버튼 누름 시작
    buttonPressStart = now;
    buttonHandled = false;

    if (pairingMode) {
      // 페어링 모드 중 버튼 누름: 모드 종료
      pairingMode = false;
      buttonHandled = true;
      Serial.println("Pairing mode exited");
      setRGB(0, 0, 1);  // 대기 상태로 복귀
    }
  }

  if (buttonState == LOW && buttonLastState == LOW && !pairingMode && !buttonHandled) {
    // 페어링 모드 진입 조건 확인
    if (now - buttonPressStart >= PAIRING_HOLD_TIME) {
      pairingMode = true;
      buttonHandled = true;
      lastBlinkTime = now;
      Serial.println("Pairing mode entered");
    }
  }

  if (buttonState == HIGH && buttonLastState == LOW) {
    // 버튼 뗌: 플래그 리셋
    buttonHandled = false;
    buttonPressStart = 0;
  }

  buttonLastState = buttonState;

  // ===== 페어링 완료 LED 깜빡임 처리 =====
  if (pairingCompleted) {
    if (now - pairingBlinkStart >= 500) {
      pairingBlinkStart = now;
      pairingBlinkCount++;

      if (pairingBlinkCount % 2 == 1) {
        setRGB(0, 0, 1);  // 파란색 켜기
      } else {
        setRGB(0, 0, 0);  // 끄기
      }

      // 3번 깜빡임 완료 (켜기+끄기 x 3)
      if (pairingBlinkCount >= 6) {
        pairingCompleted = false;
        setRGB(0, 0, 1);  // 대기 상태로 복귀
      }
    }
  }

  // ===== 페어링 모드 LED 깜빡임 =====
  if (pairingMode) {
    digitalWrite(LED_BUILTIN, HIGH);
    if (now - lastBlinkTime >= 500) {
      static bool blinkState = false;
      blinkState = !blinkState;
      setRGB(blinkState, blinkState, blinkState);  // 흰색 깜빡임
      lastBlinkTime = now;
    }
  } else if (!pairingCompleted) {
    // ===== 일반 상태 제어 =====
    // 1. 광고 수신 및 EMS 처리
    if (advReceived) {
      digitalWrite(LED_BUILTIN, LOW);

      if (emergencySignal) {
        // 비상 정지 신호 수신
        Serial.println("Beacon received: Emergency Stop Signal");
        resetOutputs("by EMS");

        // EMS 표시: 빨간색 3회 깜빡임
        for (int i = 0; i < 3; i++) {
          setRGB(1, 0, 0);
          delay(100);
          setRGB(0, 0, 0);
          delay(100);
        }

        emergencySignal = false;
        lastBeaconTime = 0;
        lastS2State = false;
      } else {
        // 정상 비콘 수신
        Serial.printf("Beacon received: %.2fV\n", currentVoltage);

        // 저전압 상태 확인
        bool currentS2State = (currentVoltage < LOW_BAT_THRESHOLD);
        if (currentS2State != lastS2State) {
          if (currentS2State) {
            Serial.println("Output Signal 2 (Low Bat) Mode ON");
          } else {
            Serial.println("Output Signal 2 OFF (Restored)");
            digitalWrite(LOW_BAT_S2, LOW);
          }
          lastS2State = currentS2State;
        }
      }

      delay(10);
      digitalWrite(LED_BUILTIN, HIGH);
      advReceived = false;
    }

    // 2. 가동 중 상태 제어
    if (lastBeaconTime != 0 && (now - lastBeaconTime < KEEP_ALIVE_TIME)) {
      // 메인 출력 ON
      if (!digitalRead(OUTPUT_S1)) {
        digitalWrite(OUTPUT_S1, HIGH);
        Serial.println("Output Signal 1 ON");
      }

      // 저전압 표시
      if (lastS2State) {
        if (now - lastS2ToggleTime >= 1000) {
          s2FlashState = !s2FlashState;
          digitalWrite(LOW_BAT_S2, s2FlashState);
          lastS2ToggleTime = now;
        }
        // 저전압 시 노란색 깜빡임
        if (s2FlashState) {
          setRGB(1, 1, 0);
        } else {
          setRGB(0, 1, 0);
        }
      } else {
        digitalWrite(LOW_BAT_S2, LOW);
        setRGB(0, 1, 0);  // 정상: 초록색
      }
    }

    // 3. 타임아웃/정지 상태 제어
    else {
      if (digitalRead(OUTPUT_S1)) {
        resetOutputs("Timeout");
        // 타임아웃 표시: 자주색 3회 깜빡임
        for (int i = 0; i < 3; i++) {
          setRGB(1, 0, 1);
          delay(100);
          setRGB(0, 0, 0);
          delay(100);
        }
      }

      setRGB(0, 0, 1);  // 대기: 파란색
      lastBeaconTime = 0;
      lastS2State = false;
    }
  }

  // ===== BLE 스캔 결과 주기적 클리어 =====
  if (now - lastClearTime > 10000) {
    pBLEScan->clearResults();
    lastClearTime = now;
  }

  delay(10);
}

/*
 * ===== 코드 설명 =====
 *
 * 이 코드는 ESP32를 사용하여 NRF52 송신기로부터 BLE iBeacon 신호를 수신하고,
 * 안전 제어 시스템을 구현합니다.
 *
 * 주요 기능:
 * 1. BLE iBeacon 스캔 및 수신
 * 2. 페어링 모드 (버튼으로 진입/종료)
 * 3. 배터리 전압 모니터링 및 저전압 경고
 * 4. 비상 정지 신호 (EMS) 처리
 * 5. RGB LED를 통한 상태 표시
 * 6. 출력 신호 제어 (S1, S2)
 *
 * LED 색상 코드:
 * - 파란색: 대기 상태
 * - 초록색: 정상 작동
 * - 노란색: 저전압 경고
 * - 빨간색: 비상 정지
 * - 자주색: 타임아웃
 * - 흰색: 페어링 모드
 *
 * 버튼 동작:
 * - 1초 이상 누름: 페어링 모드 진입 (흰색 깜빡임)
 * - 페어링 모드 중 누름: 모드 종료
 *
 * 페어링 과정:
 * 1. RX 페어링 모드 진입
 * 2. TX 페어링 버튼 누름 → 페어링 광고 송출
 * 3. RX 페어링 광고 수신 → UUID/Major 저장
 * 4. 파란색 3회 깜빡임으로 완료 표시
 */
