#include "dusk/stereo.h"

#include "dusk/logging.h"
#include "dusk/settings.h"

#include "d/d_com_inf_game.h"
#include "d/d_msg_object.h"
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_player.h"
#include "f_op/f_op_camera_mng.h"
#include "m_Do/m_Do_graphic.h"
#include "m_Do/m_Do_mtx.h"
#include "mtx.h"

#include <aurora/gfx.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace dusk::stereo {

namespace {

constexpr int kMaxCameras = 1; // TP uses a single camera slot (mCameraInfo[1]).

struct SavedCameraState {
    bool valid;
    f32 projM02;          // projMtx[0][2]
    f32 viewM03;          // viewMtx[0][3] (camera X translation in view space)
    f32 viewNoTransM03;   // viewMtxNoTrans[0][3] (defensively saved)
    Mtx invViewMtx;       // full save: derived from viewMtx, must restore exactly
    Mtx44 projViewMtx;    // full save: derived from projMtx * viewMtx
    cXyz lookatEye;       // world-space camera position
    cXyz lookatCenter;    // world-space camera target
};

SavedCameraState s_saved[kMaxCameras]{};

// Tracks which eye the painter most recently pushed. Used by
// refraction_skew_correction_x.
AuroraEye s_current_eye = AURORA_EYE_LEFT;

// Null-safe wrapper for dMsgObject_isTalkNowCheck(). The inline ultimately
// derefs dMsgObject_c::mpRenProc, which is NULL between scenes (set by
// _delete before dComIfGp_setMsgObjectClass(NULL)) and during the brief
// window inside dMsgObject_Create between setMsgObjectClass(obj) and
// obj->_create() that allocates mpRenProc. Production call sites (timer /
// meter / NPC actors) only run after the msg object is fully constructed,
// but stereo queries run from the camera/draw paths which are alive on
// title-screen and scene-transition frames where mMsgObjectClass is NULL.
bool is_text_box_active() {
    if (dComIfGp_getMsgObjectClass() == nullptr) {
        return false;
    }
    return dMsgObject_isTalkNowCheck();
}

// Smoothed close-up convergence multiplier (0..1). closeup_scale_tick() snaps
// this down instantly when the close-up predicate becomes true (so comfort
// kicks in immediately when you draw a bow or open a textbox) and eases it
// back up toward 1.0 with an exponential roll-off when the predicate releases.
// Read by effective_convergence().
//
// Under the OLD world-unit parameterization this scaled the stored separation
// AND the convergence together -- which, once expanded, left the projection
// shear completely unchanged (the two occurrences of the scale cancelled) and
// only shrank the view-space eye baseline. In other words it was ALWAYS a
// convergence pull-in wearing a separation's clothes. Clip space makes that
// explicit: separation is left alone and this multiplies convergence only,
// which reproduces the old on-screen disparity frame-for-frame (both forms are
// exactly linear in this scale) while finally saying what it does.
f32 s_smoothed_closeup_scale = 1.0f;

// Exit roll-off time constant. ~1.5s gives a slow, unhurried expansion so
// the screen plane doesn't jump back out the instant a textbox closes or a
// weapon is stowed. Combined with the per-frame dt (~1/60s) via
// kAssumedFrameDt below.
//
// Deliberately an exponential ease on the scale, i.e. on CONVERGENCE, and not
// the reciprocal-convergence ease the depth-buffer-driven auto-convergence
// literature calls for. That advice exists because a control loop solving
// conv_target = z_near * (1 + 2*target/separation) has a target that itself
// races toward zero as an object approaches, and a plain lerp on convergence
// then lunges over the last stretch. This is not that: it is a two-state
// comfort predicate with a FIXED endpoint, and in clip space the visible
// disparity d = separation * (convergence/z - 1) is exactly LINEAR in
// convergence -- so an exponential ease here is an exponential ease on the
// quantity the eye actually tracks. Smoothing 1/convergence instead would
// crawl at first and lunge at the end, which is the failure that advice is
// trying to prevent.
constexpr f32 kCloseupExitTimeConstSec = 1.5f;
constexpr f32 kAssumedFrameDt = 1.0f / 60.0f;

// ---------------------------------------------------------------------------
// One-time legacy-config conversion constants.
//
// These calibrate the exact algebraic conversion from the old
// eye-separation-in-world-units form to clip space:
//
//     shear_old = (sep_wu / 2) * projMtx[0][0] / convergence
//               = sep_wu / (2 * convergence * tan_half_h)
//     shear_new = separation
//  => separation = sep_wu / (2 * conv_wu * tan_half_h_ref)
//
// so the conversion needs the tan(half HORIZONTAL FoV) the old sliders were
// tuned at. C_MTXPerspective takes a VERTICAL fovy plus an aspect and builds
// m[0][0] = cot(fovy/2)/aspect, so tan_half_h = tan(fovy/2) * aspect and both
// halves of that need an anchor.
//
// 60 degrees is TP's de-facto default gameplay Fovy -- not a named engine
// constant (FoV is set per camera-style / per-scene throughout d_camera.cpp)
// but by far the most common fallback (initial camera state, most dialogue and
// idle camera resets, most event-camera defaults). 16:9 is the aspect this
// widescreen port is tuned and played at; the live aspect varies with the
// window, which is precisely one of the hidden dependencies clip space removes
// (an ultrawide window used to silently weaken the 3D effect).
// ---------------------------------------------------------------------------
constexpr f32 kLegacyReferenceFovDeg = 60.0f;
constexpr f32 kLegacyReferenceAspect = 16.0f / 9.0f;
constexpr f32 kDegToRad = 0.017453292519943295f;

// ---------------------------------------------------------------------------
// Auto-convergence (depth-buffer-driven).
//
// Goal: keep the NEAREST significant on-screen object's pop-out under a comfort
// budget without the player chasing the convergence slider, and without the
// background silently flattening when the pull-in engages.
//
// That last clause is free here and is the reason this is worth building now
// rather than under the old parameterization. In world-unit separation,
// pulling convergence toward the player also grew every FAR object's disparity
// (background disparity was sep/conv), so the background visibly popped exactly
// when near-object protection kicked in -- the fix was to co-scale separation
// by conv_target/conv_manual and hand the caller a `depth_scale` to apply. In
// clip space, background disparity IS `separation` and convergence does not
// touch it, so there is nothing to lock: no co-scaling, no depth_scale, and the
// loop reduces to a single closed form.
//
// Depth comes from Aurora's existing depth-peek snapshot (the same rate-limited
// GPU->CPU readback GXPeekZ uses, which TP already drives from d_drawlist's
// occlusion checks): a CPU-side grid at the logical GC framebuffer size, valued
// in GX Z24 with 0 at the near plane and 0x00FFFFFF at the far plane. It is
// captured on the frame's LAST render pass, which is safe here only because
// J2D draws with depth writes disabled (J2DGrafContext's
// GXSetZMode(GX_FALSE, GX_LEQUAL, GX_FALSE)) -- so the grid holds the 3D scene
// and never the HUD. If 2D drawing ever starts writing depth, this loop starts
// seeing the HUD at the near plane and will slam convergence to its floor.
// ---------------------------------------------------------------------------

// Region of interest: the middle 85% x 90% of the frame. Trimming the border
// excludes HUD-hugging edge geometry and the slivers of near wall/floor that
// clip the frame edge when the camera is tight to a surface -- neither is what
// the player is looking at, and both would otherwise dominate a "nearest"
// query.
constexpr f32 kAutoConvRoiX0 = 0.075f;
constexpr f32 kAutoConvRoiX1 = 0.925f;
constexpr f32 kAutoConvRoiY0 = 0.05f;
constexpr f32 kAutoConvRoiY1 = 0.95f;

// Block-min grid over that ROI. 48x32 blocks of roughly 11x12 source texels
// each; the reduction reads every texel in the ROI (~220k) once per LANDED
// snapshot, which the generation check below limits to <=30Hz.
constexpr u32 kAutoConvCols = 48;
constexpr u32 kAutoConvRows = 32;

// Index into the sorted block minima taken as "nearest". Deliberately a low
// PERCENTILE and not a literal min: one stray texel (a particle, a muzzle
// flash, a sliver of geometry) would otherwise read as "an object is 2cm away"
// and slam convergence to the floor. At 1536 blocks this is index 30, so a real
// object has to cover roughly 2% of the ROI to move the estimate -- i.e. "the
// nearest object that actually covers pixels".
constexpr u32 kAutoConvPercentileIndex = (kAutoConvCols * kAutoConvRows) / 50;

// Temporal median window over the raw per-snapshot estimate. A second,
// independent outlier filter in TIME rather than space: it catches the
// single-snapshot spikes the spatial percentile alone won't (a projectile
// crossing the camera, one frame of a loading/teleport transition).
constexpr int kAutoConvMedianWindow = 5;

// Asymmetric EMA rates on the median. Perceptually these are not
// interchangeable: something suddenly getting closer must pull convergence in
// QUICKLY (that is the entire comfort case), while something receding should
// relax SLOWLY, or convergence chases every small recession and the whole
// scene reads as swimmy.
constexpr f32 kAutoConvEmaNear = 0.20f;
constexpr f32 kAutoConvEmaFar = 0.05f;

// RELATIVE deadband on that EMA, not an absolute-unit one: a fixed threshold in
// world units is simultaneously too twitchy up close and too sluggish at range.
constexpr f32 kAutoConvDeadband = 0.005f;

// Absolute floor on the auto pull-in, in world units, combined with a
// near-plane-derived one. One convergence slider step.
constexpr f32 kAutoConvMinConvergence = 25.0f;
constexpr f32 kAutoConvNearPlaneMultiple = 2.0f;

// Scratch grid for the block-min reduction. Sized once; never reallocated.
std::vector<u32> s_autoconv_grid;

// Raw per-snapshot nearest-depth estimates, for the temporal median.
std::array<f32, kAutoConvMedianWindow> s_autoconv_history{};
int s_autoconv_history_count = 0;
int s_autoconv_history_pos = 0;

// Smoothed nearest-object depth in world units. <= 0 means "no estimate yet".
f32 s_autoconv_z_ema = 0.0f;

// Smoothed 1/convergence. See auto_convergence_tick for why the smoothing runs
// in reciprocal space HERE but not in closeup_scale_tick.
f32 s_autoconv_inv_convergence = 0.0f;

// Convergence the loop is currently publishing, and whether it has anything
// worth publishing. When false the whole feature is transparent and
// effective_convergence() falls through to the manual/close-up value.
f32 s_autoconv_convergence = 0.0f;
bool s_autoconv_engaged = false;

// Generation of the last depth snapshot actually reduced, so a frame that sees
// no new data does no work.
uint64_t s_autoconv_generation = 0;

// Sticky diagnostic: set when the loop wanted depth but the snapshot machinery
// never produced any. Surfaced in the ImGui overlay so "auto-convergence does
// nothing" is distinguishable from "auto-convergence has nothing to do".
bool s_autoconv_depth_available = false;

AuroraStereoMode current_mode() {
    return static_cast<AuroraStereoMode>(static_cast<int>(getSettings().game.stereoMode.getValue()));
}

bool should_reduce_separation_for_closeups();

// The user's manual convergence after the predicate-driven close-up pull-in.
// This is the CEILING both automatic adjustments respect: the close-up scale
// never exceeds 1.0 and auto-convergence never targets above this, so between
// them the screen plane only ever moves CLOSER than what the user asked for.
// That keeps both features reading as protection rather than "the slider
// doesn't work".
f32 closeup_convergence() {
    return getSettings().game.stereoConvergence.getValue() * s_smoothed_closeup_scale;
}

// The convergence the rest of the pipeline should use this frame. Cheap: the
// depth-driven loop resolves once per frame in auto_convergence_tick() and
// parks its answer, so every call site inside a frame agrees by construction
// and the reduction can't accidentally run per-actor.
//
// Centralized so push_eye_offset, current_eye_offset_x and
// screen_parallax_x_for_world_pos agree.
f32 effective_convergence() {
    if (s_autoconv_engaged) {
        return s_autoconv_convergence;
    }
    return closeup_convergence();
}

// GX Z24 -> view-space distance in world units.
//
// Aurora normalizes both its forward and reversed-Z configurations to the same
// Z24 convention (0 near, 0x00FFFFFF far), and that normalized value is exactly
// the forward device depth t, which relates to distance by
//     t = [F/(F-n)] * (1 - n/depth)   =>   depth = n*F / (F - t*(F-n))
// for a C_MTXPerspective-built projection. Note this is a hyperbola: t is
// tightly packed near the camera, which is precisely where this loop needs
// resolution, so a 24-bit depth value is far more than enough here.
f32 z24_to_view_depth(u32 z24, f32 nearZ, f32 farZ) {
    const f32 t = static_cast<f32>(z24) * (1.0f / 16777215.0f);
    const f32 denom = farZ - t * (farZ - nearZ);
    if (denom <= 0.0001f) {
        return farZ;
    }
    return nearZ * farZ / denom;
}

void auto_convergence_reset() {
    s_autoconv_history_count = 0;
    s_autoconv_history_pos = 0;
    s_autoconv_z_ema = 0.0f;
    s_autoconv_inv_convergence = 0.0f;
    s_autoconv_convergence = 0.0f;
    s_autoconv_engaged = false;
    s_autoconv_generation = 0;
    s_autoconv_depth_available = false;
}

// Live tan(half horizontal FoV), read straight off the projection matrix
// rather than recomputed from fovy + aspect. C_MTXPerspective sets
// m[0][0] = cot(fovy/2) / aspect, which is exactly 1 / tan_half_h, so this
// tracks whatever the engine actually built -- including any per-scene aspect
// or FoV handling we don't model. Returns 0 if the projection is degenerate or
// there is no camera.
f32 current_tan_half_h() {
    const camera_process_class* camera = dComIfGp_getCamera(0);
    if (camera == nullptr) {
        return 0.0f;
    }
    const f32 p00 = camera->view.projMtx[0][0];
    if (p00 <= 0.0001f) {
        return 0.0f;
    }
    return 1.0f / p00;
}

// True when something close to the camera dominates the frame and full
// stereo depth would be uncomfortable: first-person aim modes (bow,
// slingshot, clawshot, dominion rod, hookshot) and any open dialog/message
// box. We use the same pull-in for both since both want the same "bring the
// screen plane to the near subject" treatment.
//
// Whole-frame approach: J3D's deferred render uses cached pointers to the
// camera matrix, so flipping matrices mid-frame for a single actor has no
// effect on what actually ends up on-screen. The scale must be set before
// push_eye_offset for the eye loop.
bool should_reduce_separation_for_closeups() {
    // Predicates kept narrow + safe (avoiding the msg-object internals that
    // deref nullable jmessage_tReference / jmessage_tRenderingProcessor
    // pointers between messages):
    //  - dMsgObject_isTalkNowCheck: the universal "any text box is currently
    //    showing" check (msg-object status != 1 = idle). Catches NPC dialog,
    //    Midna whispers, narrator strings like the "it's a monster!" reaction
    //    when wolf Link enters town -- none of which set mItemInfo.mMesgStatus,
    //    so dComIfGp_getMesgStatus() returns 0 for them (the field is never
    //    written anywhere in the codebase).
    //  - isPauseFlag: pause menu / inventory / map / collection.
    //  - mSight.getDrawFlg + checkBowAnime: any aim mode (FP bow/slingshot/
    //    clawshot/dominion rod, plus third-person boomerang/whistle target-
    //    lock). User wants ALL of these to use the close-up pull-in.
    //  - Talk / item-get / treasure procs catch the *full* close-up sequences
    //    -- the item-get animation runs for ~2s before the text box appears,
    //    and Midna-by-Z runs through PROC_TALK before the message screen does.
    //    Using the proc enum gets the whole arc, not just the text-box.
    if (is_text_box_active() || dComIfGp_isPauseFlag()) {
        return true;
    }
    daPy_py_c* player = dComIfGp_getLinkPlayer();
    if (player == nullptr) {
        return false;
    }
    daAlink_c* alink = static_cast<daAlink_c*>(player);
    // mSight.getDrawFlg covers bow/slingshot/clawshot/hookshot FP aim AND
    // the dominion-rod over-the-shoulder aim. We keep them all under the
    // close-up pull-in per the user's preference.
    if (alink->mSight.getDrawFlg() || alink->checkBowAnime()) {
        return true;
    }
    switch (alink->mProcID) {
    case daAlink_c::PROC_TALK:                 // NPC + Midna-by-Z
    case daAlink_c::PROC_GET_ITEM:             // bug / heart piece / etc pickup
    case daAlink_c::PROC_LOOK_UP_TO_GET_ITEM:  // sky-held item pickup
    case daAlink_c::PROC_OPEN_TREASURE:        // chest opening
    // Horse-call leaf: picking it up (one-shot anim) and sustained blow
    // (held-button loop) both zoom on Link's hands/face.
    case daAlink_c::PROC_GRASS_WHISTLE_GET:
    case daAlink_c::PROC_GRASS_WHISTLE_WAIT:
    // Wolf howling: free howl (target / enemy alert) and Howling Stone
    // scripted demo (note-matching minigame) both frame on Link's head.
    case daAlink_c::PROC_WOLF_HOWL:
    case daAlink_c::PROC_WOLF_HOWL_DEMO:
    // NOTE: PROC_WOLF_SERVICE_WAIT (0xEE) was previously listed here on the
    // belief it was a "wolf analogue to PROC_TALK" for scripted-scene camera
    // lead-ins. That was wrong: its only real entry point (procWolfWait's
    // idle countdown, d_a_alink_wolf.inc) is wolf's ORDINARY stand-still
    // idle state -- the timer is quartered for wolf, so it fires after only
    // ~3s of no input. Including it here collapsed the stereo depth every
    // time you idled as wolf (user-reported "3D snaps ~3s after I stop
    // moving"). The genuine wolf scripted scenes it was meant to catch run
    // through PROC_TALK (already handled) or open a text box (caught by
    // is_text_box_active()), so dropping this case loses no real close-up
    // framing. Do NOT re-add it.
    // FP clawshot/hookshot stays in SUBJECT throughout the aim + chain-
    // extension phases. mSight is on during aim (caught above) but flips
    // off when the chain is firing; PROC_HOOKSHOT_SUBJECT keeps closeup
    // active for that brief firing phase too. Third-person clawshot (Z-
    // target lock-on) starts in PROC_HOOKSHOT_MOVE without ever passing
    // through SUBJECT, so it correctly stays at full stereo depth.
    case daAlink_c::PROC_HOOKSHOT_SUBJECT:
        return true;
    default:
        return false;
    }
}

f32 closeup_scale_target() {
    if (should_reduce_separation_for_closeups()) {
        return getSettings().game.stereoFpSeparationScale.getValue();
    }
    return 1.0f;
}

} // namespace

