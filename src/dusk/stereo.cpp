#include "dusk/stereo.h"

#include "dusk/settings.h"

#include "d/d_com_inf_game.h"
#include "d/d_msg_object.h"
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_player.h"
#include "f_op/f_op_camera_mng.h"
#include "m_Do/m_Do_graphic.h"
#include "m_Do/m_Do_mtx.h"
#include "mtx.h"

#include <algorithm>
#include <cmath>

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

// Smoothed close-up separation scale (0..1). closeup_scale_tick() snaps this
// down instantly when the close-up predicate becomes true (so comfort kicks
// in immediately when you draw a bow or open a textbox) and eases it back
// up toward 1.0 with an exponential roll-off when the predicate releases
// (so the world doesn't pop "wider" the instant you stow the weapon or
// close the dialog). Read by effective_separation_scale().
f32 s_smoothed_closeup_scale = 1.0f;

// Exit roll-off time constant. ~1.5s gives a slow, unhurried expansion so
// the world doesn't pop wider the instant a textbox closes or a weapon is
// stowed. Combined with the per-frame dt (~1/60s) via kAssumedFrameDt below.
constexpr f32 kCloseupExitTimeConstSec = 1.5f;
constexpr f32 kAssumedFrameDt = 1.0f / 60.0f;

// TP's de-facto default gameplay Fovy in degrees. Not a named engine
// constant -- FoV is set per camera-style/per-scene throughout d_camera.cpp
// -- but 60 is by far the most common fallback (initial camera state, most
// dialogue/idle camera resets, most event-camera defaults), making it the
// most defensible calibration anchor for "the FoV the separation/convergence
// sliders were tuned while looking at."
constexpr f32 kReferenceFovDeg = 60.0f;

// Max fov_scale change per second, applied as a rate limiter rather than an
// exponential EMA. TP's own camera-style hand-off (chaseCamera re-entering
// after FP aim / dialog releases, see d_camera.cpp) already blends Fovy back
// with a genuine LINEAR ramp over a dynamically-computed duration -- an EMA
// on top of that ramping input trails it by a constant lag the whole time,
// and once the ramp stops the EMA still has to close that residual lag,
// producing a small extra "catch-up" pop that lands AFTER the camera has
// already finished moving (independently of, but confusingly close in time
// to, any unrelated close-up separation ease still running). A rate limiter
// tracks a ramp with ZERO steady-state lag as long as the ramp's own speed
// stays under this cap, while still spreading a genuine instantaneous FoV
// cut (e.g. a dialogue layout hard-setting Fovy) over a handful of frames
// instead of a single-frame pop. ~7/s closes a full-range hop (widest zoom
// to widest cutscene FoV, roughly 2.5x in scale) in about a third of a
// second.
constexpr f32 kFovScaleMaxStepPerSec = 7.0f;

constexpr f32 kDegToRad = 0.017453292519943295f;

// Rate-limited FoV-aware separation scale (see kFovScaleMaxStepPerSec for
// why this is a rate limiter and not an EMA). 1.0 at the reference FoV;
// shrinks as the camera zooms in (narrower Fovy) to keep rendered disparity
// constant. Never written back to user settings. See fov_scale_tick().
f32 s_fov_scale = 1.0f;

AuroraStereoMode current_mode() {
    return static_cast<AuroraStereoMode>(static_cast<int>(getSettings().game.stereoMode.getValue()));
}

bool should_reduce_separation_for_closeups();
f32 effective_separation_scale();

// The convergence the rest of the pipeline should use this frame: the user's
// manual slider value, scaled only by the close-up scale (NOT the FoV scale --
// convergence is a distance, not a disparity, so it doesn't need FoV
// compensation). The close-up scale IS applied because during FP aim / dialog
// / item-get the near subject is close enough that pulling the comfort plane
// in with it (not just shrinking separation) reads better. Centralized so
// push_eye_offset and refraction_skew_correction_x agree.
f32 effective_convergence() {
    return getSettings().game.stereoConvergence.getValue() * s_smoothed_closeup_scale;
}

