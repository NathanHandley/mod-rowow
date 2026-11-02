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
#include "GameObject.h"
#include "Item.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"

#include "ROWOW.h"
#include "ROWOW_Skill.h"

#include <fstream>

// RO's items on the ground and monster drops, under rAthena's names: map_addflooritem and the floor rules (map.cpp), mob_dead's drops, mob_getdroprate and
// mob_drop_ratio_adjust (mob.cpp), pc_takeitem (pc.cpp).  A floor item is a temporary gameobject, whose template gives its name and sprite, and its RO state is
// a ROWOWFloorItem in the map's ROWOWMapFloorItems.
//
// Threads: the floor items of a map are that map's CustomData.  A hit that kills, a monster's think, the click (CMSG_GAMEOBJ_USE is PROCESS_INPLACE, so it
// runs in the player's map update), the skill units and the map update hook all run in that map's thread.  The GM commands run in a session's update or
// on the world thread with the maps idle.  So nothing here locks.  What a grid unload or a stop leaves is handled by the gameobject leaving the world
// (OnGameObjectRemoveWorld), in the map's own update too or with the maps stopped.  The tables are written at startup and only read after that

namespace
{
    // enum directions: x and y of the cell that way.  DIR_MAX, past the last, is any cell around
    constexpr int8 DIR_CENTER = ROWOW_RO_DIRECTION_CENTER;
    constexpr int8 DIR_NORTH = 0;
    constexpr int8 DIR_WEST = ROWOW_RO_DIRECTION_WEST;
    constexpr int8 DIR_SOUTHEAST = 5;
    constexpr int8 DIR_MAX = ROWOW_RO_DIRECTION_COUNT;
    int32 constexpr DirectionX[DIR_MAX] = { 0, -1, -1, -1, 0, 1, 1, 1 };
    int32 constexpr DirectionY[DIR_MAX] = { 1, 1, 0, -1, -1, -1, 0, 1 };

    constexpr int32 PICKUP_RANGE = 2; // pc_takeitem's distance, in cells
    constexpr int32 SUBCELL_STEPS = 16; // subx and suby are in 16ths of a cell
    constexpr char const* GET_ITEM_FAILED_TEXT = "You cannot get the item."; // msgstringtable's line for ZC_ITEM_PICKUP_ACK's failures
    constexpr uint32 MaxBagSlotCount = (INVENTORY_SLOT_ITEM_END - INVENTORY_SLOT_ITEM_START) + (INVENTORY_SLOT_BAG_END - INVENTORY_SLOT_BAG_START) * MAX_BAG_SIZE;

    // check_distance, with no CIRCULAR_AREA
    bool CheckDistance(int32 dx, int32 dy, int32 distance)
    {
        return std::max(std::abs(dx), std::abs(dy)) <= distance;
    }
}

void ROWOWMod::LoadFloorItemData()
{
    GroundItemsByItemTemplateID.clear();
    GroundItemEntryLow = 0;
    GroundItemEntryHigh = 0;
    MobDropsByMobID.clear();
    MobMvpDropsByMobID.clear();
    MobCompanionDropsByMobID.clear();
    if (IsEnabled == false)
        return;

    // The looks of the items on the ground
    QueryResult tableResult = WorldDatabase.Query("SELECT 1 FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = 'mod_rowow_ground_item'");
    if (tableResult)
    {
        if (QueryResult result = WorldDatabase.Query("SELECT `ItemTemplateID`, `GameObjectEntry`, `UnidentifiedGameObjectEntry` FROM `mod_rowow_ground_item`"))
        {
            do
            {
                Field* fields = result->Fetch();
                ROWOWGroundItem& groundItem = GroundItemsByItemTemplateID[fields[0].Get<uint32>()];
                groundItem.GameObjectEntry = fields[1].Get<uint32>();
                groundItem.UnidentifiedGameObjectEntry = fields[2].Get<uint32>();
                for (uint32 entry : { groundItem.GameObjectEntry, groundItem.UnidentifiedGameObjectEntry })
                {
                    if (entry == 0)
                        continue;
                    GroundItemEntryLow = GroundItemEntryLow == 0 ? entry : std::min(GroundItemEntryLow, entry);
                    GroundItemEntryHigh = std::max(GroundItemEntryHigh, entry);
                }
            } while (result->NextRow());
        }
    }
    else
        LOG_ERROR("module.ROWOW", "ROWOWMod::LoadFloorItemData found no mod_rowow_ground_item table, so nothing can lie on the ground (run the converter)");
    if (QueryResult companionTableResult = WorldDatabase.Query("SELECT 1 FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = 'mod_rowow_companion_drop'"))
    {
        if (QueryResult result = WorldDatabase.Query("SELECT `MobID`, `ItemTemplateID`, `Rate` FROM `mod_rowow_companion_drop`"))
        {
            do
            {
                Field* fields = result->Fetch();
                ROWOWMobDrop drop;
                drop.ItemTemplateID = fields[1].Get<uint32>();
                drop.Rate = int32(fields[2].Get<uint32>());
                MobCompanionDropsByMobID[fields[0].Get<uint32>()].push_back(drop);
            } while (result->NextRow());
        }
    }

    // mob_drop_ratio_adjust: each monster's drops as mob_db lists them, the rates adjusted by drops.conf.  mob_item_ratio.yml is empty in pre-renewal, and no
    // drop of mob_db names a RandomOptionGroup
    ROWOWDataTable const* mobTable = GetDataTable(ROWOW_DATA_TABLE_MOB);
    if (mobTable == nullptr)
        return;
    int32 idColumnIndex = mobTable->GetColumnIndex("Id");
    int32 dropsColumnIndex = mobTable->GetColumnIndex("Drops");
    int32 mvpDropsColumnIndex = mobTable->GetColumnIndex("MvpDrops");
    int32 raceGroupsColumnIndex = mobTable->GetColumnIndex("RaceGroups");
    uint32 unknownItemCount = 0;
    auto parseDrops = [&unknownItemCount, this](std::string const& cell, std::vector<ROWOWMobDrop>& dropsOut)
    {
        for (auto const& dropFields : ROWOWDataTable::ParseMapList(cell))
        {
            ROWOWMobDrop drop;
            for (auto const& field : dropFields)
            {
                if (field.first == "Item")
                {
                    std::string aegisName = field.second;
                    std::transform(aegisName.begin(), aegisName.end(), aegisName.begin(), ::tolower);
                    auto itemIDIter = ItemIDsByAegisName.find(aegisName);
                    drop.ItemID = itemIDIter != ItemIDsByAegisName.end() ? itemIDIter->second : 0;
                }
                else if (field.first == "Rate")
                    drop.Rate = int32(strtol(field.second.c_str(), nullptr, 10));
            }
            auto itemTemplateIDIter = ItemTemplateIDsByItemID.find(drop.ItemID);
            if (itemTemplateIDIter == ItemTemplateIDsByItemID.end())
            {
                unknownItemCount++;
                continue;
            }
            drop.ItemTemplateID = itemTemplateIDIter->second;
            if (drop.Rate != 0)
                dropsOut.push_back(drop);
        }
    };
    uint32 dropCount = 0;
    for (size_t rowIndex = 0; rowIndex < mobTable->Rows.size(); rowIndex++)
    {
        uint32 mobID = uint32(mobTable->GetInt(rowIndex, idColumnIndex, 0));
        auto statusIter = MobStatusesByMobID.find(mobID);
        if (statusIter == MobStatusesByMobID.end())
            continue;
        ROWOWStatus const& status = statusIter->second;
        bool isMvp = (status.Mode & ROWOW_MODE_MVP) != 0;
        bool isBoss = isMvp == false && status.Class == ROWOW_CLASS_BOSS;
        bool isTreasure = mobTable->GetCell(rowIndex, raceGroupsColumnIndex).find("Treasure") != std::string::npos;

        std::vector<ROWOWMobDrop> mvpDrops;
        parseDrops(mobTable->GetCell(rowIndex, mvpDropsColumnIndex), mvpDrops);
        for (ROWOWMobDrop& drop : mvpDrops)
        {
            if (BattleConfig.ItemRateMvp == 0)
                continue; // remove the item if the rate of item_dropratio_adjust is 0
            drop.Rate = MobDropAdjust(drop.Rate, BattleConfig.ItemRateMvp, BattleConfig.ItemDropMvpMin, BattleConfig.ItemDropMvpMax);
            MobMvpDropsByMobID[mobID].push_back(drop);
        }

        std::vector<ROWOWMobDrop> drops;
        parseDrops(mobTable->GetCell(rowIndex, dropsColumnIndex), drops);
        for (ROWOWMobDrop& drop : drops)
        {
            int32 rate = drop.Rate;
            if (BattleConfig.DropRateIncrease != 0 && rate < 5000)
                rate++;

            // Treasure box drop rates, and by the item's type
            int32 rateAdjust = 0;
            int32 rateMin = 0;
            int32 rateMax = 0;
            ROWOWItem const* roItem = GetROItemByItemTemplateID(drop.ItemTemplateID);
            uint8 itemType = roItem != nullptr ? roItem->Type : uint8(ROWOW_ITEM_TYPE_ETC);
            ROWOWDropRateConfig const* rateConfig = &BattleConfig.ItemDropCommon;
            if (isTreasure == true)
                rateConfig = &BattleConfig.ItemDropTreasure;
            else if (itemType == ROWOW_ITEM_TYPE_HEALING)
                rateConfig = &BattleConfig.ItemDropHeal;
            else if (itemType == ROWOW_ITEM_TYPE_USABLE || itemType == ROWOW_ITEM_TYPE_CASH)
                rateConfig = &BattleConfig.ItemDropUse;
            else if (itemType == ROWOW_ITEM_TYPE_WEAPON || itemType == ROWOW_ITEM_TYPE_ARMOR || itemType == ROWOW_ITEM_TYPE_PETARMOR)
                rateConfig = &BattleConfig.ItemDropEquip;
            else if (itemType == ROWOW_ITEM_TYPE_CARD)
                rateConfig = &BattleConfig.ItemDropCard;
            rateAdjust = isTreasure == true ? rateConfig->Rate : (isMvp == true ? rateConfig->RateMvp : (isBoss == true ? rateConfig->RateBoss : rateConfig->Rate));
            rateMin = rateConfig->Min;
            rateMax = rateConfig->Max;
            if (rateAdjust == 0)
                continue; // remove the item if the rate of item_dropratio_adjust is 0
            drop.Rate = MobDropAdjust(rate, rateAdjust, rateMin, rateMax);
            MobDropsByMobID[mobID].push_back(drop);
            dropCount++;
        }
    }
    LOG_INFO("module.ROWOW", "ROWOWMod::LoadFloorItemData loaded {} ground items, {} drops of {} monsters ({} of items that weren't converted), {} companion drops",
        GroundItemsByItemTemplateID.size(), dropCount, MobDropsByMobID.size(), unknownItemCount, MobCompanionDropsByMobID.size());
}

