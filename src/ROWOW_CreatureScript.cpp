//  Author: Nathan Handley (nathanhandley@protonmail.com)
//  Copyright (c) 2026 Nathan Handley
//
//  This program is free software; you can redistribute it and/or modify it
//  under the terms of the GNU Affero General Public License as published by the
//  Free Software Foundation; either version 3 of the License, or (at your
//  option) any later version.
//
//  This program is distributed in the hope that it will be useful, but WITHOUT
//  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
//  FITNESS FOR A PARTICULAR PURPOSE.See the GNU Affero General Public License for
//  more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "Creature.h"
#include "EventMap.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "MoveSplineInit.h"
#include "ScriptMgr.h"
#include "ScriptedCreature.h"

#include "ROWOW.h"

#include <cmath>

using namespace std;

// Ragnarok Online creatures always stand on, and move along, the ground.  The core's own movement (random movement, chasing, going
// home) runs in straight lines between points that are far apart, which floats or sinks a creature over RO's uneven terrain, so this
// AI does that movement itself with paths of small ground-snapped steps, and keeps idle creatures snapped to the ground
class ROWOW_CreatureScript : public CreatureScript
{
public:
    ROWOW_CreatureScript() : CreatureScript("ROWOW_CreatureScript") {}

    struct ROWOW_CreatureScriptAI : public ScriptedAI
    {
        ROWOW_CreatureScriptAI(Creature* creature) : ScriptedAI(creature) {}

        enum Events
        {
            EVENT_RANDOM_WALK = 1,
            EVENT_GROUND_SNAP_CHECK = 2
        };
        EventMap events;

        bool IsMovingOnContouredPath = false;
        bool HasChaseGoal = false;
        float ChaseGoalX = 0;
        float ChaseGoalY = 0;
        uint32 ChaseRepathTimer = 0;
        uint32 OutOfReachTimeInMS = 0; // How long the victim has been out of reach (no path to it, or this creature can't move)
        ObjectGuid OutOfReachVictimGUID; // The victim OutOfReachTimeInMS is counting for
        float AttackRange = 0; // Yards a ranged attacker attacks from (RO archers, plants and the like), 0 for melee
        uint32 AttackVisualSpellID = 0; // Shown on the target with each attack (an arrow, the Hydra's tentacle), 0 for none

        // RO monsters with an attack range attack from that distance with their normal attack
        void LoadCreatureTemplateValues()
        {
            AttackRange = 0;
            AttackVisualSpellID = 0;
            ROWOWCreatureTemplate const* creatureTemplate = ROWOW->GetCreatureTemplate(me->GetEntry());
            if (creatureTemplate == nullptr)
                return;
            AttackRange = creatureTemplate->AttackRange;
            AttackVisualSpellID = creatureTemplate->AttackVisualSpellID;
        }

        void InitializeAI() override
        {
            // The random walking is done here, so the core's random movement can't run alongside it.  The chasing too, so the core
            // doesn't start its own chase on attack
            if (ROWOW->IsEnabled == true)
            {
                me->SetCombatMovement(false);
                TakeOverDefaultMovement();
                LoadCreatureTemplateValues();
            }
            ScriptedAI::InitializeAI();
        }

        void TakeOverDefaultMovement()
        {
            if (me->GetDefaultMovementType() != RANDOM_MOTION_TYPE)
                return;
            me->SetDefaultMovementType(IDLE_MOTION_TYPE);
            me->GetMotionMaster()->Initialize();
        }

        void Reset() override
        {
            events.Reset();
            IsMovingOnContouredPath = false;
            HasChaseGoal = false;
            ChaseRepathTimer = 0;
            OutOfReachTimeInMS = 0;
            OutOfReachVictimGUID.Clear();
            if (ROWOW->IsEnabled == false)
                return;

            // A respawn into a different entry reloads the core's movement type, and has its own attack
            TakeOverDefaultMovement();
            LoadCreatureTemplateValues();

            // The first ground check runs on the first update, once the creature is fully in the world
            events.ScheduleEvent(EVENT_GROUND_SNAP_CHECK, 0ms);
            if (me->GetWanderDistance() > 0 && me->HasUnitFlag(UNIT_FLAG_DISABLE_MOVE) == false)
                events.ScheduleEvent(EVENT_RANDOM_WALK, Milliseconds(urand(ROWOW->ConfigRandomWalkMinDelayInMS, ROWOW->ConfigRandomWalkMaxDelayInMS)));
        }

