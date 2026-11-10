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

#include "Bag.h"
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
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Tokenize.h"
#include "World.h"
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
            if (aura->IsClientHidden() == true) // An identity's look (RefreshWorldIdentityLook keeps it right), not a buff
                continue;
            spellIDsToRemove.push_back(aura->GetId());
        }
        for (uint32 spellID : spellIDsToRemove)
            player->RemoveAurasDueToSpell(spellID);
    }

    bool IsCarriedBagSlot(uint8 bag, uint8 slot)
    {
        return bag == INVENTORY_SLOT_BAG_0 && slot >= INVENTORY_SLOT_BAG_START && slot < INVENTORY_SLOT_BAG_END;
    }

    // Takes the carried set off the character: the bags' items first (so each bag comes off empty), then the bags, then the backpack, each with
    // the place it had.  The items stay as they are, their rows and GUIDs too (the RO equipment is kept by GUID); only their places change, which the
    // next save with the inventory writes (SaveIdentityStowedItems)
    std::vector<ROWOWStowedItem> TakeCarriedSet(Player* player)
    {
        std::vector<ROWOWStowedItem> takenItems;
        auto takeItem = [player, &takenItems](uint8 bag, uint8 slot)
        {
            Item* item = player->GetItemByPos(bag, slot);
            if (item == nullptr)
                return;
            player->MoveItemFromInventory(bag, slot, true);
            if (item->GetState() == ITEM_UNCHANGED)
                item->FSetState(ITEM_CHANGED); // Its item_instance row is written with the set, as it is now (the refund the move took off)
            ROWOWStowedItem takenItem;
            takenItem.StowedItem = item;
            takenItem.Bag = bag;
            takenItem.Slot = slot;
            takenItems.push_back(takenItem);
        };
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; bagSlot++)
            if (Bag* bag = player->GetBagByPos(bagSlot))
                for (uint32 slot = 0; slot < bag->GetBagSize(); slot++)
                    takeItem(bagSlot, uint8(slot));
        for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; slot++)
            takeItem(INVENTORY_SLOT_BAG_0, slot);
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; slot++)
            takeItem(INVENTORY_SLOT_BAG_0, slot);
        return takenItems;
    }

    // Puts an item of a carried set in a free place of the carried slots.  Only the place is checked (a bag slot takes a bag, a bag takes what its kind
    // takes), not what a new item is: the item was there before, and a unique item's twin got while it was put away mustn't keep it out
    bool PutCarriedItem(Player* player, Item* item, uint8 bag, uint8 slot)
    {
        if (player->GetItemByPos(bag, slot) != nullptr)
            return false;
        if (bag == INVENTORY_SLOT_BAG_0)
        {
            if (IsCarriedBagSlot(bag, slot) == true ? item->ToBag() == nullptr : (slot < INVENTORY_SLOT_ITEM_START || slot >= INVENTORY_SLOT_ITEM_END))
                return false;
        }
        else
        {
            Bag* container = (bag >= INVENTORY_SLOT_BAG_START && bag < INVENTORY_SLOT_BAG_END) ? player->GetBagByPos(bag) : nullptr;
            if (container == nullptr || slot >= container->GetBagSize() || item->IsNotEmptyBag() == true ||
                ItemCanGoIntoBag(item->GetTemplate(), container->GetTemplate()) == false)
                return false;
        }
        // Player::MoveItemToInventory's, without the epic item achievement: the item comes back to the bags, it isn't new
        player->ItemAddedQuestCheck(item->GetEntry(), item->GetCount());
        ItemPosCountVec destination;
        destination.push_back(ItemPosCount((uint16(bag) << 8) | slot, item->GetCount()));
        Item* storedItem = player->StoreItem(destination, item, true);
        storedItem->SetState(ITEM_CHANGED, player);
        if (storedItem->IsBOPTradable() == true)
            player->AddTradeableItem(storedItem);
        return true;
    }

    // Each in the first free carried slot that takes it, the bags first into the free bag slots when they're to be worn
    void PutCarriedItemsAnywhere(Player* player, std::vector<Item*> const& items, bool areBagsWorn, std::vector<Item*>& unplacedItemsOut)
    {
        std::vector<Item*> remainingItems;
        for (Item* item : items)
        {
            bool isPlaced = false;
            if (areBagsWorn == true && item->ToBag() != nullptr)
                for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END && isPlaced == false; slot++)
                    isPlaced = PutCarriedItem(player, item, INVENTORY_SLOT_BAG_0, slot);
            if (isPlaced == false)
                remainingItems.push_back(item);
        }
        for (Item* item : remainingItems)
        {
            bool isPlaced = false;
            for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END && isPlaced == false; slot++)
                isPlaced = PutCarriedItem(player, item, INVENTORY_SLOT_BAG_0, slot);
            for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END && isPlaced == false; bagSlot++)
                if (Bag* bag = player->GetBagByPos(bagSlot))
                    for (uint32 slot = 0; slot < bag->GetBagSize() && isPlaced == false; slot++)
                        isPlaced = PutCarriedItem(player, item, bagSlot, uint8(slot));
            if (isPlaced == false)
                unplacedItemsOut.push_back(item);
        }
    }

    // Puts a carried set on the character, each item back in its place: the bags first (they hold the rest), then the backpack, then the bags' items.
    // One whose place is gone goes in any free slot; what has no room at all is returned
    std::vector<Item*> PutCarriedSet(Player* player, std::vector<ROWOWStowedItem> const& items, bool areBagsWorn)
    {
        std::vector<Item*> homelessItems;
        for (uint8 pass = 0; pass < 3; pass++)
        {
            for (ROWOWStowedItem const& stowedItem : items)
            {
                uint8 itemPass = IsCarriedBagSlot(stowedItem.Bag, stowedItem.Slot) == true ? 0 : (stowedItem.Bag == INVENTORY_SLOT_BAG_0 ? 1 : 2);
                if (itemPass != pass)
                    continue;
                if (PutCarriedItem(player, stowedItem.StowedItem, stowedItem.Bag, stowedItem.Slot) == false)
                    homelessItems.push_back(stowedItem.StowedItem);
            }
        }
        std::vector<Item*> unplacedItems;
        PutCarriedItemsAnywhere(player, homelessItems, areBagsWorn, unplacedItems);
        return unplacedItems;
    }

    // What no carried slot has room for goes by mail.  Each mail's transaction also takes its items out of the bags' rows and the put away set's, so
    // whenever the server stops, each item is in one place: in the mail, or where it was before
    void MailCarriedItems(Player* player, std::vector<Item*> const& items)
    {
        for (size_t firstIndex = 0; firstIndex < items.size(); firstIndex += MAX_MAIL_ITEMS)
        {
            CharacterDatabaseTransaction mailTrans = CharacterDatabase.BeginTransaction();
            MailDraft draft("Ragnarok Online", "Your bags had no room for this when they changed for Ragnarok Online, so it was sent to you here.");
            for (size_t itemIndex = firstIndex; itemIndex < items.size() && itemIndex < firstIndex + MAX_MAIL_ITEMS; itemIndex++)
            {
                Item* item = items[itemIndex];
                LOG_INFO("module.ROWOW", "Carried set of {}: item {} (entry {}) had no room, sent by mail", player->GetName(), item->GetGUID().GetCounter(), item->GetEntry());
                Item::DeleteFromInventoryDB(mailTrans, item->GetGUID().GetCounter());
                mailTrans->Append("DELETE FROM `mod_rowow_identity_stowed_item` WHERE `item_guid` = {}", item->GetGUID().GetCounter());
                item->SaveToDB(mailTrans);
                draft.AddItem(item);
            }
            draft.SendMailTo(mailTrans, MailReceiver(player), MailSender(MAIL_NORMAL, 0, MAIL_STATIONERY_GM));
            CharacterDatabase.CommitTransaction(mailTrans);
        }
    }
}

ROWOWIdentity::~ROWOWIdentity()
{
    // The put away carried set's items go with the identity (a logout, a reset): the player's slots don't have them
    for (ROWOWStowedItem const& stowedItem : StowedItems)
        delete stowedItem.StowedItem;
}