int32 ROWOWMod::MobDropAdjust(int32 baseRate, int32 rateAdjust, int32 rateMin, int32 rateMax) const
{
    // mob_drop_adjust: the logarithmic equation by Ishizu-Chan, or the classical linear rate adjustment
    double rate = baseRate;
    if (BattleConfig.LogarithmicDrops != 0 && rateAdjust > 0 && rateAdjust != 100 && baseRate > 0)
        rate = rate * pow(5.0 - log10(rate), log(rateAdjust / 100.) / log(5.0)) + 0.5;
    else
        rate = rate * rateAdjust / 100;
    return int32(std::clamp<double>(rate, rateMin, rateMax));
}

int32 ROWOWMod::MobGetDropRate(Unit const* src, int32 baseRate, int32 factor) const
{
    // mob_getdroprate.  mob_size_influence is off, the bonuses of a level difference are pk_mode's and renewal's, no pre-renewal item has
    // bDropAddRace or bDropAddClass, and there's no VIP
    int32 dropRate = baseRate;
    if (src != nullptr)
    {
        ROWOWStatus const* status = GetROStatus(src);
        int32 luk = status != nullptr ? int32(status->Stats[ROWOW_STAT_LUK]) : 0;
        if (BattleConfig.DropsByLuk != 0) // Drops affected by luk as a fixed increase [Valaris]
            dropRate += (luk * BattleConfig.DropsByLuk / 100) * factor;
        if (BattleConfig.DropsByLuk2 != 0) // Drops affected by luk as a % increase [Skotlex]
            dropRate += int32(0.5 + dropRate * luk * BattleConfig.DropsByLuk2 / 10000.) * factor;
        if (src->IsPlayer() == true) // Player specific drop rate adjustments
        {
            int32 dropRateBonus = 100;
            if (ROWOWStatusChangeEntry const* itemBoost = GetSCE(src, ROWOW_SC_ITEMBOOST))
                dropRateBonus += itemBoost->Val1;
            if (ROWOWStatusChangeEntry const* receiveItem = GetSCE(src, ROWOW_SC_PERIOD_RECEIVEITEM_2ND))
                dropRateBonus += receiveItem->Val1;
            int32 cap = BattleConfig.DropRateCap * factor;
            dropRate = int32(0.5 + dropRate * dropRateBonus / 100.);

            // Now limit the drop rate to never be exceed the cap (default: 90%), unless it is originally above it already.
            if (dropRate > cap && baseRate < cap)
                dropRate = cap;
        }
    }
    dropRate = std::min(dropRate, 10000 * factor); // Cap it to 100%

    // If the monster's drop rate can become 0, else cap to 0.01% or 0.001% drop rate - as on official servers
    return BattleConfig.DropRate0Item != 0 ? std::max(dropRate, 0) : std::max(dropRate, 1);
}