bool is_close_up_focus_active() {
    return should_reduce_separation_for_closeups();
}

f32 current_separation() {
    if (current_mode() == AURORA_STEREO_OFF) {
        return 0.0f;
    }
    const f32 separation = getSettings().game.stereoSeparation.getValue();
    return (separation > 0.0f) ? separation : 0.0f;
}

f32 current_eye_offset_x() {
    const f32 separation = current_separation();
    if (separation <= 0.0f) {
        return 0.0f;
    }
    const f32 convergence = effective_convergence();
    if (convergence <= 0.0001f) {
        return 0.0f;
    }
    const f32 tanHalfH = current_tan_half_h();
    if (tanHalfH <= 0.0f) {
        return 0.0f;
    }
    // Derived (see the clip-space note at the top of stereo.h): half of the
    // total eye baseline 2 * separation * tan_half_h * convergence. Moves with
    // FoV and with the close-up convergence pull-in -- never cache this.
    const f32 sign = (s_current_eye == AURORA_EYE_LEFT) ? -1.0f : 1.0f;
    return sign * separation * tanHalfH * convergence;
}

f32 current_projection_shear_x() {
    const f32 separation = current_separation();
    if (separation <= 0.0f) {
        return 0.0f;
    }
    // Must match push_eye_offset's projShift term exactly. Convergence and FoV
    // both drop out: the shear IS the knob.
    const f32 sign = (s_current_eye == AURORA_EYE_LEFT) ? -1.0f : 1.0f;
    return -sign * separation;
}

