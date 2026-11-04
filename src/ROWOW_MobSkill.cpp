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

#include "Chat.h"
#include "Creature.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TemporarySummon.h"

#include "ROWOW.h"
#include "ROWOW_Skill.h"

#include <algorithm>
#include <set>

using namespace std;

// RO monsters' skills, a port of rAthena's mob.cpp (pre-renewal): mob_readskilldb, mobskill_use, mobskill_event, the slaves (mob_summonslave,
// mob_ai_sub_hard_slavemob and the like), mob_dead's MVP reward and the revive.  A monster's skill runs through the R6 skill engine as a player's does:
// its row gives the skill level and the cast time, and it casts the skill's spell with WoW's own rules off (RO's range, facing and conditions are all
// here).  Everything runs in the monster's map thread; the slaves are on the master's map.  The tables are written at startup and read only after

namespace
{
    uint64 constexpr MIN_MOBLINKTIME = 1000;

    // The WoW checks RO's rules replace, for a monster's cast.  Not TRIGGERED_CAST_DIRECTLY: the row's cast time counts
    TriggerCastFlags constexpr MOB_SKILL_CAST_FLAGS = TriggerCastFlags(TRIGGERED_IGNORE_GCD | TRIGGERED_IGNORE_SPELL_AND_CATEGORY_CD | TRIGGERED_IGNORE_POWER_AND_REAGENT_COST |
        TRIGGERED_IGNORE_SHAPESHIFT | TRIGGERED_IGNORE_CASTER_AURASTATE | TRIGGERED_IGNORE_CASTER_MOUNTED_OR_ON_VEHICLE | TRIGGERED_IGNORE_CASTER_AURAS | TRIGGERED_DONT_REPORT_CAST_ERROR);

    int32 const COUNT_MOBSKILL_CONDITION_NONE = -1;

    int32 ParseMobSkillState(std::string const& name)
    {
        // mob_parse_row_mobskilldb's state table
        static std::pair<char const*, int32> const states[] = { { "any", ROWOW_MSS_ANY }, { "idle", ROWOW_MSS_IDLE }, { "walk", ROWOW_MSS_WALK }, { "loot", ROWOW_MSS_LOOT },
            { "dead", ROWOW_MSS_DEAD }, { "attack", ROWOW_MSS_BERSERK }, { "angry", ROWOW_MSS_ANGRY }, { "chase", ROWOW_MSS_RUSH }, { "follow", ROWOW_MSS_FOLLOW },
            { "anytarget", ROWOW_MSS_ANYTARGET } };
        for (auto const& state : states)
            if (name == state.first)
                return state.second;
        return -2;
    }

    int32 ParseMobSkillCondition(std::string const& name)
    {
        // mob_parse_row_mobskilldb's cond1 table
        static std::pair<char const*, int32> const conditions[] = { { "always", ROWOW_MSC_ALWAYS }, { "myhpltmaxrate", ROWOW_MSC_MYHPLTMAXRATE }, { "myhpinrate", ROWOW_MSC_MYHPINRATE },
            { "friendhpltmaxrate", ROWOW_MSC_FRIENDHPLTMAXRATE }, { "friendhpinrate", ROWOW_MSC_FRIENDHPINRATE }, { "mystatuson", ROWOW_MSC_MYSTATUSON }, { "mystatusoff", ROWOW_MSC_MYSTATUSOFF },
            { "friendstatuson", ROWOW_MSC_FRIENDSTATUSON }, { "friendstatusoff", ROWOW_MSC_FRIENDSTATUSOFF }, { "attackpcgt", ROWOW_MSC_ATTACKPCGT }, { "attackpcge", ROWOW_MSC_ATTACKPCGE },
            { "slavelt", ROWOW_MSC_SLAVELT }, { "slavele", ROWOW_MSC_SLAVELE }, { "closedattacked", ROWOW_MSC_CLOSEDATTACKED }, { "longrangeattacked", ROWOW_MSC_LONGRANGEATTACKED },
            { "skillused", ROWOW_MSC_SKILLUSED }, { "afterskill", ROWOW_MSC_AFTERSKILL }, { "casttargeted", ROWOW_MSC_CASTTARGETED }, { "rudeattacked", ROWOW_MSC_RUDEATTACKED },
            { "masterhpltmaxrate", ROWOW_MSC_MASTERHPLTMAXRATE }, { "masterattacked", ROWOW_MSC_MASTERATTACKED }, { "alchemist", ROWOW_MSC_ALCHEMIST }, { "onspawn", ROWOW_MSC_SPAWN },
            { "mobnearbygt", ROWOW_MSC_MOBNEARBYGT }, { "groundattacked", ROWOW_MSC_GROUNDATTACKED }, { "damagedgt", ROWOW_MSC_DAMAGEDGT }, { "trickcasting", ROWOW_MSC_TRICKCASTING } };
        for (auto const& condition : conditions)
            if (name == condition.first)
                return condition.second;
        return COUNT_MOBSKILL_CONDITION_NONE;
    }

    int32 ParseMobSkillTarget(std::string const& name)
    {
        // mob_parse_row_mobskilldb's target table
        static std::pair<char const*, int32> const targets[] = { { "target", ROWOW_MST_TARGET }, { "randomtarget", ROWOW_MST_RANDOM }, { "self", ROWOW_MST_SELF }, { "friend", ROWOW_MST_FRIEND },
            { "master", ROWOW_MST_MASTER }, { "around5", ROWOW_MST_AROUND5 }, { "around6", ROWOW_MST_AROUND6 }, { "around7", ROWOW_MST_AROUND7 }, { "around8", ROWOW_MST_AROUND8 },
            { "around1", ROWOW_MST_AROUND1 }, { "around2", ROWOW_MST_AROUND2 }, { "around3", ROWOW_MST_AROUND3 }, { "around4", ROWOW_MST_AROUND4 }, { "around", ROWOW_MST_AROUND } };
        for (auto const& target : targets)
            if (name == target.first)
                return target.second;
        return ROWOW_MST_TARGET;
    }

    int64 ParseMobSkillCond2(std::string const& value)
    {
        // mob_parse_row_mobskilldb's cond2: a number, or a special constant
        static std::pair<char const*, int32> const constants[] = { { "anybad", -1 }, { "stone", ROWOW_SC_STONE }, { "freeze", ROWOW_SC_FREEZE }, { "stun", ROWOW_SC_STUN }, { "sleep", ROWOW_SC_SLEEP },
            { "poison", ROWOW_SC_POISON }, { "curse", ROWOW_SC_CURSE }, { "silence", ROWOW_SC_SILENCE }, { "confusion", ROWOW_SC_CONFUSION }, { "blind", ROWOW_SC_BLIND },
            { "hiding", ROWOW_SC_HIDING }, { "sight", ROWOW_SC_SIGHT } };
        for (auto const& constant : constants)
            if (value == constant.first)
                return constant.second;
        return strtoll(value.c_str(), nullptr, 10);
    }
}

