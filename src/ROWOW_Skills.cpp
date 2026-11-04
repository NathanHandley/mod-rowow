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
#include "DatabaseEnv.h"
#include "Group.h"
#include "Log.h"
#include "Player.h"
#include "QueryResult.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include "ROWOW.h"
#include "ROWOW_JobMapIDs.h"

using namespace std;

// RO skills: the port of rAthena's skill tree and skill learning (pc.cpp's pc_calc_skilltree, pc_skillup and pc_jobchange, and the
// skill_tree.yml loader).  The skill data is loaded at startup and only read after; an identity's skills are only touched in its
// player's own thread

namespace
{
    // The character variable pc_jobchange keeps the job level a character changed to its second job at (rAthena's JOBCHANGE2ND_VAR)
    char const* const JobChange2ndVariable = "jobchange_level";

    // The longest skill list one addon message carries (the client drops longer addon messages)
    constexpr size_t SkillMessageChunkLength = 200;

    std::string ToUpper(std::string text)
    {
        for (char& curChar : text)
            curChar = char(toupper((unsigned char)curChar));
        return text;
    }

    std::string ToLower(std::string text)
    {
        for (char& curChar : text)
            curChar = char(tolower((unsigned char)curChar));
        return text;
    }

    // A name as rAthena matches it to its constants: uppercase, without underscores
    std::string GetConstantKey(std::string const& name)
    {
        std::string key;
        for (char curChar : name)
            if (curChar != '_')
                key.push_back(char(toupper((unsigned char)curChar)));
        return key;
    }

    // The names of rAthena's skill constants, in their enums' order (without the prefix and underscores)
    char const* const NKNames[ROWOW_NK_MAX] = { "NODAMAGE", "SPLASH", "SPLASHSPLIT", "IGNOREATKCARD", "IGNOREELEMENT", "IGNOREDEFENSE", "IGNOREFLEE",
        "IGNOREDEFCARD", "CRITICAL", "IGNORELONGCARD", "SIMPLEDEFENSE" };
    char const* const Inf2Names[ROWOW_INF2_MAX] = { "ISQUEST", "ISNPC", "ISWEDDING", "ISSPIRIT", "ISGUILD", "ISSONG", "ISENSEMBLE", "ISTRAP", "TARGETSELF",
        "NOTARGETSELF", "PARTYONLY", "GUILDONLY", "NOTARGETENEMY", "ISAUTOSHADOWSPELL", "ISCHORUS", "IGNOREBGREDUCTION", "IGNOREGVGREDUCTION", "DISABLENEARNPC",
        "TARGETTRAP", "IGNORELANDPROTECTOR", "ALLOWWHENHIDDEN", "ALLOWWHENPERFORMING", "TARGETEMPERIUM", "IGNOREKAGEHUMI", "ALTERRANGEVULTURE",
        "ALTERRANGESNAKEEYE", "ALTERRANGESHADOWJUMP", "ALTERRANGERADIUS", "ALTERRANGERESEARCHTRAP", "IGNOREHOVERING", "ALLOWONWARG", "ALLOWONMADO",
        "TARGETMANHOLE", "TARGETHIDDEN", "INCREASEDANCEWITHWUGDAMAGE", "IGNOREWUGBITE", "IGNOREAUTOGUARD", "IGNORECICADA", "SHOWSCALE", "IGNOREGTB",
        "TOGGLEABLE", "IGNORENONCRITATKBONUS" };
    char const* const UnitFlagNames[ROWOW_UF_MAX] = { "NONE", "NOENEMY", "NOREITERATION", "NOFOOTSET", "NOOVERLAP", "PATHCHECK", "NOPC", "NOMOB", "SKILL",
        "DANCE", "ENSEMBLE", "SONG", "DUALMODE", "NOKNOCKBACK", "RANGEDSINGLEUNIT", "CRAZYWEEDIMMUNE", "REMOVEDBYFIRERAIN", "KNOCKBACKGROUP", "HIDDENTRAP" };
    char const* const RequireStateNames[] = { "NONE", "HIDDEN", "RIDING", "FALCON", "CART", "SHIELD", "RECOVERWEIGHTRATE", "MOVEENABLE", "WATER",
        "RIDINGDRAGON", "WUG", "RIDINGWUG", "MADO", "ELEMENTALSPIRIT", "ELEMENTALSPIRIT2", "PECO", "SUNSTANCE", "MOONSTANCE", "STARSTANCE", "UNIVERSESTANCE" };
    char const* const WeaponTypeNames[ROWOW_WEAPON_TYPE_MAX] = { "FIST", "DAGGER", "1HSWORD", "2HSWORD", "1HSPEAR", "2HSPEAR", "1HAXE", "2HAXE", "MACE",
        "2HMACE", "STAFF", "BOW", "KNUCKLE", "MUSICAL", "WHIP", "BOOK", "KATAR", "REVOLVER", "RIFLE", "GATLING", "SHOTGUN", "GRENADE", "HUUMA", "2HSTAFF" };
    char const* const AmmoTypeNames[] = { "NONE", "ARROW", "DAGGER", "BULLET", "SHELL", "GRENADE", "SHURIKEN", "KUNAI", "CANNONBALL", "THROWWEAPON" };
    std::pair<char const*, int32> const BattleCheckTargetNames[] = { { "NOONE", ROWOW_BCT_NOONE }, { "SELF", ROWOW_BCT_SELF }, { "ENEMY", ROWOW_BCT_ENEMY },
        { "PARTY", ROWOW_BCT_PARTY }, { "GUILDALLY", ROWOW_BCT_GUILDALLY }, { "NEUTRAL", ROWOW_BCT_NEUTRAL }, { "SAMEGUILD", ROWOW_BCT_SAMEGUILD },
        { "ALL", ROWOW_BCT_ALL }, { "WOS", ROWOW_BCT_WOS }, { "SLAVE", ROWOW_BCT_SLAVE }, { "GUILD", ROWOW_BCT_GUILD }, { "NOGUILD", ROWOW_BCT_NOGUILD },
        { "NOPARTY", ROWOW_BCT_NOPARTY }, { "NOENEMY", ROWOW_BCT_NOENEMY }, { "ALLY", ROWOW_BCT_ALLY }, { "FRIEND", ROWOW_BCT_FRIEND } };

    template<size_t N>
    int32 FindConstantIndex(char const* const (&names)[N], std::string const& name)
    {
        std::string key = GetConstantKey(name);
        for (size_t index = 0; index < N; index++)
            if (key == names[index])
                return int32(index);
        return -1;
    }

    // A "Key=true,..." cell's names that are on, as indexes into the constant names
    template<size_t N, size_t B>
    void ReadFlags(std::string const& cell, char const* const (&names)[N], std::bitset<B>& flagsOut)
    {
        for (auto const& flag : ROWOWDataTable::ParseMap(cell))
        {
            int32 index = FindConstantIndex(names, flag.first);
            if (index >= 0 && size_t(index) < B)
                flagsOut[size_t(index)] = flag.second == "true";
        }
    }

    // SkillDatabase::parseNode: a single value fills every level; a per-level list ("Level=1,Size=9;...") sets the levels it names, then
    // carries on a linear trend it finds in them past the last named level (stepping by up to half the levels named, never crossing from
    // positive to below 1), or else repeats the last named level
    void ReadSkillLevelValues(std::string const& cell, int32 (&valuesOut)[ROWOW_MAX_SKILL_LEVEL])
    {
        if (cell.empty() == true)
            return;
        if (cell.find('=') == std::string::npos)
        {
            int32 value = int32(strtol(cell.c_str(), nullptr, 10));
            for (int32& levelValue : valuesOut)
                levelValue = value;
            return;
        }
        size_t maxLevel = 0;
        for (auto const& entry : ROWOWDataTable::ParseMapList(cell))
        {
            size_t level = 0;
            std::string const* valueText = nullptr;
            for (auto const& field : entry)
            {
                if (field.first == "Level")
                    level = size_t(strtoul(field.second.c_str(), nullptr, 10));
                else
                    valueText = &field.second;
            }
            if (level == 0 || level > ROWOW_MAX_SKILL_LEVEL || valueText == nullptr)
                continue;
            valuesOut[level - 1] = int32(strtol(valueText->c_str(), nullptr, 10));
            maxLevel = std::max(maxLevel, level);
        }
        if (maxLevel == 0)
            return;
        size_t i = maxLevel;
        for (size_t step = 1; step <= i / 2; step++)
        {
            int32 diff = valuesOut[i - 1] - valuesOut[i - step - 1];
            size_t j;
            for (j = i - 1; j >= step; j--)
                if ((valuesOut[j] - valuesOut[j - step]) != diff)
                    break;
            if (j >= step) // No match, try next step
                continue;
            for (; i < ROWOW_MAX_SKILL_LEVEL; i++) // Apply linear increase
            {
                valuesOut[i] = valuesOut[i - step] + diff;
                if (valuesOut[i] < 1 && valuesOut[i - 1] >= 0) // Check if we have switched from + to -, cap the decrease to 0 in said cases
                {
                    valuesOut[i] = 1;
                    diff = 0;
                    step = 1;
                }
            }
            return;
        }
        for (; i < ROWOW_MAX_SKILL_LEVEL; i++)
            valuesOut[i] = valuesOut[maxLevel - 1];
    }

