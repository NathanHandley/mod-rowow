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
//
//  Ported from rAthena (https://github.com/rathena/rathena), src/map/skill.cpp and pc.cpp, pre-renewal paths:
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL.  For more information, see LICENCE in rAthena's main folder

#include "Item.h"
#include "Log.h"
#include "Player.h"
#include "Unit.h"

#include "ROWOW.h"
#include "ROWOW_Script.h"

using namespace std;

// Autospells and autobonuses, the item bonuses that act on hits: bAutoSpell casts a skill as the player attacks (skill_additional_effect),
// bAutoSpellWhenHit as it's hit (skill_counter_additional_effect), bAutoSpellOnSkill as it uses a skill (skill_onskillusage); autobonus,
// autobonus2 and autobonus3 run a bonus script for a while on the same triggers (pc_exeautobonus), which the status calculation then counts
// as the items' (pc_delautobonus's restore).  The lists are rebuilt by each status calculation from the worn items; a running autobonus is
// the identity's.  All of it runs in the map thread of the player and its target

namespace
{
    // The trigger's attack type, range and kind all match the attack (BF_WEAPONMASK, BF_RANGEMASK and BF_SKILLMASK)
    bool IsAutoTriggered(int32 battleFlag, int32 attackType)
    {
        int32 matched = battleFlag & attackType;
        return (matched & (ROWOW_BF_WEAPON | ROWOW_BF_MAGIC | ROWOW_BF_MISC)) != 0 && (matched & (ROWOW_BF_SHORT | ROWOW_BF_LONG)) != 0 &&
            (matched & (ROWOW_BF_SKILL | ROWOW_BF_NORMAL)) != 0;
    }

    // skill_isNotOk, the part an autospell meets: the map's rules and the skill's cooldown
    bool IsSkillNotOk(Unit* unit, uint16 skillID)
    {
        if (Player* sd = unit->ToPlayer(); sd != nullptr && ROWOW->SkillIsNotOk(sd, skillID) == true)
            return true;
        ROWOWSkillUnitData const* ud = ROWOW->GetSkillUnitData(unit);
        auto cooldownIter = ud->CooldownEndTimesBySkillID.find(skillID);
        return cooldownIter != ud->CooldownEndTimesBySkillID.end() && cooldownIter->second > ROWOWMod::GetTick();
    }
}

bool ROWOWMod::SkillAutoCast(Unit* src, Unit* target, uint16 skillID, uint16 skillLevel, uint64 tick)
{
    // The autospell's cast: sd->state.autocast while its costs are paid (no SP: skill_consume_requirement's autocast) and it goes off through
    // the castend dispatch, then the caster's can-act delay is pushed to the skill's after-cast delay
    Player* sd = src->ToPlayer();
    ROWOWSkillUnitData* ud = GetSkillUnitData(src);
    uint8 type = SkillGetCastType(skillID);
    int32 x = 0, y = 0;
    GetUnitCell(target, x, y);
    if (type == ROWOW_CAST_GROUND && SkillPosMaxcountCheck(src, x, y, skillID, skillLevel, false) == false)
        return false;
    if (BattleConfig.AutospellCheckRange != 0 && DistanceBL(src, target) > SkillGetRange2(src, skillID, skillLevel, true))
        return false;
    if (skillID == ROWOW_SKILL_PF_SPIDERWEB) // Special case, due to its nature of coding.
        type = ROWOW_CAST_GROUND;
    else if (skillID == ROWOW_SKILL_AS_SONICBLOW) // Special case, Sonic Blow autospell should stop the player attacking.
        src->AttackStop();
    bool wasAutoCast = ud->IsAutoCast;
    ud->IsAutoCast = true;
    if (sd != nullptr)
        SkillConsumeRequirement(sd, skillID, skillLevel, 1);
    switch (type)
    {
        case ROWOW_CAST_GROUND:
            ud->SkillZ = target->GetPositionZ();
            SkillCastendPos2(src, x, y, skillID, skillLevel, tick, 0);
            break;
        case ROWOW_CAST_NODAMAGE:
            SkillCastendNoDamageID(src, target, skillID, skillLevel, tick, 0);
            break;
        case ROWOW_CAST_DAMAGE:
            SkillCastendDamageID(src, target, skillID, skillLevel, tick, 0);
            break;
    }
    ud->IsAutoCast = wasAutoCast;
    return true;
}

