#include "dusk/stereo.h"

#include "dusk/settings.h"

#include "d/d_com_inf_game.h"
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_player.h"
#include "f_op/f_op_camera_mng.h"
#include "f_op/f_op_actor_mng.h"
#include "m_Do/m_Do_graphic.h"
#include "m_Do/m_Do_mtx.h"
#include "mtx.h"

#include <dolphin/gx/GXCpu2Efb.h>

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

// Current smoothed auto-convergence value (world units). Initialized to the
// user's slider default; auto_convergence_tick() updates it each sim frame
// when enabled. Never written back to user settings.
f32 s_auto_convergence = 300.0f;

AuroraStereoMode current_mode() {
    return static_cast<AuroraStereoMode>(static_cast<int>(getSettings().game.stereoMode.getValue()));
}

// Returns whichever convergence the rest of the pipeline should use this
// frame: the auto-converged value when the toggle is on, otherwise the user
// slider value. Centralized so push_eye_offset and refraction_skew_correction_x
// agree.
f32 effective_convergence() {
    if (getSettings().game.enableAutoConvergence.getValue()) {
        return s_auto_convergence;
    }
    return getSettings().game.stereoConvergence.getValue();
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
    //  - getMesgStatus: standard NPC text box (already-rendering).
    //  - isPauseFlag: pause menu / inventory / map / collection.
    //  - mSight.getDrawFlg + checkBowAnime: any aim mode (FP bow/slingshot/
    //    clawshot/dominion rod, plus third-person boomerang/whistle target-
    //    lock). User wants ALL of these to use the close-up scale.
    //  - Talk / item-get / treasure procs catch the *full* close-up sequences
    //    -- the item-get animation runs for ~2s before the text box appears,
    //    and Midna-by-Z runs through PROC_TALK before getMesgStatus fires.
    //    Using the proc enum gets the whole arc, not just the text-box.
    if (dComIfGp_getMesgStatus() != 0 || dComIfGp_isPauseFlag()) {
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

f32 effective_separation_scale() {
    if (should_reduce_separation_for_closeups()) {
        return getSettings().game.stereoFpSeparationScale.getValue();
    }
    return 1.0f;
}

} // namespace

bool is_close_up_focus_active() {
    return should_reduce_separation_for_closeups();
}

namespace {

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

} // namespace

bool active() {
    return current_mode() != AURORA_STEREO_OFF;
}

void apply_config_from_settings() {
    // stereoHudDepth slider value -20..20 maps to UV-space horizontal parallax
    // -0.02..0.02 (i.e. up to 2% of screen width). Positive = HUD pops in
    // front of the screen plane.
    const f32 hudDepthUv = getSettings().game.stereoHudDepth.getValue() * 0.001f;
    const AuroraStereoConfig cfg{
        .mode = current_mode(),
        .eyeSeparation = getSettings().game.stereoEyeSeparation.getValue(),
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

namespace {

f32 distance_to(const cXyz& a, const cXyz& b) {
    const f32 dx = a.x - b.x;
    const f32 dy = a.y - b.y;
    const f32 dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Convert a D24 depth-buffer value (0..0xFFFFFF) into camera-forward distance,
// inverting the GameCube perspective projection:
//   z_depth = -m22 - m23/(-depth)   with m22, m23 from the projection matrix.
//   depth   = m23 / (z_depth + m22)
// Returns 0 on degenerate inputs (caller treats that as "no signal").
f32 depth_buffer_z_to_view_distance(u32 z_uint, const camera_process_class& camera) {
    if (z_uint == 0 || z_uint >= 0x00FFFFFE) {
        return 0.0f; // near plane or sky/clear -- skip
    }
    const f32 z_depth = static_cast<f32>(z_uint) / 16777215.0f;
    const f32 m22 = camera.view.projMtx[2][2];
    const f32 m23 = camera.view.projMtx[2][3];
    const f32 denom = z_depth + m22;
    if (std::abs(denom) <= 1e-6f) {
        return 0.0f;
    }
    const f32 depth = m23 / denom;
    return depth > 0.0f ? depth : 0.0f;
}

void publish_effective_convergence() {
    apply_config_from_settings(); // pulls effective_convergence() under the hood
}

} // namespace

void auto_convergence_tick() {
    if (!getSettings().game.enableAutoConvergence.getValue()) {
        return;
    }
    if (!active()) {
        return;
    }
    camera_process_class* camera = dComIfGp_getCamera(0);
    if (camera == nullptr) {
        return;
    }

    // Dialog open: freeze the comfort plane so it doesn't jump when the text
    // box appears. We do NOT republish here -- whatever apply_config did last
    // frame stays in effect.
    if (dComIfGp_getMesgStatus() != 0) {
        return;
    }

    const cXyz& cam_eye = camera->view.lookat.eye;
    const cXyz& cam_center = camera->view.lookat.center;
    f32 desired = 0.0f;
    bool have_target = false;

    // 1. Z-target lock-on: distance to the targeted actor.
    daPy_py_c* player = dComIfGp_getLinkPlayer();
    daAlink_c* alink = static_cast<daAlink_c*>(player); // Link is always a daAlink_c
    if (alink != nullptr) {
        if (fopAc_ac_c* target = alink->getAtnActor()) {
            desired = distance_to(fopAcM_GetPosition(target), cam_eye);
            have_target = true;
        }
        // 2. Aim mode: the sight reticle's world hit point. Multiply by 0.5
        // so the target sits HALFWAY between the camera and the screen plane,
        // i.e. the target POPS OUT toward the viewer instead of locking at
        // screen depth. Aim feels much more positive with the target floating
        // in front of you.
        if (!have_target && alink->mSight.getDrawFlg()) {
            if (const cXyz* sight_pos = alink->mSight.getPosP()) {
                desired = distance_to(*sight_pos, cam_eye) * 0.5f;
                have_target = true;
            }
        }
    }
    // 3. Cutscene / event: camera lookat distance is what the director points
    //    the audience at.
    if (!have_target && dComIfGp_event_runCheck()) {
        desired = distance_to(cam_center, cam_eye);
        have_target = true;
    }
    // 4. Fallback: depth at screen center via GXPeekZ (existing async readback
    //    at ~30Hz, smoothing hides the lag).
    if (!have_target) {
        const u16 cx = static_cast<u16>(mDoGph_gInf_c::getWidthF() * 0.5f);
        const u16 cy = static_cast<u16>(mDoGph_gInf_c::getHeightF() * 0.5f);
        u32 z_uint = 0;
        GXPeekZ(cx, cy, &z_uint);
        const f32 depth = depth_buffer_z_to_view_distance(z_uint, *camera);
        if (depth > 0.0f) {
            desired = depth;
            have_target = true;
        }
    }
    if (!have_target) {
        return;
    }

    // Clamp to a reasonable comfort range so a stray bad sample can't pin the
    // comfort plane at 0 or infinity.
    desired = std::clamp(desired, 50.0f, 10000.0f);

    // Exponential smoothing. dt is approximated at 1/60 -- the smoothing
    // window is in the 0.1s..0.5s range so frame-rate sensitivity is minor.
    const f32 smoothing = getSettings().game.autoConvergenceSmoothing.getValue();
    constexpr f32 kAssumedDt = 1.0f / 60.0f;
    const f32 alpha = (smoothing <= 0.0f) ? 1.0f : std::clamp(kAssumedDt / smoothing, 0.0f, 1.0f);
    s_auto_convergence = s_auto_convergence + alpha * (desired - s_auto_convergence);

    publish_effective_convergence();
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

} // namespace dusk::stereo