f32 tan_half_fov(f32 fovyDeg) {
    return std::tan(fovyDeg * kDegToRad * 0.5f);
}

// dynamic3d-style FoV compensation: disparity scales with sep * P00, and
// P00 = cot(fovy/2)/aspect, so a narrower FoV inflates disparity unless
// separation is scaled down by tan(halfGame)/tan(halfRef) to cancel it.
f32 compute_fov_scale(f32 fovyDeg) {
    if (fovyDeg <= 0.0f || fovyDeg >= 180.0f) {
        return 1.0f;
    }
    const f32 tanHalfGame = tan_half_fov(fovyDeg);
    const f32 tanHalfRef = tan_half_fov(kReferenceFovDeg);
    if (tanHalfGame <= 0.0f || tanHalfRef <= 0.0f) {
        return 1.0f;
    }
    return tanHalfGame / tanHalfRef;
}

// True when something close to the camera dominates the frame and full
// eye separation would be uncomfortable: first-person aim modes (bow,
// slingshot, clawshot, dominion rod, hookshot) and any open dialog/message
// box. We use the same scale for both since both want the same "shrink the
// stereo for close subject" treatment.
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
    //    lock). User wants ALL of these to use the close-up scale.
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
    // close-up scale per the user's preference.
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
    // through SUBJECT, so it correctly stays at full eye sep.
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

f32 effective_separation_scale() {
    return s_smoothed_closeup_scale * s_fov_scale;
}

} // namespace

bool is_close_up_focus_active() {
    return should_reduce_separation_for_closeups();
}

f32 current_eye_offset_x() {
    if (current_mode() == AURORA_STEREO_OFF) {
        return 0.0f;
    }
    const f32 separation = getSettings().game.stereoEyeSeparation.getValue() * effective_separation_scale();
    if (separation <= 0.0001f) {
        return 0.0f;
    }
    const f32 sign = (s_current_eye == AURORA_EYE_LEFT) ? -1.0f : 1.0f;
    return sign * (separation * 0.5f);
}