void ROWOWMod::SkillAutoSpellAdditionalEffect(Unit* src, Unit* bl, uint16 /*skillID*/, int32 attackType, uint64 tick)
{
    // skill_additional_effect's "Autospell when attacking" (bAutoSpell): a ranged attacker's chances are halved
    Player* sd = src->ToPlayer();
    ROWOWStatus const* sstatus = GetROStatus(src);
    if (sd == nullptr || sstatus == nullptr || bl->IsAlive() == false || sstatus->Bonus.AutoSpell.empty() == true)
        return;
    std::vector<ROWOWAutoSpell> autoSpells = sstatus->Bonus.AutoSpell; // A cast may recalculate the status
    for (ROWOWAutoSpell const& autoSpell : autoSpells)
    {
        if (IsAutoTriggered(autoSpell.BattleFlag, attackType) == false)
            continue; // one or more trigger conditions were not fulfilled
        uint16 skill = autoSpell.SkillID;
        if (GetSkill(skill) == nullptr || IsSkillNotOk(src, skill) == true)
            continue;
        uint16 skillLevel = autoSpell.Level != 0 ? autoSpell.Level : 1;
        if ((autoSpell.Flag & ROWOW_AUTOSPELL_FORCE_RANDOM_LEVEL) != 0)
            skillLevel = uint16(urand(1, skillLevel));
        int32 rate = sstatus->IsArrowAttack == false ? autoSpell.Rate : autoSpell.Rate / 2;
        if (int32(urand(0, 999)) >= rate)
            continue;
        Unit* tbl = (autoSpell.Flag & ROWOW_AUTOSPELL_FORCE_TARGET) != 0 ? bl : src;
        if (SkillAutoCast(src, tbl, skill, skillLevel, tick) == false)
            continue;
        // Set canact delay. [Skotlex]
        ROWOWSkillUnitData* ud = GetSkillUnitData(src);
        int32 delay = SkillDelayFix(src, skill, skillLevel);
        if (ud->CanActTime < tick + uint64(delay))
        {
            ud->CanActTime = std::max<uint64>(tick + uint64(delay), ud->CanActTime);
            SendSkillDelayToClient(src, skill);
        }
    }
}

void ROWOWMod::SkillAutoSpellCounterAdditionalEffect(Unit* src, Unit* bl, uint16 skillID, int32 attackType, uint64 tick)
{
    // skill_counter_additional_effect's autospells and autobonuses: the attacked player's counter-spells (bAutoSpellWhenHit, halved against
    // ranged weapon attacks) and autobonus2, and the attacker's autobonus
    ROWOWSkill const* skill = GetSkill(skillID);
    bool isNoDamage = skillID != 0 && skill != nullptr && skill->NK[ROWOW_NK_NODAMAGE] == true;
    Player* dstsd = bl->ToPlayer();
    ROWOWStatus const* dstatus = dstsd != nullptr ? GetROStatus(dstsd) : nullptr;
    // Trigger counter-spells to retaliate against damage causing skills.
    if (dstatus != nullptr && bl->IsAlive() == true && dstatus->Bonus.AutoSpell2.empty() == false && isNoDamage == false)
    {
        std::vector<ROWOWAutoSpell> autoSpells = dstatus->Bonus.AutoSpell2;
        for (ROWOWAutoSpell const& autoSpell : autoSpells)
        {
            if (IsAutoTriggered(autoSpell.BattleFlag, attackType) == false)
                continue; // one or more trigger conditions were not fulfilled
            uint16 autoSkillID = autoSpell.SkillID;
            uint16 autoSkillLevel = autoSpell.Level != 0 ? autoSpell.Level : 1;
            if ((autoSpell.Flag & ROWOW_AUTOSPELL_FORCE_RANDOM_LEVEL) != 0)
                autoSkillLevel = uint16(urand(1, autoSkillLevel));
            int32 autoRate = autoSpell.Rate;
            // Physical range attacks only trigger autospells half of the time
            if ((attackType & (ROWOW_BF_WEAPON | ROWOW_BF_LONG)) == (ROWOW_BF_WEAPON | ROWOW_BF_LONG))
                autoRate /= 2;
            if (GetSkill(autoSkillID) == nullptr || IsSkillNotOk(bl, autoSkillID) == true)
                continue;
            if (int32(urand(0, 999)) >= autoRate)
                continue;
            Unit* tbl = (autoSpell.Flag & ROWOW_AUTOSPELL_FORCE_TARGET) != 0 ? src : bl;
            if (SkillAutoCast(bl, tbl, autoSkillID, autoSkillLevel, tick) == false)
                continue;
            // Set canact delay. [Skotlex]
            ROWOWSkillUnitData* ud = GetSkillUnitData(bl);
            int32 delay = SkillDelayFix(bl, autoSkillID, autoSkillLevel);
            if (ud->CanActTime < tick + uint64(delay))
            {
                ud->CanActTime = std::max<uint64>(tick + uint64(delay), ud->CanActTime);
                SendSkillDelayToClient(bl, autoSkillID);
            }
        }
    }

    // Check for player autobonuses when attacking
    if (Player* sd = src->ToPlayer())
    {
        if (ROWOWStatus const* sstatus = GetROStatus(sd))
        {
            std::vector<ROWOWAutoBonus> autoBonuses = sstatus->Bonus.AutoBonus;
            for (ROWOWAutoBonus const& autoBonus : autoBonuses)
            {
                if (int32(urand(0, 1000)) >= autoBonus.Rate)
                    continue;
                if (IsAutoTriggered(autoBonus.AtkType, attackType) == false)
                    continue; // one or more trigger conditions were not fulfilled
                PCExeAutoBonus(sd, autoBonus);
            }
        }
    }

    // Check for player autobonuses when attacked
    if (dstatus != nullptr && bl->IsAlive() == true && isNoDamage == false)
    {
        std::vector<ROWOWAutoBonus> autoBonuses = dstatus->Bonus.AutoBonus2;
        for (ROWOWAutoBonus const& autoBonus : autoBonuses)
        {
            if (int32(urand(0, 1000)) >= autoBonus.Rate)
                continue;
            if (IsAutoTriggered(autoBonus.AtkType, attackType) == false)
                continue; // one or more trigger conditions were not fulfilled
            PCExeAutoBonus(dstsd, autoBonus);
        }
    }
}

