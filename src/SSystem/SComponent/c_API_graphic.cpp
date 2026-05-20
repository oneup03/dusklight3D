/**
 * c_API_graphic.cpp
 *
 */

#include "SSystem/SComponent/c_API_graphic.h"
#include "SSystem/SComponent/c_API.h"

#ifdef TARGET_PC
#include "dusk/stereo.h"
#include <aurora/aurora.h>
#endif

void cAPIGph_Painter() {
#ifdef TARGET_PC
    if (dusk::stereo::active()) {
        // Update the auto-convergence target and close-up scale roll-off once
        // per simulation frame, BEFORE the eye loop, so both eyes use the same
        // adjusted convergence and same smoothed close-up scale.
        dusk::stereo::auto_convergence_tick();
        dusk::stereo::closeup_scale_tick();
        for (AuroraEye eye : {AURORA_EYE_LEFT, AURORA_EYE_RIGHT}) {
            aurora_set_active_eye(eye);
            dusk::stereo::push_eye_offset(eye);
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
