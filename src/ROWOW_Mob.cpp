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
#include "CreatureAI.h"
#include "GameTime.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ThreatManager.h"

#include "ROWOW.h"
#include "ROWOW_Skill.h"

#include <algorithm>

using namespace std;

// RO monster AI, a port of rAthena's mob.cpp (pre-renewal): mob_ai_sub_hard and what it calls, under the same names.  A monster's target is its own
// (ROWOWMobData::TargetID, md->target_id): the WoW threat list only keeps the WoW combat state, and the WoW victim follows the target.  Mob skills and
// slaves are in ROWOW_MobSkill.cpp, the items it loots in ROWOW_FloorItem.cpp.  Everything here runs in the monster's map thread.  Other monsters of the map (assist)
// and the players it looks at are on that map too, so nothing here needs a lock; startup data is only read

namespace
{
    // mob.hpp and mob.cpp
    uint64 constexpr MIN_MOBTHINKTIME = 100;
    uint64 constexpr MIN_MOBLINKTIME = 1000;
    uint64 constexpr MIN_RANDOMWALKTIME = 4000;
    uint64 constexpr MOB_SKILL_INTERVAL = 1000;
    int32 constexpr ACTIVE_AI_RANGE = 4;
    uint8 constexpr RUDE_ATTACKED_COUNT = 1;
    size_t constexpr DAMAGELOG_SIZE = 30;
    int32 constexpr MOBSKILL_EVENT_NONE = -1; // mobskill_use's event for no event

    // status_db.hasSCF for SCF_MOBLOSETARGET: a status that makes a monster lose its target
    bool HasMobLoseTargetStatus(ROWOWMod const* mod, ROWOWStatusChange const* sc)
    {
        for (auto const& entryByType : sc->EntriesByType)
        {
            ROWOWStatusChangeDB const* scdb = mod->GetStatusChangeDB(entryByType.first);
            if (scdb != nullptr && scdb->Flag[ROWOW_SCF_MOBLOSETARGET] == true)
                return true;
        }
        return false;
    }
}

ROWOWMobData* ROWOWMod::GetMobData(Creature* md) const
{
    return md->CustomData.GetDefault<ROWOWMobData>("ROWOWMobData");
}

bool ROWOWMod::MobIsChasing(uint8 skillState)
{
    // mob_is_chasing
    return skillState == ROWOW_MSS_LOOT || skillState == ROWOW_MSS_RUSH || skillState == ROWOW_MSS_FOLLOW;
}

void ROWOWMod::MobSetState(Creature* md, ROWOWMobData* mob, uint8 skillState) const
{
    // mob_setstate: the attack and chase states keep the aggressive bit, and going to any other state resets it to the monster's mode
    switch (skillState)
    {
        case ROWOW_MSS_BERSERK:
        case ROWOW_MSS_ANGRY:
            mob->SkillState = mob->IsAggressive == true ? ROWOW_MSS_ANGRY : ROWOW_MSS_BERSERK;
            break;
        case ROWOW_MSS_RUSH:
        case ROWOW_MSS_FOLLOW:
            mob->SkillState = mob->IsAggressive == true ? ROWOW_MSS_FOLLOW : ROWOW_MSS_RUSH;
            break;
        default:
        {
            ROWOWStatus const* status = GetROStatus(md);
            mob->IsAggressive = status != nullptr && (status->Mode & ROWOW_MODE_ANGRY) != 0;
            mob->SkillState = skillState;
            break;
        }
    }
}

void ROWOWMod::MobSpawnReset(Creature* md)
{
    // mob_spawn's reset of the monster, once it's back (or new)
    uint64 tick = GetTick();
    ROWOWMobData* mob = GetMobData(md);
    mob->TargetID.Clear();
    mob->LootTargetID.Clear();
    mob->LootWalkID.Clear();
    mob->LootItems.clear();
    mob->IsNoRewards = false;
    mob->AttackedID.Clear();
    mob->NormAttackedID.Clear();
    mob->ProvokeFlag.Clear();
    mob->AttackedCount = 0;
    mob->AttackedEvents.clear();
    mob->IsAttackContinue = false;
    mob->AttackTargetID.Clear();
    mob->TargetToID.Clear();
    mob->IsWalking = false;
    mob->CanMoveTime = 0;
    MobSetState(md, mob, ROWOW_MSS_IDLE);
    mob->NextThinkTime = tick;
    mob->NextWalkTime = tick + urand(0, 999) + MIN_RANDOMWALKTIME;
    mob->LastLinkTime = 0;
    mob->LastPCNearTime = 0;
    mob->LastCanMove = tick;
    mob->LastSkillCheck = tick;
    mob->SpottedLog.clear();
    mob->SpawnTime = 0;
    mob->SkillIdx = -1;
    mob->SkillDelays.clear();
    mob->AfterSkillID = 0;
    mob->IsRebirth = false;
    mob->TrickCasting = 0;
    mob->IsCanEscape = false;
    mob->SlaveGUIDs.clear();
    md->CustomData.Erase("ROWOWMobBaseStatus"); // status_calc_mob: NPC_SPEEDUP's speed goes
    mob->IsSpawnSkillPending = true; // mobskill_use(md, tick, MSC_SPAWN), at its first update
    md->CustomData.Erase("ROWOWDamageLog"); // dmglog
    md->SetRegeneratingHealth(false); // RO monsters don't regenerate on their own (no regen_data)
    md->SetRespawnCompatibilityMode(); // The creature waits out its death (the core would destroy it and make a new one at its database spot)

    // Spread out the monsters' thinking, so a grid that loads doesn't have them all think in the same update
    mob->ThinkTimer = uint32(urand(0, uint32(MIN_MOBTHINKTIME)));
    mob->LazyTimer = uint32(urand(0, uint32(MIN_MOBTHINKTIME * 10)));

    // A spawn line's own name (mob_parse_dataset), shown as a script monster's is: the creature's name and the pet name query (SpawnScriptMob)
    ROWOWCreatureSpawn const* spawn = GetCreatureSpawn(md);
    if (spawn != nullptr && spawn->Line != nullptr && spawn->Line->Name.empty() == false && spawn->Line->Name != md->GetName())
    {
        md->SetName(spawn->Line->Name);
        md->SetUInt32Value(UNIT_FIELD_PETNUMBER, 0x80000000 | (md->GetGUID().GetCounter() & 0x7FFFFFFF));
        md->SetUInt32Value(UNIT_FIELD_PET_NAME_TIMESTAMP, uint32(GameTime::GetGameTime().count()));
    }
}

void ROWOWMod::MobAddSpotted(ROWOWMobData* mob, ObjectGuid const& guid) const
{
    // mob_add_spotted
    if (std::find(mob->SpottedLog.begin(), mob->SpottedLog.end(), guid) != mob->SpottedLog.end())
        return;
    if (mob->SpottedLog.size() < DAMAGELOG_SIZE)
        mob->SpottedLog.push_back(guid);
}

void ROWOWMod::MobCleanSpotted(Creature* md, ROWOWMobData* mob) const
{
    // mob_clean_spotted: those no longer around.  (rAthena looks the character up anywhere: a player who left the map can't see it either)
    for (size_t spottedIndex = mob->SpottedLog.size(); spottedIndex > 0; --spottedIndex)
        if (ObjectAccessor::GetPlayer(*md, mob->SpottedLog[spottedIndex - 1]) == nullptr)
            mob->SpottedLog.erase(mob->SpottedLog.begin() + (spottedIndex - 1));
}

bool ROWOWMod::CheckDistanceBL(Unit const* src, Unit const* target, int32 distance) const
{
    // check_distance_bl (no CIRCULAR_AREA)
    return DistanceBL(src, target) <= distance;
}

bool ROWOWMod::BattleCheckRange(Unit const* src, Unit const* bl, int32 range) const
{
    // battle_check_range, for a monster: within range, and within sight when more than a cell off
    if (src->GetMap() != bl->GetMap())
        return false;
    if (CheckDistanceBL(src, bl, range) == false)
        return false;
    int32 d = DistanceBL(src, bl);
    if (d < 2)
        return true; // No need for path checking.
    if (d > BattleConfig.AreaSize)
        return false; // Avoid targetting objects beyond your range of sight.
    ROWOWMapGrid const* mapGrid = GetMapGrid(src->GetMapId());
    if (mapGrid == nullptr)
        return true;
    int32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    GetUnitCell(src, x0, y0);
    GetUnitCell(bl, x1, y1);
    return PathSearchLong(*mapGrid, x0, y0, x1, y1);
}