void ROWOWMod::LoadIdentityData()
{
    JobsByID.clear();
    StatusPointsByBaseLevel.clear();
    MapIDsByMapName.clear();
    MapNamesByMapID.clear();
    MapPartsByMapName.clear();
    MapPartsByMapKey.clear();
    MapPartIndexesByMapID.clear();
    MapPartsByMapID.clear();
    ItemTemplateIDsByItemID.clear();
    ItemTemplateIDs.clear();
    StartMapID = 0;
    StartItems.clear();
    StartZeny = 0;
    StartBagItemTemplateID = 0;
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
        // A map's own name (not one of rAthena's aliases for it) is the one the warp lists show.  A map stitched into a bigger one (RoMapMerges)
        // is a name of the bigger map, with its cells' offset there, its own size and the map it was (columns an older conversion doesn't have read 0)
        auto getColumn = [](char const* columnName)
        {
            bool hasColumn = WorldDatabase.Query("SELECT 1 FROM information_schema.columns WHERE table_schema = DATABASE() AND table_name = 'mod_rowow_map_name' AND column_name = '{}'",
                columnName) != nullptr;
            return hasColumn == true ? std::string("`") + columnName + "`" : std::string("0");
        };
        std::string partColumns = getColumn("CellOffsetX") + ", " + getColumn("CellOffsetY") + ", " + getColumn("CellWidth") + ", " + getColumn("CellHeight") + ", " +
            getColumn("FormerMapID");
        // A part's own name is its row that isn't an alias and has a size (prt_fild08); rAthena's other names for it (prt_fild08a) are aliases with its
        // offsets, and the stitched map's own row (no size) names the stitched map.  An older conversion made every name of a part an alias
        std::unordered_set<std::string> partOwnNames;
        if (QueryResult result = WorldDatabase.Query("SELECT `MapName`, `MapID`, {}, {} FROM `mod_rowow_map_name`", getColumn("IsAlias"), partColumns))
        {
            do
            {
                Field* fields = result->Fetch();
                std::string mapName = ToLower(fields[0].Get<std::string>());
                uint32 mapID = fields[1].Get<uint32>();
                bool isAlias = fields[2].Get<uint32>() != 0;
                ROWOWMapPart part;
                part.MapID = mapID;
                part.CellOffsetX = fields[3].Get<int32>();
                part.CellOffsetY = fields[4].Get<int32>();
                part.CellWidth = fields[5].Get<int32>();
                part.CellHeight = fields[6].Get<int32>();
                part.FormerMapID = fields[7].Get<uint32>();
                part.Name = mapName;
                MapIDsByMapName[mapName] = mapID;
                if (isAlias == false && part.HasArea() == false)
                    MapNamesByMapID[mapID] = mapName; // A part's own name is the part's (GetMapNameAt), not the stitched map's
                if (part.CellOffsetX != 0 || part.CellOffsetY != 0 || part.HasArea() == true || part.FormerMapID != 0)
                {
                    MapPartsByMapName[mapName] = part;
                    if (isAlias == false && part.HasArea() == true)
                        partOwnNames.insert(mapName);
                }
            } while (result->NextRow());
        }

        // Each stitched map's parts, the names of one part (its own and rAthena's aliases, prt_fild08a) being one part by its offset: the part is its
        // own name's row, or with none (an older conversion) the shortest name's, its size and former map from any of its rows
        std::map<std::pair<uint32, std::pair<int32, int32>>, ROWOWMapPart> partsByOffset;
        for (auto const& partByName : MapPartsByMapName)
        {
            ROWOWMapPart const& part = partByName.second;
            auto partEmplace = partsByOffset.emplace(std::make_pair(part.MapID, std::make_pair(part.CellOffsetX, part.CellOffsetY)), part);
            if (partEmplace.second == true)
                continue;
            ROWOWMapPart& groupPart = partEmplace.first->second;
            bool isOwnName = partOwnNames.count(part.Name) != 0;
            bool isGroupOwnName = partOwnNames.count(groupPart.Name) != 0;
            bool isShorter = part.Name.size() < groupPart.Name.size() || (part.Name.size() == groupPart.Name.size() && part.Name < groupPart.Name);
            ROWOWMapPart otherPart = part;
            if (isOwnName != isGroupOwnName ? isOwnName == true : isShorter == true)
            {
                otherPart = groupPart;
                groupPart = part;
            }
            if (groupPart.HasArea() == false)
            {
                groupPart.CellWidth = otherPart.CellWidth;
                groupPart.CellHeight = otherPart.CellHeight;
            }
            if (groupPart.FormerMapID == 0)
                groupPart.FormerMapID = otherPart.FormerMapID;
        }
        for (auto& partByOffset : partsByOffset)
            MapPartsByMapID[partByOffset.first.first].push_back(partByOffset.second);
        for (auto const& partsByMapID : MapPartsByMapID)
            for (ROWOWMapPart const& part : partsByMapID.second)
                if (part.FormerMapID != 0)
                    MapPartsByMapKey[part.FormerMapID] = &part;
        for (auto& partByName : MapPartsByMapName)
            if (auto partIter = partsByOffset.find(std::make_pair(partByName.second.MapID, std::make_pair(partByName.second.CellOffsetX, partByName.second.CellOffsetY)));
                partIter != partsByOffset.end())
                partByName.second = partIter->second; // Every name of a part is the part: its name, size and map key

        // GetMapPartAt takes the first part a cell is in, so parts that overlap give the cells they share to one of them
        for (auto const& partsByMapID : MapPartsByMapID)
        {
            std::vector<ROWOWMapPart> const& parts = partsByMapID.second;
            for (size_t partIndex = 0; partIndex < parts.size(); partIndex++)
            {
                ROWOWMapPart const& part = parts[partIndex];
                for (size_t otherIndex = partIndex + 1; otherIndex < parts.size(); otherIndex++)
                {
                    ROWOWMapPart const& other = parts[otherIndex];
                    if (part.HasArea() == false || other.HasArea() == false || part.CellOffsetX >= other.CellOffsetX + other.CellWidth ||
                        other.CellOffsetX >= part.CellOffsetX + part.CellWidth || part.CellOffsetY >= other.CellOffsetY + other.CellHeight ||
                        other.CellOffsetY >= part.CellOffsetY + part.CellHeight)
                        continue;
                    LOG_ERROR("module.ROWOW", "ROWOWMod::LoadIdentityData: the parts {} (cells {},{} size {}x{}) and {} (cells {},{} size {}x{}) of stitched map {} overlap, "
                        "and the cells they share are {}'s", part.Name, part.CellOffsetX, part.CellOffsetY, part.CellWidth, part.CellHeight, other.Name, other.CellOffsetX,
                        other.CellOffsetY, other.CellWidth, other.CellHeight, partsByMapID.first, part.Name);
                }
            }
        }

        // ...found by block, each part grown a cell (a gap cell is its nearer part's)
        for (auto const& partsByMapID : MapPartsByMapID)
        {
            if (partsByMapID.second.size() > std::numeric_limits<uint16>::max())
            {
                LOG_ERROR("module.ROWOW", "ROWOWMod::LoadIdentityData: stitched map {} has {} parts, too many to index, so they're looked up one by one",
                    partsByMapID.first, partsByMapID.second.size());
                continue;
            }
            std::vector<ROWOWCellRect> partRects;
            for (ROWOWMapPart const& part : partsByMapID.second)
                partRects.push_back({ part.CellOffsetX, part.CellOffsetY, part.HasArea() == true ? part.CellWidth : 0, part.HasArea() == true ? part.CellHeight : 0 });
            MapPartIndexesByMapID[partsByMapID.first].Build(partRects, 1);
        }
        if (MapPartsByMapName.empty() == false)
            LOG_INFO("module.ROWOW", "ROWOWMod::LoadIdentityData: {} RO map names are {} parts of {} stitched maps", MapPartsByMapName.size(), partsByOffset.size(),
                MapPartsByMapID.size());
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
                StartCellX = int32(strtol(std::string(pointFields[1]).c_str(), nullptr, 10));
                StartCellY = int32(strtol(std::string(pointFields[2]).c_str(), nullptr, 10));
                StartMapID = ResolveMapCell(std::string(pointFields[0]), StartCellX, StartCellY);
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

    // The general bag a new identity is given: the converter's one container in its RO item range (none from an older convert)
    for (auto const& itemTemplateByEntry : *sObjectMgr->GetItemTemplateStore())
    {
        ItemTemplate const& itemTemplate = itemTemplateByEntry.second;
        if (itemTemplate.ItemId < ROWOW_ITEM_TEMPLATE_ENTRY_START || itemTemplate.ItemId > ROWOW_ITEM_TEMPLATE_ENTRY_END || itemTemplate.Class != ITEM_CLASS_CONTAINER ||
            itemTemplate.ContainerSlots == 0)
            continue;
        if (StartBagItemTemplateID == 0 || itemTemplate.ItemId < StartBagItemTemplateID)
            StartBagItemTemplateID = itemTemplate.ItemId;
    }
    if (StartBagItemTemplateID == 0)
        LOG_ERROR("module.ROWOW", "ROWOWMod::LoadIdentityData found no start bag in the RO item range, so new RO identities get no bags");
    LOG_INFO("module.ROWOW", "ROWOWMod::LoadIdentityData loaded {} jobs, {} map names, {} items and {} start items (start map {} cell {},{}), start bag {}", JobsByID.size(),
        MapIDsByMapName.size(), ItemTemplateIDs.size(), StartItems.size(), StartMapID, StartCellX, StartCellY, StartBagItemTemplateID);
    MoveSavedSpotsOffStitchedMaps();
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

ROWOWMapPart const* ROWOWMod::GetMapPart(std::string const& mapName) const
{
    auto partIter = MapPartsByMapName.find(ToLower(mapName));
    if (partIter == MapPartsByMapName.end())
        return nullptr;
    return &partIter->second;
}

uint32 ROWOWMod::ResolveMapCell(std::string const& mapName, int32& cellX, int32& cellY) const
{
    // A map's own cells are the map's; a part of a stitched map (prt_fild08 of prt_fields) has its cells moved by its offset
    if (ROWOWMapPart const* part = GetMapPart(mapName))
    {
        cellX += part->CellOffsetX;
        cellY += part->CellOffsetY;
        return part->MapID;
    }
    return GetMapIDByMapName(mapName);
}

uint32 ROWOWMod::ResolveMapArea(std::string const& mapName, int32& x1, int32& y1, int32& x2, int32& y2) const
{
    // A box (mapwarp's and areawarp's -1 for the whole map): a part's box is moved like its cells, and its whole map is its own cells only
    ROWOWMapPart const* part = GetMapPart(mapName);
    if (part == nullptr)
        return GetMapIDByMapName(mapName);
    if (x1 < 0)
    {
        if (part->HasArea() == true)
        {
            x1 = part->CellOffsetX;
            y1 = part->CellOffsetY;
            x2 = part->CellOffsetX + part->CellWidth - 1;
            y2 = part->CellOffsetY + part->CellHeight - 1;
        }
        return part->MapID;
    }
    x1 += part->CellOffsetX;
    y1 += part->CellOffsetY;
    x2 += part->CellOffsetX;
    y2 += part->CellOffsetY;
    if (part->HasArea() == true)
    {
        // Kept to the part's cells, as rAthena keeps a box to its map's (one larger than the map would reach into the next part); a box wholly
        // off the part holds no cell (far past every map, never -1, which is "the whole map")
        int32 maxX = part->CellOffsetX + part->CellWidth - 1;
        int32 maxY = part->CellOffsetY + part->CellHeight - 1;
        int32 lowX = std::max(std::min(x1, x2), part->CellOffsetX);
        int32 highX = std::min(std::max(x1, x2), maxX);
        int32 lowY = std::max(std::min(y1, y2), part->CellOffsetY);
        int32 highY = std::min(std::max(y1, y2), maxY);
        if (lowX > highX || lowY > highY)
        {
            constexpr int32 noCell = std::numeric_limits<int32>::max() / 2;
            x1 = x2 = y1 = y2 = noCell;
        }
        else
        {
            x1 = lowX;
            x2 = highX;
            y1 = lowY;
            y2 = highY;
        }
    }
    return part->MapID;
}

void ROWOWMod::MoveSavedSpotsOffStitchedMaps()
{
    // A map that's now a part of a stitched one (its FormerMapID) no longer is a map: the characters on it, its home binds and corpses, and the
    // identities' save, last and return spots and memo points on it go to the same spot of the stitched map.  At startup, before any session (the
    // world thread, on the character database directly).  A former map's cells had its middle at the WoW origin (the converter's layout of one
    // map), so a spot's cell there is its cell in the part
    std::unordered_map<uint32, ROWOWMapPart const*> partsByFormerMapID;
    for (auto const& partByName : MapPartsByMapName)
        if (partByName.second.FormerMapID != 0 && partByName.second.HasArea() == true && GetMapGrid(partByName.second.MapID) != nullptr)
            partsByFormerMapID.emplace(partByName.second.FormerMapID, &partByName.second);
    if (partsByFormerMapID.empty() == true)
        return;
    std::string formerMapIDsText;
    for (auto const& partByFormerMapID : partsByFormerMapID)
        formerMapIDsText += (formerMapIDsText.empty() == true ? "" : ",") + std::to_string(partByFormerMapID.first);

    // The stitched map's position of a former map's spot (one off the part goes to the nearest cell of it; false when there's no position there)
    auto getStitchedPosition = [this, &partsByFormerMapID](uint32 formerMapID, float x, float y, ROWOWPosition& positionOut)
    {
        ROWOWMapPart const* part = partsByFormerMapID.at(formerMapID);
        ROWOWMapGrid const* mapGrid = GetMapGrid(part->MapID);
        float originX = -float(part->CellHeight) * mapGrid->CellSize * 0.5f;
        float originY = float(part->CellWidth) * mapGrid->CellSize * 0.5f;
        int32 cellX = std::clamp(int32(std::floor((originY - y) / mapGrid->CellSize)), 0, part->CellWidth - 1) + part->CellOffsetX;
        int32 cellY = std::clamp(int32(std::floor((x - originX) / mapGrid->CellSize)), 0, part->CellHeight - 1) + part->CellOffsetY;
        return GetCellPosition(part->MapID, cellX, cellY, positionOut);
    };
    // Each table's map column read as its own type: the core's are smallint unsigned (character_entry_point's int unsigned), the identity's signed
    // (-1 for none).  The zones the core keeps with them (characters.zone, character_homebind.zoneId) stay: the module doesn't know the stitched
    // map's areas, and the core works the character's zone out again once it's on the map
    auto getSmallMapID = [](Field const& field) { return uint32(field.Get<uint16>()); };
    auto getMapID = [](Field const& field) { return field.Get<uint32>(); };
    auto getSignedMapID = [](Field const& field) { return uint32(std::max<int32>(0, field.Get<int32>())); };
    uint32 movedCount = 0;
    auto moveSpots = [&](char const* tableName, char const* keyColumn, char const* mapColumn, char const* xColumn, char const* yColumn, char const* zColumn,
        uint32 (*readMapID)(Field const&))
    {
        QueryResult result = CharacterDatabase.Query("SELECT `{}`, `{}`, `{}`, `{}` FROM `{}` WHERE `{}` IN ({})", keyColumn, mapColumn, xColumn, yColumn, tableName,
            mapColumn, formerMapIDsText);
        if (!result)
            return;
        do
        {
            Field* fields = result->Fetch();
            uint32 formerMapID = readMapID(fields[1]);
            ROWOWPosition position;
            if (partsByFormerMapID.count(formerMapID) == 0 || getStitchedPosition(formerMapID, fields[2].Get<float>(), fields[3].Get<float>(), position) == false)
                continue;
            CharacterDatabase.DirectExecute("UPDATE `{}` SET `{}` = {}, `{}` = {}, `{}` = {}, `{}` = {} WHERE `{}` = {} AND `{}` = {}", tableName, mapColumn, position.MapID,
                xColumn, position.X, yColumn, position.Y, zColumn, position.Z, keyColumn, fields[0].Get<uint32>(), mapColumn, formerMapID);
            movedCount++;
        } while (result->NextRow());
    };
    moveSpots("characters", "guid", "map", "position_x", "position_y", "position_z", getSmallMapID);
    moveSpots("character_homebind", "guid", "mapId", "posX", "posY", "posZ", getSmallMapID);
    moveSpots("corpse", "guid", "mapId", "posX", "posY", "posZ", getSmallMapID);
    moveSpots("character_entry_point", "guid", "joinMapId", "joinX", "joinY", "joinZ", getMapID); // Where leaving a battleground or dungeon finder group goes
    moveSpots("mod_rowow_identity", "guid", "save_map", "save_x", "save_y", "save_z", getSignedMapID);
    moveSpots("mod_rowow_identity", "guid", "last_map", "last_x", "last_y", "last_z", getSignedMapID);
    moveSpots("mod_rowow_identity", "guid", "return_map", "return_x", "return_y", "return_z", getSignedMapID);

    // Memo points are cells of their map
    if (QueryResult result = CharacterDatabase.Query("SELECT `guid`, `slot`, `map`, `x`, `y` FROM `mod_rowow_identity_memo` WHERE `map` IN ({})", formerMapIDsText))
    {
        do
        {
            Field* fields = result->Fetch();
            auto partIter = partsByFormerMapID.find(fields[2].Get<uint32>());
            if (partIter == partsByFormerMapID.end())
                continue;
            ROWOWMapPart const* part = partIter->second;
            CharacterDatabase.DirectExecute("UPDATE `mod_rowow_identity_memo` SET `map` = {}, `x` = {}, `y` = {} WHERE `guid` = {} AND `slot` = {}", part->MapID,
                fields[3].Get<int32>() + part->CellOffsetX, fields[4].Get<int32>() + part->CellOffsetY, fields[0].Get<uint32>(), uint32(fields[1].Get<uint8>()));
            movedCount++;
        } while (result->NextRow());
    }
    if (movedCount > 0)
        LOG_INFO("module.ROWOW", "ROWOWMod::MoveSavedSpotsOffStitchedMaps moved {} saved spots from the maps now stitched into others ({})", movedCount, formerMapIDsText);
}

std::unordered_map<std::string, std::unordered_set<uint32>> ROWOWMod::GetMapIDsByMapFlag() const
{
    // RoMapFlags' rows in order (an "off" takes the flag away again), by any of a map's RO names, by map key: a part of a stitched map keeps its own
    // flags (Prontera's town flags stay inside Prontera)
    std::unordered_map<std::string, std::unordered_set<uint32>> mapIDsByFlag;
    ROWOWDataTable const* mapFlagTable = GetDataTable(ROWOW_DATA_TABLE_MAP_FLAG);
    if (mapFlagTable == nullptr)
        return mapIDsByFlag;
    int32 mapColumnIndex = mapFlagTable->GetColumnIndex("Map");
    int32 flagColumnIndex = mapFlagTable->GetColumnIndex("Flag");
    int32 valueColumnIndex = mapFlagTable->GetColumnIndex("Value");
    for (size_t rowIndex = 0; rowIndex < mapFlagTable->Rows.size(); rowIndex++)
    {
        uint32 mapKey = GetMapKeyByMapName(mapFlagTable->GetCell(rowIndex, mapColumnIndex));
        if (mapKey == 0)
            continue;
        std::string const& flag = mapFlagTable->GetCell(rowIndex, flagColumnIndex);
        if (mapFlagTable->GetCell(rowIndex, valueColumnIndex) == "off")
            mapIDsByFlag[flag].erase(mapKey);
        else
            mapIDsByFlag[flag].insert(mapKey);
    }
    return mapIDsByFlag;
}

ROWOWMapPart const* ROWOWMod::GetMapPartAt(uint32 mapID, int32 cellX, int32 cellY) const
{
    // A stitched map's parts are side by side with no overlap (Midgard has a couple of hundred, so they're found by block: the parts a cell's block
    // lists, in their order).  A cell in the 2-cell gap between two parts is the nearer one's (one cell off it), as the converter's sub-areas reach
    // the middle of the gaps: the gaps aren't walkable in RO, but WoW movement isn't held to the cells, and a step over a gap shouldn't leave the
    // part and come back (map-change steps twice)
    auto partsIter = MapPartsByMapID.find(mapID);
    if (partsIter == MapPartsByMapID.end())
        return nullptr;
    std::vector<ROWOWMapPart> const& parts = partsIter->second;
    auto isNear = [cellX, cellY](ROWOWMapPart const& part)
    {
        return part.HasArea() == true && cellX >= part.CellOffsetX - 1 && cellY >= part.CellOffsetY - 1 && cellX <= part.CellOffsetX + part.CellWidth &&
            cellY <= part.CellOffsetY + part.CellHeight;
    };
    ROWOWMapPart const* nearPart = nullptr;
    auto indexIter = MapPartIndexesByMapID.find(mapID);
    if (indexIter != MapPartIndexesByMapID.end() && indexIter->second.IsBuilt() == true)
    {
        uint16 const* candidate = nullptr;
        uint16 const* candidateEnd = nullptr;
        indexIter->second.GetCandidates(cellX, cellY, candidate, candidateEnd);
        for (; candidate != candidateEnd; ++candidate)
        {
            ROWOWMapPart const& part = parts[*candidate];
            if (part.HasCell(cellX, cellY) == true)
                return &part;
            if (nearPart == nullptr && isNear(part) == true)
                nearPart = &part;
        }
        return nearPart;
    }
    for (ROWOWMapPart const& part : parts)
    {
        if (part.HasCell(cellX, cellY) == true)
            return &part;
        if (nearPart == nullptr && isNear(part) == true)
            nearPart = &part;
    }
    return nearPart;
}

uint32 ROWOWMod::GetMapKey(uint32 mapID, int32 cellX, int32 cellY) const
{
    ROWOWMapPart const* part = GetMapPartAt(mapID, cellX, cellY);
    return part != nullptr ? part->GetMapKey() : mapID;
}

uint32 ROWOWMod::GetUnitMapKey(Unit const* unit) const
{
    // Only a stitched map's units need their cell
    if (MapPartsByMapID.find(unit->GetMapId()) == MapPartsByMapID.end())
        return unit->GetMapId();
    int32 x = 0, y = 0;
    GetUnitCell(unit, x, y);
    return GetMapKey(unit->GetMapId(), x, y);
}

bool ROWOWMod::IsOnSameROMap(Unit const* a, Unit const* b) const
{
    // Units on one WoW map share its thread, so their positions are safe to read here
    return a->IsInMap(b) == true && GetUnitMapKey(a) == GetUnitMapKey(b);
}

uint32 ROWOWMod::GetPositionMapKey(ROWOWPosition const& position) const
{
    ROWOWMapGrid const* mapGrid = GetMapGrid(position.MapID);
    if (mapGrid == nullptr || MapPartsByMapID.find(position.MapID) == MapPartsByMapID.end())
        return position.MapID;
    return GetMapKey(position.MapID, mapGrid->GetCellX(position.Y), mapGrid->GetCellY(position.X));
}

uint32 ROWOWMod::GetMapKeyByMapName(std::string const& mapName) const
{
    ROWOWMapPart const* part = GetMapPart(mapName);
    return part != nullptr ? part->GetMapKey() : GetMapIDByMapName(mapName);
}

uint32 ROWOWMod::GetMapIDOfMapKey(uint32 mapKey) const
{
    auto partIter = MapPartsByMapKey.find(mapKey);
    return partIter != MapPartsByMapKey.end() ? partIter->second->MapID : mapKey;
}

std::string ROWOWMod::GetMapNameAt(uint32 mapID, int32& cellX, int32& cellY) const
{
    // strcharinfo(3) and getmapxy: a part's own name and cells (prt_fild08 170,370; a gap cell its part's edge cell), the stitched map's own off
    // every part
    if (ROWOWMapPart const* part = GetMapPartAt(mapID, cellX, cellY))
    {
        cellX = std::clamp(cellX - part->CellOffsetX, 0, part->CellWidth - 1);
        cellY = std::clamp(cellY - part->CellOffsetY, 0, part->CellHeight - 1);
        return part->Name;
    }
    auto mapNameIter = MapNamesByMapID.find(mapID);
    return mapNameIter != MapNamesByMapID.end() ? mapNameIter->second : std::string();
}

std::string ROWOWMod::GetMapNameOfMapKey(uint32 mapKey) const
{
    auto partIter = MapPartsByMapKey.find(mapKey);
    if (partIter != MapPartsByMapKey.end())
        return partIter->second->Name;
    auto mapNameIter = MapNamesByMapID.find(mapKey);
    return mapNameIter != MapNamesByMapID.end() ? mapNameIter->second : std::string();
}

bool ROWOWMod::GetCellPosition(uint32 mapID, int32 cellX, int32 cellY, ROWOWPosition& positionOut) const
{
    // The ground height from the server's own cells, the GAT's: this can run on any map's thread for any map, so it touches no Map.  The arrival puts the
    // player down on the floor if it's left in the air (CheckArrivalFloor)
    ROWOWMapGrid const* mapGrid = GetMapGrid(mapID);
    if (mapGrid == nullptr || mapGrid->IsCellOnMap(cellX, cellY) == false)
        return false;
    positionOut.IsSet = true;
    positionOut.MapID = mapID;
    positionOut.X = mapGrid->GetCellCenterWorldX(cellY);
    positionOut.Y = mapGrid->GetCellCenterWorldY(cellX);
    positionOut.Z = mapGrid->GetCellGroundZ(cellX, cellY) + 0.5f; // A little above, so arriving there never starts under the ground
    positionOut.O = 0;
    return true;
}

void ROWOWMod::CheckNoSaveArrival(Player* player)
{
    // pc_makesavestatus: a nosave map keeps no last position, so a login there comes back at the save point.  In the player's update, once it's on the map
    ROWOWIdentity* identity = GetIdentity(player);
    if (identity->IsNoSaveLoginPending == false || player->IsBeingTeleported() == true || player->IsInWorld() == false)
        return;
    identity->IsNoSaveLoginPending = false;
    TeleportToSavePoint(player);
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

void ROWOWMod::UpdateMapKey(Player* player)
{
    // A stitched map's parts are RO maps of their own, so walking over a seam, or a warp from one part into another (or off every part), is RO's
    // map change (pc_setpos to another map) on one WoW map, which the core never sees as one: what a map change ends ends here as the core's map
    // change ends it (OnPlayerMapChanged's statuses, chat room and storage, OnPlayerBeforeTeleport's ground and timer skills; a warp's floor check
    // is OnPlayerBeforeTeleport's).  A change of WoW map is the core's own, and OnPlayerMapChanged keeps the key then.  The player's own map thread,
    // in the form, once it's landed
    ROWOWIdentity* identity = GetIdentity(player);
    if (player->IsBeingTeleported() == true || player->IsInWorld() == false)
        return;
    uint32 mapKey = GetUnitMapKey(player);
    if (mapKey == identity->MapKey)
        return;
    uint32 lastMapKey = identity->MapKey;
    identity->MapKey = mapKey;
    if (lastMapKey == 0 || GetMapIDOfMapKey(lastMapKey) != player->GetMapId())
        return; // The first update, or another WoW map
    StatusChangeClearOnChangeMap(player);
    SkillClearUnitGroup(player);
    SkillClearTimerSkill(player);
    ChatLeaveChat(player, false);
    StorageClose(player);
}

void ROWOWMod::UpdateLoginDeath(Player* player, uint32 diff)
{
    // pc_authok with 0 HP: an identity that logged out dead is dead again where it lay, with RO's restart window.  It dies a moment after
    // the login (the player's own thread), so the client already has the form and shows RO's window, not WoW's release.  Nothing of a
    // death runs again: no penalty, no events, it only lies dead
    ROWOWIdentity* identity = GetIdentity(player);
    if (identity->LoginDeathTimer == 0 || player->IsBeingTeleported() == true)
        return;
    if (identity->LoginDeathTimer > diff)
    {
        identity->LoginDeathTimer -= diff;
        return;
    }
    identity->LoginDeathTimer = 0;
    if (player->IsAlive() == false)
        return;
    player->SetHealth(0);
    player->KillPlayer();
}

ROWOWIdentity* ROWOWMod::GetIdentity(Player* player)
{
    ROWOWIdentity* identity = player->CustomData.GetDefault<ROWOWIdentity>("ROWOWIdentity");
    if (identity->IsLoaded == true)
        return identity;
    identity->IsLoaded = true;
    identity->CharacterGUID = player->GetGUID().GetCounter();
    LoadIdentitySlots(player, *identity);
    LoadIdentityRows(player, *identity);
    return identity;
}

bool ROWOWMod::HasMidgardPathfinder(Player const* player) const
{
    return player != nullptr && player->HasAchieved(ROWOW_ACHIEVEMENT_MIDGARD_PATHFINDER) == true;
}

bool ROWOWMod::HasMidgardWayfarer(Player const* player) const
{
    return player != nullptr && player->HasAchieved(ROWOW_ACHIEVEMENT_MIDGARD_WAYFARER) == true;
}

bool ROWOWMod::HasMidgardWorldwalker(Player const* player) const
{
    return player != nullptr && player->HasAchieved(ROWOW_ACHIEVEMENT_MIDGARD_WORLDWALKER) == true;
}

uint32 ROWOWMod::GetHighestBaseLevel(Player* player)
{
    // The identity in use, and the slot list's line of each one put away (read at load, and kept when a slot is put away: SelectIdentitySlot)
    ROWOWIdentity* identity = GetIdentity(player);
    uint32 highestBaseLevel = identity->Exists == true ? identity->BaseLevel : 0;
    for (uint8 slotIndex = 0; slotIndex < ROWOW_IDENTITY_SLOT_COUNT; slotIndex++)
    {
        ROWOWIdentitySlot const& identitySlot = identity->Slots[slotIndex];
        if (slotIndex + 1 == identity->Slot || identitySlot.Exists == false)
            continue;
        highestBaseLevel = std::max(highestBaseLevel, identitySlot.BaseLevel);
    }
    return highestBaseLevel;
}

void ROWOWMod::GrantMidgardAchievements(Player* player, uint32 baseLevel)
{
    if (IsEnabled == false || player == nullptr)
        return;
    // The core refuses achievements in GM mode (with a chat message), so they're left for the next check out of it (a login or an arrival in the RO world)
    if (player->IsGameMaster() == true)
        return;
    static const std::pair<uint32, uint32> achievementIDsAndBaseLevels[] =
    {
        { ROWOW_ACHIEVEMENT_MIDGARD_PATHFINDER, ROWOW_ACHIEVEMENT_MIDGARD_PATHFINDER_BASE_LEVEL },
        { ROWOW_ACHIEVEMENT_MIDGARD_WAYFARER, ROWOW_ACHIEVEMENT_MIDGARD_WAYFARER_BASE_LEVEL },
        { ROWOW_ACHIEVEMENT_MIDGARD_WORLDWALKER, ROWOW_ACHIEVEMENT_MIDGARD_WORLDWALKER_BASE_LEVEL },
    };
    for (auto const& achievementIDAndBaseLevel : achievementIDsAndBaseLevels)
    {
        if (baseLevel < achievementIDAndBaseLevel.second || player->HasAchieved(achievementIDAndBaseLevel.first) == true)
            continue;
        AchievementEntry const* achievementEntry = sAchievementStore.LookupEntry(achievementIDAndBaseLevel.first);
        if (achievementEntry == nullptr)
        {
            LOG_ERROR("module.ROWOW", "ROWOWMod::GrantMidgardAchievements error, no achievement with ID {} exists (base level {} of {})", achievementIDAndBaseLevel.first,
                baseLevel, player->GetName());
            continue;
        }
        player->CompletedAchievement(achievementEntry);
    }
}

void ROWOWMod::CheckMidgardAchievements(Player* player)
{
    if (IsEnabled == false || player == nullptr)
        return;
    GrantMidgardAchievements(player, GetHighestBaseLevel(player));
}

void ROWOWMod::LoadLastIdentityID()
{
    // The slots' identity IDs go on from the highest given (startup, before any session)
    if (QueryResult result = CharacterDatabase.Query("SELECT MAX(`identity`) FROM `mod_rowow_identity_slot` WHERE `identity` >= {}", ROWOW_IDENTITY_ID_SLOT_START))
        if (result->Fetch()[0].IsNull() == false)
            LastIdentityID = std::max<uint32>(result->Fetch()[0].Get<uint32>(), ROWOW_IDENTITY_ID_SLOT_START - 1);
}

void ROWOWMod::LoadIdentitySlots(Player* player, ROWOWIdentity& identity)
{
    // The slot picked and the slots' shared zeny (mod_rowow_identity_character), and each slot's name and identity.  A character from before the slots
    // has no rows: its identity, if it has one, is slot 1's (its ID is the character's GUID), and its zeny the shared zeny
    ObjectGuid::LowType guid = player->GetGUID().GetCounter();
    for (uint8 slotIndex = 0; slotIndex < ROWOW_IDENTITY_SLOT_COUNT; slotIndex++)
    {
        identity.Slots[slotIndex] = ROWOWIdentitySlot();
        identity.Slots[slotIndex].Name = GetIdentitySlotDefaultName(slotIndex + 1);
    }
    identity.Slots[0].IdentityID = guid;
    identity.Slot = 1;
    identity.IsZenyShared = false;
    identity.IsHairShared = false;
    identity.SpriteJobID = -1;
    identity.IsPecoMountLookEnabled = true;
    if (QueryResult result = CharacterDatabase.Query("SELECT `slot`, `zeny`, `hair_style`, `hair_color`, `sprite_job`, `peco_mount_look` FROM `mod_rowow_identity_character` WHERE `guid` = {}", guid))
    {
        Field* fields = result->Fetch();
        identity.Slot = uint8(std::clamp<uint32>(fields[0].Get<uint8>(), 1, ROWOW_IDENTITY_SLOT_COUNT));
        identity.Zeny = fields[1].Get<uint32>();
        identity.IsZenyShared = true;
        // The head its slots share (style 0: none yet, the slot in use has its own row's then, as before the shared head)
        if (fields[2].Get<uint8>() != 0)
        {
            identity.HairStyle = fields[2].Get<uint8>();
            identity.HairColor = fields[3].Get<uint8>();
            identity.IsHairShared = true;
        }
        identity.SpriteJobID = fields[4].Get<int16>();
        identity.IsPecoMountLookEnabled = fields[5].Get<uint8>() != 0;
    }
    if (QueryResult result = CharacterDatabase.Query("SELECT `slot`, `identity`, `name` FROM `mod_rowow_identity_slot` WHERE `guid` = {}", guid))
    {
        do
        {
            Field* fields = result->Fetch();
            uint8 slot = fields[0].Get<uint8>();
            if (slot < 1 || slot > ROWOW_IDENTITY_SLOT_COUNT)
                continue;
            ROWOWIdentitySlot& identitySlot = identity.Slots[slot - 1];
            if (slot != 1) // Slot 1's is always the character's GUID
                identitySlot.IdentityID = fields[1].Get<uint32>();
            if (fields[2].Get<std::string>().empty() == false)
                identitySlot.Name = fields[2].Get<std::string>();
            identitySlot.IsStored = true;
        } while (result->NextRow());
    }
    // A slot picked has its ID (a row lost would leave it none: slot 1 then)
    if (identity.Slots[identity.Slot - 1].IdentityID == 0)
        identity.Slot = 1;
    identity.IdentityID = identity.Slots[identity.Slot - 1].IdentityID;

    // What the slot list shows of each identity
    std::string identityIDs;
    for (ROWOWIdentitySlot const& identitySlot : identity.Slots)
        if (identitySlot.IdentityID != 0)
            identityIDs += fmt::format("{}{}", identityIDs.empty() == true ? "" : ",", identitySlot.IdentityID);
    if (QueryResult result = CharacterDatabase.Query("SELECT `guid`, `job`, `job_level`, `zeny`, `base_level` FROM `mod_rowow_identity` WHERE `guid` IN ({})", identityIDs))
    {
        do
        {
            Field* fields = result->Fetch();
            ObjectGuid::LowType identityID = fields[0].Get<uint32>();
            for (ROWOWIdentitySlot& identitySlot : identity.Slots)
            {
                if (identitySlot.IdentityID != identityID)
                    continue;
                identitySlot.Exists = true;
                identitySlot.JobID = fields[1].Get<uint16>();
                identitySlot.JobLevel = fields[2].Get<uint8>();
                identitySlot.BaseLevel = fields[4].Get<uint8>();
            }
            // From before the shared zeny: the identity's own is the shared zeny
            if (identity.IsZenyShared == false && identityID == identity.IdentityID)
            {
                identity.Zeny = fields[3].Get<uint32>();
                identity.IsZenyShared = true;
            }
        } while (result->NextRow());
    }
    LoadIdentityLook(player, identity);
}

void ROWOWMod::LoadIdentityLook(Player* player, ROWOWIdentity& identity)
{
    // The jobs the character's identities are or have been.  What the slots' jobs and the jobs leading to them lack is added (and written) now: the
    // identities from before the history, and a job set straight in the database
    identity.JobHistoryJobIDs.clear();
    if (QueryResult result = CharacterDatabase.Query("SELECT `job` FROM `mod_rowow_identity_job_history` WHERE `guid` = {}", identity.CharacterGUID))
    {
        do
            identity.JobHistoryJobIDs.insert(result->Fetch()[0].Get<uint16>());
        while (result->NextRow());
    }
    for (ROWOWIdentitySlot const& identitySlot : identity.Slots)
        if (identitySlot.Exists == true)
            RecordJobHistory(player, identity, identitySlot.JobID);
}

void ROWOWMod::RecordJobHistory(Player* /*player*/, ROWOWIdentity& identity, uint16 jobID)
{
    // Kept for the character, whichever slot reached the job, and never taken back (a reset slot's jobs stay).  Written at once, on its own: the
    // identity's save may be long after, and an identity reset has no save of the old one's
    for (uint16 historyJobID : GetJobHistoryJobIDs(jobID))
        if (identity.JobHistoryJobIDs.insert(historyJobID).second == true)
            CharacterDatabase.Execute("INSERT IGNORE INTO `mod_rowow_identity_job_history` (`guid`, `job`) VALUES ({}, {})", identity.CharacterGUID, historyJobID);
}

bool ROWOWMod::HasAnyIdentity(ROWOWIdentity const& identity) const
{
    return identity.Exists == true || HasOtherIdentity(identity) == true;
}

bool ROWOWMod::HasOtherIdentity(ROWOWIdentity const& identity) const
{
    // The slot list's lines of the slots put away (the slot in use reads the identity itself)
    for (uint8 slotIndex = 0; slotIndex < ROWOW_IDENTITY_SLOT_COUNT; slotIndex++)
        if (slotIndex + 1 != identity.Slot && identity.Slots[slotIndex].Exists == true)
            return true;
    return false;
}

bool ROWOWMod::HasJobIllusion(ROWOWJob const* job, bool isMale) const
{
    return job != nullptr && IllusionSpellIDsByJobKey.find(GetIllusionJobKey(job->IllusionJobKey, isMale)) != IllusionSpellIDsByJobKey.end();
}

ROWOWJob const* ROWOWMod::GetIdentityLookJob(Player* player)
{
    // The job picked, while it's one the character has been and has a sprite in its gender (a gender change may leave it without one); else the job of
    // the slot in use.  A picked job is shown whatever slot is in use, one with no identity yet too
    ROWOWIdentity* identity = GetIdentity(player);
    if (identity->SpriteJobID >= 0 && identity->JobHistoryJobIDs.find(uint16(identity->SpriteJobID)) != identity->JobHistoryJobIDs.end())
    {
        ROWOWJob const* spriteJob = GetJob(uint16(identity->SpriteJobID));
        if (HasJobIllusion(spriteJob, player->getGender() == GENDER_MALE) == true)
            return spriteJob;
    }
    return identity->Exists == true ? GetJob(identity->JobID) : nullptr;
}

std::string ROWOWMod::GetIdentityLookJobKey(Player* player)
{
    ROWOWJob const* job = GetIdentityLookJob(player);
    return job != nullptr ? job->IllusionJobKey : std::string();
}

uint32 ROWOWMod::GetIdentityLookIllusionSpellID(Player* player, bool isRiding)
{
    // The look's job in the character's gender: riding its Peco, the job's mounted body (R9.5; the converter's job key with "_riding") when the job
    // shown has one (a Knight shown as a Swordman rides unseen: the Swordman on foot)
    ROWOWJob const* job = GetIdentityLookJob(player);
    if (job == nullptr)
        return 0;
    bool isMale = player->getGender() == GENDER_MALE;
    auto illusionSpellIDIter = IllusionSpellIDsByJobKey.end();
    if (isRiding == true)
        illusionSpellIDIter = IllusionSpellIDsByJobKey.find(GetIllusionJobKey(job->IllusionJobKey + "_riding", isMale));
    if (illusionSpellIDIter == IllusionSpellIDsByJobKey.end())
        illusionSpellIDIter = IllusionSpellIDsByJobKey.find(GetIllusionJobKey(job->IllusionJobKey, isMale));
    return illusionSpellIDIter != IllusionSpellIDsByJobKey.end() ? illusionSpellIDIter->second : 0;
}

uint32 ROWOWMod::GetIdentityLookHeadSpellID(Player* player)
{
    ROWOWIdentity const* identity = GetIdentity(player);
    return GetHeadSpellID(identity->HairStyle, identity->HairColor, player->getGender() == GENDER_MALE);
}

std::vector<uint16> ROWOWMod::GetSpriteJobIDs(Player* player)
{
    // One job for each sprite the history's jobs have in the character's gender, lowest job ID first: a Novice High and a Baby look as the Novice does,
    // so the three are one entry (named by the lowest, the normal job when it's there)
    ROWOWIdentity* identity = GetIdentity(player);
    bool isMale = player->getGender() == GENDER_MALE;
    std::vector<uint16> spriteJobIDs;
    std::unordered_set<std::string> jobKeys;
    for (uint16 jobID : identity->JobHistoryJobIDs)
    {
        ROWOWJob const* job = GetJob(jobID);
        if (HasJobIllusion(job, isMale) == false || jobKeys.insert(job->IllusionJobKey).second == false)
            continue;
        spriteJobIDs.push_back(jobID);
    }
    return spriteJobIDs;
}

bool ROWOWMod::SetSpriteJob(Player* player, int32 jobID)
{
    // Options > Interface > Ragnarok (.ro spritejob): -1 for the job of the slot in use, else a job of the history, kept as the job its sprite is listed
    // under.  With no identity in any slot there's nothing to show it on, so only -1 is taken
    ROWOWIdentity* identity = GetIdentity(player);
    if (IsEnabled == false)
        return false;
    int32 spriteJobID = -1;
    if (jobID >= 0)
    {
        ROWOWJob const* job = jobID <= std::numeric_limits<uint16>::max() ? GetJob(uint16(jobID)) : nullptr;
        if (job == nullptr || HasAnyIdentity(*identity) == false || identity->JobHistoryJobIDs.find(uint16(jobID)) == identity->JobHistoryJobIDs.end())
            return false;
        for (uint16 listedJobID : GetSpriteJobIDs(player))
            if (GetJob(listedJobID)->IllusionJobKey == job->IllusionJobKey)
                spriteJobID = listedJobID;
        if (spriteJobID < 0) // No sprite in the character's gender
            return false;
    }
    if (spriteJobID != identity->SpriteJobID)
    {
        identity->SpriteJobID = spriteJobID;
        // Its row is there once a slot has had an identity (every save writes it too)
        CharacterDatabase.Execute("UPDATE `mod_rowow_identity_character` SET `sprite_job` = {} WHERE `guid` = {}", spriteJobID, identity->CharacterGUID);
        if (player->IsInWorld() == true)
            RefreshROFormLook(player); // In the form, or out of it showing the Ragnarok identity (RefreshWorldIdentityLook)
        if (identity->IsInForm == false && identity->IsPageShown == true)
            SendRagnarokPageToClient(player);
    }
    SendSpriteJobsToClient(player);
    return true;
}

bool ROWOWMod::SetPecoMountLook(Player* player, bool isEnabled)
{
    // Options > Interface > Ragnarok (.ro pecomount, Nathan 2026-11-10): whether, out of Midgard, a Peco rider's job sprite on a ground mount shows
    // its Peco in the mount's place (IsPecoShownForMount; the default), or sits on the mount as any other sprite does.  The character's own choice,
    // kept with its look (what the others see changes with it), and nothing of Midgard's: there the Peco is the RO character's own option
    ROWOWIdentity* identity = GetIdentity(player);
    if (IsEnabled == false || HasAnyIdentity(*identity) == false)
        return false;
    if (isEnabled != identity->IsPecoMountLookEnabled)
    {
        identity->IsPecoMountLookEnabled = isEnabled;
        // Its row is there once a slot has had an identity (every save writes it too)
        CharacterDatabase.Execute("UPDATE `mod_rowow_identity_character` SET `peco_mount_look` = {} WHERE `guid` = {}", isEnabled == true ? 1 : 0, identity->CharacterGUID);
        if (player->IsInWorld() == true)
            RefreshWorldIdentityLook(player); // On a mount now: the Peco or the mount at once
    }
    SendSpriteJobsToClient(player);
    return true;
}

void ROWOWMod::SendSpriteJobsToClient(Player* player)
{
    // ROWOWSPRITE: "<1 with an identity in any save slot, else 0><tab><the job picked, -1 for the job of the slot in use><tab><1 while the Peco is
    // shown in a ground mount's place out of Midgard, 0 with the mount shown as it is>", then for each job that may be picked "<tab><job ID>:<job
    // name>" (the name with spaces for rAthena's underscores).  A job picked that can't be shown now is told as -1.  Sent when asked for (.ro
    // spritejob), each time the client's interface has loaded (ROWOW_IdentityServerScript), on every arrival (CompleteROFormChange) and whenever
    // it changes
    ROWOWIdentity* identity = GetIdentity(player);
    std::vector<uint16> spriteJobIDs = GetSpriteJobIDs(player);
    int32 spriteJobID = -1;
    for (uint16 jobID : spriteJobIDs)
        if (int32(jobID) == identity->SpriteJobID)
            spriteJobID = identity->SpriteJobID;
    std::string addonMessage = fmt::format("ROWOWSPRITE\t{}\t{}\t{}", HasAnyIdentity(*identity) == true ? 1 : 0, spriteJobID, identity->IsPecoMountLookEnabled == true ? 1 : 0);
    for (uint16 jobID : spriteJobIDs)
    {
        std::string jobName = GetJob(jobID)->Name;
        std::replace(jobName.begin(), jobName.end(), '_', ' ');
        addonMessage += fmt::format("\t{}:{}", jobID, jobName);
    }
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_ADDON, nullptr, nullptr, addonMessage);
    player->SendDirectMessage(&data);
}

void ROWOWMod::LoadIdentityRows(Player* player, ROWOWIdentity& slotIdentity)
{
    // The identity of the slot picked, keyed by its ID.  Its zeny is the slots' shared zeny (LoadIdentitySlots), not its row's
    ROWOWIdentity* identity = &slotIdentity;
    uint32 guid = identity->IdentityID;
    QueryResult result = CharacterDatabase.Query("SELECT `job`, `base_level`, `base_exp`, `job_level`, `job_exp`, `status_points`, `skill_points`, `str`, `agi`, `vit`, `int`, `dex`, `luk`, "
        "`hp`, `sp`, `zeny`, `save_map`, `save_x`, `save_y`, `save_z`, `save_o`, `last_map`, `last_x`, `last_y`, `last_z`, `last_o`, `return_map`, `return_x`, `return_y`, "
        "`return_z`, `return_o`, `hair_style`, `hair_color`, `in_form`, `option`, `fame`, `carried_set` FROM `mod_rowow_identity` WHERE `guid` = {}", guid);
    if (!result)
    {
        // A new identity shares its account's variables: a # variable set before its first save must not rewrite them from an empty set
        LoadIdentityScriptVariables(player, *identity);
        return;
    }
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
    if (identity->IsZenyShared == false)
    {
        identity->Zeny = fields[15].Get<uint32>();
        identity->IsZenyShared = true;
    }
    ReadPosition(fields, 16, identity->SavePoint);
    ReadPosition(fields, 21, identity->LastPosition);
    ReadPosition(fields, 26, identity->ReturnPosition);
    if (identity->IsHairShared == false) // From before the slots' shared head: its own row's, the character's from its next save
    {
        identity->HairStyle = fields[31].Get<uint8>();
        identity->HairColor = fields[32].Get<uint8>();
    }
    identity->WasInFormAtLoad = fields[33].Get<uint8>() != 0;
    identity->Option = fields[34].Get<uint32>();
    identity->Fame = fields[35].Get<int32>();
    identity->CarriedSet = fields[36].Get<uint8>();
    identity->SavedCarriedSet = identity->CarriedSet;

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
    LoadIdentityStowedItems(player, *identity);
    LoadIdentityMemo(player, *identity);
    LoadIdentityQuests(player, *identity);
    LoadIdentityPet(player, *identity);
    LoadIdentityFollowers(player, *identity);
    PCCalcSkillTree(*identity);
}

void ROWOWMod::DeleteCharacterIdentityRows(ObjectGuid::LowType guid, CharacterDatabaseTransaction trans)
{
    // Every save slot's identity (the first one's ID is the character's GUID), the slots and their shared zeny
    std::vector<ObjectGuid::LowType> identityIDs = { guid };
    if (QueryResult result = CharacterDatabase.Query("SELECT `identity` FROM `mod_rowow_identity_slot` WHERE `guid` = {} AND `identity` <> {}", guid, guid))
    {
        do
            identityIDs.push_back(result->Fetch()[0].Get<uint32>());
        while (result->NextRow());
    }
    for (ObjectGuid::LowType identityID : identityIDs)
        DeleteIdentityRows(identityID, trans);
    trans->Append("DELETE FROM `mod_rowow_identity_slot` WHERE `guid` = {}", guid);
    trans->Append("DELETE FROM `mod_rowow_identity_character` WHERE `guid` = {}", guid);
    trans->Append("DELETE FROM `mod_rowow_identity_job_history` WHERE `guid` = {}", guid);
    DeleteWorldIdentityRows(guid, trans);
}

void ROWOWMod::DeleteIdentityRows(ObjectGuid::LowType guid, CharacterDatabaseTransaction trans)
{
    // What makes the identity, in tables of the character (its pet's row is the egg's: see OnPlayerDeleteFromDB).  The put away carried set's items
    // go with their rows: a deleted character's are gone already (owned by it), and a reset's are the identity's (the character's own are back on it)
    trans->Append("DELETE `instance` FROM `item_instance` `instance` JOIN `mod_rowow_identity_stowed_item` `stowed` ON `stowed`.`item_guid` = `instance`.`guid` WHERE `stowed`.`guid` = {}", guid);
    trans->Append("DELETE FROM `mod_rowow_identity_stowed_item` WHERE `guid` = {}", guid);
    trans->Append("DELETE FROM `mod_rowow_identity` WHERE `guid` = {}", guid);
    trans->Append("DELETE FROM `mod_rowow_identity_action` WHERE `guid` = {}", guid);
    trans->Append("DELETE FROM `mod_rowow_identity_equipment` WHERE `guid` = {}", guid);
    trans->Append("DELETE FROM `mod_rowow_identity_skill` WHERE `guid` = {}", guid);
    trans->Append("DELETE FROM `mod_rowow_identity_variable` WHERE `guid` = {}", guid);
    trans->Append("DELETE FROM `mod_rowow_identity_status` WHERE `guid` = {}", guid);
    trans->Append("DELETE FROM `mod_rowow_identity_cart` WHERE `guid` = {}", guid);
    trans->Append("DELETE FROM `mod_rowow_identity_memo` WHERE `guid` = {}", guid);
    trans->Append("DELETE FROM `mod_rowow_identity_quest` WHERE `guid` = {}", guid);
    trans->Append("DELETE FROM `mod_rowow_homunculus` WHERE `CharacterGUID` = {}", guid);
    trans->Append("DELETE FROM `mod_rowow_mercenary` WHERE `CharacterGUID` = {}", guid);
    trans->Append("DELETE FROM `mod_rowow_mercenary_owner` WHERE `CharacterGUID` = {}", guid);
}

void ROWOWMod::SaveIdentity(Player* player, CharacterDatabaseTransaction trans, bool isInventorySaved)
{
    ROWOWIdentity* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    if (identity == nullptr || identity->Exists == false)
        return;

    // In the form, the player's fields are the form's
    if (identity->IsInForm == true)
    {
        identity->Zeny = player->GetMoney();
        // Dead, or about to die again after its login: it logs in dead (the SP a respawn keeps stays as it was)
        if (player->IsAlive() == false)
            identity->HP = 0;
        else if (identity->LoginDeathTimer == 0)
        {
            identity->HP = player->GetHealth();
            identity->SP = player->GetPower(POWER_MANA);
        }
    }

    // Which carried set is on the character goes with the bags' rows: a save of the money alone keeps the one last saved with them
    if (isInventorySaved == true)
        identity->SavedCarriedSet = identity->CarriedSet;

    // The identity's rows are its slot's; the slot picked and the zeny go to the character's row, which the other slots share
    uint32 guid = identity->IdentityID;
    // ... with the head and the job sprite picked, which they share too
    trans->Append("REPLACE INTO `mod_rowow_identity_character` (`guid`, `slot`, `zeny`, `hair_style`, `hair_color`, `sprite_job`, `peco_mount_look`) VALUES ({}, {}, {}, {}, {}, {}, {})",
        player->GetGUID().GetCounter(), identity->Slot, identity->Zeny, identity->HairStyle, identity->HairColor, identity->SpriteJobID, identity->IsPecoMountLookEnabled == true ? 1 : 0);
    identity->IsZenyShared = true;
    identity->IsHairShared = identity->HairStyle != 0;
    ROWOWPosition const& savePoint = identity->SavePoint;
    ROWOWPosition const& lastPosition = identity->LastPosition;
    ROWOWPosition const& returnPosition = identity->ReturnPosition;
    trans->Append("REPLACE INTO `mod_rowow_identity` (`guid`, `job`, `base_level`, `base_exp`, `job_level`, `job_exp`, `status_points`, `skill_points`, `str`, `agi`, `vit`, "
        "`int`, `dex`, `luk`, `hp`, `sp`, `zeny`, `save_map`, `save_x`, `save_y`, `save_z`, `save_o`, `last_map`, `last_x`, `last_y`, `last_z`, `last_o`, `return_map`, "
        "`return_x`, `return_y`, `return_z`, `return_o`, `hair_style`, `hair_color`, `in_form`, `option`, `fame`, `carried_set`, `updated`) VALUES ({}, {}, {}, {}, {}, {}, {}, {}, "
        "{}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {})",
        guid, identity->JobID, identity->BaseLevel, identity->BaseExp, identity->JobLevel, identity->JobExp, identity->StatusPoints, identity->SkillPoints,
        identity->Stats[0], identity->Stats[1], identity->Stats[2], identity->Stats[3], identity->Stats[4], identity->Stats[5], identity->HP, identity->SP, identity->Zeny,
        GetPositionMapValue(savePoint), savePoint.X, savePoint.Y, savePoint.Z, savePoint.O, GetPositionMapValue(lastPosition), lastPosition.X, lastPosition.Y,
        lastPosition.Z, lastPosition.O, GetPositionMapValue(returnPosition), returnPosition.X, returnPosition.Y, returnPosition.Z, returnPosition.O,
        identity->HairStyle, identity->HairColor, identity->IsInForm == true ? 1 : 0, identity->Option, identity->Fame, identity->SavedCarriedSet,
        uint32(GameTime::GetGameTime().count()));

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
    if (isInventorySaved == true) // The cart and the put away carried set hold what left the bags: their rows go with theirs
    {
        SaveIdentityCart(player, *identity, trans);
        SaveIdentityStowedItems(player, *identity, trans);
    }
    SaveIdentityMemo(player, *identity, trans);
    SaveIdentityQuests(player, *identity, trans);
    SaveIdentityPet(player, *identity, trans);
    SaveIdentityFollowers(player, *identity, trans);
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
    identity->IsViewingROSet = false; // The carried set below is the form's now in any case

    // An arrival with no identity makes one (the Ragnarok page makes them, but a GM's teleport may come first).  The start items are given once the
    // character is in the world
    bool isNewIdentity = identity->Exists == false;
    if (isNewIdentity == true)
    {
        InitNewIdentity(*identity);
        identity->IsNewIdentityPending = true;
        RecordJobHistory(player, *identity, identity->JobID);
        SendSpriteJobsToClient(player);
    }

    // The world travel spell's way back on the form's action bars is World Return (identities from before it had the travel spell itself)
    for (auto& actionByButton : identity->ActionsByButton)
    {
        if (actionByButton.second == ROWOW_SPELL_WORLD_TRAVEL)
        {
            actionByButton.second = ROWOW_SPELL_WORLD_RETURN;
            identity->AreActionsChanged = true;
        }
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
    // Its stance or form, from one of its own spells: the arrival takes it off with the buffs, and leaving puts it back (RestoreWoWShapeshift)
    identity->WoWShapeshiftSpellID = 0;
    for (AuraEffect const* shapeshift : player->GetAuraEffectsByType(SPELL_AURA_MOD_SHAPESHIFT))
    {
        if (player->HasSpell(shapeshift->GetId()) == true)
        {
            identity->WoWShapeshiftSpellID = shapeshift->GetId();
            break;
        }
    }
    identity->IsInForm = true;

    // The form's carried set on the character, the character's own put away, before anything reads the RO equipment in it
    UpdateCarriedSet(player, *identity);

    // The client forgets the character's spells (the server keeps them, and refuses them while in the form), keeping the form's
    // With Midgard Worldwalker (Nathan 2026-11-09) none are forgotten: the character's own spells stay known in the form, their passives stay on
    // (CompleteROFormChange suspends the hidden spells' only) and they may be cast (IsWorldwalkerOpen)
    identity->HiddenSpellIDs.clear();
    bool isWorldwalkerOpen = HasMidgardWorldwalker(player);
    GetWorldIdentityState(player)->IsWorldwalkerOpen = isWorldwalkerOpen;
    uint8 activeSpec = player->GetActiveSpec();
    for (auto const& spellByID : player->GetSpellMap())
    {
        if (isWorldwalkerOpen == true)
            break;
        if (spellByID.second->State == PLAYERSPELL_REMOVED || spellByID.second->Active == false || spellByID.second->IsInSpec(activeSpec) == false)
            continue;
        if (IsSpellKeptInROForm(sSpellMgr->GetSpellInfo(spellByID.first)) == true)
            continue;
        identity->HiddenSpellIDs.push_back(spellByID.first);
    }
    for (auto const& talentBySpellID : player->GetTalentMap())
    {
        if (isWorldwalkerOpen == true)
            break;
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

        // An identity saved dead (0 HP) logs in dead where it lay, as rAthena keeps its 0 HP: once in the world (UpdateLoginDeath)
        if (isNewIdentity == false && identity->HP == 0)
            identity->LoginDeathTimer = ROWOW_LOGIN_DEATH_TIME_IN_MS;
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
    if (isROMap == true && identity->IsInForm == false && identity->IsLeavingForReset == false)
    {
        EnterROForm(player);
        return;
    }
    if (isROMap == false && identity->IsInForm == true)
    {
        LeaveROForm(player);
        return;
    }
    // Saved in the form but logging in out of the RO world (the server stopped before the character left it): its own carried set back on it
    UpdateCarriedSet(player, *identity);

    if (identity->IsInForm == true)
    {
        if (identity->IsFormArrivalPending == true)
        {
            // Leave the character's things behind: its pets and totems, its buffs, mount and shapeshift, and the passive auras of its
            // hidden spells and talents (no stat, regen or proc of the character's acts on the form).  Taking off an aura that raised
            // max health also takes that much health, so the form's HP and SP are put back after
            uint32 formHealth = player->GetHealth();
            uint32 formSP = player->GetPower(POWER_MANA);
            StowCharacterPet(player, *identity);
            player->UnsummonAllTotems();
            // (An identity's look is client hidden, which this leaves on; its sprite's head and headgear are kept too: RefreshROFormLook below
            // takes off what the form doesn't show)
            RemovePositiveAuras(player, [this](uint32 spellID) { return HeadSpellIDs.find(spellID) != HeadSpellIDs.end() || HeadgearSpellIDs.find(spellID) != HeadgearSpellIDs.end(); });
            identity->SuspendedPassiveSpellIDs.clear();
            for (uint32 spellID : identity->HiddenSpellIDs)
            {
                SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellID);
                if (spellInfo == nullptr || spellInfo->IsPassive() == false || player->HasAura(spellID) == false)
                    continue;
                player->RemoveAurasDueToSpell(spellID);
                identity->SuspendedPassiveSpellIDs.push_back(spellID);
            }
            // Taking off a stance or form puts the class's own power back (Player::InitDataForForm: a Warrior's Battle Stance gave rage,
            // which the Basic Info window and the unit frame showed as SP), so the form's SP is the power again
            player->setPowerType(POWER_MANA);
            if (player->IsAlive() == true)
            {
                player->SetHealth(std::min(formHealth, player->GetMaxHealth()));
                player->SetPower(POWER_MANA, std::min(formSP, player->GetMaxPower(POWER_MANA)));
            }

            // A new identity gets its general bags, then the start items in the bags and worn in their RO equipment positions, and is saved
            // with them
            if (identity->IsNewIdentityPending == true)
            {
                GiveNewIdentityStartItems(player, *identity);
                RefreshROFormValues(player, *identity); // The start weapon and armor count from now
                player->SaveToDB(false, false);
            }

            ROWOWJob const* job = GetJob(identity->JobID);
            ChatHandler(player->GetSession()).PSendSysMessage("You are a Ragnarok Online {}, base level {} and job level {}.", job != nullptr ? job->Name : "?",
                identity->BaseLevel, identity->JobLevel);

            // The Midgard achievements of an identity already past their levels (any save slot's)
            CheckMidgardAchievements(player);

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
            RemovePositiveAuras(player, [this](uint32 spellID) { return HeadSpellIDs.find(spellID) != HeadSpellIDs.end() || HeadgearSpellIDs.find(spellID) != HeadgearSpellIDs.end(); });
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
            RestoreCharacterPet(player, *identity);
            player->ResummonPetTemporaryUnSummonedIfAny();
        }
        identity->SuspendedPassiveSpellIDs.clear();
        RestoreWoWShapeshift(player, *identity);
        if (player->IsAlive() == true)
        {
            player->SetHealth(std::min(health, player->GetMaxHealth()));
            for (uint8 powerIndex = 0; powerIndex < MAX_POWERS; powerIndex++)
                player->SetPower(Powers(powerIndex), std::min(powers[powerIndex], player->GetMaxPower(Powers(powerIndex))));
        }
    }
    bool isFormChanged = identity->IsFormArrivalPending == true || identity->IsFormDeparturePending == true || identity->WasInFormAtLoad == true;
    identity->IsFormArrivalPending = false;
    identity->IsFormDeparturePending = false;
    identity->WasInFormAtLoad = false;
    if (isFormChanged == true)
        SendFormActionButtons(player, *identity); // In the world now, so the client takes every button (the arrival's left the other side's on the empty ones)
    // The identity for the world arrived in (out of the form too: the Ragnarok or EverQuest identity in Azeroth and Norrath), and the client told of it
    RefreshWorldIdentityLook(player);
    SendWorldIdentityToClient(player, true);
    SendSpriteJobsToClient(player); // With the identities: an interface that loaded while the character was between maps wasn't told (ROWOW_IdentityServerScript)
    SendROFormStateToClient(player);
    SendWorldTravelSpellsToClient(player);
    SendROStatusToClient(player);
    if (identity->IsInForm == true)
    {
        SendSkillsToClient(player, *identity, true);
        SendCartToClient(player, *identity, true);
        // clif_parse_LoadEndAck: arriving on a map makes the player invincible for a moment (pc_setinvincibletimer), and its pet arrives with it
        PCSetInvincibleTimer(player);
        PetArrive(player);
        FollowerArrive(player);
    }
}

void ROWOWMod::InitNewIdentity(ROWOWIdentity& identity) const
{
    // rAthena's new character: a level 1 Novice with no exp, zeny to start, and its save point at the start point.  Its HP and SP are full on its
    // first arrival: past any maximum, which EnterROForm holds them under (0 HP would be a death)
    identity.Exists = true;
    identity.JobID = ROWOW_JOB_NOVICE;
    identity.BaseLevel = 1;
    identity.JobLevel = 1;
    identity.BaseExp = 0;
    identity.JobExp = 0;
    auto statusPointsIter = StatusPointsByBaseLevel.find(1);
    identity.StatusPoints = statusPointsIter != StatusPointsByBaseLevel.end() ? statusPointsIter->second : 0;
    identity.SkillPoints = 0;
    for (uint8 statIndex = 0; statIndex < ROWOW_STAT_COUNT; statIndex++)
        identity.Stats[statIndex] = 1;
    // The character's first identity starts with zeny; one in another save slot shares what the others have
    if (identity.IsZenyShared == false)
        identity.Zeny = StartZeny;
    identity.HP = ROWOW_IDENTITY_NEW_HP_SP;
    identity.SP = ROWOW_IDENTITY_NEW_HP_SP;
    identity.SavePoint = ROWOWPosition();
    GetCellPosition(StartMapID, StartCellX, StartCellY, identity.SavePoint);
    identity.LastPosition = ROWOWPosition();
    identity.ActionsByButton.clear();
    identity.ActionsByButton[0] = ROWOW_SPELL_AUTO_ATTACK;
    identity.ActionsByButton[1] = ROWOW_SPELL_WORLD_RETURN;
    identity.AreActionsChanged = true;
    identity.ItemGUIDsByEquipPosition.clear();
    identity.IsEquipmentChanged = true;
    identity.SkillLevelsByID.clear();
    identity.AreSkillsChanged = true;
    identity.Variables.Clear();
    identity.SavedStatusChanges.clear();
    identity.AreStatusChangesChanged = true;
    identity.CartItems.fill(ROWOWCartItem());
    identity.CartNum = 0;
    identity.CartWeight = 0;
    identity.AreCartItemsChanged = true;
    identity.MemoPoints.fill(ROWOWMemoPoint());
    identity.AreMemoPointsChanged = true;
    identity.Quests.clear();
    identity.AreQuestsChanged = true;
    identity.Option = 0;
}

void ROWOWMod::GiveNewIdentityStartItems(Player* player, ROWOWIdentity& identity)
{
    // Into the form's carried set, which is on the character (the arrival's, or the Ragnarok page's): the general bags, then the start items, each
    // worn in its RO equipment position.  Once
    if (identity.IsNewIdentityPending == false || IsROSetCarried(player) == false)
        return;
    GiveStartBags(player);
    for (ROWOWStartItem const& startItem : StartItems)
    {
        Item* item = nullptr;
        GiveStartItemOrMail(player, startItem.ItemTemplateID, startItem.Amount, &item);
        if (item != nullptr && startItem.EquipPosition != 0) // One sent by mail isn't worn
            identity.ItemGUIDsByEquipPosition[startItem.EquipPosition] = item->GetGUID().GetCounter();
    }
    identity.IsEquipmentChanged = true;
    identity.IsNewIdentityPending = false;
}

void ROWOWMod::GiveStartItemOrMail(Player* player, uint32 itemTemplateID, uint32 amount, Item** itemOut)
{
    // Into the bags, or with no room, by mail (the mail is the character's, taken out of the form)
    Item* item = nullptr;
    ItemPosCountVec itemDestination;
    if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, itemDestination, itemTemplateID, amount) == EQUIP_ERR_OK)
    {
        item = player->StoreNewItem(itemDestination, itemTemplateID, true);
        if (item != nullptr)
            player->SendNewItem(item, amount, true, false);
    }
    if (itemOut != nullptr)
        *itemOut = item;
    if (item != nullptr)
        return;
    CharacterDatabaseTransaction mailTrans = CharacterDatabase.BeginTransaction();
    MailDraft draft("Ragnarok Online", "Your bags were too full for this, so it was sent to you here.");
    if (Item* mailItem = Item::CreateItem(itemTemplateID, amount, player))
    {
        mailItem->SaveToDB(mailTrans);
        draft.AddItem(mailItem);
    }
    draft.SendMailTo(mailTrans, MailReceiver(player), MailSender(MAIL_NORMAL, 0, MAIL_STATIONERY_GM));
    CharacterDatabase.CommitTransaction(mailTrans);
}