    // A skill's Element: an element name or one of the special skill elements, as one value or per level
    int32 GetSkillElementByName(std::string const& name)
    {
        std::string key = GetConstantKey(name);
        if (key == "WEAPON")
            return ROWOW_ELEMENT_WEAPON;
        if (key == "ENDOWED")
            return ROWOW_ELEMENT_ENDOWED;
        if (key == "RANDOM")
            return ROWOW_ELEMENT_RANDOM;
        int32 element = ROWOWMod::GetElementByName(name);
        return element >= 0 ? element : ROWOW_ELEMENT_NEUTRAL;
    }

    int32 GetStatusIDByName(std::string const& name)
    {
        std::string key = GetConstantKey(name);
        for (auto const& statusName : ROWOWStatusNames)
            if (GetConstantKey(statusName.first) == key)
                return statusName.second;
        return ROWOW_SC_NONE;
    }

    void SendAddonMessage(Player* player, std::string const& message)
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_ADDON, nullptr, nullptr, message);
        player->SendDirectMessage(&data);
    }

    // A transcendent job's normal job (Lord_Knight's Knight, Swordman_High's Swordman), and a normal first job's transcendent one
    uint16 GetNormalJobID(uint16 jobID)
    {
        return (jobID >= ROWOW_JOB_UPPER_START && jobID <= ROWOW_JOB_UPPER_END) ? uint16(jobID - ROWOW_JOB_UPPER_START) : jobID;
    }
    uint16 GetUpperJobID(uint16 normalJobID)
    {
        return uint16(ROWOW_JOB_UPPER_START + normalJobID);
    }
}

void ROWOWMod::LoadSkillData()
{
    SkillsByID.clear();
    SkillIDsByName.clear();
    SkillIDsBySpellID.clear();
    SkillLevelsBySpellID.clear();
    SkillTreesByJobID.clear();
    SkillImplsBySkillID.clear();
    if (IsEnabled == false)
        return;

    // The skills (rows are in rAthena's load order, so a later one wins)
    ROWOWDataTable const* skillTable = GetDataTable(ROWOW_DATA_TABLE_SKILL);
    if (skillTable == nullptr)
    {
        LOG_ERROR("module.ROWOW", "ROWOWMod::LoadSkillData found no {} table, so RO identities learn no skills", ROWOW_DATA_TABLE_SKILL);
        return;
    }
    int32 idColumnIndex = skillTable->GetColumnIndex("Id");
    int32 nameColumnIndex = skillTable->GetColumnIndex("Name");
    int32 descriptionColumnIndex = skillTable->GetColumnIndex("Description");
    int32 maxLevelColumnIndex = skillTable->GetColumnIndex("MaxLevel");
    int32 targetTypeColumnIndex = skillTable->GetColumnIndex("TargetType");
    int32 flagsColumnIndex = skillTable->GetColumnIndex("Flags");
    for (size_t rowIndex = 0; rowIndex < skillTable->Rows.size(); rowIndex++)
    {
        ROWOWSkill skill;
        skill.SkillID = uint16(skillTable->GetInt(rowIndex, idColumnIndex, 0));
        skill.Name = skillTable->GetCell(rowIndex, nameColumnIndex);
        if (skill.SkillID == 0 || skill.Name.empty() == true)
            continue;
        skill.Description = skillTable->GetCell(rowIndex, descriptionColumnIndex);
        skill.MaxLevel = uint16(std::max<int64>(1, skillTable->GetInt(rowIndex, maxLevelColumnIndex, 1)));
        LoadSkillRow(*skillTable, rowIndex, skill);
        skill.IsPassive = skill.Inf == ROWOW_INF_PASSIVE_SKILL;
        skill.IsQuest = skill.Inf2[ROWOW_INF2_ISQUEST];
        skill.IsWedding = skill.Inf2[ROWOW_INF2_ISWEDDING];
        skill.IsSpirit = skill.Inf2[ROWOW_INF2_ISSPIRIT];
        SkillIDsByName[ToUpper(skill.Name)] = skill.SkillID;
        SkillsByID[skill.SkillID] = skill;
    }

    // The WoW spell of each skill level (the converter makes this table, so it may not exist yet)
    QueryResult tableResult = WorldDatabase.Query("SELECT `table_name` FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = 'mod_rowow_skill_spell'");
    if (tableResult)
    {
        if (QueryResult result = WorldDatabase.Query("SELECT `SkillName`, `SkillLevel`, `SpellID` FROM `mod_rowow_skill_spell`"))
        {
            do
            {
                Field* fields = result->Fetch();
                uint16 skillID = GetSkillIDByName(fields[0].Get<std::string>());
                uint32 level = fields[1].Get<uint32>();
                uint32 spellID = fields[2].Get<uint32>();
                auto skillIter = SkillsByID.find(skillID);
                if (skillIter == SkillsByID.end() || level == 0 || level > skillIter->second.MaxLevel)
                    continue;
                std::vector<uint32>& spellIDsByLevel = skillIter->second.SpellIDsByLevel;
                if (spellIDsByLevel.size() < level)
                    spellIDsByLevel.resize(level, 0);
                spellIDsByLevel[level - 1] = spellID;
                SkillIDsBySpellID[spellID] = skillID;
                SkillLevelsBySpellID[spellID] = uint16(level);
            } while (result->NextRow());
        }
    }
    else
        LOG_ERROR("module.ROWOW", "ROWOWMod::LoadSkillData found no mod_rowow_skill_spell table, so the RO skills learned have no spells (run the converter)");

    // The skill trees: each job's own (SkillTreeDatabase::parseBodyNode), then those it inherits (loadingFinished)
    std::unordered_map<std::string, uint16> jobIDsByName;
    for (auto const& jobByID : JobsByID)
        jobIDsByName[GetJobNameKey(jobByID.second.Name)] = jobByID.first;
    if (ROWOWDataTable const* treeTable = GetDataTable(ROWOW_DATA_TABLE_SKILL_TREE))
    {
        int32 jobColumnIndex = treeTable->GetColumnIndex("Job");
        int32 inheritColumnIndex = treeTable->GetColumnIndex("Inherit");
        int32 treeNameColumnIndex = treeTable->GetColumnIndex("Tree_Name");
        int32 treeMaxLevelColumnIndex = treeTable->GetColumnIndex("Tree_MaxLevel");
        int32 treeBaseLevelColumnIndex = treeTable->GetColumnIndex("Tree_BaseLevel");
        int32 treeJobLevelColumnIndex = treeTable->GetColumnIndex("Tree_JobLevel");
        int32 treeRequiresColumnIndex = treeTable->GetColumnIndex("Tree_Requires");
        int32 treeExcludeColumnIndex = treeTable->GetColumnIndex("Tree_Exclude");
        for (size_t rowIndex = 0; rowIndex < treeTable->Rows.size(); rowIndex++)
        {
            // A job that isn't one of the pre-renewal ones here (the baby jobs, the Peco Knight's own tree) is left out, as is a skill
            // that isn't in the skill database
            auto jobIDIter = jobIDsByName.find(GetJobNameKey(treeTable->GetCell(rowIndex, jobColumnIndex)));
            if (jobIDIter == jobIDsByName.end())
                continue;
            uint16 jobID = jobIDIter->second;
            ROWOWSkillTree& tree = SkillTreesByJobID[jobID];
            for (auto const& inherit : ROWOWDataTable::ParseMap(treeTable->GetCell(rowIndex, inheritColumnIndex)))
            {
                auto inheritJobIDIter = jobIDsByName.find(GetJobNameKey(inherit.first));
                if (inheritJobIDIter == jobIDsByName.end())
                    continue;
                auto existingIter = std::find(tree.InheritJobIDs.begin(), tree.InheritJobIDs.end(), inheritJobIDIter->second);
                if (inherit.second != "true")
                {
                    if (existingIter != tree.InheritJobIDs.end())
                        tree.InheritJobIDs.erase(existingIter);
                }
                else if (existingIter == tree.InheritJobIDs.end())
                    tree.InheritJobIDs.push_back(inheritJobIDIter->second);
            }

            std::string const& skillName = treeTable->GetCell(rowIndex, treeNameColumnIndex);
            if (skillName.empty() == true)
                continue;
            uint16 skillID = GetSkillIDByName(skillName);
            ROWOWSkill const* skill = GetSkill(skillID);
            if (skill == nullptr)
            {
                LOG_ERROR("module.ROWOW", "ROWOWMod::LoadSkillData found skill '{}' in job {}'s tree, which isn't in {}", skillName, jobID, ROWOW_DATA_TABLE_SKILL);
                continue;
            }
            ROWOWJob const* job = GetJob(jobID);
            bool skillExists = tree.SkillsByID.find(skillID) != tree.SkillsByID.end();
            ROWOWSkillTreeEntry& entry = tree.SkillsByID[skillID];
            entry.SkillID = skillID;
            entry.MaxLevel = uint16(std::min<int64>(treeTable->GetInt(rowIndex, treeMaxLevelColumnIndex, 0), skill->MaxLevel));
            if (treeTable->GetCell(rowIndex, treeBaseLevelColumnIndex).empty() == false)
                entry.BaseLevel = uint32(std::min<int64>(treeTable->GetInt(rowIndex, treeBaseLevelColumnIndex, 0), job != nullptr ? job->MaxBaseLevel : 99));
            else if (skillExists == false)
                entry.BaseLevel = 0;
            if (treeTable->GetCell(rowIndex, treeJobLevelColumnIndex).empty() == false)
                entry.JobLevel = uint32(std::min<int64>(treeTable->GetInt(rowIndex, treeJobLevelColumnIndex, 0), job != nullptr ? job->MaxJobLevel : 50));
            else if (skillExists == false)
                entry.JobLevel = 0;
            for (auto const& require : ROWOWDataTable::ParseMapList(treeTable->GetCell(rowIndex, treeRequiresColumnIndex)))
            {
                uint16 needSkillID = 0;
                uint16 needLevel = 0;
                for (auto const& field : require)
                {
                    if (field.first == "Name")
                        needSkillID = GetSkillIDByName(field.second);
                    else if (field.first == "Level")
                        needLevel = uint16(strtoul(field.second.c_str(), nullptr, 10));
                }
                ROWOWSkill const* needSkill = GetSkill(needSkillID);
                if (needSkill == nullptr)
                    continue;
                needLevel = std::min(needLevel, needSkill->MaxLevel);
                auto needIter = std::find_if(entry.Needs.begin(), entry.Needs.end(), [needSkillID](std::pair<uint16, uint16> const& need) { return need.first == needSkillID; });
                if (needLevel == 0)
                {
                    if (needIter != entry.Needs.end())
                        entry.Needs.erase(needIter);
                }
                else if (needIter != entry.Needs.end())
                    needIter->second = needLevel;
                else
                    entry.Needs.emplace_back(needSkillID, needLevel);
            }
            std::string const& excludeCell = treeTable->GetCell(rowIndex, treeExcludeColumnIndex);
            if (excludeCell.empty() == false)
                entry.IsExcludedFromInherit = excludeCell == "true";
            else if (skillExists == false)
                entry.IsExcludedFromInherit = false;
        }
    }
    else
        LOG_ERROR("module.ROWOW", "ROWOWMod::LoadSkillData found no {} table, so RO identities learn no skills", ROWOW_DATA_TABLE_SKILL_TREE);

    // Each job gets the skills of the jobs it inherits (their own trees, as loaded) that it doesn't list itself, the last inherited
    // job's winning, with the base and job level needs capped to its own maximums.  Skills with a max level of 0 are taken out after
    std::unordered_map<uint16, std::map<uint16, ROWOWSkillTreeEntry>> inheritedSkillsByJobID;
    for (auto const& treeByJobID : SkillTreesByJobID)
    {
        ROWOWJob const* job = GetJob(treeByJobID.first);
        std::map<uint16, ROWOWSkillTreeEntry>& inheritedSkills = inheritedSkillsByJobID[treeByJobID.first];
        for (uint16 inheritJobID : treeByJobID.second.InheritJobIDs)
        {
            auto inheritTreeIter = SkillTreesByJobID.find(inheritJobID);
            if (inheritTreeIter == SkillTreesByJobID.end())
                continue;
            for (auto const& entryBySkillID : inheritTreeIter->second.SkillsByID)
            {
                if (entryBySkillID.second.IsExcludedFromInherit == true || treeByJobID.second.SkillsByID.find(entryBySkillID.first) != treeByJobID.second.SkillsByID.end())
                    continue;
                ROWOWSkillTreeEntry entry = entryBySkillID.second;
                if (job != nullptr)
                {
                    entry.BaseLevel = std::min(entry.BaseLevel, job->MaxBaseLevel);
                    entry.JobLevel = std::min(entry.JobLevel, job->MaxJobLevel);
                }
                inheritedSkills[entryBySkillID.first] = entry;
            }
        }
    }
    uint32 treeSkillCount = 0;
    for (auto& treeByJobID : SkillTreesByJobID)
    {
        for (auto const& entryBySkillID : inheritedSkillsByJobID[treeByJobID.first])
            treeByJobID.second.SkillsByID.insert(entryBySkillID);
        for (auto entryIter = treeByJobID.second.SkillsByID.begin(); entryIter != treeByJobID.second.SkillsByID.end();)
        {
            if (entryIter->second.MaxLevel == 0)
                entryIter = treeByJobID.second.SkillsByID.erase(entryIter);
            else
                ++entryIter;
        }
        treeSkillCount += uint32(treeByJobID.second.SkillsByID.size());
    }

    // Each skill's own rules (rAthena's skill_factory), by job
    AddROWOWSkillsNovice();
    AddROWOWSkillsSwordman();
    AddROWOWSkillsMage();
    AddROWOWSkillsAcolyte();
    AddROWOWSkillsArcher();
    AddROWOWSkillsMerchant();
    AddROWOWSkillsThief();
    AddROWOWSkillsTaekwon();
    AddROWOWSkillsGunslinger();
    AddROWOWSkillsNinja();
    AddROWOWSkillsNpc();
    AddROWOWSkillsHomunculus();
    AddROWOWSkillsMercenary();
    LOG_INFO("module.ROWOW", "ROWOWMod::LoadSkillData loaded {} skills ({} skill spells, {} with their own rules) and {} job skill trees ({} skills)", SkillsByID.size(),
        SkillIDsBySpellID.size(), SkillImplsBySkillID.size(), SkillTreesByJobID.size(), treeSkillCount);
}

