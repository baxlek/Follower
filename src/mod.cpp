#include "mods/service.hpp"
#include "mods/svc/actor.h"
#include "mods/svc/config.h"
#include "mods/svc/hook.hpp"
#include "mods/svc/log.hpp"

// Game includes
#include "d/actor/d_a_npc_kasi_hana.h"
#include "d/actor/d_a_npc_kasi_kyu.h"
#include "d/actor/d_a_npc_kasi_mich.h"
#include "d/d_com_inf_game.h"
#include "d/d_item_data.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_name.h"

#include <cstring>

DEFINE_MOD();

IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(ActorService, svc_actor);
IMPORT_SERVICE(ConfigService, svc_config);

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
// Kili, Hanna, and Misha (daNpcKasiKyu_c / daNpcKasiHana_c / daNpcKasiMich_c,
// stage names "km_Kyu" / "km_Hana" / "km_Mich") are the three fans who wait
// outside Castle Town's STAR minigame tent. Normally, once the player has
// beaten the STAR game (dSv_event_flag_c::F_290) and talks to the group
// again, group leader Hanna signals all three NPCs to run away (via each
// NPC's own escape() method). A mod config option ("followerChoice") picks
// one of the three; when she's signaled to flee we cancel it, flag her as a
// permanent follower, and take over her per-tick behavior instead. She keeps
// following across loading zones and other room/area transitions by being
// respawned at the player's current position whenever her actor disappears
// (the game frees NPCs that belong to rooms/stages the player has left).
// ---------------------------------------------------------------------------

enum class FollowerChoice : int { Kili = 0, Hana = 1, Misha = 2 };

// Which NPC's escape() hook is allowed to latch "following" mode. Only one of
// the three NPCs can be the active follower; the other two flee normally.
static FollowerChoice g_selectedFollower = FollowerChoice::Kili;

static bool g_following = false;
static ActorId g_followerId = 0;

// Frames to wait between respawn attempts once the follower is missing, so a
// persistently failing spawn (e.g. missing resources) doesn't spam the actor
// service every tick.
constexpr int kRespawnCooldownFrames = 30;
static int g_respawnCooldown = 0;

constexpr f32 kFollowDistance = 220.0f;  // Stand still once this close to the player.
constexpr f32 kRunDistance = 550.0f;     // Sprint to catch up once this far behind.
constexpr f32 kWalkSpeed = 7.0f;
constexpr f32 kRunSpeed = 16.0f;
constexpr s16 kTurnRate = 0x800;

// dSv_event_flag_c::F_290 - Castle Town - Star Game 1 cleared
constexpr u32 kStarGameClearedFlag = 290;

// Per-NPC stage spawn name and fpcNm_* identity, used for respawning and for
// confirming a found ActorId truly still refers to the tracked NPC's actor
// type (in case the ID gets recycled by an unrelated actor).
struct FollowerNpcTraits {
    const char* stageName;
    s16 nameEnum;
};

static const FollowerNpcTraits kFollowerTraits[3] = {
    {"km_Kyu", fpcNm_NPC_KASIKYU_e},
    {"km_Hana", fpcNm_NPC_KASIHANA_e},
    {"km_Mich", fpcNm_NPC_KASIMICH_e},
};

// Mirrors each NPC's file-local `Motion` enum in Dusklight's
// d_a_npc_kasi_{kyu,hana,mich}.cpp (not exposed via their headers, and not
// numbered identically between the three classes), used as the first
// argument to their setMotion(). Misha has no looping idle animation, so she
// uses her plain standing pose instead.
struct FollowMotionSet {
    int run;
    int walk;
    int idle;
};

constexpr FollowMotionSet kKyuMotions{8, 6, 11};   // MOT_W_RUN_A, MOT_W_WALK_A, MOT_W_WAIT_A_LOOP
constexpr FollowMotionSet kHanaMotions{9, 7, 11};  // MOT_W_RUN_A, MOT_W_WALK_A, MOT_W_WAIT_A_LOOP
constexpr FollowMotionSet kMichMotions{8, 6, 4};   // MOT_W_RUN_A, MOT_W_WALK_A, MOT_W_WAIT_A

static FollowerChoice parse_follower_choice(const char* value) {
    if (value != nullptr) {
        if (!strcmp(value, "hana") || !strcmp(value, "hanna")) {
            return FollowerChoice::Hana;
        }
        if (!strcmp(value, "mich") || !strcmp(value, "misha")) {
            return FollowerChoice::Misha;
        }
    }
    return FollowerChoice::Kili;
}

