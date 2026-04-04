#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEBeacon.h>

#define TARGET_UUID     "00107746-5017-0000-0000-000000000000"
#define TARGET_MAJOR    256      // Must match sender's BEACON_MAJOR (Endian-adjusted)
#define OUTPUT_PIN      33       // Relay control pin
#define LED_BUILTIN     2        // Built-in status LED
#define LOW_BAT_LED     32        // Low battery warning LED pin
#define LOW_BAT_THRESHOLD 3.5    // Low battery threshold (3.5V)

BLEScan* pBLEScan;
volatile unsigned long lastBeaconTime = 0; 
volatile float currentVoltage = 0.0;    
const unsigned long KEEP_ALIVE_TIME = 2500; // Relay ON duration after last signal

// Callback class for handling discovered BLE devices
class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    // Check if manufacturer data exists
    if (!advertisedDevice.haveManufacturerData()) return;

    String strData = advertisedDevice.getManufacturerData();
    if (strData.length() < 25) return;

    // Check Apple iBeacon Prefix (0x4C 0x00)
    if ((uint8_t)strData[0] != 0x4C || (uint8_t)strData[1] != 0x00) return;

    BLEBeacon oBeacon;
    oBeacon.setData(strData);

    // Verify UUID and Major ID
    if (String(oBeacon.getProximityUUID().toString().c_str()).equalsIgnoreCase(TARGET_UUID) &&
        oBeacon.getMajor() == TARGET_MAJOR) {
      
      lastBeaconTime = millis(); // Update last found time

      // Extract Minor value (Voltage*100) directly to avoid Endian issues
      // Minor bytes are at index 22 and 23
      uint16_t rawMinor = (uint8_t)strData[22] << 8 | (uint8_t)strData[23];
      currentVoltage = rawMinor / 100.0; // Restore to voltage (e.g., 425 -> 4.25V)
    }
  }
};

void setup() {
  Serial.begin(115200);

  // Initialize GPIO pins
  pinMode(OUTPUT_PIN, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(LOW_BAT_LED, OUTPUT);
  
  digitalWrite(OUTPUT_PIN, LOW);
  digitalWrite(LED_BUILTIN, LOW);
  digitalWrite(LOW_BAT_LED, LOW);

  // Initialize BLE Scan
  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan();
  
  // Set callback and allow duplicate data for real-time monitoring
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks(), true);
  
  pBLEScan->setActiveScan(true); 
  pBLEScan->setInterval(300); 
  pBLEScan->setWindow(299);   

  Serial.println(">>> 2026 RX Started - Monitoring Battery Voltage <<<");
}

void loop() {
  // Start BLE Scan (1.2 seconds duration)
  pBLEScan->start(1.2, false);
  pBLEScan->stop(); // Ensure scan is properly stopped

  unsigned long now = millis();

  // 1. Check Beacon Presence and Control Relay
  if (now - lastBeaconTime < KEEP_ALIVE_TIME && lastBeaconTime != 0) {
    // Beacon in range
    if (digitalRead(OUTPUT_PIN) == LOW) {
      digitalWrite(OUTPUT_PIN, HIGH);
      digitalWrite(LED_BUILTIN, HIGH);
      Serial.println(">>> Beacon Found - Relay ON");
    }

    // 2. Monitoring Voltage & Warning LED
    Serial.printf("Current Battery: %.2f V\n", currentVoltage);

    if (currentVoltage > 0 && currentVoltage < LOW_BAT_THRESHOLD) {
      digitalWrite(LOW_BAT_LED, HIGH); // Alert on low battery
    } else {
      digitalWrite(LOW_BAT_LED, LOW);  // Turn off alert if normal
    }

  } else {
    // 3. Signal Lost
    if (digitalRead(OUTPUT_PIN) == HIGH) {
      digitalWrite(OUTPUT_PIN, LOW);
      digitalWrite(LED_BUILTIN, LOW);
      digitalWrite(LOW_BAT_LED, LOW);
      Serial.println(">>> Beacon Lost - Relay OFF");
      currentVoltage = 0.0;
    }
  }

  delay(10); 
}