void ROWOWMod::LoadMobSkillData()
{
    MobSkillsByMobID.clear();
    MobChatsByID.clear();
    if (IsEnabled == false)
        return;

    // mob_chat_db
    if (ROWOWDataTable const* chatTable = GetDataTable("mod_rowow_db_mob_chat"))
    {
        int32 idColumnIndex = chatTable->GetColumnIndex("Id");
        int32 colorColumnIndex = chatTable->GetColumnIndex("Color");
        int32 dialogColumnIndex = chatTable->GetColumnIndex("Dialog");
        for (size_t rowIndex = 0; rowIndex < chatTable->Rows.size(); rowIndex++)
        {
            ROWOWMobChat& chat = MobChatsByID[uint32(chatTable->GetInt(rowIndex, idColumnIndex, 0))];
            std::string const& color = chatTable->GetCell(rowIndex, colorColumnIndex);
            chat.Color = color.empty() == false ? uint32(strtoul(color.c_str(), nullptr, 16)) : 0xFF0000; // Default: 0xFF0000
            chat.Message = chatTable->GetCell(rowIndex, dialogColumnIndex);
        }
    }

    if (BattleConfig.MobSkillRate == 0)
        return; // Mob skill use disabled. Not reading mob skills.
    ROWOWDataTable const* skillTable = GetDataTable("mod_rowow_db_mob_skill");
    if (skillTable == nullptr)
    {
        LOG_ERROR("module.ROWOW", "ROWOWMod::LoadMobSkillData found no mod_rowow_db_mob_skill table, so monsters use no skills");
        return;
    }
    int32 mobColumnIndex = skillTable->GetColumnIndex("MobID");
    int32 stateColumnIndex = skillTable->GetColumnIndex("State");
    int32 skillColumnIndex = skillTable->GetColumnIndex("SkillID");
    int32 levelColumnIndex = skillTable->GetColumnIndex("SkillLevel");
    int32 rateColumnIndex = skillTable->GetColumnIndex("Rate");
    int32 castTimeColumnIndex = skillTable->GetColumnIndex("CastTime");
    int32 delayColumnIndex = skillTable->GetColumnIndex("Delay");
    int32 cancelColumnIndex = skillTable->GetColumnIndex("Cancelable");
    int32 targetColumnIndex = skillTable->GetColumnIndex("Target");
    int32 cond1ColumnIndex = skillTable->GetColumnIndex("ConditionType");
    int32 cond2ColumnIndex = skillTable->GetColumnIndex("ConditionValue");
    int32 valColumnIndexes[5] = { skillTable->GetColumnIndex("Value1"), skillTable->GetColumnIndex("Value2"), skillTable->GetColumnIndex("Value3"), skillTable->GetColumnIndex("Value4"),
        skillTable->GetColumnIndex("Value5") };
    int32 emotionColumnIndex = skillTable->GetColumnIndex("Emotion");
    int32 chatColumnIndex = skillTable->GetColumnIndex("Chat");
    uint32 rowCount = 0;
    for (size_t rowIndex = 0; rowIndex < skillTable->Rows.size(); rowIndex++)
    {
        // mob_parse_row_mobskilldb
        uint32 mobID = uint32(skillTable->GetInt(rowIndex, mobColumnIndex, 0));
        auto statusIter = MobStatusesByMobID.find(mobID);
        uint16 skillID = uint16(skillTable->GetInt(rowIndex, skillColumnIndex, 0));
        ROWOWSkill const* skill = GetSkill(skillID);
        std::vector<ROWOWMobSkill>& skills = MobSkillsByMobID[mobID];
        int32 state = ParseMobSkillState(skillTable->GetCell(rowIndex, stateColumnIndex));
        if (statusIter == MobStatusesByMobID.end() || skill == nullptr || skills.size() >= ROWOW_MAX_MOBSKILL || state == -2)
            continue;
        ROWOWMobSkill ms;
        ms.State = state;
        ms.SkillID = skillID;

        // We strip max skill level
        int64 level = skillTable->GetInt(rowIndex, levelColumnIndex, 0);
        ms.SkillLevel = uint16(std::min<int64>(level <= 0 ? 1 : level, BattleConfig.MobMaxSkillLevel));

        // Apply battle_config modifiers to rate (permillage) and delay [Skotlex]
        int64 rate = skillTable->GetInt(rowIndex, rateColumnIndex, 0);
        if (BattleConfig.MobSkillRate != 100)
            rate = rate * BattleConfig.MobSkillRate / 100;
        ms.Permillage = rate > 10000 ? 10000 : (rate == 0 ? 1 : int32(rate));
        ms.CastTime = int32(skillTable->GetInt(rowIndex, castTimeColumnIndex, 0));
        int64 delay = skillTable->GetInt(rowIndex, delayColumnIndex, 0);
        if (BattleConfig.MobSkillDelayRate != 100)
            delay = delay * BattleConfig.MobSkillDelayRate / 100;
        ms.Delay = (delay < 0 || delay > ROWOW_MOB_MAX_DELAY) ? ROWOW_MOB_MAX_DELAY : int32(delay);
        std::string const& cancel = skillTable->GetCell(rowIndex, cancelColumnIndex);
        ms.IsCancel = cancel == "yes" || strtol(cancel.c_str(), nullptr, 10) != 0;

        // Check that the target condition is right for the skill type. [Skotlex]
        ms.Target = uint8(ParseMobSkillTarget(skillTable->GetCell(rowIndex, targetColumnIndex)));
        if (SkillGetCastType(ms.SkillID) == ROWOW_CAST_GROUND)
        {
            if (ms.Target > ROWOW_MST_AROUND)
                ms.Target = ROWOW_MST_TARGET;
        }
        else if (ms.Target > ROWOW_MST_MASTER)
            ms.Target = ROWOW_MST_TARGET;
        ms.Cond1 = ParseMobSkillCondition(skillTable->GetCell(rowIndex, cond1ColumnIndex));
        ms.Cond2 = ParseMobSkillCond2(skillTable->GetCell(rowIndex, cond2ColumnIndex));
        for (int32 i = 0; i < 5; i++)
            ms.Val[i] = int32(strtol(skillTable->GetCell(rowIndex, valColumnIndexes[i]).c_str(), nullptr, 0));

        if (ms.SkillID == ROWOW_SKILL_NPC_EMOTION && ms.Val[1] == int32(statusIter->second.Mode))
        {
            ms.Val[1] = 0;
            ms.Val[4] = 1; // request to return mode to normal.
        }
        if (ms.SkillID == ROWOW_SKILL_NPC_EMOTION_ON && ms.Val[1] != 0)
        {
            // Adds a mode to the mob.  Remove aggressive mode when the new mob type is passive.
            if ((ms.Val[1] & ROWOW_MODE_AGGRESSIVE) == 0)
                ms.Val[3] |= ROWOW_MODE_AGGRESSIVE;
            ms.Val[2] |= ms.Val[1]; // Add the new mode.
            ms.Val[1] = 0; // Do not "set" it.
        }
        std::string const& emotion = skillTable->GetCell(rowIndex, emotionColumnIndex);
        ms.Emotion = emotion.empty() == false ? int32(strtol(emotion.c_str(), nullptr, 10)) : -1;
        std::string const& chat = skillTable->GetCell(rowIndex, chatColumnIndex);
        uint32 chatID = chat.empty() == false ? uint32(strtoul(chat.c_str(), nullptr, 10)) : 0;
        ms.MsgID = MobChatsByID.find(chatID) != MobChatsByID.end() ? uint16(chatID) : 0;
        skills.push_back(ms);
        rowCount++;
    }
    LOG_INFO("module.ROWOW", "ROWOWMod::LoadMobSkillData loaded {} skill rows of {} monsters and {} chat lines", rowCount, MobSkillsByMobID.size(), MobChatsByID.size());
}

std::vector<ROWOWMobSkill> const* ROWOWMod::GetMobSkills(uint32 mobID) const
{
    auto skillsIter = MobSkillsByMobID.find(mobID);
    return skillsIter != MobSkillsByMobID.end() ? &skillsIter->second : nullptr;
}