        // Movement the core controls (fear, confuse, knockbacks, jumps, charges) or that's otherwise impossible right now.  Creatures
        // that never move (RO plants and the like) count as blocked unless that's ignored, since they still get put on the ground
        bool IsMovementBlocked(bool ignoreDisableMove = false)
        {
            if (me->HasUnitState(UNIT_STATE_LOST_CONTROL) == true || me->HasUnitState(UNIT_STATE_NOT_MOVE) == true)
                return true;
            if (me->HasUnitState(UNIT_STATE_CASTING) == true)
                return true;
            if (ignoreDisableMove == false && me->HasUnitFlag(UNIT_FLAG_DISABLE_MOVE) == true)
                return true;
            if (me->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_CONTROLLED) != NULL_MOTION_TYPE)
                return true;
            return false;
        }

        // Puts a standing creature back on the ground if it has drifted off of it.  The ground found has to be below (or barely above)
        // the creature, so a creature under a low bridge or table isn't put on top of it
        void SnapToGround()
        {
            if (me->IsInWorld() == false || me->isDead() == true)
                return;
            if (me->movespline->Finalized() == false || IsMovementBlocked(true) == true)
                return;
            float groundZ = 0;
            if (ROWOW->GetGroundZ(me, me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(), groundZ) == false)
                return;
            if (groundZ > me->GetPositionZ() + ROWOW->ConfigMoveMaxRisePerStep)
            {
                if (ROWOW->GetGroundZ(me, me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(), 0.5f, groundZ) == false)
                    return;
            }
            if (std::fabs(groundZ - me->GetPositionZ()) <= ROWOW->ConfigGroundSnapTolerance)
                return;
            me->NearTeleportTo(me->GetPositionX(), me->GetPositionY(), groundZ, me->GetOrientation());
        }

