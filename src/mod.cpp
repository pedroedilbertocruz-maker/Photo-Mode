// Photo Mode for Dusklight.
//
// Press both stick buttons (L3 + R3) to freeze the game, hide the HUD and fly a free camera.
// The time of day can be changed while frozen, which makes it easy to judge lighting mods at
// sunrise, noon and sunset from the exact same framing.
//
// How it works:
//  - L3/R3 are not GameCube buttons, but Aurora reports them in PADStatus::extButton, which the
//    game leaves unused. We read JUTGamePad::mPadStatus[0] once per frame.
//  - The camera is driven through a CameraService operator. Operators run inside dCamera_c::Run,
//    which the game skips while paused, so fopCam_Execute is hooked to keep the camera running
//    while everything else is frozen (the same approach as Dusklight's debug fly camera).
//  - Changing the clock while frozen re-runs CalcTevColor so light, sky and fog colors follow
//    (the normal per-frame environment update is paused along with the game).

#include "global.h"

#include "JSystem/JUtility/JUTGamePad.h"
#include "d/d_attention.h"
#include "d/d_com_inf_game.h"
#include "d/d_kankyo.h"
#include "d/d_meter2.h"
#include "d/d_s_play.h"
#include "dolphin/pad.h"
#include "f_op/f_op_camera_mng.h"
#include "f_pc/f_pc_method.h"
#include "m_Do/m_Do_controller_pad.h"
#include "mods/service.hpp"
#include "mods/svc/camera.h"
#include "mods/svc/config.h"
#include "mods/svc/hook.h"
#include "mods/svc/hook.hpp"
#include "mods/svc/log.h"
#include "mods/svc/ui.h"

#include "flycam.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

DEFINE_MOD();
IMPORT_SERVICE(CameraService, svc_camera);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(UiService, svc_ui);
IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(LogService, svc_log);