ROWOWMobSkill const* ROWOWMod::GetMobSkillInUse(Unit const* src) const
{
    // md->db->skill[md->skill_idx]: the row a monster is casting.  None for a cast of a script's or a GM's, or of a monster that isn't RO's
    Creature const* md = src != nullptr ? src->ToCreature() : nullptr;
    ROWOWMobData const* mob = md != nullptr ? md->CustomData.Get<ROWOWMobData>("ROWOWMobData") : nullptr;
    if (mob != nullptr && mob->FollowerSkill.SkillID != 0)
        return &mob->FollowerSkill; // What a homunculus or mercenary was told to cast
    if (mob == nullptr || mob->SkillIdx < 0)
        return nullptr;
    std::vector<ROWOWMobSkill> const* skills = GetMobSkills(GetROMobID(md));
    return skills != nullptr && size_t(mob->SkillIdx) < skills->size() ? &(*skills)[mob->SkillIdx] : nullptr;
}

std::vector<std::string> ROWOWMod::GetMobSkillsWithoutImpl(uint32& skillCountOut) const
{
    // Every distinct skill of the table as the converter wrote it (rows the load dropped too), for finding the ones no skill class answers
    std::vector<std::string> skillNames;
    std::set<uint16> skillIDs;
    if (ROWOWDataTable const* skillTable = GetDataTable("mod_rowow_db_mob_skill"))
    {
        int32 skillColumnIndex = skillTable->GetColumnIndex("SkillID");
        for (size_t rowIndex = 0; rowIndex < skillTable->Rows.size(); rowIndex++)
            skillIDs.insert(uint16(skillTable->GetInt(rowIndex, skillColumnIndex, 0)));
    }
    for (uint16 skillID : skillIDs)
    {
        if (GetSkillImpl(skillID) != nullptr)
            continue;
        ROWOWSkill const* skill = GetSkill(skillID);
        skillNames.push_back(skill != nullptr ? skill->Name : std::to_string(skillID));
    }
    skillCountOut = uint32(skillIDs.size());
    return skillNames;
}

bool ROWOWMod::WriteMobSkillDump(std::string const& filePath) const
{
    // What LoadMobSkillData left, row for row, for comparing with the oracle's @oraclemobskills
    FILE* file = fopen(filePath.c_str(), "w");
    if (file == nullptr)
        return false;
    for (auto const& skillsByMobID : MobSkillsByMobID)
        for (ROWOWMobSkill const& ms : skillsByMobID.second)
            fprintf(file, "mobskill %u state=%d skill=%u lv=%u rate=%d cast=%d delay=%d cancel=%d target=%u cond1=%d cond2=%lld val=%d,%d,%d,%d,%d emotion=%d msg=%u\n", skillsByMobID.first, ms.State,
                ms.SkillID, ms.SkillLevel, ms.Permillage, ms.CastTime, ms.Delay, ms.IsCancel == true ? 1 : 0, ms.Target, ms.Cond1, static_cast<long long>(ms.Cond2), ms.Val[0], ms.Val[1], ms.Val[2],
                ms.Val[3], ms.Val[4], ms.Emotion, ms.MsgID);
    fclose(file);
    return true;
}

void ROWOWMod::SetMobSkillTrace(bool isOn)
{
    std::lock_guard<std::mutex> lock(MobSkillTraceMutex);
    IsMobSkillTraceOn = isOn;
    if (isOn == true)
        MobSkillTraceCounts.clear();
}

bool ROWOWMod::WriteMobSkillTrace(std::string const& filePath) const
{
    std::lock_guard<std::mutex> lock(MobSkillTraceMutex);
    FILE* file = fopen(filePath.c_str(), "w");
    if (file == nullptr)
        return false;
    for (auto const& countByKey : MobSkillTraceCounts)
        fprintf(file, "%s count=%u\n", countByKey.first.c_str(), countByKey.second);
    fclose(file);
    return true;
}

bool ROWOWMod::MobChatDisplayMessage(Creature* md, uint16 msgID)
{
    // mob_chat_display_message: the line, in its color, as the monster's speech to those around.  (WoW's chat puts the name in front, as RO's "name : line")
    auto chatIter = MobChatsByID.find(msgID);
    if (chatIter == MobChatsByID.end())
        return false;
    md->Say(fmt::format("|cff{:06x}{}|r", chatIter->second.Color & 0xFFFFFF, chatIter->second.Message), LANG_UNIVERSAL);
    return true;
}

void ROWOWMod::MobSkillDelay(Creature* md, ROWOWMobData* mob, uint64 tick)
{
    // mobskill_delay: set whether the skill was cast or cast-cancelled.  If skill was used by a script, do not apply any skill delay
    std::vector<ROWOWMobSkill> const* ms = GetMobSkills(GetROMobID(md));
    if (mob->SkillIdx < 0 || ms == nullptr || size_t(mob->SkillIdx) >= ms->size())
        return;
    mob->SkillDelays.resize(ms->size(), 0);
    uint16 skillID = (*ms)[mob->SkillIdx].SkillID;
    if ((BattleConfig.MobAI & 0x200) != 0)
    {
        mob->SkillDelays[mob->SkillIdx] = tick + uint64((*ms)[mob->SkillIdx].Delay);
        return;
    }

    // Officially the skill delay is per skill rather than per skill db entry.  We simulate this by setting the delay for all entries of the same skill.
    // Officially it actually tries to find the skill again using the monster's current state.  If the skill cannot be found anymore because the
    // monster's state has changed no delay will be applied
    int32 delay = 0;
    for (ROWOWMobSkill const& skill : *ms)
    {
        if (skill.SkillID != skillID)
            continue;
        if (skill.State == mob->SkillState || skill.State == ROWOW_MSS_ANY ||
            (skill.State == ROWOW_MSS_ANYTARGET && mob->TargetID.IsEmpty() == false && mob->SkillState != ROWOW_MSS_LOOT)) // State and skill match, use the first delay found
        {
            delay = skill.Delay;
            break;
        }
    }
    // Apply delay found to all entries of the skill
    for (size_t i = 0; i < ms->size(); i++)
        if ((*ms)[i].SkillID == skillID)
            mob->SkillDelays[i] = tick + uint64(delay);
}

void ROWOWMod::MobSetDelay(Creature* md, uint64 tick, bool isCastEnd)
{
    // unit_set_attackdelay for a monster's cast end and cancel: it gets its attack delay then, not when the cast starts (the swing timer), and mob_set_delay:
    // its AI is inactive for its attack motion after it finished casting, and the skill delays are set
    ROWOWMobData* mob = GetMobData(md);
    ROWOWStatus const* status = GetROStatus(md);
    if (status == nullptr)
        return;
    if (md->getAttackTimer(BASE_ATTACK) < uint32(status->AttackDelay))
        md->setAttackTimer(BASE_ATTACK, uint32(status->AttackDelay));
    if (isCastEnd == true && (BattleConfig.MobAI & 0x2000) == 0)
        mob->NextThinkTime = std::max<uint64>(tick + uint64(status->AttackMotion), mob->NextThinkTime);
    MobSkillDelay(md, mob, tick);
}

uint32 ROWOWMod::MobCountSlave(Creature* md)
{
    // mob_countslave: the list keeps what the map's search would find.  Dead or gone slaves leave it
    ROWOWMobData* mob = GetMobData(md);
    auto isGone = [md](ObjectGuid const& slaveGUID)
    {
        Creature* slave = ObjectAccessor::GetCreature(*md, slaveGUID);
        return slave == nullptr || slave->IsAlive() == false;
    };
    mob->SlaveGUIDs.erase(std::remove_if(mob->SlaveGUIDs.begin(), mob->SlaveGUIDs.end(), isGone), mob->SlaveGUIDs.end());
    return uint32(mob->SlaveGUIDs.size());
}

