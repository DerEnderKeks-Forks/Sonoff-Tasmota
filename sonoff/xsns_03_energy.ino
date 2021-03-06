/*
  xsns_03_energy.ino - HLW8012 (Sonoff Pow) and PZEM004T energy sensor support for Sonoff-Tasmota

  Copyright (C) 2017  Theo Arends

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#define USE_ENERGY_SENSOR

#ifdef USE_ENERGY_SENSOR
/*********************************************************************************************\
 * HLW8012 and PZEM004T - Energy
\*********************************************************************************************/

#define FEATURE_POWER_LIMIT  true

enum EnergyHardware { ENERGY_NONE, ENERGY_HLW8012, ENERGY_PZEM004T };

enum EnergyCommands {
  CMND_POWERLOW, CMND_POWERHIGH, CMND_VOLTAGELOW, CMND_VOLTAGEHIGH, CMND_CURRENTLOW, CMND_CURRENTHIGH,
  CMND_HLWPCAL, CMND_HLWPSET, CMND_HLWUCAL, CMND_HLWUSET, CMND_HLWICAL, CMND_HLWISET,
  CMND_ENERGYRESET, CMND_MAXENERGY, CMND_MAXENERGYSTART,
  CMND_MAXPOWER, CMND_MAXPOWERHOLD, CMND_MAXPOWERWINDOW,
  CMND_SAFEPOWER, CMND_SAFEPOWERHOLD, CMND_SAFEPOWERWINDOW };
const char kEnergyCommands[] PROGMEM =
  D_CMND_POWERLOW "|" D_CMND_POWERHIGH "|" D_CMND_VOLTAGELOW "|" D_CMND_VOLTAGEHIGH "|" D_CMND_CURRENTLOW "|" D_CMND_CURRENTHIGH "|"
  D_CMND_HLWPCAL "|" D_CMND_HLWPSET "|" D_CMND_HLWUCAL "|" D_CMND_HLWUSET "|" D_CMND_HLWICAL "|" D_CMND_HLWISET "|"
  D_CMND_ENERGYRESET "|" D_CMND_MAXENERGY "|" D_CMND_MAXENERGYSTART "|"
  D_CMND_MAXPOWER "|" D_CMND_MAXPOWERHOLD "|" D_CMND_MAXPOWERWINDOW "|"
  D_CMND_SAFEPOWER "|" D_CMND_SAFEPOWERHOLD "|"  D_CMND_SAFEPOWERWINDOW ;

bool energy_power_factor_ready = false;
float energy_voltage = 0;         // 123.1 V
float energy_current = 0;         // 123.123 A
float energy_power = 0;           // 123.1 W
float energy_power_factor = 0;    // 0.12
float energy_daily = 0;           // 12.123 kWh
float energy_total = 0;           // 12345.12345 kWh
float energy_start = 0;           // 12345.12345 kWh total from yesterday
unsigned long energy_kWhtoday;    // 1212312345 Wh * 10^-5 (deca micro Watt hours) - 5763924 = 0.05763924 kWh = 0.058 kWh = energy_daily
unsigned long energy_period = 0;  //

byte energy_min_power_flag = 0;
byte energy_max_power_flag = 0;
byte energy_min_voltage_flag = 0;
byte energy_max_voltage_flag = 0;
byte energy_min_current_flag = 0;
byte energy_max_current_flag = 0;

byte energy_power_steady_cntr = 8;  // Allow for power on stabilization
byte energy_max_energy_state = 0;

#if FEATURE_POWER_LIMIT
byte energy_mplr_counter = 0;
uint16_t energy_mplh_counter = 0;
uint16_t energy_mplw_counter = 0;
#endif  // FEATURE_POWER_LIMIT

byte energy_startup = 1;
byte energy_fifth_second = 0;
Ticker ticker_energy;

/*********************************************************************************************\
 * HLW8012 - Energy
 *
 * Based on Source: Shenzhen Heli Technology Co., Ltd
\*********************************************************************************************/

#define HLW_PREF            10000    // 1000.0W
#define HLW_UREF             2200    // 220.0V
#define HLW_IREF             4545    // 4.545A

#define HLW_POWER_PROBE_TIME   10    // Number of seconds to probe for power before deciding none used

byte hlw_select_ui_flag;
byte hlw_load_off;
byte hlw_cf1_timer;
unsigned long hlw_cf_pulse_length;
unsigned long hlw_cf_pulse_last_time;
unsigned long hlw_cf1_pulse_length;
unsigned long hlw_cf1_pulse_last_time;
unsigned long hlw_cf1_summed_pulse_length;
unsigned long hlw_cf1_pulse_counter;
unsigned long hlw_cf1_voltage_pulse_length;
unsigned long hlw_cf1_current_pulse_length;
unsigned long hlw_energy_period_counter;

unsigned long hlw_cf1_voltage_max_pulse_counter;
unsigned long hlw_cf1_current_max_pulse_counter;

#ifndef USE_WS2812_DMA  // Collides with Neopixelbus but solves exception
void HlwCfInterrupt() ICACHE_RAM_ATTR;
void HlwCf1Interrupt() ICACHE_RAM_ATTR;
#endif  // USE_WS2812_DMA

void HlwCfInterrupt()  // Service Power
{
  unsigned long us = micros();

  if (hlw_load_off) {  // Restart plen measurement
    hlw_cf_pulse_last_time = us;
    hlw_load_off = 0;
  } else {
    hlw_cf_pulse_length = us - hlw_cf_pulse_last_time;
    hlw_cf_pulse_last_time = us;
    hlw_energy_period_counter++;
  }
}

void HlwCf1Interrupt()  // Service Voltage and Current
{
  unsigned long us = micros();

  hlw_cf1_pulse_length = us - hlw_cf1_pulse_last_time;
  hlw_cf1_pulse_last_time = us;
  if ((hlw_cf1_timer > 2) && (hlw_cf1_timer < 8)) {  // Allow for 300 mSec set-up time and measure for up to 1 second
    hlw_cf1_summed_pulse_length += hlw_cf1_pulse_length;
    hlw_cf1_pulse_counter++;
    if (10 == hlw_cf1_pulse_counter) {
      hlw_cf1_timer = 8;  // We need up to ten samples within 1 second (low current could take up to 0.3 second)
    }
  }
}

