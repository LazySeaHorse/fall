// Placeholder implementation: links, detects nothing. Replaced by the real DSP detector.
#include "NoteDetector.h"

namespace fall
{
struct Calibration::Impl { };
Calibration::Calibration() : impl (std::make_unique<Impl>()) {}
Calibration::~Calibration() = default;
Calibration::Calibration (const Calibration& o) : impl (std::make_unique<Impl> (*o.impl)) {}
Calibration& Calibration::operator= (const Calibration& o) { *impl = *o.impl; return *this; }
bool Calibration::hasKey (int) const { return false; }
int  Calibration::numKeys() const { return 0; }
void Calibration::clear() {}
juce::MemoryBlock Calibration::toBinary() const { return {}; }
bool Calibration::fromBinary (const juce::MemoryBlock&) { return true; }

struct NoteDetector::Impl { };
NoteDetector::NoteDetector() : impl (std::make_unique<Impl>()) {}
NoteDetector::~NoteDetector() = default;
void NoteDetector::prepare (double, int) {}
void NoteDetector::reset() {}
void NoteDetector::setExpected (const PitchSet&) {}
void NoteDetector::setWatchKeys (const PitchSet&) {}
void NoteDetector::setSensitivity (float) {}
void NoteDetector::setCalibration (const Calibration&) {}
int  NoteDetector::process (const float*, int, DetectedOnset*, int) { return 0; }
float NoteDetector::getInputLevelDb() const noexcept { return -100.0f; }
void NoteDetector::beginCapture (int) {}
bool NoteDetector::captureFinished() const noexcept { return false; }
void NoteDetector::storeCapture (Calibration&) const {}
} // namespace fall
