#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEBeacon.h>

#define SWAP_ENDIAN_16(x) (uint16_t)(((x) << 8) | ((x) >> 8))
#define SENDER_MAJOR     1 

#define LED_RED          5
#define LED_GRN          6
#define LED_BLU          7     
#define LED_BUILTIN      8 

#define OUTPUT_S1        10     
#define LOW_BAT_S2       3      
#define LOW_BAT_THRESHOLD 3.5 

const uint8_t TARGET_UUID[16] = {
  0x00, 0x10, 0x77, 0x46, 0x50, 0x17, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

const uint16_t TARGET_MAJOR = SENDER_MAJOR;  // iBeacon은 이미 big-endian 형식
const unsigned long KEEP_ALIVE_TIME = 3000; 

BLEScan* pBLEScan;
volatile unsigned long lastBeaconTime = 0; 
volatile float currentVoltage = 0.0;    
volatile bool advReceived = false;
volatile bool emergencySignal = false;
unsigned long lastClearTime = 0;

class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (!advertisedDevice.haveManufacturerData()) return;

    String md = advertisedDevice.getManufacturerData();
    size_t len = md.length();
    if (len < 25) return;
    if ((uint8_t)md[0] != 0x4C || (uint8_t)md[1] != 0x00) return;
    if ((uint8_t)md[2] != 0x02 || (uint8_t)md[3] != 0x15) return;

    // UUID 비교 (offset 4-19, 16 bytes)
    for (uint8_t i = 0; i < 16; i++) {
      if ((uint8_t)md[4 + i] != TARGET_UUID[i]) {
        Serial.printf("UUID mismatch at [%d]: 0x%02X vs 0x%02X\n", i, (uint8_t)md[4 + i], TARGET_UUID[i]);
        return;
      }
    }
    //Serial.println("UUID matched!");

    // Major 비교 (offset 20-21, big-endian)
    uint16_t major = ((uint16_t)(uint8_t)md[20] << 8) | (uint8_t)md[21];
    //Serial.printf("Major: 0x%04X vs 0x%04X (TARGET)\n", major, TARGET_MAJOR);
    if (major != TARGET_MAJOR) return;

    // Minor 값 읽기 (offset 22-23, big-endian)
    uint16_t rawMinor = ((uint16_t)(uint8_t)md[22] << 8) | (uint8_t)md[23];
    //Serial.printf("Minor: %d\n", rawMinor);
    
    if (rawMinor == 9999) {
      lastBeaconTime = 0;
      emergencySignal = true;
      advReceived = true;
      return;
    }

    lastBeaconTime = millis();
    currentVoltage = rawMinor / 100.0;
    emergencySignal = false;
    advReceived = true;
  }
};

void setup() {
  Serial.begin(115200);

  pinMode(OUTPUT_S1, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_GRN, OUTPUT);
  pinMode(LED_BLU, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(LOW_BAT_S2, OUTPUT);
  
  digitalWrite(OUTPUT_S1, LOW);   
  digitalWrite(LED_BLU, LOW);     
  digitalWrite(LED_BUILTIN, HIGH);
  digitalWrite(LOW_BAT_S2, LOW);  

  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks(), true);
  pBLEScan->setActiveScan(false); 
  pBLEScan->setInterval(100);    
  pBLEScan->setWindow(99);       

  pBLEScan->start(0, nullptr, false); 
  Serial.println("Beacon Scan Start");
}

void loop() {
  unsigned long now = millis();

  // 1. 광고 수신 및 전압 기반 저전압 상태 즉시 업데이트
  if (advReceived) {
    digitalWrite(LED_BLU, HIGH);
    
    if (emergencySignal) {
      Serial.println("Beacon received: Emergency Stop Signal");
    } else if (currentVoltage > 0) {
      Serial.printf("Beacon received: %.2fV\n", currentVoltage);
      // 수신된 전압이 기준보다 낮으면 S2 ON, 정상 전압이면 S2 OFF
      if (currentVoltage < LOW_BAT_THRESHOLD) {
        digitalWrite(LOW_BAT_S2, HIGH); // 저전압 발생 시 켬
      } else {
        digitalWrite(LOW_BAT_S2, LOW);  // 정상 전압 들어오면 끔
      }
    }

    delay(20);
    digitalWrite(LED_BLU, LOW);
    advReceived = false;
  }

  // 2. 10초마다 스캔 결과 메모리 정리 (clearResults 호출 빈도 감소)
  if (now - lastClearTime > 10000) {
    pBLEScan->clearResults();
    lastClearTime = now;
  }

  // 3. 릴레이 제어 (KEEP_ALIVE_TIME 기준)
  if (lastBeaconTime != 0 && (now - lastBeaconTime < KEEP_ALIVE_TIME)) {
    if (digitalRead(OUTPUT_S1) == LOW) {
      digitalWrite(OUTPUT_S1, HIGH);
      digitalWrite(LED_BUILTIN, LOW);
      Serial.println("Output Signal 1 ON");
    }
  } 
  else {
    // 신호 유실 또는 EMS 수신 시
    if (digitalRead(OUTPUT_S1) == HIGH) {
      digitalWrite(OUTPUT_S1, LOW);
      digitalWrite(LED_BUILTIN, HIGH);
      Serial.println("Output Signal 1 OFF");
    }
    lastBeaconTime = 0;
  }

  delay(10); 

  // 메모리 사용량 모니터링 (10초마다 출력)
  static unsigned long lastMemoryCheck = 0;
  if (millis() - lastMemoryCheck > 10000) {
    Serial.printf("Free heap: %d bytes, Total heap: %d bytes\n", ESP.getFreeHeap(), ESP.getHeapSize());
    Serial.printf("Min free heap: %d bytes\n", ESP.getMinFreeHeap());
    Serial.printf("Max alloc heap: %d bytes\n", ESP.getMaxAllocHeap());
    #ifdef BOARD_HAS_PSRAM
      Serial.printf("PSRAM free: %d bytes, Total PSRAM: %d bytes\n", ESP.getFreePsram(), ESP.getPsramSize());
    #endif
    // 힙 무결성 체크 (선택적, 성능 영향 있음)
    if (heap_caps_check_integrity_all(true)) {
      Serial.println("Heap integrity: OK");
    } else {
      Serial.println("Heap integrity: CORRUPTED");
    }
    lastMemoryCheck = millis();
  }
}