void HlwEverySecond()
{
  unsigned long hlw_len;
  unsigned long hlw_temp;

  if (hlw_energy_period_counter) {
    hlw_len = 10000 / hlw_energy_period_counter;
    hlw_energy_period_counter = 0;
    if (hlw_len) {
      hlw_temp = ((HLW_PREF * Settings.hlw_power_calibration) / hlw_len) / 36;
      energy_kWhtoday += hlw_temp;
      RtcSettings.energy_kWhtoday = energy_kWhtoday;

      energy_total = (float)(RtcSettings.energy_kWhtotal + (energy_kWhtoday / 1000)) / 100000;
      energy_daily = (float)energy_kWhtoday / 100000000;
    }
  }
}

void HlwEvery200ms()
{
  unsigned long hlw_w = 0;
  unsigned long hlw_u = 0;
  unsigned long hlw_i = 0;

  if (micros() - hlw_cf_pulse_last_time > (HLW_POWER_PROBE_TIME * 1000000)) {
    hlw_cf_pulse_length = 0;    // No load for some time
    hlw_load_off = 1;
  }

  if (hlw_cf_pulse_length && (power &1) && !hlw_load_off) {
    hlw_w = (HLW_PREF * Settings.hlw_power_calibration) / hlw_cf_pulse_length;
    energy_power = (float)hlw_w / 10;
  } else {
    energy_power = 0;
  }

  hlw_cf1_timer++;
  if (hlw_cf1_timer >= 8) {
    hlw_cf1_timer = 0;
    hlw_select_ui_flag = (hlw_select_ui_flag) ? 0 : 1;
    digitalWrite(pin[GPIO_HLW_SEL], hlw_select_ui_flag);

    if (hlw_cf1_pulse_counter) {
      hlw_cf1_pulse_length = hlw_cf1_summed_pulse_length / hlw_cf1_pulse_counter;
    } else {
      hlw_cf1_pulse_length = 0;
    }
    if (hlw_select_ui_flag) {
      hlw_cf1_voltage_pulse_length = hlw_cf1_pulse_length;
      hlw_cf1_voltage_max_pulse_counter = hlw_cf1_pulse_counter;

      if (hlw_cf1_voltage_pulse_length && (power &1)) {     // If powered on always provide voltage
        hlw_u = (HLW_UREF * Settings.hlw_voltage_calibration) / hlw_cf1_voltage_pulse_length;
        energy_voltage = (float)hlw_u / 10;
      } else {
        energy_voltage = 0;
      }

    } else {
      hlw_cf1_current_pulse_length = hlw_cf1_pulse_length;
      hlw_cf1_current_max_pulse_counter = hlw_cf1_pulse_counter;

      if (hlw_cf1_current_pulse_length && energy_power) {   // No current if no power being consumed
        hlw_i = (HLW_IREF * Settings.hlw_current_calibration) / hlw_cf1_current_pulse_length;
        energy_current = (float)hlw_i / 1000;
      } else {
        energy_current = 0;
      }

    }
    hlw_cf1_summed_pulse_length = 0;
    hlw_cf1_pulse_counter = 0;
  }

/*
  energy_power = 0;
  if (hlw_cf_pulse_length && (power &1) && !hlw_load_off) {
    hlw_w = (HLW_PREF * Settings.hlw_power_calibration) / hlw_cf_pulse_length;
    energy_power = (float)hlw_w / 10;
  }
  energy_voltage = 0;
  if (hlw_cf1_voltage_pulse_length && (power &1)) {     // If powered on always provide voltage
    hlw_u = (HLW_UREF * Settings.hlw_voltage_calibration) / hlw_cf1_voltage_pulse_length;
    energy_voltage = (float)hlw_u / 10;
  }
  energy_current = 0;
  if (hlw_cf1_current_pulse_length && energy_power) {   // No current if no power being consumed
    hlw_i = (HLW_IREF * Settings.hlw_current_calibration) / hlw_cf1_current_pulse_length;
    energy_current = (float)hlw_i / 1000;
  }
*/
  energy_power_factor_ready = true;
}

void HlwInit()
{
  if (!Settings.hlw_power_calibration || (4975 == Settings.hlw_power_calibration)) {
    Settings.hlw_power_calibration = HLW_PREF_PULSE;
    Settings.hlw_voltage_calibration = HLW_UREF_PULSE;
    Settings.hlw_current_calibration = HLW_IREF_PULSE;
  }

  hlw_cf_pulse_length = 0;
  hlw_cf_pulse_last_time = 0;
  hlw_cf1_pulse_length = 0;
  hlw_cf1_pulse_last_time = 0;
  hlw_cf1_voltage_pulse_length = 0;
  hlw_cf1_current_pulse_length = 0;
  hlw_cf1_voltage_max_pulse_counter = 0;
  hlw_cf1_current_max_pulse_counter = 0;

  hlw_load_off = 1;
  hlw_energy_period_counter = 0;

  hlw_select_ui_flag = 0;  // Voltage;

  pinMode(pin[GPIO_HLW_SEL], OUTPUT);
  digitalWrite(pin[GPIO_HLW_SEL], hlw_select_ui_flag);
  pinMode(pin[GPIO_HLW_CF1], INPUT_PULLUP);
  attachInterrupt(pin[GPIO_HLW_CF1], HlwCf1Interrupt, FALLING);
  pinMode(pin[GPIO_HLW_CF], INPUT_PULLUP);
  attachInterrupt(pin[GPIO_HLW_CF], HlwCfInterrupt, FALLING);

  hlw_cf1_timer = 0;
}

#ifdef USE_PZEM004T
/*********************************************************************************************\
 * PZEM004T - Energy
 *
 * Source: Victor Ferrer https://github.com/vicfergar/Sonoff-MQTT-OTA-Arduino
 * Based on: PZEM004T library https://github.com/olehs/PZEM004T
\*********************************************************************************************/

#define PZEM_BAUD_RATE              9600

/*********************************************************************************************\
 * Subset SoftwareSerial
\*********************************************************************************************/

#define PZEM_SERIAL_BUFFER_SIZE     20
#define PZEM_SERIAL_WAIT { while (ESP.getCycleCount() -start < wait) optimistic_yield(1); wait += pzem_serial_bit_time; }

uint8_t pzem_serial_rx_pin;
uint8_t pzem_serial_tx_pin;
uint8_t pzem_serial_in_pos = 0;
uint8_t pzem_serial_out_pos = 0;
uint8_t pzem_serial_buffer[PZEM_SERIAL_BUFFER_SIZE];
unsigned long pzem_serial_bit_time;
unsigned long pzem_serial_bit_time_start;

