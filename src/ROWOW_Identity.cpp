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
#include "DBCStores.h"
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
        char const* ItemJobName; // The item_db Jobs name it uses items as (a transcendent job its first or second job's)
    };

    ROWOWJobName const PreRenewalJobNames[] =
    {
        { 0, "Novice", "Novice" }, { 1, "Swordman", "Swordman" }, { 2, "Mage", "Mage" }, { 3, "Archer", "Archer" },
        { 4, "Acolyte", "Acolyte" }, { 5, "Merchant", "Merchant" }, { 6, "Thief", "Thief" }, { 7, "Knight", "Knight" },
        { 8, "Priest", "Priest" }, { 9, "Wizard", "Wizard" }, { 10, "Blacksmith", "Blacksmith" }, { 11, "Hunter", "Hunter" },
        { 12, "Assassin", "Assassin" }, { 14, "Crusader", "Crusader" }, { 15, "Monk", "Monk" }, { 16, "Sage", "Sage" },
        { 17, "Rogue", "Rogue" }, { 18, "Alchemist", "Alchemist" }, { 19, "Bard", "BardDancer" }, { 20, "Dancer", "BardDancer" },
        { 23, "Super_Novice", "SuperNovice" }, { 24, "Gunslinger", "Gunslinger" }, { 25, "Ninja", "Ninja" },
        { 4001, "Novice_High", "Novice" }, { 4002, "Swordman_High", "Swordman" }, { 4003, "Mage_High", "Mage" },
        { 4004, "Archer_High", "Archer" }, { 4005, "Acolyte_High", "Acolyte" }, { 4006, "Merchant_High", "Merchant" },
        { 4007, "Thief_High", "Thief" }, { 4008, "Lord_Knight", "Knight" }, { 4009, "High_Priest", "Priest" },
        { 4010, "High_Wizard", "Wizard" }, { 4011, "Whitesmith", "Blacksmith" }, { 4012, "Sniper", "Hunter" },
        { 4013, "Assassin_Cross", "Assassin" }, { 4015, "Paladin", "Crusader" }, { 4016, "Champion", "Monk" },
        { 4017, "Professor", "Sage" }, { 4018, "Stalker", "Rogue" }, { 4019, "Creator", "Alchemist" }, { 4020, "Clown", "BardDancer" },
        { 4021, "Gypsy", "BardDancer" }, { 4046, "Taekwon", "Taekwon" }, { 4047, "Star_Gladiator", "StarGladiator" },
        { 4049, "Soul_Linker", "SoulLinker" }, { 4023, "Baby", "Novice" }, { 4024, "Baby_Swordman", "Swordman" }, { 4025, "Baby_Mage", "Mage" },
        { 4026, "Baby_Archer", "Archer" }, { 4027, "Baby_Acolyte", "Acolyte" }, { 4028, "Baby_Merchant", "Merchant" }, { 4029, "Baby_Thief", "Thief" },
        { 4030, "Baby_Knight", "Knight" }, { 4031, "Baby_Priest", "Priest" }, { 4032, "Baby_Wizard", "Wizard" }, { 4033, "Baby_Blacksmith", "Blacksmith" },
        { 4034, "Baby_Hunter", "Hunter" }, { 4035, "Baby_Assassin", "Assassin" }, { 4037, "Baby_Crusader", "Crusader" }, { 4038, "Baby_Monk", "Monk" },
        { 4039, "Baby_Sage", "Sage" }, { 4040, "Baby_Rogue", "Rogue" }, { 4041, "Baby_Alchemist", "Alchemist" }, { 4042, "Baby_Bard", "BardDancer" },
        { 4043, "Baby_Dancer", "BardDancer" }, { 4045, "Super_Baby", "SuperNovice" },
    };

    // Each Baby job's normal job (pc_mapid2jobid of its class_ less JOBL_BABY).  A Baby has its job's look, items, skills and data, but is
    // small, has a lower stat cap (max_baby_parameter) and makes potions and weapons less often
    std::pair<uint16, uint16> const BabyJobNormalJobIDs[] =
    {
        { 4023, 0 }, { 4024, 1 }, { 4025, 2 }, { 4026, 3 }, { 4027, 4 }, { 4028, 5 }, { 4029, 6 }, { 4030, 7 }, { 4031, 8 }, { 4032, 9 }, { 4033, 10 },
        { 4034, 11 }, { 4035, 12 }, { 4037, 14 }, { 4038, 15 }, { 4039, 16 }, { 4040, 17 }, { 4041, 18 }, { 4042, 19 }, { 4043, 20 }, { 4045, 23 },
    };

    // Each second job's first job, as a normal job (rAthena's MAPID_FIRSTMASK of a JOBL_2 job; the Super Novice is a Novice of its own)
    std::pair<uint16, uint16> const SecondJobFirstJobIDs[] =
    {
        { 7, 1 }, { 14, 1 }, { 8, 4 }, { 15, 4 }, { 9, 2 }, { 16, 2 }, { 10, 5 }, { 18, 5 }, { 11, 3 }, { 19, 3 }, { 20, 3 }, { 12, 6 }, { 17, 6 },
        { 4008, 1 }, { 4015, 1 }, { 4009, 4 }, { 4016, 4 }, { 4010, 2 }, { 4017, 2 }, { 4011, 5 }, { 4019, 5 }, { 4012, 3 }, { 4020, 3 }, { 4021, 3 },
        { 4013, 6 }, { 4018, 6 }, { 4047, 4046 }, { 4049, 4046 },
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
    MapNamesByMapID.clear();
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
        for (int32 itemJobIndex = 0; itemJobIndex < ROWOW_ITEM_JOB_COUNT; itemJobIndex++)
            if (strcmp(ROWOWItemJobNames[itemJobIndex], jobName.ItemJobName) == 0)
                job.ItemJobIndex = itemJobIndex;
        job.NormalJobID = job.JobID;
        job.IllusionJobKey = ToLower(job.Name);
        if (job.IllusionJobKey.size() > 5 && job.IllusionJobKey.compare(job.IllusionJobKey.size() - 5, 5, "_high") == 0)
            job.IllusionJobKey.resize(job.IllusionJobKey.size() - 5); // Transcendent first jobs look like the first job
        jobIDsByName[GetJobNameKey(job.Name)] = job.JobID;
    }
    for (auto const& secondJobFirstJobID : SecondJobFirstJobIDs)
        JobsByID[secondJobFirstJobID.first].FirstJobID = secondJobFirstJobID.second;
    for (auto const& babyJobNormalJobID : BabyJobNormalJobIDs)
    {
        // A Baby job looks like its job and is in the same line (a Baby_Knight's first job is the Swordman, as a normal job)
        ROWOWJob& job = JobsByID[babyJobNormalJobID.first];
        ROWOWJob const& normalJob = JobsByID[babyJobNormalJobID.second];
        job.IsBaby = true;
        job.NormalJobID = normalJob.JobID;
        job.IllusionJobKey = normalJob.IllusionJobKey;
        job.FirstJobID = normalJob.FirstJobID;
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
                auto jobIDIter = jobIDsByName.find(GetJobNameKey(jobField.first));
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
        // A map's own name (not one of rAthena's aliases for it) is the one the warp lists show
        bool hasAliasColumn = WorldDatabase.Query("SELECT 1 FROM information_schema.columns WHERE table_schema = DATABASE() AND table_name = 'mod_rowow_map_name' AND column_name = 'IsAlias'") != nullptr;
        if (QueryResult result = WorldDatabase.Query(hasAliasColumn == true ? "SELECT `MapName`, `MapID`, `IsAlias` FROM `mod_rowow_map_name`" : "SELECT `MapName`, `MapID`, 0 FROM `mod_rowow_map_name`"))
        {
            do
            {
                Field* fields = result->Fetch();
                std::string mapName = ToLower(fields[0].Get<std::string>());
                uint32 mapID = fields[1].Get<uint32>();
                MapIDsByMapName[mapName] = mapID;
                if (fields[2].Get<uint32>() == 0)
                    MapNamesByMapID[mapID] = mapName;
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

std::string ROWOWMod::GetJobNameKey(std::string const& jobName)
{
    // The job database names its jobs by rAthena's JOB_ constants, some of which have aliases differing only in underscores
    std::string jobNameKey;
    for (char curChar : jobName)
        if (curChar != '_')
            jobNameKey.push_back(char(tolower((unsigned char)curChar)));
    return jobNameKey;
}

ROWOWJob const* ROWOWMod::GetJob(uint16 jobID) const
{
    auto jobIter = JobsByID.find(jobID);
    if (jobIter == JobsByID.end())
        return nullptr;
    return &jobIter->second;
}

uint16 ROWOWMod::GetNonBabyJobID(uint16 jobID) const
{
    ROWOWJob const* job = GetJob(jobID);
    return job != nullptr ? job->NormalJobID : jobID;
}

uint8 ROWOWMod::GetItemClassMask(ROWOWJob const& job)
{
    if (job.IsUpper == true)
        return ROWOW_ITEM_CLASS_UPPER;
    return job.IsBaby == true ? ROWOW_ITEM_CLASS_BABY : ROWOW_ITEM_CLASS_NORMAL;
}

uint16 ROWOWMod::GetBabyJobID(uint16 normalJobID) const
{
    for (auto const& jobByID : JobsByID)
        if (jobByID.second.IsBaby == true && jobByID.second.NormalJobID == normalJobID)
            return jobByID.first;
    return 0;
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

    // The ground height (the map may not be the player's).  An RO map's ground is its models: their highest floor, which is where the client
    // lands on the cell from above.  The terrain under them isn't the ground (the converted terrain is up to 70 yards above an indoor map's
    // floor, and falling from it kills), so it's only the answer when the models' collision isn't loaded (a map no one is on): then
    // the arrival puts the player down on the floor (CheckArrivalFloor)
    Map* map = sMapMgr->CreateBaseMap(mapID);
    if (map == nullptr)
        return false;
    float x = mapGrid->GetCellCenterWorldX(cellY);
    float y = mapGrid->GetCellCenterWorldY(cellX);
    float z = map->GetMapCollisionData().GetStaticTree().getHeight(x, y, MAX_HEIGHT, MAX_FALL_DISTANCE);
    if (z <= INVALID_HEIGHT)
        z = map->GetHeight(x, y, MAX_HEIGHT);
    z = std::max(z, map->GetWaterLevel(x, y));
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

void ROWOWMod::CheckArrivalFloor(Player* player)
{
    // Once after arriving on an RO map in the form (its map's thread, the grid around it loaded): a player left in the air above the floor
    // (an arrival worked out before the map's models were loaded, see GetCellPosition) is put down on it, as RO has no falling
    ROWOWIdentity* identity = GetIdentity(player);
    if (identity->IsArrivalFloorCheckPending == false || player->IsBeingTeleported() == true)
        return;
    identity->IsArrivalFloorCheckPending = false;
    if (player->GetTransport() != nullptr || player->IsFlying() == true)
        return;
    Map* map = player->GetMap();
    float floorZ = map->GetMapCollisionData().GetStaticTree().getHeight(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ() + 1.0f, MAX_FALL_DISTANCE);
    if (floorZ <= INVALID_HEIGHT)
        return;
    floorZ = std::max(floorZ, map->GetWaterLevel(player->GetPositionX(), player->GetPositionY()));
    if (player->GetPositionZ() - floorZ > ROWOW_ARRIVAL_FLOOR_TOLERANCE)
        player->NearTeleportTo(player->GetPositionX(), player->GetPositionY(), floorZ + 0.5f, player->GetOrientation());
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
        "`return_z`, `return_o`, `hair_style`, `hair_color`, `in_form`, `option` FROM `mod_rowow_identity` WHERE `guid` = {}", guid);
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
    identity->Option = fields[34].Get<uint32>();

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
    if (QueryResult skillResult = CharacterDatabase.Query("SELECT `skill_id`, `level` FROM `mod_rowow_identity_skill` WHERE `guid` = {}", guid))
    {
        do
        {
            Field* skillFields = skillResult->Fetch();
            if (skillFields[1].Get<uint16>() > 0)
                identity->SkillLevelsByID[skillFields[0].Get<uint16>()] = skillFields[1].Get<uint16>();
        } while (skillResult->NextRow());
    }
    LoadIdentityScriptVariables(player, *identity);
    if (QueryResult statusResult = CharacterDatabase.Query("SELECT `type`, `tick`, `val1`, `val2`, `val3`, `val4` FROM `mod_rowow_identity_status` WHERE `guid` = {}", guid))
    {
        // chrif_load_scdata: the statuses kept, started again when the identity is next in the RO world
        do
        {
            Field* statusFields = statusResult->Fetch();
            ROWOWSavedStatusChange saved;
            saved.Type = statusFields[0].Get<int32>();
            saved.Tick = statusFields[1].Get<int64>();
            saved.Val1 = statusFields[2].Get<int32>();
            saved.Val2 = statusFields[3].Get<int32>();
            saved.Val3 = statusFields[4].Get<int32>();
            saved.Val4 = statusFields[5].Get<int32>();
            identity->SavedStatusChanges.push_back(saved);
        } while (statusResult->NextRow());
    }
    LoadIdentityCart(player, *identity);
    LoadIdentityMemo(player, *identity);
    LoadIdentityQuests(player, *identity);
    PCCalcSkillTree(*identity);
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
        "`return_x`, `return_y`, `return_z`, `return_o`, `hair_style`, `hair_color`, `in_form`, `option`, `updated`) VALUES ({}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, "
        "{}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {})",
        guid, identity->JobID, identity->BaseLevel, identity->BaseExp, identity->JobLevel, identity->JobExp, identity->StatusPoints, identity->SkillPoints,
        identity->Stats[0], identity->Stats[1], identity->Stats[2], identity->Stats[3], identity->Stats[4], identity->Stats[5], identity->HP, identity->SP, identity->Zeny,
        GetPositionMapValue(savePoint), savePoint.X, savePoint.Y, savePoint.Z, savePoint.O, GetPositionMapValue(lastPosition), lastPosition.X, lastPosition.Y,
        lastPosition.Z, lastPosition.O, GetPositionMapValue(returnPosition), returnPosition.X, returnPosition.Y, returnPosition.Z, returnPosition.O,
        identity->HairStyle, identity->HairColor, identity->IsInForm == true ? 1 : 0, identity->Option, uint32(GameTime::GetGameTime().count()));

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
    if (identity->AreSkillsChanged == true)
    {
        trans->Append("DELETE FROM `mod_rowow_identity_skill` WHERE `guid` = {}", guid);
        for (auto const& levelBySkillID : identity->SkillLevelsByID)
            trans->Append("INSERT INTO `mod_rowow_identity_skill` (`guid`, `skill_id`, `level`) VALUES ({}, {}, {})", guid, levelBySkillID.first, levelBySkillID.second);
        identity->AreSkillsChanged = false;
    }
    SaveIdentityScriptVariables(player, *identity, trans);
    SaveIdentityStatusChanges(player, *identity, trans);
    SaveIdentityCart(player, *identity, trans);
    SaveIdentityMemo(player, *identity, trans);
    SaveIdentityQuests(player, *identity, trans);
}

bool ROWOWMod::IsSpellKeptInROForm(SpellInfo const* spellInfo) const
{
    // The fixed RO spells (the world travel spell), auto attack, languages (so chat still works) and the RO companion pets.  The RO skill
    // spells a character learned as itself (from before the identity) are the character's, not the form's: the form knows the spells of
    // the skills the identity learned (IsIdentitySkillSpell)
    if (spellInfo == nullptr)
        return false;
    if (spellInfo->Id >= ROWOW_SPELL_ID_RO_START && spellInfo->Id <= ROWOW_SPELL_ID_FIXED_END)
        return true;
    if (spellInfo->Id == ROWOW_SPELL_AUTO_ATTACK)
        return true;
    if (IsROCompanionPetSpell(spellInfo) == true)
        return true;
    return spellInfo->HasEffect(SPELL_EFFECT_LANGUAGE);
}

bool ROWOWMod::IsROCompanionPetSpell(SpellInfo const* spellInfo) const
{
    // An RO companion pet's summon (the converter's RoCompanionPetsMap.csv): an RO spell summoning a minipet
    if (spellInfo == nullptr || spellInfo->Id < ROWOW_SPELL_ID_RO_START || spellInfo->Id > ROWOW_SPELL_ID_RO_END)
        return false;
    for (uint8 effectIndex = 0; effectIndex < MAX_SPELL_EFFECTS; effectIndex++)
    {
        if (spellInfo->Effects[effectIndex].Effect != SPELL_EFFECT_SUMMON)
            continue;
        SummonPropertiesEntry const* properties = sSummonPropertiesStore.LookupEntry(spellInfo->Effects[effectIndex].MiscValueB);
        if (properties != nullptr && properties->Type == SUMMON_TYPE_MINIPET)
            return true;
    }
    return false;
}

bool ROWOWMod::IsROCompanionPetCarrier(ItemTemplate const* itemTemplate) const
{
    // A carrier casts the learning spell with the spell it teaches as its second (Player::CastItemUseSpell's learning case)
    if (itemTemplate == nullptr || (itemTemplate->Spells[0].SpellId != 483 && itemTemplate->Spells[0].SpellId != 55884))
        return false;
    return IsROCompanionPetSpell(sSpellMgr->GetSpellInfo(itemTemplate->Spells[1].SpellId));
}

void ROWOWMod::EnterROForm(Player* player)
{
    if (IsEnabled == false || player == nullptr)
        return;
    ROWOWIdentity* identity = GetIdentity(player);
    if (identity->IsInForm == true)
        return;

    // The first entry makes the identity: a level 1 Novice (rAthena's new character), saving at the start point.  The start items are
    // given once the character is in the world
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
        identity->IsEquipmentChanged = true;
        identity->SkillLevelsByID.clear();
        identity->AreSkillsChanged = true;
        identity->Variables.Clear();
        identity->SavedStatusChanges.clear();
        identity->AreStatusChangesChanged = true;
        identity->CartItems.fill(ROWOWCartItem());
        identity->CartNum = 0;
        identity->CartWeight = 0;
        identity->AreCartItemsChanged = true;
        identity->MemoPoints.fill(ROWOWMemoPoint());
        identity->AreMemoPointsChanged = true;
        identity->Quests.clear();
        identity->AreQuestsChanged = true;
        identity->Option = 0;
        identity->IsNewIdentityPending = true;
    }

    // The character's own values, put back on leaving
    player->InterruptNonMeleeSpells(false);
    identity->WoWLevel = player->GetLevel();
    identity->WoWXP = player->GetUInt32Value(PLAYER_XP);
    identity->WoWMoney = player->GetMoney();
    identity->WoWHealth = player->GetHealth();
    for (uint8 powerIndex = 0; powerIndex < MAX_POWERS; powerIndex++)
        identity->WoWPowers[powerIndex] = player->GetPower(Powers(powerIndex));
    identity->WoWPowerType = uint8(player->getPowerType());
    for (uint8 attackType = 0; attackType < MAX_ATTACK; attackType++)
        identity->WoWAttackTimes[attackType] = player->GetAttackTime(WeaponAttackType(attackType));
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

    // The learned skills' spells, before the action bars that hold them
    RefreshIdentitySkills(player, *identity);
    SendROFormClientSpellsAndActions(player, *identity);

    // Zeny, the identity's level and maximums, and its HP and SP
    player->SetMoney(identity->Zeny);
    player->setPowerType(POWER_MANA);
    RefreshROFormValues(player, *identity);
    if (player->IsAlive() == true)
    {
        player->SetHealth(identity->HP == 0 ? player->GetMaxHealth() : std::min(identity->HP, player->GetMaxHealth()));
        player->SetPower(POWER_MANA, (isNewIdentity == true || identity->SP > player->GetMaxPower(POWER_MANA)) ? player->GetMaxPower(POWER_MANA) : identity->SP);
    }
    identity->RegenHPTimer = 0;
    identity->RegenSPTimer = 0;

    identity->IsFormArrivalPending = true;
    identity->IsFormDeparturePending = false;
    if (player->IsInWorld() == true)
        CompleteROFormChange(player);
}

void ROWOWMod::CompleteROFormChange(Player* player)
{
    // A safety net for an arrival that didn't come through OnPlayerSendInitialPacketsBeforeAddToMap
    ROWOWIdentity* identity = GetIdentity(player);
    bool isROMap = IsROMap(player->GetMapId());
    if (isROMap == true && identity->IsInForm == false)
    {
        EnterROForm(player);
        return;
    }
    if (isROMap == false && identity->IsInForm == true)
    {
        LeaveROForm(player);
        return;
    }

    if (identity->IsInForm == true)
    {
        if (identity->IsFormArrivalPending == true)
        {
            // Leave the character's things behind: its pets and totems, its buffs, mount and shapeshift, and the passive auras of its
            // hidden spells and talents (no stat, regen or proc of the character's acts on the form).  Taking off an aura that raised
            // max health also takes that much health, so the form's HP and SP are put back after
            uint32 formHealth = player->GetHealth();
            uint32 formSP = player->GetPower(POWER_MANA);
            player->UnsummonPetTemporaryIfAny();
            player->UnsummonAllTotems();
            RemovePositiveAuras(player, [](uint32 /*spellID*/) { return false; });
            identity->SuspendedPassiveSpellIDs.clear();
            for (uint32 spellID : identity->HiddenSpellIDs)
            {
                SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellID);
                if (spellInfo == nullptr || spellInfo->IsPassive() == false || player->HasAura(spellID) == false)
                    continue;
                player->RemoveAurasDueToSpell(spellID);
                identity->SuspendedPassiveSpellIDs.push_back(spellID);
            }
            if (player->IsAlive() == true)
            {
                player->SetHealth(std::min(formHealth, player->GetMaxHealth()));
                player->SetPower(POWER_MANA, std::min(formSP, player->GetMaxPower(POWER_MANA)));
            }

            // A new identity gets the start items in the bags and worn in their RO equipment positions, and is saved with them
            if (identity->IsNewIdentityPending == true)
            {
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
                identity->IsNewIdentityPending = false;
                RefreshROFormValues(player, *identity); // The start weapon and armor count from now
                player->SaveToDB(false, false);
            }

            ROWOWJob const* job = GetJob(identity->JobID);
            ChatHandler(player->GetSession()).PSendSysMessage("You are a Ragnarok Online {}, base level {} and job level {}.", job != nullptr ? job->Name : "?",
                identity->BaseLevel, identity->JobLevel);

            // The statuses kept from the last time in the RO world (RO's login), once the character's own buffs are gone
            RestoreStatusChanges(player, *identity);
        }
        else
            StatusChangeClearOnChangeMap(player); // From one RO map to another
        RefreshROFormLook(player);
    }
    else if (identity->IsFormDeparturePending == true || identity->WasInFormAtLoad == true)
    {
        // Off with the form's buffs and the job's look.  Saved in the form but not in the RO world (the server stopped before the
        // character left it), only the look is left over, since the character row never stops having the character's own values
        // The character's own health and powers are put back after, as the auras coming and going change them (leaving the form,
        // the values it entered with: setting them before its passive auras were back held them under the lower maximums)
        uint32 health = identity->IsFormDeparturePending == true ? std::max<uint32>(1, identity->WoWHealth) : player->GetHealth();
        uint32 powers[MAX_POWERS];
        for (uint8 powerIndex = 0; powerIndex < MAX_POWERS; powerIndex++)
            powers[powerIndex] = identity->IsFormDeparturePending == true ? identity->WoWPowers[powerIndex] : player->GetPower(Powers(powerIndex));
        if (identity->IsFormDeparturePending == true)
            RemovePositiveAuras(player, [](uint32 /*spellID*/) { return false; });
        RemoveROFormAuras(player);
        if (identity->IsFormDeparturePending == true)
        {
            // The character's passive auras back, as learning the spells casts them
            for (uint32 spellID : identity->SuspendedPassiveSpellIDs)
            {
                SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellID);
                if (spellInfo != nullptr && (player->HasSpell(spellID) == true || player->HasTalent(spellID, player->GetActiveSpec()) == true) &&
                    player->IsNeedCastPassiveSpellAtLearn(spellInfo) == true && player->HasAura(spellID) == false)
                    player->CastSpell(player, spellID, true);
            }
            player->ResummonPetTemporaryUnSummonedIfAny();
        }
        identity->SuspendedPassiveSpellIDs.clear();
        if (player->IsAlive() == true)
        {
            player->SetHealth(std::min(health, player->GetMaxHealth()));
            for (uint8 powerIndex = 0; powerIndex < MAX_POWERS; powerIndex++)
                player->SetPower(Powers(powerIndex), std::min(powers[powerIndex], player->GetMaxPower(Powers(powerIndex))));
        }
    }
    identity->IsFormArrivalPending = false;
    identity->IsFormDeparturePending = false;
    identity->WasInFormAtLoad = false;
    SendROFormStateToClient(player);
    SendROStatusToClient(player);
    if (identity->IsInForm == true)
    {
        SendSkillsToClient(player, *identity, true);
        SendCartToClient(player, *identity, true);
        // clif_parse_LoadEndAck: arriving on a map makes the player invincible for a moment (pc_setinvincibletimer)
        PCSetInvincibleTimer(player);
    }
}

