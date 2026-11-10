#include "icedetect.h"

#include <algorithm>

namespace icedetect {

const char* stateName(State s) {
  switch (s) {
    case State::Clear: return "clear";
    case State::Condensation: return "condensation";
    case State::Frost: return "frost";
    case State::Ice: return "ice";
    case State::Melting: return "melting";
    default: return "unknown";
  }
}

static bool isSolid(State s) { return s == State::Frost || s == State::Ice || s == State::Melting; }

void Detector::reset() {
  state_ = State::Unknown;
  cand_ = State::Unknown;
  candN_ = 0;
  onN_ = 0;
  offN_ = 0;
  defrost_ = false;
}

State Detector::classify(bool haveCap, double cap, bool haveT, double t, bool haveR, double r) const {
  if (!haveCap || !haveT) return State::Unknown;   // no baseline or no temperature: cannot judge
  const bool deposit = cap >= p_.deposit_pct;
  const bool liquid = haveR && r < p_.liquid_log10;
  const bool frozenRegime = t <= p_.freeze_c;
  const bool prevSolid = isSolid(state_);

  if (prevSolid && liquid && deposit) return State::Melting;
  if (!deposit && !liquid) return State::Clear;
  if (frozenRegime) {
    if (liquid) return State::Condensation;
    return cap >= p_.ice_pct ? State::Ice : State::Frost;
  }
  if (deposit && prevSolid) return State::Melting;
  return State::Condensation;
}

Output Detector::step(const Input& in) {
  const bool haveCap = std::isfinite(in.cap_delta_pct);
  const bool haveT = std::isfinite(in.temp_c);
  const bool haveR = std::isfinite(in.res_log10);
  const double cap = haveCap ? std::fabs(in.cap_delta_pct) : 0.0;

  const State raw = classify(haveCap, cap, haveT, in.temp_c, haveR, in.res_log10);

  // Confirmation: the reported state changes only after the same new state was seen
  // confirm_windows times in a row.
  if (raw == state_) {
    candN_ = 0;
  } else {
    if (raw == cand_) {
      candN_++;
    } else {
      cand_ = raw;
      candN_ = 1;
    }
    if (candN_ >= p_.confirm_windows) {
      state_ = raw;
      candN_ = 0;
    }
  }

  double thickness = 0.0;
  if (isSolid(state_) && haveCap) thickness = std::min(cap * p_.mm_per_pct, p_.max_mm);

  // Defrost request: latched, hysteresis on the thickness estimate; none while the state is unknown.
  if (state_ == State::Unknown) {
    defrost_ = false;
    onN_ = 0;
    offN_ = 0;
  } else if (!defrost_) {
    onN_ = thickness >= p_.defrost_on_mm ? onN_ + 1 : 0;
    if (onN_ >= p_.defrost_on_windows) {
      defrost_ = true;
      onN_ = 0;
      offN_ = 0;
    }
  } else {
    offN_ = thickness <= p_.defrost_off_mm ? offN_ + 1 : 0;
    if (offN_ >= p_.defrost_off_windows) {
      defrost_ = false;
      offN_ = 0;
      onN_ = 0;
    }
  }

  Output out;
  out.state = state_;
  out.raw_state = raw;
  out.thickness_mm = thickness;
  out.defrost_request = defrost_;
  return out;
}

}  // namespace icedetect
