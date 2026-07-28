#include "render_math.h"
#include "render_props.h"
#include "render_scene_state.h"
#include "render_text.h"

#include <cmath>
#include <render/render.h>

// Internal to the render module. The queue phase of a frame: every model that
// will be drawn is written into the per-frame draw-record buffers here, and
// nothing is recorded into a command buffer yet. Split from render_scene.cpp,
// which was one 600-line function and well over the project's file limit.

namespace render {

namespace {

/// A thrown grenade, in metres. The sim treats it as a point, so this is purely
/// how big it looks — and it is drawn a good deal larger than a real grenade,
/// because at true scale it is a handful of pixels nobody reacts to.
constexpr f32 GRENADE_RADIUS = 0.17f;

/// The fireball. Its radius is the sim's own 5m blast, so what you see is what
/// hurt you, and it lasts a third of a second.
constexpr f32 BLAST_FX_RADIUS = 5.0f;
constexpr u32 BLAST_FX_TICKS = 20;

/// How far the viewmodel swings while wallrunning, in metres across and down.
/// The gun is bolted to the view, so a wallrun would otherwise be the one big
/// movement the player's own hands sit out.
constexpr f32 WALLRUN_VM_SHIFT = 0.05f;

/// Speed at which a body is fully running rather than easing out of its idle.
/// Below the sim's flat-out run so that a bot at a walk is already committed to
/// a stride instead of sliding along in a half-blend.
constexpr f32 RUN_BLEND_SPEED = 5.0f;

/// Where a shot appears to leave a body, from the fire-time eye: right of
/// centre, below the eyeline, and out in front of the chest. Roughly where the
/// hands hold the weapon, which is what these have to agree with.
constexpr f32 MUZZLE_RIGHT = 0.20f;
constexpr f32 MUZZLE_DOWN = 0.22f;
constexpr f32 MUZZLE_FORWARD = 0.35f;


/// The reload. The weapon art is a single rigid mesh with no skeleton, so the
/// magazine cannot be animated on its own; the whole weapon is swung out of the
/// aim instead, which is what the motion reads as anyway. Real arm art replaces
/// this with a proper clip.
constexpr f32 RELOAD_DROP = 0.11f;   ///< Metres the weapon falls out of the aim.
constexpr f32 RELOAD_PULL = 0.06f;   ///< Metres it draws back toward the chest.
constexpr f32 RELOAD_ROLL = 0.85f;   ///< Radians it rolls over, showing the magazine well.
constexpr f32 RELOAD_PITCH = 0.30f;  ///< Radians the muzzle tips up.

/// How far out of the aim the weapon is, `phase` of the way through a reload.
/// It leaves fast, stays down while the magazine is swapped, and comes back
/// under control; a symmetric curve reads like the gun is bobbing rather than
/// being worked on.
[[nodiscard]] f32 reload_swing(f32 phase) {
    constexpr f32 OUT = 0.20f;   // down by here
    constexpr f32 BACK = 0.72f;  // starts coming up here
    if (phase < OUT) {
        const f32 t = phase / OUT;
        return t * t * (3.0f - 2.0f * t);
    }
    if (phase < BACK) {
        return 1.0f;
    }
    const f32 t = (phase - BACK) / (1.0f - BACK);
    const f32 eased = t * t * (3.0f - 2.0f * t);
    return 1.0f - eased;
}

void queue_beam(RenderModel& model, u32 slot, const core::Vec3& origin, const core::Vec3& span,
                const f32 tint[4]) {
    const f32 len = span.length();
    if (len < 0.1f) {
        return;
    }
    const core::Vec3 dir = span * (1.0f / len);
    const f32 pitch = std::asin(dir.y > 1.0f ? 1.0f : (dir.y < -1.0f ? -1.0f : dir.y));
    const f32 yaw = std::atan2(dir.x, -dir.z);
    const core::Quat q_yaw = core::Quat::from_axis_half(
        core::Vec3{0.0f, 1.0f, 0.0f}, std::sin(yaw * 0.5f), std::cos(yaw * 0.5f));
    const core::Quat q_pitch = core::Quat::from_axis_half(
        core::Vec3{1.0f, 0.0f, 0.0f}, std::sin(pitch * 0.5f), std::cos(pitch * 0.5f));
    model_queue_stretched(model, slot, origin, q_yaw * q_pitch, core::Vec3{0.02f, 0.02f, len},
                          tint);
}

}  // namespace


void Scene::queue_frame(const gpu::Frame& frame, const sim::World& curr, f32 alpha,
                        const FrameView& view, TrooperPose* out_poses) {
    const f32 aspect = view.aspect;

    // Cull pre-pass, before the rendering pass begins: queue this frame's draw
    // records, zero the survivor counters, and let compute build the indirect
    // command buffers from whatever the frustum keeps.
    model_begin(models_->gun);
    model_begin(models_->bot_gun);
    model_begin(models_->viewmodel);
    model_begin(models_->trooper.base);
    model_begin(models_->mech);
    model_begin(models_->grenade);
    model_begin(models_->blast);
    model_begin(models_->tracer);
    model_begin(models_->tracer_vm);
    model_begin(models_->hitmarker);
    model_begin(models_->overlay);
    // Grenades in flight, and the fireballs left by the ones that have gone off.
    for (const sim::Grenade& g : curr.grenades) {
        if (g.active != 0) {
            model_queue(models_->grenade, frame.slot, core::Vec3{g.x, g.y, g.z}, core::Quat{},
                        GRENADE_RADIUS);
        }
    }
    for (const Blast& b : blasts_) {
        if (!b.live) {
            continue;
        }
        const u32 age = curr.tick.raw - b.tick;
        if (age >= BLAST_FX_TICKS) {
            continue;
        }
        // Punches out fast and then eases, which is what an explosion looks
        // like; a fireball that grows at a constant rate reads as a balloon.
        const f32 t = static_cast<f32>(age) / static_cast<f32>(BLAST_FX_TICKS);
        const f32 grow = 1.0f - (1.0f - t) * (1.0f - t);
        model_queue(models_->blast, frame.slot, core::Vec3{b.x, b.y, b.z}, core::Quat{},
                    BLAST_FX_RADIUS * (0.25f + 0.75f * grow));
    }

    // Everyone else in the match, tinted by team.
    constexpr f32 TEAM_TINTS[2][4] = {{0.30f, 0.50f, 1.00f, 1.0f}, {1.00f, 0.42f, 0.22f, 1.0f}};

    // The chassis, unless the player is looking out of it. Neutral gunmetal
    // while dormant, team-tinted while a robot runs it.
    const sim::Mech& mech = curr.mech;
    if (mech.alive != 0 && mech.pilot != 0) {
        constexpr f32 NEUTRAL[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        const f32* mech_tint =
            mech.pilot == sim::NO_PILOT ? NEUTRAL : TEAM_TINTS[curr.chars[mech.pilot].team & 1];
        const core::Quat mech_rot = core::Quat::from_axis_half(
            core::Vec3{0.0f, 1.0f, 0.0f}, std::sin(mech.yaw * 0.5f), std::cos(mech.yaw * 0.5f));
        model_queue_tinted(models_->mech, frame.slot, core::Vec3{mech.x, mech.y, mech.z},
                           mech_rot, 1.0f, mech_tint);
    }
    // The character model poses out at 1.82m facing +Z; scale it into the 1.8m
    // hull and flip it to our yaw-zero-faces-minus-Z convention. Each bot gets
    // its own skinning slice: alive bots blend idle into run by speed, and a
    // fresh corpse plays the death clip once and freezes on its last frame.
    const f32 anim_time = static_cast<f32>(curr.tick.raw) * sim::SIM_DT + alpha * sim::SIM_DT;
    u32 highest_clip = TROOPER_RIG.clip_idle;
    for (u32 c = 0; c < 4; ++c) {
        highest_clip = TROOPER_RIG.clip_run[c] > highest_clip ? TROOPER_RIG.clip_run[c]
                                                              : highest_clip;
    }
    const bool rigged = models_->trooper.clip_count > highest_clip;
    TrooperPose* poses = out_poses;
    for (u32 i = 1; i < sim::MAX_PLAYERS; ++i) {
        const sim::Character& other = curr.chars[i];
        if (other.merged != 0) {
            continue;  // the body is inside the mech
        }
        TrooperPose& pose = poses[i];
        if (other.alive == 0) {
            // Corpses show for the first while of the 180-tick respawn wait.
            const f32 dead_for = (180.0f - static_cast<f32>(other.respawn_ticks)) * sim::SIM_DT;
            if (dead_for > 1.6f || !rigged) {
                continue;
            }
            const f32 death_end = models_->trooper.clips[TROOPER_RIG.clip_death].duration - 0.001f;
            pose = TrooperPose{TROOPER_RIG.clip_death, TROOPER_RIG.clip_death, 0.0f,
                               dead_for < death_end ? dead_for : death_end, true};
        } else {
            // Locomotion by the angle between where they are going and where
            // they are looking. A bot walking a route while tracking a target
            // is almost never doing both in the same direction, and playing the
            // forward run regardless is what makes them moonwalk.
            const f32 hspeed = std::sqrt(other.vx * other.vx + other.vz * other.vz);
            const f32 forward = other.vx * std::sin(other.yaw) - other.vz * std::cos(other.yaw);
            const f32 rightward = other.vx * std::cos(other.yaw) + other.vz * std::sin(other.yaw);
            // Zero velocity gives atan2(0, 0) = 0, which is the forward clip,
            // and at a standstill the idle blend has taken over anyway.
            f32 sector = std::atan2(rightward, forward) * (2.0f / 3.14159265f);
            if (sector < 0.0f) {
                sector += 4.0f;  // -2..2 turns into 0..4 clip slots
            }
            const u32 dir = static_cast<u32>(sector);
            const f32 between = sector - static_cast<f32>(dir);

            const f32 time = anim_time + static_cast<f32>(i) * 0.37f;
            if (hspeed < RUN_BLEND_SPEED) {
                // Standing to running, on the direction they are heading.
                pose = TrooperPose{TROOPER_RIG.clip_idle, TROOPER_RIG.clip_run[dir & 3],
                                   hspeed / RUN_BLEND_SPEED, time, true};
            } else {
                // At speed, between the two runs either side of the heading.
                pose = TrooperPose{TROOPER_RIG.clip_run[dir & 3],
                                   TROOPER_RIG.clip_run[(dir + 1) & 3], between, time, true};
            }
        }
        const f32 half = (other.yaw + TROOPER_RIG.yaw_offset) * 0.5f;
        const core::Quat yaw_rot = core::Quat::from_axis_half(core::Vec3{0.0f, 1.0f, 0.0f},
                                                              std::sin(half), std::cos(half));

        // Wallrunners bank into the wall, head first, and ease back out of it
        // when they leave. Banking about the world facing axis rather than the
        // model's own means the lean survives the yaw offset the rig needs.
        const f32 target_bank =
            other.state == sim::MoveState::Wallrun
                ? -wall_side(other.yaw, other.wall_nx, other.wall_nz) * WALLRUN_BANK
                : 0.0f;
        trooper_bank_[i] += (target_bank - trooper_bank_[i]) * LEAN_EASE;
        const core::Vec3 facing{std::sin(other.yaw), 0.0f, -std::cos(other.yaw)};
        const f32 bank_half = trooper_bank_[i] * 0.5f;
        const core::Quat rot =
            core::Quat::from_axis_half(facing, std::sin(bank_half), std::cos(bank_half)) * yaw_rot;
        if (rigged) {
            // Pose before queueing: the weapon hangs off a joint, so the joint
            // has to have moved before anything can be placed on it.
            skinned_model_animate(models_->trooper, i, pose.clip_a, pose.clip_b, pose.blend,
                                  pose.time, frame.slot);
            skinned_model_queue(models_->trooper, frame.slot, i,
                                core::Vec3{other.x, other.y, other.z}, rot, TROOPER_RIG.scale,
                                TEAM_TINTS[other.team & 1]);

            // The weapon takes its place from the hand and its aim from the
            // character. A hand rolling through the run cycle would otherwise
            // swing the barrel well off the shot the sim actually fired, and
            // the tracer leaves along the aim.
            if (other.alive != 0) {
                const core::Vec3 hand_local =
                    skinned_model_attach(models_->trooper, i).transform_point(core::Vec3{}) *
                    TROOPER_RIG.scale;
                const f32 gun_yaw = other.yaw * 0.5f;
                const f32 gun_pitch = other.pitch * 0.5f;
                const core::Quat gun_rot =
                    core::Quat::from_axis_half(core::Vec3{0.0f, 1.0f, 0.0f}, std::sin(gun_yaw),
                                               std::cos(gun_yaw)) *
                    core::Quat::from_axis_half(core::Vec3{1.0f, 0.0f, 0.0f}, std::sin(gun_pitch),
                                               std::cos(gun_pitch));
                const core::Vec3 hand =
                    core::Vec3{other.x, other.y, other.z} + rot.rotate(hand_local);
                model_queue(models_->bot_gun, frame.slot,
                            hand + gun_rot.rotate(core::Vec3{0.0f, 0.0f, -TROOPER_RIG.gun_forward}),
                            gun_rot, TROOPER_RIG.gun_scale);
            }
        } else {
            model_queue_tinted(models_->trooper.base, frame.slot,
                               core::Vec3{other.x, other.y, other.z}, rot, 1.0f,
                               TEAM_TINTS[other.team & 1]);
        }
    }

    // Tracers: a bright beam along each recent shot. Bots get world-space
    // beams from their eyes to the frozen impact. The player's beam is drawn
    // like the gun: camera space under the viewmodel projection, running from
    // the muzzle to the crosshair axis at the impact's distance, so it always
    // ends exactly where the crosshair points.
    for (u32 i = 0; i < sim::MAX_PLAYERS; ++i) {
        const sim::Character& shooter = curr.chars[i];
        if (shooter.alive == 0 || shooter.shot_age > 2) {
            continue;
        }
        const f32 scp = std::cos(shooter.shot_pitch);
        const core::Vec3 shot_dir{std::sin(shooter.shot_yaw) * scp, std::sin(shooter.shot_pitch),
                                  -std::cos(shooter.shot_yaw) * scp};
        const core::Vec3 impact = core::Vec3{shooter.shot_x, shooter.shot_y, shooter.shot_z} +
                                  shot_dir * shooter.last_shot_t;
        const f32 fade[4] = {1.0f - 0.3f * static_cast<f32>(shooter.shot_age),
                             1.0f - 0.3f * static_cast<f32>(shooter.shot_age), 1.0f, 1.0f};
        if (i == 0) {
            const core::Vec3 eye = view.eye;
            const core::Vec3 muzzle{0.17f, -0.14f, -0.57f};
            const core::Vec3 end{0.0f, 0.0f, -(impact - eye).length()};
            queue_beam(models_->tracer_vm, frame.slot, muzzle, end - muzzle, fade);
        } else {
            // Out of the weapon, not out of the eye. The sim traces from the
            // eye like any hitscan shooter, but a beam drawn from there leaves
            // a bot's face. Offset in the frozen fire-time frame so the beam
            // stays put for its two frames instead of dragging along behind a
            // bot who has since run on.
            const core::Vec3 eye{shooter.shot_x, shooter.shot_y, shooter.shot_z};
            const core::Vec3 right{std::cos(shooter.shot_yaw), 0.0f, std::sin(shooter.shot_yaw)};
            const core::Vec3 origin = eye + right * MUZZLE_RIGHT +
                                      core::Vec3{0.0f, -MUZZLE_DOWN, 0.0f} +
                                      shot_dir * MUZZLE_FORWARD;
            queue_beam(models_->tracer, frame.slot, origin, impact - origin, fade);
        }
    }

    // The first-person viewmodel lives in camera space and is drawn with its
    // own fixed-FOV projection, so it stays rigidly glued to the view no
    // matter what the world camera does (speed FOV, roll, interpolation). The
    // muzzle kicks back for a few frames after firing.
    const f32 kick =
        curr.player().shot_age < 4 ? (4.0f - static_cast<f32>(curr.player().shot_age)) * 0.012f
                                   : 0.0f;
    // Wallrunning swings the weapon toward the wall and drops it, the pilot's
    // half of the lean the camera is already doing.
    const f32 vm_half = cur_vm_lean_ * WALLRUN_BANK * 0.5f;
    const core::Quat lean_rot = core::Quat::from_axis_half(core::Vec3{0.0f, 0.0f, 1.0f},
                                                           std::sin(vm_half), std::cos(vm_half));
    // Reload: swing the weapon down and roll it over, so the hands are visibly
    // busy for exactly as long as the simulation says the magazine takes.
    const f32 swing = reload_swing(sim::reload_phase(curr.player()));
    const f32 roll_half = swing * RELOAD_ROLL * 0.5f;
    const f32 pitch_half = swing * RELOAD_PITCH * 0.5f;
    const core::Quat reload_roll = core::Quat::from_axis_half(
        core::Vec3{0.0f, 0.0f, 1.0f}, std::sin(roll_half), std::cos(roll_half));
    const core::Quat reload_pitch = core::Quat::from_axis_half(
        core::Vec3{1.0f, 0.0f, 0.0f}, std::sin(pitch_half), std::cos(pitch_half));
    const core::Quat vm_rot = lean_rot * reload_roll * reload_pitch;
    model_queue(models_->gun, frame.slot,
                core::Vec3{0.17f + cur_vm_lean_ * WALLRUN_VM_SHIFT,
                           -0.14f - (cur_vm_lean_ < 0.0f ? -cur_vm_lean_ : cur_vm_lean_) *
                                        WALLRUN_VM_SHIFT -
                               swing * RELOAD_DROP,
                           -0.45f + kick + swing * RELOAD_PULL},
                vm_rot, 0.6f);
    model_queue(models_->viewmodel, frame.slot, core::Vec3{}, core::Quat{}, 1.0f);
    if (curr.player().shot_hit != 0 && curr.player().shot_age < 8) {
        model_queue(models_->hitmarker, frame.slot, core::Vec3{0.0f, 0.0f, -1.2f}, core::Quat{},
                    1.0f);
    }

    // Screen feedback: a red flash when hurt, a dark red shroud while dead,
    // and the match banner tint during the end phase.
    if (curr.player().alive != 0 && curr.player().hurt_age < 12) {
        const f32 a = 0.38f * (1.0f - static_cast<f32>(curr.player().hurt_age) / 12.0f);
        const f32 tint[4] = {0.9f, 0.08f, 0.08f, a};
        model_queue_tinted(models_->overlay, frame.slot, core::Vec3{}, core::Quat{}, 1.0f, tint);
    }
    if (curr.player().alive == 0) {
        const f32 tint[4] = {0.35f, 0.02f, 0.02f, 0.5f};
        model_queue_tinted(models_->overlay, frame.slot, core::Vec3{}, core::Quat{}, 1.0f, tint);
    }
    if (curr.winner != 0) {
        const bool won = (curr.winner - 1) == curr.player().team;
        const f32 tint[4] = {won ? 0.1f : 0.6f, won ? 0.5f : 0.08f, 0.12f, 0.28f};
        model_queue_tinted(models_->overlay, frame.slot, core::Vec3{}, core::Quat{}, 1.0f, tint);
    }
    hud_queue(models_->overlay, models_->font, frame.slot, curr, aspect);
}

}  // namespace render