void ROWOWMod::SkillOnSkillUsage(Player* player, Unit* bl, uint16 skillID, uint64 tick)
{
    // skill_onskillusage: the autospells (bAutoSpellOnSkill) and autobonus3 of a skill the player used.  An autospell already going
    // (lock) doesn't go again inside itself
    ROWOWStatus const* status = GetROStatus(player);
    if (status == nullptr || skillID == 0)
        return;
    ROWOWSkillUnitData* ud = GetSkillUnitData(player);
    std::vector<ROWOWAutoSpell> autoSpells = status->Bonus.AutoSpell3;
    for (ROWOWAutoSpell const& autoSpell : autoSpells)
    {
        if (autoSpell.TriggerSkillID != skillID)
            continue;
        uint32 lockKey = (uint32(autoSpell.TriggerSkillID) << 16) | autoSpell.SkillID;
        if (std::find(ud->AutoSpellLocks.begin(), ud->AutoSpellLocks.end(), lockKey) != ud->AutoSpellLocks.end())
            continue; // autospell already being executed
        uint16 skill = autoSpell.SkillID;
        if (GetSkill(skill) == nullptr || IsSkillNotOk(player, skill) == true)
            continue;
        // DANGER DANGER: here force target actually means use yourself as target!
        Unit* tbl = (autoSpell.Flag & ROWOW_AUTOSPELL_FORCE_TARGET) != 0 ? player : bl;
        if (tbl == nullptr)
            continue; // No target
        if (int32(urand(0, 999)) >= autoSpell.Rate)
            continue;
        uint16 skillLevel = autoSpell.Level != 0 ? autoSpell.Level : 1;
        if ((autoSpell.Flag & ROWOW_AUTOSPELL_FORCE_RANDOM_LEVEL) != 0)
            skillLevel = uint16(urand(1, skillLevel)); // random skill_lv
        ud->AutoSpellLocks.push_back(lockKey);
        SkillAutoCast(player, tbl, skill, skillLevel, tick);
        ud = GetSkillUnitData(player);
        ud->AutoSpellLocks.erase(std::remove(ud->AutoSpellLocks.begin(), ud->AutoSpellLocks.end(), lockKey), ud->AutoSpellLocks.end());
    }

    // Check for player autobonuses when being attacked by skill_id
    std::vector<ROWOWAutoBonus> autoBonuses = status->Bonus.AutoBonus3;
    for (ROWOWAutoBonus const& autoBonus : autoBonuses)
    {
        if (int32(urand(0, 1000)) >= autoBonus.Rate)
            continue;
        if (autoBonus.AtkType != skillID)
            continue;
        PCExeAutoBonus(player, autoBonus);
    }
}

