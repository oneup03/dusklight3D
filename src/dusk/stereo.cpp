#include "dusk/stereo.h"

#include "dusk/settings.h"

#include "d/d_com_inf_game.h"
#include "f_op/f_op_camera_mng.h"
#include "m_Do/m_Do_mtx.h"
#include "mtx.h"

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

AuroraStereoMode current_mode() {
    return static_cast<AuroraStereoMode>(static_cast<int>(getSettings().game.stereoMode.getValue()));
}

} // namespace

bool active() {
    return current_mode() != AURORA_STEREO_OFF;
}

void apply_config_from_settings() {
    const AuroraStereoConfig cfg{
        .mode = current_mode(),
        .eyeSeparation = getSettings().game.stereoEyeSeparation.getValue(),
        .convergence = getSettings().game.stereoConvergence.getValue(),
        .hudDepth = getSettings().game.stereoHudDepth.getValue(),
    };
    aurora_set_stereo_config(&cfg);
}

void push_eye_offset(AuroraEye eye) {
    const f32 separation = getSettings().game.stereoEyeSeparation.getValue();
    const f32 convergence = getSettings().game.stereoConvergence.getValue();
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
