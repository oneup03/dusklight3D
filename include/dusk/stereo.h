#ifndef DUSK_STEREO_H
#define DUSK_STEREO_H

#include <aurora/aurora.h>
#include <mtx.h>

namespace dusk::stereo {

// True when the user-selected stereo mode is anything other than Off.
bool active();

// Read the current dusk::getSettings() stereo values and push them down to
// Aurora via aurora_set_stereo_config. Call on startup and after any UI change.
void apply_config_from_settings();

// Per-eye projection helpers. push_eye_offset applies an asymmetric-frustum
// shift to camera 0's projMtx for the given eye; pop_eye_offset restores it.
// Pairs must be balanced and used around a single painter invocation.
void push_eye_offset(AuroraEye eye);
void pop_eye_offset();

} // namespace dusk::stereo

#endif