bool PzemSerialValidGpioPin(uint8_t pin) {
  return (pin >= 0 && pin <= 5) || (pin >= 9 && pin <= 10) || (pin >= 12 && pin <= 15);
}

bool PzemSerial(uint8_t receive_pin, uint8_t transmit_pin)
{
  if (!((PzemSerialValidGpioPin(receive_pin)) && (PzemSerialValidGpioPin(transmit_pin) || transmit_pin == 16))) {
    return false;
  }
  pzem_serial_rx_pin = receive_pin;
  pinMode(pzem_serial_rx_pin, INPUT);
  attachInterrupt(pzem_serial_rx_pin, PzemSerialRxRead, FALLING);

  pzem_serial_tx_pin = transmit_pin;
  pinMode(pzem_serial_tx_pin, OUTPUT);
  digitalWrite(pzem_serial_tx_pin, 1);

  pzem_serial_bit_time = ESP.getCpuFreqMHz() *1000000 /PZEM_BAUD_RATE;   // 8333
  pzem_serial_bit_time_start = pzem_serial_bit_time + pzem_serial_bit_time /3 -500;  // 10610 ICACHE_RAM_ATTR start delay
//  pzem_serial_bit_time_start = pzem_serial_bit_time;                           // Non ICACHE_RAM_ATTR start delay (experimental)

  return true;
}

int PzemSerialRead() {
  if (pzem_serial_in_pos == pzem_serial_out_pos) {
    return -1;
  }
  int ch = pzem_serial_buffer[pzem_serial_out_pos];
  pzem_serial_out_pos = (pzem_serial_out_pos +1) % PZEM_SERIAL_BUFFER_SIZE;
  return ch;
}

int PzemSerialAvailable() {
  int avail = pzem_serial_in_pos - pzem_serial_out_pos;
  if (avail < 0) {
    avail += PZEM_SERIAL_BUFFER_SIZE;
  }
  return avail;
}

size_t PzemSerialTxWrite(uint8_t b)
{
  unsigned long wait = pzem_serial_bit_time;
  digitalWrite(pzem_serial_tx_pin, HIGH);
  unsigned long start = ESP.getCycleCount();
    // Start bit;
  digitalWrite(pzem_serial_tx_pin, LOW);
  PZEM_SERIAL_WAIT;
  for (int i = 0; i < 8; i++) {
    digitalWrite(pzem_serial_tx_pin, (b & 1) ? HIGH : LOW);
    PZEM_SERIAL_WAIT;
    b >>= 1;
  }
   // Stop bit
  digitalWrite(pzem_serial_tx_pin, HIGH);
  PZEM_SERIAL_WAIT;
  return 1;
}

size_t PzemSerialWrite(const uint8_t *buffer, size_t size = 1) {
  size_t n = 0;
  while(size--) {
    n += PzemSerialTxWrite(*buffer++);
  }
  return n;
}

//void PzemSerialRxRead() ICACHE_RAM_ATTR;  // Add 215 bytes to iram usage
void PzemSerialRxRead() {
  // Advance the starting point for the samples but compensate for the
  // initial delay which occurs before the interrupt is delivered
  unsigned long wait = pzem_serial_bit_time_start;
  unsigned long start = ESP.getCycleCount();
  uint8_t rec = 0;
  for (int i = 0; i < 8; i++) {
    PZEM_SERIAL_WAIT;
    rec >>= 1;
    if (digitalRead(pzem_serial_rx_pin)) {
      rec |= 0x80;
    }
  }
  // Stop bit
  PZEM_SERIAL_WAIT;
  // Store the received value in the buffer unless we have an overflow
  int next = (pzem_serial_in_pos +1) % PZEM_SERIAL_BUFFER_SIZE;
  if (next != pzem_serial_out_pos) {
    pzem_serial_buffer[pzem_serial_in_pos] = rec;
    pzem_serial_in_pos = next;
  }
  // Must clear this bit in the interrupt register,
  // it gets set even when interrupts are disabled
  GPIO_REG_WRITE(GPIO_STATUS_W1TC_ADDRESS, 1 << pzem_serial_rx_pin);
}

/*********************************************************************************************/

#define PZEM_VOLTAGE (uint8_t)0xB0
#define RESP_VOLTAGE (uint8_t)0xA0

#define PZEM_CURRENT (uint8_t)0xB1
#define RESP_CURRENT (uint8_t)0xA1

#define PZEM_POWER   (uint8_t)0xB2
#define RESP_POWER   (uint8_t)0xA2

#define PZEM_ENERGY  (uint8_t)0xB3
#define RESP_ENERGY  (uint8_t)0xA3

#define PZEM_SET_ADDRESS (uint8_t)0xB4
#define RESP_SET_ADDRESS (uint8_t)0xA4

#define PZEM_POWER_ALARM (uint8_t)0xB5
#define RESP_POWER_ALARM (uint8_t)0xA5

#define PZEM_DEFAULT_READ_TIMEOUT 500
#define PZEM_ERROR_VALUE -1.0

struct PZEMCommand {
  uint8_t command;
  uint8_t addr[4];
  uint8_t data;
  uint8_t crc;
};

IPAddress pzem_ip(192, 168, 1, 1);

float PZEM004T_voltage_rcv()
{
  uint8_t data[sizeof(PZEMCommand) -2];

  if (!PZEM004T_recieve(RESP_VOLTAGE, data)) {
    return PZEM_ERROR_VALUE;
  }
  return (data[0] << 8) + data[1] + (data[2] / 10.0);                      // 65535.x V
}

float PZEM004T_current_rcv()
{
  uint8_t data[sizeof(PZEMCommand) -2];

  if (!PZEM004T_recieve(RESP_CURRENT, data)) {
    return PZEM_ERROR_VALUE;
  }
  return (data[0] << 8) + data[1] + (data[2] / 100.0);                     // 65535.xx A
}

float PZEM004T_power_rcv()
{
  uint8_t data[sizeof(PZEMCommand) -2];

  if (!PZEM004T_recieve(RESP_POWER, data)) {
    return PZEM_ERROR_VALUE;
  }
  return (data[0] << 8) + data[1];                                         // 65535 W
}

