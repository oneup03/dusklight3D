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

// Depth-aware per-particle correction for any screen-space-projection
// texgen built like `mtx = LightPerspective * camera_space_srt` and
// sampled at billboard corners (the JPA refraction pattern in
// loadPrj/loadPrjAnm). Returns the value to ADD to srt[0][3] before the
// concat. `srt_z_view` is the camera-space Z of the particle (= negative
// depth).
//
// Derivation: the texgen UV's natural per-eye drift is
//   ΔUV_texgen = -mPrjMtx[0][0] * eyeOffsetX / depth
// because mPosCamMtx shifts srt[0][3] by -eyeOffsetX and q in the
// perspective divide ≈ depth. Geometry drifts off-axis as
//   ΔUV_geom = -0.5 * eyeOffsetX * projMtx[0][0] * (1/depth - 1/convergence)
// With C_MTXLightPerspective's scaleS=0.5, mPrjMtx[0][0] = 0.5 *
// projMtx[0][0], so the gap reduces to a depth-independent constant
//   +0.5 * eyeOffsetX * projMtx[0][0] / convergence.
// Adding -eyeOffsetX * srt[2][3] / convergence to srt[0][3] produces
// exactly that constant after the mPrjMtx[0][0]/depth divide.
//
// Returns 0 when stereo is off or convergence is degenerate.
f32 refraction_skew_correction_x(f32 srt_z_view);

} // namespace dusk::stereo

#endif
