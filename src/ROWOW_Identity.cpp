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
#include "GameTime.h"
#include "Item.h"
#include "Log.h"
#include "Mail.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QueryResult.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Tokenize.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include "ROWOW.h"

using namespace std;

// RO identity: one per character, made on its first entry into the RO world (a level 1 Novice at the training ground, as rAthena makes
// a new pre-renewal character).  In the RO world the character is in the identity's form: the job's look in the character's gender,
// the identity's level, HP, SP and zeny, and the form's own spells and action bars.  Leaving the RO world puts the character back.
// The character row (characters) always keeps the character's own level, experience, money, health and powers: while in the form,
// the save hook writes those in place of the form's, and the identity's values to mod_rowow_identity in the same transaction

namespace
{
    // rAthena's pre-renewal jobs: their IDs (e_job in mmo.hpp) and names (the job database's keys)
    struct ROWOWJobName
    {
        uint16 JobID;
        char const* Name;
    };

    ROWOWJobName const PreRenewalJobNames[] =
    {
        { 0, "Novice" }, { 1, "Swordman" }, { 2, "Mage" }, { 3, "Archer" }, { 4, "Acolyte" }, { 5, "Merchant" }, { 6, "Thief" },
        { 7, "Knight" }, { 8, "Priest" }, { 9, "Wizard" }, { 10, "Blacksmith" }, { 11, "Hunter" }, { 12, "Assassin" },
        { 14, "Crusader" }, { 15, "Monk" }, { 16, "Sage" }, { 17, "Rogue" }, { 18, "Alchemist" }, { 19, "Bard" }, { 20, "Dancer" },
        { 23, "Super_Novice" }, { 24, "Gunslinger" }, { 25, "Ninja" },
        { 4001, "Novice_High" }, { 4002, "Swordman_High" }, { 4003, "Mage_High" }, { 4004, "Archer_High" }, { 4005, "Acolyte_High" },
        { 4006, "Merchant_High" }, { 4007, "Thief_High" }, { 4008, "Lord_Knight" }, { 4009, "High_Priest" }, { 4010, "High_Wizard" },
        { 4011, "Whitesmith" }, { 4012, "Sniper" }, { 4013, "Assassin_Cross" }, { 4015, "Paladin" }, { 4016, "Champion" },
        { 4017, "Professor" }, { 4018, "Stalker" }, { 4019, "Creator" }, { 4020, "Clown" }, { 4021, "Gypsy" },
        { 4046, "Taekwon" }, { 4047, "Star_Gladiator" }, { 4049, "Soul_Linker" },
    };

    std::string ToLower(std::string text)
    {
        for (char& curChar : text)
            curChar = char(tolower((unsigned char)curChar));
        return text;
    }

    // "Level=1,Hp=40;Level=2,Hp=45;..." into values by level (index is the level less 1)
    template<typename T>
    void ReadValuesByLevel(std::string const& cell, char const* valueName, std::vector<T>& valuesOut)
    {
        for (auto const& entry : ROWOWDataTable::ParseMapList(cell))
        {
            uint32 level = 0;
            T value = 0;
            bool hasValue = false;
            for (auto const& field : entry)
            {
                if (field.first == "Level")
                    level = uint32(strtoul(field.second.c_str(), nullptr, 10));
                else if (field.first == valueName)
                {
                    value = T(strtoull(field.second.c_str(), nullptr, 10));
                    hasValue = true;
                }
            }
            if (level == 0 || hasValue == false)
                continue;
            if (valuesOut.size() < level)
                valuesOut.resize(level, 0);
            valuesOut[level - 1] = value;
        }
    }

    // The positions are kept as a map (-1 for none) and its coordinates
    void ReadPosition(Field* fields, uint32 firstFieldIndex, ROWOWPosition& positionOut)
    {
        int32 mapID = fields[firstFieldIndex].Get<int32>();
        positionOut.IsSet = mapID >= 0;
        positionOut.MapID = mapID >= 0 ? uint32(mapID) : 0;
        positionOut.X = fields[firstFieldIndex + 1].Get<float>();
        positionOut.Y = fields[firstFieldIndex + 2].Get<float>();
        positionOut.Z = fields[firstFieldIndex + 3].Get<float>();
        positionOut.O = fields[firstFieldIndex + 4].Get<float>();
    }

    int32 GetPositionMapValue(ROWOWPosition const& position)
    {
        return position.IsSet == true ? int32(position.MapID) : -1;
    }

    void SendQuietSpellChange(Player* player, uint32 spellID, bool isLearned)
    {
        // The 1 after the stock fields keeps the client quiet about it (MergedWorldClient's Packet_SMSG_LEARNED_SPELL/REMOVED_SPELL)
        if (isLearned == true)
        {
            WorldPacket data(SMSG_LEARNED_SPELL, 4 + 2 + 1);
            data << uint32(spellID);
            data << uint16(0);
            data << uint8(1);
            player->SendDirectMessage(&data);
        }
        else
        {
            WorldPacket data(SMSG_REMOVED_SPELL, 4 + 1);
            data << uint32(spellID);
            data << uint8(1);
            player->SendDirectMessage(&data);
        }
    }

