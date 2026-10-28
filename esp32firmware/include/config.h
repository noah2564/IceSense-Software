#pragma once
// Compile-time constants. Everything runtime-changeable (intervals, sample rate, baselines)
// lives in the persisted Config struct in main.cpp and is set via set_config / capture_baseline.

#define FW_VERSION      "0.1.0"
#define SCHEMA_VERSION  "1.0.0"
#define MQTT_TOPIC_PREFIX "ice/v1"

// ---- Pins (adjust to your board). Use ADC1 pins only: ADC2 does not work while WiFi is on.
#if CONFIG_IDF_TARGET_ESP32S3
  #define PIN_TOUCH        4    // touch pad -> fin electrode (capacitive)
  #define PIN_RES_EXCITE   5    // drives the resistive divider, ON only while measuring
  #define PIN_RES_ADC      6    // ADC1
  #define PIN_NTC_ADC      7    // ADC1
#else  // classic ESP32
  #define PIN_TOUCH        4    // T0
  #define PIN_RES_EXCITE   25
  #define PIN_RES_ADC      34   // ADC1_CH6 (input only)
  #define PIN_NTC_ADC      35   // ADC1_CH7 (input only)
#endif

// ---- Resistive channel: EXCITE -> R_REF -> [ADC node + 1 nF to GND] -> electrode grid -> GND
// With 1 Mohm the usable range is roughly 10 kohm .. 10 Mohm. Dry fins are far above that
// (reported as open_circuit); a high-impedance front end or RC timing is the next step.
#define ADC_VCC            3.3
#define RES_R_REF_OHM      1000000.0
#define RES_SETTLE_MS      10        // 1 Mohm * 1 nF = 1 ms time constant -> 10 ms is ~10 tau
#define RES_OPEN_MV        3000      // ADC node above this: resistance beyond measurable range
#define RES_SHORT_MV       20        // below this: electrode shorted
#define RES_MIN_OHM        1000.0
#define RES_MAX_OHM        1.0e9

// ---- Coil temperature: 3V3 -> R_FIXED -> [ADC node] -> NTC -> GND
#define NTC_R_FIXED_OHM    10000.0
#define NTC_R0_OHM         10000.0   // NTC resistance at 25 C
#define NTC_BETA           3950.0

// ---- Defaults and limits
#define DEFAULT_TELEMETRY_INTERVAL_S 10
#define DEFAULT_SAMPLE_RATE_HZ       10
#define MIN_TELEMETRY_INTERVAL_S     5
#define MAX_TELEMETRY_INTERVAL_S     3600
#define MIN_SAMPLE_RATE_HZ           1
#define MAX_SAMPLE_RATE_HZ           20   // each sample takes ~15 ms (resistive settle time dominates)
#define STATUS_INTERVAL_S            60
#define BASELINE_SAMPLES             20

// ---- Buffering / MQTT
#define OUTQ_MAX            20      // messages kept in RAM while offline (oldest dropped first)
#define MQTT_BUFFER_BYTES   4096