float PZEM004T_energy_rcv()
{
  uint8_t data[sizeof(PZEMCommand) -2];

  if (!PZEM004T_recieve(RESP_ENERGY, data)) {
    return PZEM_ERROR_VALUE;
  }
  return ((uint32_t)data[0] << 16) + ((uint16_t)data[1] << 8) + data[2];   // 16777215 Wh
}

bool PZEM004T_setAddress_rcv()
{
  return PZEM004T_recieve(RESP_SET_ADDRESS, 0);
}

void PZEM004T_send(uint8_t cmd)
{
  PZEMCommand pzem;

  pzem.command = cmd;
  for (int i = 0; i < sizeof(pzem.addr); i++) {
    pzem.addr[i] = pzem_ip[i];
  }
  pzem.data = 0;

  uint8_t *bytes = (uint8_t*)&pzem;
  pzem.crc = PZEM004T_crc(bytes, sizeof(pzem) - 1);

  while (PzemSerialAvailable()) {
    PzemSerialRead();
  }
  PzemSerialWrite(bytes, sizeof(pzem));
}

bool PZEM004T_isReady()
{
  return PzemSerialAvailable() >= sizeof(PZEMCommand);
}

bool PZEM004T_recieve(uint8_t resp, uint8_t *data)
{
  uint8_t buffer[sizeof(PZEMCommand)];

  unsigned long startTime = millis();
  uint8_t len = 0;
  while ((len < sizeof(PZEMCommand)) && (millis() - startTime < PZEM_DEFAULT_READ_TIMEOUT)) {
    if (PzemSerialAvailable() > 0) {
      uint8_t c = (uint8_t)PzemSerialRead();
      if (!c && !len) {
        continue;  // skip 0 at startup
      }
      buffer[len++] = c;
    }
//    yield();  // do background netw tasks while blocked for IO (prevents ESP watchdog trigger) - This triggers Watchdog!!!
  }

  if (len != sizeof(PZEMCommand)) {
//    AddLog_P(LOG_LEVEL_DEBUG, PSTR(D_LOG_DEBUG "Pzem comms timeout"));
    return false;
  }
  if (buffer[6] != PZEM004T_crc(buffer, len - 1)) {
//    AddLog_P(LOG_LEVEL_DEBUG, PSTR(D_LOG_DEBUG "Pzem crc error"));
    return false;
  }
  if (buffer[0] != resp) {
//    AddLog_P(LOG_LEVEL_DEBUG, PSTR(D_LOG_DEBUG "Pzem bad response"));
    return false;
  }
  if (data) {
    for (int i = 0; i < sizeof(PZEMCommand) -2; i++) {
      data[i] = buffer[1 + i];
    }
  }

  return true;
}

uint8_t PZEM004T_crc(uint8_t *data, uint8_t sz)
{
  uint16_t crc = 0;
  for (uint8_t i = 0; i < sz; i++) {
    crc += *data++;
  }
  return (uint8_t)(crc & 0xFF);
}

/*********************************************************************************************/

typedef enum
{
  SET_ADDRESS,
  READ_VOLTAGE,
  READ_CURRENT,
  READ_POWER,
  READ_ENERGY,
} PZEMReadStates;

PZEMReadStates pzem_read_state = SET_ADDRESS;

byte pzem_sendRetry = 0;

void PzemEvery200ms()
{
  bool dataReady = PZEM004T_isReady();

  if (dataReady) {
    float pzem_value;
    switch (pzem_read_state) {
      case SET_ADDRESS:
        if (PZEM004T_setAddress_rcv()) {
          pzem_read_state = READ_VOLTAGE;
        }
        break;
      case READ_VOLTAGE:
        pzem_value = PZEM004T_voltage_rcv();
        if (pzem_value != PZEM_ERROR_VALUE) {
          energy_voltage = pzem_value;    // 230.2V
          pzem_read_state = READ_CURRENT;
        }
        break;
      case READ_CURRENT:
        pzem_value = PZEM004T_current_rcv();
        if (pzem_value != PZEM_ERROR_VALUE) {
          energy_current = pzem_value;    // 17.32A
          pzem_read_state = READ_POWER;
        }
        break;
      case READ_POWER:
        pzem_value = PZEM004T_power_rcv();
        if (pzem_value != PZEM_ERROR_VALUE) {
          energy_power = pzem_value;  // 20W
          energy_power_factor_ready = true;
          pzem_read_state = READ_ENERGY;
        }
        break;
      case READ_ENERGY:
        pzem_value = PZEM004T_energy_rcv();
        if (pzem_value != PZEM_ERROR_VALUE) {
          energy_total = pzem_value / 1000;    // 99999Wh
          if (!energy_startup) {
            if (energy_total < energy_start) {
              energy_start = energy_total;
              Settings.hlw_power_calibration = energy_start * 1000;
            }
            energy_kWhtoday = (energy_total - energy_start) * 100000000;
            energy_daily = (float)energy_kWhtoday / 100000000;
          }
          pzem_read_state = READ_VOLTAGE;
        }
        break;
    }
  }

  if (0 == pzem_sendRetry || dataReady) {
    pzem_sendRetry = 5;

    switch (pzem_read_state) {
      case SET_ADDRESS:
        PZEM004T_send(PZEM_SET_ADDRESS);
        break;
      case READ_VOLTAGE:
        PZEM004T_send(PZEM_VOLTAGE);
        break;
      case READ_CURRENT:
        PZEM004T_send(PZEM_CURRENT);
        break;
      case READ_POWER:
        PZEM004T_send(PZEM_POWER);
        break;
      case READ_ENERGY:
        PZEM004T_send(PZEM_ENERGY);
        break;
    }
  }
  else {
    pzem_sendRetry--;
  }
}

bool PzemInit()
{
  return PzemSerial(pin[GPIO_PZEM_RX], pin[GPIO_PZEM_TX]);
}

/********************************************************************************************/
#endif  // USE_PZEM004T

