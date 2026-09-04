#ifndef DUSK_STEREO_H
#define DUSK_STEREO_H

#include <aurora/aurora.h>
#include <mtx.h>

#include "SSystem/SComponent/c_xyz.h"

// -----------------------------------------------------------------------------
// Stereo 3D parameterization: CLIP SPACE
// -----------------------------------------------------------------------------
// The primary stereo knob is `separation`, a dimensionless CLIP-SPACE quantity
// (the NVIDIA / 3Dmigoto convention, `x += separation * (w - convergence)`),
// not a physical eye distance in world units. It is defined by:
//
//     per-eye NDC x is offset by +/-separation at infinity, and NDC x spans the
//     full screen over [-1, +1], therefore
//
//         TOTAL BACKGROUND DISPARITY = separation * SCREEN WIDTH
//
// so `separation = 0.05` puts objects at infinity 5% of the screen width apart.
// That is a quantity the user can see, expressed in the same terms as the
// constraint that actually bounds it: uncrossed disparity larger than the
// viewer's IPD forces the eyes to diverge and cannot be fused at any comfort
// level, giving a hard ceiling of roughly IPD / screen_width (~0.105 on a 27"
// 16:9 panel).
//
// The two consequences that matter for everything below:
//
//   * THE PROJECTION SHEAR IS INVARIANT UNDER CONVERGENCE, and equals the knob
//     itself: projMtx[0][2] += dir * separation. Convergence does not appear.
//   * THE VIEW-SPACE EYE OFFSET IS DERIVED, NOT STORED. It is
//     separation * tan_half_h * convergence per eye, so it moves with BOTH the
//     live FoV and the live convergence. Nothing may treat it as a constant.
//
// This replaces an earlier eye-separation-in-world-units parameterization. The
// two are algebraically identical, but the old one needed a reference-FoV
// slider plus a rate-limited tan(fov/2) auto-scale to stay usable under TP's
// wildly varying Fovy -- and that whole apparatus was multiplying in a term the
// projection's own 1/tan_half_h immediately divided back out. It cancels
// exactly, so it is gone (see migrate_legacy_config for the one-time conversion
// of saved profiles).
//
// The one behaviour that genuinely changed: CONVERGENCE NO LONGER CHANGES
// BACKGROUND DEPTH. Background disparity is `separation`, full stop; moving
// convergence only moves what sits in front of the screen plane.
// -----------------------------------------------------------------------------

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

// One-time conversion of a profile saved under the old eye-separation-in-
// world-units parameterization to the clip-space `stereoSeparation` key.
// Fires only when the new key is absent from the config file while the old
// one is present, so it runs exactly once per profile and never touches an
// already-converted one. Call once at startup, BEFORE the first
// apply_config_from_settings().
void migrate_legacy_config();

// Read the current dusk::getSettings() stereo values and push them down to
// Aurora via aurora_set_stereo_config.
//
// MUST be called once per frame (cAPIGph_Painter does, before the eye loop),
// not just on UI changes: the AuroraStereoConfig::eyeSeparation field is the
// DERIVED world-unit eye baseline, which under the clip-space parameterization
// moves with the live FoV and the live (close-up-scaled) convergence. Aurora's
// GX-layer texgen corrections read that value directly instead of the game's
// camera state, so a stale one decouples them from the real per-eye offset --
// the "surface floats off the magnet" class of artifact.
void apply_config_from_settings();

// Per-eye projection helpers. push_eye_offset applies an asymmetric-frustum
// shift to camera 0's projMtx for the given eye; pop_eye_offset restores it.
// Pairs must be balanced and used around a single painter invocation.
void push_eye_offset(AuroraEye eye);
void pop_eye_offset();

// The user's clip-space separation for this frame: total background disparity
// as a fraction of screen width. 0 when stereo is off. Unlike the old
// world-unit separation this carries NO scales -- close-up comfort is expressed
// as a convergence pull-in (see effective_convergence's contract in stereo.cpp),
// which is what it always physically was.
f32 current_separation();