static const char* follower_choice_name(FollowerChoice choice) {
    switch (choice) {
        case FollowerChoice::Hana:
            return "Hanna";
        case FollowerChoice::Misha:
            return "Misha";
        default:
            return "Kili";
    }
}

template <class T>
static void follow_tick_impl(T* self, const FollowMotionSet& motions) {
    fopAc_ac_c* player = dComIfGp_getPlayer(0);
    if (player == nullptr) {
        return;
    }

    self->_turn_pos(player->current.pos, kTurnRate);

    const f32 dist = fopAcM_searchActorDistanceXZ(self, player);
    if (dist > kRunDistance) {
        self->setMotion(motions.run, -1.0f, 0);
        fopAcM_SetSpeedF(self, kRunSpeed);
    } else if (dist > kFollowDistance) {
        self->setMotion(motions.walk, -1.0f, 0);
        fopAcM_SetSpeedF(self, kWalkSpeed);
    } else {
        self->setMotion(motions.idle, -1.0f, 0);
        fopAcM_SetSpeedF(self, 0.0f);
    }

    self->playMotion();
}

// Takes over an NPC's entire per-tick behavior once she's the active,
// following NPC. daNpcF_c::execute() only performs position
// integration/ground collision when main() returns TRUE, so we must force
// that even though we skip the original body.
template <class T>
static HookAction on_kasi_main_pre(
    FollowerChoice choice, void* args, void* retval, const FollowMotionSet& motions) {
    if (!g_following || g_selectedFollower != choice) {
        return HOOK_CONTINUE;
    }

    T* self = mods::arg<T*>(args, 0);
    if (fopAcM_GetID(self) != g_followerId) {
        return HOOK_CONTINUE;
    }

    follow_tick_impl(self, motions);
    if (retval != nullptr) {
        *static_cast<BOOL*>(retval) = TRUE;
    }
    return HOOK_SKIP_ORIGINAL;
}

// Fires the first (and only) time this NPC is signaled to run away after the
// post-STAR-game conversation. If she's the currently-selected follower
// choice, we latch "following" mode and cancel the flee entirely; otherwise
// we leave her alone so she flees normally.
template <class T>
static HookAction on_kasi_escape_pre(FollowerChoice choice, void* args) {
    if (g_selectedFollower != choice) {
        return HOOK_CONTINUE;
    }

    T* self = mods::arg<T*>(args, 0);

    if (!g_following && daNpcF_chkEvtBit(kStarGameClearedFlag)) {
        g_following = true;
        g_followerId = fopAcM_GetID(self);
        g_respawnCooldown = 0;
        mods::log::info("{} is now following the player", follower_choice_name(choice));
    }

    if (g_following && fopAcM_GetID(self) == g_followerId) {
        return HOOK_SKIP_ORIGINAL;
    }

    return HOOK_CONTINUE;
}

DEFINE_HOOK(&daNpcKasiKyu_c::main, KasiKyuMain);
DEFINE_HOOK(&daNpcKasiHana_c::main, KasiHanaMain);
DEFINE_HOOK(&daNpcKasiMich_c::main, KasiMichMain);

static HookAction on_kasi_kyu_main_pre(ModContext*, void* args, void* retval, void*) {
    return on_kasi_main_pre<daNpcKasiKyu_c>(FollowerChoice::Kili, args, retval, kKyuMotions);
}

static HookAction on_kasi_hana_main_pre(ModContext*, void* args, void* retval, void*) {
    return on_kasi_main_pre<daNpcKasiHana_c>(FollowerChoice::Hana, args, retval, kHanaMotions);
}

static HookAction on_kasi_mich_main_pre(ModContext*, void* args, void* retval, void*) {
    return on_kasi_main_pre<daNpcKasiMich_c>(FollowerChoice::Misha, args, retval, kMichMotions);
}

DEFINE_HOOK(&daNpcKasiKyu_c::escape, KasiKyuEscape);
DEFINE_HOOK(&daNpcKasiHana_c::escape, KasiHanaEscape);
DEFINE_HOOK(&daNpcKasiMich_c::escape, KasiMichEscape);

static HookAction on_kasi_kyu_escape_pre(ModContext*, void* args, void*, void*) {
    return on_kasi_escape_pre<daNpcKasiKyu_c>(FollowerChoice::Kili, args);
}

static HookAction on_kasi_hana_escape_pre(ModContext*, void* args, void*, void*) {
    return on_kasi_escape_pre<daNpcKasiHana_c>(FollowerChoice::Hana, args);
}

