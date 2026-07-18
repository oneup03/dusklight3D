#ifndef DUSK_STEREO_H
#define DUSK_STEREO_H

#include <aurora/aurora.h>
#include <mtx.h>

#include "SSystem/SComponent/c_xyz.h"

namespace dusk::stereo {

// True when the user-selected stereo mode is anything other than Off.
bool active();

// True when this is the "first" (or only) eye of the per-eye render loop
// this frame -- LEFT in stereo mode, always true in mono. cAPIGph_Painter
// invokes the shared per-eye draw entry point (mDoGph_Painter, which is
// where painterMtd/g_imguiConsole's PreDraw/PostDraw live) once per eye when
// stereo is active. Any once-per-frame logic hosted inside that shared
// entry point (ImGui debug-menu construction, F-key toggles, etc.) must
// guard on this, or it runs twice per frame -- ImGui key-press state
// (IsKeyPressed) only updates once per real frame, so an F-key toggle wired
// through PreDraw() flips on then immediately back off within the same
// frame, making the window flash and vanish instead of staying open.
bool is_first_eye_of_frame();

// Read the current dusk::getSettings() stereo values and push them down to
// Aurora via aurora_set_stereo_config. Call on startup and after any UI change.
void apply_config_from_settings();

// Per-eye projection helpers. push_eye_offset applies an asymmetric-frustum
// shift to camera 0's projMtx for the given eye; pop_eye_offset restores it.
// Pairs must be balanced and used around a single painter invocation.
void push_eye_offset(AuroraEye eye);
void pop_eye_offset();

// Signed per-eye half-separation in view-space X: -sep/2 for LEFT,
// +sep/2 for RIGHT, 0 in mono. Includes the close-up reduction scale.
// Public so per-actor stereo correction code (e.g. d_a_obj_mhole) can
// compute its own geometry-correct matrix shifts.
f32 current_eye_offset_x();

// The horizontal shear push_eye_offset applied to the camera projection for
// the current eye (the exact delta it added to projMtx[0][2]); 0 in mono or
// with degenerate convergence. Screen-space ViewProjmap texgens (the magnet
// field's LightPerspective effect matrix and friends) must add the SAME
// shear to THEIR projection's [0][2], or their scene-capture sampling
// decouples from the per-eye render projection by a constant screen-space
// offset (the "surface floats off the magnet" artifact). The eye TRANSLATION
// needs no texgen-side correction: it already enters the texcoord chain
// through GX_PNMTX0 (view * model), which J3D ConcatView shapes load per eye
// at shape-draw time.
f32 current_projection_shear_x();

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
// alongside fov_scale_tick() before the eye loop.
void closeup_scale_tick();

// Step the rate-limited FoV-aware auto-scale once per simulation frame. TP's
// Fovy swings widely at runtime (hawkeye/telescope zoom, cutscene and
// dialogue framing from ~30 to ~115 degrees) and none of that flows into
// separation today, so the stereo depth effect visibly inflates when the
// camera zooms in and collapses when it zooms out. Reads the live
// camera->view.fovy, compares it against a fixed reference FoV calibrated
// to TP's de-facto default gameplay Fovy (60 degrees -- the value the
// engine itself falls back to more than any other), and rate-limits the
// tan(fov/2) ratio toward that target (see kFovScaleMaxStepPerSec) rather
// than exponentially smoothing it -- TP's own camera-style hand-off (e.g.
// chaseCamera resuming after FP aim/dialog) already blends Fovy back with a
// genuine linear ramp, and an EMA on top of a ramping input trails it by a
// constant lag that then has to visibly "catch up" once the ramp stops. A
// rate limiter tracks that ramp with zero steady-state lag while still
// spreading a genuine instantaneous FoV cut over a handful of frames.
// Convergence is a distance and deliberately excluded from this scale (see
// effective_convergence()) -- only separation needs FoV compensation.
// Call once per frame alongside closeup_scale_tick() before the eye loop.
void fov_scale_tick();

// Read-only snapshot of internal smoothing state, for the ImGui camera
// debug overlay. Not read by any gameplay/render logic -- exists purely so
// live values (fov_scale, closeup_scale, the combined separation multiplier,
// convergence) can be watched in real time while diagnosing timing issues.
struct DebugState {
    f32 fovScale;        // s_fov_scale (rate-limited, see fov_scale_tick)
    f32 closeupScale;    // s_smoothed_closeup_scale
    f32 separationScale; // effective_separation_scale() -- product of closeup * fov scales
    f32 convergence;     // effective_convergence() -- what push_eye_offset actually uses
};
DebugState debug_state();

} // namespace dusk::stereo

#endif