namespace {

constexpr float kCenterDistance = 100.0f;

DEFINE_HOOK_SYMBOL("src/f_op/f_op_camera.cpp#fopCam_Execute", int(camera_class*), CamExecute);
DEFINE_HOOK(&dMeter2_c::_draw, MeterDraw);
DEFINE_HOOK(&dAttention_c::Draw, AttentionDraw);

ConfigVarHandle g_cvarHideHud = 0;
ConfigVarHandle g_cvarKeepTime = 0;
ConfigVarHandle g_cvarMoveSpeed = 0;  // percent
ConfigVarHandle g_cvarLookSpeed = 0;  // percent
ConfigVarHandle g_cvarTimeSpeed = 0;  // in-game minutes per real second
ConfigVarHandle g_cvarInvertX = 0;
ConfigVarHandle g_cvarInvertY = 0;

CameraOperatorHandle g_operator = 0;
UiWindowHandle g_controlsWindow = 0;
bool g_freezeHooked = false;

enum class Mode { Off, Entering, Active, Exiting };

struct Session {
    Mode mode = Mode::Off;
    photo::Camera cam;
    // Game camera state on entry, restored on exit.
    float savedEye[3] = {};
    float savedCenter[3] = {};
    float savedFovy = 60.0f;
    float savedBank = 0.0f;
    // Clock on entry and whether it was changed.
    float savedTime = 0.0f;
    bool timeChanged = false;
    std::chrono::steady_clock::time_point lastTick{};
};

Session g_session;
bool g_prevCombo = false;

int64_t get_int(ConfigVarHandle handle, int64_t fallback) {
    int64_t value = fallback;
    if (handle == 0 || svc_config->get_int(mod_ctx, handle, &value) != MOD_OK) {
        return fallback;
    }
    return value;
}

bool get_bool(ConfigVarHandle handle, bool fallback) {
    bool value = fallback;
    if (handle == 0 || svc_config->get_bool(mod_ctx, handle, &value) != MOD_OK) {
        return fallback;
    }
    return value;
}

bool active() {
    return g_session.mode != Mode::Off;
}

void toast(const char* title, const char* body) {
    UiToastDesc desc = UI_TOAST_DESC_INIT;
    desc.title_rml = title;
    desc.body_rml = body;
    desc.duration_ms = 2500;
    svc_ui->push_toast(mod_ctx, &desc);
}

// --- Event lock (keeps menus and the player from reacting while frozen) ---------------------

void lock_events(bool lock) {
    dEvt_control_c* event = dComIfGp_getEvent();
    if (event == nullptr) {
        return;
    }
    event->mEventStatus = lock ? 1 : 0;
    dComIfGp_getEventManager().setCameraPlay(lock ? 1 : 0);
}

// --- Time of day ------------------------------------------------------------------------------

void apply_time(float degrees) {
    dComIfGs_setTime(degrees);
    g_env_light.daytime = degrees;
    // The environment's per-frame update is frozen with the game; recompute the light, sky and
    // fog colors for the new time directly.
    g_env_light.CalcTevColor();
}

// --- Enter / exit -----------------------------------------------------------------------------

bool can_enter(const char** reason) {
    if (dComIfGp_getCamera(0) == nullptr || dComIfGp_getPlayer(0) == nullptr) {
        *reason = "Not available here.";
        return false;
    }
    if (dComIfGp_event_runCheck() || dComIfGp_isPauseFlag()) {
        *reason = "Not available during cutscenes or menus.";
        return false;
    }
    return true;
}

void request_enter() {
    if (active()) {
        return;
    }
    const char* reason = nullptr;
    if (!can_enter(&reason)) {
        toast("Photo Mode", reason);
        return;
    }
    g_session = Session{};
    g_session.mode = Mode::Entering;
    g_session.savedTime = dComIfGs_getTime();
}

void request_exit() {
    if (g_session.mode == Mode::Entering) {
        g_session.mode = Mode::Off;
    } else if (g_session.mode == Mode::Active) {
        g_session.mode = Mode::Exiting;
    }
}

void toggle() {
    if (active()) {
        request_exit();
    } else {
        request_enter();
    }
}

// Exit work that does not depend on the camera running (also used on shutdown).
void finish_exit() {
    lock_events(false);
    if (g_session.timeChanged && !get_bool(g_cvarKeepTime, false)) {
        apply_time(g_session.savedTime);
    }
    g_session.mode = Mode::Off;
}

// --- Input ------------------------------------------------------------------------------------

bool stick_buttons_held() {
    const u32 ext = JUTGamePad::mPadStatus[0].extButton;
    return (ext & PAD_BUTTON_LEFT_STICK) != 0 && (ext & PAD_BUTTON_RIGHT_STICK) != 0;
}

photo::Input read_input() {
    interface_of_controller_pad& pad = mDoCPd_c::getCpadInfo(PAD_1);
    photo::Input in;
    in.move_x = pad.mMainStickPosX;
    in.move_y = pad.mMainStickPosY;
    // Same sign convention as Dusklight's debug fly camera: -C stick X turns right.
    in.look_x = -pad.mCStickPosX * (get_bool(g_cvarInvertX, false) ? -1.0f : 1.0f);
    in.look_y = pad.mCStickPosY;
    in.rise = pad.mTriggerRight;
    in.sink = pad.mTriggerLeft;
    in.fast = mDoCPd_c::getHoldZ(PAD_1) != 0;
    if (mDoCPd_c::getHoldY(PAD_1)) {
        in.roll -= 1.0f;
    }
    if (mDoCPd_c::getHoldX(PAD_1)) {
        in.roll += 1.0f;
    }
    if (mDoCPd_c::getHoldUp(PAD_1)) {
        in.zoom += 1.0f;
    }
    if (mDoCPd_c::getHoldDown(PAD_1)) {
        in.zoom -= 1.0f;
    }
    return in;
}

photo::Tuning tuning() {
    photo::Tuning t;
    t.move_speed *= static_cast<float>(std::clamp<int64_t>(get_int(g_cvarMoveSpeed, 100), 10, 500)) / 100.0f;
    t.look_speed *= static_cast<float>(std::clamp<int64_t>(get_int(g_cvarLookSpeed, 100), 10, 300)) / 100.0f;
    t.invert_y = get_bool(g_cvarInvertY, false);
    return t;
}

// --- Camera operator (runs inside dCamera_c::Run, game thread) --------------------------------

void write_state(CameraOperatorState* state, const float eye[3], const float center[3], float fovy,
    float bank) {
    for (int i = 0; i < 3; ++i) {
        state->eye[i] = eye[i];
        state->center[i] = center[i];
    }
    state->fovy = fovy;
    state->bank_degrees = bank;
}

bool on_camera_operate(ModContext*, CameraOperatorState* state, void*) {
    Session& s = g_session;
    if (s.mode == Mode::Off) {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();

    if (s.mode == Mode::Entering) {
        for (int i = 0; i < 3; ++i) {
            s.savedEye[i] = state->eye[i];
            s.savedCenter[i] = state->center[i];
        }
        s.savedFovy = state->fovy;
        s.savedBank = state->bank_degrees;
        s.cam = photo::from_look_at(state->eye, state->center, state->fovy, state->bank_degrees);
        s.lastTick = now;
        s.mode = Mode::Active;
    }

    if (s.mode == Mode::Exiting) {
        // Hand the game camera back exactly where it was.
        write_state(state, s.savedEye, s.savedCenter, s.savedFovy, s.savedBank);
        finish_exit();
        return true;
    }

    const float dt = std::chrono::duration<float>(now - s.lastTick).count();
    s.lastTick = now;

    lock_events(true);

    if (mDoCPd_c::getTrigB(PAD_1)) {
        s.mode = Mode::Exiting;
        write_state(state, s.savedEye, s.savedCenter, s.savedFovy, s.savedBank);
        finish_exit();
        return true;
    }
    if (mDoCPd_c::getTrigA(PAD_1)) {
        s.cam.bank = s.savedBank;
        s.cam.fovy = s.savedFovy;
    }

    photo::update(s.cam, read_input(), tuning(), dt);

    // D-pad left/right scrubs the clock.
    float scrub = 0.0f;
    if (mDoCPd_c::getHoldLeft(PAD_1)) {
        scrub -= 1.0f;
    }
    if (mDoCPd_c::getHoldRight(PAD_1)) {
        scrub += 1.0f;
    }
    if (scrub != 0.0f && dt > 0.0f) {
        const float minutesPerSecond =
            static_cast<float>(std::clamp<int64_t>(get_int(g_cvarTimeSpeed, 120), 5, 720));
        const float hours = scrub * minutesPerSecond / 60.0f * std::min(dt, 0.1f);
        apply_time(photo::advance_time_degrees(dComIfGs_getTime(), hours));
        s.timeChanged = true;
    }

    float center[3];
    photo::center(s.cam, kCenterDistance, center);
    write_state(state, s.cam.eye, center, s.cam.fovy, s.cam.bank);
    return true;
}

// --- Hooks ------------------------------------------------------------------------------------

// Keeps the camera process executing while the scene is frozen, mirroring fopCam_Execute's own
// debug-fly-camera branch.
HookAction on_cam_execute_pre(ModContext*, void* args, void* retval, void*) {
    if (!active()) {
        return HOOK_CONTINUE;
    }
    camera_class* camera = mods::arg<camera_class*>(args, 0);
    dScnPly_c::setPauseTimer(1);
    const int result = fpcMtd_Execute((process_method_class*)camera->submethod, camera);
    if (retval != nullptr) {
        *static_cast<int*>(retval) = result;
    }
    return HOOK_SKIP_ORIGINAL;
}

HookAction on_meter_draw_pre(ModContext*, void*, void* retval, void*) {
    if (!active() || !get_bool(g_cvarHideHud, true)) {
        return HOOK_CONTINUE;
    }
    if (retval != nullptr) {
        *static_cast<int*>(retval) = 1;
    }
    return HOOK_SKIP_ORIGINAL;
}

HookAction on_attention_draw_pre(ModContext*, void*, void*, void*) {
    if (!active() || !get_bool(g_cvarHideHud, true)) {
        return HOOK_CONTINUE;
    }
    return HOOK_SKIP_ORIGINAL;
}

// --- UI ---------------------------------------------------------------------------------------

void add_control(UiElementHandle pane, const UiControlDesc& desc) {
    svc_ui->pane_add_control(mod_ctx, pane, &desc, nullptr);
}

void add_toggle(UiElementHandle pane, const char* label, ConfigVarHandle cvar, const char* help) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_TOGGLE;
    control.label = label;
    control.help_rml = help;
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = cvar;
    add_control(pane, control);
}

void add_number(UiElementHandle pane, const char* label, ConfigVarHandle cvar, int64_t min,
    int64_t max, int64_t step, const char* suffix, const char* help) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_NUMBER;
    control.label = label;
    control.help_rml = help;
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = cvar;
    control.min = min;
    control.max = max;
    control.step = step;
    control.suffix = suffix;
    add_control(pane, control);
}