void Energy200ms()
{
  energy_fifth_second++;
  if (5 == energy_fifth_second) {
    energy_fifth_second = 0;

    if (ENERGY_HLW8012 == energy_flg) {
      HlwEverySecond();
    }

    if (RtcTime.valid) {
      if (LocalTime() == Midnight()) {
        Settings.energy_kWhyesterday = energy_kWhtoday;
        Settings.energy_kWhtotal += (energy_kWhtoday / 1000);
        RtcSettings.energy_kWhtotal = Settings.energy_kWhtotal;
        energy_kWhtoday = 0;
        RtcSettings.energy_kWhtoday = energy_kWhtoday;
#ifdef USE_PZEM004T
        if (ENERGY_PZEM004T == energy_flg) {
          energy_start = energy_total;
          Settings.hlw_power_calibration = energy_start * 1000;
        }
#endif  // USE_PZEM004T
        energy_max_energy_state = 3;
      }
      if ((RtcTime.hour == Settings.energy_max_energy_start) && (3 == energy_max_energy_state)) {
        energy_max_energy_state = 0;
      }
      if (energy_startup && (RtcTime.day_of_year == Settings.energy_kWhdoy)) {
        energy_kWhtoday = Settings.energy_kWhtoday;
        RtcSettings.energy_kWhtoday = energy_kWhtoday;
        energy_start = (float)Settings.hlw_power_calibration / 1000;  // Used by PZEM004T to store total yesterday
        energy_startup = 0;
      }
    }
  }

  if (ENERGY_HLW8012 == energy_flg) {
    HlwEvery200ms();
#ifdef USE_PZEM004T
  }
  else if (ENERGY_PZEM004T == energy_flg) {
    PzemEvery200ms();
#endif  // USE_PZEM004T
  }

  if (energy_power_factor_ready && energy_voltage && energy_current && energy_power) {
    energy_power_factor_ready = false;
    float power_factor = energy_power / (energy_voltage * energy_current);
    if (power_factor > 1) {
      power_factor = 1;
    }
    energy_power_factor = power_factor;
  }
}

void EnergySaveState()
{
  Settings.energy_kWhdoy = (RtcTime.valid) ? RtcTime.day_of_year : 0;
  Settings.energy_kWhtoday = energy_kWhtoday;
  Settings.energy_kWhtotal = RtcSettings.energy_kWhtotal;
}

boolean EnergyMargin(byte type, uint16_t margin, uint16_t value, byte &flag, byte &save_flag)
{
  byte change;

  if (!margin) {
    return false;
  }
  change = save_flag;
  if (type) {
    flag = (value > margin);
  } else {
    flag = (value < margin);
  }
  save_flag = flag;
  return (change != save_flag);
}

void EnergySetPowerSteadyCounter(byte value)
{
  energy_power_steady_cntr = 2;
}

void EnergyMarginCheck()
{
  uint16_t energy_daily_u;
  uint16_t energy_power_u;
  uint16_t energy_voltage_u;
  uint16_t energy_current_u;
  boolean flag;
  boolean jsonflg;

  if (energy_power_steady_cntr) {
    energy_power_steady_cntr--;
    return;
  }

  if (power && (Settings.energy_min_power || Settings.energy_max_power || Settings.energy_min_voltage || Settings.energy_max_voltage || Settings.energy_min_current || Settings.energy_max_current)) {
    energy_power_u = (uint16_t)(energy_power);
    energy_voltage_u = (uint16_t)(energy_voltage);
    energy_current_u = (uint16_t)(energy_current * 1000);

//    snprintf_P(log_data, sizeof(log_data), PSTR("HLW: W %d, U %d, I %d"), energy_power_u, energy_voltage_u, energy_current_u);
//    AddLog(LOG_LEVEL_DEBUG);

    snprintf_P(mqtt_data, sizeof(mqtt_data), PSTR("{"));
    jsonflg = 0;
    if (EnergyMargin(0, Settings.energy_min_power, energy_power_u, flag, energy_min_power_flag)) {
      snprintf_P(mqtt_data, sizeof(mqtt_data), PSTR("%s%s\"" D_CMND_POWERLOW "\":\"%s\""), mqtt_data, (jsonflg)?",":"", GetStateText(flag));
      jsonflg = 1;
    }
    if (EnergyMargin(1, Settings.energy_max_power, energy_power_u, flag, energy_max_power_flag)) {
      snprintf_P(mqtt_data, sizeof(mqtt_data), PSTR("%s%s\"" D_CMND_POWERHIGH "\":\"%s\""), mqtt_data, (jsonflg)?",":"", GetStateText(flag));
      jsonflg = 1;
    }
    if (EnergyMargin(0, Settings.energy_min_voltage, energy_voltage_u, flag, energy_min_voltage_flag)) {
      snprintf_P(mqtt_data, sizeof(mqtt_data), PSTR("%s%s\"" D_CMND_VOLTAGELOW "\":\"%s\""), mqtt_data, (jsonflg)?",":"", GetStateText(flag));
      jsonflg = 1;
    }
    if (EnergyMargin(1, Settings.energy_max_voltage, energy_voltage_u, flag, energy_max_voltage_flag)) {
      snprintf_P(mqtt_data, sizeof(mqtt_data), PSTR("%s%s\"" D_CMND_VOLTAGEHIGH "\":\"%s\""), mqtt_data, (jsonflg)?",":"", GetStateText(flag));
      jsonflg = 1;
    }
    if (EnergyMargin(0, Settings.energy_min_current, energy_current_u, flag, energy_min_current_flag)) {
      snprintf_P(mqtt_data, sizeof(mqtt_data), PSTR("%s%s\"" D_CMND_CURRENTLOW "\":\"%s\""), mqtt_data, (jsonflg)?",":"", GetStateText(flag));
      jsonflg = 1;
    }
    if (EnergyMargin(1, Settings.energy_max_current, energy_current_u, flag, energy_max_current_flag)) {
      snprintf_P(mqtt_data, sizeof(mqtt_data), PSTR("%s%s\"" D_CMND_CURRENTHIGH "\":\"%s\""), mqtt_data, (jsonflg)?",":"", GetStateText(flag));
      jsonflg = 1;
    }
    if (jsonflg) {
      snprintf_P(mqtt_data, sizeof(mqtt_data), PSTR("%s}"), mqtt_data);
      MqttPublishPrefixTopic_P(2, PSTR(D_RSLT_MARGINS));
      EnergyMqttShow();
    }
  }

#if FEATURE_POWER_LIMIT
  // Max Power
  if (Settings.energy_max_power_limit) {
    if (energy_power > Settings.energy_max_power_limit) {
      if (!energy_mplh_counter) {
        energy_mplh_counter = Settings.energy_max_power_limit_hold;
      } else {
        energy_mplh_counter--;
        if (!energy_mplh_counter) {
          snprintf_P(mqtt_data, sizeof(mqtt_data), PSTR("{\"" D_MAXPOWERREACHED "\":\"%d%s\"}"), energy_power_u, (Settings.flag.value_units) ? " " D_UNIT_WATT : "");
          MqttPublishPrefixTopic_P(1, S_RSLT_WARNING);
          EnergyMqttShow();
          ExecuteCommandPower(1, 0);
          if (!energy_mplr_counter) {
            energy_mplr_counter = Settings.param[P_MAX_POWER_RETRY] +1;
          }
          energy_mplw_counter = Settings.energy_max_power_limit_window;
        }
      }
    }
    else if (power && (energy_power_u <= Settings.energy_max_power_limit)) {
      energy_mplh_counter = 0;
      energy_mplr_counter = 0;
      energy_mplw_counter = 0;
    }
    if (!power) {
      if (energy_mplw_counter) {
        energy_mplw_counter--;
      } else {
        if (energy_mplr_counter) {
          energy_mplr_counter--;
          if (energy_mplr_counter) {
            snprintf_P(mqtt_data, sizeof(mqtt_data), PSTR("{\"" D_POWERMONITOR "\":\"%s\"}"), GetStateText(1));
            MqttPublishPrefixTopic_P(5, PSTR(D_POWERMONITOR));
            ExecuteCommandPower(1, 1);
          } else {
            snprintf_P(mqtt_data, sizeof(mqtt_data), PSTR("{\"" D_MAXPOWERREACHEDRETRY "\":\"%s\"}"), GetStateText(0));
            MqttPublishPrefixTopic_P(1, S_RSLT_WARNING);
            EnergyMqttShow();
          }
        }
      }
    }
  }

  // Max Energy
  if (Settings.energy_max_energy) {
    energy_daily_u = (uint16_t)(energy_daily * 1000);
    if (!energy_max_energy_state && (RtcTime.hour == Settings.energy_max_energy_start)) {
      energy_max_energy_state = 1;
      snprintf_P(mqtt_data, sizeof(mqtt_data), PSTR("{\"" D_ENERGYMONITOR "\":\"%s\"}"), GetStateText(1));
      MqttPublishPrefixTopic_P(5, PSTR(D_ENERGYMONITOR));
      ExecuteCommandPower(1, 1);
    }
    else if ((1 == energy_max_energy_state) && (energy_daily_u >= Settings.energy_max_energy)) {
      energy_max_energy_state = 2;
      dtostrfd(energy_daily, 3, mqtt_data);
      snprintf_P(mqtt_data, sizeof(mqtt_data), PSTR("{\"" D_MAXENERGYREACHED "\":\"%s%s\"}"), mqtt_data, (Settings.flag.value_units) ? " " D_UNIT_KILOWATTHOUR : "");
      MqttPublishPrefixTopic_P(1, S_RSLT_WARNING);
      EnergyMqttShow();
      ExecuteCommandPower(1, 0);
    }
  }
#endif  // FEATURE_POWER_LIMIT
}

