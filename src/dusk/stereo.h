#ifndef DUSK_STEREO_H
#define DUSK_STEREO_H

#include <aurora/aurora.h>
#include <mtx.h>

// J3DTexMtxInfo / J3DModel live at global scope (libs/JSystem). Forward-declare
// here so dusk::stereo can take pointers to them without dragging J3D headers.
struct J3DTexMtxInfo;
class J3DModel;

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

// Inject stereo correction into a J3D "effect" (texgen) matrix that was built
// to project world positions into UV space for sampling the captured frame
// buffer (e.g. water reflections built via C_MTXLightPerspective +
// setWideZoomLightProjection in d_kankyo / d_a_obj_lv3WaterB).
//
// Use the (info, base) overload at the call site -- the actor Draw method that
// builds the texgen matrix only runs once per frame, so a single in-place
// correction would only fix one eye (both eyes share the J3DTexMtxInfo's
// mEffectMtx). The painter funnel re-applies the correct per-eye matrix to
// each registered info between iterations.
//
// The matrix's q row must be (0, 0, -1, 0) so the texgen does a perspective
// divide by view depth. The injected shift bakes:
//   delta_u(d) = -eyeOffsetX * M[0][0] * (1/d - 1/convergence)
// into M[0][2] and M[0][3] so the UV picks up a depth-dependent per-eye shift
// matching the eye-shifted captured framebuffer. At convergence depth the
// shift is zero; closer pops the reflection out, farther recedes it.
//
// No-op when stereo is off. The J3DModel pointer is required because the
// material's display list caches the texgen matrix at build time; rewriting
// mEffectMtx alone doesn't change the cached DL. The painter funnel calls
// model->calcMaterial() + model->diff() per eye to rebuild the DL with the
// per-eye-corrected matrix.
void apply_eye_to_reflection_effect_mtx(Mtx mtx, ::J3DTexMtxInfo* info, ::J3DModel* model);

// Variant for "halo" / player-centered texgens that are built by concatenating
// a C_MTXLightPerspective with a lookAt looking DOWN at the player (e.g. MA20
// in d_kankyo.cpp -- the circular reflection that appears in water at the
// character's feet). The composite matrix has q row (0, -1, 0, player.y)
// instead of (0, 0, -1, 0), so the view-shift's per-eye UV contribution has
// the opposite sign of a standard reflection. This helper bakes in the
// opposite-direction correction so the halo doesn't appear mirrored between
// eyes.
void apply_eye_to_halo_effect_mtx(Mtx mtx, ::J3DTexMtxInfo* info, ::J3DModel* model);

// Returns the per-eye X translation that push_eye_offset applied to the
// view matrix for whichever eye is currently active (s_current_eye).
// Sign matches what push_eye_offset subtracted from viewMtx[0][3]:
// LEFT eye -> -halfSep, RIGHT eye -> +halfSep. Returns 0 when stereo is
// off so callers can unconditionally add it back without branching.
f32 current_eye_offset_x();

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

// Painter-funnel hooks. apply_reflection_corrections_for_eye re-derives each
// registered material's mEffectMtx from its saved base matrix using the given
// eye, so the matrix is correct for whichever eye is about to render next.
// clear_reflection_registry is called at end of cAPIGph_Painter so the next
// frame's actor Draws repopulate from scratch.
void apply_reflection_corrections_for_eye(AuroraEye eye);
void clear_reflection_registry();

} // namespace dusk::stereo

#endif