    // Takes off every aura that isn't passive and is good for the player (buffs, mounts, forms, illusions), but not those the keep
    // test keeps.  Bad ones run out on their own
    template<typename KeepTest>
    void RemovePositiveAuras(Player* player, KeepTest keepTest)
    {
        std::vector<uint32> spellIDsToRemove;
        for (auto const& appliedAura : player->GetAppliedAuras())
        {
            Aura const* aura = appliedAura.second->GetBase();
            if (aura->IsPassive() == true || appliedAura.second->IsPositive() == false || keepTest(aura->GetId()) == true)
                continue;
            spellIDsToRemove.push_back(aura->GetId());
        }
        for (uint32 spellID : spellIDsToRemove)
            player->RemoveAurasDueToSpell(spellID);
    }
}

void ROWOWMod::LoadIdentityData()
{
    JobsByID.clear();
    StatusPointsByBaseLevel.clear();
    MapIDsByMapName.clear();
    ItemTemplateIDsByItemID.clear();
    ItemTemplateIDs.clear();
    StartMapID = 0;
    StartItems.clear();
    StartZeny = 0;
    if (IsEnabled == false)
        return;

    // The jobs, with the values the job database has for each (rows are in rAthena's load order, so a later one wins, as in rAthena)
    std::unordered_map<std::string, uint16> jobIDsByName;
    for (ROWOWJobName const& jobName : PreRenewalJobNames)
    {
        ROWOWJob& job = JobsByID[jobName.JobID];
        job.JobID = jobName.JobID;
        job.Name = jobName.Name;
        job.IsUpper = jobName.JobID >= ROWOW_JOB_UPPER_START && jobName.JobID <= ROWOW_JOB_UPPER_END;
        job.IllusionJobKey = ToLower(job.Name);
        if (job.IllusionJobKey.size() > 5 && job.IllusionJobKey.compare(job.IllusionJobKey.size() - 5, 5, "_high") == 0)
            job.IllusionJobKey.resize(job.IllusionJobKey.size() - 5); // Transcendent first jobs look like the first job
        jobIDsByName[job.Name] = job.JobID;
    }
    if (ROWOWDataTable const* jobTable = GetDataTable(ROWOW_DATA_TABLE_JOB))
    {
        int32 jobsColumnIndex = jobTable->GetColumnIndex("Jobs");
        int32 maxBaseLevelColumnIndex = jobTable->GetColumnIndex("MaxBaseLevel");
        int32 maxJobLevelColumnIndex = jobTable->GetColumnIndex("MaxJobLevel");
        int32 baseHPColumnIndex = jobTable->GetColumnIndex("BaseHp");
        int32 baseSPColumnIndex = jobTable->GetColumnIndex("BaseSp");
        int32 baseExpColumnIndex = jobTable->GetColumnIndex("BaseExp");
        int32 jobExpColumnIndex = jobTable->GetColumnIndex("JobExp");
        for (size_t rowIndex = 0; rowIndex < jobTable->Rows.size(); rowIndex++)
        {
            for (auto const& jobField : ROWOWDataTable::ParseMap(jobTable->GetCell(rowIndex, jobsColumnIndex)))
            {
                auto jobIDIter = jobIDsByName.find(jobField.first);
                if (jobIDIter == jobIDsByName.end() || jobField.second != "true")
                    continue;
                ROWOWJob& job = JobsByID[jobIDIter->second];
                job.MaxBaseLevel = uint32(jobTable->GetInt(rowIndex, maxBaseLevelColumnIndex, job.MaxBaseLevel));
                job.MaxJobLevel = uint32(jobTable->GetInt(rowIndex, maxJobLevelColumnIndex, job.MaxJobLevel));
                ReadValuesByLevel(jobTable->GetCell(rowIndex, baseHPColumnIndex), "Hp", job.BaseHPByLevel);
                ReadValuesByLevel(jobTable->GetCell(rowIndex, baseSPColumnIndex), "Sp", job.BaseSPByLevel);
                ReadValuesByLevel(jobTable->GetCell(rowIndex, baseExpColumnIndex), "Exp", job.BaseExpByLevel);
                ReadValuesByLevel(jobTable->GetCell(rowIndex, jobExpColumnIndex), "Exp", job.JobExpByLevel);
            }
        }
    }
    else
        LOG_ERROR("module.ROWOW", "ROWOWMod::LoadIdentityData found no {} table, so RO identities have 1 HP and SP", ROWOW_DATA_TABLE_JOB);

    // Status points by base level
    if (ROWOWDataTable const* statPointTable = GetDataTable(ROWOW_DATA_TABLE_STATPOINT))
    {
        int32 levelColumnIndex = statPointTable->GetColumnIndex("Level");
        int32 pointsColumnIndex = statPointTable->GetColumnIndex("Points");
        for (size_t rowIndex = 0; rowIndex < statPointTable->Rows.size(); rowIndex++)
            StatusPointsByBaseLevel[uint32(statPointTable->GetInt(rowIndex, levelColumnIndex, 0))] = uint32(statPointTable->GetInt(rowIndex, pointsColumnIndex, 0));
    }

    // RO map names and RO items (the converter creates these tables, so they may not exist yet)
    QueryResult tableResult = WorldDatabase.Query("SELECT `table_name` FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name IN ('mod_rowow_map_name', 'mod_rowow_item')");
    std::unordered_set<std::string> tableNames;
    if (tableResult)
    {
        do
        {
            tableNames.insert(tableResult->Fetch()[0].Get<std::string>());
        } while (tableResult->NextRow());
    }
    if (tableNames.find("mod_rowow_map_name") != tableNames.end())
    {
        if (QueryResult result = WorldDatabase.Query("SELECT `MapName`, `MapID` FROM `mod_rowow_map_name`"))
        {
            do
            {
                Field* fields = result->Fetch();
                MapIDsByMapName[ToLower(fields[0].Get<std::string>())] = fields[1].Get<uint32>();
            } while (result->NextRow());
        }
    }
    if (tableNames.find("mod_rowow_item") != tableNames.end())
    {
        if (QueryResult result = WorldDatabase.Query("SELECT `ItemTemplateID`, `ItemID` FROM `mod_rowow_item`"))
        {
            do
            {
                Field* fields = result->Fetch();
                ItemTemplateIDsByItemID[fields[1].Get<uint32>()] = fields[0].Get<uint32>();
                ItemTemplateIDs.insert(fields[0].Get<uint32>());
            } while (result->NextRow());
        }
    }

    // What a new identity starts with: rAthena's pre-renewal new character values.  rAthena picks one of the start points at random to
    // spread new characters over copies of the training ground; there's one copy here, so it's always the first
    if (ROWOWDataTable const* charConfigTable = GetDataTable(ROWOW_DATA_TABLE_CHAR_CONFIG))
    {
        int32 keyColumnIndex = charConfigTable->GetColumnIndex("Key");
        int32 valueColumnIndex = charConfigTable->GetColumnIndex("Value");
        for (size_t rowIndex = 0; rowIndex < charConfigTable->Rows.size(); rowIndex++)
        {
            std::string const& key = charConfigTable->GetCell(rowIndex, keyColumnIndex);
            std::string const& value = charConfigTable->GetCell(rowIndex, valueColumnIndex);
            if (key == "start_point_pre")
            {
                // "<map>,<x>,<y>:<map>,<x>,<y>:..."
                std::vector<std::string_view> pointFields = Acore::Tokenize(std::string_view(value).substr(0, value.find(':')), ',', false);
                if (pointFields.size() < 3)
                    continue;
                StartMapID = GetMapIDByMapName(std::string(pointFields[0]));
                StartCellX = int32(strtol(std::string(pointFields[1]).c_str(), nullptr, 10));
                StartCellY = int32(strtol(std::string(pointFields[2]).c_str(), nullptr, 10));
                if (StartMapID == 0)
                    LOG_ERROR("module.ROWOW", "ROWOWMod::LoadIdentityData found no converted map for the RO start point '{}', so new RO identities can't enter the RO world", value);
            }
            else if (key == "start_items_pre")
            {
                // "<item id>,<amount>,<equip position>:..."
                for (std::string_view itemText : Acore::Tokenize(value, ':', false))
                {
                    std::vector<std::string_view> itemFields = Acore::Tokenize(itemText, ',', false);
                    if (itemFields.empty() == true)
                        continue;
                    uint32 itemID = uint32(strtoul(std::string(itemFields[0]).c_str(), nullptr, 10));
                    auto itemTemplateIDIter = ItemTemplateIDsByItemID.find(itemID);
                    if (itemTemplateIDIter == ItemTemplateIDsByItemID.end())
                    {
                        LOG_ERROR("module.ROWOW", "ROWOWMod::LoadIdentityData found no item template for RO start item {}", itemID);
                        continue;
                    }
                    ROWOWStartItem startItem;
                    startItem.ItemTemplateID = itemTemplateIDIter->second;
                    if (itemFields.size() > 1)
                        startItem.Amount = std::max<uint32>(1, uint32(strtoul(std::string(itemFields[1]).c_str(), nullptr, 10)));
                    if (itemFields.size() > 2)
                        startItem.EquipPosition = uint32(strtoul(std::string(itemFields[2]).c_str(), nullptr, 10));
                    StartItems.push_back(startItem);
                }
            }
            else if (key == "start_zeny")
                StartZeny = uint32(strtoul(value.c_str(), nullptr, 10));
        }
    }
    LOG_INFO("module.ROWOW", "ROWOWMod::LoadIdentityData loaded {} jobs, {} map names, {} items and {} start items (start map {} cell {},{})", JobsByID.size(),
        MapIDsByMapName.size(), ItemTemplateIDs.size(), StartItems.size(), StartMapID, StartCellX, StartCellY);
}