constexpr const char* kControlsText[] = {
    "L3 + R3: enter / exit photo mode (B also exits)",
    "Left stick: move    Right stick: look",
    "R / L: up / down    Z (hold): faster",
    "X / Y: tilt    D-pad up / down: zoom    A: reset tilt and zoom",
    "D-pad left / right: change the time of day",
};

ModResult build_controls_tab(
    ModContext*, UiWindowHandle, UiElementHandle left, UiElementHandle right, void*, ModError*) {
    (void)right;
    svc_ui->pane_add_section(mod_ctx, left, "Controls");
    for (const char* line : kControlsText) {
        svc_ui->pane_add_text(mod_ctx, left, line, nullptr);
    }

    svc_ui->pane_add_section(mod_ctx, left, "Camera");
    add_number(left, "Move Speed", g_cvarMoveSpeed, 10, 500, 10, "%", nullptr);
    add_number(left, "Look Speed", g_cvarLookSpeed, 10, 300, 10, "%", nullptr);
    add_toggle(left, "Invert Look X", g_cvarInvertX, nullptr);
    add_toggle(left, "Invert Look Y", g_cvarInvertY, nullptr);

    svc_ui->pane_add_section(mod_ctx, left, "Scene");
    add_toggle(left, "Hide HUD", g_cvarHideHud, "Hides the HUD and targeting arrows in photo mode.");
    add_number(left, "Time Speed", g_cvarTimeSpeed, 5, 720, 5, " min/s",
        "How fast the D-pad changes the clock: in-game minutes per real second.");
    add_toggle(left, "Keep Time on Exit", g_cvarKeepTime,
        "Keep the time of day you picked in photo mode. Off restores the original time.");
    return MOD_OK;
}