int32 ROWOWMod::PartyForEachSameMap(Player* sd, int32 range, std::function<int32(Unit*)> const& func) const
{
    // party_foreachsamemap: each member of the player's party (its WoW group) on the player's map and in the form, within range cells of it
    // when there's a range, the player too.  Members on the same map share its thread
    Group* group = sd->GetGroup();
    if (group == nullptr)
        return 0;
    int32 x = 0, y = 0;
    GetUnitCell(sd, x, y);
    std::vector<Unit*> members;
    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* member = itr->GetSource();
        if (member == nullptr || member->IsInWorld() == false || IsOnSameROMap(member, sd) == false || GetROStatus(member) == nullptr)
            continue;
        int32 memberX = 0, memberY = 0;
        GetUnitCell(member, memberX, memberY);
        if (range != 0 && (memberX < x - range || memberY < y - range || memberX > x + range || memberY > y + range))
            continue;
        members.push_back(member);
    }
    int32 total = 0;
    for (Unit* member : members)
        total += func(member);
    return total;
}

bool ROWOWMod::PCCheckWeaponType(Unit const* unit, uint32 weapons) const
{
    // pc_check_weapontype: the weapon type held, or with two weapons the dual wield type and each hand's
    ROWOWStatus const* status = GetROStatus(unit);
    if (status == nullptr)
        return false;
    if (status->WeaponType < ROWOW_WEAPON_TYPE_MAX)
        return (weapons & (1u << status->WeaponType)) != 0;
    return (weapons & ((1u << status->RightWeaponType) | (1u << status->LeftWeaponType) | (1u << status->WeaponType))) != 0;
}