uint64 ROWOWMod::GetIllusionJobKey(std::string const& jobKey, bool isMale)
{
    return (uint64(std::hash<std::string>{}(jobKey)) << 1) | (isMale == true ? 1 : 0);
}

ROWOWJob const* ROWOWMod::GetJob(uint16 jobID) const
{
    auto jobIter = JobsByID.find(jobID);
    if (jobIter == JobsByID.end())
        return nullptr;
    return &jobIter->second;
}

uint32 ROWOWMod::GetMapIDByMapName(std::string const& mapName) const
{
    auto mapIDIter = MapIDsByMapName.find(ToLower(mapName));
    if (mapIDIter == MapIDsByMapName.end())
        return 0;
    return mapIDIter->second;
}

bool ROWOWMod::GetCellPosition(uint32 mapID, int32 cellX, int32 cellY, ROWOWPosition& positionOut) const
{
    ROWOWMapGrid const* mapGrid = GetMapGrid(mapID);
    if (mapGrid == nullptr)
        return false;

    // The ground height from the map's terrain and models, as the .go commands find it (the map may not be the player's)
    Map* map = sMapMgr->CreateBaseMap(mapID);
    if (map == nullptr)
        return false;
    float x = mapGrid->GetCellCenterWorldX(cellY);
    float y = mapGrid->GetCellCenterWorldY(cellX);
    float z = std::max(map->GetHeight(x, y, MAX_HEIGHT), map->GetWaterLevel(x, y));
    if (z <= INVALID_HEIGHT)
        return false;
    positionOut.IsSet = true;
    positionOut.MapID = mapID;
    positionOut.X = x;
    positionOut.Y = y;
    positionOut.Z = z + 0.5f; // A little above, so arriving there never starts under the ground
    positionOut.O = 0;
    return true;
}