void ROWOWMod::PCExeAutoBonus(Player* player, ROWOWAutoBonus const& autoBonus)
{
    // pc_exeautobonus: its other script runs now (an effect) when its items are all still worn, and its bonus script counts in the status for
    // its duration (restarted when it goes off again)
    ROWOWIdentity* identity = GetIdentity(player);
    uint32 equipPositions = 0;
    for (auto const& itemGUIDByEquipPosition : identity->ItemGUIDsByEquipPosition)
        equipPositions |= itemGUIDByEquipPosition.first;
    if (autoBonus.OtherScript.empty() == false && (equipPositions & autoBonus.Pos) == autoBonus.Pos)
    {
        if (std::shared_ptr<ROWOWScript> script = GetAutoBonusScript(autoBonus.OtherScript))
        {
            ROWOWScriptContext context;
            context.AttachedPlayer = player;
            context.Identity = identity;
            context.EquipPositions = autoBonus.Pos;
            ROWOWSCRIPT->Run(*script, context, true);
        }
    }
    auto activeIter = std::find_if(identity->ActiveAutoBonuses.begin(), identity->ActiveAutoBonuses.end(), [&autoBonus](ROWOWActiveAutoBonus const& active)
    {
        return active.Pos == autoBonus.Pos && active.BonusScript == autoBonus.BonusScript;
    });
    if (activeIter == identity->ActiveAutoBonuses.end())
        activeIter = identity->ActiveAutoBonuses.insert(identity->ActiveAutoBonuses.end(), ROWOWActiveAutoBonus());
    activeIter->BonusScript = autoBonus.BonusScript;
    activeIter->Pos = autoBonus.Pos;
    activeIter->EndTime = GetTick() + autoBonus.Duration;
    StatusCalcBL(player, std::bitset<ROWOW_SCB_MAX>().set(ROWOW_SCB_BASE)); // status_calc_pc(sd, SCO_FORCE)
}

void ROWOWMod::UpdateAutoBonuses(Player* player)
{
    // pc_endautobonus: a running autobonus ran out, and the status no longer counts it
    ROWOWIdentity* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    if (identity == nullptr || identity->ActiveAutoBonuses.empty() == true)
        return;
    uint64 tick = GetTick();
    size_t activeCount = identity->ActiveAutoBonuses.size();
    identity->ActiveAutoBonuses.erase(std::remove_if(identity->ActiveAutoBonuses.begin(), identity->ActiveAutoBonuses.end(),
        [tick](ROWOWActiveAutoBonus const& active) { return active.EndTime <= tick; }), identity->ActiveAutoBonuses.end());
    if (identity->ActiveAutoBonuses.size() != activeCount && identity->IsInForm == true)
        StatusCalcBL(player, std::bitset<ROWOW_SCB_MAX>().set(ROWOW_SCB_BASE));
}

void ROWOWMod::StatusCalcPCAutoBonuses(Player* player, ROWOWIdentity const& identity, ROWOWStatus& status) const
{
    // pc_delautobonus with restore: each running autobonus's script counts in the status as long as all its items are still worn
    if (identity.ActiveAutoBonuses.empty() == true)
        return;
    uint32 equipPositions = 0;
    for (auto const& itemGUIDByEquipPosition : identity.ItemGUIDsByEquipPosition)
        equipPositions |= itemGUIDByEquipPosition.first;
    ROWOWScriptContext context;
    context.AttachedPlayer = player;
    context.Identity = const_cast<ROWOWIdentity*>(&identity);
    context.BonusStatus = &status;
    for (ROWOWActiveAutoBonus const& active : identity.ActiveAutoBonuses)
    {
        if ((equipPositions & active.Pos) != active.Pos)
            continue; // Not all required items equipped anymore
        std::shared_ptr<ROWOWScript> script = GetAutoBonusScript(active.BonusScript);
        if (script == nullptr || script->Error.empty() == false)
            continue;
        context.EquipPositions = active.Pos;
        context.Stack.clear();
        context.InstructionIndex = 0;
        context.InstructionCount = 0;
        context.IsStopped = false;
        context.Error.clear();
        ROWOWSCRIPT->Run(*script, context, true);
    }
}

std::shared_ptr<ROWOWScript> ROWOWMod::GetAutoBonusScript(std::string const& text) const
{
    auto scriptIter = AutoBonusScriptsByText.find(text);
    return scriptIter != AutoBonusScriptsByText.end() ? scriptIter->second : nullptr;
}