void ROWOWMod::GiveStartBags(Player* player)
{
    // RO has 100 inventory entries of up to 30,000, a new Warcraft character a 16 slot backpack, so a new identity is given a few general bags
    // (Nathan 2026-11-02).  Each goes in an empty bag slot; once those are taken, into the bags, and with no room there, by mail
    if (StartBagItemTemplateID == 0)
        return;
    for (uint32 bagIndex = 0; bagIndex < ROWOW_START_BAG_COUNT; bagIndex++)
    {
        uint16 equipDestination = 0;
        if (player->CanEquipNewItem(NULL_SLOT, equipDestination, StartBagItemTemplateID, false) == EQUIP_ERR_OK)
        {
            if (Item* bag = player->EquipNewItem(equipDestination, StartBagItemTemplateID, true))
            {
                player->SendNewItem(bag, 1, true, false);
                continue;
            }
        }
        GiveStartItemOrMail(player, StartBagItemTemplateID, 1, nullptr);
    }
}

void ROWOWMod::UpdateCarriedSet(Player* player, ROWOWIdentity& identity)
{
    // The character and its RO identity each have their own carried set: the backpack, the bag slots and the bags' items (the worn equipment, keyring,
    // currencies, bank and buyback stay the character's).  The side the character is on has its set on it, and the other is put away (StowedItems),
    // each item as it is with its place, and comes back to it (Nathan 2026-11-02).  A new identity's set starts empty, and the arrival gives the start
    // bags and items.  The changed rows (the bags', the items', the put away set's and carried_set) are written only by a save of the inventory, in its
    // transaction (SaveIdentityStowedItems), so whenever the server stops the database has each item in exactly one place.  Runs where the form
    // changes: the world thread (a login, a map change, a command), with the maps idle
    // The form's set is also on the character while the Ragnarok page shows it (IsViewingROSet)
    uint8 carriedSet = (identity.IsInForm == true || identity.IsViewingROSet == true) ? ROWOW_CARRIED_SET_RO : ROWOW_CARRIED_SET_WOW;
    if (identity.CarriedSet == carriedSet || (identity.CarriedSet == ROWOW_CARRIED_SET_NONE && carriedSet == ROWOW_CARRIED_SET_WOW))
        return;

    // Nothing in the bags may be in use as they change (the form change already stopped casts): a trade offering them, the loot of one being opened
    if (player->GetTradeData() != nullptr)
        player->TradeCancel(true);
    if (ObjectGuid lootGUID = player->GetLootGUID())
        player->GetSession()->DoLootRelease(lootGUID);

    std::vector<ROWOWStowedItem> takenItems = TakeCarriedSet(player);
    std::vector<Item*> unplacedItems;
    if (identity.CarriedSet == ROWOW_CARRIED_SET_NONE && identity.IsNewIdentityPending == false)
    {
        // An identity from before the sets were apart, its RO items (the converter's RO item range, its bags with them) mixed in with the character's.
        // The character's things become its set, each in its place (one that was in an RO bag where there's room), and are put away; the RO things
        // become the form's set, each in its place, the RO bags in the bag slots.  None is put away yet, but any that were go in too, so none is lost
        takenItems.insert(takenItems.end(), identity.StowedItems.begin(), identity.StowedItems.end());
        identity.StowedItems.clear();
        std::vector<ROWOWStowedItem> characterItems;
        std::vector<ROWOWStowedItem> identityItems;
        for (ROWOWStowedItem takenItem : takenItems)
        {
            uint32 itemTemplateID = takenItem.StowedItem->GetEntry();
            if (itemTemplateID < ROWOW_ITEM_TEMPLATE_ENTRY_START || itemTemplateID > ROWOW_ITEM_TEMPLATE_ENTRY_END)
            {
                characterItems.push_back(takenItem);
                continue;
            }
            if (takenItem.StowedItem->ToBag() != nullptr && IsCarriedBagSlot(takenItem.Bag, takenItem.Slot) == false)
                takenItem.Slot = NULL_SLOT; // An RO bag given while the bag slots were full goes in one now
            identityItems.push_back(takenItem);
        }
        std::vector<Item*> unplacedCharacterItems = PutCarriedSet(player, characterItems, false);
        identity.StowedItems = TakeCarriedSet(player);
        unplacedItems = PutCarriedSet(player, identityItems, true);
        // The character's things its own set has no room for stay with the form's rather than leave the bags
        PutCarriedItemsAnywhere(player, unplacedCharacterItems, false, unplacedItems);
        LOG_INFO("module.ROWOW", "Carried set of {}: set apart, {} of the character's things put away and {} of the identity's in the bags ({} of the character's stay with them)",
            player->GetName(), identity.StowedItems.size(), identityItems.size(), unplacedCharacterItems.size());
    }
    else
    {
        unplacedItems = PutCarriedSet(player, identity.StowedItems, false);
        identity.StowedItems = takenItems;
    }
    // Only a set saved wrong (or a mixed one too full) leaves anything with no room
    MailCarriedItems(player, unplacedItems);
    identity.CarriedSet = carriedSet;
    identity.AreStowedItemsChanged = true;
}

