#ifndef DUSK_STEREO_H
#define DUSK_STEREO_H

#include <aurora/aurora.h>
#include <mtx.h>

#include "SSystem/SComponent/c_xyz.h"

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

// True when something close to the camera dominates the frame (FP aim, open
// dialog, pause/inventory, item-get sequence, NPC talk). Triggers the
// "Close-Up Eye Sep Scale" reduction this frame. Public so diagnostic /
// debug code can ask the same question dusk::stereo uses internally.
bool is_close_up_focus_active();

// Per-eye amount to ADD to the J2D ortho left/right bounds to give the
// in-game HUD (hearts, rupees, button hints, mini-map) a fixed parallax
// depth. Drives the user's stereoHudDepth slider in pixel-space (1 unit ~=
// 0.1% of viewport width). Returns 0 when stereo is off or hudDepth is 0.
//
// Positive hudDepth = HUD pops in front of the screen plane: the right eye
// view sees the HUD shifted LEFT relative to the left eye, which the brain
// fuses as negative parallax. Apply by adding the returned value to BOTH the
// left and right ortho bounds before setOrtho().
f32 hud_ortho_shift_x();

// Per-eye horizontal screen-pixel shift for a world-space point projected
// through the UNSHIFTED center camera (the projection cached by
// `mDoLib_project` when actor draws run before the per-eye painter loop).
// Add this to a J2D pane translate inside `dComIfGd_draw2DXlu` so world-
// anchored 2D-XLU elements (e.g. boomerang lock cursors) get the same depth
// parallax as the 3D scene around them. Returns 0 when stereo is off, the
// point is behind the camera, or convergence is degenerate.
//
// Formula (derivation matches push_eye_offset's view translate + projMtx
// skew):
//   ΔNDC.x_eye = eyeOffsetX * projMtx[0][0] * (1/z_view + 1/convergence)
//   ΔPixels    = ΔNDC.x_eye * viewport_width / 2
// Sign convention: LEFT eye has eyeOffsetX = -sep/2, RIGHT eye has +sep/2.
// At z_view = -convergence the shift is zero (zero-parallax plane); points
// closer than convergence pop forward, farther points recede.
f32 screen_parallax_x_for_world_pos(const cXyz& world_pos);

// Step the smoothed close-up separation scale once per simulation frame.
// Snaps down to the target instantly when the close-up predicate fires
// (FP aim, dialog, item-get, etc.) so comfort applies the same frame, and
// eases exponentially back to 1.0 when the predicate releases so the world
// doesn't pop wider the instant the trigger ends. Call once per frame
// alongside auto_convergence_tick() before the eye loop.
void closeup_scale_tick();

// Adjust the active convergence each simulation frame based on what the
// player is looking at. Priority chain:
//   1. Z-target lock-on actor distance
//   2. Aim mode (bow/slingshot/clawshot) sight hit-point distance
//   3. Cutscene/event camera lookat distance
//   4. Dialog open -> freeze (skip update, avoids text-box jumps)
//   5. Fallback: depth at screen center via GXPeekZ
// Smoothed with the user's autoConvergenceSmoothing time constant. No-op when
// enableAutoConvergence is false. Call once per simulation frame BEFORE the
// painter funnel starts the eye loop.
void auto_convergence_tick();

} // namespace dusk::stereo

#endif