Unit* ROWOWMod::MobGetMaster(Creature const* md) const
{
    // map_id2bl(md->master_id)
    ROWOWMobData const* mob = md->CustomData.Get<ROWOWMobData>("ROWOWMobData");
    if (mob != nullptr && mob->MasterID.IsEmpty() == false)
        return ObjectAccessor::GetUnit(*md, mob->MasterID);
    ROWOWMobSummon const* summon = md->CustomData.Get<ROWOWMobSummon>("ROWOWMobSummon");
    if (summon != nullptr && summon->MasterGUID.IsEmpty() == false)
        return ObjectAccessor::GetUnit(*md, summon->MasterGUID);
    return nullptr;
}

bool ROWOWMod::MobSkillUse(Creature* md, uint64 tick, int32 event, int64 damage, bool isWalking)
{
    // mobskill_use: runs the monster's skill list for its state and returns whether it used one
    ROWOWMobData* mob = GetMobData(md);
    ROWOWStatus const* status = GetROStatus(md);
    std::vector<ROWOWMobSkill> const* skills = GetMobSkills(GetROMobID(md));
    if (BattleConfig.MobSkillRate == 0 || md->HasUnitState(UNIT_STATE_CASTING) == true || skills == nullptr || skills->empty() == true || status == nullptr ||
        (status->Mode & ROWOW_MODE_NOCAST) != 0)
        return false;

    // Monsters check their non-attack-state skills once per second, but we ignore this for events for now
    if (event == -1)
        mob->LastSkillCheck = tick;
    mob->SkillDelays.resize(skills->size(), 0);

    // Pick a starting position and loop from that.
    std::vector<ROWOWMobSkill> const& ms = *skills;
    size_t i = (BattleConfig.MobAI & 0x100) != 0 ? urand(0, uint32(ms.size() - 1)) : 0;
    bool isLoopSkip = (BattleConfig.MobAI & 0x1000) != 0;
    for (size_t n = 0; n < ms.size(); i++, n++)
    {
        if (i == ms.size())
            i = 0;
        if (tick < mob->SkillDelays[i])
            continue;
        int64 c2 = ms[i].Cond2;
        Unit* fbl = nullptr; // Friend bl, which can either be a BL_PC or BL_MOB depending on the situation. [Skotlex]
        if (ms[i].State != mob->SkillState)
        {
            if (mob->SkillState == ROWOW_MSS_DEAD || (ms[i].State != ROWOW_MSS_ANY && // ANYTARGET works with any state as long as there's a target. [Skotlex]
                (ms[i].State != ROWOW_MSS_ANYTARGET || mob->TargetID.IsEmpty() == true || mob->SkillState == ROWOW_MSS_LOOT)))
                continue;
        }
        if (int32(urand(0, 9999)) > ms[i].Permillage) // Lupus (max value = 10000)
            continue;

        bool flag = false;
        if (ms[i].Cond1 == event)
            flag = true; // Trigger skill.
        else if (ms[i].Cond1 == ROWOW_MSC_SKILLUSED)
            flag = (event & 0xffff) == ROWOW_MSC_SKILLUSED && ((event >> 16) == c2 || c2 == 0);
        else if (ms[i].Cond1 == ROWOW_MSC_GROUNDATTACKED && damage > 0)
        {
            ROWOWSkill const* usedSkill = (event & 0xffff) == ROWOW_MSC_SKILLUSED ? GetSkill(uint16(event >> 16)) : nullptr;
            flag = usedSkill != nullptr && (usedSkill->Inf & ROWOW_INF_GROUND_SKILL) != 0;
        }
        else if (ms[i].Cond1 == ROWOW_MSC_DAMAGEDGT && damage > 0 && (event & 0xffff) != ROWOW_MSC_SKILLUSED) // Avoid double check if skill has been used [datawulf]
            flag = damage > c2;
        else if (event == -1)
        {
            // Avoid entering on defined events to avoid "hyper-active skill use" due to the overflow of calls to this function in battle.
            int64 hpRate = md->GetMaxHealth() > 0 ? int64(md->GetHealth()) * 100 / md->GetMaxHealth() : 0; // get_percentage
            auto findFriendHPRate = [this, md, &fbl](int64 minRate, int64 maxRate)
            {
                // mob_getfriendhprate: a monster near it (an ally player, for a summon), not itself, whose HP rate is in the range
                bool isSummon = md->CustomData.Get<ROWOWMobSummon>("ROWOWMobSummon") != nullptr;
                MapForEachInRange(md, 8, [this, md, minRate, maxRate, isSummon, &fbl](Unit* bl)
                {
                    if (fbl != nullptr || (bl == md && (BattleConfig.MobAI & 0x10) == 0) || bl->IsAlive() == false || bl->IsPlayer() != isSummon || BattleCheckTarget(md, bl, ROWOW_BCT_ENEMY) > 0)
                        return 0;
                    int64 rate = bl->GetMaxHealth() > 0 ? int64(bl->GetHealth()) * 100 / bl->GetMaxHealth() : 0;
                    if (rate >= minRate && rate <= maxRate)
                        fbl = bl;
                    return 1;
                });
                return fbl != nullptr;
            };
            auto hasStatus = [this](Unit const* bl, int32 condition, int64 type)
            {
                // mob_getstatus_sub: anybad is any of the common statuses
                ROWOWStatusChange const* sc = GetStatusChange(bl);
                bool isFound = false;
                if (sc != nullptr)
                {
                    if (type < 0)
                    {
                        for (int32 scType = ROWOW_SC_COMMON_MIN; scType <= ROWOW_SC_COMMON_MAX && isFound == false; scType++)
                            isFound = sc->HasSCE(scType);
                    }
                    else
                        isFound = sc->HasSCE(int32(type));
                }
                return (condition == ROWOW_MSC_MYSTATUSON || condition == ROWOW_MSC_FRIENDSTATUSON) ? isFound : isFound == false;
            };
            switch (ms[i].Cond1)
            {
                case ROWOW_MSC_ALWAYS:
                    flag = true;
                    break;
                case ROWOW_MSC_MYHPLTMAXRATE: // HP< maxhp%
                    flag = hpRate <= c2;
                    break;
                case ROWOW_MSC_MYHPINRATE:
                    flag = hpRate >= c2 && hpRate <= ms[i].Val[0];
                    break;
                case ROWOW_MSC_MYSTATUSON: // status[num] on
                case ROWOW_MSC_MYSTATUSOFF: // status[num] off
                {
                    ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(md));
                    flag = sc != nullptr && sc->IsEmpty() == false && hasStatus(md, ms[i].Cond1, c2);
                    break;
                }
                case ROWOW_MSC_FRIENDHPLTMAXRATE: // friend HP < maxhp%
                    flag = findFriendHPRate(0, c2);
                    break;
                case ROWOW_MSC_FRIENDHPINRATE:
                    flag = findFriendHPRate(c2, ms[i].Val[0]);
                    break;
                case ROWOW_MSC_FRIENDSTATUSON: // friend status[num] on
                case ROWOW_MSC_FRIENDSTATUSOFF: // friend status[num] off
                    // mob_getfriendstatus: the last monster near it that matches
                    MapForEachInRange(md, 8, [this, md, &hasStatus, &ms, i, c2, &fbl](Unit* bl)
                    {
                        if ((bl == md && (BattleConfig.MobAI & 0x10) == 0) || bl->IsCreature() == false || BattleCheckTarget(md, bl, ROWOW_BCT_ENEMY) > 0)
                            return 0;
                        if (hasStatus(bl, ms[i].Cond1, c2) == true)
                            fbl = bl;
                        return 0;
                    });
                    flag = fbl != nullptr;
                    break;
                case ROWOW_MSC_SLAVELT: // slave < num
                    flag = int64(MobCountSlave(md)) < c2;
                    break;
                case ROWOW_MSC_ATTACKPCGT: // attack pc > num
                    flag = int64(md->getAttackers().size()) > c2; // unit_counttargeted
                    break;
                case ROWOW_MSC_SLAVELE: // slave <= num
                    flag = int64(MobCountSlave(md)) <= c2;
                    break;
                case ROWOW_MSC_ATTACKPCGE: // attack pc >= num
                    flag = int64(md->getAttackers().size()) >= c2;
                    break;
                case ROWOW_MSC_AFTERSKILL:
                    flag = mob->AfterSkillID == c2;
                    break;
                case ROWOW_MSC_RUDEATTACKED:
                    flag = mob->AttackedCount >= 1; // RUDE_ATTACKED_COUNT
                    if (flag == true)
                        mob->AttackedCount = 0; // Rude attacked count should be reset after the skill condition is met. Thanks to Komurka [Skotlex]
                    break;
                case ROWOW_MSC_MASTERHPLTMAXRATE:
                    // mob_getmasterhpltmaxrate
                    if (Unit* master = MobGetMaster(md); master != nullptr && master->GetMaxHealth() > 0 && int64(master->GetHealth()) * 100 / master->GetMaxHealth() < c2)
                        fbl = master;
                    flag = fbl != nullptr;
                    break;
                case ROWOW_MSC_MASTERATTACKED:
                    fbl = MobGetMaster(md);
                    flag = fbl != nullptr && fbl->getAttackers().empty() == false;
                    break;
                case ROWOW_MSC_ALCHEMIST:
                {
                    ROWOWMobSummon const* summon = md->CustomData.Get<ROWOWMobSummon>("ROWOWMobSummon");
                    flag = summon != nullptr && summon->AI != ROWOW_AI_NONE && mob->TrickCasting == 0 && md->GetHealth() < md->GetMaxHealth();
                    break;
                }
                case ROWOW_MSC_MOBNEARBYGT:
                {
                    // map_foreachinallrange(mob_count_sub, md, AREA_SIZE, BL_MOB)
                    int64 count = MapForEachInRange(md, BattleConfig.AreaSize, [](Unit* bl) { return bl->IsCreature() == true ? 1 : 0; });
                    flag = count > c2;
                    break;
                }
                case ROWOW_MSC_TRICKCASTING:
                    flag = mob->TrickCasting > 0;
                    break;
            }
        }
        if (flag == false)
            continue; // Skill requisite failed to be fulfilled.

        // Execute skill
        ROWOWMobSkill const& skill = ms[i];
        int32 range = SkillGetRange2(md, skill.SkillID, skill.SkillLevel, true);
        bool isGround = SkillGetCastType(skill.SkillID) == ROWOW_CAST_GROUND;
        Unit* bl = nullptr;
        switch (skill.Target)
        {
            case ROWOW_MST_RANDOM: // Pick a random enemy within skill range.
            {
                // battle_getenemy
                std::vector<Unit*> enemies;
                MobForEachEnemyInRange(md, range, [this, md, &enemies](Unit* unit)
                {
                    if (unit != md && unit->IsAlive() == true && BattleCheckTarget(md, unit, ROWOW_BCT_ENEMY) > 0)
                        enemies.push_back(unit);
                });
                if (enemies.empty() == false)
                    bl = enemies[urand(0, uint32(enemies.size() - 1))];
                break;
            }
            case ROWOW_MST_TARGET:
            case ROWOW_MST_AROUND5:
            case ROWOW_MST_AROUND6:
            case ROWOW_MST_AROUND7:
            case ROWOW_MST_AROUND8:
                bl = mob->TargetID.IsEmpty() == false ? ObjectAccessor::GetUnit(*md, mob->TargetID) : nullptr;
                // Monsters that cannot attack put their last attacker as target
                if (bl == nullptr && isGround == false && skill.Target == ROWOW_MST_TARGET && (status->Mode & ROWOW_MODE_CANATTACK) == 0 && mob->AttackedID.IsEmpty() == false)
                    bl = ObjectAccessor::GetUnit(*md, mob->AttackedID);
                break;
            case ROWOW_MST_MASTER:
                bl = MobGetMaster(md);
                if (bl == nullptr)
                    bl = fbl != nullptr ? fbl : md;
                break;
            case ROWOW_MST_FRIEND:
                bl = fbl != nullptr ? fbl : md;
                break;
            default:
                bl = md;
                break;
        }
        bool isUsed = false;
        if (bl != nullptr && bl->IsInWorld() == true && bl->GetMap() == md->GetMap())
        {
            int32 x = 0, y = 0;
            GetUnitCell(bl, x, y);

            // Look for an area to cast the spell around...
            if (isGround == true && skill.Target >= ROWOW_MST_AROUND5)
            {
                int32 j = skill.Target >= ROWOW_MST_AROUND1 ? (skill.Target - ROWOW_MST_AROUND1) + 1 : (skill.Target - ROWOW_MST_AROUND5) + 1;
                MapSearchFreeCell(bl, j, x, y);
            }
            mob->SkillIdx = int32(i);
            if (BattleCheckRange(md, bl, range) == true)
            {
                // unit_skilluse_id2 and unit_skilluse_pos2's monster part: a player's summons can't summon or teleport
                bool isAllowed = true;
                if (skill.SkillID == ROWOW_SKILL_NPC_SUMMONSLAVE || skill.SkillID == ROWOW_SKILL_NPC_SUMMONMONSTER || skill.SkillID == ROWOW_SKILL_AL_TELEPORT)
                {
                    ROWOWMobSummon const* summon = md->CustomData.Get<ROWOWMobSummon>("ROWOWMobSummon");
                    isAllowed = summon == nullptr || summon->AI == ROWOW_AI_NONE;
                }

                // The spell of the skill's level, or the highest there is below it: a monster's level can pass the skill's, and a monster's own
                // skill has one spell only.  The engine goes by the row's level
                uint32 spellID = 0;
                for (uint16 level = skill.SkillLevel; level > 0 && spellID == 0; level--)
                    spellID = GetSkillSpellID(skill.SkillID, level);
                SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellID);
                if (isAllowed == true && spellInfo != nullptr)
                {
                    SpellCastTargets targets;
                    if (isGround == true)
                    {
                        float worldX = 0, worldY = 0;
                        GetCellWorldPosition(md->GetMapId(), x, y, worldX, worldY);
                        targets.SetDst(worldX, worldY, bl->GetPositionZ(), md->GetOrientation());
                    }
                    else
                        targets.SetUnitTarget(bl);
                    if (isWalking == false)
                        UnitStopWalking(md);
                    SpellCastResult result = md->CastSpell(targets, spellInfo, nullptr, MOB_SKILL_CAST_FLAGS);
                    isUsed = result == SPELL_CAST_OK;
                    if (isUsed == false && IsMobSkillTraceOn == true)
                        LOG_INFO("module.ROWOW", "mobtrace {} ({}) skill {} refused: {}", md->GetName(), GetROMobID(md), skill.SkillID, uint32(result));
                }
            }
        }
        if (isUsed == false)
        {
            if (isLoopSkip == true)
                continue;
            break;
        }

        // Skill used. Post-setups...
        if (skill.MsgID != 0) // Display color message [SnakeDrak]
            MobChatDisplayMessage(md, skill.MsgID);
        if (IsMobSkillTraceOn == true)
        {
            std::lock_guard<std::mutex> lock(MobSkillTraceMutex);
            MobSkillTraceCounts[fmt::format("mob={} state={} skill={} cond={} target={}", GetROMobID(md), mob->SkillState, skill.SkillID, skill.Cond1, skill.Target)]++;
            int32 mdX = 0, mdY = 0;
            GetUnitCell(md, mdX, mdY);
            LOG_INFO("module.ROWOW", "mobtrace {} ({}) state {} skill {} lv {} cast {} cond {} on {} hp {}/{} slaves {} cell {},{}", md->GetName(), GetROMobID(md), mob->SkillState, skill.SkillID,
                skill.SkillLevel, skill.CastTime, skill.Cond1, bl->GetName(), md->GetHealth(), md->GetMaxHealth(), mob->SlaveGUIDs.size(), mdX, mdY);
        }
        return true;
    }

    // No skill was used.
    mob->SkillIdx = -1;
    return false;
}