void ROWOWMod::PCGetAddDrops(Player* sd, std::vector<ROWOWAddDrop>& addDropsOut) const
{
    // sd->add_drop, as pc_bonus_item_drop keeps it: bAddMonsterDropItem (bonus2: every monster; bonus3: by race), bAddMonsterIdDropItem and
    // bAddMonsterDropItemGroup.  Same item and classification add up.  Only what's worn makes these: no arrow's bonus counts
    ROWOWStatus const* status = GetROStatus(sd);
    if (status == nullptr)
        return;
    for (ROWOWDeferredBonus const& bonus : status->Bonus.Deferred)
    {
        if (bonus.LRFlag == ROWOW_LR_FLAG_ARROW)
            continue;
        bool isItem = bonus.Type == "bAddMonsterDropItem";
        bool isGroup = bonus.Type == "bAddMonsterDropItemGroup";
        bool isID = bonus.Type == "bAddMonsterIdDropItem";
        if ((isItem == false && isGroup == false && isID == false) || bonus.Values.size() < 2)
            continue;
        ROWOWAddDrop newDrop;
        if (isGroup == true)
            newDrop.Group = ROWOWSCRIPT->GetSymbolName(bonus.Values[0]);
        else
            newDrop.ItemID = uint32(bonus.Values[0]);
        if (bonus.Values.size() >= 3)
        {
            newDrop.Class = ROWOW_ADD_DROP_CLASS_NONE;
            newDrop.Race = isID == true ? -int32(bonus.Values[1]) : int32(bonus.Values[1]);
        }
        int32 rate = int32(bonus.Values.back());

        // Apply config rate adjustment settings.
        if (rate >= 0) // Absolute drop.
        {
            rate = rate * BattleConfig.ItemRateAddDrop / 100;
            rate = std::clamp(rate, BattleConfig.ItemDropAddDropMin, BattleConfig.ItemDropAddDropMax);
        }
        else // Relative drop, max/min limits are applied at drop time.
            rate = std::min(rate * BattleConfig.ItemRateAddDrop / 100, -1);
        bool isAdded = false;
        for (ROWOWAddDrop& addDrop : addDropsOut)
        {
            if (addDrop.ItemID != newDrop.ItemID || addDrop.Group != newDrop.Group || addDrop.Race != newDrop.Race || addDrop.Class != newDrop.Class)
                continue;
            if ((rate < 0 && addDrop.Rate < 0) || (rate > 0 && addDrop.Rate > 0)) // Adjust the rate if it has same classification
                addDrop.Rate = std::min(addDrop.Rate + rate, 10000);
            isAdded = true;
            break;
        }
        if (isAdded == true)
            continue;
        newDrop.Rate = std::clamp(rate, -10000, 10000);
        addDropsOut.push_back(newDrop);
    }
}

bool ROWOWMod::PCGetZenyBonus(Player* sd, int32 mobLevel, int32& zenyOut) const
{
    // mob_dead's script-granted zeny bonus (get_zeny_num, from bGetZenyNum, the best rate's, and bAddGetZenyNum) [Skotlex]
    ROWOWStatus const* status = GetROStatus(sd);
    if (status == nullptr)
        return false;
    int32 zenyRate = 0;
    int32 zenyNum = 0;
    for (ROWOWDeferredBonus const& bonus : status->Bonus.Deferred)
    {
        if (bonus.LRFlag == ROWOW_LR_FLAG_ARROW || bonus.Values.size() < 2)
            continue;
        if (bonus.Type == "bGetZenyNum" && zenyRate < int32(bonus.Values[1]))
        {
            zenyRate = int32(bonus.Values[1]);
            zenyNum = int32(bonus.Values[0]);
        }
        else if (bonus.Type == "bAddGetZenyNum")
        {
            zenyRate += int32(bonus.Values[1]);
            zenyNum += int32(bonus.Values[0]);
        }
    }
    if (zenyNum == 0 || int32(urand(0, 99)) >= zenyRate)
        return false;
    int32 zeny = zenyNum > 0 ? zenyNum : -mobLevel * zenyNum;
    if (zeny == 0)
        zeny = 1;
    zenyOut = 1 + int32(urand(0, uint32(zeny - 1)));
    return true;
}

void ROWOWMod::MobRollDrops(uint32 mobID, int32 mobLevel, uint8 race, uint8 mobClass, Unit* src, Player* sd, Player* firstSD, std::vector<ROWOWDropItem>& itemsOut)
{
    // mob_dead's item drops, in its order: what the killer's script-granted bonuses add, the monster's own drops, Ore Discovery.  map_drops.yml has nothing in
    // pre-renewal.  A monster with item_db's pet egg among its drops isn't in the pre-renewal data, nor is any rare_drop_announce or @autoloot
    auto addItem = [this, mobID, &itemsOut](uint32 itemTemplateID, uint32 itemID)
    {
        ROWOWDropItem item;
        item.ItemTemplateID = itemTemplateID;
        item.IsIdentified = itemID == 0 || ItemDBIsIdentified(itemID); // mob_setdropitem
        item.MobID = mobID;
        itemsOut.push_back(item);
    };

    // These trigger for the killer of the monster
    if (sd != nullptr)
    {
        // process script-granted extra drop bonuses
        std::vector<ROWOWAddDrop> addDrops;
        PCGetAddDrops(sd, addDrops);
        for (ROWOWAddDrop const& addDrop : addDrops)
        {
            if (!((addDrop.Race < ROWOW_ADD_DROP_RACE_NONE && addDrop.Race == -int32(mobID)) || // Race < RC_NONE_, use mob_id
                addDrop.Race == ROWOW_RACE_ALL || addDrop.Race == int32(race) || // Matched race
                addDrop.Class == ROWOW_CLASS_ALL || addDrop.Class == int32(mobClass))) // Matched class
                continue;

            // Check if the bonus item drop rate should be multiplied with mob level/10 [Lupus]
            int32 dropRate = addDrop.Rate;
            if (dropRate < 0)
            {
                // It's negative, then it should be multiplied. with mob_level/10.  rate = base_rate * (mob_level/10) + 1
                dropRate = (-dropRate) * mobLevel / 10 + 1;
                dropRate = std::clamp(dropRate, std::max(BattleConfig.ItemDropAddDropMin, 1), std::min(BattleConfig.ItemDropAddDropMax, 10000));
            }
            if (int32(urand(0, 9999)) >= dropRate)
                continue;
            uint32 itemID = addDrop.ItemID;
            if (itemID == 0)
                itemID = ItemGroupRandomDropItem(addDrop.Group);
            auto itemTemplateIDIter = ItemTemplateIDsByItemID.find(itemID);
            if (itemID != 0 && itemTemplateIDIter != ItemTemplateIDsByItemID.end())
                addItem(itemTemplateIDIter->second, itemID);
        }

        // process script-granted zeny bonus (get_zeny_num) [Skotlex]
        int32 zeny = 0;
        if (PCGetZenyBonus(sd, mobLevel, zeny) == true)
            sd->ModifyMoney(zeny);
    }

    // Regular mob drops drop after script-granted drops: attempt to drop the item
    auto dropsIter = MobDropsByMobID.find(mobID);
    if (dropsIter != MobDropsByMobID.end())
    {
        for (ROWOWMobDrop const& entry : dropsIter->second)
        {
            int32 dropRate = MobGetDropRate(src, entry.Rate, 1);
            if (int32(urand(0, 9999)) >= dropRate)
                continue;
            addItem(entry.ItemTemplateID, entry.ItemID);
        }
    }

    // The companion carriers, which are the converter's: at their own rates, by no RO rule
    auto companionDropsIter = MobCompanionDropsByMobID.find(mobID);
    if (companionDropsIter != MobCompanionDropsByMobID.end())
        for (ROWOWMobDrop const& entry : companionDropsIter->second)
            if (int32(urand(0, 9999)) < entry.Rate)
                addItem(entry.ItemTemplateID, 0);

    // Ore Discovery (triggers if owner has loot priority, does not require to be the killer)
    if (firstSD != nullptr && PCCheckSkill(firstSD, ROWOW_SKILL_BS_FINDINGORE) > 0)
    {
        uint32 oreItemID = ItemGroupRandomDropItem("ORE");
        auto itemTemplateIDIter = ItemTemplateIDsByItemID.find(oreItemID);
        if (oreItemID != 0 && itemTemplateIDIter != ItemTemplateIDsByItemID.end())
            addItem(itemTemplateIDIter->second, oreItemID);
    }
}

