#include "mods/service.hpp"
#include "mods/svc/actor.h"
#include "mods/svc/config.h"
#include "mods/svc/hook.hpp"
#include "mods/svc/log.hpp"
#include "mods/svc/stage.h"
#include "mods/svc/ui.h"

// Game includes
#include "d/actor/d_a_npc.h"
#include "d/actor/d_a_npc_kasi_hana.h"
#include "d/actor/d_a_npc_kasi_kyu.h"
#include "d/actor/d_a_npc_kasi_mich.h"
#include "d/d_camera.h"
#include "d/d_com_inf_game.h"
#include "d/d_item_data.h"
#include "d/d_stage.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_name.h"

#include <cstring>

DEFINE_MOD();

IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(ActorService, svc_actor);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(StageService, svc_stage);
IMPORT_SERVICE(UiService, svc_ui);

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
// permanent follower, and take over her per-tick behavior instead.
//
// To keep following across loading zones/room transitions, we watch for a
// pending stage change (dComIfGp_isEnableNextStage(), set the instant the
// player triggers a loading zone or door - the same path full stage changes
// and plain room-to-room moves both go through) and use StageService to
// pre-register her as a normal static actor entry in the *destination*
// stage/room, before it ever loads. That means the engine's own room-load
// pipeline creates her exactly like any authored NPC, respecting its usual
// actor/heap/resource budget, instead of racing that pipeline with an ad-hoc
// runtime creation call - the latter was corrupting engine state badly
// enough to unload other NPCs/the player and crash. A hooked main() adopts
// whichever freshly-created instance shows up next and snaps her next to the
// player instantly. A runtime create_actor_from_name respawn is kept only as
// a last-resort fallback, for the rare case a transition isn't caught this
// way.
// ---------------------------------------------------------------------------

enum class FollowerChoice : int { Kili = 0, Hana = 1, Misha = 2 };

// Which NPC's escape() hook is allowed to latch "following" mode. Only one of
// the three NPCs can be the active follower; the other two flee normally.
static FollowerChoice g_selectedFollower = FollowerChoice::Kili;

// 0 means "no live instance currently tracked". Cleared whenever the tracked
// actor disappears, and (re)latched either by escape() the first time she
// starts following, or by main() adopting whichever matching NPC instance
// the engine's own stage/room load creates next.
static bool g_following = false;
static ActorId g_followerId = 0;
static ConfigVarHandle g_followerChoiceVar = 0;

// Her creation `parameters`, captured from the live actor the moment she
// starts following and reused for every future StageService::add_actor
// registration, so a freshly (re)created instance keeps the same identity/
// behavior flags as her original authored placement.
static u32 g_followerSpawnParam = 0;

// Tracks the single StageService edit (if any) currently registering the
// follower into an upcoming destination stage/room. add_actor edits persist
// for the whole life of the mod, so we must retract the previous one before
// registering a new destination, or she'd end up duplicated/stuck in stale
// rooms.
static StageActorHandle g_stageEditHandle = 0;
static char g_stageEditStage[8] = {};
static s16 g_stageEditRoom = -1;
// Which NPC the current g_stageEditHandle edit was registered for. Needed
// alongside the stage/room above because a bare (stage, room) match isn't
// enough to know the existing edit is still correct - the player could
// transition into the very same destination twice with a different NPC
// selected each time, which must still trigger re-registration.
static FollowerChoice g_stageEditChoice = FollowerChoice::Kili;

// (Stage, room) she was standing in the moment she first started following -
// her natively-authored home spot outside the STAR tent, shared by all three
// NPCs. Used to avoid pre-registering a duplicate add_actor entry there (the
// room's own actor-placement data already spawns her) and to decide how to
// hand her back cleanly when the player switches which NPC is following.
static char g_followerHomeStage[8] = {};
static s16 g_followerHomeRoom = -1;