void ROWOWMod::LoadSkillRow(ROWOWDataTable const& skillTable, size_t rowIndex, ROWOWSkill& skill) const
{
    // The rest of SkillDatabase::parseBodyNode: everything the skill engine reads
    auto cell = [&skillTable, rowIndex](char const* columnName) -> std::string const& { return skillTable.GetCell(rowIndex, skillTable.GetColumnIndex(columnName)); };
    std::string type = GetConstantKey(cell("Type"));
    skill.AttackType = type == "WEAPON" ? ROWOW_BF_WEAPON : (type == "MAGIC" ? ROWOW_BF_MAGIC : (type == "MISC" ? ROWOW_BF_MISC : 0));
    std::string targetType = GetConstantKey(cell("TargetType"));
    skill.Inf = targetType == "ATTACK" ? ROWOW_INF_ATTACK_SKILL : targetType == "GROUND" ? ROWOW_INF_GROUND_SKILL : targetType == "SELF" ? ROWOW_INF_SELF_SKILL :
        targetType == "SUPPORT" ? ROWOW_INF_SUPPORT_SKILL : targetType == "TRAP" ? ROWOW_INF_TRAP_SKILL : ROWOW_INF_PASSIVE_SKILL;
    std::string hit = GetConstantKey(cell("Hit"));
    skill.Hit = hit == "SINGLE" ? ROWOW_DMG_SINGLE : (hit == "MULTIHIT" ? ROWOW_DMG_MULTI_HIT : ROWOW_DMG_NORMAL);
    ReadFlags(cell("DamageFlags"), NKNames, skill.NK);
    ReadFlags(cell("Flags"), Inf2Names, skill.Inf2);
    for (auto const& flag : ROWOWDataTable::ParseMap(cell("CastTimeFlags")))
        if (flag.second == "true")
            skill.CastTimeFlags |= GetConstantKey(flag.first) == "IGNOREDEX" ? ROWOW_SKILL_CAST_IGNOREDEX : GetConstantKey(flag.first) == "IGNORESTATUS" ?
                ROWOW_SKILL_CAST_IGNORESTATUS : GetConstantKey(flag.first) == "IGNOREITEMBONUS" ? ROWOW_SKILL_CAST_IGNOREITEMBONUS : 0;
    for (auto const& flag : ROWOWDataTable::ParseMap(cell("CastDelayFlags")))
        if (flag.second == "true")
            skill.CastDelayFlags |= GetConstantKey(flag.first) == "IGNOREDEX" ? ROWOW_SKILL_CAST_IGNOREDEX : GetConstantKey(flag.first) == "IGNORESTATUS" ?
                ROWOW_SKILL_CAST_IGNORESTATUS : GetConstantKey(flag.first) == "IGNOREITEMBONUS" ? ROWOW_SKILL_CAST_IGNOREITEMBONUS : 0;
    if (cell("CastCancel").empty() == false)
        skill.IsCastCancel = cell("CastCancel") == "true";
    skill.CastDefenseReduction = int32(strtol(cell("CastDefenseReduction").c_str(), nullptr, 10));
    std::string const& status = cell("Status");
    skill.StatusID = status.empty() == true ? ROWOW_SC_NONE : GetStatusIDByName(status);
    for (auto const& copyFlag : ROWOWDataTable::ParseMap(cell("CopyFlags_Skill")))
        if (copyFlag.second == "true")
            skill.CopyOption |= copyFlag.first == "Plagiarism" ? ROWOW_SKILL_COPY_PLAGIARISM : (copyFlag.first == "Reproduce" ? ROWOW_SKILL_COPY_REPRODUCE : 0);

    ReadSkillLevelValues(cell("Range"), skill.Range);
    ReadSkillLevelValues(cell("HitCount"), skill.HitCount);
    ReadSkillLevelValues(cell("SplashArea"), skill.SplashArea);
    ReadSkillLevelValues(cell("Knockback"), skill.Knockback);
    ReadSkillLevelValues(cell("CastTime"), skill.CastTime);
    ReadSkillLevelValues(cell("AfterCastActDelay"), skill.AfterCastActDelay);
    ReadSkillLevelValues(cell("AfterCastWalkDelay"), skill.AfterCastWalkDelay);
    ReadSkillLevelValues(cell("Cooldown"), skill.Cooldown);
    ReadSkillLevelValues(cell("Duration1"), skill.Duration1);
    ReadSkillLevelValues(cell("Duration2"), skill.Duration2);
    ReadSkillLevelValues(cell("ActiveInstance"), skill.MaxCount);
    ReadSkillLevelValues(cell("Requires_HpCost"), skill.HPCost);
    ReadSkillLevelValues(cell("Requires_SpCost"), skill.SPCost);
    ReadSkillLevelValues(cell("Requires_HpRateCost"), skill.HPRateCost);
    ReadSkillLevelValues(cell("Requires_SpRateCost"), skill.SPRateCost);
    ReadSkillLevelValues(cell("Requires_ZenyCost"), skill.ZenyCost);
    ReadSkillLevelValues(cell("Requires_SpiritSphereCost"), skill.SpiritSphereCost);
    ReadSkillLevelValues(cell("Requires_AmmoAmount"), skill.AmmoAmount);
    skill.ActiveInstance = skill.MaxCount[0];

    // The element, by name for one value or every level
    std::string const& element = cell("Element");
    if (element.find('=') == std::string::npos)
    {
        int32 elementValue = element.empty() == true ? ROWOW_ELEMENT_NEUTRAL : GetSkillElementByName(element);
        for (int32& levelElement : skill.Element)
            levelElement = elementValue;
    }
    else
    {
        size_t maxLevel = 0;
        for (auto const& entry : ROWOWDataTable::ParseMapList(element))
        {
            size_t level = 0;
            int32 elementValue = ROWOW_ELEMENT_NEUTRAL;
            for (auto const& field : entry)
            {
                if (field.first == "Level")
                    level = size_t(strtoul(field.second.c_str(), nullptr, 10));
                else
                    elementValue = GetSkillElementByName(field.second);
            }
            if (level == 0 || level > ROWOW_MAX_SKILL_LEVEL)
                continue;
            skill.Element[level - 1] = elementValue;
            maxLevel = std::max(maxLevel, level);
        }
        for (size_t level = maxLevel; maxLevel > 0 && level < ROWOW_MAX_SKILL_LEVEL; level++)
            skill.Element[level] = skill.Element[maxLevel - 1];
    }

    // The weapons, ammo, state, statuses, items and equipment it needs
    for (auto const& weapon : ROWOWDataTable::ParseMap(cell("Requires_Weapon")))
    {
        int32 weaponType = FindConstantIndex(WeaponTypeNames, weapon.first);
        if (weaponType >= 0 && weapon.second == "true")
            skill.RequiredWeapons |= 1u << weaponType;
    }
    for (auto const& ammo : ROWOWDataTable::ParseMap(cell("Requires_Ammo")))
    {
        int32 ammoType = FindConstantIndex(AmmoTypeNames, ammo.first);
        if (ammoType > 0 && ammo.second == "true")
            skill.RequiredAmmo |= 1u << ammoType;
    }
    std::string const& state = cell("Requires_State");
    if (state.empty() == false)
        skill.RequiredState = std::max(0, FindConstantIndex(RequireStateNames, state));
    for (auto const& requiredStatus : ROWOWDataTable::ParseMap(cell("Requires_Status")))
    {
        int32 statusID = GetStatusIDByName(requiredStatus.first);
        if (statusID != ROWOW_SC_NONE && requiredStatus.second == "true")
            skill.RequiredStatuses.push_back(statusID);
    }
    for (auto const& entry : ROWOWDataTable::ParseMapList(cell("Requires_ItemCost")))
    {
        ROWOWSkillItemRequirement requirement;
        for (auto const& field : entry)
        {
            if (field.first == "Item")
            {
                std::string lowerName = field.second;
                for (char& curChar : lowerName)
                    curChar = char(tolower((unsigned char)curChar));
                auto itemIDIter = ItemIDsByAegisName.find(lowerName);
                requirement.ItemID = itemIDIter != ItemIDsByAegisName.end() ? itemIDIter->second : 0;
            }
            else if (field.first == "Amount")
                requirement.Amount = int32(strtol(field.second.c_str(), nullptr, 10));
            else if (field.first == "Level")
                requirement.Level = uint16(strtoul(field.second.c_str(), nullptr, 10));
        }
        if (requirement.ItemID != 0)
            skill.RequiredItems.push_back(requirement);
        else
            LOG_ERROR("module.ROWOW", "ROWOWMod::LoadSkillData found an item skill {} needs that isn't a converted RO item", skill.Name);
    }
    for (auto const& equipment : ROWOWDataTable::ParseMap(cell("Requires_Equipment")))
    {
        std::string lowerName = equipment.first;
        for (char& curChar : lowerName)
            curChar = char(tolower((unsigned char)curChar));
        auto itemIDIter = ItemIDsByAegisName.find(lowerName);
        if (itemIDIter != ItemIDsByAegisName.end() && equipment.second == "true")
            skill.RequiredEquipment.push_back(itemIDIter->second);
    }

    // Its ground units: the units as UNT_ constants, BCT_ALL and BL_CHAR (less the UF_NOPC and UF_NOMOB kinds) by default
    skill.UnitID = cell("Unit_Id");
    skill.UnitAlternateID = cell("Unit_AlternateId");
    for (auto const& unitName : ROWOWSkillUnitNames)
    {
        if (skill.UnitID.empty() == false && GetConstantKey(skill.UnitID) == GetConstantKey(unitName.first))
            skill.UnitTypeID = unitName.second;
        if (skill.UnitAlternateID.empty() == false && GetConstantKey(skill.UnitAlternateID) == GetConstantKey(unitName.first))
            skill.UnitTypeID2 = unitName.second;
    }
    if (skill.UnitID.empty() == false && skill.UnitTypeID == 0)
        LOG_ERROR("module.ROWOW", "ROWOWMod::LoadSkillData found skill {}'s Unit Id {}, which isn't a unit", skill.Name, skill.UnitID);
    ReadSkillLevelValues(cell("Unit_Layout"), skill.UnitLayout);
    ReadSkillLevelValues(cell("Unit_Range"), skill.UnitRange);
    skill.UnitInterval = int32(strtol(cell("Unit_Interval").c_str(), nullptr, 10));
    std::string unitTarget = GetConstantKey(cell("Unit_Target"));
    if (skill.UnitID.empty() == false && unitTarget.empty() == true)
        skill.UnitTarget = ROWOW_BCT_ALL;
    for (auto const& targetName : BattleCheckTargetNames)
        if (unitTarget == targetName.first)
            skill.UnitTarget = targetName.second;
    ReadFlags(cell("Unit_Flag"), UnitFlagNames, skill.UnitFlags);
    skill.UnitBLTarget = (skill.UnitFlags[ROWOW_UF_NOPC] == true ? 0 : ROWOW_BL_PC) | (skill.UnitFlags[ROWOW_UF_NOMOB] == true ? 0 : ROWOW_BL_MOB);
}