void ROWOWMod::MobDeadLootPriority(Creature* md, Unit* src, ROWOWLootPriority& priority) const
{
    // mob_dead's loot priority: the players on its map and alive, by the damage they did, the first in the damage log with first_attack_loot_bonus percent of all
    // the damage on top of its own.  With no one logged the killer has it
    priority.FirstSD = src != nullptr ? src->ToPlayer() : nullptr;
    ROWOWDamageLog const* damageLog = md->CustomData.Get<ROWOWDamageLog>("ROWOWDamageLog");
    if (damageLog == nullptr)
        return;
    struct ROWOWLootDamage
    {
        Player* SD = nullptr;
        uint64 Damage = 0;
    };
    std::vector<ROWOWLootDamage> lootDamage;
    uint64 totalDamage = 0;
    for (ROWOWDamageLogEntry const& entry : damageLog->Entries)
    {
        totalDamage += entry.Damage;
        Player* sd = ObjectAccessor::GetPlayer(*md, entry.PlayerGUID);
        if (sd == nullptr || sd->IsAlive() == false || IsInROForm(sd) == false)
            continue;
        // A homunculus's damage is added to its master's (Slave damage is added to the player's damage)
        auto existingIter = std::find_if(lootDamage.begin(), lootDamage.end(), [sd](ROWOWLootDamage const& damage) { return damage.SD == sd; });
        if (existingIter != lootDamage.end())
        {
            existingIter->Damage += entry.Damage;
            continue;
        }
        ROWOWLootDamage damage;
        damage.SD = sd;
        damage.Damage = entry.Damage;
        lootDamage.push_back(damage);
    }
    if (lootDamage.empty() == true)
        return;
    lootDamage[0].Damage += totalDamage * uint64(BattleConfig.FirstAttackLootBonus) / 100;
    std::stable_sort(lootDamage.begin(), lootDamage.end(), [](ROWOWLootDamage const& left, ROWOWLootDamage const& right) { return left.Damage > right.Damage; });
    priority.FirstSD = lootDamage[0].SD;
    priority.SecondSD = lootDamage.size() > 1 ? lootDamage[1].SD : nullptr;
    priority.ThirdSD = lootDamage.size() > 2 ? lootDamage[2].SD : nullptr;
}

void ROWOWMod::MobDeadDrops(Creature* md, Unit* src, ROWOWLootPriority const& priority, bool isNoDrops)
{
    // mob_dead's drops: what it looted comes back first in the list and always, its own drops but for a monster that came back, a nomobloot map or (a player's
    // summon) as alchemist_summon_reward says.  A drop falls on the monster's cell, then around it (mob_process_drop_list)
    ROWOWMobData* mob = GetMobData(md);
    ROWOWMobSummon const* summon = md->CustomData.Get<ROWOWMobSummon>("ROWOWMobSummon");
    uint8 ai = summon != nullptr ? summon->AI : uint8(ROWOW_AI_NONE);
    std::vector<ROWOWDropItem> dropItems;
    bool isNoMobLoot = MapIDsByMapFlag.count("nomobloot") != 0 && MapIDsByMapFlag.at("nomobloot").count(md->GetMapId()) != 0;
    if (isNoDrops == false && isNoMobLoot == false && mob->IsRebirth == false &&
        (ai == ROWOW_AI_NONE || BattleConfig.AlchemistSummonReward == 2 || (ai == ROWOW_AI_SPHERE && BattleConfig.AlchemistSummonReward == 1)))
    {
        ROWOWStatus const* status = GetROStatus(md);
        Player* sd = src != nullptr ? src->ToPlayer() : nullptr;
        MobRollDrops(GetROMobID(md), status != nullptr ? int32(status->Level) : 1, status != nullptr ? status->Race : uint8(0), status != nullptr ? status->Class : uint8(0),
            src, sd, priority.FirstSD, dropItems);
    }
    int32 x = 0, y = 0;
    GetUnitCell(md, x, y);
    ObjectGuid first = priority.FirstSD != nullptr ? priority.FirstSD->GetGUID() : ObjectGuid::Empty;
    ObjectGuid second = priority.SecondSD != nullptr ? priority.SecondSD->GetGUID() : ObjectGuid::Empty;
    ObjectGuid third = priority.ThirdSD != nullptr ? priority.ThirdSD->GetGUID() : ObjectGuid::Empty;
    auto processDropList = [this, md, x, y, first, second, third](std::vector<ROWOWDropItem> const& items, int8 dir)
    {
        for (ROWOWDropItem const& item : items)
        {
            MapAddFloorItem(md->GetMap(), item, x, y, first, second, third, ROWOW_FLOORITEM_NO_DROP_CHECK, dir, true);

            // The drop location loops between three locations: SE -> W -> N -> SE
            if (dir <= DIR_NORTH)
                dir = DIR_SOUTHEAST;
            else if (dir == DIR_SOUTHEAST)
                dir = DIR_WEST;
            else
                dir = DIR_NORTH;
        }
    };
    processDropList(dropItems, DIR_CENTER); // First regular drop always drops at center
    processDropList(mob->LootItems, DIR_NORTH); // Looted drops start north instead
    mob->LootItems.clear();
}

void ROWOWMod::ClearFloorItemRow(Map* map, ObjectGuid const& guid)
{
    if (ROWOWMapFloorItems* floorItems = map->CustomData.Get<ROWOWMapFloorItems>("ROWOWMapFloorItems"))
        floorItems->ItemsByGUID.erase(guid);
}

ROWOWFloorItem const* ROWOWMod::MapFindFloorItem(Map* map, ObjectGuid const& guid) const
{
    ROWOWMapFloorItems const* floorItems = map->CustomData.Get<ROWOWMapFloorItems>("ROWOWMapFloorItems");
    if (floorItems == nullptr)
        return nullptr;
    auto itemIter = floorItems->ItemsByGUID.find(guid);
    return itemIter != floorItems->ItemsByGUID.end() ? &itemIter->second : nullptr;
}

