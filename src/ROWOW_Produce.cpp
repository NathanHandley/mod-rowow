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
//  Ported from rAthena (https://github.com/rathena/rathena), src/map/skill.cpp and clif.cpp, pre-renewal paths:
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL.  For more information, see LICENCE in rAthena's main folder

#include "Chat.h"
#include "Item.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include "ROWOW.h"

using namespace std;

// Making and mending items with skills: produce_db and skill_produce_mix (a Blacksmith's forging and smelting, an Alchemist's brewing and
// Twilight Alchemy, Holy Water, the Elemental Converters), the lists the RO_Identity addon picks from (clif_skill_produce_mix_list,
// clif_item_repair_list, clif_item_refine_list), Weapon Repair (skill_repairweapon) and Upgrade Weapon (skill_weaponrefine).  A forged weapon
// keeps its Star Crumbs and element stone in an enchantment and its smith as its creator.  All of it runs in the player's own map thread (a
// repaired player being on its map)

namespace
{
    // The items skill_produce_mix, skill_repairweapon and skill_weaponrefine name (item_db Ids, rAthena's ITEMID_*)
    uint32 const ItemIDRedPotion = 501;
    uint32 const ItemIDYellowPotion = 503;
    uint32 const ItemIDWhitePotion = 504;
    uint32 const ItemIDRedSlimPotion = 545;
    uint32 const ItemIDYellowSlimPotion = 546;
    uint32 const ItemIDWhiteSlimPotion = 547;
    uint32 const ItemIDEmptyBottle = 713;
    uint32 const ItemIDOrideconStone = 756;
    uint32 const ItemIDAlcohol = 970;
    uint32 const ItemIDOridecon = 984;
    uint32 const ItemIDAnvil = 986;
    uint32 const ItemIDOrideconAnvil = 987;
    uint32 const ItemIDGoldenAnvil = 988;
    uint32 const ItemIDEmperiumAnvil = 989;
    uint32 const ItemIDFlameHeart = 994;
    uint32 const ItemIDGreatNature = 997;
    uint32 const ItemIDIron = 998;
    uint32 const ItemIDSteel = 999;
    uint32 const ItemIDStarCrumb = 1000;
    uint32 const ItemIDIronOre = 1002;
    uint32 const ItemIDPhracon = 1010;
    uint32 const ItemIDEmveretarcon = 1011;
    uint32 const ItemIDFireBottle = 7135;
    uint32 const ItemIDAcidBottle = 7136;
    uint32 const ItemIDManEaterBottle = 7137;
    uint32 const ItemIDMiniBottle = 7138;
    uint32 const ItemIDCoatingBottle = 7139;

    // The elements of the element stones, Flame Heart to Great Nature (skill_produce_mix's ele_table), and of a forge enchantment's stone index
    int32 const ElementStoneElements[4] = { ROWOW_ELEMENT_FIRE, ROWOW_ELEMENT_WATER, ROWOW_ELEMENT_WIND, ROWOW_ELEMENT_EARTH };

    // skill_repairweapon's materials by weapon level, and armor's; skill_weaponrefine's by weapon level
    uint32 const RepairWeaponMaterials[4] = { ItemIDIronOre, ItemIDIron, ItemIDSteel, ItemIDOrideconStone };
    uint32 const RepairArmorMaterial = ItemIDSteel;
    uint32 const RefineWeaponMaterials[4] = { ItemIDPhracon, ItemIDEmveretarcon, ItemIDOridecon, ItemIDOridecon };

    // menuskill_id -1: the produce list an item opened (a hammer, the Mini Furnace), which no skill owns
    uint16 const MenuSkillItemProduce = 0xFFFF;

    // COOKMASTERY_VAR: the character variable of a cook's mastery (sd->cook_mastery)
    char const* const CookMasteryVariable = "COOK_MASTERY";

    // The RO item of an item_db Id, or null
    ROWOWItem const* GetROItemByItemID(uint32 itemID)
    {
        auto itemTemplateIDIter = ROWOW->ItemTemplateIDsByItemID.find(itemID);
        if (itemTemplateIDIter == ROWOW->ItemTemplateIDsByItemID.end())
            return nullptr;
        auto itemIter = ROWOW->ItemsByItemTemplateID.find(itemTemplateIDIter->second);
        return itemIter != ROWOW->ItemsByItemTemplateID.end() ? &itemIter->second : nullptr;
    }

    std::string GetItemName(uint32 itemTemplateID)
    {
        ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(itemTemplateID);
        return itemTemplate != nullptr ? itemTemplate->Name1 : std::to_string(itemTemplateID);
    }

    void SendAddonMessage(Player* player, std::string const& message)
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_ADDON, nullptr, nullptr, message);
        player->SendDirectMessage(&data);
    }
}