ROWOWSkill const* ROWOWMod::GetSkill(uint16 skillID) const
{
    auto skillIter = SkillsByID.find(skillID);
    if (skillIter == SkillsByID.end())
        return nullptr;
    return &skillIter->second;
}

uint16 ROWOWMod::GetSkillIDByName(std::string const& skillName) const
{
    auto skillIDIter = SkillIDsByName.find(ToUpper(skillName));
    if (skillIDIter == SkillIDsByName.end())
        return 0;
    return skillIDIter->second;
}

ROWOWSkillTreeEntry const* ROWOWMod::GetSkillTreeEntry(uint16 jobID, uint16 skillID) const
{
    auto treeIter = SkillTreesByJobID.find(jobID);
    if (treeIter == SkillTreesByJobID.end())
        return nullptr;
    auto entryIter = treeIter->second.SkillsByID.find(skillID);
    if (entryIter == treeIter->second.SkillsByID.end())
        return nullptr;
    return &entryIter->second;
}

int32 ROWOWMod::PCCheckSkill(Unit const* unit, uint16 skillID) const
{
    // pc_checkskill, for a player in the form (a monster knows no player skills)
    if (unit == nullptr || unit->IsPlayer() == false)
        return 0;
    ROWOWIdentity const* identity = unit->ToPlayer()->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    if (identity == nullptr || identity->IsInForm == false)
        return 0;
    return PCCheckSkill(*identity, skillID);
}

int32 ROWOWMod::PCCheckSkill(ROWOWIdentity const& identity, uint16 skillID) const
{
    // pc_checkskill: a skill counts while it's on the tree the identity may use (pc_calc_skilltree gave it its id), and the skill Plagiarism
    // copied at its copied level.  A skill its equipment grants, or that it has for now (a spirit link's), counts at that level
    if (skillID != 0 && skillID == identity.CloneSkillID)
        return identity.CloneSkillLevel;
    if (auto itemIter = identity.ItemSkillLevelsByID.find(skillID); itemIter != identity.ItemSkillLevelsByID.end())
        return itemIter->second;
    if (auto tempIter = identity.TempSkillLevelsByID.find(skillID); tempIter != identity.TempSkillLevelsByID.end())
        return tempIter->second;
    if (identity.TreeSkillIDs.find(skillID) == identity.TreeSkillIDs.end())
        return 0;
    auto levelIter = identity.SkillLevelsByID.find(skillID);
    return levelIter != identity.SkillLevelsByID.end() ? levelIter->second : 0;
}

uint16 ROWOWMod::SkillTreeGetMax(uint16 skillID, uint16 jobID) const
{
    // skill_tree_get_max
    if (ROWOWSkillTreeEntry const* entry = GetSkillTreeEntry(jobID, skillID))
        return entry->MaxLevel;
    ROWOWSkill const* skill = GetSkill(skillID);
    return skill != nullptr ? skill->MaxLevel : 0;
}

uint32 ROWOWMod::PCCalcSkillPoint(ROWOWIdentity const& identity) const
{
    // pc_calc_skillpoint: the levels learned, without the quest skills (unless quest_skill_learn) and the wedding skills
    uint32 skillPoints = 0;
    for (auto const& levelBySkillID : identity.SkillLevelsByID)
    {
        ROWOWSkill const* skill = GetSkill(levelBySkillID.first);
        if (skill == nullptr || levelBySkillID.second == 0)
            continue;
        if ((skill->IsQuest == false || BattleConfig.QuestSkillLearn != 0) && (skill->IsWedding == false || skill->IsSpirit == true))
            skillPoints += levelBySkillID.second;
    }
    return skillPoints;
}

void ROWOWMod::GetSkillUpLimitPoints(ROWOWIdentity const& identity, int32& firstJobSkillPointsOut, int32& secondJobSkillPointsOut) const
{
    // skillup_limit's skill points used before the first job's skills open (the Novice's basic skill points: its max job level less 1), and
    // before a second job's open (those and the first job's: the job level it changed at less 1).  0 for none
    firstJobSkillPointsOut = 0;
    secondJobSkillPointsOut = 0;
    ROWOWJob const* job = GetJob(identity.JobID);
    if (BattleConfig.SkillUpLimit == 0 || job == nullptr)
        return;
    ROWOWJob const* noviceJob = GetJob(ROWOW_JOB_NOVICE);
    firstJobSkillPointsOut = noviceJob != nullptr ? int32(noviceJob->MaxJobLevel) - 1 : 9;
    if (job->FirstJobID > 0)
    {
        int64 changeLevel = GetIdentityVariable(identity, JobChange2ndVariable);
        if (changeLevel == 0)
        {
            ROWOWJob const* firstJob = GetJob(uint16(job->FirstJobID));
            changeLevel = firstJob != nullptr ? firstJob->MaxJobLevel : 0;
        }
        secondJobSkillPointsOut = firstJobSkillPointsOut + int32(changeLevel) - 1;
    }
}

uint16 ROWOWMod::PCCalcSkillTreeNormalizeJob(ROWOWIdentity const& identity) const
{
    // pc_calc_skilltree_normalize_job: with skillup_limit, a job's skills wait until the Novice's basic skill points (the Novice's max job
    // level less 1) and a second job's first job points (the job level it changed at less 1) are used.  A transcendent job stays one
    ROWOWJob const* job = GetJob(identity.JobID);
    if (BattleConfig.SkillUpLimit == 0 || job == nullptr)
        return identity.JobID;
    uint16 normalizedJobID = identity.JobID;
    int32 skillPoints = int32(PCCalcSkillPoint(identity));
    int32 firstJobSkillPoints = 0;
    int32 secondJobSkillPoints = 0;
    GetSkillUpLimitPoints(identity, firstJobSkillPoints, secondJobSkillPoints);
    if (skillPoints < firstJobSkillPoints)
        normalizedJobID = ROWOW_JOB_NOVICE;
    else if (job->FirstJobID > 0 && skillPoints < secondJobSkillPoints)
        normalizedJobID = uint16(job->FirstJobID);
    if (normalizedJobID == identity.JobID)
        return identity.JobID;

    // The special masks: a transcendent job's tree stays transcendent where there is one, and a Baby's stays a Baby's
    if (job->IsUpper == true && GetJob(GetUpperJobID(normalizedJobID)) != nullptr)
        return GetUpperJobID(normalizedJobID);
    if (job->IsBaby == true && GetBabyJobID(normalizedJobID) != 0)
        return GetBabyJobID(normalizedJobID);
    return normalizedJobID;
}