bool ROWOWMod::MobCheckVisibility(Creature* md, Unit const* target, bool isCheckBlind) const
{
    // status_check_visibility for a monster: within its chase range (1 while blind), on its map, and not hidden from it
    ROWOWStatus const* status = GetROStatus(md);
    int32 viewRange = status != nullptr ? status->Range3 : BattleConfig.AreaSize;
    if (isCheckBlind == true && GetSCE(md, ROWOW_SC_BLIND) != nullptr)
        viewRange = 1;
    if (md->GetMap() != target->GetMap() || CheckDistanceBL(md, target, viewRange) == false)
        return false;
    return StatusCheckVisibility(md, target);
}

bool ROWOWMod::MobCanReach(Creature* md, Unit const* bl, int32 range) const
{
    // mob_can_reach
    int32 x = 0, y = 0;
    return UnitCanReachBL(md, bl, range, x, y);
}

int32 ROWOWMod::MobLinkSearch(Creature* md, Unit* target, uint64 tick)
{
    // unit_attack_timer_sub's link: the monsters of the same kind in assist_range and sight that have no target take this one (mob_linksearch)
    ROWOWMapGrid const* mapGrid = GetMapGrid(md->GetMapId());
    uint32 mobID = GetROMobID(md);
    int32 x0 = 0, y0 = 0;
    GetUnitCell(md, x0, y0);
    return MapForEachInRange(md, BattleConfig.AssistRange, [this, md, mapGrid, mobID, x0, y0, target, tick](Unit* bl)
    {
        Creature* linkMD = bl->ToCreature();
        if (linkMD == nullptr || linkMD == md || linkMD->IsAlive() == false || GetROMobID(linkMD) != mobID)
            return 0;
        ROWOWStatus const* status = GetROStatus(linkMD);
        if (status == nullptr || (status->Mode & ROWOW_MODE_ASSIST) == 0)
            return 0;
        int32 x1 = 0, y1 = 0;
        GetUnitCell(linkMD, x1, y1);
        if (BattleConfig.SkillWallCheck != 0 && mapGrid != nullptr && PathSearchLong(*mapGrid, x0, y0, x1, y1) == false) // map_foreachinshootrange
            return 0;

        // Check if mob qualifies for assistance.  Line of sight to the ally is already checked at this point, and no valid path to the target
        // is required
        ROWOWMobData* linkMob = GetMobData(linkMD);
        if (tick - linkMob->LastLinkTime >= MIN_MOBLINKTIME && linkMob->TargetID.IsEmpty() == true)
        {
            linkMob->LastLinkTime = tick;
            linkMob->TargetID = target->GetGUID();
            return 1;
        }
        return 0;
    });
}

bool ROWOWMod::MobCanChangeTarget(Creature* md, ROWOWMobData* mob, Unit const* target, uint32 mode) const
{
    // mob_can_changetarget.  Special feature that makes monsters always attack the person that provoked them
    if ((BattleConfig.MobAI & 0x800) != 0 && mob->ProvokeFlag.IsEmpty() == false)
    {
        if (mob->ProvokeFlag == target->GetGUID())
            return true;
        else if ((BattleConfig.MobAI & 0x4) == 0)
            return false;
    }
    switch (mob->SkillState)
    {
        case ROWOW_MSS_BERSERK:
        {
            if ((mode & ROWOW_MODE_CHANGETARGETMELEE) == 0)
                return false;
            // If the special normal attacked event occurred, always change target in berserk state
            if (mob->NormAttackedID == target->GetGUID())
                return true;
            // If the special setting to switch target even on skills is set, we need to verify the range here
            if ((BattleConfig.MobAI & 0x80) == 0)
                return false;
            ROWOWStatus const* status = GetROStatus(md);
            return (BattleConfig.MobAI & 0x4) != 0 || (status != nullptr && CheckDistanceBL(md, target, status->Range + 1) == true);
        }
        case ROWOW_MSS_RUSH:
            return (mode & ROWOW_MODE_CHANGETARGETCHASE) != 0;
        case ROWOW_MSS_FOLLOW:
        case ROWOW_MSS_ANGRY:
        case ROWOW_MSS_IDLE:
        case ROWOW_MSS_WALK:
        case ROWOW_MSS_LOOT:
            return true;
        default:
            return false;
    }
}

bool ROWOWMod::MobRandomTarget(Creature* md, Unit*& target)
{
    // mob_randomtarget: a monster with MD_RANDOMTARGET picks a random enemy in reach for each attack.  False when there's none in shoot range
    ROWOWStatus const* status = GetROStatus(md);
    if (status == nullptr || (status->Mode & ROWOW_MODE_RANDOMTARGET) == 0)
        return true; // Keep current target
    std::vector<Unit*> enemies;
    MapForEachInRange(md, status->Range, [this, md, &enemies](Unit* bl)
    {
        if (bl != md && BattleCheckTarget(md, bl, ROWOW_BCT_ENEMY) > 0 && StatusCheckVisibility(md, bl) == true)
            enemies.push_back(bl);
        return 0;
    });
    if (enemies.empty() == true)
        return false;
    Unit* enemy = enemies[urand(0, uint32(enemies.size() - 1))];
    if (BattleCheckRange(md, enemy, status->Range) == false)
        return false;
    target = enemy;
    return true;
}

void ROWOWMod::MobTarget(Creature* md, Unit* bl)
{
    // mob_target: nothing is carried out if it has a target it won't change, otherwise it locks on
    ROWOWStatus const* status = GetROStatus(md);
    if (status == nullptr)
        return;
    ROWOWMobData* mob = GetMobData(md);
    if (mob->TargetID.IsEmpty() == false && MobCanChangeTarget(md, mob, bl, status->Mode) == false)
        return;
    if (StatusCheckSkillUse(md, bl, 0, 0) == false)
        return;
    mob->TargetID = bl->GetGUID(); // Since there was no disturbance, it locks on to target.
    if (mob->ProvokeFlag.IsEmpty() == false && bl->GetGUID() != mob->ProvokeFlag)
        mob->ProvokeFlag.Clear();
    // When an angry monster is provoked, it will switch to retaliate AI
    if (mob->ProvokeFlag.IsEmpty() == false && mob->IsAggressive == true)
        mob->IsAggressive = false;
}

void ROWOWMod::MobSetProvoked(Creature* md, Unit* src)
{
    // SkillProvoke on a monster: provoke_flag, then mob_target
    GetMobData(md)->ProvokeFlag = src->GetGUID();
    MobTarget(md, src);
}

void ROWOWMod::MobCastSensor(Creature* md, Unit* src, uint64 tick)
{
    // unit_skilluse_id2: a skill being cast at a monster, by a player.  A cast sensor monster that's idle or walking (MD_CASTSENSORIDLE), or
    // chasing (MD_CASTSENSORCHASE), turns on the caster
    ROWOWStatus const* tstatus = GetROStatus(md);
    if (tstatus == nullptr)
        return;
    MobSkillEvent(md, src, tick, MOBSKILL_EVENT_NONE, 0); // Cast targetted skill event.
    if ((tstatus->Mode & (ROWOW_MODE_CASTSENSORIDLE | ROWOW_MODE_CASTSENSORCHASE)) == 0 || BattleCheckTarget(md, src, ROWOW_BCT_ENEMY) <= 0)
        return;
    ROWOWMobData* mob = GetMobData(md);
    switch (mob->SkillState)
    {
        case ROWOW_MSS_RUSH:
        case ROWOW_MSS_FOLLOW:
            if ((tstatus->Mode & ROWOW_MODE_CASTSENSORCHASE) == 0)
                break;
            mob->TargetID = src->GetGUID();
            mob->IsAggressive = (tstatus->Mode & ROWOW_MODE_ANGRY) != 0;
            break;
        case ROWOW_MSS_IDLE:
        case ROWOW_MSS_WALK:
            if ((tstatus->Mode & ROWOW_MODE_CASTSENSORIDLE) == 0)
                break;
            mob->TargetID = src->GetGUID();
            mob->IsAggressive = (tstatus->Mode & ROWOW_MODE_ANGRY) != 0;
            break;
    }
}