// Signed per-eye view-space X offset of the camera: -baseline/2 for LEFT,
// +baseline/2 for RIGHT, 0 in mono. DERIVED, not a setting:
//
//     eye_offset_x = dir * separation * tan_half_h * convergence
//
// with tan_half_h read live as 1 / projMtx[0][0] (C_MTXPerspective builds
// m[0][0] = cot(fovy/2) / aspect, which is exactly 1 / tan(half horizontal
// FoV)). It therefore changes whenever the camera zooms or the close-up
// convergence pull-in engages -- do not cache it across frames or treat it as
// a physical constant. Public so per-actor stereo correction code can compute
// its own geometry-correct matrix shifts.
f32 current_eye_offset_x();

// The horizontal shear push_eye_offset applied to the camera projection for
// the current eye (the exact delta it added to projMtx[0][2]); 0 in mono.
// Under clip space this is simply -dir * separation -- no FoV term, no
// convergence term. Screen-space ViewProjmap texgens (the magnet field's
// LightPerspective effect matrix and friends) must add the SAME shear to
// THEIR projection's [0][2], or their scene-capture sampling decouples from
// the per-eye render projection by a constant screen-space offset (the
// "surface floats off the magnet" artifact). The eye TRANSLATION needs no
// texgen-side correction: it already enters the texcoord chain through
// GX_PNMTX0 (view * model), which J3D ConcatView shapes load per eye at
// shape-draw time.
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
// Under clip space, substituting the derived eyeOffsetX = dir * separation *
// tan_half_h * convergence makes CONVERGENCE CANCEL OUT of that expression
// entirely, leaving -dir * separation * tan_half_h * srt_z_view. That is not a
// simplification of convenience: the correction compensates a gap created by
// the projection shear, and the shear is convergence-invariant, so the
// correction has to be too.
//
// Returns 0 when stereo is off or separation is zero.
f32 refraction_skew_correction_x(f32 srt_z_view);

// True when something close to the camera dominates the frame (FP aim, open
// dialog, pause/inventory, item-get sequence, NPC talk). Triggers the
// automatic convergence pull-in this frame. Public so diagnostic / debug
// code can ask the same question dusk::stereo uses internally.
bool is_close_up_focus_active();

// Per-eye amount to ADD to the J2D ortho left/right bounds to give the
// in-game HUD (hearts, rupees, button hints, mini-map) a fixed parallax
// depth. Drives the user's stereoHudDepth slider in pixel-space (1 unit ~=
// 0.1% of viewport width). Returns 0 when stereo is off or hudDepth is 0.
//
// This slider was already a screen-width fraction and so needs no clip-space
// conversion -- it is the same kind of quantity `separation` now is. Note the
// corollary from the clip-space form: a UI layer placed at a fixed MULTIPLE of
// convergence has a convergence-invariant shift, so this being independent of
// the convergence slider is correct, not an oversight.
//
// Positive hudDepth = HUD sits BEHIND the screen plane (pushed into the
// screen); negative brings it forward, out of the screen. Apply by adding the
// returned value to BOTH the left and right ortho bounds before setOrtho().
//
// The sign convention lives entirely inside this function, so the J2D ortho
// sites and the world-anchored 2D elements that re-add this value to cancel it
// track any change to it automatically -- do not re-negate at a call site.
f32 hud_ortho_shift_x();