void ROWOWMod::PCCalcSkillTree(ROWOWIdentity& identity) const
{
    // pc_calc_skilltree: the skills of the (normalized) job's tree it may learn or use now, those whose needed skills it learned and
    // whose base and job levels it reached, found again until no more are added.  Quest and wedding skills only once learned; spirit skills
    // only while a Soul Linker's spirit links it, then at level 1 for now (SKILL_FLAG_TEMPORARY: not saved, not raised)
    identity.TreeSkillIDs.clear();
    identity.TempSkillLevelsByID.clear();
    identity.CloneSkillID = 0;
    identity.CloneSkillLevel = 0;
    uint16 jobID = PCCalcSkillTreeNormalizeJob(identity);
    auto treeIter = SkillTreesByJobID.find(jobID);
    if (treeIter == SkillTreesByJobID.end())
        return;
    auto getLearnedLevel = [&identity](uint16 skillID) -> uint16
    {
        auto levelIter = identity.SkillLevelsByID.find(skillID);
        return levelIter != identity.SkillLevelsByID.end() ? levelIter->second : 0;
    };
    // SKILL_FLAG_PERM_GRANTED: a skill a quest granted ignores the skill tree
    for (auto const& levelBySkillID : identity.SkillLevelsByID)
        if (ROWOWSkill const* skill = GetSkill(levelBySkillID.first); skill != nullptr && skill->IsQuest == true &&
            GetIdentityVariable(identity, fmt::format(GrantedSkillVariableFormat, levelBySkillID.first)) != 0)
            identity.TreeSkillIDs.insert(levelBySkillID.first);
    bool isChanged = true;
    while (isChanged == true)
    {
        isChanged = false;
        for (auto const& entryBySkillID : treeIter->second.SkillsByID)
        {
            uint16 skillID = entryBySkillID.first;
            if (identity.TreeSkillIDs.find(skillID) != identity.TreeSkillIDs.end())
                continue;
            ROWOWSkillTreeEntry const& entry = entryBySkillID.second;
            if (BattleConfig.SkillFree == 0)
            {
                bool isFailed = false;
                for (auto const& need : entry.Needs)
                {
                    uint16 needLevel = identity.TreeSkillIDs.find(need.first) != identity.TreeSkillIDs.end() ? getLearnedLevel(need.first) : 0;
                    if (needLevel < need.second)
                    {
                        isFailed = true;
                        break;
                    }
                }
                if (identity.BaseLevel < entry.BaseLevel || identity.JobLevel < entry.JobLevel)
                    isFailed = true;
                if (isFailed == true)
                    continue;
            }
            ROWOWSkill const* skill = GetSkill(skillID);
            if (skill == nullptr)
                continue;
            if (getLearnedLevel(skillID) == 0 && ((skill->IsQuest == true && BattleConfig.QuestSkillLearn == 0) || skill->IsWedding == true ||
                (skill->IsSpirit == true && identity.SpiritSkillID == 0)))
                continue; // Cannot be learned via normal means. Note this check DOES allows raising already known skills.
            identity.TreeSkillIDs.insert(skillID);
            if (skill->IsSpirit == true) // Spirit skills cannot be learned, they will only show up on your tree when you get buffed.
                identity.TempSkillLevelsByID[skillID] = 1;
            isChanged = true;
        }
    }

    // Taekwon Ranker Bonus Skill Tree: its whole tree, but only as bonus skills in case it drops from ranking.  Not for a Novice's tree, and only
    // once its skill points are spent, to avoid problems with the job change quest
    if (jobID != ROWOW_JOB_NOVICE && identity.SkillPoints == 0 && PCIsTaekwonRanker(identity) == true)
    {
        for (auto const& entryBySkillID : treeIter->second.SkillsByID)
        {
            ROWOWSkill const* skill = GetSkill(entryBySkillID.first);
            if (skill == nullptr || skill->IsQuest == true || skill->IsWedding == true)
                continue; // Do not include Quest/Wedding skills.
            identity.TreeSkillIDs.insert(entryBySkillID.first);
            identity.TempSkillLevelsByID[entryBySkillID.first] = SkillTreeGetMax(entryBySkillID.first, identity.JobID);
        }
    }

    // Enable Bard/Dancer spirit linked skills: the other's song at 10 for each of its own it knows at 10.  (rAthena picks by sex; the Bard
    // and Clown are the male jobs, the Dancer and Gypsy the female)
    if (identity.SpiritSkillID == ROWOW_SKILL_SL_BARDDANCER)
    {
        static std::pair<uint16, uint16> const linkedSkills[] = { { ROWOW_SKILL_BA_WHISTLE, ROWOW_SKILL_DC_HUMMING },
            { ROWOW_SKILL_BA_ASSASSINCROSS, ROWOW_SKILL_DC_DONTFORGETME }, { ROWOW_SKILL_BA_POEMBRAGI, ROWOW_SKILL_DC_FORTUNEKISS },
            { ROWOW_SKILL_BA_APPLEIDUN, ROWOW_SKILL_DC_SERVICEFORYOU } };
        bool isMale = identity.JobID == ROWOW_JOB_BARD || identity.JobID == ROWOW_JOB_CLOWN || identity.JobID == ROWOW_JOB_BABY_BARD;
        for (auto const& skill : linkedSkills)
        {
            if (PCCheckSkill(identity, isMale == true ? skill.first : skill.second) < 10)
                continue;
            identity.TempSkillLevelsByID[isMale == true ? skill.second : skill.first] = 10; // Tag it as a non-savable, non-uppable, bonus skill
        }
    }

    // pc_reg_received: the skill Plagiarism copied (CLONE_SKILL), at most at the level of Plagiarism it knows now.  A skill of its own tree is
    // never a copy (skill_isCopyable)
    int32 plagiarismLevel = PCCheckSkill(identity, ROWOW_SKILL_RG_PLAGIARISM);
    uint16 cloneSkillID = uint16(GetIdentityVariable(identity, ROWOW_SKILL_VAR_PLAGIARISM));
    if (plagiarismLevel > 0 && GetSkill(cloneSkillID) != nullptr && identity.TreeSkillIDs.find(cloneSkillID) == identity.TreeSkillIDs.end())
    {
        identity.CloneSkillID = cloneSkillID;
        identity.CloneSkillLevel = uint16(std::clamp<int64>(GetIdentityVariable(identity, ROWOW_SKILL_VAR_PLAGIARISM_LV), 0, plagiarismLevel));
    }
}

bool ROWOWMod::PCSkillUp(Player* player, uint16 skillID)
{
    // pc_skillup: one more level of a skill on the tree it may raise now, for a skill point, up to the job's max level of it
    ROWOWIdentity* identity = GetIdentity(player);
    if (identity->Exists == false || GetSkill(skillID) == nullptr)
        return false;
    auto levelIter = identity->SkillLevelsByID.find(skillID);
    uint16 level = levelIter != identity->SkillLevelsByID.end() ? levelIter->second : 0;
    if (identity->SkillPoints == 0 || identity->TreeSkillIDs.find(skillID) == identity->TreeSkillIDs.end() || level >= SkillTreeGetMax(skillID, identity->JobID) ||
        identity->TempSkillLevelsByID.find(skillID) != identity->TempSkillLevelsByID.end() || PCCheckSkillIsGranted(*identity, skillID) == true)
        return false;
    identity->SkillLevelsByID[skillID] = level + 1;
    identity->SkillPoints--;
    identity->AreSkillsChanged = true;
    RefreshROForm(player);
    return true;
}

void ROWOWMod::PCSkill(Player* player, uint16 skillID, uint16 level)
{
    // pc_skill with ADDSKILL_PERMANENT: sets the level, 0 taking the skill away
    ROWOWIdentity* identity = GetIdentity(player);
    ROWOWSkill const* skill = GetSkill(skillID);
    if (identity->Exists == false || skill == nullptr)
        return;
    level = std::min(level, skill->MaxLevel);
    if (level == 0)
        identity->SkillLevelsByID.erase(skillID);
    else
        identity->SkillLevelsByID[skillID] = level;
    identity->AreSkillsChanged = true;
    RefreshROForm(player);
}

void ROWOWMod::PCAddTempSkill(ROWOWIdentity& identity, uint16 skillID, int32 level, bool isAddLevel) const
{
    // pc_skill's ADDSKILL_TEMP (the higher of what it has and the level) and ADDSKILL_TEMP_ADDLEVEL (added to what it has)
    if (GetSkill(skillID) == nullptr || level <= 0)
        return;
    int32 current = PCCheckSkill(identity, skillID);
    identity.ItemSkillLevelsByID[skillID] = uint16(isAddLevel == true ? current + level : std::max(current, level));
}

bool ROWOWMod::PCCheckSkillIsGranted(ROWOWIdentity const& identity, uint16 skillID) const
{
    return identity.ItemSkillLevelsByID.find(skillID) != identity.ItemSkillLevelsByID.end();
}

void ROWOWMod::PCRefreshItemSkills(Player* player, ROWOWIdentity& identity)
{
    // status_calc_pc_sub's "if the skills changed, clif_skillinfoblock": each status calculation made them again
    if (identity.ItemSkillLevelsByID == identity.SentItemSkillLevelsByID)
        return;
    identity.SentItemSkillLevelsByID = identity.ItemSkillLevelsByID;
    GrantIdentitySkillSpells(player, identity);
    SendSkillsToClient(player, identity, false);
}

