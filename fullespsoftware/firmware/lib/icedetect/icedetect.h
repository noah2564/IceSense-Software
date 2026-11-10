// Ice detection: C++ port of algorithm/icedetect.py (the reference implementation).
// Keep this file free of Arduino/ESP-IDF dependencies so it can be tested on a PC.
// Change the logic in Python first, regenerate algorithm/testvectors.csv, then port the change here.
#pragma once
#include <cmath>
#include <cstdint>

namespace icedetect {

#define ICEDETECT_NAME "icedetect"
#define ICEDETECT_VERSION "0.1"

// Numeric values are part of the output concept (docs/outputs.md): do not renumber.
enum class State : uint8_t { Unknown = 0, Clear = 1, Condensation = 2, Frost = 3, Ice = 4, Melting = 5 };

const char* stateName(State s);

struct Params {
  double freeze_c = 0.5;        // coil at or below this: deposits are frozen
  double deposit_pct = 5.0;     // |capacitance change| at or above this: something is on the electrode
  double ice_pct = 20.0;        // at or above this while frozen: thick deposit (Ice instead of Frost)
  double liquid_log10 = 5.5;    // log10(ohm) below this: a liquid film is present
  double mm_per_pct = 0.10;     // thickness per percent capacitance change (UNCALIBRATED)
  double max_mm = 15.0;         // clamp for the thickness estimate
  int confirm_windows = 2;      // a new state must be seen this many windows in a row
  double defrost_on_mm = 2.0;   // request defrost at or above this thickness ...
  int defrost_on_windows = 3;   // ... for this many windows in a row
  double defrost_off_mm = 0.5;  // release the request at or below this thickness ...
  int defrost_off_windows = 2;  // ... for this many windows in a row
};

struct Input {
  double cap_delta_pct = NAN;   // NaN = not available (no baseline)
  double res_log10 = NAN;       // NaN = not available
  double temp_c = NAN;          // NaN = not available (sensor fault)
};

struct Output {
  State state = State::Unknown;
  State raw_state = State::Unknown;   // classification of this window before confirmation
  double thickness_mm = 0.0;
  bool defrost_request = false;
};

class Detector {
 public:
  explicit Detector(const Params& p = Params()) : p_(p) { reset(); }
  void setParams(const Params& p) { p_ = p; }
  const Params& params() const { return p_; }
  void reset();
  Output step(const Input& in);
  State state() const { return state_; }

 private:
  State classify(bool haveCap, double cap, bool haveT, double t, bool haveR, double r) const;
  Params p_;
  State state_ = State::Unknown;
  State cand_ = State::Unknown;
  int candN_ = 0;
  int onN_ = 0;
  int offN_ = 0;
  bool defrost_ = false;
};

}  // namespace icedetect
