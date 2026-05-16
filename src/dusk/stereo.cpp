#include "dusk/stereo.h"

#include "dusk/settings.h"

#include "d/d_com_inf_game.h"
#include "f_op/f_op_camera_mng.h"
#include "m_Do/m_Do_mtx.h"
#include "mtx.h"

#include "JSystem/J3DGraphAnimator/J3DModel.h"
#include "JSystem/J3DGraphAnimator/J3DModelData.h"
#include "JSystem/J3DGraphBase/J3DStruct.h"
#include "JSystem/J3DGraphBase/J3DTransform.h"

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

// Tracks which eye the painter most recently pushed. Used by the in-place
// fallback path of apply_eye_to_reflection_effect_mtx; the registry path
// reads the eye that the painter passes to apply_reflection_corrections.
AuroraEye s_current_eye = AURORA_EYE_LEFT;

// Reflection-texgen registry. The water actor's Draw method runs once per
// frame (before the painter's eye loop) and builds the material's display
// list with one cached matrix value -- both eyes would otherwise render with
// it. To get both eyes, the actor Draw registers (model, info, base) here,
// and the painter funnel rewrites mEffectMtx + rebuilds the model's DL per
// eye via calcMaterial()/diff() between iterations. The model pointer is
// required because the matrix lives baked inside the DL; updating just
// mEffectMtx without rebuilding the DL has no visible effect.
enum class ReflectionStyle {
    Standard, // q row = (0, 0, -1, 0); perspective divide by depth
    Halo,     // q row = (0, -1, 0, player.y); divide by height-from-player.
              // Used for MA20-style player-centered halo on water surfaces --
              // the per-eye view-shift contribution has opposite sign vs the
              // standard reflection so the correction must flip sign too.
};

struct ReflectionEntry {
    J3DTexMtxInfo* info;
    J3DModel* model;
    Mtx base;
    ReflectionStyle style;
};
std::vector<ReflectionEntry> s_reflections;

void apply_reflection_correction_to(Mtx mtx, AuroraEye eye) {
    const f32 separation = getSettings().game.stereoEyeSeparation.getValue();
    const f32 convergence = getSettings().game.stereoConvergence.getValue();
    if (convergence <= 0.0001f) {
        return;
    }

    // Per-eye correction magnitudes in view-space X (game units).
    //
    // Determined empirically: a symmetric ±sep/2 shift on the depth-dependent
    // term leaves a triangular per-eye spread (near water splits more than
    // far water) because of asymmetric interaction with the texgen / per-eye
    // EFB sampling pipeline. Asymmetric magnitudes -- left eye gets half,
    // right eye gets full sep -- bring both eyes' reflections into alignment
    // across camera angles. The constant z-coefficient term stays symmetric
    // since it doesn't interact with depth.
    //
    // This water-reflection correction uses its own asymmetric formula
    // determined empirically. The JPA refraction texgen fix lives in
    // refraction_skew_correction_x() and is depth-aware (geometrically
    // derived) rather than empirically tuned.
    const f32 halfSep = separation * 0.5f;
    const f32 depthShift = (eye == AURORA_EYE_LEFT) ? -halfSep : 2.0f * halfSep;
    const f32 constShift = (eye == AURORA_EYE_LEFT) ? -halfSep : halfSep;
    const f32 m00 = mtx[0][0];

    // mtx[0][3]: depth-dependent UV shift (constant in s, becomes ~1/depth
    // in u after the perspective divide by -view_z).
    mtx[0][3] += -depthShift * m00;
    // mtx[0][2]: constant per-eye UV translation regardless of depth.
    // Both eyes' UVs end up offset by m00 * halfSep / convergence.
    mtx[0][2] += -constShift * m00 / convergence;
}

void apply_halo_correction_to(Mtx mtx, AuroraEye eye) {
    const f32 separation = getSettings().game.stereoEyeSeparation.getValue();
    if (separation <= 0.0001f) {
        return;
    }

    // Halo-style texgen (MA20). The matrix is C_MTXLightPerspective * lookAt
    // with the lookAt aimed straight down at the player, so the q row of the
    // composite ends up (0, -1, 0, player.y) -- perspective divide is by
    // (player.y - view_y) rather than -view_z. The view-shift's contribution
    // to the texgen u therefore has the OPPOSITE sign of the standard
    // reflection case; without flipping the correction sign, the halo looks
    // mirrored between eyes (it shifts the wrong way as separation grows).
    //
    // Symmetric per-eye magnitude here -- no asymmetric scaling like the
    // standard reflection needed, because this matrix has no SRT/qMtx pipeline
    // shenanigans (the q row comes from the lookAt, not from the standard
    // J3D texgen perspective row).
    //
    // We also skip the mtx[0][2] term: the z-coefficient in this composite
    // matrix doesn't have a clean "constant UV translation" interpretation
    // because q doesn't involve view_z.
    const f32 halfSep = separation * 0.5f;
    const f32 sign = (eye == AURORA_EYE_LEFT) ? -1.0f : 1.0f;
    const f32 eyeOffsetX = sign * halfSep;
    const f32 m00 = mtx[0][0];

    mtx[0][3] += eyeOffsetX * m00;
}

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
    s_current_eye = eye;
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