f32 current_projection_shear_x() {
    const f32 eyeOffsetX = current_eye_offset_x();
    if (eyeOffsetX == 0.0f) {
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
    // Must match push_eye_offset's projShift term exactly: projMtx[0][0] is
    // untouched by the eye push, so reading it live between push/pop yields
    // the same value push_eye_offset used.
    return -eyeOffsetX * camera->view.projMtx[0][0] / convergence;
}

bool active() {
    return current_mode() != AURORA_STEREO_OFF;
}

bool is_first_eye_of_frame() {
    return !active() || s_current_eye == AURORA_EYE_LEFT;
}

void apply_config_from_settings() {
    // stereoHudDepth slider value -20..20 maps to UV-space horizontal parallax
    // -0.02..0.02 (i.e. up to 2% of screen width). Positive = HUD pops in
    // front of the screen plane.
    const f32 hudDepthUv = getSettings().game.stereoHudDepth.getValue() * 0.001f;
    // eyeSeparation must carry the SAME effective_separation_scale() factor
    // current_eye_offset_x()/push_eye_offset() use for the actual per-eye
    // view/proj shift. Aurora's GX-layer texgen corrections (water
    // reflection PTTEXMTX, screen-space refraction) read this config
    // directly rather than the game's live camera state, so passing the raw
    // slider value here would decouple them from the real eye offset the
    // instant close-up scale, FoV auto-scale, or the auto-convergence
    // depth-lock kicks in -- the exact "surface floats off the magnet"
    // class of artifact documented on current_projection_shear_x().
    const AuroraStereoConfig cfg{
        .mode = current_mode(),
        .eyeSeparation = getSettings().game.stereoEyeSeparation.getValue() * effective_separation_scale(),
        .convergence = effective_convergence(),
        .hudDepth = hudDepthUv,
        .refractionAmplitudeScale = std::clamp(
            getSettings().game.stereoRefractionScale.getValue(), 0.0f, 1.0f),
    };
    aurora_set_stereo_config(&cfg);
}

void push_eye_offset(AuroraEye eye) {
    s_current_eye = eye;
    const f32 separation = getSettings().game.stereoEyeSeparation.getValue() * effective_separation_scale();
    const f32 convergence = effective_convergence();
    if (convergence <= 0.0001f) {
        return;
    }
    const f32 sign = (eye == AURORA_EYE_LEFT) ? -1.0f : 1.0f;
    const f32 eyeOffsetX = sign * (separation * 0.5f); // camera position in view-space X

    for (int i = 0; i < kMaxCameras; ++i) {
        camera_process_class* camera = dComIfGp_getCamera(i);
        if (camera == nullptr) {
            s_saved[i].valid = false;
            continue;
        }
        auto& view = camera->view;

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

        // (2) Skew the projection so the convergence plane (objects at the
        //     'convergence' distance in front of the camera) has zero parallax.
        //     Without this, the zero-parallax plane sits at infinity and the
        //     whole scene is in front of the screen.
        //
        //     Derivation: with e_x = sign*sep/2 the eye view-position and z_c
        //     = convergence, we want the convergence point to land at NDC
        //     origin. ndc_x = (-e_x * m[0][0]) / z_c - m[0][2], so setting
        //     ndc_x = 0 gives m[0][2] = -e_x * m[0][0] / z_c. Use the (now
        //     possibly camera_draw-rebuilt) m[0][0] in view.projMtx so the
        //     shift scales correctly with FOV.
        const f32 projShift = -eyeOffsetX * view.projMtx[0][0] / convergence;
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
    const f32 eyeOffsetX = current_eye_offset_x();
    if (eyeOffsetX == 0.0f) {
        return 0.0f;
    }
    const f32 convergence = effective_convergence();
    if (convergence <= 0.0001f) {
        return 0.0f;
    }
    return -eyeOffsetX * srt_z_view / convergence;
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
    // left bound shifts the rendered content LEFT on screen; we want LEFT eye
    // to see HUD shifted RIGHT and RIGHT eye to see HUD shifted LEFT (for
    // positive hudDepth -> "pops forward"), so we add +eye_sign * shift.
    const f32 shift_pixels = hudDepth * 0.001f * viewport_width;
    return eye_sign * shift_pixels;
}

f32 screen_parallax_x_for_world_pos(const cXyz& world_pos) {
    if (!active()) {
        return 0.0f;
    }
    const f32 eyeOffsetX = current_eye_offset_x();
    if (eyeOffsetX == 0.0f) {
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

    const f32 p00 = view.projMtx[0][0];
    const f32 viewport_width = mDoGph_gInf_c::getWidthF();
    const f32 delta_ndc = eyeOffsetX * p00 * (1.0f / z_view + 1.0f / convergence);
    return delta_ndc * viewport_width * 0.5f;
}

void closeup_scale_tick() {
    const f32 target = closeup_scale_target();
    if (target <= s_smoothed_closeup_scale) {
        // Entering / tightening close-up: snap so comfort applies the same
        // frame the trigger fires.
        s_smoothed_closeup_scale = target;
    } else {
        // Releasing close-up: exponential ease back to full separation.
        const f32 alpha = std::clamp(kAssumedFrameDt / kCloseupExitTimeConstSec, 0.0f, 1.0f);
        s_smoothed_closeup_scale += alpha * (target - s_smoothed_closeup_scale);
    }
}

void fov_scale_tick() {
    const camera_process_class* camera = dComIfGp_getCamera(0);
    if (camera == nullptr) {
        return;
    }
    const f32 target = compute_fov_scale(camera->view.fovy);
    const f32 maxStep = kFovScaleMaxStepPerSec * kAssumedFrameDt;
    s_fov_scale += std::clamp(target - s_fov_scale, -maxStep, maxStep);
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
    return DebugState{
        .fovScale = s_fov_scale,
        .closeupScale = s_smoothed_closeup_scale,
        .separationScale = effective_separation_scale(),
        .convergence = effective_convergence(),
    };
}

} // namespace dusk::stereo