void ROWOWMod::LoadProduceData()
{
    // produce_db, from mod_rowow_db_produce (the converter's RoProduce.csv): the recipes whose product is a converted item
    ProduceDB.clear();
    if (IsEnabled == false)
        return;
    ROWOWDataTable const* produceTable = GetDataTable("mod_rowow_db_produce");
    if (produceTable == nullptr)
    {
        LOG_ERROR("module.ROWOW", "ROWOWMod::LoadProduceData found no mod_rowow_db_produce table, so nothing can be made (run the converter)");
        return;
    }
    int32 productColumnIndex = produceTable->GetColumnIndex("ProduceItemID");
    int32 itemLevelColumnIndex = produceTable->GetColumnIndex("ItemLevel");
    int32 skillColumnIndex = produceTable->GetColumnIndex("RequireSkill");
    int32 skillLevelColumnIndex = produceTable->GetColumnIndex("RequireSkillLevel");
    int32 materialsColumnIndex = produceTable->GetColumnIndex("Materials");
    for (size_t rowIndex = 0; rowIndex < produceTable->Rows.size(); rowIndex++)
    {
        ROWOWProduce produce;
        produce.NameID = uint32(produceTable->GetInt(rowIndex, productColumnIndex, 0));
        if (produce.NameID == 0 || ItemTemplateIDsByItemID.find(produce.NameID) == ItemTemplateIDsByItemID.end())
            continue;
        produce.ItemLevel = int32(produceTable->GetInt(rowIndex, itemLevelColumnIndex, 0));
        produce.RequireSkillID = uint16(produceTable->GetInt(rowIndex, skillColumnIndex, 0));
        produce.RequireSkillLevel = int32(produceTable->GetInt(rowIndex, skillLevelColumnIndex, 0));
        for (auto const& material : ROWOWDataTable::ParseMapList(produceTable->GetCell(rowIndex, materialsColumnIndex)))
        {
            uint32 materialID = 0;
            int32 amount = 0;
            for (auto const& field : material)
            {
                if (field.first == "ItemID")
                    materialID = uint32(strtoul(field.second.c_str(), nullptr, 10));
                else if (field.first == "Amount")
                    amount = int32(strtol(field.second.c_str(), nullptr, 10));
            }
            if (materialID != 0)
                produce.Materials.emplace_back(materialID, amount);
        }
        ProduceDB.push_back(produce);
    }
    LOG_INFO("module.ROWOW", "ROWOWMod::LoadProduceData loaded {} produce_db recipes", ProduceDB.size());
}

Item* ROWOWMod::PCSearchInventory(Player* player, uint32 itemID) const
{
    // pc_search_inventory: the first item of an item_db Id in the bags, identified and not worn, or null
    auto itemTemplateIDIter = ItemTemplateIDsByItemID.find(itemID);
    ROWOWIdentity const* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    if (itemTemplateIDIter == ItemTemplateIDsByItemID.end() || identity == nullptr)
        return nullptr;
    for (uint8 bag = 0; bag <= 4; bag++)
    {
        for (uint32 slot = 1; slot <= MAX_BAG_SIZE; slot++)
        {
            Item* item = GetItemByClientPosition(player, bag, slot);
            if (item != nullptr && item->GetEntry() == itemTemplateIDIter->second && GetIdentityEquipPosition(*identity, item->GetGUID().GetCounter()) == 0)
                return item;
        }
    }
    return nullptr;
}

bool ROWOWMod::GetItemForge(Item const* item, int32& starCrumbsOut, int32& elementOut)
{
    // card[0] == CARD0_FORGE: the forge enchantment in the first card slot (card[1]: the Star Crumbs and the element)
    starCrumbsOut = 0;
    elementOut = ROWOW_ELEMENT_NEUTRAL;
    if (item == nullptr)
        return false;
    uint32 enchantmentID = item->GetEnchantmentId(SOCK_ENCHANTMENT_SLOT);
    if (enchantmentID < ROWOW_ITEM_FORGE_ENCHANTMENT_START || enchantmentID >= ROWOW_ITEM_FORGE_ENCHANTMENT_START + ROWOW_ITEM_FORGE_ENCHANTMENT_COUNT)
        return false;
    uint32 forge = enchantmentID - ROWOW_ITEM_FORGE_ENCHANTMENT_START;
    starCrumbsOut = int32(forge / ROWOW_ITEM_FORGE_ELEMENT_COUNT);
    uint32 stone = forge % ROWOW_ITEM_FORGE_ELEMENT_COUNT;
    elementOut = stone != 0 ? ElementStoneElements[stone - 1] : ROWOW_ELEMENT_NEUTRAL;
    return true;
}

int32 ROWOWMod::SkillCanProduceMix(Player* player, uint32 nameID, int32 trigger, int32 qty)
{
    // skill_can_produce_mix: the recipe of the item the player's skills allow (a menu's skill picks its own), that the bags have room for
    // and whose materials are all held, qty times (a guide, amount 0, only held).  trigger is the item level asked for: above 20 exactly
    // it, 11-20 any cooking, else a weapon level up to it (-1 any).  The recipe's index plus 1, 0 for none
    if (ItemTemplateIDsByItemID.find(nameID) == ItemTemplateIDsByItemID.end())
        return 0;
    ROWOWIdentity const* identity = GetIdentity(player);
    size_t i = 0;
    for (; i < ProduceDB.size(); i++)
    {
        if (ProduceDB[i].NameID != nameID)
            continue;
        uint16 j = ProduceDB[i].RequireSkillID;
        if (j > 0 && PCCheckSkill(player, j) < ProduceDB[i].RequireSkillLevel)
            continue; // must iterate again to check other skills that produce it. [malufett]
        if (j > 0 && identity->MenuSkillID != 0 && identity->MenuSkillID != MenuSkillItemProduce && identity->MenuSkillID != j)
            continue; // special case
        break;
    }
    if (i >= ProduceDB.size())
        return 0;
    ROWOWProduce const& produce = ProduceDB[i];

    // Cannot carry the produced stuff (pc_checkadditem: here, no room in the bags)
    ItemPosCountVec destination;
    if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, destination, ItemTemplateIDsByItemID.at(nameID), uint32(std::max(1, qty))) != EQUIP_ERR_OK)
        return 0;

    // Matching the requested produce list
    if (trigger >= 0)
    {
        if (trigger > 20)
        {
            // Non-weapon, non-food item (itemlv must match)
            if (produce.ItemLevel != trigger)
                return 0;
        }
        else if (trigger > 10)
        {
            // Food (any item level between 10 and 20 will do)
            if (produce.ItemLevel <= 10 || produce.ItemLevel > 20)
                return 0;
        }
        else if (produce.ItemLevel > trigger)
            return 0; // Weapon (itemlv must be higher or equal)
    }

    // Check on player's inventory
    for (auto const& material : produce.Materials)
    {
        if (material.second == 0)
        {
            if (PCSearchInventory(player, material.first) == nullptr)
                return 0;
            continue;
        }
        auto materialTemplateIDIter = ItemTemplateIDsByItemID.find(material.first);
        if (materialTemplateIDIter == ItemTemplateIDsByItemID.end() || int64(player->GetItemCount(materialTemplateIDIter->second)) < int64(qty) * material.second)
            return 0;
    }
    return int32(i) + 1;
}

