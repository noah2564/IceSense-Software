#pragma once
// Compile-time constants. Everything runtime-changeable (window length, detection parameters,
// baseline, relay enable) lives in the persisted settings in main.cpp and is changed from the web
// page or by MQTT commands.

#define FW_VERSION      "0.2.0"
#define SCHEMA_VERSION  "1.0.0"
#define MQTT_TOPIC_PREFIX "ice/v1"

// ---- ESP32-S3-DevKitC-1-N8R8 board pins
#if !CONFIG_IDF_TARGET_ESP32S3
  #error "ICE-SENSE firmware requires ESP32-S3"
#endif
#define PIN_I2C_A_SDA       2   // Wire: LM75B + MCP3426
#define PIN_I2C_A_SCL       1
#define PIN_I2C_B_SDA       6   // Wire1: FDC2214
#define PIN_I2C_B_SCL       5
#define PIN_FDC2214_SD      4   // Active HIGH shutdown; hold LOW to run
#define PIN_FDC2214_INTB    7   // Input only; status is polled instead of using this pin
#define PIN_ESP_ADC_J3_2   19   // Defined for the J3 connector; unused (USB D- / ADC2)
#define PIN_ESP_ADC_J3_4   20   // Defined for the J3 connector; unused (USB D+ / ADC2)
#define PIN_SPARE_J6        3   // Spare screw-terminal GPIO; unused
// WARNING: GPIO36 may be used by octal PSRAM on ESP32-S3-N8R8 modules. Keep this as one
// obvious board-specific define so it can be changed if the module routing requires it.
#define PIN_RELAY          36   // BS107 gate, active HIGH; gate pull-down keeps it off at boot
#define RELAY_ACTIVE_LEVEL HIGH

// ---- I2C buses and device addresses
#define I2C_CLOCK_HZ              400000UL
#define I2C_TIMEOUT_MS            20
#define LM75B_I2C_ADDRESS         0x48
#define MCP3426_I2C_ADDRESS       0x68  // A0 variant is expected; verify with the boot I2C scan
#define FDC2214_I2C_ADDRESS_LOW   0x2A
#define FDC2214_I2C_ADDRESS_HIGH  0x2B

// ---- MCP3426 conversion settings
#define MCP3426_CHANNEL_FRONT     0     // Assumed CH1 = IN1+ front node minus IN1- reference
#define MCP3426_CHANNEL_BACK      1     // Assumed CH2 = IN2+ back node minus IN2- reference
#define MCP3426_RESOLUTION_BITS   14    // 14-bit / 60 SPS leaves time for two-channel scans
#define MCP3426_GAIN              1     // PGA x1; raise only after measuring the real front end
#define MCP3426_CONTINUOUS        0     // One-shot conversion
#define MCP3426_CONVERSION_TIMEOUT_MS 25 // 14-bit conversion is about 16.7 ms
#define MCP3426_REFERENCE_V       2.048 // Internal reference, volts

// ---- Resistive front end: 3V3 -> 1M -> sensor node -> sensor -> GND; shared 40k/40k reference
#define RES_R_FIXED_OHM           1.0e6
#define RES_R_REF_TOP_OHM         40.0e3
#define RES_R_REF_BOTTOM_OHM      40.0e3
#define MCP3426_ZIN_DIFF_OHM_X1   2.25e6 // Differential input impedance at PGA x1; model divides by gain
#define MCP3426_ZIN_CM_OHM       25.0e6 // Common-mode impedance from each ADC input to ground
#define RES_VDD_V                 3.3   // Nominal supply; replace with measured board value
#define RES_VDIFF_OFFSET_V        0.0   // Calibrate only after bring-up with known resistors
#define RES_MIN_OHM               1.0e3
#define RES_MAX_OHM               1.0e9

// ---- FDC2214 channel-0 setup
#define FDC2214_POWERUP_DELAY_MS  2     // TI requires >=2 ms after SD goes LOW
#define FDC2214_RESET_DELAY_MS    2     // Bound/reset settling wait
#define FDC2214_CONVERSION_TIMEOUT_MS 20 // Bound sensor task latency so two 14-bit ADC reads fit under 100 ms
#define FDC2214_REFERENCE_CLOCK_HZ 40000000UL // Internal reference-clock nominal rate
#define FDC2214_DEBUG              0     // Set to 1 to log code, inferred frequency and tank capacitance
#ifndef FDC2214_USE_EXTERNAL_CLKIN
#define FDC2214_USE_EXTERNAL_CLKIN 0 // 0 selects the internal oscillator; set 1 only when CLKIN is provided
#endif
#define FDC2214_CONFIG_ACTIVE_CHAN_CH0       (0u << 14)
#define FDC2214_CONFIG_SLEEP_MODE_EN          (0u << 13)
#define FDC2214_CONFIG_RESERVED_BIT12         (1u << 12)
#define FDC2214_CONFIG_SENSOR_ACTIVATE_SEL    (0u << 11)
#define FDC2214_CONFIG_RESERVED_BIT10         (1u << 10)
#define FDC2214_CONFIG_REF_CLK_SRC            ((unsigned)(FDC2214_USE_EXTERNAL_CLKIN) << 9)
#define FDC2214_CONFIG_RESERVED_BIT8          (0u << 8)
#define FDC2214_CONFIG_INTB_DIS               (1u << 7)
#define FDC2214_CONFIG_HIGH_CURRENT_DRV       (0u << 6)
#define FDC2214_CONFIG_RESERVED_LOW           0x0001u
#define FDC2214_CONFIG_CH0                    (FDC2214_CONFIG_ACTIVE_CHAN_CH0 | FDC2214_CONFIG_SLEEP_MODE_EN | \
                                               FDC2214_CONFIG_RESERVED_BIT12 | FDC2214_CONFIG_SENSOR_ACTIVATE_SEL | \
                                               FDC2214_CONFIG_RESERVED_BIT10 | FDC2214_CONFIG_REF_CLK_SRC | \
                                               FDC2214_CONFIG_RESERVED_BIT8 | FDC2214_CONFIG_INTB_DIS | \
                                               FDC2214_CONFIG_HIGH_CURRENT_DRV | FDC2214_CONFIG_RESERVED_LOW)
#define FDC2214_RCOUNT_CH0        0x0000 // TODO: calculate from actual tank and sample-rate requirements
#define FDC2214_SETTLECOUNT_CH0   0x0000 // TODO: determine from actual tank settling time
#define FDC2214_CLOCK_DIVIDERS_CH0 0x0000 // TODO: select FIN/FREF dividers from tank frequency
#define FDC2214_DRIVE_CURRENT_CH0 0x0000 // TODO: set after measuring sensor oscillation amplitude
#define FDC2214_TANK_SETTINGS_VALID 0    // Set to 1 only after all four values above are measured/validated
#define FDC2214_MUX_CONFIG_CH0    0x020D // Autoscan off; retain TI's required reserved/deglitch field values
#define FDC2214_ERROR_CONFIG      0x0000 // No interrupt outputs; poll STATUS instead
#define FDC_TANK_L_UH             0.0   // TODO: populate with measured tank inductance to enable capacitance conversion
#define FDC_TANK_C_PF             0.0   // TODO: populate parasitic/tank capacitance to enable capacitance conversion

// ---- Sampling and detection window
#define SAMPLE_RATE_HZ              10    // 10 Hz; MCP3426 one-shot at 14-bit is 60 SPS
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