bool active() {
    return current_mode() != AURORA_STEREO_OFF;
}

bool is_first_eye_of_frame() {
    return !active() || s_current_eye == AURORA_EYE_LEFT;
}

void migrate_legacy_config() {
    auto& separation = getSettings().game.stereoSeparation;
    auto& legacy = getSettings().game.stereoEyeSeparation;
    auto& convergence = getSettings().game.stereoConvergence;

    // Fire only when the new key is absent from the saved profile while the old
    // one is present. Both conditions matter: the first keeps an already-
    // converted profile untouched, the second keeps a fresh install on the new
    // default instead of round-tripping it through a conversion.
    if (separation.getLayer() != config::ConfigVarLayer::Default ||
        legacy.getLayer() == config::ConfigVarLayer::Default) {
        return;
    }

    const f32 legacySeparationWu = legacy.getValue();
    const f32 convergenceWu = convergence.getValue();
    if (legacySeparationWu <= 0.0f || convergenceWu <= 0.0001f) {
        return;
    }

    // separation = sep_wu / (2 * conv_wu * tan_half_h_ref). Exact, not
    // approximate: it reproduces the user's existing image pixel-for-pixel at
    // their saved convergence and reference FoV.
    const f32 tanHalfHRef =
        std::tan(kLegacyReferenceFovDeg * kDegToRad * 0.5f) * kLegacyReferenceAspect;
    const f32 converted = legacySeparationWu / (2.0f * convergenceWu * tanHalfHRef);
    const f32 clamped = std::clamp(converted, 0.0f, 0.15f);

    separation.setValue(clamped);
    DuskLog.info(
        "Stereo: converted legacy eye separation {:.1f} world units @ convergence {:.1f} to "
        "clip-space separation {:.4f} ({:.1f}% of screen width at infinity)",
        legacySeparationWu, convergenceWu, clamped, clamped * 100.0f);

    // The HUD-depth slider's sign convention flipped at the same time (positive
    // now means BEHIND the screen plane, so the comfortable setting is a
    // positive number instead of a negative one). Negating here preserves the
    // user's existing HUD placement exactly rather than mirroring it.
    //
    // Safe to piggyback on this gate: it fires only for a profile that predates
    // the clip-space migration, which is definitionally a profile written under
    // the old HUD convention too. A profile saved in the narrow window between
    // the two changes keeps its old sign and has to be re-picked by hand -- not
    // worth a second version key to catch.
    auto& hudDepth = getSettings().game.stereoHudDepth;
    if (hudDepth.getLayer() != config::ConfigVarLayer::Default && hudDepth.getValue() != 0.0f) {
        const f32 flipped = -hudDepth.getValue();
        hudDepth.setValue(flipped);
        DuskLog.info("Stereo: flipped legacy HUD depth {:.0f} to {:.0f} for the reversed sign "
                     "convention (positive is now behind the screen plane)",
            -flipped, flipped);
    }
}