void ROWOWMod::MobAISubHardActiveSearch(Creature* md, ROWOWMobData* mob, Unit* bl, Unit*& target, uint32 mode, int32 range2, int32 range3) const
{
    // mob_ai_sub_hard_activesearch.  If can't seek yet, not an enemy, or you can't attack it, skip.  A summoned plant goes by RO's sides alone: WoW lets
    // no creature attack a neutral one, and most monsters are neutral to a player's faction, which the plant has
    bool isSummon = md->CustomData.Get<ROWOWMobSummon>("ROWOWMobSummon") != nullptr;
    if (target == bl || bl == md || (isSummon == false && IsROFollower(bl) == false && md->IsValidAttackTarget(bl) == false) || StatusCheckSkillUse(md, bl, 0, 0) == false)
        return;
    if ((mode & ROWOW_MODE_TARGETWEAK) != 0)
    {
        ROWOWStatus const* status = GetROStatus(md);
        ROWOWStatus const* tstatus = GetROStatus(bl);
        if (status != nullptr && tstatus != nullptr && int32(tstatus->Level) >= int32(status->Level) - 5)
            return;
    }
    if (BattleCheckTarget(md, bl, ROWOW_BCT_ENEMY) <= 0)
        return;
    if (MobCanTarget(md, bl) == false)
        return; // Gangster paradise protection.
    if ((BattleConfig.HomSetting & ROWOW_HOMSET_FIRST_TARGET) != 0 && target != nullptr && GetFollowerType(target) == ROWOW_BL_HOM && GetFollowerType(bl) != ROWOW_BL_HOM)
        return; // For some reason Homun targets are never overridden.
    int32 dist = DistanceBL(md, bl);
    if ((target == nullptr || CheckDistanceBL(md, target, dist) == false) && BattleCheckRange(md, bl, range2) == true) // Pick closest target?
    {
        // ACTIVEPATHSEARCH: count walk path cells.  Standing monsters use range2, walking monsters use range3
        ROWOWMapGrid const* mapGrid = GetMapGrid(md->GetMapId());
        if (mapGrid != nullptr && mapGrid->HasCells() == true)
        {
            int32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            GetUnitCell(md, x0, y0);
            GetUnitCell(bl, x1, y1);
            std::vector<uint8> path;
            if (PathSearch(path, md->GetMap(), *mapGrid, x0, y0, x1, y1, ROWOW_CELL_CHKWALL) == false)
                return;
            if ((mob->IsWalking == false && int32(path.size()) > range2) || (mob->IsWalking == true && int32(path.size()) > range3))
                return;
        }
        target = bl;
        mob->TargetID = bl->GetGUID();
    }
}

void ROWOWMod::MobForEachEnemyInRange(Creature* md, int32 range, std::function<void(Unit*)> const& func)
{
    // map_foreachinallrange over DEFAULT_ENEMY_TYPE.  A monster of its own only has players and what they summoned as enemies, so it needs no
    // search of the grid: this runs every think of every aggressive monster near a player.  A summoned monster has the whole map to look through
    if (md->CustomData.Get<ROWOWMobSummon>("ROWOWMobSummon") != nullptr)
    {
        MapForEachInRange(md, range, [&func](Unit* bl) { func(bl); return 0; });
        return;
    }
    Map::PlayerList const& players = md->GetMap()->GetPlayers();
    for (Map::PlayerList::const_iterator playerIter = players.begin(); playerIter != players.end(); ++playerIter)
    {
        Player* sd = playerIter->GetSource();
        if (sd == nullptr || sd->IsInWorld() == false)
            continue;
        if (GetROStatus(sd) != nullptr && CheckDistanceBL(md, sd, range) == true)
            func(sd);
        ROWOWSkillUnitData const* ud = sd->CustomData.Get<ROWOWSkillUnitData>("ROWOWSkillUnitData");
        if (ud == nullptr)
            continue;
        for (ObjectGuid const& summonGUID : ud->SummonGUIDs)
            if (Creature* summon = ObjectAccessor::GetCreature(*md, summonGUID); summon != nullptr && summon->IsInWorld() == true && CheckDistanceBL(md, summon, range) == true)
                func(summon);
        for (int32 followerType : { ROWOW_BL_HOM, ROWOW_BL_MER })
        {
            ROWOWFollower const* fd = GetFollower(sd, followerType);
            Creature* follower = fd != nullptr ? GetFollowerCreature(sd, *fd) : nullptr;
            if (follower != nullptr && follower->IsAlive() == true && CheckDistanceBL(md, follower, range) == true)
                func(follower);
        }
    }
}

void ROWOWMod::MobAISubHardChangeChase(Creature* md, ROWOWMobData* mob, Unit* bl, Unit*& target, int32 attackRange) const
{
    // mob_ai_sub_hard_changechase.  If can't seek yet, not an enemy, or you can't attack it, skip
    if (target == bl || bl == md || BattleCheckTarget(md, bl, ROWOW_BCT_ENEMY) <= 0 || StatusCheckSkillUse(md, bl, 0, 0) == false)
        return;
    if (BattleCheckRange(md, bl, attackRange) == true)
    {
        target = bl;
        mob->TargetID = bl->GetGUID();
    }
}

void ROWOWMod::MobUnlockTarget(Creature* md, uint64 tick)
{
    // mob_unlocktarget: a lock of target is stopped and the monster moves to a standby state
    ROWOWMobData* mob = GetMobData(md);
    bool isDefault = false;
    switch (mob->SkillState)
    {
        case ROWOW_MSS_WALK:
            if (mob->IsWalking == true)
                break;
            MobSetState(md, mob, ROWOW_MSS_IDLE); // Because it is not unset when the mob finishes walking.
            break;
        case ROWOW_MSS_IDLE:
            break;
        case ROWOW_MSS_LOOT:
            UnitStopWalkingSoon(md, tick); // Looters that lost their target stop after 0.5-1.5 cells moved
            isDefault = true;
            break;
        default:
            isDefault = true;
            break;
    }
    if (isDefault == true)
    {
        mob->AttackTargetID.Clear(); // unit_stop_attack
        MobSetState(md, mob, ROWOW_MSS_IDLE);
        if ((BattleConfig.MobAI & 0x8) != 0) // Walk instantly after dropping target
            mob->NextWalkTime = tick + urand(0, 999);
        else
            mob->NextWalkTime = tick + urand(0, 999) + MIN_RANDOMWALKTIME;
    }
    mob->TargetID.Clear();
    mob->LootTargetID.Clear();
    mob->LootWalkID.Clear();
    mob->IsAttackContinue = false;
    mob->TargetToID.Clear();
}

bool ROWOWMod::MobRandomWalk(Creature* md, uint64 tick)
{
    // mob_randomwalk
    ROWOWMobData* mob = GetMobData(md);
    ROWOWStatus const* status = GetROStatus(md);
    ROWOWMapGrid const* mapGrid = GetMapGrid(md->GetMapId());
    if (status == nullptr || mapGrid == nullptr || mapGrid->HasCells() == false)
        return false;
    int32 const d = 7;
    if (mob->NextWalkTime > tick || (status->Mode & ROWOW_MODE_NORANDOMWALK) != 0 || UnitCanMove(md, mob, tick) == false || (status->Mode & ROWOW_MODE_CANMOVE) == 0)
        return false;

    // Make sure the monster has no target anymore, otherwise the walkpath will check for it
    mob->IsAttackContinue = false;
    mob->TargetToID.Clear();

    int32 r = int32(urand(0, 0x7FFFFFFF));
    int32 rdir = int32(urand(0, 3)); // Randomize direction in which we iterate to prevent monster cluttering up in one corner
    int32 dx = r % (d * 2 + 1) - d;
    int32 dy = r / (d * 2 + 1) % (d * 2 + 1) - d;
    int32 max = (d * 2 + 1) * (d * 2 + 1);
    int32 mdX = 0, mdY = 0;
    GetUnitCell(md, mdX, mdY);
    int32 i = 0;
    for (i = 0; i < max; i++) // Search of a movable place
    {
        int32 x = dx + mdX;
        int32 y = dy + mdY;
        if ((x != mdX || y != mdY) && MapGetCellNoPass(md->GetMap(), *mapGrid, x, y) == false && UnitWalkToXY(md, x, y) == true)
            break;

        // Could not move to cell, try the 7th cell in direction randomly decided by rdir.  We don't move step-by-step because this will make
        // monster stick to the walls
        switch (rdir)
        {
            case 0:
                dx += d;
                if (dx > d)
                {
                    dx -= d * 2 + 1;
                    dy += d;
                    if (dy > d)
                        dy -= d * 2 + 1;
                }
                break;
            case 1:
                dx -= d;
                if (dx < -d)
                {
                    dx += d * 2 + 1;
                    dy -= d;
                    if (dy < -d)
                        dy += d * 2 + 1;
                }
                break;
            case 2:
                dy += d;
                if (dy > d)
                {
                    dy -= d * 2 + 1;
                    dx += d;
                    if (dx > d)
                        dx -= d * 2 + 1;
                }
                break;
            case 3:
                dy -= d;
                if (dy < -d)
                {
                    dy += d * 2 + 1;
                    dx -= d;
                    if (dx < -d)
                        dx += d * 2 + 1;
                }
                break;
        }
    }
    if (i == max)
        return false; // None of the available cells worked, try again next interval
    MobSetState(md, mob, ROWOW_MSS_WALK);
    mob->NextWalkTime = tick + urand(0, 999) + MIN_RANDOMWALKTIME + mob->WalkPathTime;
    return true;
}