void ROWOWMod::MobSkillEvent(Creature* md, Unit* src, uint64 tick, int32 flag, int64 damage)
{
    // mobskill_event: the skills that answer being attacked or targeted
    if (md->IsInWorld() == false || md->IsAlive() == false)
        return;
    ROWOWMobData* mob = GetMobData(md);
    ObjectGuid targetID = mob->TargetID;
    if (targetID.IsEmpty() == true || BattleConfig.MobChangeTargetBySkill != 0)
        mob->TargetID = src->GetGUID();
    bool res = false;
    if (flag == -1)
        res = MobSkillUse(md, tick, ROWOW_MSC_CASTTARGETED);
    else if ((flag & 0xffff) == ROWOW_MSC_SKILLUSED)
        res = MobSkillUse(md, tick, flag, damage);
    else if ((flag & ROWOW_BF_SHORT) != 0)
        res = MobSkillUse(md, tick, ROWOW_MSC_CLOSEDATTACKED, damage);
    else if ((flag & ROWOW_BF_LONG) != 0 && (flag & ROWOW_BF_MAGIC) == 0) // Long-attacked should not include magic.
        res = MobSkillUse(md, tick, ROWOW_MSC_LONGRANGEATTACKED, damage);
    else if (damage > 0) // Trigger for any damage dealt from other attack types without affecting other triggers [datawulf]
        res = MobSkillUse(md, tick, -2, damage);
    if (res == false)
        mob->TargetID = targetID; // Restore previous target only if skill condition failed to trigger. [Skotlex]
    else if (BattleCheckTarget(md, src, ROWOW_BCT_ENEMY) <= 0)
        mob->TargetID = targetID; // Otherwise check if the target is an enemy, and unlock if needed.
}