static HookAction on_kasi_mich_escape_pre(ModContext*, void* args, void*, void*) {
    return on_kasi_escape_pre<daNpcKasiMich_c>(FollowerChoice::Misha, args);
}

// Respawns the selected follower NPC at the player's current position/room.
// Used whenever her actor has disappeared (a loading zone or room change
// freed her) so she can keep following in the new area.
static void respawn_follower_at_player() {
    fopAc_ac_c* player = dComIfGp_getPlayer(0);
    if (player == nullptr) {
        return;
    }

    const FollowerNpcTraits& traits = kFollowerTraits[static_cast<int>(g_selectedFollower)];

    ActorSpawnParams params = {
        .parameters = 0,
        .argument = 0,
        .room_num = fopAcM_GetRoomNo(player),
        .position = {player->current.pos.x, player->current.pos.y, player->current.pos.z},
        .angle = {player->current.angle.x, player->current.angle.y, player->current.angle.z},
        .scale = {1.0f, 1.0f, 1.0f},
    };

    ActorId newId = 0;
    if (svc_actor->create_actor_from_name(mod_ctx, traits.stageName, &params, &newId) == MOD_OK) {
        g_followerId = newId;
        mods::log::info("Respawned {} follower next to the player", follower_choice_name(g_selectedFollower));
    } else {
        mods::log::error("failed to respawn {} follower", follower_choice_name(g_selectedFollower));
    }
}

static void on_follower_choice_changed(
    ModContext*, ConfigVarHandle, const ConfigVarValue* value, const ConfigVarValue*, void*) {
    g_selectedFollower = parse_follower_choice(value->string_value);
    mods::log::info("Follower choice set to {}", follower_choice_name(g_selectedFollower));
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
    result = mods::hook::add_pre<KasiHanaMain>(on_kasi_hana_main_pre);
    if (result != MOD_OK) {
        mods::log::error("failed to install pre_hook on_kasi_hana_main_pre");
        return result;
    }
    result = mods::hook::add_pre<KasiMichMain>(on_kasi_mich_main_pre);
    if (result != MOD_OK) {
        mods::log::error("failed to install pre_hook on_kasi_mich_main_pre");
        return result;
    }

    result = mods::hook::add_pre<KasiKyuEscape>(on_kasi_kyu_escape_pre);
    if (result != MOD_OK) {
        mods::log::error("failed to install pre_hook on_kasi_kyu_escape_pre");
        return result;
    }
    result = mods::hook::add_pre<KasiHanaEscape>(on_kasi_hana_escape_pre);
    if (result != MOD_OK) {
        mods::log::error("failed to install pre_hook on_kasi_hana_escape_pre");
        return result;
    }
    result = mods::hook::add_pre<KasiMichEscape>(on_kasi_mich_escape_pre);
    if (result != MOD_OK) {
        mods::log::error("failed to install pre_hook on_kasi_mich_escape_pre");
        return result;
    }

    // "followerChoice" selects which STAR-tent NPC becomes the permanent
    // follower: "kili" (default), "hanna", or "misha". Edit this in the
    // mod's config.json (or pass --cvar) before talking to the group.
    ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
    desc.name = "followerChoice";
    desc.type = CONFIG_VAR_STRING;
    desc.default_string = "kili";
    ConfigVarHandle followerVar = 0;
    result = svc_config->register_var(mod_ctx, &desc, &followerVar);
    if (result != MOD_OK) {
        mods::log::error("failed to register followerChoice config var");
        return result;
    }

    char buffer[16] = {};
    size_t length = 0;
    if (svc_config->get_string(mod_ctx, followerVar, buffer, sizeof(buffer), &length) == MOD_OK) {
        g_selectedFollower = parse_follower_choice(buffer);
    }
    svc_config->subscribe(mod_ctx, followerVar, on_follower_choice_changed, nullptr, nullptr);

    mods::log::info("my_mod initialized (follower choice: {})", follower_choice_name(g_selectedFollower));
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    if (g_following) {
        const FollowerNpcTraits& traits = kFollowerTraits[static_cast<int>(g_selectedFollower)];
        fopAc_ac_c* existing = fopAcM_SearchByID(g_followerId);
        const bool stillAlive = existing != nullptr && fopAcM_GetName(existing) == traits.nameEnum;

        if (!stillAlive) {
            if (g_respawnCooldown > 0) {
                --g_respawnCooldown;
            } else {
                respawn_follower_at_player();
                g_respawnCooldown = kRespawnCooldownFrames;
            }
        } else {
            g_respawnCooldown = 0;
        }
    }

    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    return MOD_OK;
}
}
