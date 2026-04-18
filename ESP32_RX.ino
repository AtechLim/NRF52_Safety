#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

#define SENDER_MAJOR      1 
#define LED_RED           5
#define LED_GRN           6
#define LED_BLU           7     
#define LED_BUILTIN       8 
#define OUTPUT_S1         10     
#define LOW_BAT_S2        3 
#define LOW_BAT_THRESHOLD 3.5 
#define KEEP_ALIVE_TIME   3000 

const uint8_t TARGET_UUID[16] = { 0x00, 0x10, 0x77, 0x46, 0x50, 0x17, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

BLEScan* pBLEScan;
volatile unsigned long lastBeaconTime = 0; 
volatile float currentVoltage = 0.0;    
volatile bool advReceived = false, emergencySignal = false;
unsigned long lastClearTime = 0;

void setRGB(bool r, bool g, bool b) {
  digitalWrite(LED_RED, r); digitalWrite(LED_GRN, g); digitalWrite(LED_BLU, b);
}

// 모든 출력 및 상태 초기화
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

class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (!advertisedDevice.haveManufacturerData()) return;
    String md = advertisedDevice.getManufacturerData();
    if (md.length() < 25 || (uint8_t)md[0] != 0x4C || (uint8_t)md[1] != 0x00 || (uint8_t)md[2] != 0x02 || (uint8_t)md[3] != 0x15) return;

    for (uint8_t i = 0; i < 16; i++) if ((uint8_t)md[4 + i] != TARGET_UUID[i]) return;

    uint16_t major = ((uint16_t)(uint8_t)md[20] << 8) | (uint8_t)md[21];
    if (major != SENDER_MAJOR) return;

    uint16_t rawMinor = ((uint16_t)(uint8_t)md[22] << 8) | (uint8_t)md[23];
    if (rawMinor == 9999) emergencySignal = true;
    else {
      lastBeaconTime = millis();
      currentVoltage = rawMinor / 100.0;
      emergencySignal = false;
    }
    advReceived = true;
  }
};

void setup() {
  Serial.begin(115200);
  int pins[] = {OUTPUT_S1, LOW_BAT_S2, LED_RED, LED_GRN, LED_BLU, LED_BUILTIN};
  for (int p : pins) pinMode(p, OUTPUT);
  setRGB(0, 0, 1); 
  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks(), true);
  pBLEScan->start(0, nullptr, false); 
  Serial.println("Beacon Scan Start");
}

void loop() {
  unsigned long now = millis();
  static bool lastS2State = false, s2FlashState = false;
  static unsigned long lastS2ToggleTime = 0;

  // 1. 광고 수신 및 EMS 처리
  if (advReceived) {
    digitalWrite(LED_BUILTIN, LOW); 
    if (emergencySignal) {
      Serial.println("Beacon received: Emergency Stop Signal");
      resetOutputs("by EMS");
      // EMS 빨간색 깜빡임 (3회)
      for(int i=0; i<3; i++) { setRGB(1, 0, 0); delay(100); setRGB(0, 0, 0); delay(100); }
      emergencySignal = false; lastBeaconTime = 0; lastS2State = false;
    } else {
      Serial.printf("Beacon received: %.2fV\n", currentVoltage);
      bool currentS2State = (currentVoltage < LOW_BAT_THRESHOLD);
      if (currentS2State != lastS2State) {
        if (currentS2State) Serial.println("Output Signal 2 (Low Bat) Mode ON");
        else { Serial.println("Output Signal 2 OFF (Restored)"); digitalWrite(LOW_BAT_S2, LOW); }
        lastS2State = currentS2State;
      }
    }
    delay(10);
    digitalWrite(LED_BUILTIN, HIGH); advReceived = false;
  }

  // 2. 가동 중 상태 제어
  if (lastBeaconTime != 0 && (now - lastBeaconTime < KEEP_ALIVE_TIME)) {
    if (!digitalRead(OUTPUT_S1)) { digitalWrite(OUTPUT_S1, HIGH); Serial.println("Output Signal 1 ON"); }
    
    if (lastS2State) {
      if (now - lastS2ToggleTime >= 1000) { s2FlashState = !s2FlashState; digitalWrite(LOW_BAT_S2, s2FlashState); lastS2ToggleTime = now; }
      if (s2FlashState) setRGB(1, 1, 0); // 저전압 시 노란색
      else setRGB(0, 1, 0);             // 그 외 초록색
    } else {
      digitalWrite(LOW_BAT_S2, LOW); setRGB(0, 1, 0);
    }
  } 
  // 3. 타임아웃/정지 상태 제어
  else {
    if (digitalRead(OUTPUT_S1)) { // S1이 켜져 있다가 꺼지는 순간 감지
      resetOutputs("Timeout");
      // 타임아웃 자주색 깜빡임 (3회)
      for(int i=0; i<3; i++) { setRGB(1, 0, 1); delay(100); setRGB(0, 0, 0); delay(100); }
    }
    setRGB(0, 0, 1); // 대기 중 파란색 복귀
    lastBeaconTime = 0; lastS2State = false;
  }

  if (now - lastClearTime > 10000) { pBLEScan->clearResults(); lastClearTime = now; }
  delay(10);
}
