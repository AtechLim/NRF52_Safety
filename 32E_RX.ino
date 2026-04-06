#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEBeacon.h>

#define SWAP_ENDIAN_16(x) (uint16_t)(((x) << 8) | ((x) >> 8))

#define TARGET_UUID      "00107746-5017-0000-0000-000000000000"
#define SENDER_MAJOR     1 

#define LED_RED          5
#define LED_GRN          6
#define LED_BLU          7     
#define LED_BUILTIN      8 

#define OUTPUT_S1        10     
#define LOW_BAT_S2       3      
#define LOW_BAT_THRESHOLD 3.5 

const uint16_t TARGET_MAJOR = SWAP_ENDIAN_16(SENDER_MAJOR);
const unsigned long KEEP_ALIVE_TIME = 2000; 

BLEScan* pBLEScan;
volatile unsigned long lastBeaconTime = 0; 
volatile float currentVoltage = 0.0;    
unsigned long lastClearTime = 0; 
volatile bool advReceived = false;

class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (!advertisedDevice.haveManufacturerData()) return;
    String strData = advertisedDevice.getManufacturerData();
    if (strData.length() < 25) return;

    if ((uint8_t)strData[0] != 0x4C || (uint8_t)strData[1] != 0x00) return;

    BLEBeacon oBeacon;
    oBeacon.setData(strData);

    if (String(oBeacon.getProximityUUID().toString().c_str()).equalsIgnoreCase(TARGET_UUID) &&
        oBeacon.getMajor() == TARGET_MAJOR) {
      
      uint16_t rawMinor = SWAP_ENDIAN_16(oBeacon.getMinor());
      if (rawMinor == 9999) {
        lastBeaconTime = 0;
        Serial.println("EMS");
      } else {
        lastBeaconTime = millis(); 
        currentVoltage = rawMinor / 100.0;
        advReceived = true; 
      }
    }
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
  Serial.println("Scan Start");
}

void loop() {
  unsigned long now = millis();

  // 1. 광고 수신 및 전압 기반 저전압 상태 즉시 업데이트
  if (advReceived) {
    digitalWrite(LED_BLU, HIGH);
    Serial.printf("%.2fV\n", currentVoltage);
    
    // 수신된 전압이 기준보다 낮으면 S2 ON, 정상 전압이면 S2 OFF
    // 신호가 끊기더라도 이 상태를 유지함
    if (currentVoltage > 0) {
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

  // 2. 1초마다 메모리 정리 (안정성 유지)
  if (now - lastClearTime > 1000) {
    pBLEScan->clearResults();
    lastClearTime = now;
  }

  // 3. 릴레이 제어 (KEEP_ALIVE_TIME 기준)
  if (lastBeaconTime != 0 && (now - lastBeaconTime < KEEP_ALIVE_TIME)) {
    if (digitalRead(OUTPUT_S1) == LOW) {
      digitalWrite(OUTPUT_S1, HIGH);
      digitalWrite(LED_BUILTIN, LOW);
      Serial.println("RY ON");
    }
  } 
  else {
    // 신호 유실 또는 EMS 수신 시
    if (digitalRead(OUTPUT_S1) == HIGH) {
      digitalWrite(OUTPUT_S1, LOW);
      digitalWrite(LED_BUILTIN, HIGH);
      Serial.println("RY OFF");
    }
    lastBeaconTime = 0;
  }

  delay(10); 
}