void apply_config_from_settings() {
    // stereoHudDepth slider value -20..20 maps to UV-space horizontal parallax
    // -0.02..0.02 (i.e. up to 2% of screen width). Positive = HUD pops in
    // front of the screen plane.
    const f32 hudDepthUv = getSettings().game.stereoHudDepth.getValue() * 0.001f;

    // Aurora's GX-layer texgen corrections (the Ordon river's PTTEXMTX[0]
    // reflection fix in command_processor.cpp) consume eyeSeparation as a
    // WORLD-UNIT eye baseline, matching the camera translate push_eye_offset
    // performs. Under clip space that baseline is derived rather than stored,
    // so it must be recomputed from the live FoV and the live close-up-scaled
    // convergence every frame -- which is why cAPIGph_Painter calls this once
    // per frame and not just on UI changes.
    const f32 separation = current_separation();
    const f32 convergence = effective_convergence();
    const f32 tanHalfH = current_tan_half_h();
    const f32 eyeBaseline = 2.0f * separation * tanHalfH * convergence;

    // Ghost / crosstalk reduction is a stereo-only correction: forced to its
    // exact no-ops when the mode is Off so the mono present path stays
    // bit-identical (same contract as refractionAmplitudeScale).
    const bool stereoOn = current_mode() != AURORA_STEREO_OFF;
    const f32 ghostContrast =
        stereoOn ? std::clamp(getSettings().game.stereoGhostContrast.getValue(), 0.5f, 1.0f) : 1.0f;
    const f32 ghostBlackFloor =
        stereoOn ? std::clamp(getSettings().game.stereoGhostBlackFloor.getValue(), 0.0f, 0.25f)
                 : 0.0f;

    const AuroraStereoConfig cfg{
        .mode = current_mode(),
        .eyeSeparation = eyeBaseline,
        .convergence = convergence,
        .hudDepth = hudDepthUv,
        .refractionAmplitudeScale = std::clamp(
            getSettings().game.stereoRefractionScale.getValue(), 0.0f, 1.0f),
        .ghostContrast = ghostContrast,
        .ghostBlackFloor = ghostBlackFloor,
    };
    aurora_set_stereo_config(&cfg);
}