void on_controls_window_closed(ModContext*, UiWindowHandle, void*) {
    g_controlsWindow = 0;
}

void on_open_controls(ModContext*, void*) {
    if (g_controlsWindow != 0) {
        return;
    }
    UiTabDesc tabs[1] = {UI_TAB_DESC_INIT};
    tabs[0].title = "Photo Mode";
    tabs[0].build = build_controls_tab;
    UiWindowDesc desc = UI_WINDOW_DESC_INIT;
    desc.tabs = tabs;
    desc.tab_count = 1;
    desc.on_closed = on_controls_window_closed;
    if (svc_ui->window_push(mod_ctx, &desc, &g_controlsWindow) != MOD_OK) {
        svc_log->error(mod_ctx, "failed to open photo mode settings");
    }
}

ModResult build_panel(ModContext*, UiElementHandle panel, void*, ModError*) {
    svc_ui->pane_add_text(mod_ctx, panel, "Press L3 + R3 in game to enter photo mode.", nullptr);
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_BUTTON;
    control.label = "Settings and Controls";
    control.on_pressed = on_open_controls;
    add_control(panel, control);
    return MOD_OK;
}

ModResult register_bool(const char* name, bool def, ConfigVarHandle& out, ModError* error) {
    ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
    desc.name = name;
    desc.type = CONFIG_VAR_BOOL;
    desc.default_bool = def;
    if (svc_config->register_var(mod_ctx, &desc, &out) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register photo mode option");
    }
    return MOD_OK;
}