void ROWOWMod::RefreshROFormValues(Player* player, ROWOWIdentity& identity)
{
    // The level, the experience bar showing the base level's experience, the status, the HP and SP maximums from it (see the max
    // health and power hooks), and the time between attacks (adelay, from the ASPD)
    player->SetLevel(uint8(std::min<uint32>(identity.BaseLevel, 255)), true);
    player->SetUInt32Value(PLAYER_XP, identity.BaseExp);
    player->SetUInt32Value(PLAYER_NEXT_LEVEL_XP, uint32(std::min<uint64>(PCNextBaseExp(identity), std::numeric_limits<uint32>::max())));
    CalculateIdentityStatus(player, identity);
    player->UpdateMaxHealth();
    player->UpdateMaxPower(POWER_MANA);
    player->UpdateManaRegen(); // None of Warcraft's (the client's own guess of it included)
    player->SetAttackTime(BASE_ATTACK, uint32(identity.Status.AttackDelay));
}

void ROWOWMod::RefreshROForm(Player* player)
{
    ROWOWIdentity* identity = GetIdentity(player);
    if (identity->IsInForm == false)
        return;
    PCCheckItem(player, *identity);
    RefreshIdentitySkills(player, *identity);
    RefreshROFormValues(player, *identity);
    if (player->IsInWorld() == true)
        RefreshROFormLook(player);
    SendROStatusToClient(player);
    SendEquipmentToClient(player, *identity, false);
    SendSkillsToClient(player, *identity, false);
}