void EnergyMqttShow()
{
// {"Time":"2017-12-16T11:48:55","ENERGY":{"Total":0.212,"Yesterday":0.000,"Today":0.014,"Period":2.0,"Power":22.0,"Factor":1.00,"Voltage":213.6,"Current":0.100}}
  snprintf_P(mqtt_data, sizeof(mqtt_data), PSTR("{\"" D_TIME "\":\"%s\""), GetDateAndTime().c_str());
  EnergyShow(1);
  MqttPublishPrefixTopic_P(2, PSTR(D_RSLT_ENERGY), Settings.flag.mqtt_sensor_retain);
}

/*********************************************************************************************\
 * Commands
\*********************************************************************************************/

boolean EnergyCommand(char *type, uint16_t index, char *dataBuf, uint16_t data_len, int16_t payload)
{
  char command [CMDSZ];
  char sunit[CMDSZ];
  boolean serviced = true;
  uint8_t status_flag = 0;
  uint8_t unit = 0;
  unsigned long nvalue = 0;

  int command_code = GetCommandCode(command, sizeof(command), type, kEnergyCommands);
  if (CMND_POWERLOW == command_code) {
    if ((payload >= 0) && (payload < 3601)) {
      Settings.energy_min_power = payload;
    }
    nvalue = Settings.energy_min_power;
    unit = UNIT_WATT;
  }
  else if (CMND_POWERHIGH == command_code) {
    if ((payload >= 0) && (payload < 3601)) {
      Settings.energy_max_power = payload;
    }
    nvalue = Settings.energy_max_power;
    unit = UNIT_WATT;
  }
  else if (CMND_VOLTAGELOW == command_code) {
    if ((payload >= 0) && (payload < 501)) {
      Settings.energy_min_voltage = payload;
    }
    nvalue = Settings.energy_min_voltage;
    unit = UNIT_VOLT;
  }
  else if (CMND_VOLTAGEHIGH == command_code) {
    if ((payload >= 0) && (payload < 501)) {
      Settings.energy_max_voltage = payload;
    }
    nvalue = Settings.energy_max_voltage;
    unit = UNIT_VOLT;
  }
  else if (CMND_CURRENTLOW == command_code) {
    if ((payload >= 0) && (payload < 16001)) {
      Settings.energy_min_current = payload;
    }
    nvalue = Settings.energy_min_current;
    unit = UNIT_MILLIAMPERE;
  }
  else if (CMND_CURRENTHIGH == command_code) {
    if ((payload >= 0) && (payload < 16001)) {
      Settings.energy_max_current = payload;
    }
    nvalue = Settings.energy_max_current;
    unit = UNIT_MILLIAMPERE;
  }
  else if ((CMND_ENERGYRESET == command_code) && (index > 0) && (index <= 3)) {
    char *p;
    unsigned long lnum = strtoul(dataBuf, &p, 10);
    if (p != dataBuf) {
      switch (index) {
      case 1:
        energy_kWhtoday = lnum *100000;
        RtcSettings.energy_kWhtoday = energy_kWhtoday;
        Settings.energy_kWhtoday = energy_kWhtoday;
        break;
      case 2:
        Settings.energy_kWhyesterday = lnum *100000;
        break;
      case 3:
        RtcSettings.energy_kWhtotal = lnum *100;
        Settings.energy_kWhtotal = RtcSettings.energy_kWhtotal;
        break;
      }
    }
    char energy_yesterday_chr[10];
    char stoday_energy[10];
    char energy_total_chr[10];
    dtostrfd((float)Settings.energy_kWhyesterday / 100000000, Settings.flag2.energy_resolution, energy_yesterday_chr);
    dtostrfd((float)RtcSettings.energy_kWhtoday / 100000000, Settings.flag2.energy_resolution, stoday_energy);
    dtostrfd((float)(RtcSettings.energy_kWhtotal + (energy_kWhtoday / 1000)) / 100000, Settings.flag2.energy_resolution, energy_total_chr);
    snprintf_P(mqtt_data, sizeof(mqtt_data), PSTR("{\"%s\":{\"" D_TOTAL "\":%s,\"" D_YESTERDAY "\":%s,\"" D_TODAY "\":%s}}"),
      command, energy_total_chr, energy_yesterday_chr, stoday_energy);
    status_flag = 1;
  }
  else if ((ENERGY_HLW8012 == energy_flg) && (CMND_HLWPCAL == command_code)) {
    if ((payload > 0) && (payload < 32001)) {
      Settings.hlw_power_calibration = (payload > 4000) ? payload : HLW_PREF_PULSE;  // 12530
    }
    nvalue = Settings.hlw_power_calibration;
    unit = UNIT_MICROSECOND;
  }
  else if ((ENERGY_HLW8012 == energy_flg) && (CMND_HLWPSET == command_code)) {
    if ((payload > 0) && (payload < 3601) && hlw_cf_pulse_length) {
      Settings.hlw_power_calibration = (payload * 10 * hlw_cf_pulse_length) / HLW_PREF;
    }
    snprintf_P(command, sizeof(command), PSTR(D_CMND_HLWPCAL));
    nvalue = Settings.hlw_power_calibration;
    unit = UNIT_MICROSECOND;
  }
  else if ((ENERGY_HLW8012 == energy_flg) && (CMND_HLWUCAL == command_code)) {
    if ((payload > 0) && (payload < 32001)) {
      Settings.hlw_voltage_calibration = (payload > 999) ? payload : HLW_UREF_PULSE;  // 1950
    }
    nvalue = Settings.hlw_voltage_calibration;
    unit = UNIT_MICROSECOND;
  }
  else if ((ENERGY_HLW8012 == energy_flg) && (CMND_HLWUSET == command_code)) {
    if ((payload > 0) && (payload < 501) && hlw_cf1_voltage_pulse_length) {
      Settings.hlw_voltage_calibration = (payload * 10 * hlw_cf1_voltage_pulse_length) / HLW_UREF;
    }
    snprintf_P(command, sizeof(command), PSTR(D_CMND_HLWUCAL));
    nvalue = Settings.hlw_voltage_calibration;
    unit = UNIT_MICROSECOND;
  }
  else if ((ENERGY_HLW8012 == energy_flg) && (CMND_HLWICAL == command_code)) {
    if ((payload > 0) && (payload < 32001)) {
      Settings.hlw_current_calibration = (payload > 1100) ? payload : HLW_IREF_PULSE;  // 3500
    }
    nvalue = Settings.hlw_current_calibration;
    unit = UNIT_MICROSECOND;
  }
  else if ((ENERGY_HLW8012 == energy_flg) && (CMND_HLWISET == command_code)) {
    if ((payload > 0) && (payload < 16001) && hlw_cf1_current_pulse_length) {
      Settings.hlw_current_calibration = (payload * hlw_cf1_current_pulse_length) / HLW_IREF;
    }
    snprintf_P(command, sizeof(command), PSTR(D_CMND_HLWICAL));
    nvalue = Settings.hlw_current_calibration;
    unit = UNIT_MICROSECOND;
  }
#if FEATURE_POWER_LIMIT
  else if (CMND_MAXPOWER == command_code) {
    if ((payload >= 0) && (payload < 3601)) {
      Settings.energy_max_power_limit = payload;
    }
    nvalue = Settings.energy_max_power_limit;
    unit = UNIT_WATT;
  }
  else if (CMND_MAXPOWERHOLD == command_code) {
    if ((payload >= 0) && (payload < 3601)) {
      Settings.energy_max_power_limit_hold = (1 == payload) ? MAX_POWER_HOLD : payload;
    }
    nvalue = Settings.energy_max_power_limit_hold;
    unit = UNIT_SECOND;
  }
  else if (CMND_MAXPOWERWINDOW == command_code) {
    if ((payload >= 0) && (payload < 3601)) {
      Settings.energy_max_power_limit_window = (1 == payload) ? MAX_POWER_WINDOW : payload;
    }
    nvalue = Settings.energy_max_power_limit_window;
    unit = UNIT_SECOND;
  }
  else if (CMND_SAFEPOWER == command_code) {
    if ((payload >= 0) && (payload < 3601)) {
      Settings.energy_max_power_safe_limit = payload;
    }
    nvalue = Settings.energy_max_power_safe_limit;
    unit = UNIT_WATT;
  }
  else if (CMND_SAFEPOWERHOLD == command_code) {
    if ((payload >= 0) && (payload < 3601)) {
      Settings.energy_max_power_safe_limit_hold = (1 == payload) ? SAFE_POWER_HOLD : payload;
    }
    nvalue = Settings.energy_max_power_safe_limit_hold;
    unit = UNIT_SECOND;
  }
  else if (CMND_SAFEPOWERWINDOW == command_code) {
    if ((payload >= 0) && (payload < 1440)) {
      Settings.energy_max_power_safe_limit_window = (1 == payload) ? SAFE_POWER_WINDOW : payload;
    }
    nvalue = Settings.energy_max_power_safe_limit_window;
    unit = UNIT_MINUTE;
  }
  else if (CMND_MAXENERGY == command_code) {
    if ((payload >= 0) && (payload < 3601)) {
      Settings.energy_max_energy = payload;
      energy_max_energy_state = 3;
    }
    nvalue = Settings.energy_max_energy;
    unit = UNIT_WATTHOUR;
  }
  else if (CMND_MAXENERGYSTART == command_code) {
    if ((payload >= 0) && (payload < 24)) {
      Settings.energy_max_energy_start = payload;
    }
    nvalue = Settings.energy_max_energy_start;
    unit = UNIT_HOUR;
  }
#endif  // FEATURE_POWER_LIMIT
  else {
    serviced = false;
  }
  if (!status_flag) {
    if (Settings.flag.value_units) {
      snprintf_P(mqtt_data, sizeof(mqtt_data), S_JSON_COMMAND_NVALUE_SPACE_UNIT, command, nvalue, GetTextIndexed(sunit, sizeof(sunit), unit, kUnitNames));
    } else {
      snprintf_P(mqtt_data, sizeof(mqtt_data), S_JSON_COMMAND_NVALUE, command, nvalue);
    }
  }
  return serviced;
}