bool ROWOWMod::MapCellFree(Map* map, ROWOWMapGrid const& mapGrid, int32 x, int32 y, bool isCountingUnits) const
{
    // map_cell_free: passable, not within 4-5 cells of the map's edge (officially not symmetric), and no RO combatant on it (BL_CHAR|BL_PET, if asked)
    int32 edgeValid = std::min(BattleConfig.MapEdgeSize, 5);
    if (x < edgeValid || x > mapGrid.Width - edgeValid || y < edgeValid || y > mapGrid.Height - edgeValid)
        return false;
    if (MapGetCellNoPass(map, mapGrid, x, y) == true)
        return false;
    if (isCountingUnits == false)
        return true;
    return MapForEachInCellArea(map, PHASEMASK_NORMAL, x, y, x, y, [](Unit* bl) { return bl->IsAlive() == true ? 1 : 0; }) == 0;
}

bool ROWOWMod::MapSearchFreeCellDist(Map* map, ROWOWMapGrid const& mapGrid, int32& x, int32& y, int32 distMin, int32 distMax, bool isCountingUnits) const
{
    // map_search_freecell_dist: the official algorithm, each quadrant and line once.  The mirrors prevent that always the same quadrant is checked first
    int32 mirrorX = urand(0, 1) != 0 ? -1 : 1;
    int32 mirrorY = urand(0, 1) != 0 ? -1 : 1;
    for (int32 i = -1; i <= 1; i++)
    {
        for (int32 j = -1; j <= 1; j++)
        {
            if (i == 0 && j == 0)
                continue;
            int32 checkX = x + mirrorX * i * irand(distMin, distMax);
            int32 checkY = y + mirrorY * j * irand(distMin, distMax);
            if (MapCellFree(map, mapGrid, checkX, checkY, isCountingUnits) == true)
            {
                x = checkX;
                y = checkY;
                return true;
            }
        }
    }
    return false;
}

ObjectGuid ROWOWMod::MapAddFloorItem(Map* map, ROWOWDropItem const& item, int32 x, int32 y, ObjectGuid const& firstCharID, ObjectGuid const& secondCharID,
    ObjectGuid const& thirdCharID, int32 flags, int8 dir, bool isCountingUnits)
{
    // map_addflooritem.  flags: &1 MVP item, &2 search free cell in 5x5 area instead of 3x3, &4 bypass droppable check.  dir: where the item should drop around
    // the target (DIR_MAX: a random cell around it, DIR_CENTER: the cell itself).  isCountingUnits is the block types it shouldn't stack on, BL_CHAR|BL_PET or none
    ROWOWMapGrid const* mapGrid = GetMapGrid(map->GetId());
    auto groundItemIter = GroundItemsByItemTemplateID.find(item.ItemTemplateID);
    if (mapGrid == nullptr || mapGrid->HasCells() == false || groundItemIter == GroundItemsByItemTemplateID.end())
        return ObjectGuid::Empty;
    if ((flags & ROWOW_FLOORITEM_NO_DROP_CHECK) == 0 && BattleConfig.ItemOnFloor != 0)
        if (ROWOWItem const* roItem = GetROItemByItemTemplateID(item.ItemTemplateID); roItem != nullptr && roItem->IsNoTrade == true)
            return ObjectGuid::Empty; // can't be dropped
    if (dir > DIR_CENTER && dir < DIR_MAX)
    {
        x += DirectionX[dir];
        y += DirectionY[dir];
    }

    // If cell occupied and not center cell, drop item around the drop target cell
    if (dir == DIR_MAX || (dir != DIR_CENTER && MapCellFree(map, *mapGrid, x, y, isCountingUnits) == false))
        MapSearchFreeCellDist(map, *mapGrid, x, y, 1, (flags & ROWOW_FLOORITEM_WIDE_SEARCH) != 0 ? 2 : 1, isCountingUnits);

    // subx and suby: where on the cell, in RO's own steps
    float worldX = mapGrid->OriginX + (float(y) + float(irand(1, 4) * 3) / float(SUBCELL_STEPS)) * mapGrid->CellSize;
    float worldY = mapGrid->OriginY - (float(x) + float(irand(1, 4) * 3) / float(SUBCELL_STEPS)) * mapGrid->CellSize;
    float worldZ = map->GetHeight(PHASEMASK_NORMAL, worldX, worldY, mapGrid->GetCellGroundZ(x, y) + ConfigMoveFloorSearchHeight, true, ConfigMoveFloorSearchDistance);
    if (worldZ <= INVALID_HEIGHT)
        worldZ = mapGrid->GetCellGroundZ(x, y);
    uint32 entry = item.IsIdentified == true || groundItemIter->second.UnidentifiedGameObjectEntry == 0 ? groundItemIter->second.GameObjectEntry : groundItemIter->second.UnidentifiedGameObjectEntry;
    GameObject* go = map->SummonGameObject(entry, worldX, worldY, worldZ, 0, 0, 0, 0, 0, 0, false);
    if (go == nullptr)
        return ObjectGuid::Empty;

    // If item is flagged as MVP item or dropped by bosses, it is protected for longer
    bool isExtendProtection = (flags & ROWOW_FLOORITEM_MVP) != 0;
    if (isExtendProtection == false && item.MobID != 0)
    {
        if (auto statusIter = MobStatusesByMobID.find(item.MobID); statusIter != MobStatusesByMobID.end())
            isExtendProtection = (statusIter->second.Mode & ROWOW_MODE_MVP) != 0 || statusIter->second.Class == ROWOW_CLASS_BOSS;
    }
    uint64 tick = GetTick();
    ROWOWMapFloorItems* floorItems = map->CustomData.GetDefault<ROWOWMapFloorItems>("ROWOWMapFloorItems");
    ROWOWFloorItem& fitem = floorItems->ItemsByGUID[go->GetGUID()];
    fitem.GUID = go->GetGUID();
    fitem.ID = ++floorItems->LastID;
    fitem.Item = item;
    fitem.X = x;
    fitem.Y = y;
    fitem.FirstGetCharID = firstCharID;
    fitem.SecondGetCharID = secondCharID;
    fitem.ThirdGetCharID = thirdCharID;
    fitem.FirstGetTick = tick + uint64(isExtendProtection == true ? BattleConfig.MvpItemFirstGetTime : BattleConfig.ItemFirstGetTime);
    fitem.SecondGetTick = fitem.FirstGetTick + uint64(isExtendProtection == true ? BattleConfig.MvpItemSecondGetTime : BattleConfig.ItemSecondGetTime);
    fitem.ThirdGetTick = fitem.SecondGetTick + uint64(isExtendProtection == true ? BattleConfig.MvpItemThirdGetTime : BattleConfig.ItemThirdGetTime);
    floorItems->ClearQueue.emplace_back(tick + uint64(BattleConfig.FlooritemLifetime), go->GetGUID());
    return go->GetGUID();
}

void ROWOWMod::MapClearFloorItem(Map* map, ObjectGuid const& guid)
{
    // map_clearflooritem: gone from the ground for all who see it.  The gameobject leaves the world with the map's remove list
    if (ROWOWMapFloorItems* floorItems = map->CustomData.Get<ROWOWMapFloorItems>("ROWOWMapFloorItems"))
        floorItems->ItemsByGUID.erase(guid);
    if (GameObject* go = map->GetGameObject(guid))
        go->Delete();
}