bool ROWOWMod::IsROSetCarried(Player* player) const
{
    ROWOWIdentity const* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    return identity != nullptr && (identity->IsInForm == true || identity->IsViewingROSet == true) && identity->CarriedSet == ROWOW_CARRIED_SET_RO;
}

bool ROWOWMod::IsStartBagMoveRefused(Player* player, uint8 sourceBag, uint8 sourceSlot, uint8 destinationBag, uint8 destinationSlot) const
{
    // The form's Adventurer's Bags can't be taken off, in the RO world or on the Ragnarok page (Nathan 2026-11-04): one only changes places with another
    // bag slot.  A move puts each of its two places' items in the other, so a bag slot holding one may only meet another bag slot (a destroy, or a move to
    // anywhere in the bags, has no bag slot on its other side)
    if (StartBagItemTemplateID == 0 || IsROSetCarried(player) == false)
        return false;
    auto isStartBagAt = [player, this](uint8 bag, uint8 slot)
    {
        Item* item = IsCarriedBagSlot(bag, slot) == true ? player->GetItemByPos(bag, slot) : nullptr;
        return item != nullptr && item->GetEntry() == StartBagItemTemplateID;
    };
    if (isStartBagAt(sourceBag, sourceSlot) == true && IsCarriedBagSlot(destinationBag, destinationSlot) == false)
        return true;
    return isStartBagAt(destinationBag, destinationSlot) == true && IsCarriedBagSlot(sourceBag, sourceSlot) == false;
}

