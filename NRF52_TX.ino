#include <bluefruit.h>

#define BEACON_MAJOR 1

#define PIN_DRV5032FB 0
#define LED_RED 1
#define PIN_WAKEUP_EXT 17

uint8_t beaconUuid[] = {
  0x00, 0x10, 0x77, 0x46, 0x50, 0x17, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

uint16_t readBattery() {
  NRF_SAADC->RESOLUTION = SAADC_RESOLUTION_VAL_12bit; 
  NRF_SAADC->CH[0].CONFIG = (SAADC_CH_CONFIG_GAIN_Gain1_6 << SAADC_CH_CONFIG_GAIN_Pos) |
                            (SAADC_CH_CONFIG_REFSEL_Internal << SAADC_CH_CONFIG_REFSEL_Pos) |
                            (SAADC_CH_CONFIG_TACQ_10us << SAADC_CH_CONFIG_TACQ_Pos) |
                            (SAADC_CH_CONFIG_MODE_SE << SAADC_CH_CONFIG_MODE_Pos);
  NRF_SAADC->CH[0].PSELP = SAADC_CH_PSELP_PSELP_VDDHDIV5 << SAADC_CH_PSELP_PSELP_Pos;
  NRF_SAADC->ENABLE = 1;

  static int16_t result = 0; 
  NRF_SAADC->RESULT.PTR = (uint32_t)&result;
  NRF_SAADC->RESULT.MAXCNT = 1;
  NRF_SAADC->TASKS_START = 1;
  while (!NRF_SAADC->EVENTS_STARTED);
  NRF_SAADC->TASKS_SAMPLE = 1;
  while (!NRF_SAADC->EVENTS_END);
  NRF_SAADC->TASKS_STOP = 1;
  while (!NRF_SAADC->EVENTS_STOPPED);
  NRF_SAADC->ENABLE = 0;

  return (uint16_t)((max(0, result) * 1800L) / 4096);
}

void shutdown_extra_power() {
  NRF_UARTE0->ENABLE = 0;

  NVIC_DisableIRQ(FPU_IRQn);
  __set_FPSCR(__get_FPSCR() & ~(0x0000009F));
  (void) __get_FPSCR();
  NVIC_ClearPendingIRQ(FPU_IRQn);
}

void shutdown_gpio() {
  for (uint8_t i = 0; i < 22; i++) {
    if (i != PIN_DRV5032FB && i != LED_RED && i != PIN_WAKEUP_EXT) nrf_gpio_cfg_default(i); // && i != EXT_VCC
  }
}

void setup() {

  NRF_POWER->DCDCEN = 1;
  shutdown_extra_power();
  shutdown_gpio(); //0.02ma 정도 줄어듬 
  
  pinMode(PIN_DRV5032FB, INPUT);
  pinMode(PIN_WAKEUP_EXT, INPUT);
  pinMode(LED_RED, OUTPUT);
  digitalWrite(LED_RED, LOW);
  //pinMode(EXT_VCC, OUTPUT);
  //digitalWrite(EXT_VCC, LOW);

  uint16_t v_bat = readBattery(); 

  if (digitalRead(PIN_DRV5032FB) == LOW || digitalRead(PIN_WAKEUP_EXT) == LOW) {
      digitalWrite(LED_RED, HIGH);
      delay(10);
      digitalWrite(LED_RED, LOW);

    if (Bluefruit.begin()) {
      Bluefruit.autoConnLed(false);
      Bluefruit.setTxPower(0); // 0,4,8
      BLEBeacon beacon(beaconUuid, BEACON_MAJOR, v_bat, -59);
      Bluefruit.Advertising.setBeacon(beacon);
      Bluefruit.Advertising.setInterval(800, 1000);
      Bluefruit.Advertising.start(0);
      //NRF_POWER->DCDCEN = 1;  활성화 하면 3ma 로 증가함
      //shutdown_extra_power();  전류 차이 없음 
      digitalWrite(LED_RED, HIGH);
      delay(10);
      digitalWrite(LED_RED, LOW);

      while (digitalRead(PIN_DRV5032FB) == LOW || digitalRead(PIN_WAKEUP_EXT) == LOW) {
        //sd_app_evt_wait(); 활성화 하면 0.3ma -> 0.7ma 로 전류 상승
        delay(100);
        //suspendLoop();
        //waitForEvent();								
      }

      Bluefruit.Advertising.stop();
      BLEBeacon stopBeacon(beaconUuid, BEACON_MAJOR, 9999, -59);
      Bluefruit.Advertising.setBeacon(stopBeacon);
	    Bluefruit.Advertising.setInterval(32, 32);
      Bluefruit.Advertising.start(0);
      delay(100); // 약 5~7회 송출될 시간 (20ms * 7)
      Bluefruit.Advertising.stop();
    }
  }
  nrf_gpio_cfg_sense_input(digitalPinToPinName(PIN_DRV5032FB), NRF_GPIO_PIN_NOPULL, NRF_GPIO_PIN_SENSE_LOW);
  nrf_gpio_cfg_sense_input(digitalPinToPinName(PIN_WAKEUP_EXT), NRF_GPIO_PIN_NOPULL, NRF_GPIO_PIN_SENSE_LOW);
  shutdown_extra_power();
  NRF_POWER->SYSTEMOFF = 1;
}

void loop() {

}
