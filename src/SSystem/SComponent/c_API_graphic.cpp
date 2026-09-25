/**
 * c_API_graphic.cpp
 *
 */

#include "SSystem/SComponent/c_API_graphic.h"
#include "SSystem/SComponent/c_API.h"

#ifdef TARGET_PC
#include "dusk/stereo.h"
#include "d/d_kankyo.h"
#include <aurora/aurora.h>
#endif

void cAPIGph_Painter() {
#ifdef TARGET_PC
    if (dusk::stereo::active()) {
        // Step the close-up convergence pull-in once per simulation frame,
        // BEFORE the eye loop, so both eyes use the same smoothed value.
        // (There used to be a second tick here for a rate-limited FoV
        // separation auto-scale; the clip-space parameterization makes it
        // identically 1, so it's gone -- see dusk/stereo.h.)
        dusk::stereo::closeup_scale_tick();
        // Then the depth-driven auto-convergence loop, which takes the
        // close-up-scaled convergence as its ceiling -- so it must run after
        // the tick above, and before anything reads effective_convergence().
        dusk::stereo::auto_convergence_tick();
        // Re-push the stereo config every frame, after the tick and before the
        // eye loop. Under clip space the world-unit eye baseline Aurora's
        // GX-layer texgen fixes consume is DERIVED from the live FoV and the
        // live close-up-scaled convergence, so pushing only on UI changes
        // would leave those fixes reading a stale offset the moment the camera
        // zooms or a close-up engages.
        dusk::stereo::apply_config_from_settings();
        // Capture the frame's global light state before either eye runs. The
        // per-frame setup that establishes it happens outside this loop, while
        // draw-time code (grass, flowers, particles) rewrites it mid-pass and
        // never restores it -- so without this the second eye would inherit the
        // first eye's leftovers and light half the scene differently.
        dKy_stereo_saveLightBaseline();
        for (AuroraEye eye : {AURORA_EYE_LEFT, AURORA_EYE_RIGHT}) {
            aurora_set_active_eye(eye);
            dusk::stereo::push_eye_offset(eye);
            dKy_stereo_restoreLightBaseline();
            g_cAPI_Interface.painterMtd();
            dusk::stereo::pop_eye_offset();
        }
        return;
    }
#endif
    g_cAPI_Interface.painterMtd();
}

void cAPIGph_BeforeOfDraw() {
    g_cAPI_Interface.beforeOfDrawMtd();
}

void cAPIGph_AfterOfDraw() {
    g_cAPI_Interface.afterOfDrawMtd();
}