int32 ROWOWMod::SkillProduceMixRate(Player* player, uint16 skillID, uint32 nameID, int32 sc, int32 ele)
{
    // skill_produce_mix's make_per (in 10000) for one try of the item with sc Star Crumbs and an element stone's element (ele): smelting by
    // Iron/Steel Tempering and Enchanted Stone Craft, brewing by Potion Research and Prepare Potion, forging by the smithing skill, Weaponry
    // Research, the weapon's level, the additions (-25% an element, -15% a Star Crumb) and the best anvil held; each with the job level, DEX
    // and LUK and a random part (rolled again each call)
    ROWOWStatus const* status = GetROStatus(player);
    ROWOWIdentity const* identity = GetIdentity(player);
    ROWOWItem const* roItem = GetROItemByItemID(nameID);
    if (status == nullptr)
        return 0;
    bool isEquip = roItem != nullptr && (roItem->IsWeapon == true || roItem->IsArmor == true);
    int32 wlv = isEquip == true && roItem->IsWeapon == true ? roItem->WeaponLevel : 0;
    int32 jobLevel = int32(identity->JobLevel);
    ROWOWJob const* job = GetJob(identity->JobID);
    bool isBaby = job != nullptr && job->IsBaby == true;
    int32 makePer = 0;
    if (isEquip == false)
    {
        // A cooking kit's list (menuskill_id AM_PHARMACY with a dish's level), for its dishes alone: they have no skill of their own
        if (skillID == 0 && identity->MenuSkillID == ROWOW_SKILL_AM_PHARMACY && identity->MenuSkillValue > 10 && identity->MenuSkillValue <= 20)
        {
            ROWOWProduce const* produce = nullptr;
            for (ROWOWProduce const& candidate : ProduceDB)
                if (candidate.NameID == nameID && candidate.RequireSkillID == 0)
                    produce = &candidate;
            if (identity->MenuSkillValue >= 15) // Legendary Cooking Set.
                return 10000; // 100% Success
            if (produce == nullptr)
                return 5000;
            int32 cookMastery = int32(GetIdentityVariable(*identity, CookMasteryVariable));
            int32 num = int32(produce->Materials.size()) - 1; // exclude the recipe
            int32 span = 30 + 5 * (cookMastery / 400) - (6 + cookMastery / 80);
            makePer = 1200 * (identity->MenuSkillValue - 10)
                + 20 * (int32(identity->BaseLevel) + 1)
                + 20 * (status->Stats[ROWOW_STAT_DEX] + 1)
                + 100 * (int32(urand(0, std::max(span, 1) - 1)) + (6 + cookMastery / 80))
                - 400 * (produce->ItemLevel - 11 + 1)
                - 10 * (100 - status->Stats[ROWOW_STAT_LUK] + 1)
                - 500 * (num - 1)
                - 100 * (int32(urand(0, 3)) + 1);
            return std::max(makePer, 1);
        }
        switch (skillID)
        {
            case ROWOW_SKILL_BS_IRON:
            case ROWOW_SKILL_BS_STEEL:
            case ROWOW_SKILL_BS_ENCHANTEDSTONE:
            {
                // Ores & Metals Refining - skill bonuses are straight from kRO website [DracoRPG]
                int32 i = PCCheckSkill(player, skillID);
                // Base chance
                makePer = jobLevel * 20;
                makePer += status->Stats[ROWOW_STAT_DEX] * 10;
                makePer += status->Stats[ROWOW_STAT_LUK] * 10;
                makePer += int32(urand(1, 100)) * 10;
                switch (nameID)
                {
                    case ItemIDIron:
                        makePer += 4000 + i * 500; // Temper Iron bonus: +45/+50/+55/+60/+65
                        break;
                    case ItemIDSteel:
                        makePer += 3000 + i * 500; // Temper Steel bonus: +35/+40/+45/+50/+55
                        break;
                    case ItemIDStarCrumb:
                        makePer = 100000; // Star Crumbs are 100% success crafting rate? (made 1000% so it succeeds even after penalties) [Skotlex]
                        break;
                    default: // Enchanted Stones
                        makePer += 1000 + i * 500; // Enchanted stone Craft bonus: +15/+20/+25/+30/+35
                        break;
                }
                break;
            }
            case ROWOW_SKILL_ASC_CDP:
                makePer = (2000 + 40 * status->Stats[ROWOW_STAT_DEX] + 20 * status->Stats[ROWOW_STAT_LUK]);
                break;
            case ROWOW_SKILL_AL_HOLYWATER:
                makePer = 100000; // 100% success
                break;
            case ROWOW_SKILL_AM_PHARMACY: // Potion Preparation - reviewed with the help of various Ragnainfo sources [DracoRPG]
            case ROWOW_SKILL_AM_TWILIGHT1:
            case ROWOW_SKILL_AM_TWILIGHT2:
            case ROWOW_SKILL_AM_TWILIGHT3:
                makePer = PCCheckSkill(player, ROWOW_SKILL_AM_LEARNINGPOTION) * 50 + PCCheckSkill(player, ROWOW_SKILL_AM_PHARMACY) * 300 + jobLevel * 20 +
                    (status->Stats[ROWOW_STAT_INT] / 2) * 10 + status->Stats[ROWOW_STAT_DEX] * 10 + status->Stats[ROWOW_STAT_LUK] * 10;
                if (ROWOWHomunData const* hd = GetHomunculus(player); HomIsActive(hd) == true) // Player got a homun
                {
                    // His homun is a vanil with instruction change: +1% bonus per level
                    if (auto instructIter = hd->Skills.find(ROWOW_SKILL_HVAN_INSTRUCT); instructIter != hd->Skills.end())
                        makePer += int32(instructIter->second) * 100;
                }
                switch (nameID)
                {
                    case ItemIDRedPotion:
                    case ItemIDYellowPotion:
                    case ItemIDWhitePotion:
                        makePer += int32(1 + urand(0, 99)) * 10 + 2000;
                        break;
                    case ItemIDAlcohol:
                        makePer += int32(1 + urand(0, 99)) * 10 + 1000;
                        break;
                    case ItemIDFireBottle:
                    case ItemIDAcidBottle:
                    case ItemIDManEaterBottle:
                    case ItemIDMiniBottle:
                        makePer += int32(1 + urand(0, 99)) * 10;
                        break;
                    case ItemIDYellowSlimPotion:
                        makePer -= int32(1 + urand(0, 49)) * 10;
                        break;
                    case ItemIDWhiteSlimPotion:
                    case ItemIDCoatingBottle:
                        makePer -= int32(1 + urand(0, 99)) * 10;
                        break;
                    // Common items, receive no bonus or penalty, listed just because they are commonly produced
                    default:
                        break;
                }
                if (BattleConfig.PotionProduceRate != 100)
                    makePer = makePer * BattleConfig.PotionProduceRate / 100;
                // Baby classes receive a 30% penalty for these skills
                if (isBaby == true)
                    makePer = (makePer * 70) / 100;
                break;
            case ROWOW_SKILL_SA_CREATECON: // Elemental Converter Creation
                makePer = 100000; // should be 100% success rate
                break;
            default:
                makePer = 5000;
                break;
        }
    }
    else
    {
        // Weapon Forging - skill bonuses are straight from kRO website, other things from a jRO calculator [DracoRPG]
        makePer = jobLevel * 20;
        makePer += status->Stats[ROWOW_STAT_DEX] * 10;
        makePer += status->Stats[ROWOW_STAT_LUK] * 10;
        makePer += int32(urand(1, 100)) * 10;
        // Weapon level base chance
        if (wlv > 0 && wlv < 4)
            makePer += (4 / wlv) * 1000; // +40/+20/+10
        makePer += PCCheckSkill(player, skillID) * 500; // Smithing skills bonus: +5/+10/+15
        // Weaponry Research bonus: +1/+2/+3/+4/+5/+6/+7/+8/+9/+10
        makePer += PCCheckSkill(player, ROWOW_SKILL_BS_WEAPONRESEARCH) * 100;
        // Oridecon Research bonus (custom): +1/+2/+3/+4/+5
        if (BattleConfig.OrideconResearchFix == 1 && wlv >= 3)
            makePer += PCCheckSkill(player, ROWOW_SKILL_BS_ORIDEOCON) * 100;
        // Element Stone: -25%
        if (ele != 0)
            makePer -= 2500;
        // Star Crumb: -15% each
        makePer -= sc * 1500;
        if (PCSearchInventory(player, ItemIDEmperiumAnvil) != nullptr)
            makePer += 1000; // Emperium Anvil: +10
        else if (PCSearchInventory(player, ItemIDGoldenAnvil) != nullptr)
            makePer += 500; // Golden Anvil: +5
        else if (PCSearchInventory(player, ItemIDOrideconAnvil) != nullptr)
            makePer += 250; // Oridecon Anvil: +2.5
        else if (PCSearchInventory(player, ItemIDAnvil) != nullptr)
            makePer += 0; // Anvil: +0
        if (BattleConfig.WeaponProduceRate != 100)
            makePer = makePer * BattleConfig.WeaponProduceRate / 100;
        // Baby classes receive a 30% penalty when the target item is a weapon
        if (isBaby == true)
            makePer = (makePer * 70) / 100;
    }

    if (makePer < 1)
        makePer = 1;
    return makePer;
}

