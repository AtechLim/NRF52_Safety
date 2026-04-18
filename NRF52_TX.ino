/*
 * NRF52 Safety System - NRF52 Transmitter (TX)
 * BLE iBeacon 송신 및 배터리 모니터링 시스템
 * 저전력 모드와 페어링 기능 포함
 */

#include <bluefruit.h>

// ===== 상수 정의 =====
// 비콘 설정
#define BEACON_MAJOR   0                          // 기본 Major 값
#define PAIRING_MARKER_MAJOR 65535                // 페어링 마커 Major 값

// 핀 정의
#define PIN_DRV5032FB  0                          // 드라이버 IC 핀 (웨이크업)
#define LED_RED        1                          // 빨간 LED 핀
#define PAIRING_BUTTON 2                          // 페어링 버튼 핀
#define PIN_WAKEUP_EXT 17                         // 외부 웨이크업 핀

// ===== 전역 변수 =====
// iBeacon UUID (16바이트)
uint8_t beaconUuid[] = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/**
 * 배터리 전압 읽기 함수
 * SAADC를 사용하여 내부 전압 센서로부터 배터리 전압을 측정
 * @return 배터리 전압 (단위: 0.01V, 예: 350 = 3.50V)
 */
uint16_t readBattery() {
  // SAADC 설정: 12비트 해상도, 1/6 게인, 내부 기준 전압
  NRF_SAADC->RESOLUTION = SAADC_RESOLUTION_VAL_12bit;
  NRF_SAADC->CH[0].CONFIG = (SAADC_CH_CONFIG_GAIN_Gain1_6 << SAADC_CH_CONFIG_GAIN_Pos) |
                            (SAADC_CH_CONFIG_REFSEL_Internal << SAADC_CH_CONFIG_REFSEL_Pos) |
                            (SAADC_CH_CONFIG_TACQ_10us << SAADC_CH_CONFIG_TACQ_Pos) |
                            (SAADC_CH_CONFIG_MODE_SE << SAADC_CH_CONFIG_MODE_Pos);

  // VDDH/5 입력 선택 (배터리 전압 측정용)
  NRF_SAADC->CH[0].PSELP = SAADC_CH_PSELP_PSELP_VDDHDIV5 << SAADC_CH_PSELP_PSELP_Pos;

  // SAADC 활성화 및 샘플링
  NRF_SAADC->ENABLE = SAADC_ENABLE_ENABLE_Enabled;
  static int16_t result = 0;
  NRF_SAADC->RESULT.PTR = (uint32_t)&result;
  NRF_SAADC->RESULT.MAXCNT = 1;

  // 변환 시작 및 완료 대기
  NRF_SAADC->TASKS_START = 1;
  while (!NRF_SAADC->EVENTS_STARTED);
  NRF_SAADC->TASKS_SAMPLE = 1;
  while (!NRF_SAADC->EVENTS_END);
  NRF_SAADC->TASKS_STOP = 1;
  while (!NRF_SAADC->EVENTS_STOPPED);

  // SAADC 비활성화
  NRF_SAADC->ENABLE = SAADC_ENABLE_ENABLE_Disabled;

  // 전압 계산: (ADC 값 * 1800mV) / 4096
  return (uint16_t)((max(0, result) * 1800L) / 4096);
}

/**
 * 추가 전력 소비 요소들 비활성화 함수
 * UART와 FPU를 비활성화하여 전력 절약
 */
void shutdown_extra_power() {
  // UART 비활성화
  NRF_UARTE0->ENABLE = 0;

  // FPU 비활성화
  NVIC_DisableIRQ(FPU_IRQn);
  __set_FPSCR(__get_FPSCR() & ~(0x0000009F));
  (void) __get_FPSCR();
  NVIC_ClearPendingIRQ(FPU_IRQn);
}

/**
 * GPIO 핀들을 기본 상태로 설정하여 전력 절약 함수
 * 사용하지 않는 GPIO 핀들을 입력으로 설정하고 풀다운 저항 적용
 * @note PIN_DRV5032FB, LED_RED, PIN_WAKEUP_EXT 핀은 제외
 */
void shutdown_gpio() {
  for (uint8_t i = 0; i < 22; i++) {
    if (i != PIN_DRV5032FB && i != LED_RED && i != PIN_WAKEUP_EXT && i != PAIRING_BUTTON) {
      nrf_gpio_cfg_default(i); // && i != EXT_VCC
    }
  }
}

/**
 * Arduino setup 함수
 * 시스템 초기화 및 메인 로직 실행
 */