bool ROWOWMod::MobAISubHard(Creature* md, uint64 tick)
{
    // mob_ai_sub_hard: the AI of a monster near a player.  (Monsters force-walked by script commands aren't in yet; warp chase is off, monster_ai&0x40)
    if (md->IsInWorld() == false || md->IsAlive() == false)
        return false;
    ROWOWMobData* mob = GetMobData(md);
    if (tick < mob->NextThinkTime)
        return false;

    // This prevents the lazy AI from being executed at the same time
    mob->NextThinkTime = tick;
    if (md->HasUnitState(UNIT_STATE_CASTING) == true)
        return false;

    // Abnormalities: should reset targets
    ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(md));
    if (sc != nullptr && ((sc->Opt1 != ROWOW_OPT1_NONE && sc->Opt1 != ROWOW_OPT1_STONEWAIT && sc->Opt1 != ROWOW_OPT1_BURNING) || HasMobLoseTargetStatus(this, sc) == true))
    {
        mob->TargetID.Clear();
        mob->AttackedID.Clear();
        mob->NormAttackedID.Clear();
        return false;
    }

    // Before a monster processes its AI, it will check for a skill.  If it uses a skill it will not process its AI further until the next
    // interval.  Dead and Berserk states have special interval rules
    if (mob->IsWalking == false)
    {
        bool isSkillReady = false;
        if (mob->SkillState == ROWOW_MSS_BERSERK)
            isSkillReady = md->isAttackReady();
        else
            isSkillReady = tick - mob->LastSkillCheck >= MOB_SKILL_INTERVAL;
        if (isSkillReady == true && MobSkillUse(md, tick, MOBSKILL_EVENT_NONE) == true)
            return true;
    }

    ROWOWStatus const* status = GetROStatus(md);
    if (status == nullptr)
        return false;
    int32 viewRange = GetSCE(md, ROWOW_SC_BLIND) != nullptr ? 1 : status->Range2;
    uint32 mode = status->Mode;
    int32 attackRange = status->Range;
    bool canMove = (mode & ROWOW_MODE_CANMOVE) != 0 && UnitCanMove(md, mob, tick) == true;
    if (canMove == true)
        mob->LastCanMove = tick;

    // Its target can be a floor item, which isn't a Unit and has its own ID
    Unit* tbl = nullptr;
    Unit* abl = nullptr;
    ROWOWFloorItem const* fitem = nullptr;
    if (mob->TargetID.IsEmpty() == false)
        mob->LootTargetID.Clear();
    else if (mob->LootTargetID.IsEmpty() == false && (fitem = MapFindFloorItem(md->GetMap(), mob->LootTargetID)) == nullptr)
        MobUnlockTarget(md, tick); // No valid target
    if (mob->TargetID.IsEmpty() == false)
    {
        // Check validity of current target. [Skotlex]
        tbl = ObjectAccessor::GetUnit(*md, mob->TargetID);
        if (tbl == nullptr || tbl->IsInWorld() == false || tbl->GetMap() != md->GetMap() || tbl->IsAlive() == false || GetROStatus(tbl) == nullptr ||
            (mob->AttackTargetID.IsEmpty() == true && StatusCheckSkillUse(md, tbl, 0, 0) == false) ||
            MobCanTarget(md, tbl) == false || PCIsInvincible(tbl) == true)
        {
            // No valid target: walk at least "mob_chase_refresh" cells before dropping the target unless target is non-existent
            if (tbl != nullptr && tbl->IsAlive() == true && tbl->GetMap() == md->GetMap() && mob->IsWalking == true &&
                (canMove == false || UnitWalkPathPos(md, mob, tick) <= BattleConfig.MobChaseRefresh))
                return true;
            MobUnlockTarget(md, tick); // Unlock target
            tbl = nullptr;
        }

        // Monsters in chase state that are not moving will recheck their target.  This usually happens when they used a skill that stopped them
        // or they were hit while chasing.  Items are handled later in this function
        if (tbl != nullptr && mob->IsWalking == false && MobIsChasing(mob->SkillState) == true)
        {
            // But don't do this if they stopped because target is already in attack range.  If target is no longer visible, target is dropped
            // and the monster waits for next iteration to continue
            if (BattleCheckRange(md, tbl, attackRange) == false && MobCheckVisibility(md, tbl, true) == false)
            {
                MobUnlockTarget(md, tick);
                return true;
            }
        }
    }

    // Check for target change.
    if (mob->AttackedID.IsEmpty() == false && (mode & ROWOW_MODE_CANATTACK) != 0)
    {
        if (mob->AttackedID == mob->TargetID)
        {
            // Rude attacked check: can't attack back and can't reach back
            bool isTrapped = canMove == false && mob->CanMoveTime < tick && ((BattleConfig.MobAI & 0x2) != 0 || GetSCE(md, ROWOW_SC_SPIDERWEB) != nullptr);
            if (tbl != nullptr && BattleCheckRange(md, tbl, attackRange) == false && (isTrapped == true || MobCanReach(md, tbl, status->Range3) == false) &&
                ++mob->AttackedCount > RUDE_ATTACKED_COUNT)
                MobSkillUse(md, tick, ROWOW_MSC_RUDEATTACKED);
        }
        else if ((abl = ObjectAccessor::GetUnit(*md, mob->AttackedID)) != nullptr && (tbl == nullptr || MobCanChangeTarget(md, mob, abl, mode) == true))
        {
            int32 dist = 0;
            if (abl->GetMap() != md->GetMap() || abl->IsInWorld() == false ||
                (dist = DistanceBL(md, abl)) > BattleConfig.AreaSize || // Attacker longer than visual area
                BattleCheckTarget(md, abl, ROWOW_BCT_ENEMY) <= 0 || // Attacker is not enemy of mob
                ((BattleConfig.MobAI & 0x2) != 0 && StatusCheckSkillUse(md, abl, 0, 0) == false) || // Cannot normal attack back to Attacker
                (BattleCheckRange(md, abl, attackRange) == false && // Not on Melee Range and ...
                    ((canMove == false && mob->CanMoveTime < tick && ((BattleConfig.MobAI & 0x2) != 0 || GetSCE(md, ROWOW_SC_SPIDERWEB) != nullptr)) ||
                        MobCanReach(md, abl, dist + status->Range3) == false))) // Reach check
            {
                // Rude attacked.  Self damage does not cause rude attack
                if (abl != md && ++mob->AttackedCount > RUDE_ATTACKED_COUNT)
                    MobSkillUse(md, tick, ROWOW_MSC_RUDEATTACKED);
            }
            else if ((BattleConfig.MobAI & 0x2) == 0 && StatusCheckSkillUse(md, abl, 0, 0) == false)
            {
                // Can't attack back, but didn't invoke a rude attacked skill...
            }
            else
            {
                // Attackable: if a monster can change the target to the attacker, it will change the target
                mob->TargetID = mob->AttackedID; // set target
                mob->LootTargetID.Clear();
                fitem = nullptr;
                if (mob->AttackedCount > 0)
                    mob->AttackedCount--; // Should we reset rude attack count?
                tbl = abl; // Set the new target
            }
        }

        // Clear it since it's been checked for already.
        mob->AttackedID.Clear();
        mob->NormAttackedID.Clear();
    }

    // Processing of slave monster
    bool isSlaveLostTarget = false;
    if (mob->MasterID.IsEmpty() == false)
    {
        if (MobAISubHardSlaveMob(md, mob, tick) == true)
            return true;
        isSlaveLostTarget = true;
    }

    // Scan area for items to loot, avoid trying to loot if the mob is full and can't consume the items.
    if (canMove == true && (mode & ROWOW_MODE_LOOTER) != 0 && tick > GetSkillUnitData(md)->CanActTime &&
        (mob->LootItems.size() < ROWOW_LOOTITEM_SIZE || BattleConfig.MonsterLootType != 1))
    {
        if (tbl == nullptr && fitem == nullptr)
            fitem = MobLootSearch(md, mob, BattleConfig.LootRange); // Search for items in loot range
        else if (fitem != nullptr && BattleConfig.MonsterLootSearchType == 0)
        {
            // Looter already has a target item, but we want to check if there is an item that's closer
            int32 x = 0, y = 0;
            GetUnitCell(md, x, y);
            int32 dist = std::max(std::abs(fitem->X - x), std::abs(fitem->Y - y)) - 1;
            if (dist > 0)
                if (ROWOWFloorItem const* closerItem = MobLootSearch(md, mob, dist))
                    fitem = closerItem;
        }
    }

    // Scan area for targets
    if (((mode & ROWOW_MODE_AGGRESSIVE) != 0 && ((tbl == nullptr && fitem == nullptr) || isSlaveLostTarget == true)) || mob->SkillState == ROWOW_MSS_FOLLOW)
    {
        ObjectGuid prevID = mob->TargetID;
        int32 range2 = status->Range2;
        int32 range3 = status->Range3;
        MobForEachEnemyInRange(md, viewRange, [this, md, mob, &tbl, mode, range2, range3](Unit* bl)
        {
            MobAISubHardActiveSearch(md, mob, bl, tbl, mode, range2, range3);
        });

        // If a monster finds a new target that is already in attack range it immediately switches to rush mode.  This behavior overrides even
        // angry mode and other mode-specific behavior
        if (tbl != nullptr && prevID != mob->TargetID && BattleCheckRange(md, tbl, attackRange) == true)
        {
            mob->IsAggressive = false;
            MobSetState(md, mob, ROWOW_MSS_RUSH);
        }
    }
    else if ((mode & ROWOW_MODE_CHANGECHASE) != 0 && (mob->SkillState == ROWOW_MSS_RUSH || mob->SkillState == ROWOW_MSS_FOLLOW))
    {
        int32 searchSize = std::min(viewRange, attackRange);
        MobForEachEnemyInRange(md, searchSize, [this, md, mob, &tbl, attackRange](Unit* bl)
        {
            MobAISubHardChangeChase(md, mob, bl, tbl, attackRange);
        });
    }

    if (tbl != nullptr)
    {
        mob->LootTargetID.Clear(); // A unit took the item's place as its target
        fitem = nullptr;
    }
    if (tbl == nullptr && fitem == nullptr) // No targets available.
    {
        // Make sure target is unlocked
        MobUnlockTarget(md, tick);

        // Random walk
        if (mob->MasterID.IsEmpty() == true && mob->NextWalkTime <= tick && MobRandomWalk(md, tick) == false)
        {
            // Delay next random walk when this one failed
            if (mob->NextWalkTime < mob->CanMoveTime)
                mob->NextWalkTime = mob->CanMoveTime;
            else
                mob->NextWalkTime = tick + urand(0, 999);
        }
        return true;
    }

    // Target exists, attack or loot as applicable.
    if (fitem != nullptr)
        return MobAISubHardLoot(md, mob, fitem, mode, canMove, tick);

    // At this point we know the target is attackable, attempt to attack.  Normal attack / berserk skill is only used when target is in range
    if (BattleCheckRange(md, tbl, attackRange) == true)
    {
        bool isStop = true;

        // Hiding is a special case because it prevents normal attacks but allows skill usage
        if (sc == nullptr || (sc->Option & ROWOW_OPTION_HIDE) == 0)
        {
            // Target within range and potentially able to use normal attack, engage.  Only attack if no more attack delay left
            if (mob->AttackTargetID != tbl->GetGUID())
                isStop = UnitAttack(md, mob, tbl);
        }
        else
            MobSetState(md, mob, ROWOW_MSS_BERSERK); // These status changes pretend the attack was successful without attempting it

        // Stop and make sure there is no chase target when attack was not skipped
        if (isStop == true)
        {
            UnitStopWalking(md);
            mob->TargetToID.Clear();
        }
        return true; // Target still in attack range, no need to chase the target
    }

    // Only update target cell / drop target after having moved at least "mob_chase_refresh" cells
    if (mob->IsWalking == true && (canMove == false || UnitWalkPathPos(md, mob, tick) <= BattleConfig.MobChaseRefresh) && MobIsChasing(mob->SkillState) == true)
        return true;

    // Out of range
    bool isCantMove = sc != nullptr && sc->Cant.Move == true;
    if ((mode & ROWOW_MODE_CANMOVE) == 0 || (canMove == false && (isCantMove == true || mob->CanMoveTime > tick)))
    {
        // Can't chase.  Immobile and trapped mobs will use idle skills and unlock their target after a while.  Only switch mode if no more attack
        // delay left
        if (mob->AttackTargetID.IsEmpty() == true)
        {
            if (tick - mob->LastCanMove > uint64(BattleConfig.MobUnlockTime))
                MobUnlockTarget(md, tick); // Unlock target
            else
                MobSetState(md, mob, ROWOW_MSS_IDLE); // Set to use idle skill next interval but keep target for now
        }
        return true;
    }

    // Officially we can stop here if monster is still chasing its target.  If setting to update the chase path is set, we continue if current
    // target tile not within attack range.  In this case, we should also stop updating the chase path when target no longer in chase range.
    if (mob->IsWalking == true && mob->TargetToID == tbl->GetGUID() &&
        ((BattleConfig.MobAI & 0x1) == 0 || CheckDistanceBL(md, tbl, status->Range3) == false))
        return true;

    // Follow up if possible.  Monsters in Angry state only start chasing when target is in chase range
    if (mob->SkillState == ROWOW_MSS_ANGRY && MobCanReach(md, tbl, status->Range3) == false)
    {
        MobUnlockTarget(md, tick);
        return true;
    }
    if (UnitWalkToBL(md, mob, tbl, attackRange, true) == false)
        MobUnlockTarget(md, tick);
    return true;
}