bool ROWOWMod::SkillProduceMix(Player* player, uint16 skillID, uint32 nameID, int32 slot1, int32 slot2, int32 slot3, int32 qty, int32 produceIndex)
{
    // skill_produce_mix: the recipe's materials used up (and a weapon's Star Crumbs and element stone, the slots), then the item made at the
    // skill's success rate (make_per, in 10000).  A forged weapon keeps its Star Crumbs and element and its smith; several items (Twilight
    // Alchemy) are each made at the rate, as a dish is by a cooking kit's list.  Brewed potions carry their maker's name as forged weapons do, kept
    // on the stack's ITEM_FIELD_CREATOR, which keeps it apart (Item::CanBeMergedPartlyWith).  (The third jobs' cases aren't in)
    ROWOWStatus const* status = GetROStatus(player);
    ROWOWIdentity* identity = GetIdentity(player);
    if (status == nullptr)
        return false;
    int32 idx = produceIndex;
    if (produceIndex == -1)
    {
        if ((idx = SkillCanProduceMix(player, nameID, -1, qty)) == 0)
            return false;
        idx--;
    }
    if (idx < 0 || idx >= int32(ProduceDB.size()))
        return false;
    ROWOWProduce const& produce = ProduceDB[idx];
    if (qty < 1)
        qty = 1;
    if (skillID == 0)
        skillID = produce.RequireSkillID; // A skill can be specified for some override cases.

    // Note that qty should always be one if you are using these!
    int32 slots[3] = { slot1, slot2, slot3 };
    int32 sc = 0, ele = 0;
    for (int32 slot : slots)
    {
        if (slot <= 0)
            continue;
        Item* item = PCSearchInventory(player, uint32(slot));
        if (item == nullptr)
            continue;
        if (uint32(slot) == ItemIDStarCrumb)
        {
            uint32 count = 1;
            player->DestroyItemCount(item, count, true);
            sc++;
        }
        if (uint32(slot) >= ItemIDFlameHeart && uint32(slot) <= ItemIDGreatNature && ele == 0)
        {
            uint32 count = 1;
            player->DestroyItemCount(item, count, true);
            ele = ElementStoneElements[uint32(slot) - ItemIDFlameHeart];
        }
    }

    for (auto const& material : produce.Materials)
    {
        auto materialTemplateIDIter = ItemTemplateIDsByItemID.find(material.first);
        if (materialTemplateIDIter == ItemTemplateIDsByItemID.end())
            continue;
        uint32 x = uint32(qty * material.second);
        if (x > 0)
            player->DestroyItemCount(materialTemplateIDIter->second, x, true);
    }

    ROWOWItem const* roItem = GetROItemByItemID(nameID);
    bool isEquip = roItem != nullptr && (roItem->IsWeapon == true || roItem->IsArmor == true);
    int32 makePer = SkillProduceMixRate(player, skillID, nameID, sc, ele);

    ChatHandler handler(player->GetSession());
    std::string itemName = GetItemName(ItemTemplateIDsByItemID.at(nameID));
    if (qty > 1 || int32(urand(0, 9999)) < makePer)
    {
        // Success, or crafting multiple items
        if (isEquip == true)
        {
            // CARD0_FORGE: the Star Crumbs and the element in the first card slot's enchantment, the smith as the item's creator
            Item* item = nullptr;
            if (GiveROItem(player, nameID, 1, true, &item) == false)
                return false;
            if (item != nullptr)
            {
                int32 stone = 0;
                for (int32 i = 0; i < 4; i++)
                    if (ElementStoneElements[i] == ele)
                        stone = i + 1;
                item->SetEnchantment(SOCK_ENCHANTMENT_SLOT, ROWOW_ITEM_FORGE_ENCHANTMENT_START + std::min(sc, 3) * ROWOW_ITEM_FORGE_ELEMENT_COUNT + stone, 0, 0);
                item->SetGuidValue(ITEM_FIELD_CREATOR, player->GetGUID());
                item->SetState(ITEM_CHANGED, player);
                SendItemCardsToClient(player, *identity, true);
            }
            // clif_produceeffect(0) and the refine success effect
            handler.PSendSysMessage("You have successfully forged {}.", itemName);
            int32 wlv = roItem->IsWeapon == true ? roItem->WeaponLevel : 0;
            if (wlv >= 3 && ((ele != 0 ? 1 : 0) + sc) >= 3) // Fame point system [DracoRPG]
                PCAddFame(player, BattleConfig.FameForge); // Success to forge a lv3 weapon with 3 additional ingredients = +10 fame point
            return true;
        }

        // Each of several is made at the rate
        int32 amount = 0;
        int32 fame = 0;
        bool isCondensedPotion = (nameID == ItemIDRedSlimPotion || nameID == ItemIDYellowSlimPotion || nameID == ItemIDWhiteSlimPotion) && (skillID == ROWOW_SKILL_AM_PHARMACY ||
            skillID == ROWOW_SKILL_AM_TWILIGHT1 || skillID == ROWOW_SKILL_AM_TWILIGHT2 || skillID == ROWOW_SKILL_AM_TWILIGHT3);
        for (int32 i = 0; i < qty; i++)
        {
            if (qty == 1 || int32(urand(0, 9999)) < makePer)
            {
                amount++; // Success
                if (isCondensedPotion == false)
                    continue;

                // Add fame as needed.
                switch (++identity->PotionSuccessCounter)
                {
                    case 3: fame += BattleConfig.FamePharmacy3; break; // Success to prepare 3 Condensed Potions in a row
                    case 5: fame += BattleConfig.FamePharmacy5; break; // Success to prepare 5 Condensed Potions in a row
                    case 7: fame += BattleConfig.FamePharmacy7; break; // Success to prepare 7 Condensed Potions in a row
                    case 10: // Success to prepare 10 Condensed Potions in a row
                        fame += BattleConfig.FamePharmacy10;
                        identity->PotionSuccessCounter = 0;
                        break;
                    default:
                        break;
                }
            }
            else if (isCondensedPotion == true) // Failure
                identity->PotionSuccessCounter = 0;
        }
        if (fame != 0)
            PCAddFame(player, fame);
        if (amount > 0)
        {
            // produce_item_name_input: the products that carry their maker's name (CARD0_CREATE)
            int32 nameFlag = 0x80;
            switch (skillID)
            {
                case ROWOW_SKILL_AM_PHARMACY:
                case ROWOW_SKILL_AM_TWILIGHT1:
                case ROWOW_SKILL_AM_TWILIGHT2:
                case ROWOW_SKILL_AM_TWILIGHT3:
                    nameFlag = 0x2;
                    break;
                case ROWOW_SKILL_AL_HOLYWATER:
                    nameFlag = 0x8;
                    break;
                case ROWOW_SKILL_ASC_CDP:
                    nameFlag = 0x10;
                    break;
                default:
                    break;
            }
            ObjectGuid::LowType creatorGUID = (BattleConfig.ProduceItemNameInput & nameFlag) != 0 ? player->GetGUID().GetCounter() : 0;
            uint32 itemTemplateID = ItemTemplateIDsByItemID.at(nameID);
            if (StoreROItem(player, itemTemplateID, uint32(amount), nullptr, creatorGUID) == nullptr)
                GiveROItem(player, nameID, uint32(amount), true); // No room: it lands at its feet, nameless
            switch (skillID)
            {
                case ROWOW_SKILL_AM_PHARMACY:
                case ROWOW_SKILL_AM_TWILIGHT1:
                case ROWOW_SKILL_AM_TWILIGHT2:
                case ROWOW_SKILL_AM_TWILIGHT3:
                case ROWOW_SKILL_ASC_CDP:
                    // clif_produceeffect(2): the pharmacy success
                    handler.PSendSysMessage(amount > 1 ? "You have successfully made {} x{}." : "You have successfully made {}.", itemName, amount);
                    break;
                case ROWOW_SKILL_BS_IRON:
                case ROWOW_SKILL_BS_STEEL:
                case ROWOW_SKILL_BS_ENCHANTEDSTONE:
                    // clif_produceeffect(0): the refine success
                    handler.PSendSysMessage("You have successfully made {}.", itemName);
                    break;
                default: // Those that don't require a skill?
                    if (produce.ItemLevel > 10 && produce.ItemLevel <= 20)
                    {
                        // Cooking items.
                        handler.PSendSysMessage("You have successfully cooked {}.", itemName); // clif_specialeffect(EF_COOKING_OK)
                        SetIdentityVariable(*identity, CookMasteryVariable, GetIdentityVariable(*identity, CookMasteryVariable) + (int64(1) << ((produce.ItemLevel - 11) / 2)) * 5);
                    }
                    break;
            }
            return true;
        }
    }

    // Failure
    switch (skillID)
    {
        case ROWOW_SKILL_ASC_CDP: // 25% Damage yourself, and display same effect as failed potion.
            StatusPercentChange(nullptr, player, -25, 0, 1);
            [[fallthrough]];
        case ROWOW_SKILL_AM_PHARMACY:
        case ROWOW_SKILL_AM_TWILIGHT1:
        case ROWOW_SKILL_AM_TWILIGHT2:
        case ROWOW_SKILL_AM_TWILIGHT3:
            // clif_produceeffect(3): the pharmacy failure
            handler.PSendSysMessage("You failed to make {}.", itemName);
            identity->PotionSuccessCounter = 0; // Fame point system [DracoRPG]
            break;
        default:
            // clif_produceeffect(1): the refine failure (forging, smelting)
            if (isEquip == true || skillID == ROWOW_SKILL_BS_IRON || skillID == ROWOW_SKILL_BS_STEEL || skillID == ROWOW_SKILL_BS_ENCHANTEDSTONE)
                handler.PSendSysMessage("You failed to make {}.", itemName);
            else if (produce.ItemLevel > 10 && produce.ItemLevel <= 20)
            {
                // Cooking items.
                handler.PSendSysMessage("You failed to cook {}.", itemName); // clif_specialeffect(EF_COOKING_FAIL)
                int64 mastery = int64(1) << ((produce.ItemLevel - 11) / 2);
                SetIdentityVariable(*identity, CookMasteryVariable, GetIdentityVariable(*identity, CookMasteryVariable) - mastery - ((mastery >> 1) * 3));
            }
            break;
    }
    return false;
}

