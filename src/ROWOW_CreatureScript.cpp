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
#include "ScriptedGossip.h"

#include "ROWOW.h"

#include <cmath>

using namespace std;

// Ragnarok Online creatures always stand on, and move along, the ground, and an RO monster is run by RO's own AI (ROWOW_Mob.cpp): this script hands it
// the creature's update and the core's calls, and keeps idle creatures snapped to the ground.  WoW's threat and evading don't decide anything for it
class ROWOW_CreatureScript : public CreatureScript
{
public:
    ROWOW_CreatureScript() : CreatureScript("ROWOW_CreatureScript") {}

    struct ROWOW_CreatureScriptAI : public ScriptedAI
    {
        ROWOW_CreatureScriptAI(Creature* creature) : ScriptedAI(creature) {}

        enum Events
        {
            EVENT_GROUND_SNAP_CHECK = 1
        };
        EventMap events;

        bool IsROMonster = false; // An RO monster, not an RO town NPC

        void InitializeAI() override
        {
            // The walking is done by the AI, so the core's random movement can't run alongside it.  The chasing too, so the core doesn't start its
            // own chase on attack
            if (ROWOW->IsEnabled == true)
            {
                me->SetCombatMovement(false);
                TakeOverDefaultMovement();
                IsROMonster = ROWOW->GetROMobID(me) != 0;
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
            if (ROWOW->IsEnabled == false)
                return;

            // A respawn into a different entry reloads the core's movement type
            TakeOverDefaultMovement();
            IsROMonster = ROWOW->GetROMobID(me) != 0;
            if (IsROMonster == true)
                ROWOW->MobSpawnReset(me);

            // The first ground check runs on the first update, once the creature is fully in the world
            events.ScheduleEvent(EVENT_GROUND_SNAP_CHECK, 0ms);
        }

        // Puts a standing creature back on the ground if it has drifted off of it.  The ground found has to be below (or barely above)
        // the creature, so a creature under a low bridge or table isn't put on top of it
        void SnapToGround()
        {
            if (me->IsInWorld() == false || me->isDead() == true)
                return;
            if (me->movespline->Finalized() == false || ROWOW->IsMobMovementBlocked(me, true) == true)
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

        void UpdateAI(uint32 diff) override
        {
            if (ROWOW->IsEnabled == false)
            {
                ScriptedAI::UpdateAI(diff);
                return;
            }

            events.Update(diff);
            while (uint32 eventId = events.ExecuteEvent())
            {
                if (eventId == EVENT_GROUND_SNAP_CHECK)
                {
                    SnapToGround();
                    events.ScheduleEvent(EVENT_GROUND_SNAP_CHECK, Milliseconds(ROWOW->ConfigGroundSnapCheckIntervalInMS));
                }
            }
            if (IsROMonster == true)
                ROWOW->MobUpdate(me, diff);
            else
                ScriptedAI::UpdateAI(diff);
        }

        bool CanAIAttack(Unit const* target) const override
        {
            // The RO target rules the WoW threat list doesn't know (a Rogue in Gangster's Paradise): a target that fails them is dropped
            return ROWOW->IsEnabled == false || ROWOW->MobCanTarget(me, target) == true;
        }

        // An RO monster looks for its own targets (mob_ai_sub_hard_activesearch)
        void MoveInLineOfSight(Unit* who) override
        {
            if (ROWOW->IsEnabled == false || IsROMonster == false)
                ScriptedAI::MoveInLineOfSight(who);
        }

        void AttackStart(Unit* who) override
        {
            // What the skills call to turn a monster on someone (mob_target)
            if (ROWOW->IsEnabled == true && IsROMonster == true)
            {
                if (who != nullptr)
                    ROWOW->MobTarget(me, who);
                return;
            }
            ScriptedAI::AttackStart(who);
        }

        void EnterEvadeMode(EvadeReason why) override
        {
            // An RO monster never goes home: it drops its target where it stands (mob_unlocktarget) and keeps its HP and statuses.  The skills that
            // call this (Hiding, Ankle Snare's kin) mean that
            if (ROWOW->IsEnabled == true && IsROMonster == true)
            {
                if (me->IsAlive() == true)
                {
                    ROWOW->MobUnlockTarget(me, ROWOWMod::GetTick());
                    ROWOW->MobReleaseCombat(me, ROWOW->GetMobData(me));
                }
                return;
            }
            ScriptedAI::EnterEvadeMode(why);
        }

        void JustDied(Unit* killer) override
        {
            ScriptedAI::JustDied(killer);
            if (ROWOW->IsEnabled == true && IsROMonster == true)
                ROWOW->MobSetDelaySpawn(me);
        }

        bool CanRespawn() override
        {
            return ROWOW->IsEnabled == false || ROWOW->MobCanRespawn(me);
        }
    };

    CreatureAI* GetAI(Creature* creature) const override
    {
        return new ROWOW_CreatureScriptAI(creature);
    }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        // Clicking an RO NPC in the form starts its script (npc_click), its dialog in the RO NPC window; the converter's gossip (the
        // script's first lines) is left for anyone else.  Shops stay the vendors they are
        if (ROWOW->IsEnabled == false || ROWOW->IsInROForm(player) == false)
            return false;
        ROWOWNpc const* npc = ROWOW->GetCreatureNpc(creature);
        if (npc == nullptr || npc->Script == nullptr || npc->IsShop == true)
            return false;
        CloseGossipMenuFor(player);
        if (ROWOW->PCCantAct(player) == false) // clif_parse_NpcClicked's pc_cant_act2: a script could use up what the trade offers
            ROWOW->StartNpcRun(player, npc);
        return true;
    }
};

void AddROWOWCreatureScripts()
{
    new ROWOW_CreatureScript();
}