bool ROWOWMod::MobAISubHardSlaveMob(Creature* md, ROWOWMobData* mob, uint64 tick)
{
    // mob_ai_sub_hard_slavemob: following the master, taking its target, dying with it.  True ends this think
    Creature* master = ObjectAccessor::GetCreature(*md, mob->MasterID);
    if (master == nullptr || master->IsAlive() == false)
    {
        StatusKill(md);
        return true;
    }
    ROWOWStatus const* status = GetROStatus(md);
    if (status == nullptr)
        return false;
    int32 masterDistance = DistanceBL(md, master);
    if ((status->Mode & ROWOW_MODE_CANMOVE) != 0) // If the mob can move, follow around. [Check by Skotlex]
    {
        // Teleport to master if further away than 15 cells
        if (BattleConfig.SlaveStickWithMaster != 0 && masterDistance > BattleConfig.AreaSize + 1)
        {
            UnitWarpNear(md, master->GetPosition());
            return true;
        }
        if (mob->TargetID.IsEmpty() == false)
            return false; // Slave is busy with a target.

        // Approach master if within view range, chase back to Master's area also if standing on top of the master.
        if ((masterDistance > ROWOW_MOB_SLAVEDISTANCE || masterDistance == 0) && UnitCanMove(md, mob, tick) == true)
        {
            int32 x = 0, y = 0;
            if (MapSearchFreeCell(master, ROWOW_MOB_SLAVEDISTANCE, x, y) == true)
            {
                if (UnitWalkToXY(md, x, y) == false) // Slave is too far from master (outside of battle_config.max_walk_path range), stay put
                {
                    UnitStopWalking(md);
                    return false; // Fail here so target will be picked back up when in range
                }
                mob->AttackTargetID.Clear(); // Slave will walk back to master if in range: unit_stop_attack
                return true;
            }
        }
    }

    // Avoid attempting to lock the master's target too often to avoid unnecessary overload. [Skotlex]
    if (tick - mob->LastLinkTime >= MIN_MOBLINKTIME && mob->TargetID.IsEmpty() == true)
    {
        ROWOWMobData const* masterMob = GetMobData(master);
        Unit* tbl = nullptr;
        if (masterMob->AttackTargetID.IsEmpty() == false && masterMob->IsAttackContinue == true)
            tbl = ObjectAccessor::GetUnit(*md, masterMob->AttackTargetID);
        else if (masterMob->TargetToID.IsEmpty() == false && masterMob->IsAttackContinue == true)
            tbl = ObjectAccessor::GetUnit(*md, masterMob->TargetToID);
        else if (masterMob->TargetID.IsEmpty() == false) // If master is a monster, it might still have a target after using a skill
            tbl = ObjectAccessor::GetUnit(*md, masterMob->TargetID);
        if (tbl != nullptr)
        {
            mob->LastLinkTime = tick;
            if (StatusCheckSkillUse(md, tbl, 0, 0) == true)
            {
                mob->TargetID = tbl->GetGUID();
                return true;
            }
        }
    }
    return false;
}

