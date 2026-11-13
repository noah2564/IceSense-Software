#pragma once

#include <cmath>

namespace resistive_model {

struct Model {
  double supplyV;
  double fixedOhm;
  double referenceTopOhm;
  double referenceBottomOhm;
  double differentialInputOhmAtGain1;
  double commonModeInputOhm;
  double voltageOffsetV;
  double minimumOhm;
  double maximumOhm;
};

struct Estimate {
  double resistanceOhm = NAN;
  bool openCircuit = false;
  bool shortCircuit = false;
};

inline bool voltageDiffForResistance(double sensorOhm, double gain, const Model& model,
                                     double& voltageDiffV) {
  if (!std::isfinite(sensorOhm) || sensorOhm <= 0 || !std::isfinite(gain) || gain <= 0 ||
      !std::isfinite(model.supplyV) || model.supplyV <= 0 || model.fixedOhm <= 0 ||
      model.referenceTopOhm <= 0 || model.referenceBottomOhm <= 0 ||
      model.differentialInputOhmAtGain1 <= 0 || model.commonModeInputOhm <= 0) return false;

  const double sensorConductance = 1.0 / model.fixedOhm + 1.0 / sensorOhm;
  const double sensorCurrent = model.supplyV / model.fixedOhm;
  const double referenceConductance = 1.0 / model.referenceTopOhm + 1.0 / model.referenceBottomOhm;
  const double referenceCurrent = model.supplyV / model.referenceTopOhm;
  const double differentialConductance = gain / model.differentialInputOhmAtGain1;
  const double commonModeConductance = 1.0 / model.commonModeInputOhm;
  const double pDiagonal = sensorConductance + differentialConductance + commonModeConductance;
  const double nDiagonal = referenceConductance + differentialConductance + commonModeConductance;
  const double determinant = pDiagonal * nDiagonal - differentialConductance * differentialConductance;
  if (!(determinant > 0) || !std::isfinite(determinant)) return false;

  const double pVoltage = (sensorCurrent * nDiagonal + differentialConductance * referenceCurrent) / determinant;
  const double nVoltage = (pDiagonal * referenceCurrent + differentialConductance * sensorCurrent) / determinant;
  voltageDiffV = pVoltage - nVoltage + model.voltageOffsetV;
  return std::isfinite(voltageDiffV);
}

inline bool resistanceFromVoltageDiff(double measuredVoltageDiffV, double gain, const Model& model,
                                      Estimate& estimate) {
  estimate = {};
  if (!std::isfinite(measuredVoltageDiffV) || !(model.minimumOhm > 0) ||
      !(model.maximumOhm > model.minimumOhm)) return false;

  double minimumVoltage = 0, maximumVoltage = 0;
  if (!voltageDiffForResistance(model.minimumOhm, gain, model, minimumVoltage) ||
      !voltageDiffForResistance(model.maximumOhm, gain, model, maximumVoltage) ||
      !(maximumVoltage > minimumVoltage)) return false;

  if (measuredVoltageDiffV <= minimumVoltage) {
    estimate.resistanceOhm = model.minimumOhm;
    estimate.shortCircuit = true;
    return true;
  }
  if (measuredVoltageDiffV >= maximumVoltage) {
    estimate.resistanceOhm = model.maximumOhm;
    estimate.openCircuit = true;
    return true;
  }

  const double logMinimum = std::log10(model.minimumOhm);
  const double logMaximum = std::log10(model.maximumOhm);
  double low = logMinimum;
  double high = logMaximum;
  for (int iteration = 0; iteration < 64; iteration++) {
    const double midpoint = (low + high) * 0.5;
    double predictedVoltage = 0;
    if (!voltageDiffForResistance(std::pow(10.0, midpoint), gain, model, predictedVoltage)) return false;
    if (predictedVoltage < measuredVoltageDiffV) low = midpoint;
    else high = midpoint;
  }
  estimate.resistanceOhm = std::pow(10.0, (low + high) * 0.5);
  return std::isfinite(estimate.resistanceOhm);
}

}  // namespace resistive_model