void ROWOWMod::MobAISubLazy(Creature* md, ROWOWMobData* mob, uint64 tick)
{
    // mob_ai_sub_lazy: a monster with no player near.  (mob_active_time and boss_active_time keep the hard AI on for a while after one left: both
    // are off, as is monster_ai&0x20)
    MobCleanSpotted(md, mob);

    // Don't continue if the hard AI was executed recently (twice its interval to account for lag), or if skills cannot be used yet
    if (tick - mob->NextThinkTime < MIN_MOBTHINKTIME * 2)
        return;
    if (tick - mob->LastSkillCheck < MOB_SKILL_INTERVAL)
        return;
    if (mob->MasterID.IsEmpty() == false)
    {
        if (mob->SpottedLog.empty() == false)
            MobAISubHardSlaveMob(md, mob, tick);
        return;
    }

    // Remove target and set to idle state when movement was finished
    MobUnlockTarget(md, tick);
    ROWOWStatus const* status = GetROStatus(md);
    if (status == nullptr)
        return;
    bool isStatusImmune = (status->Mode & ROWOW_MODE_STATUS_IMMUNE) != 0;
    if (mob->NextWalkTime < tick && (status->Mode & ROWOW_MODE_CANMOVE) != 0 && UnitCanMove(md, mob, tick) == true)
    {
        // Move probability for mobs away from players.  In Aegis, this is 100% for mobs that have been activated by players and none otherwise.
        if (mob->SpottedLog.empty() == false && int32(urand(0, 99)) < (isStatusImmune == true ? BattleConfig.BossNoPCMoveRate : BattleConfig.MobNoPCMoveRate))
            MobRandomWalk(md, tick);
    }
    else if (mob->IsWalking == false)
    {
        // Probability for mobs far from players from doing their IDLE skill.  In Aegis, this is 100% for mobs that have been activated by players
        if (mob->SpottedLog.empty() == false && int32(urand(0, 99)) < (isStatusImmune == true ? BattleConfig.BossNoPCIdleSkillRate : BattleConfig.MobNoPCIdleSkillRate))
            MobSkillUse(md, tick, MOBSKILL_EVENT_NONE);
    }
}

bool ROWOWMod::MobIsNearPC(Creature* md, ROWOWMobData* mob) const
{
    // mob_ai_sub_foreachclient: whether a player is within AREA_SIZE + ACTIVE_AI_RANGE, spotting it
    bool isNearPC = false;
    Map::PlayerList const& players = md->GetMap()->GetPlayers();
    for (Map::PlayerList::const_iterator playerIter = players.begin(); playerIter != players.end(); ++playerIter)
    {
        Player* sd = playerIter->GetSource();
        if (sd == nullptr || sd->IsInWorld() == false || CheckDistanceBL(sd, md, BattleConfig.AreaSize + ACTIVE_AI_RANGE) == false)
            continue;
        MobAddSpotted(mob, sd->GetGUID());
        isNearPC = true;
    }
    return isNearPC;
}

void ROWOWMod::MobSetAttackedID(Creature* md, Unit* src, uint64 tick, bool isNormAttacked)
{
    // mob_set_attacked_id: retaliate against the attacker, or the master of a monster that hit it (retaliate_to_master).  As it was attacked, the
    // monster leaves aggressive mode, and the AI runs at once
    ROWOWMobData* mob = GetMobData(md);
    ObjectGuid attackedID = src->GetGUID();
    if (src->IsCreature() == true && BattleConfig.RetaliateToMaster != 0)
        if (Unit* master = MobGetMaster(src->ToCreature()))
            attackedID = master->GetGUID();
    mob->AttackedID = attackedID;
    if (isNormAttacked == true)
        mob->NormAttackedID = attackedID;
    mob->IsAggressive = false;
    MobAISubHard(md, tick);
}