// How long (in ticks) the follower has been missing with no live instance
// adopted yet. The normal path (StageService pre-registration + main()
// adoption) should resolve this almost immediately once the destination room
// loads; the runtime create_actor_from_name fallback below only kicks in
// after a long delay, as a last resort for transitions we didn't catch (e.g.
// a teleport that never sets isEnableNextStage()).
static int g_missingFrames = 0;
constexpr int kFallbackRespawnDelayFrames = 150;  // ~2.5s at 60fps.

// Much shorter fallback delay used when she goes missing with no stage/room
// transition in flight at all (dComIfGp_isEnableNextStage() false) - i.e.
// she wasn't caught mid-load, she was silently culled by the engine's own
// room-visibility system for lagging too far behind the player in open,
// multi-room areas with no loading zone to catch (see mod_update). There's
// no destination-room load to wait out here, so there's no reason to sit on
// the long delay above - every extra frame is a frame the lost-follower
// teleport rescue in follow_tick_impl can't run either, since that rescue
// needs a still-living instance to move.
constexpr int kCulledRespawnDelayFrames = 10;  // ~0.17s at 60fps.

// Frames to wait between fallback respawn attempts once the follower is
// missing, so a persistently failing spawn (e.g. missing resources) doesn't
// spam the actor service every tick.
constexpr int kRespawnCooldownFrames = 30;
static int g_respawnCooldown = 0;

// The engine's stage-load sequence updates the start-stage name/room very
// early (well before the destination room's actor heap is actually ready),
// and mod_update() keeps ticking across that whole multi-frame sequence.
// Spawning into a room that isn't fully set up yet was corrupting engine
// state badly enough to unload other NPCs/the player and eventually crash.
// To avoid racing that setup, the (rare) fallback path below requires the
// player's (stage, room) pair to stay unchanged for kStageSettleFrames
// consecutive ticks before attempting a respawn, which only happens once
// gameplay has actually resumed.
constexpr int kStageSettleFrames = 30;
static char g_lastStageName[8] = {};
static int8_t g_lastRoomNo = -1;
static int g_stageStableFrames = 0;

// Distance at which she's close enough to stop and idle - this matches the
// distance she's always naturally settled to once the player also stops
// moving.
constexpr f32 kIdleDistance = 220.0f;

// Target "following" gap: a bit further than the idle distance (per
// request, +200 units) so that once she starts moving to catch up she keeps
// going until she's back within this range, instead of closing all the way
// up to the player's heels. Combined with the hysteresis below (she only
// *starts* moving again once past this distance, and only *stops* once back
// within kIdleDistance), this keeps her trailing distance steady/consistent
// rather than jerky stop-and-go right at a single exact boundary.
constexpr f32 kFollowDistance = kIdleDistance + 200.0f;  // 420.0f

// Beyond this distance (e.g. she just got left behind after a loading zone,
// or the player sprints off) she breaks into a full run to catch back up
// quickly instead of ambling along at walk speed indefinitely.
constexpr f32 kRunDistance = kFollowDistance + 400.0f;  // 820.0f

constexpr f32 kWalkSpeed = 12.0f;
// Her run animation's playback rate is driven entirely by the engine's
// internal, protected per-actor animation state (mAnm_p), which these NPC
// classes don't expose any public setter/getter for - unlike a few other
// actor types in the engine (e.g. the fairy/mirror NPCs), so we have no way
// to speed the animation itself up to visually match a faster run speed
// without touching Dusklight's own source. 15.0f mirrors her native
// escape()'s sprint speed (her fastest officially-authored movement for
// this same run animation), keeping her movement visually in sync with the
// animation instead of "ice-skating" at a higher raw speed.
constexpr f32 kRunSpeed = 15.0f;
constexpr s16 kTurnRate = 0x800;

// See the manual ground-snap in follow_tick_impl: how far ahead of her (in
// her direction of travel) and how high above her the ground-height probe
// point is sampled, and the maximum she's allowed to rise in a single tick.
constexpr f32 kGroundProbeAheadDist = 40.0f;
constexpr f32 kGroundProbeHeight = 250.0f;
constexpr f32 kMaxStepUpPerTick = 80.0f;