void ROWOWMod::PCFameRefine(Player* player, Item const* item)
{
    // Fame point system [DracoRPG]: success to refine to +10 a weapon you forged: +1 at level 1, +25 at level 2, +1000 at level 3
    ROWOWItem const* roItem = GetROItem(item);
    int32 starCrumbs = 0, element = 0;
    if (roItem == nullptr || roItem->IsWeapon == false || GetItemForge(item, starCrumbs, element) == false || item->GetGuidValue(ITEM_FIELD_CREATOR) != player->GetGUID())
        return;
    switch (roItem->WeaponLevel)
    {
        case 1: PCAddFame(player, BattleConfig.FameRefineLv1); break;
        case 2: PCAddFame(player, BattleConfig.FameRefineLv2); break;
        case 3: PCAddFame(player, BattleConfig.FameRefineLv3); break;
        default: break;
    }
}

void ROWOWMod::ClifSkillProduceMixList(Player* player, int32 skillID, int32 trigger)
{
    // clif_skill_produce_mix_list: the items the player can make of the list's kind (trigger, the item level asked for) and its skill's (-1,
    // an item's list: any skill's), for the RO_Identity addon to list (ROWOWPRODUCE "<trigger>\t<item template>,...", and for weapons
    // "\t<Star Crumb>:<Flame Heart>:<Mystic Frozen>:<Rough Wind>:<Great Nature>" templates, the additions a forge may take).  The pick comes
    // back as .ro produce (clif_parse_ProduceMix)
    ROWOWIdentity* identity = GetIdentity(player);
    uint16 menuSkillID = skillID > 0 ? uint16(skillID) : MenuSkillItemProduce;
    // Avoid resending the menu
    if (identity->MenuSkillID == menuSkillID)
        return;
    std::string itemTemplateIDs;
    int32 count = 0;
    for (ROWOWProduce const& produce : ProduceDB)
    {
        if (SkillCanProduceMix(player, produce.NameID, trigger, 1) == 0)
            continue;
        if (skillID > 0 && produce.RequireSkillID != skillID)
            continue;
        itemTemplateIDs += fmt::format("{}{}", itemTemplateIDs.empty() == true ? "" : ",", ItemTemplateIDsByItemID.at(produce.NameID));
        count++;
    }
    std::string message = fmt::format("ROWOWPRODUCE\t{}\t{}", trigger, itemTemplateIDs);
    if (trigger >= 1 && trigger <= 10)
    {
        // A forge's additions: a Star Crumb and the four element stones
        std::string additions;
        for (uint32 itemID : { ItemIDStarCrumb, ItemIDFlameHeart, ItemIDFlameHeart + 1, ItemIDFlameHeart + 2, ItemIDGreatNature })
        {
            auto itemTemplateIDIter = ItemTemplateIDsByItemID.find(itemID);
            additions += fmt::format("{}{}", additions.empty() == true ? "" : ":", itemTemplateIDIter != ItemTemplateIDsByItemID.end() ? itemTemplateIDIter->second : 0);
        }
        message += "\t" + additions;
    }
    SendAddonMessage(player, message);
    if (count > 0)
    {
        identity->MenuSkillID = menuSkillID;
        identity->MenuSkillValue = trigger;
    }
}