int64 ROWOWMod::BattleCalcWalkDelay(Unit const* bl, int64 damage, int16 div) const
{
    // battle_calc_walkdelay: the target's damage motion, more for a skill of many hits, at the walk delay rate
    if (damage < 1 || div == 0)
        return 0;
    ROWOWStatus const* status = GetROStatus(bl);
    if (status == nullptr)
        return 0;
    int64 delay = status->DamageMotion;
    if (div > 1) // Multi-hit skills mean higher delays
    {
        if (div > 10)
            div = 2; // If there are more than 10 hits, it calculates as 2 hits
        delay += BattleConfig.MultihitDelay * (div - 1);
    }
    delay = delay * (bl->IsPlayer() == true ? BattleConfig.PCWalkDelayRate : BattleConfig.WalkDelayRate) / 100;
    return delay > 0 ? delay : 1; // Return 1 to specify there should be no noticeable delay, but you should stop walking.
}

void ROWOWMod::MobBattleAttacked(Unit* src, Unit* target, int64 damage, int16 div, uint64 tick, int32 amotion, bool isNormAttacked, int32 attackType, uint16 skillID)
{
    // battle_damage's end, when an attack connects: the monster stops (status_damage's unit_set_walkdelay, damage_walk_delay being off) and the
    // attacked event comes at the end of the walk delay (mob_attacked, mob_norm_attacked), after the attack motion the damage waits for
    Creature* md = target != nullptr ? target->ToCreature() : nullptr;
    if (md == nullptr || src == nullptr || GetROMobID(md) == 0 || md->IsAlive() == false)
        return;
    // Trigger monster skill conditions
    if (src != target)
    {
        if (damage > 0)
            MobSkillEvent(md, src, tick, attackType, damage);
        if (skillID > 0 && md->IsAlive() == true)
            MobSkillEvent(md, src, tick, ROWOW_MSC_SKILLUSED | (int32(skillID) << 16), damage);
        if (md->IsAlive() == false)
            return;
    }
    ROWOWMobData* mob = GetMobData(md);
    int64 delay = BattleCalcWalkDelay(target, damage, div);
    if (delay > 0)
        UnitSetWalkDelay(md, mob, tick, delay);
    ROWOWMobAttackedEvent attackedEvent;
    attackedEvent.Tick = tick + uint64(std::max(0, amotion)) + uint64(delay);
    attackedEvent.SrcGUID = src->GetGUID();
    attackedEvent.IsNormAttacked = isNormAttacked;
    mob->AttackedEvents.push_back(attackedEvent);
}

void ROWOWMod::MobReleaseCombat(Creature* md, ROWOWMobData* mob)
{
    // RO has no combat state: a monster with no target, that nothing is swinging at, leaves WoW's.  Nothing else of an evade: it keeps its HP, its
    // statuses and its damage log, and stays where it stands
    if (mob->TargetID.IsEmpty() == false || mob->AttackedEvents.empty() == false || md->getAttackers().empty() == false)
        return;
    if (md->IsInCombat() == false && md->IsEngaged() == false)
        return;
    md->AttackStop();
    md->GetCombatManager().EndAllPvECombat();
    md->SetLootRecipient(nullptr);
    md->ResetPlayerDamageReq();
    if (md->IsEngaged() == true && md->AI() != nullptr)
        md->AI()->EngagementOver();
}

void ROWOWMod::MobSyncVictim(Creature* md, ROWOWMobData* mob)
{
    // The WoW victim follows the RO target, for what the client shows and for the swing
    if (mob->TargetID.IsEmpty() == true)
    {
        if (md->GetVictim() != nullptr)
            md->AttackStop();
        return;
    }
    if (md->GetVictim() != nullptr && md->GetVictim()->GetGUID() == mob->TargetID)
    {
        md->UpdateLeashExtensionTime(); // RO has no leash: mob_ai_sub_hard is what drops a target
        return;
    }
    if (Unit* target = ObjectAccessor::GetUnit(*md, mob->TargetID))
    {
        // No WoW call for help: RO's is the link (mob_linksearch)
        md->SetNoCallAssistance(true);
        md->Attack(target, true);
        md->SetAssistanceTimer(0);
    }
}

void ROWOWMod::MobUpdate(Creature* md, uint32 diff)
{
    // The monster's update: its attacked events, then its AI each MIN_MOBTHINKTIME (mob_ai_hard) near a player and ten times slower otherwise
    // (mob_ai_lazy), and its attack, which the timer of unit_attack_timer runs on the swing's own time
    ROWOWMobData* mob = GetMobData(md);
    uint64 tick = GetTick();
    if (mob->IsWalking == true && md->GetMotionMaster()->HasMovementGeneratorType(ESCORT_MOTION_TYPE) == false)
    {
        mob->IsWalking = false;
        mob->IsCanEscape = false; // Remove any possible escape states present for mobs once they stopped moving.
    }
    for (size_t eventIndex = 0; eventIndex < mob->AttackedEvents.size();)
    {
        if (mob->AttackedEvents[eventIndex].Tick > tick)
        {
            ++eventIndex;
            continue;
        }
        ROWOWMobAttackedEvent attackedEvent = mob->AttackedEvents[eventIndex];
        mob->AttackedEvents.erase(mob->AttackedEvents.begin() + eventIndex);
        if (Unit* src = ObjectAccessor::GetUnit(*md, attackedEvent.SrcGUID))
            MobSetAttackedID(md, src, tick, attackedEvent.IsNormAttacked);
        if (md->IsAlive() == false)
            return;
    }

    // mob_spawn's and mob_revive's skills
    if (mob->IsSpawnSkillPending == true)
    {
        mob->IsSpawnSkillPending = false;
        MobSkillUse(md, tick, ROWOW_MSC_SPAWN);
        if (md->IsAlive() == false)
            return;
    }

    bool isThink = false;
    if (mob->ThinkTimer <= diff)
    {
        mob->ThinkTimer = uint32(MIN_MOBTHINKTIME) - std::min<uint32>(diff - mob->ThinkTimer, uint32(MIN_MOBTHINKTIME) - 1);
        isThink = true;
    }
    else
        mob->ThinkTimer -= diff;
    if (isThink == true)
    {
        if (MobIsNearPC(md, mob) == true)
        {
            UnitUpdateChase(md, mob, tick);

            // unit_walktoxy_nextcell: walk and chase skills.  Monsters use them every second, but RO only gets there every "speed" ms: to check one skill per
            // second on average it takes off half the speed.  A skill used stops the walk but for NPC_SPEEDUP's trick, the emotions and the walk state's
            // own (MobSkillUse leaves it going; a cast with a cast time holds it anyway)
            ROWOWStatus const* status = GetROStatus(md);
            if (mob->IsWalking == true && status != nullptr && tick - mob->LastSkillCheck > MOB_SKILL_INTERVAL - uint64(status->Speed) / 2 && MobSkillUse(md, tick, MOBSKILL_EVENT_NONE, 0, true) == true)
            {
                ROWOWSkillUnitData const* ud = md->CustomData.Get<ROWOWSkillUnitData>("ROWOWSkillUnitData");
                uint16 skillID = ud != nullptr && mob->SkillIdx >= 0 ? ud->SkillID : mob->AfterSkillID;
                if ((skillID != ROWOW_SKILL_NPC_SPEEDUP || mob->TrickCasting == 0) && skillID != ROWOW_SKILL_NPC_EMOTION && skillID != ROWOW_SKILL_NPC_EMOTION_ON && mob->SkillState != ROWOW_MSS_WALK)
                    UnitStopWalking(md);
            }
            if (md->IsAlive() == false)
                return;
            if (MobAISubHard(md, tick) == true)
                mob->LastPCNearTime = tick;
        }
        else
        {
            if (mob->LazyTimer <= uint32(MIN_MOBTHINKTIME))
            {
                mob->LazyTimer = uint32(MIN_MOBTHINKTIME * 10);
                MobAISubLazy(md, mob, tick);
            }
            else
                mob->LazyTimer -= uint32(MIN_MOBTHINKTIME);
            UnitUpdateChase(md, mob, tick);
        }
        if (md->IsAlive() == false)
            return;
        MobReleaseCombat(md, mob);
    }
    MobSyncVictim(md, mob);
    UnitAttackTimerSub(md, mob, tick);
}

// Respawn

