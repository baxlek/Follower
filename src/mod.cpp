#include "mods/service.hpp"
#include "mods/svc/actor.h"
#include "mods/svc/hook.hpp"
#include "mods/svc/log.hpp"

// Game includes
#include "d/actor/d_a_npc_kasi_kyu.h"
#include "d/d_com_inf_game.h"
#include "d/d_item_data.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_name.h"

#include <string>

DEFINE_MOD();

IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(ActorService, svc_actor);

// Example game hook: turn heart drops into green rupees.
DEFINE_HOOK(fopAcM_createItem, CreateItem);

static HookAction on_create_item_pre(ModContext*, void* args, void*, void*) {
    int& itemNo = mods::arg_ref<int>(args, 1);
    if (itemNo == dItemNo_HEART_e) {
        itemNo = dItemNo_GREEN_RUPEE_e;
    }
    return HOOK_CONTINUE;
}

// ---------------------------------------------------------------------------
// STAR tent follower
//
// Kili (daNpcKasiKyu_c, stage name "km_Kyu") is one of the three fans who wait
// outside Castle Town's STAR minigame tent. Normally, once the player has
// beaten the STAR game (dSv_event_flag_c::F_290) and talks to the group again,
// group leader Hanna signals all three NPCs to run away (daNpcKasiKyu_c::escape).
// Here we intercept that signal: instead of letting Kili flee, we flag her as
// a permanent follower, take over her per-tick behavior, and keep her alive
// across loading zones by respawning her at the player's new position whenever
// the stage changes.
// ---------------------------------------------------------------------------

// Mirrors the file-local `Motion` enum in Dusklight's d_a_npc_kasi_kyu.cpp (not exposed via the
// header), used as the first argument to daNpcKasiKyu_c::setMotion.
constexpr int kMotWaitALoop = 11;
constexpr int kMotWalkA = 6;
constexpr int kMotRunA = 8;

constexpr f32 kFollowDistance = 220.0f;  // Stand still once this close to the player.
constexpr f32 kRunDistance = 550.0f;     // Sprint to catch up once this far behind.
constexpr f32 kWalkSpeed = 7.0f;
constexpr f32 kRunSpeed = 16.0f;
constexpr s16 kTurnRate = 0x800;

// dSv_event_flag_c::F_290 - Castle Town - Star Game 1 cleared
constexpr u32 kStarGameClearedFlag = 290;

static bool g_following = false;
static ActorId g_followerId = 0;
static std::string g_lastStageName;

static void follow_tick(daNpcKasiKyu_c* self) {
    fopAc_ac_c* player = dComIfGp_getPlayer(0);
    if (player == nullptr) {
        return;
    }

    self->_turn_pos(player->current.pos, kTurnRate);

    const f32 dist = fopAcM_searchActorDistanceXZ(self, player);
    if (dist > kRunDistance) {
        self->setMotion(kMotRunA, -1.0f, 0);
        fopAcM_SetSpeedF(self, kRunSpeed);
    } else if (dist > kFollowDistance) {
        self->setMotion(kMotWalkA, -1.0f, 0);
        fopAcM_SetSpeedF(self, kWalkSpeed);
    } else {
        self->setMotion(kMotWaitALoop, -1.0f, 0);
        fopAcM_SetSpeedF(self, 0.0f);
    }

    self->playMotion();
}

DEFINE_HOOK(&daNpcKasiKyu_c::main, KasiKyuMain);

// Takes over Kili's entire per-tick behavior once she's following. daNpcF_c::execute() only
// performs position integration/ground collision when main() returns TRUE, so we must force that
// even though we skip the original body.
static HookAction on_kasi_kyu_main_pre(ModContext*, void* args, void* retval, void*) {
    daNpcKasiKyu_c* self = mods::arg<daNpcKasiKyu_c*>(args, 0);

    if (g_following && fopAcM_GetID(self) == g_followerId) {
        follow_tick(self);
        if (retval != nullptr) {
            *static_cast<BOOL*>(retval) = TRUE;
        }
        return HOOK_SKIP_ORIGINAL;
    }

    return HOOK_CONTINUE;
}

DEFINE_HOOK(&daNpcKasiKyu_c::escape, KasiKyuEscape);

// Fires the first (and only) time Hanna signals Kili to run away after the post-STAR-game
// conversation. We latch "following" mode here and cancel the flee entirely.
static HookAction on_kasi_kyu_escape_pre(ModContext*, void* args, void*, void*) {
    daNpcKasiKyu_c* self = mods::arg<daNpcKasiKyu_c*>(args, 0);

    if (!g_following && daNpcF_chkEvtBit(kStarGameClearedFlag)) {
        g_following = true;
        g_followerId = fopAcM_GetID(self);
        g_lastStageName = dComIfGp_getStartStageName();
        mods::log::info("Kili is now following the player");
    }

    if (g_following && fopAcM_GetID(self) == g_followerId) {
        return HOOK_SKIP_ORIGINAL;
    }

    return HOOK_CONTINUE;
}

// Respawns Kili at the player's current position/room. Used after a loading zone destroys her
// actor so she can keep following in the new stage.
static void respawn_follower_at_player() {
    fopAc_ac_c* player = dComIfGp_getPlayer(0);
    if (player == nullptr) {
        return;
    }

    ActorSpawnParams params = {
        .parameters = 0,
        .argument = 0,
        .room_num = fopAcM_GetRoomNo(player),
        .position = {player->current.pos.x, player->current.pos.y, player->current.pos.z},
        .angle = {player->current.angle.x, player->current.angle.y, player->current.angle.z},
        .scale = {1.0f, 1.0f, 1.0f},
    };

    ActorId newId = 0;
    if (svc_actor->create_actor_from_name(mod_ctx, "km_Kyu", &params, &newId) == MOD_OK) {
        g_followerId = newId;
        mods::log::info("Respawned Kili follower in new stage");
    } else {
        mods::log::error("failed to respawn Kili follower after stage change");
    }
}

extern "C" {
MOD_EXPORT ModResult mod_initialize(ModError*) {
    // Installs a pre hook on fopAcM_createItem.
    ModResult result = mods::hook::add_pre<CreateItem>(on_create_item_pre);
    if (result != MOD_OK) {
        mods::log::error("failed to install pre_hook on_create_item_pre");
        return result;
    }

    result = mods::hook::add_pre<KasiKyuMain>(on_kasi_kyu_main_pre);
    if (result != MOD_OK) {
        mods::log::error("failed to install pre_hook on_kasi_kyu_main_pre");
        return result;
    }

    result = mods::hook::add_pre<KasiKyuEscape>(on_kasi_kyu_escape_pre);
    if (result != MOD_OK) {
        mods::log::error("failed to install pre_hook on_kasi_kyu_escape_pre");
        return result;
    }

    mods::log::info("my_mod initialized");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    if (g_following) {
        const char* stageName = dComIfGp_getStartStageName();
        if (stageName != nullptr && g_lastStageName != stageName) {
            g_lastStageName = stageName;

            fopAc_ac_c* existing = fopAcM_SearchByID(g_followerId);
            const bool stillAlive =
                existing != nullptr && fopAcM_GetName(existing) == fpcNm_NPC_KASIKYU_e;
            if (!stillAlive) {
                respawn_follower_at_player();
            }
        }
    }

    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    return MOD_OK;
}
}