/********************************************************************************************/

void EnergyInit()
{
  energy_flg = ENERGY_NONE;
  if ((pin[GPIO_HLW_SEL] < 99) && (pin[GPIO_HLW_CF1] < 99) && (pin[GPIO_HLW_CF] < 99)) {
    energy_flg = ENERGY_HLW8012;
    HlwInit();
#ifdef USE_PZEM004T
  } else if ((pin[GPIO_PZEM_RX] < 99) && (pin[GPIO_PZEM_TX])) {
    if (PzemInit()) {
      energy_flg = ENERGY_PZEM004T;
    }
#endif  // USE_PZEM004T
  }

  if (energy_flg) {
    energy_kWhtoday = (RtcSettingsValid()) ? RtcSettings.energy_kWhtoday : 0;

    energy_startup = 1;
    ticker_energy.attach_ms(200, Energy200ms);
  }
}

#ifdef USE_WEBSERVER
const char HTTP_ENERGY_SNS[] PROGMEM =
  "{s}" D_VOLTAGE "{m}%s " D_UNIT_VOLT "{e}"
  "{s}" D_CURRENT "{m}%s " D_UNIT_AMPERE "{e}"
  "{s}" D_POWERUSAGE "{m}%s " D_UNIT_WATT "{e}"
  "{s}" D_POWER_FACTOR "{m}%s{e}"
  "{s}" D_ENERGY_TODAY  "{m}%s " D_UNIT_KILOWATTHOUR "{e}"
  "{s}" D_ENERGY_YESTERDAY "{m}%s " D_UNIT_KILOWATTHOUR "{e}"
  "{s}" D_ENERGY_TOTAL "{m}%s " D_UNIT_KILOWATTHOUR "{e}";      // {s} = <tr><th>, {m} = </th><td>, {e} = </td></tr>