void ROWOWMod::LoadMobSpawnData()
{
    MobSpawnLines.clear();
    CreatureSpawnsBySpawnID.clear();
    if (IsEnabled == false)
        return;
    ROWOWDataTable const* spawnTable = GetDataTable("mod_rowow_db_mob_spawn");
    if (spawnTable == nullptr)
        return;

    // Each line as npc_parse_mob leaves it
    int32 xColumnIndex = spawnTable->GetColumnIndex("X");
    int32 yColumnIndex = spawnTable->GetColumnIndex("Y");
    int32 xSpanColumnIndex = spawnTable->GetColumnIndex("XSpan");
    int32 ySpanColumnIndex = spawnTable->GetColumnIndex("YSpan");
    int32 delayColumnIndex = spawnTable->GetColumnIndex("RespawnDelayMs");
    int32 varianceColumnIndex = spawnTable->GetColumnIndex("RespawnVarianceMs");
    int32 isBossColumnIndex = spawnTable->GetColumnIndex("IsBoss");
    int32 nameColumnIndex = spawnTable->GetColumnIndex("DisplayName");
    int32 mobIDColumnIndex = spawnTable->GetColumnIndex("MobId");
    int32 mapColumnIndex = spawnTable->GetColumnIndex("MapName");
    ROWOWDataTable const* mobTable = GetDataTable(ROWOW_DATA_TABLE_MOB);
    MobSpawnLines.reserve(spawnTable->Rows.size());
    for (size_t rowIndex = 0; rowIndex < spawnTable->Rows.size(); rowIndex++)
    {
        ROWOWMobSpawnLine line;
        line.X = int32(spawnTable->GetInt(rowIndex, xColumnIndex, 0));
        line.Y = int32(spawnTable->GetInt(rowIndex, yColumnIndex, 0));
        line.XS = std::max<int32>(0, int32(spawnTable->GetInt(rowIndex, xSpanColumnIndex, 0)));
        line.YS = std::max<int32>(0, int32(spawnTable->GetInt(rowIndex, ySpanColumnIndex, 0)));
        line.IsBoss = spawnTable->GetInt(rowIndex, isBossColumnIndex, 0) != 0;

        // mob_parse_dataset: "--en--" is mob_db's name and "--ja--" its JapaneseName, anything else the line's own name, which the
        // client is shown.  Only a name that differs from mob_db's (the creature template's) needs showing
        int64 mobRowIndex = GetMobRowIndex(uint32(spawnTable->GetInt(rowIndex, mobIDColumnIndex, 0)));
        if (nameColumnIndex >= 0 && mobTable != nullptr && mobRowIndex >= 0)
        {
            std::string name = spawnTable->GetCell(rowIndex, nameColumnIndex);
            if (name == "--ja--")
                name = mobTable->GetCell(size_t(mobRowIndex), mobTable->GetColumnIndex("JapaneseName"));
            if (name != "--en--" && name.empty() == false && name != mobTable->GetCell(size_t(mobRowIndex), mobTable->GetColumnIndex("Name")))
                line.Name = name;
        }

        // A line with no delay is 5 seconds, as is a delay under mob_respawn_time
        int64 delay = spawnTable->GetInt(rowIndex, delayColumnIndex, 0);
        line.Delay1 = delay == 0 || delay < BattleConfig.MobRespawnTime ? 5000 : uint32(delay);
        line.Delay2 = uint32(std::max<int64>(0, spawnTable->GetInt(rowIndex, varianceColumnIndex, 0)));

        // Fixed X or Y coordinate.  Need to set this to 1 as we reduce it by 1 when calling the search function
        if (line.XS == 0 && line.X > 0)
            line.XS = 1;
        if (line.YS == 0 && line.Y > 0)
            line.YS = 1;

        // Check if monsters should have variance applied to their respawn time
        if (((BattleConfig.MobSpawnVariance & 1) == 0 && line.IsBoss == true) || ((BattleConfig.MobSpawnVariance & 2) == 0 && line.IsBoss == false))
            line.Delay2 = 0; // Remove the variance

        // A line of a map stitched into a bigger one keeps to its part of it (its monsters' centers are the stitched map's cells already)
        ROWOWMapPart const* part = mapColumnIndex >= 0 ? GetMapPart(spawnTable->GetCell(rowIndex, mapColumnIndex)) : nullptr;
        if (part != nullptr && part->HasArea() == true)
        {
            line.AreaX = part->CellOffsetX;
            line.AreaY = part->CellOffsetY;
            line.AreaWidth = part->CellWidth;
            line.AreaHeight = part->CellHeight;
        }
        MobSpawnLines.push_back(line);
    }

    QueryResult tableResult = WorldDatabase.Query("SELECT 1 FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = 'mod_rowow_creature_spawn'");
    if (!tableResult)
        return;
    QueryResult result = WorldDatabase.Query("SELECT `CreatureGUID`, `SpawnLine`, `CenterX`, `CenterY` FROM `mod_rowow_creature_spawn`");
    if (!result)
        return;
    CreatureSpawnsBySpawnID.reserve(size_t(result->GetRowCount()));
    do
    {
        Field* fields = result->Fetch();
        uint32 spawnLine = fields[1].Get<uint32>();
        if (spawnLine >= MobSpawnLines.size())
        {
            LOG_ERROR("module.ROWOW", "ROWOWMod::LoadMobSpawnData: creature {} has spawn line {} of {}", fields[0].Get<uint32>(), spawnLine, MobSpawnLines.size());
            continue;
        }
        ROWOWCreatureSpawn& spawn = CreatureSpawnsBySpawnID[fields[0].Get<uint32>()];
        spawn.Line = &MobSpawnLines[spawnLine];
        spawn.CenterX = fields[2].Get<int32>();
        spawn.CenterY = fields[3].Get<int32>();
    } while (result->NextRow());
    LOG_INFO("module.ROWOW", "ROWOWMod::LoadMobSpawnData loaded {} spawn lines and the spawns of {} monsters", MobSpawnLines.size(), CreatureSpawnsBySpawnID.size());
}

ROWOWCreatureSpawn const* ROWOWMod::GetCreatureSpawn(Creature const* md) const
{
    auto spawnIter = CreatureSpawnsBySpawnID.find(md->GetSpawnId());
    if (spawnIter == CreatureSpawnsBySpawnID.end())
        return nullptr;
    return &spawnIter->second;
}

int64 ROWOWMod::MobSpawnTimeOf(Creature const* md, ROWOWMobSpawnLine const& line) const
{
    // mob_setdelayspawn's time: delay1 and the random variance, the spawn delay fix, and never faster than a second
    uint32 spawntime = line.Delay1; // Base respawn time
    if (line.Delay2 != 0)
        spawntime += urand(0, line.Delay2 - 1); // random variance
    ROWOWStatus const* status = GetROBaseStatus(md);
    uint32 mode = status != nullptr ? status->Mode : 0;

    // Divide by 100 first to prevent overflows (precision loss is minimal as duration is in ms already)
    if ((mode & ROWOW_MODE_STATUS_IMMUNE) != 0) // Status Immune
    {
        if (BattleConfig.BossSpawnDelay != 100)
            spawntime = spawntime / 100 * uint32(BattleConfig.BossSpawnDelay);
    }
    else if ((mode & (ROWOW_MODE_IGNORE_MELEE | ROWOW_MODE_IGNORE_MAGIC | ROWOW_MODE_IGNORE_RANGED | ROWOW_MODE_IGNORE_MISC)) != 0) // Plant type
    {
        if (BattleConfig.PlantSpawnDelay != 100)
            spawntime = spawntime / 100 * uint32(BattleConfig.PlantSpawnDelay);
    }
    else if (BattleConfig.MobSpawnDelay != 100) // Normal mobs
        spawntime = spawntime / 100 * uint32(BattleConfig.MobSpawnDelay);
    return int64(std::max<uint32>(1000, spawntime)); // Monsters should never respawn faster than 1 second
}

void ROWOWMod::MobSetDelaySpawn(Creature* md)
{
    // mob_setdelayspawn, at the monster's death.  The core's clock counts whole seconds, so it's set to the second before the time, and MobSpawn
    // (CanRespawn) holds the respawn until the time itself.  A corpse goes after ROWOW.Creature.CorpseTimeInMS, as RO's body fades (UpdateMobCorpse): the
    // core's whole seconds are only the time it would go by itself
    ROWOWCreatureSpawn const* spawn = GetCreatureSpawn(md);
    if (spawn == nullptr)
        return; // Doesn't has respawn data!
    uint64 tick = GetTick();
    int64 spawntime = MobSpawnTimeOf(md, *spawn->Line);
    ROWOWMobData* mob = GetMobData(md);
    mob->SpawnTime = tick + uint64(spawntime);
    mob->LastSpawnTime = uint32(spawntime);
    mob->DeadTime = tick;
    md->SetRespawnTime(uint32(spawntime / 1000));
    if (md->GetCreatureTemplate()->rank >= CREATURE_ELITE_ELITE)
        md->SaveRespawnTime(); // The core saved its own time at death
    md->SetCorpseRemoveTime((ConfigCorpseTimeInMS + 999) / 1000);
}