        // Stops this AI's own path only, leaving any movement the core controls (like a fear) alone
        void StopContouredMovement()
        {
            IsMovingOnContouredPath = false;
            HasChaseGoal = false;
            if (me->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_ACTIVE) != ESCORT_MOTION_TYPE)
                return;
            me->GetMotionMaster()->MovementExpiredOnSlot(MOTION_SLOT_ACTIVE, false);
            if (me->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_CONTROLLED) == NULL_MOTION_TYPE)
                me->StopMoving();
        }

        // Like RO's random walk: a spot up to several cells away in any direction, then a pause of a few seconds plus the walk time.
        // Unlike RO the spot stays within the spawn's wander distance, so creatures don't drift away from their spawn area over time
        void PerformRandomWalk()
        {
            uint32 nextWalkDelayInMS = urand(ROWOW->ConfigRandomWalkMinDelayInMS, ROWOW->ConfigRandomWalkMaxDelayInMS);
            if (IsMovementBlocked() == true)
            {
                events.ScheduleEvent(EVENT_RANDOM_WALK, Milliseconds(nextWalkDelayInMS));
                return;
            }
            float spawnX = 0;
            float spawnY = 0;
            float spawnZ = 0;
            me->GetRespawnPosition(spawnX, spawnY, spawnZ);
            float wanderDistance = me->GetWanderDistance();
            float maxWalkDistance = ROWOW->ConfigRandomWalkMaxDistance;
            for (uint32 attempt = 0; attempt < 4; ++attempt)
            {
                float targetX = me->GetPositionX() + frand(-maxWalkDistance, maxWalkDistance);
                float targetY = me->GetPositionY() + frand(-maxWalkDistance, maxWalkDistance);
                float spawnDeltaX = targetX - spawnX;
                float spawnDeltaY = targetY - spawnY;
                float spawnDistance = std::sqrt(spawnDeltaX * spawnDeltaX + spawnDeltaY * spawnDeltaY);
                if (spawnDistance > wanderDistance && spawnDistance > 0.01f)
                {
                    targetX = spawnX + spawnDeltaX * (wanderDistance / spawnDistance);
                    targetY = spawnY + spawnDeltaY * (wanderDistance / spawnDistance);
                }

                // One walk never goes farther than a normal random walk, even when heading back toward the spawn area from far away
                float walkDeltaX = targetX - me->GetPositionX();
                float walkDeltaY = targetY - me->GetPositionY();
                float walkDistance = std::sqrt(walkDeltaX * walkDeltaX + walkDeltaY * walkDeltaY);
                float longestWalkDistance = maxWalkDistance * 1.4143f;
                if (walkDistance > longestWalkDistance)
                {
                    targetX = me->GetPositionX() + walkDeltaX * (longestWalkDistance / walkDistance);
                    targetY = me->GetPositionY() + walkDeltaY * (longestWalkDistance / walkDistance);
                }
                if (me->GetExactDist2d(targetX, targetY) < 1.0f)
                    continue;

                Movement::PointsArray path;
                if (ROWOW->BuildGroundContouredPath(me, targetX, targetY, 0, path) == false)
                    continue;
                me->GetMotionMaster()->MoveSplinePath(&path, FORCED_MOVEMENT_WALK);
                IsMovingOnContouredPath = true;
                HasChaseGoal = false;

                // The pause starts once the walk is done
                float walkSpeed = me->GetSpeed(MOVE_WALK);
                if (walkSpeed > 0.1f)
                {
                    float pathLength = 0;
                    for (size_t i = 1; i < path.size(); ++i)
                        pathLength += std::sqrt((path[i].x - path[i - 1].x) * (path[i].x - path[i - 1].x) + (path[i].y - path[i - 1].y) * (path[i].y - path[i - 1].y));
                    nextWalkDelayInMS += uint32(pathLength / walkSpeed * 1000.0f);
                }
                break;
            }
            events.ScheduleEvent(EVENT_RANDOM_WALK, Milliseconds(nextWalkDelayInMS));
        }

        // Within reach of this creature's attack: melee range, or for a ranged attacker its attack range with nothing in the way
        bool IsVictimInAttackReach(Unit* victim)
        {
            if (me->IsWithinMeleeRange(victim) == true)
                return true;
            if (AttackRange <= 0)
                return false;
            if (me->GetExactDist(victim) > AttackRange + victim->GetCombatReach())
                return false;
            return me->IsWithinLOSInMap(victim);
        }

        // RO monster attacks are the normal attack, whatever the range, along with what the attack shows at the target
        void DoROAttackIfReady()
        {
            if (AttackRange <= 0 && AttackVisualSpellID == 0)
            {
                DoMeleeAttackIfReady();
                return;
            }
            Unit* victim = me->GetVictim();
            if (victim == nullptr || me->HasUnitState(UNIT_STATE_CASTING) == true || me->isAttackReady() == false)
                return;
            if (IsVictimInAttackReach(victim) == false)
                return;

            // Turn to the target first, as RO monsters do (a ranged attacker isn't brought around by a chase)
            if (me->movespline->Finalized() == true && me->HasInArc(float(M_PI) / 4.0f, victim) == false && IsMovementBlocked(true) == false)
                me->SetFacingToObject(victim);
            me->AttackerStateUpdate(victim);
            me->resetAttackTimer();
            if (AttackVisualSpellID != 0)
                me->CastSpell(victim, AttackVisualSpellID, true);
        }

        void UpdateChase(uint32 diff)
        {
            Unit* victim = me->GetVictim();
            if (victim == nullptr)
                return;

            // An RO monster has no leash to its spawn point: it chases its target while it sees it (within area_size cells), and drops
            // it where it stands once it doesn't (mob_unlocktarget).  Meanwhile mod-everquest's leash for creatures off its maps is held off
            if (ROWOW->GetROMobID(me) != 0)
            {
                if (me->GetExactDist2d(victim) > float(ROWOW->BattleConfig.AreaSize) * ROWOW->CellSize + me->GetCombatReach() + victim->GetCombatReach())
                {
                    EnterEvadeMode(EVADE_REASON_OTHER);
                    return;
                }
                me->UpdateLeashExtensionTime();
            }

            // The out of reach time is for one victim, so a new victim starts over
            if (OutOfReachVictimGUID != victim->GetGUID())
            {
                OutOfReachVictimGUID = victim->GetGUID();
                OutOfReachTimeInMS = 0;
                ChaseRepathTimer = 0;
                if (me->CanNotReachTarget() == true)
                    me->SetCannotReachTarget();
            }

            // In reach, so stop
            if (IsVictimInAttackReach(victim) == true)
            {
                OutOfReachTimeInMS = 0;
                if (me->CanNotReachTarget() == true)
                    me->SetCannotReachTarget();
                if (IsMovingOnContouredPath == true && IsMovementBlocked() == false)
                    StopContouredMovement();
                return;
            }

            // A creature that never moves gives up on a target that stays out of its reach (RO plants don't chase), without the core's
            // unreachable-target handling, which would make it immune to attacks.  Taking damage starts the count over (see DamageTaken),
            // so it doesn't give up on someone shooting it from range
            if (me->HasUnitFlag(UNIT_FLAG_DISABLE_MOVE) == true)
            {
                if (me->CanNotReachTarget() == true)
                    me->SetCannotReachTarget();
                OutOfReachTimeInMS += diff;
                if (OutOfReachTimeInMS >= ROWOW->ConfigOutOfReachGiveUpTimeInMS)
                {
                    OutOfReachTimeInMS = 0;
                    EnterEvadeMode(EVADE_REASON_NO_PATH);
                }
                return;
            }

            // While the core controls the movement (fear, confuse, knockback) this AI's path must not be running or queued under it
            if (IsMovementBlocked() == true)
            {
                if (me->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_CONTROLLED) != NULL_MOTION_TYPE)
                    StopContouredMovement();
                return;
            }

            // Re-path now and then, but only when the target has moved away from where this creature is already heading
            if (ChaseRepathTimer > diff)
            {
                ChaseRepathTimer -= diff;
                return;
            }
            ChaseRepathTimer = ROWOW->ConfigChaseRepathIntervalInMS;
            if (IsMovingOnContouredPath == true && HasChaseGoal == true && victim->GetExactDist2d(ChaseGoalX, ChaseGoalY) < ROWOW->ConfigChaseRepathTargetMoveDistance)
                return;

            // A path that can't get within reach (a wall, a cliff, a target up on a ledge) means the target can't be reached, which lets
            // the core's unreachable target handling (evading after a while) take over, as it does for its own chase.  A ranged attacker
            // only closes to within its attack range, unless something blocks its view from there
            Movement::PointsArray path;
            float meleeRange = me->GetMeleeRange(victim);
            float reachDistance = meleeRange;
            float stopShortDistance = meleeRange * 0.5f;
            if (AttackRange > meleeRange && (me->GetExactDist(victim) > AttackRange || me->IsWithinLOSInMap(victim) == true))
            {
                reachDistance = AttackRange;
                stopShortDistance = AttackRange * ROWOW->ConfigRangedAttackApproachFraction;
            }
            bool canReach = ROWOW->BuildGroundContouredPath(me, victim->GetPositionX(), victim->GetPositionY(), stopShortDistance, path);
            if (canReach == true)
            {
                G3D::Vector3 const& pathEnd = path.back();
                float endDeltaX = victim->GetPositionX() - pathEnd.x;
                float endDeltaY = victim->GetPositionY() - pathEnd.y;
                float endDeltaZ = victim->GetPositionZ() - pathEnd.z;
                if (std::sqrt(endDeltaX * endDeltaX + endDeltaY * endDeltaY + endDeltaZ * endDeltaZ) > reachDistance)
                    canReach = false;
            }
            else if (me->GetExactDist2d(victim) <= stopShortDistance + 0.1f)
                canReach = false; // Close in 2D, but out of reach in height
            // The path is only straight-line steps, so a failure can be a tree or a pillar in the way.  Only after it keeps failing does the
            // target count as unreachable
            if (canReach == false)
            {
                OutOfReachTimeInMS += ROWOW->ConfigChaseRepathIntervalInMS;
                if (OutOfReachTimeInMS >= ROWOW->ConfigUnreachableAfterTimeInMS && me->CanNotReachTarget() == false)
                    me->SetCannotReachTarget(victim->GetGUID());
                if (path.size() < 2)
                    return;
            }
            else
            {
                OutOfReachTimeInMS = 0;
                if (me->CanNotReachTarget() == true)
                    me->SetCannotReachTarget();
            }

            me->GetMotionMaster()->MoveSplinePath(&path, FORCED_MOVEMENT_RUN);
            IsMovingOnContouredPath = true;
            HasChaseGoal = true;
            ChaseGoalX = victim->GetPositionX();
            ChaseGoalY = victim->GetPositionY();
        }

        void UpdateAI(uint32 diff) override
        {
            if (ROWOW->IsEnabled == false)
            {
                ScriptedAI::UpdateAI(diff);
                return;
            }

            events.Update(diff);

            // Note when a path has been walked
            if (IsMovingOnContouredPath == true && me->GetMotionMaster()->HasMovementGeneratorType(ESCORT_MOTION_TYPE) == false)
            {
                IsMovingOnContouredPath = false;
                HasChaseGoal = false;
            }

            if (UpdateVictim() == false)
            {
                while (uint32 eventId = events.ExecuteEvent())
                {
                    switch (eventId)
                    {
                    case EVENT_RANDOM_WALK:
                    {
                        PerformRandomWalk();
                    } break;
                    case EVENT_GROUND_SNAP_CHECK:
                    {
                        if (IsMovingOnContouredPath == false && me->IsInEvadeMode() == false)
                            SnapToGround();
                        events.ScheduleEvent(EVENT_GROUND_SNAP_CHECK, Milliseconds(ROWOW->ConfigGroundSnapCheckIntervalInMS));
                    } break;
                    default: break;
                    }
                }
                return;
            }

            // In combat, a creature that's stopped is still kept on the ground (the core's own movement can leave it off of it)
            while (uint32 eventId = events.ExecuteEvent())
            {
                if (eventId == EVENT_GROUND_SNAP_CHECK)
                {
                    if (IsMovingOnContouredPath == false)
                        SnapToGround();
                    events.ScheduleEvent(EVENT_GROUND_SNAP_CHECK, Milliseconds(ROWOW->ConfigGroundSnapCheckIntervalInMS));
                }
            }
            UpdateChase(diff);
            DoROAttackIfReady();
        }

        void DamageTaken(Unit* attacker, uint32& damage, DamageEffectType damageType, SpellSchoolMask damageSchoolMask) override
        {
            ScriptedAI::DamageTaken(attacker, damage, damageType, damageSchoolMask);
            if (attacker != nullptr && me->HasUnitFlag(UNIT_FLAG_DISABLE_MOVE) == true)
                OutOfReachTimeInMS = 0;
        }

        void JustEngagedWith(Unit* who) override
        {
            ScriptedAI::JustEngagedWith(who);
            if (ROWOW->IsEnabled == false)
                return;
            events.CancelEvent(EVENT_RANDOM_WALK);
            StopContouredMovement();
            ChaseRepathTimer = 0;
        }

        void EnterEvadeMode(EvadeReason why) override
        {
            // RO monsters give up a chase where they stand instead of walking back to their spawn point
            if (ROWOW->IsEnabled == true && me->IsAlive() == true)
            {
                IsMovingOnContouredPath = false;
                HasChaseGoal = false;
                me->SetHomePosition(me->GetPosition());
            }
            ScriptedAI::EnterEvadeMode(why);
        }
    };

    CreatureAI* GetAI(Creature* creature) const override
    {
        return new ROWOW_CreatureScriptAI(creature);
    }
};

void AddROWOWCreatureScripts()
{
    new ROWOW_CreatureScript();
}