void ROWOWMod::RefreshROFormLook(Player* player)
{
    // The job's look, in the character's gender: riding its Peco, the job's mounted body (R9.5; the converter's job key with "_riding")
    ROWOWIdentity* identity = GetIdentity(player);
    ROWOWJob const* job = GetJob(identity->JobID);
    uint32 wantedIllusionSpellID = 0;
    if (job != nullptr)
    {
        bool isMale = player->getGender() == GENDER_MALE;
        auto illusionSpellIDIter = IllusionSpellIDsByJobKey.end();
        if (PCIsRiding(player) == true)
            illusionSpellIDIter = IllusionSpellIDsByJobKey.find(GetIllusionJobKey(job->IllusionJobKey + "_riding", isMale));
        if (illusionSpellIDIter == IllusionSpellIDsByJobKey.end())
            illusionSpellIDIter = IllusionSpellIDsByJobKey.find(GetIllusionJobKey(job->IllusionJobKey, isMale));
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
}

void ROWOWMod::RemoveROFormAuras(Player* player)
{
    std::vector<uint32> illusionSpellIDs;
    for (auto const& illusionBySpellID : IllusionsBySpellID)
        if (player->HasAura(illusionBySpellID.first) == true)
            illusionSpellIDs.push_back(illusionBySpellID.first);
    for (uint32 spellID : illusionSpellIDs)
        player->RemoveAurasDueToSpell(spellID);
    RefreshIllusionHead(player, nullptr);
    RefreshIllusionHeadgear(player, nullptr);
    RefreshIllusionOptionLooks(player, nullptr);
    RefreshIllusionGarment(player, nullptr);
    RefreshIllusionHatEffects(player, nullptr);
}

void ROWOWMod::LeaveROForm(Player* player)
{
    if (player == nullptr)
        return;
    ROWOWIdentity* identity = GetIdentity(player);
    if (identity->IsInForm == false)
        return;

    // The form's values to keep, and its statuses (leaving the RO world is RO's logout).  The arrival invincibility is the form's: WoW's
    // immunity to NPCs goes with it (a reset identity would no longer know of it)
    LeaveStatusChanges(player, *identity);
    if (identity->InvincibleEndTime != 0)
    {
        identity->InvincibleEndTime = 0;
        player->SetImmuneToNPC(false);
    }
    identity->Zeny = player->GetMoney();
    if (player->IsAlive() == true)
    {
        identity->HP = player->GetHealth();
        identity->SP = player->GetPower(POWER_MANA);
    }
    player->InterruptNonMeleeSpells(false);
    identity->IsInForm = false;
    GrantIdentitySkillSpells(player, *identity); // Out of the form, it knows none of them

    // The character's own values back
    player->SetLevel(identity->WoWLevel, true);
    player->SetUInt32Value(PLAYER_XP, identity->WoWXP);
    player->SetUInt32Value(PLAYER_NEXT_LEVEL_XP, sObjectMgr->GetXPForLevel(identity->WoWLevel));
    player->SetMoney(identity->WoWMoney);
    player->setPowerType(Powers(identity->WoWPowerType));
    for (uint8 attackType = 0; attackType < MAX_ATTACK; attackType++)
        if (identity->WoWAttackTimes[attackType] != 0)
            player->SetAttackTime(WeaponAttackType(attackType), identity->WoWAttackTimes[attackType]);
    player->UpdateMaxHealth();
    for (uint8 powerIndex = 0; powerIndex < MAX_POWERS; powerIndex++)
        player->UpdateMaxPower(Powers(powerIndex));
    player->UpdateManaRegen();
    if (player->IsAlive() == true)
    {
        player->SetHealth(std::max<uint32>(1, identity->WoWHealth));
        for (uint8 powerIndex = 0; powerIndex < MAX_POWERS; powerIndex++)
            player->SetPower(Powers(powerIndex), identity->WoWPowers[powerIndex]);
    }

    // The character's spells and action bars back.  Arriving on a map, the client was just sent them all
    // (Player::SendInitialPacketsBeforeAddToMap)
    if (player->IsInWorld() == true)
    {
        for (uint32 spellID : identity->HiddenSpellIDs)
            SendQuietSpellChange(player, spellID, true);
        WorldPacket clearActionsPacket(SMSG_ACTION_BUTTONS, 1);
        clearActionsPacket << uint8(2);
        player->SendDirectMessage(&clearActionsPacket);
        player->SendInitialActionButtons();
    }
    identity->HiddenSpellIDs.clear();

    identity->IsFormDeparturePending = true;
    identity->IsFormArrivalPending = false;
    if (player->IsInWorld() == true)
        CompleteROFormChange(player);
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
    // rAthena's pc_respawn: standing again at the save point with the restart HP and SP (pc_setrestartvalue, from the respawn values
    // status_calc_pc_sub works out).  With restart_hp_rate below 50 a Novice comes back with half its HP and everyone else with 1, healed
    // on top of the 1 HP a resurrection starts from, and SP stays as it was unless that's below the restart SP
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
    uint32 maxHP = identity->Status.MaxHP;
    uint32 maxSP = identity->Status.MaxSP;
    uint32 restartHP = 0;
    if ((GetNonBabyJobID(identity->JobID) == ROWOW_JOB_NOVICE || identity->JobID == ROWOW_JOB_NOVICE_HIGH) && BattleConfig.RestartHPRate < 50)
        restartHP = maxHP / 2;
    else
        restartHP = uint32(uint64(maxHP) * BattleConfig.RestartHPRate / 100);
    restartHP = std::max<uint32>(1, restartHP);
    uint32 restartSP = std::max<uint32>(1, uint32(uint64(maxSP) * BattleConfig.RestartSPRate / 100));
    uint32 sp = std::max(identity->SP, restartSP);

    if (player->getDeathState() == DeathState::JustDied)
        player->KillPlayer();
    player->ResurrectPlayer(1.0f);
    player->SetHealth(std::min(1 + restartHP, player->GetMaxHealth()));
    player->SetPower(POWER_MANA, std::min(sp, player->GetMaxPower(POWER_MANA)));
    SendStatusChangesToClient(player); // The statuses that stay through death show again
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
        if (std::find(identity.SkillSpellIDs.begin(), identity.SkillSpellIDs.end(), spellID) == identity.SkillSpellIDs.end())
            SendQuietSpellChange(player, spellID, false);
}

void ROWOWMod::GrantIdentitySkillSpells(Player* player, ROWOWIdentity& identity)
{
    // In the form, the spells of every learned level of each skill on the tree it may use (a lower level can be picked, as in RO), learned
    // as temporary spells so they're never saved with the character's own.  Each is only learned and forgotten as that changes: forgetting
    // a spell takes off its auras.  The client is told quietly
    std::vector<uint32> spellIDs;
    if (identity.IsInForm == true)
    {
        for (auto const& levelBySkillID : identity.SkillLevelsByID)
        {
            ROWOWSkill const* skill = GetSkill(levelBySkillID.first);
            if (skill == nullptr || identity.TreeSkillIDs.find(levelBySkillID.first) == identity.TreeSkillIDs.end())
                continue;
            for (uint16 level = 1; level <= levelBySkillID.second && level <= skill->SpellIDsByLevel.size(); level++)
                if (skill->SpellIDsByLevel[level - 1] != 0 && sSpellMgr->GetSpellInfo(skill->SpellIDsByLevel[level - 1]) != nullptr)
                    spellIDs.push_back(skill->SpellIDsByLevel[level - 1]);
        }
        // The skills it has for now (a spirit link's), each level up to that one
        for (auto const& levelBySkillID : identity.TempSkillLevelsByID)
            if (ROWOWSkill const* tempSkill = GetSkill(levelBySkillID.first))
                for (uint16 level = 1; level <= levelBySkillID.second && level <= tempSkill->SpellIDsByLevel.size(); level++)
                    if (tempSkill->SpellIDsByLevel[level - 1] != 0 && sSpellMgr->GetSpellInfo(tempSkill->SpellIDsByLevel[level - 1]) != nullptr)
                        spellIDs.push_back(tempSkill->SpellIDsByLevel[level - 1]);
        // The skill Plagiarism copied, each level up to the copied one
        if (ROWOWSkill const* cloneSkill = GetSkill(identity.CloneSkillID))
            for (uint16 level = 1; level <= identity.CloneSkillLevel && level <= cloneSkill->SpellIDsByLevel.size(); level++)
                if (cloneSkill->SpellIDsByLevel[level - 1] != 0 && sSpellMgr->GetSpellInfo(cloneSkill->SpellIDsByLevel[level - 1]) != nullptr)
                    spellIDs.push_back(cloneSkill->SpellIDsByLevel[level - 1]);
        std::sort(spellIDs.begin(), spellIDs.end());
        spellIDs.erase(std::unique(spellIDs.begin(), spellIDs.end()), spellIDs.end());
    }
    for (uint32 spellID : identity.SkillSpellIDs)
    {
        if (std::binary_search(spellIDs.begin(), spellIDs.end(), spellID) == true)
            continue;
        player->removeSpell(spellID, SPEC_MASK_ALL, true, false);
        SendQuietSpellChange(player, spellID, false);
    }
    for (uint32 spellID : spellIDs)
    {
        if (std::binary_search(identity.SkillSpellIDs.begin(), identity.SkillSpellIDs.end(), spellID) == true)
            continue;
        player->_addSpell(spellID, SPEC_MASK_ALL, true, false, false);
        SendQuietSpellChange(player, spellID, true);
    }
    identity.SkillSpellIDs = spellIDs;
}

bool ROWOWMod::IsIdentitySkillSpell(Player* player, uint32 spellID) const
{
    ROWOWIdentity const* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    return identity != nullptr && identity->IsInForm == true && std::binary_search(identity->SkillSpellIDs.begin(), identity->SkillSpellIDs.end(), spellID) == true;
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
    // An item worn in several positions (a two-handed weapon, a headgear covering two) is kept under all of them together
    for (auto const& itemGUIDByEquipPosition : identity->ItemGUIDsByEquipPosition)
        if ((itemGUIDByEquipPosition.first & equipPosition) == equipPosition)
            return player->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(itemGUIDByEquipPosition.second));
    return nullptr;
}