void ROWOWMod::MobSummonSlave(Creature* md2, int32 const* value, int32 amount, uint16 skillID)
{
    // mob_summonslave: amount monsters of the five in value, round-robin. [adapted by Skotlex]  Only NPC_SUMMONSLAVE's are slaves: the others stay on
    // their own.  Monsters summoned this way have no spawn line, so they go when they die
    if (GetMobCreatureEntry(uint32(value[0])) == 0)
        return;
    int32 count = 0;
    while (count < 5 && GetMobCreatureEntry(uint32(value[count])) != 0)
        count++;
    if (count < 1)
        return;
    int32 k = 0;
    if (amount > 0 && amount < count) // Do not start on 0, pick some random sub subset [Skotlex]
    {
        k = int32(urand(0, uint32(count - 1)));
        amount += k; // Increase final value by same amount to preserve total number to summon.
    }
    int32 hpRate = 0;
    if (BattleConfig.MonsterClassChangeRecover == 0 && (skillID == ROWOW_SKILL_NPC_TRANSFORMATION || skillID == ROWOW_SKILL_NPC_METAMORPHOSIS))
        hpRate = md2->GetMaxHealth() > 0 ? int32(uint64(md2->GetHealth()) * 100 / md2->GetMaxHealth()) : 0;
    ROWOWMobData* masterMob = GetMobData(md2);
    int32 masterX = 0, masterY = 0;
    GetUnitCell(md2, masterX, masterY);
    for (; k < amount; k++)
    {
        uint32 mobID = uint32(value[k % count]); // Summon slaves in round-robin fashion. [Skotlex]
        int32 x = masterX, y = masterY;
        if (MapSearchFreeCell(md2, ROWOW_MOB_SLAVEDISTANCE, x, y) == false)
        {
            x = masterX;
            y = masterY;
        }
        Creature* md = MobOnceSpawnSub(md2, x, y, mobID, ROWOW_AI_NONE, 0);
        if (md == nullptr)
            continue;
        ROWOWMobData* mob = GetMobData(md);
        if (skillID == ROWOW_SKILL_NPC_SUMMONSLAVE)
        {
            mob->MasterID = md2->GetGUID();
            masterMob->SlaveGUIDs.push_back(md->GetGUID());

            // status_calc_mob's slave parts: the master's speed (slaves_inherit_speed) as its base status, then its mode (status_calc_slave_mode)
            ROWOWStatus const* masterBase = GetROBaseStatus(md2);
            ROWOWStatus const* slaveBase = GetROBaseStatus(md);
            if (masterBase != nullptr && slaveBase != nullptr && (BattleConfig.SlavesInheritSpeed & ((masterBase->Mode & ROWOW_MODE_CANMOVE) != 0 ? 1 : 2)) != 0)
            {
                ROWOWMobBaseStatus* baseStatus = md->CustomData.GetDefault<ROWOWMobBaseStatus>("ROWOWMobBaseStatus");
                baseStatus->Status = *slaveBase;
                baseStatus->Status.Speed = std::max(masterBase->Speed, ROWOW_MIN_WALK_SPEED);
            }
            ROWOWStatus const* masterStatus = GetROStatus(md2);
            switch (BattleConfig.SlavesInheritMode)
            {
                case 0:
                    break;
                case 1: // Always aggressive
                    if ((GetROStatus(md)->Mode & ROWOW_MODE_AGGRESSIVE) == 0)
                        SCStart4(md, md, ROWOW_SC_MODECHANGE, 100, 1, 0, ROWOW_MODE_AGGRESSIVE, 0, ROWOW_INFINITE_TICK);
                    break;
                case 2: // Always passive
                    if ((GetROStatus(md)->Mode & ROWOW_MODE_AGGRESSIVE) != 0)
                        SCStart4(md, md, ROWOW_SC_MODECHANGE, 100, 1, 0, 0, ROWOW_MODE_AGGRESSIVE, ROWOW_INFINITE_TICK);
                    break;
                case 4: // Overwrite with slave mode
                    SCStart4(md, md, ROWOW_SC_MODECHANGE, 100, 1, ROWOW_MODE_CANMOVE | ROWOW_MODE_NORANDOMWALK | ROWOW_MODE_CANATTACK, 0, 0, ROWOW_INFINITE_TICK);
                    break;
                default: // Copy master
                    if (masterStatus != nullptr && (masterStatus->Mode & ROWOW_MODE_AGGRESSIVE) != 0)
                        SCStart4(md, md, ROWOW_SC_MODECHANGE, 100, 1, 0, ROWOW_MODE_AGGRESSIVE, 0, ROWOW_INFINITE_TICK);
                    else
                        SCStart4(md, md, ROWOW_SC_MODECHANGE, 100, 1, 0, 0, ROWOW_MODE_AGGRESSIVE, ROWOW_INFINITE_TICK);
                    break;
            }
            StatusCalcBL(md, std::bitset<ROWOW_SCB_MAX>().set(ROWOW_SCB_SPEED).set(ROWOW_SCB_MODE));
        }
        if (hpRate != 0) // Scale HP
            md->SetHealth(std::max<uint32>(1, uint32(uint64(md->GetMaxHealth()) * uint32(hpRate) / 100)));
        ClifSkillNoDamage(md, md, skillID, 1);
    }
}

uint32 ROWOWMod::MobRandomClass(int32 const* value) const
{
    // mob_random_class: one of the five that exist (the first ones until one doesn't).  Pick a random value, hoping it exists. [Skotlex]
    int32 count = 0;
    while (count < 5 && GetMobCreatureEntry(uint32(value[count])) != 0)
        count++;
    return count > 0 ? uint32(value[urand(0, uint32(count - 1))]) : 0;
}

void ROWOWMod::MobWarpSlave(Creature* md, int32 range)
{
    // mob_warpslave: the slaves to a free cell around the master
    if (range < 1)
        range = 1; // Min range needed to avoid crashes and stuff. [Skotlex]
    MobCountSlave(md);
    Position masterPosition = md->GetPosition();
    for (ObjectGuid const& slaveGUID : GetMobData(md)->SlaveGUIDs)
        if (Creature* slave = ObjectAccessor::GetCreature(*md, slaveGUID))
            UnitWarpNear(slave, masterPosition);
}

void ROWOWMod::MobDeleteSlave(Creature* md)
{
    // mob_deleteslave: its slaves die with it (status_kill: no exp, no drops)
    ROWOWMobData* mob = GetMobData(md);
    std::vector<ObjectGuid> slaveGUIDs;
    slaveGUIDs.swap(mob->SlaveGUIDs);
    for (ObjectGuid const& slaveGUID : slaveGUIDs)
    {
        Creature* slave = ObjectAccessor::GetCreature(*md, slaveGUID);
        if (slave == nullptr || slave->IsAlive() == false)
            continue;
        StatusKill(slave);
    }
}

Player* ROWOWMod::MobGetMVPPlayer(Creature* md)
{
    // mob_data::get_mvp_player: most damage dealt and tanked among those on the map and alive.  There cannot be an MVP player if the monster is not an MVP
    ROWOWStatus const* status = GetROStatus(md);
    ROWOWDamageLog const* damageLog = md->CustomData.Get<ROWOWDamageLog>("ROWOWDamageLog");
    if (status == nullptr || (status->Mode & ROWOW_MODE_MVP) == 0 || damageLog == nullptr)
        return nullptr;
    uint64 mvpDamage = 0;
    Player* mvpSD = nullptr;
    for (ROWOWDamageLogEntry const& entry : damageLog->Entries)
    {
        Player* sd = ObjectAccessor::GetPlayer(*md, entry.PlayerGUID);
        if (sd == nullptr || sd->IsAlive() == false || IsInROForm(sd) == false || IsOnSameROMap(sd, md) == false)
            continue;
        if (mvpDamage >= entry.Damage + entry.DamageTanked)
            continue;
        mvpDamage = entry.Damage + entry.DamageTanked;
        mvpSD = sd;
    }
    return mvpSD;
}

int32 ROWOWMod::MobDeadAttackerCount(Creature* md)
{
    // mob_dead's count: only logged into same map chars are counted for the total (on a stitched map, the same part).  A monster that came back
    // (mob_revive) counts its self damage out
    ROWOWDamageLog const* damageLog = md->CustomData.Get<ROWOWDamageLog>("ROWOWDamageLog");
    int32 count = 0;
    if (damageLog != nullptr)
        for (ROWOWDamageLogEntry const& entry : damageLog->Entries)
            if (Player* sd = ObjectAccessor::GetPlayer(*md, entry.PlayerGUID))
                if (IsOnSameROMap(sd, md) == true)
                    count++;
    return count;
}