namespace {
void register_or_update(Mtx mtx, J3DTexMtxInfo* info, J3DModel* model, ReflectionStyle style) {
    bool found = false;
    for (auto& entry : s_reflections) {
        if (entry.info == info) {
            entry.model = model;
            entry.style = style;
            MTXCopy(mtx, entry.base);
            found = true;
            break;
        }
    }
    if (!found) {
        ReflectionEntry entry;
        entry.info = info;
        entry.model = model;
        entry.style = style;
        MTXCopy(mtx, entry.base);
        s_reflections.push_back(entry);
    }
}
} // namespace

void apply_eye_to_reflection_effect_mtx(Mtx mtx, J3DTexMtxInfo* info, J3DModel* model) {
    if (!active() || info == nullptr) {
        if (info != nullptr) {
            info->setEffectMtx(mtx);
        }
        return;
    }
    register_or_update(mtx, info, model, ReflectionStyle::Standard);
    // Seed the actor Draw's DL with the BASE matrix so it's neutral; the
    // painter funnel rebuilds per eye between iterations.
    info->setEffectMtx(mtx);
}

void apply_eye_to_halo_effect_mtx(Mtx mtx, J3DTexMtxInfo* info, J3DModel* model) {
    if (!active() || info == nullptr) {
        if (info != nullptr) {
            info->setEffectMtx(mtx);
        }
        return;
    }
    register_or_update(mtx, info, model, ReflectionStyle::Halo);
    info->setEffectMtx(mtx);
}

void apply_reflection_corrections_for_eye(AuroraEye eye) {
    if (!active()) {
        return;
    }
    // Step 1: rewrite each material's mEffectMtx for this eye, picking the
    // correction formula that matches the texgen's matrix structure.
    for (auto& entry : s_reflections) {
        Mtx corrected;
        MTXCopy(entry.base, corrected);
        switch (entry.style) {
        case ReflectionStyle::Standard:
            apply_reflection_correction_to(corrected, eye);
            break;
        case ReflectionStyle::Halo:
            apply_halo_correction_to(corrected, eye);
            break;
        }
        entry.info->setEffectMtx(corrected);
    }
    // Step 2: rebuild each affected model's DL so the new mEffectMtx flows
    // through calcMaterial -> mMtx -> diff -> baked into the DL that the
    // packet draw will invoke. Without this rebuild the DL still carries the
    // matrix value from the original actor Draw and both eyes render with it.
    // Dedupe linearly -- there's usually <= 2 distinct models, so a set is
    // overkill.
    J3DModel* rebuilt[8] = {};
    int rebuiltCount = 0;
    for (auto& entry : s_reflections) {
        if (entry.model == nullptr) {
            continue;
        }
        bool already = false;
        for (int i = 0; i < rebuiltCount; ++i) {
            if (rebuilt[i] == entry.model) { already = true; break; }
        }
        if (already || rebuiltCount >= 8) {
            continue;
        }
        // Use simpleCalcMaterial(j3dDefaultMtx) -- same input the actor Draw
        // used (modelData->simpleCalcMaterial((MtxP)j3dDefaultMtx)). Going
        // through J3DModel::calcMaterial would feed the joint's anm matrix
        // instead, producing a different mMtx than the actor Draw expected
        // and leaving each eye's rebuilt DL with a constant offset.
        entry.model->getModelData()->simpleCalcMaterial((MtxP)j3dDefaultMtx);
        entry.model->diff();
        rebuilt[rebuiltCount++] = entry.model;
    }
}

void clear_reflection_registry() {
    s_reflections.clear();
}

f32 current_eye_offset_x() {
    if (!active()) {
        return 0.0f;
    }
    const f32 separation = getSettings().game.stereoEyeSeparation.getValue();
    if (separation <= 0.0001f) {
        return 0.0f;
    }
    const f32 sign = (s_current_eye == AURORA_EYE_LEFT) ? -1.0f : 1.0f;
    return sign * (separation * 0.5f);
}

f32 refraction_skew_correction_x(f32 srt_z_view) {
    const f32 eyeOffsetX = current_eye_offset_x();
    if (eyeOffsetX == 0.0f) {
        return 0.0f;
    }
    const f32 convergence = getSettings().game.stereoConvergence.getValue();
    if (convergence <= 0.0001f) {
        return 0.0f;
    }
    return -eyeOffsetX * srt_z_view / convergence;
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