void push_eye_offset(AuroraEye eye) {
    s_current_eye = eye;
    const f32 separation = current_separation();
    if (separation <= 0.0f) {
        return;
    }
    const f32 convergence = effective_convergence();
    if (convergence <= 0.0001f) {
        return;
    }
    const f32 sign = (eye == AURORA_EYE_LEFT) ? -1.0f : 1.0f;

    for (int i = 0; i < kMaxCameras; ++i) {
        camera_process_class* camera = dComIfGp_getCamera(i);
        if (camera == nullptr) {
            s_saved[i].valid = false;
            continue;
        }
        auto& view = camera->view;

        // tan(half horizontal FoV) from the live projection. Guard on
        // separation (above) and on this rather than on projMtx[0][0]
        // directly -- a zero separation is the "user turned 3D depth off"
        // case and must short-circuit before anything divides.
        const f32 p00 = view.projMtx[0][0];
        if (p00 <= 0.0001f) {
            s_saved[i].valid = false;
            continue;
        }
        const f32 tanHalfH = 1.0f / p00;

        // Derived view-space eye offset. Unlike the old parameterization this
        // is NOT a stored setting -- it tracks FoV and convergence so that
        // zero parallax stays at `convergence` while `separation` (and hence
        // background disparity) is held fixed.
        const f32 eyeOffsetX = sign * separation * tanHalfH * convergence;

        // Save originals so pop_eye_offset can restore exactly. We snapshot the
        // full invViewMtx and projViewMtx because they're recomputed below and a
        // bit-exact restore matters for any downstream consumer (water/reflection
        // sampling, audio camera, terrain checks, etc).
        s_saved[i].projM02 = view.projMtx[0][2];
        s_saved[i].viewM03 = view.viewMtx[0][3];
        s_saved[i].viewNoTransM03 = view.viewMtxNoTrans[0][3];
        MTXCopy(view.invViewMtx, s_saved[i].invViewMtx);
        MTXCopy(view.projViewMtx, s_saved[i].projViewMtx);
        s_saved[i].lookatEye = view.lookat.eye;
        s_saved[i].lookatCenter = view.lookat.center;
        s_saved[i].valid = true;

        // Camera's world-space "right" axis. The view matrix's first row maps
        // world -> view X, so (viewMtx[0][0..2]) is exactly the camera's right
        // axis expressed in world coords. Computed before we touch viewMtx so
        // it reflects the unshifted camera orientation.
        const f32 rightX = view.viewMtx[0][0];
        const f32 rightY = view.viewMtx[0][1];
        const f32 rightZ = view.viewMtx[0][2];

        // (1) Translate the camera laterally in view space. The view matrix
        //     maps world -> view; subtracting eyeOffsetX from view[0][3] moves
        //     all world points by -eyeOffsetX in view X, which is equivalent
        //     to the camera moving by +eyeOffsetX in its own X axis. This is
        //     what gives depth-correct parallax: closer objects shift more in
        //     screen space than farther ones.
        view.viewMtx[0][3] -= eyeOffsetX;
        // viewMtxNoTrans is rotation-only (used for skybox etc) and stays at
        // zero translation -- skybox tracks rotation but never translates.

        // (2) Shear the projection. Under the clip-space parameterization the
        //     shear IS the separation knob: it carries no FoV term and no
        //     convergence term, and is what fixes total background disparity
        //     at `separation` * screen width regardless of what the camera or
        //     the convergence slider do.
        //
        //     Equivalently (and this is what makes it interchangeable with the
        //     old form): -eyeOffsetX * projMtx[0][0] / convergence, with
        //     eyeOffsetX as derived above, collapses to exactly -sign *
        //     separation because tan_half_h * projMtx[0][0] == 1 and the
        //     convergence factors cancel.
        //
        //     SIGN: LEFT eye gets +separation, RIGHT gets -separation. This is
        //     the PROJECTION SHEAR convention and is independent of the
        //     screen-space compositor convention used by
        //     screen_parallax_x_for_world_pos / hud_ortho_shift_x, which comes
        //     out the other way round. It also matches 3Dmigoto, which stores
        //     the left eye's separation negated in its params texture (a cheap
        //     external cross-check). Verify by putting an object nearer than
        //     convergence on screen: it must show CROSSED disparity.
        const f32 projShift = -sign * separation;
        view.projMtx[0][2] += projShift;

        // (3) Refresh derived matrices. Several effects -- water/reflection
        //     sampling, indirect texture coord generation, terrain checks --
        //     read view.invViewMtx or view.projViewMtx instead of viewMtx,
        //     so they'd otherwise sample the unshifted camera and decouple
        //     from the geometry's depth parallax.
        cMtx_inverse(view.viewMtx, view.invViewMtx);
        cMtx_concatProjView(view.projMtx, view.viewMtx, view.projViewMtx);

        // (4) Shift the world-space camera position and target along the
        //     camera's right axis. Water reflections, env lighting and audio
        //     read view.lookat.eye / .center directly to build mirrored
        //     cameras or position-relative effects -- if they see the
        //     unshifted camera the reflection's eye is decoupled from the
        //     geometry's eye, producing the "split in opposite directions"
        //     artifact on reflective surfaces.
        view.lookat.eye.x += eyeOffsetX * rightX;
        view.lookat.eye.y += eyeOffsetX * rightY;
        view.lookat.eye.z += eyeOffsetX * rightZ;
        view.lookat.center.x += eyeOffsetX * rightX;
        view.lookat.center.y += eyeOffsetX * rightY;
        view.lookat.center.z += eyeOffsetX * rightZ;
    }
}