void ROWOWMod::StartViewingROSet(Player* player)
{
    // The Ragnarok page is shown (.ro view 1): the form's carried set on the character, so the page's equipment slots and the bags are the identity's.
    // Not in combat or dead (the character's own bags are what it fights with), nor while an item is in use (a trade, a loot window)
    ROWOWIdentity* identity = GetIdentity(player);
    if (IsEnabled == false || identity->IsInForm == true)
        return;
    if (identity->Exists == false)
    {
        SendRagnarokPageToClient(player);
        return;
    }
    char const* blockedText = nullptr;
    if (player->IsInCombat() == true)
        blockedText = "You can't open your Ragnarok Online character's bags in combat.";
    else if (player->IsAlive() == false)
        blockedText = "You can't open your Ragnarok Online character's bags while dead.";
    else if (player->IsBeingTeleported() == true || player->GetTradeData() != nullptr || player->GetLootGUID() || player->IsNonMeleeSpellCast(false) == true)
        blockedText = "You can't open your Ragnarok Online character's bags right now.";
    if (blockedText != nullptr)
    {
        SendRagnarokPageToClient(player, blockedText);
        return;
    }
    if (identity->IsViewingROSet == false)
    {
        identity->IsViewingROSet = true;
        UpdateCarriedSet(player, *identity);
    }
    UpdatePageAreaName(*identity);
    SendRagnarokPageToClient(player);
    SendEquipmentToClient(player, *identity, true);
    SendItemCardsToClient(player, *identity, true);
    SendHairToClient(player);
}