ROWOWIdentity* ROWOWMod::GetIdentity(Player* player)
{
    ROWOWIdentity* identity = player->CustomData.GetDefault<ROWOWIdentity>("ROWOWIdentity");
    if (identity->IsLoaded == true)
        return identity;
    identity->IsLoaded = true;

    uint32 guid = player->GetGUID().GetCounter();
    QueryResult result = CharacterDatabase.Query("SELECT `job`, `base_level`, `base_exp`, `job_level`, `job_exp`, `status_points`, `skill_points`, `str`, `agi`, `vit`, `int`, `dex`, `luk`, "
        "`hp`, `sp`, `zeny`, `save_map`, `save_x`, `save_y`, `save_z`, `save_o`, `last_map`, `last_x`, `last_y`, `last_z`, `last_o`, `return_map`, `return_x`, `return_y`, "
        "`return_z`, `return_o`, `hair_style`, `hair_color`, `in_form` FROM `mod_rowow_identity` WHERE `guid` = {}", guid);
    if (!result)
        return identity;
    Field* fields = result->Fetch();
    identity->Exists = true;
    identity->JobID = fields[0].Get<uint16>();
    identity->BaseLevel = fields[1].Get<uint8>();
    identity->BaseExp = fields[2].Get<uint32>();
    identity->JobLevel = fields[3].Get<uint8>();
    identity->JobExp = fields[4].Get<uint32>();
    identity->StatusPoints = fields[5].Get<uint16>();
    identity->SkillPoints = fields[6].Get<uint16>();
    for (uint8 statIndex = 0; statIndex < ROWOW_STAT_COUNT; statIndex++)
        identity->Stats[statIndex] = fields[7 + statIndex].Get<uint16>();
    identity->HP = fields[13].Get<uint32>();
    identity->SP = fields[14].Get<uint32>();
    identity->Zeny = fields[15].Get<uint32>();
    ReadPosition(fields, 16, identity->SavePoint);
    ReadPosition(fields, 21, identity->LastPosition);
    ReadPosition(fields, 26, identity->ReturnPosition);
    identity->HairStyle = fields[31].Get<uint8>();
    identity->HairColor = fields[32].Get<uint8>();
    identity->WasInFormAtLoad = fields[33].Get<uint8>() != 0;

    if (QueryResult actionResult = CharacterDatabase.Query("SELECT `button`, `action` FROM `mod_rowow_identity_action` WHERE `guid` = {}", guid))
    {
        do
        {
            Field* actionFields = actionResult->Fetch();
            identity->ActionsByButton[actionFields[0].Get<uint8>()] = actionFields[1].Get<uint32>();
        } while (actionResult->NextRow());
    }
    if (QueryResult equipmentResult = CharacterDatabase.Query("SELECT `equip_position`, `item_guid` FROM `mod_rowow_identity_equipment` WHERE `guid` = {}", guid))
    {
        do
        {
            Field* equipmentFields = equipmentResult->Fetch();
            identity->ItemGUIDsByEquipPosition[equipmentFields[0].Get<uint32>()] = equipmentFields[1].Get<uint32>();
        } while (equipmentResult->NextRow());
    }
    return identity;
}