void ROWOWMod::ClifCookingList(Player* player, int32 trigger)
{
    // clif_cooking_list with AM_PHARMACY: a cooking kit's dishes the player has the materials and the guide for (the produce list, trigger the
    // kit's level), asked by the cooking script command.  The window's pick is .ro produce, as a potion's: clif_parse_Cooking with type 1, no skill
    ROWOWIdentity* identity = GetIdentity(player);
    std::string itemTemplateIDs;
    for (ROWOWProduce const& produce : ProduceDB)
    {
        if (produce.ItemLevel <= 10 || produce.ItemLevel > 20 || SkillCanProduceMix(player, produce.NameID, trigger, 1) == 0)
            continue;
        std::string text = fmt::format("{}", ItemTemplateIDsByItemID.at(produce.NameID));
        if (("," + itemTemplateIDs + ",").find("," + text + ",") == std::string::npos)
            itemTemplateIDs += (itemTemplateIDs.empty() == true ? "" : ",") + text;
    }
    SendAddonMessage(player, fmt::format("ROWOWPRODUCE\t{}\t{}", trigger, itemTemplateIDs));
    identity->MenuSkillID = ROWOW_SKILL_AM_PHARMACY;
    identity->MenuSkillValue = trigger;
}

void ROWOWMod::ClifParseProduceMix(Player* player, uint32 nameID, uint32 slot1, uint32 slot2, uint32 slot3)
{
    // clif_parse_ProduceMix: the produce list's pick (an item_db Id; 0 cancels), made with its additions if the list allows it, and the menu
    // closed (clif_menuskill_clear)
    ROWOWIdentity* identity = GetIdentity(player);
    if (identity->MenuSkillID != MenuSkillItemProduce && identity->MenuSkillID != ROWOW_SKILL_AM_PHARMACY)
        return;
    if (nameID != 0)
    {
        int32 produceIndex = SkillCanProduceMix(player, nameID, identity->MenuSkillValue, 1);
        if (produceIndex != 0)
            SkillProduceMix(player, 0, nameID, int32(slot1), int32(slot2), int32(slot3), 1, produceIndex - 1);
    }
    identity->MenuSkillID = 0;
    identity->MenuSkillValue = 0;
}