void ROWOWMod::StopViewingROSet(Player* player)
{
    // The page is closed (.ro view 0), or the character does something its own set is for (CanPacketReceive's list, combat, a teleport, death, logging
    // out): its own carried set back on it, the form's put away
    ROWOWIdentity* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    if (identity == nullptr || identity->IsViewingROSet == false)
        return;
    identity->IsViewingROSet = false;
    UpdateCarriedSet(player, *identity);
    SendRagnarokPageToClient(player);
}

void ROWOWMod::SendRagnarokPageToClient(Player* player, char const* blockedText)
{
    // ROWOWPAGE: "N" with no identity yet in any save slot (the page then has the hair picked), "M<tab><hair style><tab><hair color>" with none in the
    // slot in use but one in another (a new one here has their head: no hair to pick), "B<tab><why>" when the bags can't be opened now, else "V" (its carried set is on the character) or "C" (put
    // away), then "<tab><illusion job key of the job sprite shown: GetIdentityLookJob><tab><job name><tab><base level><tab><job level><tab><zeny><tab><base exp><tab><HP><tab><SP><tab><STR>,<AGI>,
    // <VIT>,<INT>,<DEX>,<LUK><tab><area>" (the HP and SP as saved: past any maximum for a new identity, which is full).  The save slots' list comes first
    ROWOWIdentity* identity = GetIdentity(player);
    SendIdentitySlotsToClient(player);
    std::string addonMessage;
    if (identity->Exists == false && HasOtherIdentity(*identity) == true)
        addonMessage = fmt::format("ROWOWPAGE\tM\t{}\t{}", identity->HairStyle, identity->HairColor);
    else if (identity->Exists == false)
        addonMessage = "ROWOWPAGE\tN";
    else if (blockedText != nullptr)
        addonMessage = fmt::format("ROWOWPAGE\tB\t{}", blockedText);
    else
    {
        ROWOWJob const* job = GetJob(identity->JobID);
        addonMessage = fmt::format("ROWOWPAGE\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{},{},{},{},{},{}\t{}", identity->IsViewingROSet == true ? "V" : "C",
            GetIdentityLookJobKey(player), job != nullptr ? job->Name : "?", identity->BaseLevel, identity->JobLevel, identity->Zeny, identity->BaseExp,
            identity->HP, identity->SP, identity->Stats[0], identity->Stats[1], identity->Stats[2], identity->Stats[3], identity->Stats[4], identity->Stats[5],
            identity->PageAreaName);
    }
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_ADDON, nullptr, nullptr, addonMessage);
    player->SendDirectMessage(&data);
}