f32 refraction_skew_correction_x(f32 srt_z_view) {
    const f32 separation = current_separation();
    if (separation <= 0.0f) {
        return 0.0f;
    }
    const f32 tanHalfH = current_tan_half_h();
    if (tanHalfH <= 0.0f) {
        return 0.0f;
    }
    // -eyeOffsetX * srt_z_view / convergence with the derived eyeOffsetX; the
    // convergence factors cancel (see the header note). Deliberately computed
    // from separation directly rather than via current_eye_offset_x() so a
    // degenerate convergence can't zero this while the shear it compensates is
    // still being applied.
    const f32 sign = (s_current_eye == AURORA_EYE_LEFT) ? -1.0f : 1.0f;
    return -sign * separation * tanHalfH * srt_z_view;
}

f32 hud_ortho_shift_x() {
    if (!active()) {
        return 0.0f;
    }
    const f32 hudDepth = getSettings().game.stereoHudDepth.getValue();
    if (hudDepth == 0.0f) {
        return 0.0f;
    }
    // Eye sign: -1 = left, +1 = right (matches current_eye_offset_x convention).
    const f32 eye_sign = (s_current_eye == AURORA_EYE_RIGHT) ? 1.0f : -1.0f;
    const f32 viewport_width = mDoGph_gInf_c::getWidthF();
    // 1 slider unit = 0.1% of viewport width per eye. Increasing the ortho's
    // left bound shifts the rendered content LEFT on screen.
    //
    // The leading negation is the slider's sign convention, and it is the ONLY
    // place that convention lives: every consumer -- the two J2D setOrtho sites
    // and the world-anchored 2D elements that re-add this value to cancel it --
    // goes through this function, so they stay consistent by construction.
    // POSITIVE hudDepth now pushes the HUD BEHIND the screen plane (into the
    // screen), which is the direction people actually want, so the comfortable
    // setting is a positive number rather than a negative one.
    const f32 shift_pixels = hudDepth * 0.001f * viewport_width;
    return -eye_sign * shift_pixels;
}