void setup() {
  // DC-DC 컨버터 활성화로 전력 효율 향상
  NRF_POWER->DCDCEN = 1;

  // 추가 전력 소비 요소들 비활성화
  shutdown_extra_power();

  // GPIO 핀들 기본 상태로 설정하여 전력 절약
  shutdown_gpio();

  // GPIO 핀 모드 설정
  pinMode(PIN_DRV5032FB, INPUT);
  pinMode(PIN_WAKEUP_EXT, INPUT);
  pinMode(PAIRING_BUTTON, INPUT);
  pinMode(LED_RED, OUTPUT);
  digitalWrite(LED_RED, LOW);

  // 배터리 전압 측정
  uint16_t v_bat = readBattery(); 
  bool isPairingActive = false; // 현재 페어링 모드 송출 여부 확인용 플래그

  // 웨이크업 조건 확인: 드라이버 IC, 외부 신호, 또는 버튼 입력
  if (digitalRead(PIN_DRV5032FB) == LOW || digitalRead(PIN_WAKEUP_EXT) == LOW || digitalRead(PAIRING_BUTTON) == LOW) {
    // 웨이크업 표시: LED 깜빡임
    digitalWrite(LED_RED, HIGH);
    delay(10);
    digitalWrite(LED_RED, LOW);

    // BLE 초기화
    if (Bluefruit.begin()) {
      Bluefruit.autoConnLed(false);
      Bluefruit.setTxPower(4); // 0, 4, 8 dBm

      // 기본 비콘 설정 (배터리 전압을 Minor 값으로 사용)
      BLEBeacon beacon(beaconUuid, BEACON_MAJOR, v_bat, -59);

      // 페어링 모드 확인 및 비콘 설정
      if (digitalRead(PAIRING_BUTTON) == LOW) {
        isPairingActive = true;
        // 페어링 모드 광고: major=65535, minor=실제 Major
			   
        beacon = BLEBeacon(beaconUuid, PAIRING_MARKER_MAJOR, BEACON_MAJOR, -59);
			 
      }

      // BLE 광고 설정 및 시작
      Bluefruit.Advertising.setBeacon(beacon);
      Bluefruit.Advertising.setInterval(800, 1000);
      Bluefruit.Advertising.start(0);

      // 광고 중 웨이크업 신호 유지 대기
      while (digitalRead(PIN_DRV5032FB) == LOW || digitalRead(PIN_WAKEUP_EXT) == LOW || digitalRead(PAIRING_BUTTON) == LOW) {
        
        // [추가] 작동 중 페어링 버튼이 눌리면 페어링 비콘으로 전환
        if (digitalRead(PAIRING_BUTTON) == LOW && !isPairingActive) {
          isPairingActive = true;
          Bluefruit.Advertising.stop();
          BLEBeacon pairingBeacon(beaconUuid, PAIRING_MARKER_MAJOR, BEACON_MAJOR, -59);
          Bluefruit.Advertising.setBeacon(pairingBeacon);
          Bluefruit.Advertising.start(0);
        }
        // [추가] 버튼을 뗐을 때 센서 신호가 남아있다면 일반 광고로 복구
        else if (digitalRead(PAIRING_BUTTON) == HIGH && isPairingActive) {
          if (digitalRead(PIN_DRV5032FB) == LOW || digitalRead(PIN_WAKEUP_EXT) == LOW) {
            isPairingActive = false;
            Bluefruit.Advertising.stop();
            BLEBeacon normalBeacon(beaconUuid, BEACON_MAJOR, v_bat, -59);
            Bluefruit.Advertising.setBeacon(normalBeacon);
            Bluefruit.Advertising.start(0);
          }
        }

        delay(100);
	  
	   
      }

      // 광고 중지
      Bluefruit.Advertising.stop();

      // [수정] 페어링 모드가 아닐 때만(센서/외부 입력 종료 시) 종료 비콘 송출
      // 루프를 빠져나온 시점에 버튼이 눌려있지 않아야 함
      if (isPairingActive || digitalRead(PAIRING_BUTTON) == LOW) {
        // 페어링 버튼으로 인한 종료 시에는 9999를 보내지 않고 종료
      } else {
        BLEBeacon stopBeacon(beaconUuid, BEACON_MAJOR, 9999, -59);
        Bluefruit.Advertising.setBeacon(stopBeacon);
        Bluefruit.Advertising.setInterval(32, 32);
        Bluefruit.Advertising.start(0);
        delay(100); // 약 5~7회 송출될 시간 (20ms * 7)
        Bluefruit.Advertising.stop();
      }
    }
  }
  // GPIO 센스 입력 설정: 저전력 웨이크업용
  nrf_gpio_cfg_sense_input(digitalPinToPinName(PIN_DRV5032FB), NRF_GPIO_PIN_NOPULL, NRF_GPIO_PIN_SENSE_LOW);
  nrf_gpio_cfg_sense_input(digitalPinToPinName(PIN_WAKEUP_EXT), NRF_GPIO_PIN_NOPULL, NRF_GPIO_PIN_SENSE_LOW);
  nrf_gpio_cfg_sense_input(digitalPinToPinName(PAIRING_BUTTON), NRF_GPIO_PIN_PULLUP, NRF_GPIO_PIN_SENSE_LOW);

  // 추가 전력 소비 요소들 비활성화
  shutdown_extra_power();

  // 시스템 종료: 저전력 슬립 모드 진입
  NRF_POWER->SYSTEMOFF = 1;
}

/**
 * Arduino loop 함수
					 
 */
void loop() {
  // 시스템은 setup()에서 SYSTEMOFF 모드로 진입하므로 실행되지 않음
			
}