void ROWOWMod::MobDeadMVP(Creature* md, Player* mvpSD, int32 count, ROWOWLootPriority const& priority, bool isNoRewards)
{
    // mob_dead's MVP reward: mvp.spr over the MVP for those around (clif_mvp_effect), its exp and its item.  The item goes straight to the bags (pc_additem), or
    // what doesn't fit to the ground at the MVP's feet with the MVP's long protection.  type&3 is none of the rewards
    uint32 mobID = GetROMobID(md);
    int64 mobRowIndex = GetMobRowIndex(mobID);
    ROWOWDataTable const* mobTable = GetDataTable(ROWOW_DATA_TABLE_MOB);
    if (mobRowIndex < 0 || mobTable == nullptr)
        return;
    PlayScriptLook(mvpSD, ROWOW_SCRIPT_LOOK_EMOTION, "mvp"); // clif_mvp_effect: the converter's look of mvp.spr, kept with the emotions
    if (isNoRewards == true)
        return;
    ChatHandler handler(mvpSD->GetSession());

    // mob_db's mexp at mvp_exp_rate, then exp_bonus_attacker for each extra attacker, the mapflag noexp aside
    int64 mexp = mobTable->GetInt(size_t(mobRowIndex), mobTable->GetColumnIndex("MvpExp"), 0) * BattleConfig.MvpExpRate / 100;
    if (mexp > 0)
    {
        if (BattleConfig.ExpBonusAttacker > 0 && count > 1)
        {
            count = std::min(count, BattleConfig.ExpBonusMaxAttacker);
            mexp += mexp * (BattleConfig.ExpBonusAttacker * (count - 1)) / 100;
        }
        mexp = std::clamp<int64>(mexp, 1, std::numeric_limits<uint32>::max());
        handler.PSendSysMessage("Congratulations! You are the MVP! Your reward EXP Points are {} !!", mexp); // clif_mvp_exp
        PCGainExp(mvpSD, uint64(mexp), 0);
    }

    // Order might be random depending on item_drop_mvp_mode config setting: only one item is given unless it's 2
    auto mvpDropsIter = MobMvpDropsByMobID.find(mobID);
    if (mvpDropsIter == MobMvpDropsByMobID.end() || HasMapFlag(md, "nomvploot") == true)
        return;
    std::vector<ROWOWMobDrop> mdrop = mvpDropsIter->second;
    if (BattleConfig.ItemDropMvpMode == 1)
    {
        for (size_t i = mdrop.size(); i > 1; i--) // rnd_vector_order
            std::swap(mdrop[i - 1], mdrop[urand(0, uint32(i - 1))]);
    }
    for (ROWOWMobDrop const& entry : mdrop)
    {
        int32 rate = entry.Rate;
        if (rate != 10000)
        {
            if (rate <= 0 && BattleConfig.DropRate0Item == 0)
                rate = 1;
            if (int32(urand(0, 9999)) >= rate) // if ==0, then it doesn't drop
                continue;
        }
        handler.PSendSysMessage("Congratulations! You are the MVP! Your reward item is {} !!", GetROItemName(entry.ItemID)); // clif_mvp_item
        bool isIdentified = ItemDBIsIdentified(entry.ItemID);
        if (PCAddItem(mvpSD, entry.ItemTemplateID, 1, isIdentified) == false)
        {
            handler.SendSysMessage("You cannot get the item."); // clif_additem
            int32 x = 0, y = 0;
            GetUnitCell(mvpSD, x, y);
            ROWOWDropItem item;
            item.ItemTemplateID = entry.ItemTemplateID;
            item.IsIdentified = isIdentified;
            MapAddFloorItem(mvpSD->GetMap(), item, x, y, mvpSD->GetGUID(), priority.SecondSD != nullptr ? priority.SecondSD->GetGUID() : ObjectGuid::Empty,
                priority.ThirdSD != nullptr ? priority.ThirdSD->GetGUID() : ObjectGuid::Empty, ROWOW_FLOORITEM_MVP, ROWOW_RO_DIRECTION_CENTER, false);
        }
        if (BattleConfig.ItemDropMvpMode != 2)
            break; // If item_drop_mvp_mode is not 2, then only one item should be granted
    }
}

void ROWOWMod::MobDead(Creature* md, Unit* killer)
{
    // mob_dead's rewards, in its order: the loot priority and the MVP (the first attacker bonus doesn't count for the MVP), the exp, the drops, the MVP reward,
    // then the slaves.  status_kill's type (no exp, no drops) leaves only what it looted
    if (GetROMobID(md) == 0)
        return;
    ROWOWMobData* mob = GetMobData(md);
    bool isNoRewards = mob->IsNoRewards;
    mob->IsNoRewards = false;
    ROWOWLootPriority priority;
    MobDeadLootPriority(md, killer, priority);
    ROWOWStatus const* status = GetROStatus(md);
    Player* mvpSD = BattleConfig.MvpToLootPriority == 0 ? MobGetMVPPlayer(md) : (status != nullptr && (status->Mode & ROWOW_MODE_MVP) != 0 ? priority.FirstSD : nullptr);
    int32 count = MobDeadAttackerCount(md);
    if (isNoRewards == false)
        MobDeadExp(md, killer);

    // The master or Mercenary can increase the kill count, if the monster level is greater or equal than half the baselevel of the master
    if (Player* sd = killer != nullptr ? const_cast<Player*>(BattleGetMaster(killer)->ToPlayer()) : nullptr; sd != nullptr && (killer->IsPlayer() == true || GetFollowerType(killer) == ROWOW_BL_MER) &&
        GetMercenary(sd) != nullptr && status != nullptr && status->Level >= GetIdentity(sd)->BaseLevel / 2)
        MercenaryKills(sd);
    MobDeadDrops(md, killer, priority, isNoRewards);
    if (mvpSD != nullptr)
        MobDeadMVP(md, mvpSD, count, priority, isNoRewards);
    MobDeleteSlave(md);
}

void ROWOWMod::MobRevive(Creature* md, int32 perHP)
{
    // status_revive and mob_revive, the monster not having died in WoW's eyes: its share of HP, its target gone, an idle state.  We reset the damage log, so
    // the lost HP is no one's (rAthena sets it as self damage so players don't get exp for it)
    ROWOWMobData* mob = GetMobData(md);
    uint64 tick = GetTick();
    uint32 hp = uint32(uint64(md->GetMaxHealth()) * uint32(perHP) / 100);
    md->SetHealth(std::clamp<uint32>(hp, 1, md->GetMaxHealth()));
    MobSetState(md, mob, ROWOW_MSS_IDLE);
    mob->NextWalkTime = tick + urand(0, 999) + 4000; // MIN_RANDOMWALKTIME
    mob->LastLinkTime = tick;
    mob->LastPCNearTime = 0;
    mob->TargetID.Clear();
    mob->AttackedID.Clear();
    mob->NormAttackedID.Clear();
    md->CustomData.Erase("ROWOWDamageLog");
    mob->IsSpawnSkillPending = true;
}

bool ROWOWMod::MobBeforeDeath(Creature* md, Unit* src, uint32& damage)
{
    // status_damage, at a hit that kills a monster.  WoW's death is still to come, so its dead skills can run as mob_dead's do at 1 HP.  A monster that comes back
    // doesn't die here: it gets its rewards given as mob_dead gives them, then Kaizel or NPC_REBIRTH stand it up at once (RO shows no death for it).  True took the
    // damage away
    ROWOWMobData* mob = GetMobData(md);
    uint64 tick = GetTick();
    if (src != nullptr && src != md) // Use Dead skill only if not killed by Script or Command
    {
        MobSetState(md, mob, ROWOW_MSS_DEAD);
        MobSkillUse(md, tick, -1);
        if (md->IsAlive() == false)
            return false;
        // A dead skill's cast time can't run out: the cast ends with the monster (unit_skillcastcancel)
    }
    ROWOWStatusChangeEntry const* kaizel = GetSCE(md, ROWOW_SC_KAIZEL);
    ROWOWStatusChangeEntry const* rebirth = mob->IsRebirth == false ? GetSCE(md, ROWOW_SC_REBIRTH) : nullptr;
    if (kaizel == nullptr && rebirth == nullptr)
        return false;
    int32 perHP = kaizel != nullptr ? kaizel->Val2 : rebirth->Val2;
    int32 kaizelLevel = kaizel != nullptr ? kaizel->Val1 : 0;
    UnitSkillCastCancel(md, 0);
    MobDead(md, src);
    mob->IsRebirth = true;
    damage = 0;
    MobRevive(md, perHP);
    StatusChangeClear(md, 0);
    if (kaizelLevel != 0)
    {
        ClifSkillNoDamage(md, md, ROWOW_SKILL_ALL_RESURRECTION, 1);
        SCStart(src, md, ROWOW_SC_KYRIE, 100, 10, SkillGetTime2(ROWOW_SKILL_SL_KAIZEL, uint16(kaizelLevel)));
    }
    return true;
}