void ROWOWMod::UpdatePageAreaName(ROWOWIdentity& identity) const
{
    // Where the world travel spell takes the identity (where it left the RO world, else its save point: a new one's is the start point), as the area's name.
    // The base map's area lookup may create the map and load its terrain, so only from the world thread (the page's commands), with the maps idle
    ROWOWPosition const& position = identity.LastPosition.IsSet == true ? identity.LastPosition : identity.SavePoint;
    identity.PageAreaName.clear();
    if (position.IsSet == false)
        return;
    uint32 areaID = sMapMgr->GetAreaId(PHASEMASK_NORMAL, position.MapID, position.X, position.Y, position.Z);
    if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaID))
        identity.PageAreaName = area->area_name[sWorld->GetDefaultDbcLocale()];
}

bool ROWOWMod::CreateIdentity(Player* player, uint32 hairStyle, uint32 hairColor)
{
    // The Ragnarok page's character creation (.ro create): a new Novice with the hair picked, given its general bags and start items in its carried set
    // (which the page then shows), and saved with them.  It goes into the RO world by the page's Enter button, to the training ground
    ROWOWIdentity* identity = GetIdentity(player);
    if (IsEnabled == false || identity->Exists == true || identity->IsInForm == true || IsROMap(player->GetMapId()) == true)
        return false;
    // One head for every save slot (Nathan 2026-11-09): the hair is picked for the character's first identity only, and one made beside others has theirs
    bool isHairPicked = HasOtherIdentity(*identity) == false;
    if (isHairPicked == true && HeadSpellIDsByKey.find(GetHeadKey(hairStyle, hairColor, player->getGender() == GENDER_MALE)) == HeadSpellIDsByKey.end())
        return false;
    if (player->IsInCombat() == true || player->IsAlive() == false || player->IsBeingTeleported() == true)
        return false;
    InitNewIdentity(*identity);
    identity->IsNewIdentityPending = true;
    if (isHairPicked == true)
    {
        identity->HairStyle = uint8(hairStyle);
        identity->HairColor = uint8(hairColor);
    }
    RecordJobHistory(player, *identity, identity->JobID);
    identity->IsViewingROSet = true;
    UpdateCarriedSet(player, *identity);
    GiveNewIdentityStartItems(player, *identity);
    player->SaveToDB(false, false);
    UpdatePageAreaName(*identity);
    LOG_INFO("module.ROWOW", "{} made a Ragnarok Online character on the Ragnarok page (hair {} {}, {})", player->GetName(), identity->HairStyle, identity->HairColor,
        isHairPicked == true ? "picked" : "its other save slots'");
    SendSpriteJobsToClient(player);
    SendRagnarokPageToClient(player);
    SendEquipmentToClient(player, *identity, true);
    SendItemCardsToClient(player, *identity, true);
    SendHairToClient(player);
    return true;
}

void ROWOWMod::SendIdentitySlotsToClient(Player* player)
{
    // ROWOWSLOTS: "<slot in use>", then for each slot "<tab><name><tab>E<tab><job name><tab><job level>" with an identity, "<tab><name><tab>N<tab><tab>"
    // with none yet
    ROWOWIdentity* identity = GetIdentity(player);
    std::string addonMessage = fmt::format("ROWOWSLOTS\t{}", identity->Slot);
    for (uint8 slotIndex = 0; slotIndex < ROWOW_IDENTITY_SLOT_COUNT; slotIndex++)
    {
        ROWOWIdentitySlot const& identitySlot = identity->Slots[slotIndex];
        bool isInUse = slotIndex + 1 == identity->Slot;
        bool exists = isInUse == true ? identity->Exists : identitySlot.Exists;
        ROWOWJob const* job = exists == true ? GetJob(isInUse == true ? identity->JobID : identitySlot.JobID) : nullptr;
        if (exists == true)
            addonMessage += fmt::format("\t{}\tE\t{}\t{}", identitySlot.Name, job != nullptr ? job->Name : "?", isInUse == true ? identity->JobLevel : identitySlot.JobLevel);
        else
            addonMessage += fmt::format("\t{}\tN\t\t", identitySlot.Name);
    }
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_ADDON, nullptr, nullptr, addonMessage);
    player->SendDirectMessage(&data);
}

char const* ROWOWMod::GetIdentitySlotBlockedText(Player* player, ROWOWIdentity const& identity) const
{
    // Out of the RO world only (the Ragnarok page), with nothing under way that the identity's things or the character's bags are in
    if (identity.IsInForm == true || IsROMap(player->GetMapId()) == true)
        return "You can't change your Ragnarok Online character in the Ragnarok Online world.";
    if (player->IsInCombat() == true)
        return "You can't change your Ragnarok Online character in combat.";
    if (player->IsAlive() == false)
        return "You can't change your Ragnarok Online character while dead.";
    if (player->IsBeingTeleported() == true || player->GetTradeData() != nullptr || player->GetLootGUID() || player->IsNonMeleeSpellCast(false) == true ||
        IsWorldTravelRequested(player) == true || identity.IsLeavingForReset == true)
        return "You can't change your Ragnarok Online character right now.";
    return nullptr;
}

bool ROWOWMod::SelectIdentitySlot(Player* player, uint8 slot)
{
    // The page's drop-down (.ro slot): the identity in use is saved with its carried set put away, and the slot's is loaded in its place, its carried
    // set on the character if the page showed the bags.  A slot never picked gets its identity ID (and row) now; with no identity yet the page offers
    // to make one.  World thread, with the maps idle
    ROWOWIdentity* identity = GetIdentity(player);
    if (IsEnabled == false || slot < 1 || slot > ROWOW_IDENTITY_SLOT_COUNT)
        return false;
    if (slot == identity->Slot)
    {
        SendRagnarokPageToClient(player);
        return true;
    }
    if (char const* blockedText = GetIdentitySlotBlockedText(player, *identity))
    {
        ChatHandler(player->GetSession()).SendNotification(blockedText);
        SendRagnarokPageToClient(player); // The drop-down back on the slot in use
        return false;
    }

    // The form's carried set put away and the character's own back on it.  One from before the sets were apart is set apart first (as the page's bags
    // do), so the next identity's set never meets this one's things
    bool wasPageShown = identity->IsPageShown;
    if (identity->Exists == true && identity->CarriedSet == ROWOW_CARRIED_SET_NONE)
    {
        identity->IsViewingROSet = true;
        UpdateCarriedSet(player, *identity);
    }
    identity->IsViewingROSet = false;
    UpdateCarriedSet(player, *identity);

    // The slot list's line of the identity put away, and the slot picked: one never picked gets its identity's ID
    ObjectGuid::LowType guid = player->GetGUID().GetCounter();
    uint8 oldSlot = identity->Slot;
    ROWOWIdentitySlot& oldIdentitySlot = identity->Slots[oldSlot - 1];
    oldIdentitySlot.Exists = identity->Exists;
    oldIdentitySlot.JobID = identity->JobID;
    oldIdentitySlot.JobLevel = identity->JobLevel;
    oldIdentitySlot.BaseLevel = identity->Exists == true ? identity->BaseLevel : 0;
    ROWOWIdentitySlot& newIdentitySlot = identity->Slots[slot - 1];
    if (newIdentitySlot.IdentityID == 0)
        newIdentitySlot.IdentityID = slot == 1 ? guid : ++LastIdentityID;
    // A character that never had an identity: the zeny its first one starts with is the slots' from now
    if (identity->IsZenyShared == false)
    {
        identity->Zeny = StartZeny;
        identity->IsZenyShared = true;
    }

    // One transaction saves the character as it is now (this identity's rows, its put away set's and the bags'), the slots and the slot picked.  It's
    // queued (a save can't be committed directly), so the next identity is read from its own rows, which this doesn't write, and gets the account's
    // variables and the zeny from this one
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    player->SaveToDB(trans, false, false);
    for (uint8 slotIndex = 0; slotIndex < ROWOW_IDENTITY_SLOT_COUNT; slotIndex++)
    {
        ROWOWIdentitySlot& identitySlot = identity->Slots[slotIndex];
        if (identitySlot.IdentityID == 0 || identitySlot.IsStored == true)
            continue;
        std::string name = identitySlot.Name;
        CharacterDatabase.EscapeString(name);
        trans->Append("REPLACE INTO `mod_rowow_identity_slot` (`guid`, `slot`, `identity`, `name`) VALUES ({}, {}, {}, '{}')", guid, slotIndex + 1, identitySlot.IdentityID, name);
        identitySlot.IsStored = true;
    }
    // The head goes with the character once a slot has had an identity (this one, saved above, when no row had it yet)
    bool isHairShared = identity->IsHairShared == true || (identity->Exists == true && identity->HairStyle != 0);
    uint8 hairStyle = identity->HairStyle;
    uint8 hairColor = identity->HairColor;
    trans->Append("REPLACE INTO `mod_rowow_identity_character` (`guid`, `slot`, `zeny`, `hair_style`, `hair_color`, `sprite_job`, `peco_mount_look`) VALUES ({}, {}, {}, {}, {}, {}, {})",
        guid, slot, identity->Zeny, isHairShared == true ? hairStyle : 0, isHairShared == true ? hairColor : 0, identity->SpriteJobID, identity->IsPecoMountLookEnabled == true ? 1 : 0);
    CharacterDatabase.CommitTransaction(trans);

    // The slot's identity in this one's place
    std::array<ROWOWIdentitySlot, ROWOW_IDENTITY_SLOT_COUNT> slots = identity->Slots;
    ROWOWScriptVariables accountVariables = identity->AccountVariables;
    uint32 zeny = identity->Zeny;
    uint32 mapKey = identity->MapKey;
    std::set<uint16> jobHistoryJobIDs = identity->JobHistoryJobIDs;
    int32 spriteJobID = identity->SpriteJobID;
    bool isPecoMountLookEnabled = identity->IsPecoMountLookEnabled;
    player->CustomData.Erase("ROWOWIdentity");
    identity = player->CustomData.GetDefault<ROWOWIdentity>("ROWOWIdentity");
    identity->IsLoaded = true;
    identity->CharacterGUID = guid;
    identity->Slots = slots;
    identity->Slot = slot;
    identity->IdentityID = slots[slot - 1].IdentityID;
    identity->Zeny = zeny;
    identity->IsZenyShared = true;
    identity->MapKey = mapKey;
    // The look the slots share: the head (the slot's own rows keep only a copy), the job history and the job sprite picked
    if (isHairShared == true)
    {
        identity->HairStyle = hairStyle;
        identity->HairColor = hairColor;
        identity->IsHairShared = true;
    }
    identity->JobHistoryJobIDs = jobHistoryJobIDs;
    identity->SpriteJobID = spriteJobID;
    identity->IsPecoMountLookEnabled = isPecoMountLookEnabled;
    LoadIdentityRows(player, *identity);
    identity->AccountVariables = accountVariables; // Its rows may not have the save above yet
    LOG_INFO("module.ROWOW", "{} changed Ragnarok Online save slot {} for {} ({}, identity {})", player->GetName(), oldSlot, slot,
        identity->Exists == true ? "an identity" : "none yet", identity->IdentityID);

    // On the open page the slot's bags are shown, whatever the last slot had (an empty one has no bags to have been viewing; Nathan 2026-11-09)
    identity->IsPageShown = wasPageShown;
    if (wasPageShown == true)
        StartViewingROSet(player);
    else
        SendRagnarokPageToClient(player);
    SendROStatusToClient(player);
    SendHairToClient(player);
    return true;
}

bool ROWOWMod::MakeIdentitySlotName(std::string const& name, std::string& nameOut, std::string& errorOut)
{
    // Trimmed, with each run of spaces inside made one: 1 to ROWOW_IDENTITY_SLOT_NAME_MAX_LENGTH letters, digits, spaces and ' - _ . ! ?  (no link or
    // escape (|), no tab, which the slot list is split by, nor the parentheses the drop-down puts the job in)
    nameOut.clear();
    for (char character : name)
    {
        if (character == ' ')
        {
            if (nameOut.empty() == false && nameOut.back() != ' ')
                nameOut += ' ';
            continue;
        }
        bool isAllowed = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9') ||
            character == '\'' || character == '-' || character == '_' || character == '.' || character == '!' || character == '?';
        if (isAllowed == false)
        {
            errorOut = "A save slot's name can only have letters, numbers, spaces and ' - _ . ! ?";
            return false;
        }
        nameOut += character;
    }
    while (nameOut.empty() == false && nameOut.back() == ' ')
        nameOut.pop_back();
    if (nameOut.empty() == true)
    {
        errorOut = "A save slot's name can't be empty.";
        return false;
    }
    if (nameOut.size() > ROWOW_IDENTITY_SLOT_NAME_MAX_LENGTH)
    {
        errorOut = fmt::format("A save slot's name can have at most {} characters.", ROWOW_IDENTITY_SLOT_NAME_MAX_LENGTH);
        return false;
    }
    return true;
}