void ROWOWMod::SaveIdentity(Player* player, CharacterDatabaseTransaction trans)
{
    ROWOWIdentity* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    if (identity == nullptr || identity->Exists == false)
        return;

    // In the form, the player's fields are the form's
    if (identity->IsInForm == true)
    {
        identity->Zeny = player->GetMoney();
        if (player->IsAlive() == true)
        {
            identity->HP = player->GetHealth();
            identity->SP = player->GetPower(POWER_MANA);
        }
    }

    uint32 guid = player->GetGUID().GetCounter();
    ROWOWPosition const& savePoint = identity->SavePoint;
    ROWOWPosition const& lastPosition = identity->LastPosition;
    ROWOWPosition const& returnPosition = identity->ReturnPosition;
    trans->Append("REPLACE INTO `mod_rowow_identity` (`guid`, `job`, `base_level`, `base_exp`, `job_level`, `job_exp`, `status_points`, `skill_points`, `str`, `agi`, `vit`, "
        "`int`, `dex`, `luk`, `hp`, `sp`, `zeny`, `save_map`, `save_x`, `save_y`, `save_z`, `save_o`, `last_map`, `last_x`, `last_y`, `last_z`, `last_o`, `return_map`, "
        "`return_x`, `return_y`, `return_z`, `return_o`, `hair_style`, `hair_color`, `in_form`, `updated`) VALUES ({}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, "
        "{}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {})",
        guid, identity->JobID, identity->BaseLevel, identity->BaseExp, identity->JobLevel, identity->JobExp, identity->StatusPoints, identity->SkillPoints,
        identity->Stats[0], identity->Stats[1], identity->Stats[2], identity->Stats[3], identity->Stats[4], identity->Stats[5], identity->HP, identity->SP, identity->Zeny,
        GetPositionMapValue(savePoint), savePoint.X, savePoint.Y, savePoint.Z, savePoint.O, GetPositionMapValue(lastPosition), lastPosition.X, lastPosition.Y,
        lastPosition.Z, lastPosition.O, GetPositionMapValue(returnPosition), returnPosition.X, returnPosition.Y, returnPosition.Z, returnPosition.O,
        identity->HairStyle, identity->HairColor, identity->IsInForm == true ? 1 : 0, uint32(GameTime::GetGameTime().count()));

    if (identity->AreActionsChanged == true)
    {
        trans->Append("DELETE FROM `mod_rowow_identity_action` WHERE `guid` = {}", guid);
        for (auto const& actionByButton : identity->ActionsByButton)
            trans->Append("INSERT INTO `mod_rowow_identity_action` (`guid`, `button`, `action`) VALUES ({}, {}, {})", guid, actionByButton.first, actionByButton.second);
        identity->AreActionsChanged = false;
    }
    if (identity->IsEquipmentChanged == true)
    {
        trans->Append("DELETE FROM `mod_rowow_identity_equipment` WHERE `guid` = {}", guid);
        for (auto const& itemGUIDByEquipPosition : identity->ItemGUIDsByEquipPosition)
            trans->Append("INSERT INTO `mod_rowow_identity_equipment` (`guid`, `equip_position`, `item_guid`) VALUES ({}, {}, {})", guid, itemGUIDByEquipPosition.first,
                itemGUIDByEquipPosition.second);
        identity->IsEquipmentChanged = false;
    }
}

uint32 ROWOWMod::GetIdentityMaxHP(ROWOWIdentity const& identity) const
{
    // rAthena's status_calc_maxhp_pc, so far without the bonuses of gear, cards and skills: the job's base HP for the level, raised 1%
    // per VIT, and by a quarter for transcendent jobs
    ROWOWJob const* job = GetJob(identity.JobID);
    if (job == nullptr || job->BaseHPByLevel.empty() == true)
        return 1;
    uint32 levelIndex = std::min<uint32>(std::max<uint32>(identity.BaseLevel, 1) - 1, uint32(job->BaseHPByLevel.size()) - 1);
    double maxHP = double(job->BaseHPByLevel[levelIndex]);
    maxHP *= 1.0 + identity.Stats[2] * 0.01;
    if (job->IsUpper == true)
        maxHP *= 1.25;
    return std::max<uint32>(1, uint32(maxHP));
}

uint32 ROWOWMod::GetIdentityMaxSP(ROWOWIdentity const& identity) const
{
    // rAthena's status_calc_maxsp_pc, likewise: the job's base SP for the level, raised 1% per INT, and by a quarter for transcendent jobs
    ROWOWJob const* job = GetJob(identity.JobID);
    if (job == nullptr || job->BaseSPByLevel.empty() == true)
        return 1;
    uint32 levelIndex = std::min<uint32>(std::max<uint32>(identity.BaseLevel, 1) - 1, uint32(job->BaseSPByLevel.size()) - 1);
    double maxSP = double(job->BaseSPByLevel[levelIndex]);
    maxSP *= 1.0 + identity.Stats[3] * 0.01;
    if (job->IsUpper == true)
        maxSP *= 1.25;
    return std::max<uint32>(1, uint32(maxSP));
}

bool ROWOWMod::IsSpellKeptInROForm(SpellInfo const* spellInfo) const
{
    // The RO spells (the world travel spell, and in time the identity's skills), auto attack, and languages (so chat still works)
    if (spellInfo == nullptr)
        return false;
    if (spellInfo->Id >= ROWOW_SPELL_ID_RO_START && spellInfo->Id <= ROWOW_SPELL_ID_RO_END)
        return true;
    if (spellInfo->Id == ROWOW_SPELL_AUTO_ATTACK)
        return true;
    return spellInfo->HasEffect(SPELL_EFFECT_LANGUAGE);
}