void ROWOWMod::MapFloorItemTimer(Map* map)
{
    // map_clearflooritem_timer, from the map's update: every item stays as long as the last, so those due are at the front of the queue
    ROWOWMapFloorItems* floorItems = map->CustomData.Get<ROWOWMapFloorItems>("ROWOWMapFloorItems");
    if (floorItems == nullptr)
        return;
    uint64 tick = GetTick();
    while (floorItems->ClearQueue.empty() == false && floorItems->ClearQueue.front().first <= tick)
    {
        ObjectGuid guid = floorItems->ClearQueue.front().second;
        floorItems->ClearQueue.pop_front();
        if (floorItems->ItemsByGUID.count(guid) != 0)
            MapClearFloorItem(map, guid);
    }
}

bool ROWOWMod::IsGroundItem(GameObject const* go) const
{
    return GroundItemEntryLow != 0 && go->GetEntry() >= GroundItemEntryLow && go->GetEntry() <= GroundItemEntryHigh;
}

void ROWOWMod::OnGroundItemRemoved(GameObject* go)
{
    // A grid unloading, or the map going, takes the gameobject without asking: its row goes with it
    if (Map* map = go->FindMap())
        ClearFloorItemRow(map, go->GetGUID());
}

bool ROWOWMod::PCAddItem(Player* player, uint32 itemTemplateID, uint32 amount, bool isIdentified, Item** itemOut)
{
    // pc_additem: all of it into the bags, or none of it
    ItemPosCountVec destination;
    if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, destination, itemTemplateID, amount) != EQUIP_ERR_OK)
        return false;
    Item* item = player->StoreNewItem(destination, itemTemplateID, true);
    if (item == nullptr)
        return false;
    if (itemOut != nullptr)
        *itemOut = item;
    if (isIdentified == false)
        SetItemState(player, item, 0, false, false);
    player->SendNewItem(item, amount, true, false);
    return true;
}

bool ROWOWMod::CanStoreROItems(Player* player, std::vector<ROWOWStoreItem> const& items) const
{
    // pc_checkadditem and pc_inventoryblank over a whole purchase: room in the bags for all of it at once, each stack-sized piece among the stacks of its
    // maker.  The core's trade check (CanStoreItems) counts what each piece takes from the next, on throwaway items that never enter a map.  No bags hold
    // more stacks than MaxBagSlotCount, however they're filled, which bounds the throwaways
    std::vector<Item*> pieces;
    bool canStore = true;
    for (ROWOWStoreItem const& storeItem : items)
    {
        ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(storeItem.ItemTemplateID);
        if (itemTemplate == nullptr || storeItem.Amount == 0)
        {
            canStore = false;
            break;
        }
        uint32 stackSize = itemTemplate->GetMaxStackSize();
        if ((storeItem.Amount + stackSize - 1) / stackSize + pieces.size() > MaxBagSlotCount)
        {
            canStore = false;
            break;
        }
        for (uint32 left = storeItem.Amount; left > 0;)
        {
            uint32 count = std::min(left, stackSize);
            Item* piece = Item::CreateItem(storeItem.ItemTemplateID, count, player);
            if (piece == nullptr)
            {
                canStore = false;
                break;
            }
            if (storeItem.CreatorGUID != 0)
                piece->SetGuidValue(ITEM_FIELD_CREATOR, ObjectGuid::Create<HighGuid::Player>(storeItem.CreatorGUID));
            pieces.push_back(piece);
            left -= count;
        }
    }
    if (canStore == true)
        canStore = player->CanStoreItems(pieces.data(), int32(pieces.size()), nullptr) == EQUIP_ERR_OK;
    for (Item* piece : pieces)
        delete piece;
    return canStore;
}

bool ROWOWMod::CanStoreROItem(Player* player, uint32 itemTemplateID, uint32 amount, ObjectGuid::LowType creatorGUID) const
{
    return CanStoreROItems(player, { ROWOWStoreItem{ itemTemplateID, amount, creatorGUID } });
}

Item* ROWOWMod::StoreROItem(Player* player, uint32 itemTemplateID, uint32 amount, std::array<uint32, ROWOW_ITEM_ENCHANTMENT_SLOT_COUNT> const* enchantments, ObjectGuid::LowType creatorGUID)
{
    // An item into the bags with what makes it that item (a cart's or a shop's, a made one): its enchantments and its maker, whose stacks are
    // its own (CanBeMergedPartlyWith).  A cart slot holds more than one stack, so it goes in stack by stack, all of it or none: null, with nothing
    // stored, when there's no room.  Otherwise the last stack it went into
    ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(itemTemplateID);
    if (itemTemplate == nullptr || CanStoreROItem(player, itemTemplateID, amount, creatorGUID) == false)
        return nullptr;
    Item* storedItem = nullptr;
    for (uint32 left = amount; left > 0;)
    {
        uint32 count = std::min(left, itemTemplate->GetMaxStackSize());
        left -= count;
        Item* item = Item::CreateItem(itemTemplateID, count, player);
        if (item == nullptr)
            return storedItem;
        if (creatorGUID != 0)
            item->SetGuidValue(ITEM_FIELD_CREATOR, ObjectGuid::Create<HighGuid::Player>(creatorGUID));
        ItemPosCountVec destination;
        if (player->CanStoreItem(NULL_BAG, NULL_SLOT, destination, item, false) != EQUIP_ERR_OK)
        {
            LOG_ERROR("module.ROWOW", "ROWOWMod::StoreROItem had room for {} x {} and then none for {} of them, for {}", amount, itemTemplateID, count, player->GetName());
            delete item;
            return storedItem;
        }
        item = player->StoreItem(destination, item, true);
        if (item == nullptr)
            return storedItem;
        if (enchantments != nullptr)
            for (uint8 enchantmentSlot = 0; enchantmentSlot < MAX_ENCHANTMENT_SLOT; enchantmentSlot++)
                if ((*enchantments)[enchantmentSlot] != 0)
                    item->SetEnchantment(EnchantmentSlot(enchantmentSlot), (*enchantments)[enchantmentSlot], 0, 0);
        item->SetState(ITEM_CHANGED, player);
        player->SendNewItem(item, count, true, false);
        storedItem = item;
    }
    return storedItem;
}

bool ROWOWMod::PCCantAct(Player* sd) const
{
    // pc_cant_act: a shop open, a room, or a trade going.  (An NPC's window doesn't hold a Warcraft character back)
    return PCCanMoveAt(sd) == false || sd->GetTradeData() != nullptr;
}