f32 screen_parallax_x_for_world_pos(const cXyz& world_pos) {
    const f32 separation = current_separation();
    if (separation <= 0.0f) {
        return 0.0f;
    }
    const f32 convergence = effective_convergence();
    if (convergence <= 0.0001f) {
        return 0.0f;
    }
    const camera_process_class* camera = dComIfGp_getCamera(0);
    if (camera == nullptr) {
        return 0.0f;
    }
    const auto& view = camera->view;

    // View-space Z via row 2 of the view matrix. push_eye_offset only mutates
    // row 0 (X translation), so this is identical to the unshifted depth.
    const f32 z_view = view.viewMtx[2][0] * world_pos.x +
                       view.viewMtx[2][1] * world_pos.y +
                       view.viewMtx[2][2] * world_pos.z +
                       view.viewMtx[2][3];
    if (z_view > -0.001f) {
        return 0.0f; // behind / at camera plane
    }

    // Clip-space disparity. z_view is negative in front of the camera, so
    // (convergence / z_view + 1) is the negated (convergence/depth - 1) term
    // and the leading sign is already the compositor convention (opposite of
    // the projection shear's -- see the header note).
    const f32 sign = (s_current_eye == AURORA_EYE_LEFT) ? -1.0f : 1.0f;
    const f32 viewport_width = mDoGph_gInf_c::getWidthF();
    const f32 delta_ndc = sign * separation * (convergence / z_view + 1.0f);
    return delta_ndc * viewport_width * 0.5f;
}

void auto_convergence_tick() {
    if (!getSettings().game.stereoAutoConvergence.getValue() || !active()) {
        // Manual-as-ceiling / snap-back: disabling snaps straight back to the
        // manual value rather than easing out. An ease here reads as
        // unexplained drift right after the user turned the feature off.
        if (s_autoconv_engaged || s_autoconv_generation != 0) {
            auto_convergence_reset();
        }
        return;
    }

    // Guard on separation, not on projMtx[0][0]: under clip space the latter no
    // longer appears in the control law, and a zero separation would divide by
    // zero in the solve below.
    const f32 separation = current_separation();
    const f32 manualConvergence = closeup_convergence();
    if (separation <= 0.0f || manualConvergence <= 0.0001f) {
        auto_convergence_reset();
        return;
    }

    const camera_process_class* camera = dComIfGp_getCamera(0);
    if (camera == nullptr || !(camera->view.near_ > 0.0f) ||
        !(camera->view.far_ > camera->view.near_)) {
        auto_convergence_reset();
        return;
    }
    const f32 nearZ = camera->view.near_;
    const f32 farZ = camera->view.far_;

    // Keep asking for captures; the request is a cheap idempotent flag and the
    // renderer's own 30Hz limiter decides when one is actually taken.
    aurora::gfx::request_depth_snapshot();

    // --- measurement, only when a new snapshot actually landed ---------------
    const uint64_t pending = aurora::gfx::depth_snapshot_generation();
    if (pending != 0 && pending != s_autoconv_generation) {
        s_autoconv_grid.resize(static_cast<size_t>(kAutoConvCols) * kAutoConvRows);
        const uint64_t sampled = aurora::gfx::sample_depth_snapshot_min(kAutoConvRoiX0,
            kAutoConvRoiY0, kAutoConvRoiX1, kAutoConvRoiY1, kAutoConvCols, kAutoConvRows,
            s_autoconv_grid.data());
        if (sampled != 0) {
            s_autoconv_generation = sampled;
            s_autoconv_depth_available = true;

            // Percentile on the raw Z24 values rather than on converted depths:
            // the conversion is monotonic, so the ordering is identical and this
            // is an integer partial sort over 1536 elements instead of 1536
            // divisions followed by a float one.
            std::nth_element(s_autoconv_grid.begin(),
                s_autoconv_grid.begin() + kAutoConvPercentileIndex, s_autoconv_grid.end());
            const f32 zRaw =
                z24_to_view_depth(s_autoconv_grid[kAutoConvPercentileIndex], nearZ, farZ);

            if (std::isfinite(zRaw) && zRaw > 0.0f) {
                s_autoconv_history[s_autoconv_history_pos] = zRaw;
                s_autoconv_history_pos = (s_autoconv_history_pos + 1) % kAutoConvMedianWindow;
                if (s_autoconv_history_count < kAutoConvMedianWindow) {
                    ++s_autoconv_history_count;
                }

                std::array<f32, kAutoConvMedianWindow> sorted{};
                std::copy_n(s_autoconv_history.begin(), s_autoconv_history_count, sorted.begin());
                std::sort(sorted.begin(), sorted.begin() + s_autoconv_history_count);
                const f32 zMedian = sorted[s_autoconv_history_count / 2];

                if (s_autoconv_z_ema <= 0.0f) {
                    s_autoconv_z_ema = zMedian;
                } else {
                    const f32 rel = std::abs(zMedian - s_autoconv_z_ema) / s_autoconv_z_ema;
                    if (rel > kAutoConvDeadband) {
                        const f32 alpha =
                            (zMedian < s_autoconv_z_ema) ? kAutoConvEmaNear : kAutoConvEmaFar;
                        s_autoconv_z_ema += (zMedian - s_autoconv_z_ema) * alpha;
                    }
                }
            }
        }
    }

    if (s_autoconv_z_ema <= 0.0f) {
        // Enabled, but nothing has come back yet (first frames, or depth peek
        // unavailable on this device). Stay transparent.
        s_autoconv_engaged = false;
        return;
    }

    // --- the control law -----------------------------------------------------
    // Pop-out (crossed) disparity of the nearest object at a given convergence,
    // as a fraction of screen width, POSITIVE when nearer than the screen plane:
    //     d(conv) = separation * (conv/z - 1)
    // Setting d = target and solving gives the convergence at which the nearest
    // object sits exactly on the budget. The user's "comfort limit" IS that
    // total crossed disparity, in the same screen-width-fraction units as the
    // Separation slider.
    const f32 targetDisparity =
        std::clamp(getSettings().game.stereoAutoConvTarget.getValue(), 0.001f, 0.2f);
    f32 convTarget = s_autoconv_z_ema * (1.0f + targetDisparity / separation);

    // Floored, but deliberately NOT clamped to the ceiling here -- the ceiling
    // is applied once, after the smoothing, and the distinction matters.
    //
    // Clamping before the smoother would feed it the close-up pull-in's own
    // 1.5s exit ramp, so on every close-up release the published convergence
    // would be that ramp seen through a second lag: two smoothers on different
    // timescales driving one visible quantity, which reads as jank however
    // smooth each is on its own. Applying the ceiling after instead means that
    // whenever the scene is not demanding a pull-in, the published value tracks
    // closeup_scale_tick EXACTLY -- bit-for-bit the behaviour with this feature
    // switched off -- and the smoother only governs the depth-driven part.
    //
    // (Note this branchlessly subsumes the "is a pull-in needed at all?" test:
    // convTarget < manualConvergence is algebraically identical to the nearest
    // object's disparity at the manual convergence exceeding the budget.)
    const f32 floorConvergence =
        std::max(nearZ * kAutoConvNearPlaneMultiple, kAutoConvMinConvergence);
    convTarget = std::max(convTarget, floorConvergence);

    // --- smooth in 1/convergence space ---------------------------------------
    // Unlike closeup_scale_tick (see the note on kCloseupExitTimeConstSec, where
    // the endpoint is fixed and convergence-space easing is the right answer),
    // THIS target tracks z_near and races toward zero as an object approaches.
    // A plain lerp on convergence accelerates perceptually as convergence gets
    // small -- the classic "auto-convergence lunges the last bit of the way".
    // Smoothing the reciprocal removes that.
    //
    // This is in series with the EMA on the depth measurement itself, which is
    // fine and is what a control loop looks like: one filter on the input, one
    // on the output, composing into a single response. That is a different
    // thing from two independent smoothers racing on the same output.
    const f32 smoothing =
        std::clamp(getSettings().game.stereoAutoConvSmoothing.getValue(), 0.005f, 0.5f);
    const f32 targetInv = 1.0f / convTarget;
    if (s_autoconv_inv_convergence <= 0.0f) {
        s_autoconv_inv_convergence = targetInv;
    } else {
        s_autoconv_inv_convergence += (targetInv - s_autoconv_inv_convergence) * smoothing;
    }
    if (!(s_autoconv_inv_convergence > 0.0f)) {
        auto_convergence_reset();
        return;
    }

    // The manual (close-up-scaled) convergence is the ceiling: auto only ever
    // pulls CLOSER.
    s_autoconv_convergence = std::min(1.0f / s_autoconv_inv_convergence, manualConvergence);
    s_autoconv_engaged = true;
}