void ROWOWMod::EnterROForm(Player* player)
{
    if (IsEnabled == false || player == nullptr)
        return;
    ROWOWIdentity* identity = GetIdentity(player);
    if (identity->IsInForm == true)
        return;

    // The first entry makes the identity: a level 1 Novice (rAthena's new character), with the start items in the bags and worn in
    // their RO equipment positions, saving at the start point
    bool isNewIdentity = identity->Exists == false;
    if (isNewIdentity == true)
    {
        identity->Exists = true;
        identity->JobID = ROWOW_JOB_NOVICE;
        identity->BaseLevel = 1;
        identity->JobLevel = 1;
        identity->BaseExp = 0;
        identity->JobExp = 0;
        auto statusPointsIter = StatusPointsByBaseLevel.find(1);
        identity->StatusPoints = statusPointsIter != StatusPointsByBaseLevel.end() ? statusPointsIter->second : 0;
        identity->SkillPoints = 0;
        for (uint8 statIndex = 0; statIndex < ROWOW_STAT_COUNT; statIndex++)
            identity->Stats[statIndex] = 1;
        identity->Zeny = StartZeny;
        identity->HP = 0; // Full, below
        identity->SP = 0;
        identity->SavePoint = ROWOWPosition();
        GetCellPosition(StartMapID, StartCellX, StartCellY, identity->SavePoint);
        identity->ActionsByButton.clear();
        identity->ActionsByButton[0] = ROWOW_SPELL_AUTO_ATTACK;
        identity->ActionsByButton[1] = ROWOW_SPELL_WORLD_TRAVEL;
        identity->AreActionsChanged = true;
        identity->ItemGUIDsByEquipPosition.clear();
        for (ROWOWStartItem const& startItem : StartItems)
        {
            Item* item = nullptr;
            ItemPosCountVec itemDestination;
            if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, itemDestination, startItem.ItemTemplateID, startItem.Amount) == EQUIP_ERR_OK)
            {
                item = player->StoreNewItem(itemDestination, startItem.ItemTemplateID, true);
                if (item != nullptr)
                    player->SendNewItem(item, startItem.Amount, true, false);
            }
            if (item == nullptr)
            {
                // No room: it comes by mail instead (and isn't worn)
                CharacterDatabaseTransaction mailTrans = CharacterDatabase.BeginTransaction();
                MailDraft draft("Ragnarok Online", "Your bags were too full for this, so it was sent to you here.");
                if (Item* mailItem = Item::CreateItem(startItem.ItemTemplateID, startItem.Amount, player))
                {
                    mailItem->SaveToDB(mailTrans);
                    draft.AddItem(mailItem);
                }
                draft.SendMailTo(mailTrans, MailReceiver(player), MailSender(MAIL_NORMAL, 0, MAIL_STATIONERY_GM));
                CharacterDatabase.CommitTransaction(mailTrans);
                continue;
            }
            if (startItem.EquipPosition != 0)
                identity->ItemGUIDsByEquipPosition[startItem.EquipPosition] = item->GetGUID().GetCounter();
        }
        identity->IsEquipmentChanged = true;
    }

    // Leave the character's things behind: what it's casting, its pets and totems, and its buffs, mount and shapeshift
    player->InterruptNonMeleeSpells(false);
    player->UnsummonPetTemporaryIfAny();
    player->UnsummonAllTotems();
    RemovePositiveAuras(player, [](uint32 /*spellID*/) { return false; });

    // The character's own values, put back on leaving
    identity->WoWLevel = player->GetLevel();
    identity->WoWXP = player->GetUInt32Value(PLAYER_XP);
    identity->WoWMoney = player->GetMoney();
    identity->WoWHealth = player->GetHealth();
    for (uint8 powerIndex = 0; powerIndex < MAX_POWERS; powerIndex++)
        identity->WoWPowers[powerIndex] = player->GetPower(Powers(powerIndex));
    identity->WoWPowerType = uint8(player->getPowerType());
    identity->IsInForm = true;

    // The client forgets the character's spells (the server keeps them, and refuses them while in the form), keeping the form's
    identity->HiddenSpellIDs.clear();
    uint8 activeSpec = player->GetActiveSpec();
    for (auto const& spellByID : player->GetSpellMap())
    {
        if (spellByID.second->State == PLAYERSPELL_REMOVED || spellByID.second->Active == false || spellByID.second->IsInSpec(activeSpec) == false)
            continue;
        if (IsSpellKeptInROForm(sSpellMgr->GetSpellInfo(spellByID.first)) == true)
            continue;
        identity->HiddenSpellIDs.push_back(spellByID.first);
    }
    for (auto const& talentBySpellID : player->GetTalentMap())
    {
        if (talentBySpellID.second->State == PLAYERSPELL_REMOVED || (talentBySpellID.second->specMask & player->GetActiveSpecMask()) == 0 || talentBySpellID.second->inSpellBook == true)
            continue;
        identity->HiddenSpellIDs.push_back(talentBySpellID.first);
    }

    SendROFormClientSpellsAndActions(player, *identity);

    // Zeny, the identity's level, look and maximums, and its HP and SP
    player->SetMoney(identity->Zeny);
    player->setPowerType(POWER_MANA);
    RefreshROForm(player);
    if (player->IsAlive() == true)
    {
        player->SetHealth(identity->HP == 0 ? player->GetMaxHealth() : std::min(identity->HP, player->GetMaxHealth()));
        player->SetPower(POWER_MANA, (isNewIdentity == true || identity->SP > player->GetMaxPower(POWER_MANA)) ? player->GetMaxPower(POWER_MANA) : identity->SP);
    }

    // A new identity is saved right away, along with its start items
    if (isNewIdentity == true)
        player->SaveToDB(false, false);

    ROWOWJob const* job = GetJob(identity->JobID);
    ChatHandler(player->GetSession()).PSendSysMessage("You are a Ragnarok Online {}, base level {} and job level {}.", job != nullptr ? job->Name : "?", identity->BaseLevel,
        identity->JobLevel);
    SendROFormStateToClient(player);
}