bool ROWOWMod::PCTakeItem(Player* sd, ObjectGuid const& guid, bool isGreed)
{
    // pc_takeitem: false when it can't be had (fail: clif_additem)
    Map* map = sd->GetMap();
    ROWOWFloorItem const* fitem = MapFindFloorItem(map, guid);
    if (fitem == nullptr)
        return false;
    int32 x = 0, y = 0;
    GetUnitCell(sd, x, y);
    if (CheckDistance(fitem->X - x, fitem->Y - y, PICKUP_RANGE) == false && isGreed == false)
        return false; // Distance is too far
    if (ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(sd)); sc != nullptr && sc->Cant.PickUp == true)
        return false;
    Group* group = sd->GetGroup();
    ROWOWPartyOption option = GetPartyOption(group);
    bool isShare = group != nullptr && (option.Item & 1) != 0;

    // Time the player needs to wait until the item can be taken.  By default the player needs to wait for top, second and third attacker loot priority
    // times.  This applies even if there are no second or third top attackers
    uint64 itemGetTick = fitem->ThirdGetTick;
    ObjectGuid charID = sd->GetGUID();
    if (fitem->FirstGetCharID.IsEmpty() == true || fitem->FirstGetCharID == charID)
        itemGetTick = 0; // Top attacker or no attacker, no wait time
    else if (fitem->SecondGetCharID.IsEmpty() == false && fitem->SecondGetCharID == charID && isShare == false)
        itemGetTick = fitem->FirstGetTick; // Second top attacker, needs to wait for top attacker
    else if (fitem->ThirdGetCharID.IsEmpty() == false && fitem->ThirdGetCharID == charID && isShare == false)
        itemGetTick = fitem->SecondGetTick; // Third top attacker, needs to wait for top and second attacker
    else if (isShare == true)
    {
        // Party member loot priority.  Only if the top attackers are still online, party members gain the loot priority; check if the top attackers are a
        // party member and apply corresponding wait time.  (Group::IsMember reads the group's own list; a connected player is looked up, not touched)
        auto isOnlineMember = [group](ObjectGuid const& guid) { return guid.IsEmpty() == false && group->IsMember(guid) == true && ObjectAccessor::FindConnectedPlayer(guid) != nullptr; };
        if (isOnlineMember(fitem->FirstGetCharID) == true)
            itemGetTick = 0;
        else if (isOnlineMember(fitem->SecondGetCharID) == true)
            itemGetTick = fitem->FirstGetTick;
        else if (isOnlineMember(fitem->ThirdGetCharID) == true)
            itemGetTick = fitem->SecondGetTick;
    }
    if (GetTick() < itemGetTick)
        return false; // Cannot get item when wait time is still active

    // This function takes care of giving the item to whoever should have it, considering party-share options.  pc_additem's failure tells the player, and the
    // item stays where it is
    if (PartyShareLoot(sd, group, option, fitem->Item, fitem->FirstGetCharID) == false)
    {
        ChatHandler(sd->GetSession()).SendSysMessage(GET_ITEM_FAILED_TEXT);
        return true;
    }

    // Display pickup animation (clif_takeitem: action 3 of the sprite)
    sd->AttackStop();
    sd->HandleEmoteCommand(EMOTE_ONESHOT_LOOT);
    MapClearFloorItem(map, guid);
    return true;
}

void ROWOWMod::ClifParseTakeItem(Player* sd, GameObject* go)
{
    // clif_parse_TakeItem, on a click of the item.  The client has no pending request to answer, so a failure only needs saying
    if (sd->IsAlive() == false || IsInROForm(sd) == false)
        return;
    if (PCCantAct(sd) == true || PCTakeItem(sd, go->GetGUID(), false) == false)
        ChatHandler(sd->GetSession()).SendSysMessage(GET_ITEM_FAILED_TEXT);
}

void ROWOWMod::SkillGreed(Player* sd)
{
    // skill_greed over map_foreachinallrange: the floor items around, each by pc_takeitem with no distance to keep (BS_GREED)
    ROWOWMapFloorItems const* floorItems = sd->GetMap()->CustomData.Get<ROWOWMapFloorItems>("ROWOWMapFloorItems");
    if (floorItems == nullptr)
        return;
    int32 x = 0, y = 0;
    GetUnitCell(sd, x, y);
    int32 splash = SkillGetSplash(ROWOW_SKILL_BS_GREED, 1);
    std::vector<ObjectGuid> guids; // pc_takeitem clears what it takes from what's being walked
    for (auto const& floorItem : floorItems->ItemsByGUID)
        if (CheckDistance(floorItem.second.X - x, floorItem.second.Y - y, splash) == true)
            guids.push_back(floorItem.first);
    for (ObjectGuid const& guid : guids)
        PCTakeItem(sd, guid, true);
}

void ROWOWMod::PCDropItemAtFeet(Player* player, uint32 itemTemplateID, uint32 amount, bool isIdentified)
{
    // map_addflooritem at a player's feet, for what the bags had no room for, free for all (no owner, flags 4)
    int32 x = 0, y = 0;
    GetUnitCell(player, x, y);
    ROWOWDropItem item;
    item.ItemTemplateID = itemTemplateID;
    item.Amount = amount;
    item.IsIdentified = isIdentified;
    MapAddFloorItem(player->GetMap(), item, x, y, ObjectGuid::Empty, ObjectGuid::Empty, ObjectGuid::Empty, ROWOW_FLOORITEM_NO_DROP_CHECK, DIR_MAX, false);
}

ROWOWFloorItem const* ROWOWMod::MobLootSearch(Creature* md, ROWOWMobData* mob, int32 range)
{
    // mob_ai_sub_hard_lootsearch over map_foreachinshootrange: an item in shooting range that it can reach, the one first on the map
    // (monster_loot_search_type, the default) or the closest, which is then its target
    ROWOWMapFloorItems const* floorItems = md->GetMap()->CustomData.Get<ROWOWMapFloorItems>("ROWOWMapFloorItems");
    ROWOWMapGrid const* mapGrid = GetMapGrid(md->GetMapId());
    if (floorItems == nullptr || mapGrid == nullptr || floorItems->ItemsByGUID.empty() == true)
        return nullptr;
    int32 x = 0, y = 0;
    GetUnitCell(md, x, y);
    ROWOWFloorItem const* target = nullptr;
    int32 targetDistance = 0;
    for (auto const& floorItem : floorItems->ItemsByGUID)
    {
        ROWOWFloorItem const& fitem = floorItem.second;
        if (CheckDistance(fitem.X - x, fitem.Y - y, range) == false)
            continue;
        int32 distance = std::max(std::abs(fitem.X - x), std::abs(fitem.Y - y));
        if (target != nullptr && (BattleConfig.MonsterLootSearchType != 0 ? target->ID <= fitem.ID : targetDistance <= distance))
            continue;
        if (PathSearchLong(*mapGrid, x, y, fitem.X, fitem.Y) == false || UnitCanReachCell(md, fitem.X, fitem.Y, BattleConfig.LootRange) == false)
            continue;
        target = &fitem;
        targetDistance = distance;
    }
    if (target != nullptr)
        mob->LootTargetID = target->GUID;
    return target;
}

