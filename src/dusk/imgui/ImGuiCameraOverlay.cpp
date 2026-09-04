#include "f_op/f_op_camera_mng.h"
#include "SSystem/SComponent/c_xyz.h"
#include "d/d_com_inf_game.h"
#include "d/actor/d_a_alink.h"

#include "imgui.h"
#include "ImGuiConfig.hpp"
#include "ImGuiConsole.hpp"
#include "ImGuiMenuTools.hpp"
#include "dusk/settings.h"
#include "dusk/stereo.h"

namespace dusk {
    void ImGuiMenuTools::ShowCameraOverlay() {
        if (!getSettings().backend.enableAdvancedSettings ||
            !ImGuiConsole::CheckMenuViewToggle(ImGuiKey_F9, m_showCameraOverlay))
        {
            return;
        }

        auto* cam = (camera_process_class*)dCam_getCamera();

        if (!m_showCameraOverlay || cam == nullptr)
            return;

        auto* dCam = &cam->mCamera;

        ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
        if (m_cameraOverlayCorner != -1) {
            SetOverlayWindowLocation(m_cameraOverlayCorner);
            windowFlags |= ImGuiWindowFlags_NoMove;
        }

        // ImGui::SetNextWindowBgAlpha(0.65f);

        if (!ImGui::Begin("Camera Debug", nullptr, windowFlags)) {
            ImGui::End();
            return;
        }

        ImGui::SeparatorText("Camera Transform Data");

        cXyz center = dCam->mCenter;
        cXyz eye = dCam->mEye;

        if (ImGui::InputFloat3("Camera Center", &center.x)) {
            dCam->Reset(center, eye);
        }
        if (ImGui::InputFloat3("Camera Eye", &eye.x)) {
            dCam->Reset(center, eye);
        }

        if (ImGui::InputFloat("Camera FOV", &dCam->mFovy)) {
            dCam->mFovy = std::clamp(dCam->mFovy, 0.1f, 179.9f);
        }

        ImGui::SeparatorText("Options");

        bool eventRunning = (dComIfGp_event_runCheck() || dComIfGp_isPauseFlag()) && !getSettings().game.debugFlyCam;
        if (eventRunning) {
            ImGui::BeginDisabled();
        }
        config::ImGuiCheckbox("Fly Mode", getSettings().game.debugFlyCam);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (eventRunning) {
                ImGui::SetTooltip("Cannot enable while paused or during an active event.");
            } else {
                ImGui::SetTooltip("Detach camera and fly freely.\n"
                                  "WASD/Arrows/Left stick: move, Mouse/C-stick: look\n"
                                  "Ctrl/L: down, Space/R: up, Shift/Z: fast\n"
                                "Q Key/Y: roll left, R Key/X: roll right");
            }
        }
        if (eventRunning) {
            ImGui::EndDisabled();
        }

        if (!getSettings().game.debugFlyCam) {
            ImGui::BeginDisabled();
        }
        config::ImGuiCheckbox("Freeze Time", getSettings().game.debugFlyCamLockEvents);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (!getSettings().game.debugFlyCam) {
                ImGui::SetTooltip("Enable Fly Mode first.");
            } else {
                ImGui::SetTooltip("Freezes the game while flying.");
            }
        }
        if (!getSettings().game.debugFlyCam) {
            ImGui::EndDisabled();
        }

        if (dusk::stereo::active()) {
            ImGui::SeparatorText("Stereo 3D Debug");
            const auto dbg = dusk::stereo::debug_state();
            ImGui::Text("Fovy (raw): %.2f deg", cam->view.fovy);
            ImGui::Text("tan(half horiz FoV): %.4f", dbg.tanHalfH);
            ImGui::Text("Separation (clip space): %.4f  (%.1f%% of screen)", dbg.separation,
                dbg.separation * 100.0f);
            ImGui::Text("Closeup convergence scale: %.3f", dbg.closeupScale);
            ImGui::Text("Convergence (effective): %.1f", dbg.convergence);
            // Derived, not stored -- expected to move with both FoV and
            // convergence. A constant reading here means something upstream is
            // caching it.
            ImGui::Text("Eye baseline (derived): %.2f units", dbg.eyeBaseline);
            if (getSettings().game.stereoAutoConvergence.getValue()) {
                if (!dbg.autoConvDepthAvailable) {
                    ImGui::TextColored(ImVec4(1.f, 0.5f, 0.f, 1.f),
                        "Auto convergence: NO DEPTH SNAPSHOT");
                } else {
                    ImGui::Text("Auto convergence: %s  (ceiling %.1f)",
                        dbg.autoConvEngaged ? "engaged" : "idle", dbg.manualConvergence);
                    ImGui::Text("Nearest depth (smoothed): %.1f units", dbg.autoConvNearDepth);
                }
            }
            ImGui::Text("Close-up focus active: %s", dusk::stereo::is_close_up_focus_active() ? "TRUE" : "false");
            if (auto* player = dComIfGp_getLinkPlayer()) {
                auto* alink = static_cast<daAlink_c*>(player);
                ImGui::Text("Link ProcID: 0x%X", static_cast<unsigned>(alink->mProcID));
                ImGui::Text("mSight draw flag: %s", alink->mSight.getDrawFlg() ? "TRUE" : "false");
                ImGui::Text("checkBowAnime: %s", alink->checkBowAnime() ? "TRUE" : "false");
            }
        }

        ShowCornerContextMenu(m_cameraOverlayCorner, 0);

        ImGui::End();
    }
}