#endif  // USE_WEBSERVER

void EnergyShow(boolean json)
{
  char energy_total_chr[10];
  char energy_daily_chr[10];
  char energy_period_chr[10];
  char energy_power_chr[10];
  char energy_voltage_chr[10];
  char energy_current_chr[10];
  char energy_power_factor_chr[10];
  char energy_yesterday_chr[10];
  char speriod[20];

  bool show_energy_period = (0 == tele_period);

  float energy = 0;
  if (show_energy_period) {
    if (energy_period) {
      energy = (float)(energy_kWhtoday - energy_period) / 100000;
    }
    energy_period = energy_kWhtoday;
  }

  dtostrfd(energy_total, Settings.flag2.energy_resolution, energy_total_chr);
  dtostrfd(energy_daily, Settings.flag2.energy_resolution, energy_daily_chr);
  dtostrfd(energy, Settings.flag2.wattage_resolution, energy_period_chr);
  dtostrfd(energy_power, Settings.flag2.wattage_resolution, energy_power_chr);
  dtostrfd(energy_voltage, Settings.flag2.voltage_resolution, energy_voltage_chr);
  dtostrfd(energy_current, Settings.flag2.current_resolution, energy_current_chr);
  dtostrfd(energy_power_factor, 2, energy_power_factor_chr);
  dtostrfd((float)Settings.energy_kWhyesterday / 100000000, Settings.flag2.energy_resolution, energy_yesterday_chr);

  if (json) {
    snprintf_P(speriod, sizeof(speriod), PSTR(",\"" D_PERIOD "\":%s"), energy_period_chr);
    snprintf_P(mqtt_data, sizeof(mqtt_data), PSTR("%s,\"" D_RSLT_ENERGY "\":{\"" D_TOTAL "\":%s,\"" D_YESTERDAY "\":%s,\"" D_TODAY "\":%s%s,\"" D_POWERUSAGE "\":%s,\"" D_POWERFACTOR "\":%s,\"" D_VOLTAGE "\":%s,\"" D_CURRENT "\":%s}"),
      mqtt_data, energy_total_chr, energy_yesterday_chr, energy_daily_chr, (show_energy_period) ? speriod : "", energy_power_chr, energy_power_factor_chr, energy_voltage_chr, energy_current_chr);
#ifdef USE_DOMOTICZ
    if (show_energy_period) {  // Only send if telemetry
      dtostrfd(energy_total * 1000, 1, energy_total_chr);
      DomoticzSensorPowerEnergy((uint16_t)energy_power, energy_total_chr);  // PowerUsage, EnergyToday
      DomoticzSensor(DZ_VOLTAGE, energy_voltage_chr);  // Voltage
      DomoticzSensor(DZ_CURRENT, energy_current_chr);  // Current
    }
#endif  // USE_DOMOTICZ
#ifdef USE_WEBSERVER
  } else {
    snprintf_P(mqtt_data, sizeof(mqtt_data), HTTP_ENERGY_SNS, energy_voltage_chr, energy_current_chr, energy_power_chr, energy_power_factor_chr, energy_daily_chr, energy_yesterday_chr, energy_total_chr);
#endif  // USE_WEBSERVER
  }
}

/*********************************************************************************************\
 * Interface
\*********************************************************************************************/

#define XSNS_03

boolean Xsns03(byte function)
{
  boolean result = false;

  if (energy_flg) {
    switch (function) {
      case FUNC_XSNS_INIT:
        EnergyInit();
        break;
      case FUNC_XSNS_EVERY_SECOND:
        EnergyMarginCheck();
        break;
//      case FUNC_XSNS_PREP_BEFORE_TELEPERIOD:
//        break;
      case FUNC_XSNS_JSON_APPEND:
        EnergyShow(1);
        break;
#ifdef USE_WEBSERVER
      case FUNC_XSNS_WEB_APPEND:
        EnergyShow(0);
        break;
#endif  // USE_WEBSERVER
      case FUNC_XSNS_SAVE_BEFORE_RESTART:
        EnergySaveState();
        break;
    }
  }
  return result;
}

#endif  // USE_ENERGY_SENSOR