void ROWOWMod::ClifItemRepairList(Player* sd, Player* dstsd, uint16 skillLevel)
{
    // clif_item_repair_list: the target's broken items, for the RO_Identity addon to list (ROWOWREPAIR "<item GUID>:<item template>:<refine>,...");
    // the pick comes back as .ro repair (skill_repairweapon).  With none the skill fails
    std::string items;
    int32 c = 0;
    for (uint8 bag = 0; bag <= 4; bag++)
    {
        for (uint32 slot = 1; slot <= MAX_BAG_SIZE; slot++)
        {
            Item* item = GetItemByClientPosition(dstsd, bag, slot);
            if (item == nullptr || GetROItem(item) == nullptr || IsItemBroken(item) == false)
                continue;
            items += fmt::format("{}{}:{}:{}", items.empty() == true ? "" : ",", item->GetGUID().GetCounter(), item->GetEntry(), GetItemRefine(item));
            c++;
        }
    }
    if (c > 0)
    {
        SendAddonMessage(sd, "ROWOWREPAIR\t" + items);
        ROWOWIdentity* identity = GetIdentity(sd);
        identity->MenuSkillID = ROWOW_SKILL_BS_REPAIRWEAPON;
        identity->MenuSkillTargetGUID = dstsd->GetGUID();
        identity->MenuSkillValue2 = skillLevel;
    }
    else
        ClifSkillFail(sd, ROWOW_SKILL_BS_REPAIRWEAPON, ROWOW_USESKILL_FAIL_LEVEL);
}

void ROWOWMod::SkillRepairWeapon(Player* sd, ObjectGuid::LowType itemGUID)
{
    // skill_repairweapon: the target's broken item mended, for one of its material (by weapon level: Iron Ore, Iron, Steel, Rough Oridecon;
    // armor: Steel) from the Blacksmith's bags.  The target must still be in the skill's range and on its map
    ROWOWIdentity* identity = GetIdentity(sd);
    if (identity->MenuSkillID != ROWOW_SKILL_BS_REPAIRWEAPON)
        return;
    uint16 skillLevel = uint16(identity->MenuSkillValue2);
    ObjectGuid targetGUID = identity->MenuSkillTargetGUID;
    identity->MenuSkillID = 0; // clif_menuskill_clear
    identity->MenuSkillTargetGUID.Clear();
    identity->MenuSkillValue2 = 0;
    Player* targetSD = ObjectAccessor::GetPlayer(*sd, targetGUID);
    if (targetSD == nullptr || itemGUID == 0) // Failed.... / No item selected ('Cancel' clicked)
        return;
    Item* item = targetSD->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(itemGUID));
    ROWOWItem const* roItem = GetROItem(item);
    if (roItem == nullptr || IsItemBroken(item) == false)
        return; // Again invalid item....
    if (sd != targetSD && DistanceBL(sd, targetSD) > SkillGetRange2(sd, ROWOW_SKILL_BS_REPAIRWEAPON, skillLevel, true))
    {
        ChatHandler(sd->GetSession()).SendSysMessage("Item repair failed."); // clif_item_repaireffect(failure)
        return;
    }
    uint32 material = 0;
    if (roItem->IsWeapon == true && roItem->WeaponLevel >= 1 && roItem->WeaponLevel <= 4)
        material = RepairWeaponMaterials[roItem->WeaponLevel - 1];
    else if (roItem->IsArmor == true)
        material = RepairArmorMaterial;
    Item* materialItem = material != 0 ? PCSearchInventory(sd, material) : nullptr;
    if (materialItem == nullptr)
    {
        ClifSkillFail(sd, ROWOW_SKILL_BS_REPAIRWEAPON, ROWOW_USESKILL_FAIL_LEVEL);
        return;
    }
    ClifSkillNoDamage(sd, targetSD, ROWOW_SKILL_BS_REPAIRWEAPON, 1);
    SetItemState(targetSD, item, GetItemRefine(item), false, IsItemIdentified(item)); // clear broken state
    uint32 count = 1;
    sd->DestroyItemCount(materialItem, count, true);
    // clif_item_repaireffect: to both
    ChatHandler(sd->GetSession()).PSendSysMessage("{} has been repaired.", item->GetTemplate()->Name1);
    if (sd != targetSD)
        ChatHandler(targetSD->GetSession()).PSendSysMessage("{} has been repaired.", item->GetTemplate()->Name1);
}