void ROWOWMod::UpdateMobCorpse(Creature* md)
{
    // The body goes once it faded, to the millisecond
    if (md->getDeathState() != DeathState::Corpse || GetROMobID(md) == 0)
        return;
    ROWOWMobData const* mob = md->CustomData.Get<ROWOWMobData>("ROWOWMobData");
    if (mob == nullptr || mob->DeadTime == 0 || GetTick() < mob->DeadTime + uint64(ConfigCorpseTimeInMS))
        return;
    md->RemoveCorpse(false);
}

bool ROWOWMod::MapSearchFreeCellOfSpawn(ROWOWMapGrid const& mapGrid, Map* map, int32 baseX, int32 baseY, int32 rx, int32 ry, int32 tries, int32& xOut, int32& yOut) const
{
    // map_search_freecell with no unit (flag 1): a random reachable cell within rx,ry of the base (below 0 the whole map, less map_edge_size), never
    // the base itself nor within 5 cells of the map's edge
    if (rx == 0 && ry == 0)
    {
        xOut = baseX;
        yOut = baseY;
        return MapGetCellNoPass(map, mapGrid, baseX, baseY) == false; // No range? Return the target cell then....
    }
    int32 edge = BattleConfig.MapEdgeSize;
    int32 edgeValid = std::min(edge, 5);
    int32 edgeX = std::min(edge, (mapGrid.Width - 1) / 2); // A map too small for the edge has none
    int32 edgeY = std::min(edge, (mapGrid.Height - 1) / 2);
    while (tries-- > 0)
    {
        int32 x = rx >= 0 ? irand(baseX - rx, baseX + rx) : irand(edgeX, mapGrid.Width - edgeX - 1);
        int32 y = ry >= 0 ? irand(baseY - ry, baseY + ry) : irand(edgeY, mapGrid.Height - edgeY - 1);
        if (x == baseX && y == baseY)
            continue; // Avoid picking the same target tile.

        // Cells outside the map or within 4-5 cells of the map edge are considered invalid officially
        if (x < edgeValid || x > mapGrid.Width - edgeValid || y < edgeValid || y > mapGrid.Height - edgeValid)
            continue;
        if (MapGetCellNoPass(map, mapGrid, x, y) == false)
        {
            xOut = x;
            yOut = y;
            return true;
        }
    }
    return false;
}

bool ROWOWMod::MobSpawn(Creature* md)
{
    // mob_spawn's placement, from CanRespawn: false holds the respawn, and the core asks again each update.  Not before the monster's time; then a
    // new cell by its line's rules, or if there is none the same again after mob_respawn_time.  With the cell the monster is put there (it's dead,
    // and nothing sees it), so the core's respawn brings it back on it
    ROWOWMobData* mob = GetMobData(md);
    uint64 tick = GetTick();
    if (mob->SpawnTime != 0 && tick < mob->SpawnTime)
        return false;
    ROWOWCreatureSpawn const* spawn = GetCreatureSpawn(md);
    ROWOWMapGrid const* mapGrid = GetMapGrid(md->GetMapId());
    if (spawn == nullptr || mapGrid == nullptr || mapGrid->HasCells() == false)
        return true;
    Map* map = md->GetMap();
    ROWOWMobSpawnLine const& line = *spawn->Line;
    int32 x = spawn->CenterX;
    int32 y = spawn->CenterY;
    int32 xs = x < 0 ? 0 : line.XS; // The line of a monster with no center is anywhere on the map (-1, span 0)
    int32 ys = y < 0 ? 0 : line.YS;

    // ...or, for a map stitched into a bigger one, anywhere on its part: the part's middle, spanning the part
    int32 anywhereRX = -1;
    int32 anywhereRY = -1;
    if (line.AreaWidth > 0)
    {
        anywhereRX = line.AreaWidth / 2;
        anywhereRY = line.AreaHeight / 2;
        if (x < 0)
        {
            x = line.AreaX + anywhereRX;
            y = line.AreaY + anywhereRY;
            xs = anywhereRX + 1; // The search goes xs - 1 to each side
            ys = anywhereRY + 1;
        }
    }

    // Search can be skipped for boss monster spawns if spawn location is fixed.  We can't skip normal monsters as they should pick a random location
    // if the cell is blocked (e.g. Icewall).  The center cell has a 1/(xs*ys) chance to be picked if free
    bool isCenterFree = MapGetCellNoPass(map, *mapGrid, x, y) == false;
    if ((line.IsBoss == false || (x == 0 && y == 0) || xs != 1 || ys != 1) && (xs + ys < 1 || urand(1, uint32(std::max(1, xs * ys))) != 1 || isCenterFree == false))
    {
        // Officially the area is split into 4 squares, 4 lines and 1 dot and for each of those there is one attempt.  We simplify this to trying
        // 8 times in the whole area and then at the center cell even though that's not fully accurate
        if (MapSearchFreeCellOfSpawn(*mapGrid, map, x, y, xs - 1, ys - 1, 8, x, y) == false)
        {
            // If area search failed and center cell not reachable, try to spawn the monster anywhere on the map (50 tries)
            if (line.AreaWidth > 0)
            {
                x = line.AreaX + anywhereRX;
                y = line.AreaY + anywhereRY;
            }
            else
            {
                x = spawn->CenterX;
                y = spawn->CenterY;
            }
            if (isCenterFree == false && MapSearchFreeCellOfSpawn(*mapGrid, map, x, y, anywhereRX, anywhereRY, 50, x, y) == false)
            {
                mob->SpawnTime = tick + uint64(BattleConfig.MobRespawnTime); // Retry again later
                return false;
            }
        }
    }
    float worldX = mapGrid->GetCellCenterWorldX(y);
    float worldY = mapGrid->GetCellCenterWorldY(x);
    if (map->IsGridLoaded(worldX, worldY) == false)
        return true; // The core's own spot: a corpse can't be put on a grid nobody is on
    float worldZ = mapGrid->GetCellGroundZ(x, y);
    float groundZ = 0;
    if (GetGroundZ(md, worldX, worldY, worldZ, groundZ) == true)
        worldZ = groundZ;
    float orientation = frand(0.0f, float(M_PI) * 2.0f);
    md->SetHomePosition(worldX, worldY, worldZ, orientation);
    md->SetPosition(worldX, worldY, worldZ, orientation);
    return true;
}

bool ROWOWMod::MobCanRespawn(Creature* md)
{
    if (GetROMobID(md) == 0)
        return true;

    // The body goes first, if the respawn got here before it did (the core's clock gets here a little early)
    ROWOWMobData* mob = GetMobData(md);
    if (mob->SpawnTime != 0 && GetTick() < mob->SpawnTime)
        return false;
    if (md->getDeathState() == DeathState::Corpse)
        md->RemoveCorpse(false);
    if (MobSpawn(md) == false)
        return false;
    if (mob->DeadTime != 0)
        mob->LastSpawnDelay = uint32(GetTick() - mob->DeadTime);
    return true;
}

bool ROWOWMod::WriteMobDump(std::string const& filePath) const
{
    // What LoadBattleData and LoadMobSpawnData left, for comparing with the oracle's @oraclemobs
    FILE* file = fopen(filePath.c_str(), "w");
    ROWOWDataTable const* spawnTable = GetDataTable("mod_rowow_db_mob_spawn");
    if (file == nullptr || spawnTable == nullptr)
        return false;
    for (auto const& statusByMobID : MobStatusesByMobID)
        fprintf(file, "mob %u mode=%x range=%d range2=%d range3=%d\n", statusByMobID.first, statusByMobID.second.Mode, statusByMobID.second.Range,
            statusByMobID.second.Range2, statusByMobID.second.Range3);
    int32 mapColumnIndex = spawnTable->GetColumnIndex("MapName");
    int32 mobColumnIndex = spawnTable->GetColumnIndex("MobId");
    int32 amountColumnIndex = spawnTable->GetColumnIndex("Amount");
    for (size_t lineIndex = 0; lineIndex < MobSpawnLines.size(); lineIndex++)
    {
        ROWOWMobSpawnLine const& line = MobSpawnLines[lineIndex];
        fprintf(file, "spawn %s %d x=%d y=%d xs=%d ys=%d num=%d delay1=%u delay2=%u boss=%d\n", spawnTable->GetCell(lineIndex, mapColumnIndex).c_str(),
            int32(spawnTable->GetInt(lineIndex, mobColumnIndex, 0)), line.X, line.Y, line.XS, line.YS, int32(spawnTable->GetInt(lineIndex, amountColumnIndex, 0)),
            line.Delay1, line.Delay2, line.IsBoss == true ? 1 : 0);
    }
    fclose(file);
    return true;
}