bool ROWOWMod::MobAISubHardLoot(Creature* md, ROWOWMobData* mob, ROWOWFloorItem const* fitem, uint32 mode, bool canMove, uint64 tick)
{
    // mob_ai_sub_hard's "Loot time."  What it picks up is kept (lootitems), and goes back on the ground when it dies; a full looter loses the first it took
    int32 x = 0, y = 0;
    GetUnitCell(md, x, y);
    int32 lootRange = 0;
    if (mob->IsWalking == true)
    {
        if (x == fitem->X && y == fitem->Y)
            lootRange = 1; // Ready to loot
        else if (mob->LootWalkID == mob->LootTargetID)
            return true; // Already moving to target item
    }
    if (CheckDistance(fitem->X - x, fitem->Y - y, lootRange) == false)
    {
        // Still not within loot range.
        if ((mode & ROWOW_MODE_CANMOVE) == 0)
        {
            MobUnlockTarget(md, tick); // A looter that can't move? Real smart.
            return true;
        }
        if (canMove == false)
            return true; // Stuck. Wait before walking.
        MobSetState(md, mob, ROWOW_MSS_LOOT);
        if (UnitWalkToCell(md, mob, fitem->X, fitem->Y) == false)
            MobUnlockTarget(md, tick); // Can't loot...
        else
            mob->LootWalkID = mob->LootTargetID; // Remember current loot target
        return true;
    }

    // Within looting range.
    if (mob->AttackTargetID.IsEmpty() == false)
        return true; // Busy attacking?
    ObjectGuid guid = fitem->GUID;
    if (mob->LootItems.size() >= ROWOW_LOOTITEM_SIZE)
        mob->LootItems.erase(mob->LootItems.begin()); // Destroy first looted item...
    mob->LootItems.push_back(fitem->Item);
    MapClearFloorItem(md->GetMap(), guid); // Clear item.
    MobUnlockTarget(md, tick);
    return true;
}

bool ROWOWMod::WriteMobDropDump(std::string const& filePath) const
{
    // Each monster's drops after mob_drop_ratio_adjust, as the oracle's @oraclemobdrops writes them: "drop <mob> <item> <rate>" and "mvpdrop <mob> <item> <rate>"
    std::ofstream file(filePath, std::ios::binary);
    if (file.is_open() == false)
        return false;
    std::map<uint32, std::vector<ROWOWMobDrop>> dropsByMobID(MobDropsByMobID.begin(), MobDropsByMobID.end());
    for (auto const& mobDrops : dropsByMobID)
        for (ROWOWMobDrop const& drop : mobDrops.second)
            file << "drop " << mobDrops.first << ' ' << drop.ItemID << ' ' << drop.Rate << '\n';
    std::map<uint32, std::vector<ROWOWMobDrop>> mvpDropsByMobID(MobMvpDropsByMobID.begin(), MobMvpDropsByMobID.end());
    for (auto const& mobDrops : mvpDropsByMobID)
        for (ROWOWMobDrop const& drop : mobDrops.second)
            file << "mvpdrop " << mobDrops.first << ' ' << drop.ItemID << ' ' << drop.Rate << '\n';
    return true;
}

bool ROWOWMod::WriteMobDropSample(Player* player, uint32 mobID, uint32 kills, std::string const& filePath)
{
    // `.ro mobdropsample`: the same rolls a kill by this player makes, kills times, nothing landing anywhere: "<item template> <times>"
    ROWOWStatus const* status = nullptr;
    auto statusIter = MobStatusesByMobID.find(mobID);
    if (statusIter != MobStatusesByMobID.end())
        status = &statusIter->second;
    if (status == nullptr)
        return false;
    std::map<uint32, uint32> counts;
    std::vector<ROWOWDropItem> items;
    for (uint32 i = 0; i < kills; i++)
    {
        items.clear();
        MobRollDrops(mobID, int32(status->Level), status->Race, status->Class, player, player, player, items);
        for (ROWOWDropItem const& item : items)
            counts[item.ItemTemplateID]++;
    }
    std::ofstream file(filePath, std::ios::binary);
    if (file.is_open() == false)
        return false;
    file << "kills " << kills << '\n';
    for (auto const& count : counts)
        file << count.first << ' ' << count.second << '\n';
    return true;
}

void ROWOWMod::WriteFloorItemList(ChatHandler* handler, Map* map) const
{
    // `.ro flooritems`: how many lie on the map, and the first ones of them
    ROWOWMapFloorItems const* floorItems = map->CustomData.Get<ROWOWMapFloorItems>("ROWOWMapFloorItems");
    handler->PSendSysMessage("Floor items on this map: {}, {} in the queue to clear", floorItems != nullptr ? floorItems->ItemsByGUID.size() : 0, floorItems != nullptr ? floorItems->ClearQueue.size() : 0);
    if (floorItems == nullptr)
        return;
    uint32 shownCount = 0;
    uint64 tick = GetTick();
    for (auto const& floorItem : floorItems->ItemsByGUID)
    {
        if (++shownCount > 12)
            break;
        ROWOWFloorItem const& fitem = floorItem.second;
        ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(fitem.Item.ItemTemplateID);
        GameObject const* go = map->GetGameObject(fitem.GUID);
        handler->PSendSysMessage("{}: {} x{} {} at {},{} ({} {} {}), owners {} {} {}, first free in {} ms", fitem.GUID.GetCounter(), itemTemplate != nullptr ? itemTemplate->Name1 : "?", fitem.Item.Amount,
            fitem.Item.IsIdentified == true ? "identified" : "unidentified", fitem.X, fitem.Y, go != nullptr ? go->GetPositionX() : 0.0f, go != nullptr ? go->GetPositionY() : 0.0f,
            go != nullptr ? go->GetPositionZ() : 0.0f, fitem.FirstGetCharID.GetCounter(), fitem.SecondGetCharID.GetCounter(), fitem.ThirdGetCharID.GetCounter(),
            fitem.ThirdGetTick > tick ? fitem.ThirdGetTick - tick : 0);
    }
}

uint32 ROWOWMod::MapFloorItemFill(Player* player, uint32 count, int32 range)
{
    // `.ro floorfill`: random items on random cells around the player, free for all, to load a map with
    ROWOWMapGrid const* mapGrid = GetMapGrid(player->GetMapId());
    if (mapGrid == nullptr || GroundItemsByItemTemplateID.empty() == true)
        return 0;
    std::vector<uint32> itemTemplateIDs;
    for (auto const& groundItem : GroundItemsByItemTemplateID)
        itemTemplateIDs.push_back(groundItem.first);
    int32 x = 0, y = 0;
    GetUnitCell(player, x, y);
    uint32 addedCount = 0;
    for (uint32 i = 0; i < count; i++)
    {
        ROWOWDropItem item;
        item.ItemTemplateID = itemTemplateIDs[urand(0, uint32(itemTemplateIDs.size() - 1))];
        if (MapAddFloorItem(player->GetMap(), item, x + irand(-range, range), y + irand(-range, range), ObjectGuid::Empty, ObjectGuid::Empty, ObjectGuid::Empty,
            ROWOW_FLOORITEM_NO_DROP_CHECK, DIR_CENTER, false).IsEmpty() == false)
            addedCount++;
    }
    return addedCount;
}

class ROWOW_FloorItemGameObjectScript : public AllGameObjectScript
{
public:
    ROWOW_FloorItemGameObjectScript() : AllGameObjectScript("ROWOW_FloorItemGameObjectScript") {}

    bool CanGameObjectGossipHello(Player* player, GameObject* go) override
    {
        if (ROWOW->IsEnabled == false || ROWOW->IsGroundItem(go) == false)
            return false;
        ROWOW->ClifParseTakeItem(player, go);
        return true;
    }

    void OnGameObjectRemoveWorld(GameObject* go) override
    {
        if (ROWOW->IsEnabled == true && ROWOW->IsGroundItem(go) == true)
            ROWOW->OnGroundItemRemoved(go);
    }
};

void AddROWOWFloorItemScripts()
{
    new ROWOW_FloorItemGameObjectScript();
}