void closeup_scale_tick() {
    const f32 target = closeup_scale_target();
    if (target <= s_smoothed_closeup_scale) {
        // Entering / tightening close-up: snap so comfort applies the same
        // frame the trigger fires.
        s_smoothed_closeup_scale = target;
    } else {
        // Releasing close-up: exponential ease back out to the manual
        // convergence ceiling.
        const f32 alpha = std::clamp(kAssumedFrameDt / kCloseupExitTimeConstSec, 0.0f, 1.0f);
        s_smoothed_closeup_scale += alpha * (target - s_smoothed_closeup_scale);
    }
}

void pop_eye_offset() {
    for (int i = 0; i < kMaxCameras; ++i) {
        if (!s_saved[i].valid) {
            continue;
        }
        camera_process_class* camera = dComIfGp_getCamera(i);
        if (camera != nullptr) {
            auto& view = camera->view;
            view.projMtx[0][2] = s_saved[i].projM02;
            view.viewMtx[0][3] = s_saved[i].viewM03;
            view.viewMtxNoTrans[0][3] = s_saved[i].viewNoTransM03;
            MTXCopy(s_saved[i].invViewMtx, view.invViewMtx);
            MTXCopy(s_saved[i].projViewMtx, view.projViewMtx);
            view.lookat.eye = s_saved[i].lookatEye;
            view.lookat.center = s_saved[i].lookatCenter;
        }
        s_saved[i].valid = false;
    }
}

DebugState debug_state() {
    const f32 separation = current_separation();
    const f32 convergence = effective_convergence();
    const f32 tanHalfH = current_tan_half_h();
    return DebugState{
        .separation = separation,
        .closeupScale = s_smoothed_closeup_scale,
        .convergence = convergence,
        .eyeBaseline = 2.0f * separation * tanHalfH * convergence,
        .tanHalfH = tanHalfH,
        .manualConvergence = closeup_convergence(),
        .autoConvNearDepth = s_autoconv_z_ema,
        .autoConvEngaged = s_autoconv_engaged,
        .autoConvDepthAvailable = s_autoconv_depth_available,
    };
}

} // namespace dusk::stereo