// Whether the follower is currently walking/running to close the gap, vs.
// idling at kIdleDistance. See the hysteresis comment on kFollowDistance.
static bool g_followerMoving = false;

// Last-resort rescue for when she gets properly stuck/left behind (e.g.
// blocked by geometry her pathing can't get around) rather than just
// trailing a normal following distance behind: if she's more than
// kLostDistance (full 3D, not just XZ, so being stuck below/above the
// player on a different level also counts) from the player AND not visible
// on screen, continuously, for kLostDelayFrames, teleport her to just
// behind the camera - hidden from the player's view - so she reappears
// near them instead of staying stranded.
constexpr f32 kLostDistance = 2000.0f;
constexpr int kLostDelayFrames = 180;  // ~3s at 60fps.
constexpr f32 kTeleportBehindCameraDist = 200.0f;

// Rough humanoid-sized probe box (local to her own position/orientation),
// used with daNpcT_chkActorInScreen to test whether any part of her would
// currently be visible on screen.
constexpr f32 kScreenCheckHalfWidth = 30.0f;
constexpr f32 kScreenCheckHeight = 180.0f;

static int g_lostOffscreenFrames = 0;

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

// UI_CONTROL_DROPDOWN options for the "Follower" toggle, in FollowerChoice order.
static const char* const kFollowerChoiceOptions[3] = {"Kili", "Hanna", "Misha"};