bool ROWOWMod::RenameIdentitySlot(Player* player, std::string const& name, std::string& errorOut)
{
    // The page's rename button (.ro slotname): the name of the slot in use, never the character's.  The slot list goes back either way
    ROWOWIdentity* identity = GetIdentity(player);
    std::string slotName;
    if (IsEnabled == false)
        return false;
    if (MakeIdentitySlotName(name, slotName, errorOut) == false)
    {
        SendIdentitySlotsToClient(player);
        return false;
    }
    ROWOWIdentitySlot& identitySlot = identity->Slots[identity->Slot - 1];
    identitySlot.Name = slotName;
    identitySlot.IsStored = true;
    std::string escapedName = slotName;
    CharacterDatabase.EscapeString(escapedName);
    CharacterDatabase.Execute("REPLACE INTO `mod_rowow_identity_slot` (`guid`, `slot`, `identity`, `name`) VALUES ({}, {}, {}, '{}')", player->GetGUID().GetCounter(), identity->Slot,
        identitySlot.IdentityID, escapedName);
    SendIdentitySlotsToClient(player);
    return true;
}

void ROWOWMod::StartWorldTravel(Player* player)
{
    // The Ragnarok page's Enter button (.ro enter): the world travel spell's cast, as the spell book's was.  The character's own bags are back on it first
    ROWOWIdentity* identity = GetIdentity(player);
    if (IsEnabled == false || IsROMap(player->GetMapId()) == true)
        return;
    if (identity->Exists == false)
    {
        ChatHandler(player->GetSession()).SendSysMessage("Make your Ragnarok Online character first.");
        return;
    }
    StopViewingROSet(player);
    identity->WorldTravelRequestTick = GetTick();
    player->CastSpell(player, ROWOW_SPELL_WORLD_TRAVEL, false);
}

bool ROWOWMod::IsWorldTravelRequested(Player* player) const
{
    ROWOWIdentity const* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    return identity != nullptr && identity->WorldTravelRequestTick != 0 && GetTick() - identity->WorldTravelRequestTick < ROWOW_WORLD_TRAVEL_REQUEST_TIME_IN_MS;
}

void ROWOWMod::SendWorldTravelSpellsToClient(Player* player) const
{
    // The RO world's spell book has World Return and Order, and the book out of it neither (the way in is the Ragnarok page).  The travel spell itself is
    // never shown.  After the spells the client was sent on arriving (it was sent all it knows), so quietly
    bool isInForm = IsInROForm(player);
    SendQuietSpellChange(player, ROWOW_SPELL_WORLD_TRAVEL, false);
    for (uint32 spellID : { uint32(ROWOW_SPELL_WORLD_RETURN), uint32(ROWOW_SPELL_FOLLOWER_ORDER) })
        if (player->HasSpell(spellID) == true)
            SendQuietSpellChange(player, spellID, isInForm);
}

void ROWOWMod::LoadIdentityStowedItems(Player* player, ROWOWIdentity& identity) const
{
    // The put away carried set's items, loaded as Player::_LoadInventory loads the bags' (its columns, for Item::LoadFromDB), but kept out of the
    // player's slots.  One that can't be loaded (no such item template now) or that is in the player's slots already is left out, its item_instance
    // row as it was; the next save of the set drops its row from the set
    QueryResult result = CharacterDatabase.Query("SELECT `instance`.`creatorGuid`, `instance`.`giftCreatorGuid`, `instance`.`count`, `instance`.`duration`, `instance`.`charges`, "
        "`instance`.`flags`, `instance`.`enchantments`, `instance`.`randomPropertyId`, `instance`.`durability`, `instance`.`playedTime`, `instance`.`text`, `stowed`.`bag`, "
        "`stowed`.`slot`, `stowed`.`item_guid`, `instance`.`itemEntry` FROM `mod_rowow_identity_stowed_item` `stowed` JOIN `item_instance` `instance` ON `instance`.`guid` = "
        "`stowed`.`item_guid` WHERE `stowed`.`guid` = {}", identity.IdentityID);
    if (!result)
        return;
    do
    {
        Field* fields = result->Fetch();
        ObjectGuid::LowType itemGUID = fields[13].Get<uint32>();
        uint32 itemTemplateID = fields[14].Get<uint32>();
        ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(itemTemplateID);
        if (itemTemplate == nullptr || player->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(itemGUID)) != nullptr)
        {
            LOG_ERROR("module.ROWOW", "ROWOWMod::LoadIdentityStowedItems: {}'s put away item {} (entry {}) {}, left out", player->GetName(), itemGUID, itemTemplateID,
                itemTemplate == nullptr ? "has no item template" : "is in the bags too");
            continue;
        }
        Item* item = NewItemOrBag(itemTemplate);
        if (item->LoadFromDB(itemGUID, player->GetGUID(), fields, itemTemplateID) == false)
        {
            LOG_ERROR("module.ROWOW", "ROWOWMod::LoadIdentityStowedItems: {}'s put away item {} (entry {}) didn't load, left out", player->GetName(), itemGUID, itemTemplateID);
            delete item;
            continue;
        }
        item->FSetState(ITEM_UNCHANGED);
        ROWOWStowedItem stowedItem;
        stowedItem.StowedItem = item;
        stowedItem.Bag = fields[11].Get<uint8>();
        stowedItem.Slot = fields[12].Get<uint8>();
        identity.StowedItems.push_back(stowedItem);
    } while (result->NextRow());
}

void ROWOWMod::SaveIdentityStowedItems(Player* /*player*/, ROWOWIdentity& identity, CharacterDatabaseTransaction trans) const
{
    // In the inventory save's transaction: the put away items leave the bags' rows (and the account bank's) as they come into the set's, with their
    // item_instance rows as they are now, and the items that came back have their bags' rows from the same save (Player::_SaveInventory)
    if (identity.AreStowedItemsChanged == false)
        return;
    uint32 guid = identity.IdentityID;
    trans->Append("DELETE FROM `mod_rowow_identity_stowed_item` WHERE `guid` = {}", guid);
    for (ROWOWStowedItem const& stowedItem : identity.StowedItems)
    {
        Item* item = stowedItem.StowedItem;
        Item::DeleteFromInventoryDB(trans, item->GetGUID().GetCounter());
        item->SaveToDB(trans);
        trans->Append("INSERT INTO `mod_rowow_identity_stowed_item` (`guid`, `item_guid`, `bag`, `slot`) VALUES ({}, {}, {}, {})", guid, item->GetGUID().GetCounter(),
            stowedItem.Bag, stowedItem.Slot);
    }
    identity.AreStowedItemsChanged = false;
}

void ROWOWMod::RestoreWoWShapeshift(Player* player, ROWOWIdentity& identity)
{
    // Leaving the form: the stance or form the character came in with, which the arrival took off with its buffs.  When that isn't known (it
    // logged in in the form), a Warrior goes back to Battle Stance, and the others to none.  Cast as the passive auras are, at no cost
    uint32 spellID = identity.WoWShapeshiftSpellID;
    identity.WoWShapeshiftSpellID = 0;
    if (spellID == 0 && player->getClass() == CLASS_WARRIOR)
        spellID = ROWOW_SPELL_WARRIOR_BATTLE_STANCE;
    if (spellID == 0 || player->IsAlive() == false || player->HasSpell(spellID) == false || player->GetShapeshiftForm() != FORM_NONE)
        return;
    player->CastSpell(player, spellID, true);
}

void ROWOWMod::RefreshROFormValues(Player* player, ROWOWIdentity& identity)
{
    // The level, the experience bar showing the base level's experience, the status, the HP and SP maximums from it (see the max
    // health and power hooks), and the time between attacks (adelay, from the ASPD)
    player->SetLevel(uint8(std::min<uint32>(identity.BaseLevel, 255)), true);
    player->SetUInt32Value(PLAYER_XP, identity.BaseExp);
    player->SetUInt32Value(PLAYER_NEXT_LEVEL_XP, uint32(std::min<uint64>(PCNextBaseExp(identity), std::numeric_limits<uint32>::max())));
    CalculateIdentityStatus(player, identity);
    PCRefreshItemSkills(player, identity);
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
    // The look's job (the job sprite picked, else the slot's own job: GetIdentityLookJob), in the character's gender: riding its Peco, the job's mounted
    // body (R9.5; the converter's job key with "_riding")
    // ... while the character's identity for the world is the Ragnarok one: with another (Midgard Wayfarer) the form keeps everything but the body
    // shown.  RefreshWorldIdentityLook does all of it, as a client hidden aura (no buff icon, nothing to click off)
    RefreshWorldIdentityLook(player);
}

void ROWOWMod::RemoveROFormAuras(Player* player)
{
    // What the form alone shows comes off.  The job sprite an identity put on (a client hidden aura), its head and its headgear are left to
    // RefreshWorldIdentityLook, which CompleteROFormChange calls after this: a character that shows its Ragnarok identity in the world it has
    // arrived in already wears them (they were made right before it arrived), and taking them off here only to put them back made its model
    // load again; any other identity's refresh takes them off
    std::vector<uint32> illusionSpellIDs;
    for (auto const& illusionBySpellID : IllusionsBySpellID)
        if (player->HasAura(illusionBySpellID.first) == true && IsWorldIdentityLookAura(player, illusionBySpellID.first) == false)
            illusionSpellIDs.push_back(illusionBySpellID.first);
    for (uint32 spellID : illusionSpellIDs)
        player->RemoveAurasDueToSpell(spellID);
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

    // The form's values to keep, and its statuses (leaving the RO world is RO's logout, also to its party, and its pet stays behind).  The arrival
    // invincibility is the form's
    LeaveStatusChanges(player, *identity);
    identity->InvincibleEndTime = 0;
    PetLeaveMap(player);
    FollowerLeaveMap(player, true);
    PartyRemoveMember(player);
    ForgetNpcTouches(player); // The OnTouch_ NPC it stands on is free for the next player
    identity->Zeny = player->GetMoney();
    if (player->IsAlive() == true)
    {
        identity->HP = player->GetHealth();
        identity->SP = player->GetPower(POWER_MANA);
    }
    player->InterruptNonMeleeSpells(false);
    identity->IsInForm = false;
    UpdateCarriedSet(player, *identity); // The character's own carried set back on it, the form's put away
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
    GetWorldIdentityState(player)->IsWorldwalkerOpen = false;

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

void ROWOWMod::OpenWorldwalker(Player* player)
{
    // Midgard Worldwalker earned in the form: what the arrival hid and suspended comes back, as leaving the form brings it back (LeaveROForm,
    // CompleteROFormChange), and from now the form is as one that arrived with the achievement
    ROWOWIdentity* identity = GetIdentity(player);
    ROWOWWorldIdentityState* state = GetWorldIdentityState(player);
    if (identity->IsInForm == false || state->IsWorldwalkerOpen == true)
        return;
    state->IsWorldwalkerOpen = true;
    for (uint32 spellID : identity->HiddenSpellIDs)
        SendQuietSpellChange(player, spellID, true);
    identity->HiddenSpellIDs.clear();
    uint32 formHealth = player->GetHealth();
    uint32 formSP = player->GetPower(POWER_MANA);
    for (uint32 spellID : identity->SuspendedPassiveSpellIDs)
    {
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellID);
        if (spellInfo != nullptr && (player->HasSpell(spellID) == true || player->HasTalent(spellID, player->GetActiveSpec()) == true) &&
            player->IsNeedCastPassiveSpellAtLearn(spellInfo) == true && player->HasAura(spellID) == false)
            player->CastSpell(player, spellID, true);
    }
    identity->SuspendedPassiveSpellIDs.clear();
    // The passives coming back change maximums the form overrides: the form's HP and SP stay what they were
    if (player->IsAlive() == true)
    {
        player->SetHealth(std::min(formHealth, player->GetMaxHealth()));
        player->SetPower(POWER_MANA, std::min(formSP, player->GetMaxPower(POWER_MANA)));
    }
    ChatHandler(player->GetSession()).SendSysMessage("Midgard Worldwalker: your own abilities are open to you in Midgard.");
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

    // Into it: where the identity last left it (its save point from a nosave map, as pc_makesavestatus), or the start point for a new one
    if (identity->Exists == true && identity->LastPosition.IsSet == true)
    {
        if (HasMapFlag(GetPositionMapKey(identity->LastPosition), "nosave") == false)
        {
            destinationOut = identity->LastPosition;
            return true;
        }
        if (identity->SavePoint.IsSet == true)
        {
            destinationOut = identity->SavePoint;
            return true;
        }
    }
    return GetCellPosition(StartMapID, StartCellX, StartCellY, destinationOut);
}

void ROWOWMod::RespawnAtSavePoint(Player* player)
{
    // rAthena's pc_respawn: standing again at the save point with the restart HP and SP
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
    PCSetRestartValue(player);
    player->TeleportTo(savePoint.MapID, savePoint.X, savePoint.Y, savePoint.Z, savePoint.O);
}

void ROWOWMod::PCSetRestartValue(Player* player)
{
    // pc_setrestartvalue's normal resurrection (type 1), from the respawn values status_calc_pc_sub works out.  With restart_hp_rate below
    // 50 a Novice comes back with half its HP and everyone else with 1, healed on top of the 1 HP a resurrection starts from, and SP stays as
    // it was unless that's below the restart SP
    ROWOWIdentity* identity = GetIdentity(player);
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
        // The skills it has for now (a spirit link's, its equipment's), each level up to that one
        for (std::map<uint16, uint16> const* tempSkills : { &identity.TempSkillLevelsByID, &identity.ItemSkillLevelsByID })
            for (auto const& levelBySkillID : *tempSkills)
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

void ROWOWMod::SendFormActionButtons(Player* player, ROWOWIdentity const& identity) const
{
    // With the character in the world, the client sets every button from these (a used one, or none) instead of only the used ones
    if (identity.IsInForm == true)
        SendIdentityActionButtons(player, identity);
    else
        player->SendActionButtons(1);
}

void ROWOWMod::StowCharacterPet(Player* player, ROWOWIdentity& identity) const
{
    // A pet the core or a module summoned again in the form (arriving, logging in) goes away again, its number kept by the identity rather than
    // the character, so nothing summons it until the form comes off.  Its row stays the current pet's, so a logout in the form keeps it
    if (player->GetPet() != nullptr)
        player->UnsummonPetTemporaryIfAny();
    if (player->GetTemporaryUnsummonedPetNumber() != 0)
    {
        identity.StowedPetNumber = player->GetTemporaryUnsummonedPetNumber();
        player->SetTemporaryUnsummonedPetNumber(0);
    }
}

void ROWOWMod::RestoreCharacterPet(Player* player, ROWOWIdentity& identity) const
{
    // The pet put away for the form is the character's temporarily unsummoned one again, for the core to summon back out of the RO world
    if (identity.StowedPetNumber != 0 && player->GetTemporaryUnsummonedPetNumber() == 0)
        player->SetTemporaryUnsummonedPetNumber(identity.StowedPetNumber);
    identity.StowedPetNumber = 0;
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