void ROWOWMod::PCJobChange(Player* player, uint16 jobID)
{
    // pc_jobchange: job level 1 again, and the job level a first job changed to a second job at kept for the skill tree.  The skills and
    // skill points stay
    ROWOWIdentity* identity = GetIdentity(player);
    ROWOWJob const* job = GetJob(jobID);
    ROWOWJob const* oldJob = GetJob(identity->JobID);
    if (identity->Exists == false || job == nullptr || jobID == identity->JobID)
        return;
    if (job->FirstJobID >= 0 && (oldJob == nullptr || oldJob->FirstJobID < 0))
        SetIdentityVariable(*identity, JobChange2ndVariable, identity->JobLevel);
    if (identity->CloneSkillID != 0)
        PCSkillPlagiarismReset(player);
    if (identity->JobID == ROWOW_JOB_STAR_GLADIATOR && jobID != ROWOW_JOB_STAR_GLADIATOR)
    {
        // going off star glad lineage, reset feel and hate to not store no-longer-used vars in the database
        PCResetFeel(player);
        PCResetHate(player);
    }
    StatusChangeEnd(player, ROWOW_SC_SPIRIT); // A spirit links the old job only
    bool wasUpper = oldJob != nullptr && oldJob->IsUpper == true;
    int32 oldFameType = GetFameType(identity->JobID);
    uint8 oldFameRank = oldFameType >= 0 ? PCFameRank(player->GetGUID().GetCounter(), oldFameType) : 0;
    identity->JobID = jobID;
    identity->JobLevel = 1;
    identity->JobExp = 0;
    if (identity->BaseLevel > job->MaxBaseLevel)
    {
        identity->BaseLevel = job->MaxBaseLevel;
        identity->BaseExp = 0;
        PCResetState(*identity);
    }

    // Give or reduce transcendent status points
    uint32 transcendentStatusPoints = uint32(std::max(0, BattleConfig.TranscendentStatusPoints));
    if (job->IsUpper == true && wasUpper == false) // Change from a non t class to a t class -> give points
        identity->StatusPoints += transcendentStatusPoints;
    else if (job->IsUpper == false && wasUpper == true) // Change from a t class to a non t class -> remove points
    {
        if (identity->StatusPoints < transcendentStatusPoints)
            PCResetState(*identity); // The player already used his bonus points, so we have to reset his status points
        else
            identity->StatusPoints -= transcendentStatusPoints;
    }
    RefreshROForm(player);

    // if you were previously famous, not anymore; it may be that now they are famous
    if (oldFameRank != 0 || (identity->Fame > 0 && GetFameType(jobID) >= 0))
    {
        LoadFameLists(player->GetGUID().GetCounter());
        if (GetFameType(jobID) >= 0)
            UpdateFameList(GetFameType(jobID), player->GetGUID().GetCounter(), identity->Fame, player->GetName());
    }

    // Remove peco/cart/falcon: the new job keeps them only with Riding, Pushcart and Falconry Mastery
    if (PCCheckSkill(*identity, ROWOW_SKILL_KN_RIDING) == 0)
        PCSetRiding(player, false);
    if (PCCheckSkill(*identity, ROWOW_SKILL_HT_FALCON) == 0)
        PCSetFalcon(player, false);
    if (PCIsCartOn(player) == true && PCCheckSkill(*identity, ROWOW_SKILL_MC_PUSHCART) == 0)
        PCSetCart(player, 0);
    if (HomIsActive(GetHomunculus(player)) == true && PCCheckSkill(*identity, ROWOW_SKILL_AM_CALLHOMUN) == 0)
        HomVaporize(player, ROWOW_HOM_ST_ACTIVE);
}

int64 ROWOWMod::PCJobIDToMapID(int32 jobID) const
{
    // pc_jobid2mapid: -1 for a job with no class
    for (ROWOWJobMapID const& jobMapID : ROWOWJobMapIDs)
        if (jobMapID.JobID == jobID)
            return int64(jobMapID.MapID);
    return -1;
}

int32 ROWOWMod::PCMapIDToJobID(uint64 mapID, bool isMale) const
{
    // pc_mapid2jobid: -1 for a class with no job
    for (ROWOWMapIDJob const& mapIDJob : ROWOWMapIDJobs)
        if (mapIDJob.MapID == mapID)
            return isMale == true ? mapIDJob.MaleJobID : mapIDJob.FemaleJobID;
    return -1;
}

int32 ROWOWMod::PCResetSkill(Player* player)
{
    // pc_resetskill with flag 1 (a reset, not a count): the Peco and falcon go with Riding and Falconry Mastery, then every skill learned with
    // points is forgotten and its points come back.  Kept: wedding and spirit skills, Basic Skill past Novice, granted skills, and quest skills
    // (quest_skill_learn and quest_skill_reset are off); Play Dead past Novice is forgotten without points
    ROWOWIdentity* identity = GetIdentity(player);
    if (identity->Exists == false)
        return 0;
    if (PCIsTaekwonRanker(*identity) == true)
        return 0; // It has been confirmed on official servers that when you reset skills with a ranked Taekwon your skills are not reset
    if (PCIsRiding(player) == true && PCCheckSkill(*identity, ROWOW_SKILL_KN_RIDING) != 0)
        PCSetRiding(player, false);
    if (PCIsFalcon(player) == true && PCCheckSkill(*identity, ROWOW_SKILL_HT_FALCON) != 0)
        PCSetFalcon(player, false);
    if (PCIsCartOn(player) == true && PCCheckSkill(*identity, ROWOW_SKILL_MC_PUSHCART) != 0)
        PCSetCart(player, 0);
    if (HomIsActive(GetHomunculus(player)) == true && PCCheckSkill(*identity, ROWOW_SKILL_AM_CALLHOMUN) != 0)
        HomVaporize(player, ROWOW_HOM_ST_ACTIVE);
    bool isNovice = (PCJobIDToMapID(GetNonBabyJobID(identity->JobID)) & ROWOW_MAPID_SECONDMASK) == 0; // MAPID_NOVICE, normal or High
    int32 skillPoint = 0;
    for (auto levelIter = identity->SkillLevelsByID.begin(); levelIter != identity->SkillLevelsByID.end();)
    {
        uint16 skillID = levelIter->first;
        ROWOWSkill const* skill = GetSkill(skillID);
        if (skill == nullptr || skill->IsWedding == true || skill->IsSpirit == true || skill->IsQuest == true ||
            (skillID == ROWOW_SKILL_NV_BASIC && isNovice == false))
        {
            ++levelIter;
            continue;
        }
        if (skillID == ROWOW_SKILL_NV_TRICKDEAD && isNovice == false)
        {
            levelIter = identity->SkillLevelsByID.erase(levelIter);
            continue;
        }
        skillPoint += levelIter->second;
        levelIter = identity->SkillLevelsByID.erase(levelIter);
    }
    if (skillPoint == 0)
        return 0;
    identity->SkillPoints += skillPoint;
    identity->AreSkillsChanged = true;
    RefreshROForm(player);
    return skillPoint;
}

void ROWOWMod::PCResetState(ROWOWIdentity& identity) const
{
    // pc_resetstate: the base level's status points (statpoint.yml), and the transcendent ones for a transcendent job, with every stat 1
    auto statusPointsIter = StatusPointsByBaseLevel.find(identity.BaseLevel);
    identity.StatusPoints = statusPointsIter != StatusPointsByBaseLevel.end() ? statusPointsIter->second : 0;
    ROWOWJob const* job = GetJob(identity.JobID);
    if (job != nullptr && job->IsUpper == true)
        identity.StatusPoints += uint32(std::max(0, BattleConfig.TranscendentStatusPoints));
    for (uint8 statIndex = 0; statIndex < ROWOW_STAT_COUNT; statIndex++)
        identity.Stats[statIndex] = 1;
}

void ROWOWMod::PCResetLvl(Player* player, int32 type)
{
    // pc_resetlvl (/resetlvl, the Valkyrie's rebirth with type 1): the skills reset first (but for type 3), then type 1 makes base and job
    // level 1 with no skill points, no options and every stat 1 (a High Novice gets 100 status points and First Aid and Play Dead), type 2
    // base and job level 1 with no skill points, type 3 base level 1, type 4 job level 1.  Gear it can't wear any more comes off with the
    // next equipment check (pc_checkitem), as pc_resetlvl's pc_isequip loop takes it off
    ROWOWIdentity* identity = GetIdentity(player);
    if (identity->Exists == false)
        return;
    if (type != 3) // Also reset skills
        PCResetSkill(player);
    if (type == 1)
    {
        identity->SkillPoints = 0;
        identity->BaseLevel = 1;
        identity->JobLevel = 1;
        identity->BaseExp = 0;
        identity->JobExp = 0;
        PCSetRiding(player, false); // sc.option = 0: no Peco, falcon or cart
        PCSetFalcon(player, false);
        if (PCIsCartOn(player) == true)
            PCSetCart(player, 0);
        for (uint8 statIndex = 0; statIndex < ROWOW_STAT_COUNT; statIndex++)
            identity->Stats[statIndex] = 1;
        if (identity->JobID == ROWOW_JOB_NOVICE_HIGH)
        {
            identity->StatusPoints = 100; // not 88 [celest]
            // give platinum skills upon changing
            PCSkill(player, ROWOW_SKILL_NV_FIRSTAID, 1);
            PCSkill(player, ROWOW_SKILL_NV_TRICKDEAD, 1);
        }
    }
    if (type == 2)
    {
        identity->SkillPoints = 0;
        identity->BaseLevel = 1;
        identity->JobLevel = 1;
        identity->BaseExp = 0;
        identity->JobExp = 0;
    }
    if (type == 3)
    {
        identity->BaseLevel = 1;
        identity->BaseExp = 0;
    }
    if (type == 4)
    {
        identity->JobLevel = 1;
        identity->JobExp = 0;
    }
    RefreshROForm(player);
    SendSkillsToClient(player, *identity, true);
}