// Per-eye horizontal screen-pixel shift for a world-space point projected
// through the UNSHIFTED center camera (the projection cached by
// `mDoLib_project` when actor draws run before the per-eye painter loop).
// Add this to a J2D pane translate inside `dComIfGd_draw2DXlu` so world-
// anchored 2D-XLU elements (e.g. boomerang lock cursors) get the same depth
// parallax as the 3D scene around them. Returns 0 when stereo is off, the
// point is behind the camera, or convergence is degenerate.
//
// Clip-space form (identical to the projection's own per-eye disparity, with
// depth = -z_view so it reads as a positive distance):
//   ΔNDC.x_eye = -dir_shear * separation * (convergence/depth - 1)
//   ΔPixels    = ΔNDC.x_eye * viewport_width / 2
// Note there is NO projMtx[0][0] term: the FoV factor the old world-unit form
// carried here cancels against the one hidden in the eye offset.
//
// SIGN: this is the COMPOSITOR/screen-space convention, which is the OPPOSITE
// of the projection shear's. They answer different questions -- "which pixel
// column offset moves this eye's copy toward or away from center" versus
// "which direction bends this eye's frustum" -- and there is no rule that they
// agree. Verify them independently: with an object nearer than convergence the
// world must show CROSSED disparity (left-eye image displaced RIGHT of the
// right-eye image), while a depth-tracked 2D overlay must move in the SAME
// apparent direction as the geometry at its depth.
//
// At depth == convergence the shift is zero (zero-parallax plane); closer
// points pop forward, farther points recede.
f32 screen_parallax_x_for_world_pos(const cXyz& world_pos);

// Step the automatic close-up convergence pull-in once per simulation frame.
// Snaps to the pulled-in value instantly when the close-up predicate fires
// (FP aim, dialog, item-get, etc.) so comfort applies the same frame, and
// eases exponentially back out when the predicate releases so the screen plane
// doesn't jump away the instant the trigger ends. Call once per frame before
// the eye loop.
//
// This is the ONLY smoother in the stereo path now -- the old rate-limited FoV
// auto-scale ran alongside it and the two settling on different timescales read
// as jank even though each was individually smooth. Clip space deletes the FoV
// scale outright (there is nothing left for it to compensate), which leaves
// exactly one thing in motion.
void closeup_scale_tick();

// Step the depth-buffer-driven auto-convergence loop once per simulation frame,
// AFTER closeup_scale_tick (whose result is this loop's ceiling) and before the
// eye loop. Opt-in via game.stereoAutoConvergence; a no-op that resets all
// internal state when disabled, so effective_convergence() falls straight back
// to the manual/close-up value with no ease-out.
//
// Reads Aurora's rate-limited depth snapshot (the same GPU->CPU readback GXPeekZ
// uses), takes a low percentile of block minima over the middle of the frame as
// "the nearest object that actually covers pixels", and pulls convergence in
// only far enough to hold that object's pop-out under the user's comfort limit.
// The manual convergence slider is always the ceiling.
//
// Under clip space this needs NO companion separation adjustment. In the old
// world-unit form, pulling convergence in inflated every far object's disparity
// (background disparity was sep/conv), so the background popped exactly when
// near protection engaged and had to be cancelled by co-scaling separation.
// Background disparity is now `separation` outright, which convergence does not
// touch, so there is nothing to lock and nothing to hand back to the caller.
void auto_convergence_tick();

// Read-only snapshot of internal state, for the ImGui camera debug overlay.
// Not read by any gameplay/render logic -- exists purely so live values can be
// watched in real time while diagnosing timing issues.
struct DebugState {
    f32 separation;        // clip-space separation == background disparity / screen width
    f32 closeupScale;      // s_smoothed_closeup_scale (a CONVERGENCE multiplier)
    f32 convergence;       // effective_convergence() -- what push_eye_offset actually uses
    f32 eyeBaseline;       // DERIVED total eye separation in world units, this frame
    f32 tanHalfH;          // live tan(half horizontal FoV) == 1 / projMtx[0][0]
    f32 manualConvergence; // the ceiling: slider * close-up scale, before auto-convergence
    f32 autoConvNearDepth; // smoothed nearest-object depth in world units; 0 = no estimate
    bool autoConvEngaged;  // auto-convergence is currently driving `convergence`
    // False while enabled means the depth snapshot never arrived (device without
    // the required features, or nothing captured yet) -- distinguishes "doing
    // nothing because there's nothing to do" from "not working".
    bool autoConvDepthAvailable;
};
DebugState debug_state();

} // namespace dusk::stereo

#endif