void ROWOWMod::ClifItemRefineList(Player* sd)
{
    // clif_item_refine_list: the weapons in the bags (identified, not in the hands) below Upgrade Weapon's level whose material the
    // Whitesmith has, for the RO_Identity addon to list (ROWOWREFINE "<item GUID>:<item template>:<refine>,..."); the pick comes back as
    // .ro weaponrefine (skill_weaponrefine)
    ROWOWIdentity* identity = GetIdentity(sd);
    int32 skillLevel = PCCheckSkill(sd, ROWOW_SKILL_WS_WEAPONREFINE);
    bool hasMaterial[4];
    for (int32 i = 0; i < 4; i++)
        hasMaterial[i] = PCSearchInventory(sd, RefineWeaponMaterials[i]) != nullptr;
    std::string items;
    int32 count = 0;
    for (uint8 bag = 0; bag <= 4; bag++)
    {
        for (uint32 slot = 1; slot <= MAX_BAG_SIZE; slot++)
        {
            Item* item = GetItemByClientPosition(sd, bag, slot);
            ROWOWItem const* roItem = GetROItem(item);
            if (roItem == nullptr || roItem->IsWeapon == false || int32(GetItemRefine(item)) >= skillLevel || IsItemIdentified(item) == false ||
                roItem->WeaponLevel < 1 || roItem->WeaponLevel > 4 || hasMaterial[roItem->WeaponLevel - 1] == false ||
                (GetIdentityEquipPosition(*identity, item->GetGUID().GetCounter()) & (ROWOW_EQUIP_POSITION_HAND_R | ROWOW_EQUIP_POSITION_HAND_L)) != 0)
                continue;
            items += fmt::format("{}{}:{}:{}", items.empty() == true ? "" : ",", item->GetGUID().GetCounter(), item->GetEntry(), GetItemRefine(item));
            count++;
        }
    }
    SendAddonMessage(sd, "ROWOWREFINE\t" + items);
    if (count > 0)
    {
        identity->MenuSkillID = ROWOW_SKILL_WS_WEAPONREFINE;
        identity->MenuSkillValue = skillLevel;
    }
}

void ROWOWMod::SkillWeaponRefine(Player* sd, ObjectGuid::LowType itemGUID)
{
    // skill_weaponrefine: one refine level, for one of the weapon level's material (Phracon, Emveretarcon, Oridecon), at refine.yml's normal
    // chance plus (job level - 50) / 2 percent; a failure destroys the weapon
    ROWOWIdentity* identity = GetIdentity(sd);
    if (identity->MenuSkillID != ROWOW_SKILL_WS_WEAPONREFINE) // Packet exploit?
        return;
    int32 skillLevel = identity->MenuSkillValue;
    identity->MenuSkillID = 0; // clif_menuskill_clear
    identity->MenuSkillValue = 0;
    Item* item = itemGUID != 0 ? sd->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(itemGUID)) : nullptr;
    ROWOWItem const* ditem = GetROItem(item);
    if (ditem == nullptr || ditem->IsWeapon == false)
        return;
    ChatHandler handler(sd->GetSession());
    if (ditem->IsRefineable == false || ditem->WeaponLevel < 1 || ditem->WeaponLevel > 4)
    {
        // if the item isn't refinable
        ClifSkillFail(sd, ROWOW_SKILL_WS_WEAPONREFINE, ROWOW_USESKILL_FAIL_LEVEL);
        return;
    }
    uint32 refine = GetItemRefine(item);
    if (int32(refine) >= skillLevel || refine >= ROWOW_MAX_REFINE)
    {
        handler.PSendSysMessage("{} can't be upgraded any further.", item->GetTemplate()->Name1); // clif_upgrademessage(2)
        return;
    }
    uint32 materialID = RefineWeaponMaterials[ditem->WeaponLevel - 1];
    Item* material = PCSearchInventory(sd, materialID);
    if (material == nullptr)
    {
        handler.PSendSysMessage("You need {} to upgrade it.", GetItemName(ItemTemplateIDsByItemID.at(materialID))); // clif_upgrademessage(3)
        return;
    }
    int32 per = RefineBonuses[ditem->WeaponLevel][refine + 1].Chance / 100;
    per += (int32(identity->JobLevel) - 50) / 2; // Updated per the new kro descriptions. [Skotlex]
    uint32 count = 1;
    sd->DestroyItemCount(material, count, true);
    std::string itemName = item->GetTemplate()->Name1;
    if (per > int32(urand(0, 99)))
    {
        SetItemState(sd, item, refine + 1, IsItemBroken(item), IsItemIdentified(item));
        handler.PSendSysMessage("Upgrade success!! ({} is now +{})", itemName, refine + 1); // clif_upgrademessage(0), clif_refine success
        SendItemCardsToClient(sd, *identity, true);
        if (refine + 1 == 10)
            PCFameRefine(sd, item);
    }
    else
    {
        // The weapon is lost (pc_delitem)
        count = 1;
        sd->DestroyItemCount(item, count, true);
        handler.PSendSysMessage("Upgrade failed!! {} was destroyed.", itemName); // clif_upgrademessage(1), clif_refine failure
    }
}