static FollowerChoice parse_follower_choice(int64_t value) {
    if (value == static_cast<int64_t>(FollowerChoice::Hana)) {
        return FollowerChoice::Hana;
    }
    if (value == static_cast<int64_t>(FollowerChoice::Misha)) {
        return FollowerChoice::Misha;
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

// Computes a position just behind the camera - hidden from the player's
// view - used both to teleport an already-live, stuck follower back near
// the player (see try_rescue_lost_follower) and to place her when a runtime
// respawn is needed (see respawn_follower_near_player), so she never
// visibly "pops in" right in front of them either way. Falls back to the
// player's own position if the camera isn't available.
static cXyz compute_hidden_teleport_pos(fopAc_ac_c* player, f32 fallbackY) {
    dCamera_c* camera = dCam_getBody();
    if (camera == nullptr) {
        return player->current.pos;
    }

    // "Behind the camera" = further along the eye-to-center direction, past
    // the eye rather than toward the subject. Flattened to the XZ plane so
    // she lands beside the camera's position rather than floating at its
    // (possibly elevated) pitch - the ground-snap probe in follow_tick_impl
    // corrects her height from there on her very next tick, once alive.
    cXyz eye = camera->Eye();
    cXyz backDir = eye - camera->Center();
    backDir.y = 0.0f;
    const f32 backDirLen = backDir.absXZ();
    if (backDirLen > 1.0f) {
        backDir.x /= backDirLen;
        backDir.z /= backDirLen;
    } else {
        backDir.set(0.0f, 0.0f, 1.0f);
    }

    cXyz pos = eye + backDir * kTeleportBehindCameraDist;
    pos.y = fallbackY;
    return pos;
}

// If she's drifted far from the player (full 3D distance, so being stuck
// above/below them on a different level counts too) and isn't currently
// visible on screen, continuously, for long enough, she's probably stuck
// behind geometry her pathing couldn't resolve on its own. Teleport her to
// just behind the camera - still hidden from view - so she reappears near
// the player instead of staying stranded. Returns true if she was just
// teleported this tick.
template <class T>
static bool try_rescue_lost_follower(
    T* self, fopAc_ac_c* player, FollowerChoice choice) {
    const f32 dist3d = fopAcM_searchActorDistance(self, player);
    const bool isLost = dist3d > kLostDistance;
    const bool isOnScreen = daNpcT_chkActorInScreen(
        self, -kScreenCheckHalfWidth, 0.0f, -kScreenCheckHalfWidth, kScreenCheckHalfWidth,
        kScreenCheckHeight, kScreenCheckHalfWidth, 0.0f, 0);

    if (!isLost || isOnScreen) {
        g_lostOffscreenFrames = 0;
        return false;
    }

    if (++g_lostOffscreenFrames < kLostDelayFrames) {
        return false;
    }
    g_lostOffscreenFrames = 0;

    dCamera_c* camera = dCam_getBody();
    if (camera == nullptr) {
        return false;
    }

    const cXyz teleportPos = compute_hidden_teleport_pos(player, self->current.pos.y);
    self->current.pos = teleportPos;
    self->old.pos = teleportPos;
    mods::log::info("{} was lost and teleported back near the player", follower_choice_name(choice));
    return true;
}

template <class T>
static void follow_tick_impl(T* self, const FollowMotionSet& motions, FollowerChoice choice) {
    fopAc_ac_c* player = dComIfGp_getPlayer(0);
    if (player == nullptr) {
        return;
    }

    try_rescue_lost_follower(self, player, choice);

    self->_turn_pos(player->current.pos, kTurnRate);

    // daNpcF_c::execute()'s own per-frame ground-collision pass (which runs
    // right after main() returns) doesn't reliably keep her glued to terrain
    // across larger single-frame elevation changes - e.g. jagged/stepped
    // stairs rather than a smooth ramp. Independently re-query the ground
    // height every tick, the same way other moving actors (e.g. the
    // wall-climber in d_a_obj_cwall.cpp) do it, and snap to it ourselves
    // rather than relying solely on that pass. The query point is sampled
    // slightly ahead of her along her direction of travel (not directly
    // beneath her already-lagging current position) and from well above her,
    // so the downward sweep can find the tread at the top of a tall/jagged
    // riser when climbing stairs, not just a continuous slope.
    cXyz toPlayerXZ = player->current.pos - self->current.pos;
    toPlayerXZ.y = 0.0f;
    const f32 toPlayerDist = toPlayerXZ.absXZ();

    cXyz groundQuery = self->current.pos;
    if (toPlayerDist > 1.0f) {
        groundQuery.x += (toPlayerXZ.x / toPlayerDist) * kGroundProbeAheadDist;
        groundQuery.z += (toPlayerXZ.z / toPlayerDist) * kGroundProbeAheadDist;
    }
    groundQuery.y += kGroundProbeHeight;

    if (fopAcM_gc_c::gndCheck(&groundQuery)) {
        // Climbing is clamped to a reasonable per-tick step height so she
        // doesn't snap straight up a wall/cliff she isn't meant to scale -
        // she'll instead climb a tall staircase over a couple of ticks,
        // which still reads as smooth at 60fps. Descending is left
        // unclamped, since drops already track correctly.
        const f32 groundY = fopAcM_gc_c::getGroundY();
        const f32 maxRise = self->current.pos.y + kMaxStepUpPerTick;
        self->current.pos.y = groundY < maxRise ? groundY : maxRise;
    }

    const f32 dist = fopAcM_searchActorDistanceXZ(self, player);

    // Hysteresis: only start moving once she's past the follow distance, and
    // only stop again once she's back within the idle distance. A single
    // shared threshold would make her flicker between idle/walk right at the
    // boundary; this instead keeps her trailing distance steady.
    if (!g_followerMoving && dist > kFollowDistance) {
        g_followerMoving = true;
    } else if (g_followerMoving && dist <= kIdleDistance) {
        g_followerMoving = false;
    }

    if (!g_followerMoving) {
        self->setMotion(motions.idle, -1.0f, 0);
        fopAcM_SetSpeedF(self, 0.0f);
    } else if (dist > kRunDistance) {
        self->setMotion(motions.run, -1.0f, 0);
        fopAcM_SetSpeedF(self, kRunSpeed);
    } else {
        // Scale smoothly between walk and run speed across the remaining
        // catch-up range so closing the gap feels proportional rather than
        // an abrupt speed snap.
        f32 t = (dist - kIdleDistance) / (kRunDistance - kIdleDistance);
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        const f32 speed = kWalkSpeed + (kRunSpeed - kWalkSpeed) * t;
        self->setMotion(speed > 0.5f * (kWalkSpeed + kRunSpeed) ? motions.run : motions.walk, -1.0f, 0);
        fopAcM_SetSpeedF(self, speed);
    }

    self->playMotion();
}

// Takes over an NPC's entire per-tick behavior once she's the active,
// following NPC. daNpcF_c::execute() only performs position
// integration/ground collision when main() returns TRUE, so we must force
// that even though we skip the original body.
//
// When no live instance is currently tracked (g_followerId == 0 - she was
// just (re)created by the normal room/stage actor-list pipeline after a
// StageService pre-registration, or this is her very first tick ever),
// whichever matching NPC's main() runs next is adopted as the follower and
// snapped directly to the player's position/angle instead of gradually
// catching up, satisfying "spawn at the same point as the player".
template <class T>
static HookAction on_kasi_main_pre(
    FollowerChoice choice, void* args, void* retval, const FollowMotionSet& motions) {
    if (!g_following || g_selectedFollower != choice) {
        return HOOK_CONTINUE;
    }

    T* self = mods::arg<T*>(args, 0);
    const ActorId selfId = fopAcM_GetID(self);

    if (g_followerId == 0) {
        g_followerId = selfId;
        g_missingFrames = 0;
        g_respawnCooldown = 0;
        g_followerMoving = false;  // She's teleported right onto the player below - start idle.

        fopAc_ac_c* player = dComIfGp_getPlayer(0);
        if (player != nullptr) {
            self->current.pos = player->current.pos;
            self->current.angle = player->current.angle;
            self->current.roomNo = player->current.roomNo;
            self->old.pos = player->current.pos;
            self->old.angle = player->current.angle;
        }
        mods::log::info("{} rejoined the player", follower_choice_name(choice));
    } else if (selfId != g_followerId) {
        return HOOK_CONTINUE;
    }

    follow_tick_impl(self, motions, choice);
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
        g_followerSpawnParam = fopAcM_GetParam(self);
        g_missingFrames = 0;
        g_respawnCooldown = 0;

        // This is necessarily her native home spot outside the STAR tent -
        // the post-STAR-game conversation that leads here only ever happens
        // there. Remembered so later stage registrations can recognize this
        // exact (stage, room) as "she already spawns here natively" and skip
        // adding a duplicate.
        std::strncpy(g_followerHomeStage, dComIfGp_getStartStageName(), sizeof(g_followerHomeStage) - 1);
        g_followerHomeRoom = fopAcM_GetRoomNo(self);

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

// Pre-registers the selected follower NPC into the stage/room the player is
// about to transition into, via StageService::add_actor, so the engine's own
// room-load pipeline creates her exactly the same way it creates any
// authored static NPC in that room - respecting its normal actor/heap/
// resource budget, instead of racing it with an ad-hoc runtime creation
// call. Retracts any previous registration first: add_actor edits persist
// for the whole life of the mod, so leaving an old one in place would leave
// a stale duplicate copy of her behind in the room she's leaving.
static void update_follower_stage_registration(const char* stage, s16 room) {
    if (stage == nullptr || stage[0] == '\0' || room < 0) {
        return;
    }

    const bool isHomeDestination = g_followerHomeRoom >= 0 && room == g_followerHomeRoom &&
        std::strncmp(stage, g_followerHomeStage, sizeof(g_followerHomeStage)) == 0;
    if (isHomeDestination) {
        // She's natively authored in this exact room (it's her and the
        // other two NPCs' shared home spot outside the STAR tent) - the
        // room's own actor-placement data already spawns her there, so
        // adding another copy here would duplicate her. Retract any
        // previous registration (e.g. she's returning home after following
        // elsewhere) and let the room's native spawn be adopted instead, by
        // on_kasi_main_pre, exactly like any other destination.
        if (g_stageEditHandle != 0) {
            svc_stage->remove_actor_edit(mod_ctx, g_stageEditHandle);
            g_stageEditHandle = 0;
            g_stageEditStage[0] = '\0';
            g_stageEditRoom = -1;
        }
        return;
    }

    if (g_stageEditHandle != 0 && room == g_stageEditRoom && g_stageEditChoice == g_selectedFollower &&
        std::strncmp(stage, g_stageEditStage, sizeof(g_stageEditStage)) == 0) {
        return;  // Already registered for this exact destination and NPC.
    }

    if (g_stageEditHandle != 0) {
        svc_stage->remove_actor_edit(mod_ctx, g_stageEditHandle);
        g_stageEditHandle = 0;
    }

    const FollowerNpcTraits& traits = kFollowerTraits[static_cast<int>(g_selectedFollower)];

    stage_actor_data_class record{};
    std::memset(record.name, 0, sizeof(record.name));
    std::strncpy(record.name, traits.stageName, sizeof(record.name) - 1);
    record.base.parameters = g_followerSpawnParam;
    record.base.position = cXyz{0.0f, 0.0f, 0.0f};  // Corrected on her first main() tick.
    record.base.angle = csXyz{0, 0, 0};
    record.base.setID = 0xFFFF;  // No switch association (matches the engine's own convention).

    StageActorHandle handle = 0;
    const ModResult result = svc_stage->add_actor(
        mod_ctx, stage, static_cast<uint8_t>(room), -1, &record, sizeof(record), &handle);
    if (result == MOD_OK) {
        g_stageEditHandle = handle;
        std::strncpy(g_stageEditStage, stage, sizeof(g_stageEditStage) - 1);
        g_stageEditRoom = room;
        g_stageEditChoice = g_selectedFollower;
    } else {
        mods::log::error(
            "failed to pre-register {} into stage {} room {}", follower_choice_name(g_selectedFollower), stage,
            room);
    }
}

// Fallback: respawns the selected follower NPC near the player (hidden just
// behind the camera, same as the live lost-follower rescue, so she doesn't
// visibly pop in right in front of them) via a direct runtime actor creation
// call. Used both as a true last resort when she's been missing for a long
// time with no StageService-registered instance adopted (e.g. a teleport
// that never set isEnableNextStage() - the long delay in that case gives
// the normal/safe path plenty of time to resolve itself first), and as a
// much quicker recovery when she's missing with no stage/room transition in
// flight at all - the engine's own room-visibility system silently culling
// her for lagging too far behind in an open, multi-room area, which the
// live, on-instance teleport rescue in follow_tick_impl can never catch on
// its own (it needs a still-living instance to move). Direct runtime
// creation is what previously corrupted engine state badly enough to unload
// other NPCs/the player and crash when used during an unsettled stage load,
// so mod_update() only ever calls this once the player's (stage, room) has
// been stable for kStageSettleFrames, regardless of which of the two cases
// above triggered it.
static void respawn_follower_near_player() {
    fopAc_ac_c* player = dComIfGp_getPlayer(0);
    if (player == nullptr) {
        return;
    }

    const FollowerNpcTraits& traits = kFollowerTraits[static_cast<int>(g_selectedFollower)];
    const cXyz spawnPos = compute_hidden_teleport_pos(player, player->current.pos.y);

    ActorSpawnParams params = {
        .parameters = g_followerSpawnParam,
        .argument = 0,
        .room_num = fopAcM_GetRoomNo(player),
        .position = {spawnPos.x, spawnPos.y, spawnPos.z},
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

// Cleanly hands the previously-selected follower NPC back to the game when
// the player picks a different one, instead of leaving her instance
// orphaned wherever she physically happened to be - running unmanaged
// native AI - when main()'s choice check (g_selectedFollower != choice)
// starts failing for her on the very next tick.
//
// If she's currently standing in her native home room (the common case:
// following hasn't taken her anywhere yet, or she's just returned there),
// reset() is safe and ideal - it snaps her back to her authored position
// and idle state in place, since home.pos is only meaningful within her
// home room's own coordinate space. Otherwise she doesn't belong in
// whatever (foreign) room she's currently in at all - that instance only
// exists because she was following - so it's deleted outright; her
// authored instance will spawn fresh, as normal, next time her home room is
// loaded.
static void release_old_follower(ActorId actorId) {
    if (actorId == 0) {
        return;
    }
    fopAc_ac_c* actor = fopAcM_SearchByID(actorId);
    if (actor == nullptr) {
        return;
    }

    const bool isHome = g_followerHomeRoom >= 0 && fopAcM_GetRoomNo(actor) == g_followerHomeRoom &&
        std::strncmp(dComIfGp_getStartStageName(), g_followerHomeStage, sizeof(g_followerHomeStage)) == 0;

    if (isHome) {
        const s16 name = fopAcM_GetName(actor);
        if (name == fpcNm_NPC_KASIKYU_e) {
            static_cast<daNpcKasiKyu_c*>(actor)->reset();
        } else if (name == fpcNm_NPC_KASIHANA_e) {
            static_cast<daNpcKasiHana_c*>(actor)->reset();
        } else if (name == fpcNm_NPC_KASIMICH_e) {
            static_cast<daNpcKasiMich_c*>(actor)->reset();
        }
    } else {
        svc_actor->delete_actor(mod_ctx, actorId);
    }
}

static void on_follower_choice_changed(
    ModContext*, ConfigVarHandle, const ConfigVarValue* value, const ConfigVarValue*, void*) {
    const FollowerChoice newChoice = parse_follower_choice(value->int_value);
    if (g_following && newChoice != g_selectedFollower) {
        release_old_follower(g_followerId);
        // Safe to clear unconditionally: on_kasi_main_pre's g_followerId==0
        // adoption check is independent of this missing-frames/fallback
        // bookkeeping, so if the newly chosen NPC is already loaded nearby
        // she's adopted instantly on her very next main() tick; otherwise
        // the normal StageService pre-registration/fallback path takes over
        // exactly as it would for any other "missing follower" case.
        g_followerId = 0;
        g_missingFrames = 0;
        g_respawnCooldown = 0;
        g_followerMoving = false;
        g_lostOffscreenFrames = 0;
    }
    g_selectedFollower = newChoice;
    mods::log::info("Follower choice set to {}", follower_choice_name(g_selectedFollower));
}

// Builds the mod's panel in the host's Mods window: a single dropdown to pick
// which STAR-tent NPC follows the player. Bound directly to the
// "followerChoice" config var so the UI, config.json, and --cvar overrides
// all stay in sync automatically.
//
// UI_CONTROL_SELECT needs a help pane (mod window tabs only) and fails with
// MOD_UNSUPPORTED on a plain Mods-window panel, so UI_CONTROL_DROPDOWN is
// used instead; it binds to the same int config var/option-index scheme and
// has no help-pane requirement.
static ModResult build_mods_panel(ModContext*, UiElementHandle panel, void*, ModError*) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_DROPDOWN;
    control.label = "Follower";
    control.tooltip = "Which STAR-tent NPC follows the player after the group is talked to.";
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = g_followerChoiceVar;
    control.options = kFollowerChoiceOptions;
    control.option_count = 3;
    return svc_ui->pane_add_control(mod_ctx, panel, &control, nullptr);
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
    // follower: Kili (default, 0), Hanna (1), or Misha (2). Stored as an int
    // so it can be bound directly to a UI_CONTROL_SELECT dropdown below.
    ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
    desc.name = "followerChoice";
    desc.type = CONFIG_VAR_INT;
    desc.default_int = static_cast<int64_t>(FollowerChoice::Kili);
    result = svc_config->register_var(mod_ctx, &desc, &g_followerChoiceVar);
    if (result != MOD_OK) {
        mods::log::error("failed to register followerChoice config var");
        return result;
    }

    int64_t value = 0;
    if (svc_config->get_int(mod_ctx, g_followerChoiceVar, &value) == MOD_OK) {
        g_selectedFollower = parse_follower_choice(value);
    }
    svc_config->subscribe(mod_ctx, g_followerChoiceVar, on_follower_choice_changed, nullptr, nullptr);

    // Surfaces the "followerChoice" var as a dropdown in the mod's tab of the
    // host Mods window, since registering a config var alone doesn't produce
    // any visible UI control.
    UiModsPanelDesc panelDesc = UI_MODS_PANEL_DESC_INIT;
    panelDesc.build = build_mods_panel;
    result = svc_ui->register_mods_panel(mod_ctx, &panelDesc);
    if (result != MOD_OK) {
        mods::log::error("failed to register mods panel");
        return result;
    }

    mods::log::info("my_mod initialized (follower choice: {})", follower_choice_name(g_selectedFollower));
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    fopAc_ac_c* player = dComIfGp_getPlayer(0);

    // Track how long the player's current (stage, room) pair has been
    // stable. Only used to withhold the rare runtime fallback respawn below
    // until a stage/room transition has actually finished settling (see
    // kStageSettleFrames above).
    if (player != nullptr) {
        const char* stageName = dComIfGp_getStartStageName();
        const int8_t roomNo = fopAcM_GetRoomNo(player);
        if (std::strncmp(stageName, g_lastStageName, sizeof(g_lastStageName)) != 0 ||
            roomNo != g_lastRoomNo)
        {
            std::strncpy(g_lastStageName, stageName, sizeof(g_lastStageName) - 1);
            g_lastRoomNo = roomNo;
            g_stageStableFrames = 0;
        } else if (g_stageStableFrames < kStageSettleFrames) {
            ++g_stageStableFrames;
        }
    } else {
        g_stageStableFrames = 0;
    }

    if (g_following) {
        // The instant the player triggers any stage/room transition (a full
        // loading zone or a plain intra-stage door/room change - both go
        // through the same pending "next stage" request), pre-register the
        // follower into that exact destination before it loads.
        const bool pendingTransition = dComIfGp_isEnableNextStage();
        if (pendingTransition) {
            update_follower_stage_registration(
                dComIfGp_getNextStageName(), dComIfGp_getNextStageRoomNo());
        }

        const FollowerNpcTraits& traits = kFollowerTraits[static_cast<int>(g_selectedFollower)];
        fopAc_ac_c* existing = g_followerId != 0 ? fopAcM_SearchByID(g_followerId) : nullptr;
        const bool stillAlive = existing != nullptr && fopAcM_GetName(existing) == traits.nameEnum;

        if (!stillAlive) {
            // Clear the stale ID so on_kasi_main_pre can freely adopt the
            // next matching instance the engine's own stage/room load
            // creates (the normal, safe path).
            g_followerId = 0;
            ++g_missingFrames;

            // With an actual stage/room transition in flight, give the
            // StageService-registered destination room plenty of time to
            // load and adopt her normally before resorting to a direct
            // runtime spawn. Outside of a transition, she can only have
            // disappeared because the engine's own room-visibility system
            // culled her for lagging too far behind (there's no destination
            // load to wait out), so recover much sooner - every extra frame
            // here is a frame the live teleport rescue in follow_tick_impl
            // can't help either, since it needs a still-living instance.
            const int respawnDelay = pendingTransition ? kFallbackRespawnDelayFrames : kCulledRespawnDelayFrames;
            const bool fallbackDue = g_missingFrames >= respawnDelay;
            const bool safeToRespawn = player != nullptr && g_stageStableFrames >= kStageSettleFrames;
            if (!fallbackDue || !safeToRespawn) {
                // Not yet due, or the stage/room hasn't settled long enough
                // for a direct runtime spawn to be safe - keep waiting.
            } else if (g_respawnCooldown > 0) {
                --g_respawnCooldown;
            } else {
                respawn_follower_near_player();
                g_respawnCooldown = kRespawnCooldownFrames;
            }
        } else {
            g_missingFrames = 0;
            g_respawnCooldown = 0;
        }
    }

    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    return MOD_OK;
}
}