void ROWOWMod::RefreshROForm(Player* player)
{
    ROWOWIdentity* identity = GetIdentity(player);
    if (identity->IsInForm == false)
        return;

    // The level, and the experience bar showing the base level's experience
    ROWOWJob const* job = GetJob(identity->JobID);
    player->SetLevel(uint8(std::min<uint32>(identity->BaseLevel, 255)), true);
    player->SetUInt32Value(PLAYER_XP, identity->BaseExp);
    uint64 nextBaseExp = 0;
    if (job != nullptr && identity->BaseLevel >= 1 && identity->BaseLevel <= job->BaseExpByLevel.size())
        nextBaseExp = job->BaseExpByLevel[identity->BaseLevel - 1];
    player->SetUInt32Value(PLAYER_NEXT_LEVEL_XP, uint32(std::min<uint64>(nextBaseExp, std::numeric_limits<uint32>::max())));

    // The job's look, in the character's gender
    uint32 wantedIllusionSpellID = 0;
    if (job != nullptr)
    {
        auto illusionSpellIDIter = IllusionSpellIDsByJobKey.find(GetIllusionJobKey(job->IllusionJobKey, player->getGender() == GENDER_MALE));
        if (illusionSpellIDIter != IllusionSpellIDsByJobKey.end())
            wantedIllusionSpellID = illusionSpellIDIter->second;
    }
    std::vector<uint32> illusionSpellIDsToRemove;
    for (auto const& illusionBySpellID : IllusionsBySpellID)
        if (illusionBySpellID.first != wantedIllusionSpellID && player->HasAura(illusionBySpellID.first) == true)
            illusionSpellIDsToRemove.push_back(illusionBySpellID.first);
    for (uint32 spellID : illusionSpellIDsToRemove)
        player->RemoveAurasDueToSpell(spellID);
    if (wantedIllusionSpellID != 0 && player->HasAura(wantedIllusionSpellID) == false)
        player->AddAura(wantedIllusionSpellID, player);
    RefreshIllusionDisplay(player);

    // HP and SP maximums (see the max health and power hooks)
    player->UpdateMaxHealth();
    player->UpdateMaxPower(POWER_MANA);
}

void ROWOWMod::RemoveROFormAuras(Player* player)
{
    std::vector<uint32> illusionSpellIDs;
    for (auto const& illusionBySpellID : IllusionsBySpellID)
        if (player->HasAura(illusionBySpellID.first) == true)
            illusionSpellIDs.push_back(illusionBySpellID.first);
    for (uint32 spellID : illusionSpellIDs)
        player->RemoveAurasDueToSpell(spellID);
    RefreshIllusionHeadgear(player, nullptr);
}

void ROWOWMod::LeaveROForm(Player* player)
{
    if (player == nullptr)
        return;
    ROWOWIdentity* identity = GetIdentity(player);
    if (identity->IsInForm == false)
        return;

    // The form's values to keep
    identity->Zeny = player->GetMoney();
    if (player->IsAlive() == true)
    {
        identity->HP = player->GetHealth();
        identity->SP = player->GetPower(POWER_MANA);
    }

    // Off with the job's look, its headgear and the form's buffs
    player->InterruptNonMeleeSpells(false);
    RemovePositiveAuras(player, [](uint32 /*spellID*/) { return false; });
    identity->IsInForm = false;
    RemoveROFormAuras(player);

    // The character's own values back
    player->SetLevel(identity->WoWLevel, true);
    player->SetUInt32Value(PLAYER_XP, identity->WoWXP);
    player->SetUInt32Value(PLAYER_NEXT_LEVEL_XP, sObjectMgr->GetXPForLevel(identity->WoWLevel));
    player->SetMoney(identity->WoWMoney);
    player->setPowerType(Powers(identity->WoWPowerType));
    player->UpdateMaxHealth();
    for (uint8 powerIndex = 0; powerIndex < MAX_POWERS; powerIndex++)
        player->UpdateMaxPower(Powers(powerIndex));
    if (player->IsAlive() == true)
    {
        player->SetHealth(std::max<uint32>(1, identity->WoWHealth));
        for (uint8 powerIndex = 0; powerIndex < MAX_POWERS; powerIndex++)
            player->SetPower(Powers(powerIndex), identity->WoWPowers[powerIndex]);
    }

    // The character's spells and action bars back
    for (uint32 spellID : identity->HiddenSpellIDs)
        SendQuietSpellChange(player, spellID, true);
    identity->HiddenSpellIDs.clear();
    WorldPacket clearActionsPacket(SMSG_ACTION_BUTTONS, 1);
    clearActionsPacket << uint8(2);
    player->SendDirectMessage(&clearActionsPacket);
    player->SendInitialActionButtons();
    player->ResummonPetTemporaryUnSummonedIfAny();
    SendROFormStateToClient(player);
}