ModResult register_int(const char* name, int64_t def, ConfigVarHandle& out, ModError* error) {
    ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
    desc.name = name;
    desc.type = CONFIG_VAR_INT;
    desc.default_int = def;
    if (svc_config->register_var(mod_ctx, &desc, &out) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register photo mode option");
    }
    return MOD_OK;
}

}  // namespace

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    struct BoolOpt {
        const char* name;
        bool def;
        ConfigVarHandle* handle;
    };
    struct IntOpt {
        const char* name;
        int64_t def;
        ConfigVarHandle* handle;
    };
    const BoolOpt bools[] = {
        {"hideHud", true, &g_cvarHideHud},
        {"keepTimeOnExit", false, &g_cvarKeepTime},
        {"invertLookX", false, &g_cvarInvertX},
        {"invertLookY", false, &g_cvarInvertY},
    };
    const IntOpt ints[] = {
        {"moveSpeed", 100, &g_cvarMoveSpeed},
        {"lookSpeed", 100, &g_cvarLookSpeed},
        {"timeSpeed", 120, &g_cvarTimeSpeed},
    };
    for (const auto& opt : bools) {
        if (const ModResult r = register_bool(opt.name, opt.def, *opt.handle, error); r != MOD_OK) {
            return r;
        }
    }
    for (const auto& opt : ints) {
        if (const ModResult r = register_int(opt.name, opt.def, *opt.handle, error); r != MOD_OK) {
            return r;
        }
    }

    CameraOperatorDesc opDesc = CAMERA_OPERATOR_DESC_INIT;
    opDesc.debug_name = "photo mode";
    opDesc.priority = 1000;  // above ordinary camera mods while photo mode is on
    opDesc.operate = on_camera_operate;
    if (svc_camera->register_camera_operator(mod_ctx, &opDesc, &g_operator) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register the photo camera");
    }

    // Without this hook the camera stops while the game is frozen; photo mode then still works,
    // just without freezing.
    g_freezeHooked = mods::hook::add_pre<CamExecute>(on_cam_execute_pre) == MOD_OK;
    if (!g_freezeHooked) {
        svc_log->warn(mod_ctx, "could not hook fopCam_Execute; photo mode will not freeze the game");
    }
    if (mods::hook::add_pre<MeterDraw>(on_meter_draw_pre) != MOD_OK ||
        mods::hook::add_pre<AttentionDraw>(on_attention_draw_pre) != MOD_OK)
    {
        svc_log->warn(mod_ctx, "could not hook HUD drawing; the HUD will stay visible");
    }

    UiModsPanelDesc panelDesc = UI_MODS_PANEL_DESC_INIT;
    panelDesc.build = build_panel;
    svc_ui->register_mods_panel(mod_ctx, &panelDesc);
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    // Ignore the combo while a menu or window has focus.
    bool uiVisible = false;
    svc_ui->is_any_document_visible(mod_ctx, &uiVisible);

    const bool combo = stick_buttons_held();
    if (combo && !g_prevCombo && !uiVisible) {
        toggle();
    }
    g_prevCombo = combo;

    // If something else takes over (a cutscene starts, the stage unloads), drop out cleanly.
    if (g_session.mode == Mode::Active && dComIfGp_getCamera(0) == nullptr) {
        finish_exit();
    }
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    if (active()) {
        finish_exit();
    }
    g_session = Session{};
    g_prevCombo = false;
    g_operator = 0;
    g_controlsWindow = 0;
    g_freezeHooked = false;
    g_cvarHideHud = g_cvarKeepTime = g_cvarMoveSpeed = g_cvarLookSpeed = 0;
    g_cvarTimeSpeed = g_cvarInvertX = g_cvarInvertY = 0;
    return MOD_OK;
}
}
