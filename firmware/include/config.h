#pragma once
// Compile-time constants. Everything runtime-changeable (window length, detection parameters,
// baseline, relay enable) lives in the persisted settings in main.cpp and is changed from the web
// page or by MQTT commands.

#define FW_VERSION      "0.2.0"
#define SCHEMA_VERSION  "1.0.0"
#define MQTT_TOPIC_PREFIX "ice/v1"

// ---- Pins (adjust to your board). Use ADC1 pins only: ADC2 does not work while WiFi is on.
#if CONFIG_IDF_TARGET_ESP32S3
  #define PIN_TOUCH        4    // touch pad -> fin electrode (capacitive)
  #define PIN_RES_EXCITE   5    // drives the resistive divider, ON only while measuring
  #define PIN_RES_ADC      6    // ADC1
  #define PIN_NTC_ADC      7    // ADC1
  #define PIN_RELAY        8    // defrost-request relay driver (via transistor/relay board)
#else  // classic ESP32
  #define PIN_TOUCH        4    // T0
  #define PIN_RES_EXCITE   25
  #define PIN_RES_ADC      34   // ADC1_CH6 (input only)
  #define PIN_NTC_ADC      35   // ADC1_CH7 (input only)
  #define PIN_RELAY        26
#endif
#define RELAY_ACTIVE_LEVEL HIGH   // pin level that closes the contact; the relay is OFF at boot

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

// ---- Sampling and detection window
#define SAMPLE_RATE_HZ              10    // each sample takes ~15 ms (resistive settle time dominates)
#define DEFAULT_WINDOW_S            2     // one detection result (and one MQTT telemetry message) per window
#define MIN_WINDOW_S                1
#define MAX_WINDOW_S                60
#define STATUS_INTERVAL_S           60
#define ROLL_LEN                    20    // most recent raw samples kept for baseline capture (~2 s)
#define BASELINE_MIN_SAMPLES        10
#define WIN_QUEUE_LEN               8     // finished windows waiting for the main loop

// ---- Technician web page
#define HISTORY_LEN                 900   // windows kept in RAM for the charts / CSV (30 min at 2 s)
#define AP_SSID_PREFIX              "icesense-"

// ---- MQTT (optional, research phase)
#define OUTQ_MAX            20      // messages kept in RAM while offline (oldest dropped first)
#define MQTT_BUFFER_BYTES   4096