void ROWOWMod::SendROFormStateToClient(Player* player) const
{
    std::string addonMessage = fmt::format("ROWOWFORM\t{}", IsInROForm(player) == true ? 1 : 0);
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_ADDON, nullptr, nullptr, addonMessage);
    player->SendDirectMessage(&data);
}

bool ROWOWMod::GetWorldTravelDestination(Player* player, ROWOWPosition& destinationOut)
{
    ROWOWIdentity* identity = GetIdentity(player);

    // Out of the RO world: back where the character came from (its bind point when that's unknown)
    if (IsROMap(player->GetMapId()) == true)
    {
        if (identity->ReturnPosition.IsSet == true)
        {
            destinationOut = identity->ReturnPosition;
            return true;
        }
        destinationOut.IsSet = true;
        destinationOut.MapID = player->m_homebindMapId;
        destinationOut.X = player->m_homebindX;
        destinationOut.Y = player->m_homebindY;
        destinationOut.Z = player->m_homebindZ;
        destinationOut.O = player->GetOrientation();
        return true;
    }

    // Into it: where the identity last left it, or the start point for a new one
    if (identity->Exists == true && identity->LastPosition.IsSet == true)
    {
        destinationOut = identity->LastPosition;
        return true;
    }
    return GetCellPosition(StartMapID, StartCellX, StartCellY, destinationOut);
}

void ROWOWMod::RespawnAtSavePoint(Player* player)
{
    // rAthena's pc_respawn: standing again with full HP and SP (pc_setrestartvalue), at the save point
    ROWOWIdentity* identity = GetIdentity(player);
    ROWOWPosition savePoint = identity->SavePoint;
    if (savePoint.IsSet == false && GetCellPosition(StartMapID, StartCellX, StartCellY, savePoint) == false)
    {
        savePoint.IsSet = true;
        savePoint.MapID = player->GetMapId();
        savePoint.X = player->GetPositionX();
        savePoint.Y = player->GetPositionY();
        savePoint.Z = player->GetPositionZ();
        savePoint.O = player->GetOrientation();
    }
    if (player->getDeathState() == DeathState::JustDied)
        player->KillPlayer();
    player->ResurrectPlayer(1.0f);
    player->SetHealth(player->GetMaxHealth());
    player->SetPower(POWER_MANA, player->GetMaxPower(POWER_MANA));
    player->TeleportTo(savePoint.MapID, savePoint.X, savePoint.Y, savePoint.Z, savePoint.O);
}

void ROWOWMod::SendROFormClientSpellsAndActions(Player* player, ROWOWIdentity const& identity) const
{
    // The form's action bars first, so the client has none of the hidden spells on its bars
    WorldPacket clearActionsPacket(SMSG_ACTION_BUTTONS, 1);
    clearActionsPacket << uint8(2);
    player->SendDirectMessage(&clearActionsPacket);
    SendIdentityActionButtons(player, identity);
    for (uint32 spellID : identity.HiddenSpellIDs)
        SendQuietSpellChange(player, spellID, false);
}

void ROWOWMod::SendIdentityActionButtons(Player* player, ROWOWIdentity const& identity) const
{
    WorldPacket data(SMSG_ACTION_BUTTONS, 1 + (ROWOW_IDENTITY_ACTION_BUTTON_COUNT * 4));
    data << uint8(1);
    for (uint8 button = 0; button < ROWOW_IDENTITY_ACTION_BUTTON_COUNT; ++button)
    {
        auto actionIter = identity.ActionsByButton.find(button);
        data << uint32(actionIter != identity.ActionsByButton.end() ? actionIter->second : 0);
    }
    player->SendDirectMessage(&data);
}

void ROWOWMod::SetIdentityActionButton(Player* player, uint8 button, uint32 packedAction)
{
    if (button >= ROWOW_IDENTITY_ACTION_BUTTON_COUNT)
        return;
    ROWOWIdentity* identity = GetIdentity(player);
    if (packedAction == 0)
        identity->ActionsByButton.erase(button);
    else
        identity->ActionsByButton[button] = packedAction;
    identity->AreActionsChanged = true;
}

Item* ROWOWMod::GetIdentityEquippedItem(Player* player, uint32 equipPosition) const
{
    ROWOWIdentity const* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    if (identity == nullptr)
        return nullptr;
    auto itemGUIDIter = identity->ItemGUIDsByEquipPosition.find(equipPosition);
    if (itemGUIDIter == identity->ItemGUIDsByEquipPosition.end())
        return nullptr;
    return player->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(itemGUIDIter->second));
}