std::vector<uint16> ROWOWMod::GetSkillTreeJobIDs(uint16 jobID) const
{
    // The skill window's tabs: Novice, the first job, a transcendent second job's normal one, and the job (Lord_Knight: Novice, Swordman,
    // Knight, Lord_Knight; Swordman_High: Novice_High, Swordman_High; Baby_Knight: Baby, Baby_Swordman, Baby_Knight).  The Taekwon jobs,
    // Gunslinger and Ninja come after Novice
    std::vector<uint16> jobIDs;
    ROWOWJob const* job = GetJob(jobID);
    if (job == nullptr)
        return jobIDs;
    bool isUpper = job->IsUpper == true && GetNormalJobID(jobID) != jobID;
    uint16 noviceJobID = isUpper == true ? ROWOW_JOB_NOVICE_HIGH : (job->IsBaby == true ? ROWOW_JOB_BABY : ROWOW_JOB_NOVICE);
    jobIDs.push_back(noviceJobID);
    if (job->FirstJobID >= 0)
    {
        uint16 firstJobID = uint16(job->FirstJobID);
        if (isUpper == true && GetJob(GetUpperJobID(firstJobID)) != nullptr)
            firstJobID = GetUpperJobID(firstJobID);
        else if (job->IsBaby == true && GetBabyJobID(firstJobID) != 0)
            firstJobID = GetBabyJobID(firstJobID);
        jobIDs.push_back(firstJobID);
        if (isUpper == true)
            jobIDs.push_back(GetNormalJobID(jobID));
    }
    if (jobID != noviceJobID)
        jobIDs.push_back(jobID);
    return jobIDs;
}

int64 ROWOWMod::GetIdentityVariable(ROWOWIdentity const& identity, std::string const& name) const
{
    // The scripts' character variables, which keep their names in lowercase as rAthena's aren't case sensitive
    return identity.Variables.Get(ToLower(name), 0).GetNumber();
}

void ROWOWMod::SetIdentityVariable(ROWOWIdentity& identity, std::string const& name, int64 value) const
{
    // A variable of 0 isn't kept, as rAthena deletes it
    identity.Variables.Set(ToLower(name), 0, ROWOWScriptValue::FromNumber(value));
}

void ROWOWMod::RefreshIdentitySkills(Player* player, ROWOWIdentity& identity)
{
    PCCalcSkillTree(identity);
    GrantIdentitySkillSpells(player, identity);
}

void ROWOWMod::SendSkillsToClient(Player* player, ROWOWIdentity& identity, bool isForced)
{
    // ROWOWSKILLS: the skill window's tree, in parts the client puts back together.  "B\t<skill points>\t<job>:<job name>,...\t<skill points used>:<used
    // before the first job's skills open>:<used before the second job's>" (its tabs, and skillup_limit's points, 0 for none), then the skills of
    // the job's tree and any others learned, "S\t<skill>:<level>:<max level>:<1 when it can be learned or raised now>:<1 Plagiarism's copy, 2 equipment's off the
    // tree, 3 equipment's on it, or nothing>:<1 when skill points can raise it at all (no quest, wedding or spirit skill)>:<the skills and levels it needs first,
    // <skill>-<level>/...>[:<the level learned, for equipment's>],...", then "E".  The window plans skill points on the needed skills before a skill as RO's
    // does; the spellbook puts an equipment skill's levels past those learned under its Card Effects tab
    std::vector<std::string> messages;
    std::string jobIDsText;
    for (uint16 jobID : GetSkillTreeJobIDs(identity.JobID))
    {
        ROWOWJob const* job = GetJob(jobID);
        jobIDsText += fmt::format("{}{}:{}", jobIDsText.empty() == true ? "" : ",", jobID, job != nullptr ? job->Name : "");
    }
    int32 firstJobSkillPoints = 0;
    int32 secondJobSkillPoints = 0;
    GetSkillUpLimitPoints(identity, firstJobSkillPoints, secondJobSkillPoints);
    messages.push_back(fmt::format("ROWOWSKILLS\tB\t{}\t{}\t{}:{}:{}", identity.SkillPoints, jobIDsText, PCCalcSkillPoint(identity), firstJobSkillPoints,
        secondJobSkillPoints));
    std::set<uint16> skillIDs;
    auto treeIter = SkillTreesByJobID.find(identity.JobID);
    if (treeIter != SkillTreesByJobID.end())
        for (auto const& entryBySkillID : treeIter->second.SkillsByID)
            skillIDs.insert(entryBySkillID.first);
    for (auto const& levelBySkillID : identity.SkillLevelsByID)
        skillIDs.insert(levelBySkillID.first);
    for (auto const& levelBySkillID : identity.TempSkillLevelsByID)
        skillIDs.insert(levelBySkillID.first);
    for (auto const& levelBySkillID : identity.ItemSkillLevelsByID)
        skillIDs.insert(levelBySkillID.first);
    if (identity.CloneSkillID != 0)
        skillIDs.insert(identity.CloneSkillID);
    std::string skillsText;
    for (uint16 skillID : skillIDs)
    {
        auto levelIter = identity.SkillLevelsByID.find(skillID);
        uint16 level = levelIter != identity.SkillLevelsByID.end() ? levelIter->second : 0;
        if (auto tempIter = identity.TempSkillLevelsByID.find(skillID); tempIter != identity.TempSkillLevelsByID.end())
            level = tempIter->second; // A spirit link's skill, at its level for now
        bool isOnTree = identity.TreeSkillIDs.find(skillID) != identity.TreeSkillIDs.end();
        ROWOWSkill const* skill = GetSkill(skillID);
        ROWOWSkillTreeEntry const* treeEntry = GetSkillTreeEntry(identity.JobID, skillID);
        bool isRaisedWithPoints = skill != nullptr && treeEntry != nullptr && (skill->IsQuest == false || BattleConfig.QuestSkillLearn != 0) &&
            skill->IsWedding == false && skill->IsSpirit == false;
        std::string needsText;
        if (treeEntry != nullptr)
            for (auto const& need : treeEntry->Needs)
                needsText += fmt::format("{}{}-{}", needsText.empty() == true ? "" : "/", need.first, need.second);
        std::string skillText = fmt::format("{}:{}:{}:{}::{}:{}", skillID, level, SkillTreeGetMax(skillID, identity.JobID), isOnTree == true ? 1 : 0,
            isRaisedWithPoints == true ? 1 : 0, needsText);
        if (skillID == identity.CloneSkillID)
            skillText = fmt::format("{}:{}:{}:1:1:0:", skillID, identity.CloneSkillLevel, identity.CloneSkillLevel);
        else if (auto itemIter = identity.ItemSkillLevelsByID.find(skillID); itemIter != identity.ItemSkillLevelsByID.end())
            skillText = fmt::format("{}:{}:{}:1:{}:0::{}", skillID, itemIter->second, std::max<uint16>(itemIter->second, SkillTreeGetMax(skillID, identity.JobID)),
                treeEntry != nullptr ? 3 : 2, levelIter != identity.SkillLevelsByID.end() ? levelIter->second : 0); // Equipment's: 3 for a skill of the job's tree, which shows there unraised
        if (skillsText.empty() == false && skillsText.size() + 1 + skillText.size() > SkillMessageChunkLength)
        {
            messages.push_back("ROWOWSKILLS\tS\t" + skillsText);
            skillsText.clear();
        }
        skillsText += (skillsText.empty() == true ? "" : ",") + skillText;
    }
    if (skillsText.empty() == false)
        messages.push_back("ROWOWSKILLS\tS\t" + skillsText);
    messages.push_back("ROWOWSKILLS\tE");

    std::string allMessages;
    for (std::string const& message : messages)
        allMessages += message + "\n";
    if (isForced == false && allMessages == identity.SentSkillsMessage)
        return;
    identity.SentSkillsMessage = allMessages;
    for (std::string const& message : messages)
        SendAddonMessage(player, message);
}
