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
#include "Item.h"
#include "Log.h"
#include "Player.h"
#include "QueryResult.h"
#include "SpellAuraEffects.h"

#include "ROWOW.h"

using namespace std;

// RO illusions: the job sprite displays (by weapon and shield held) and the head and headgear auras of a player transformed into an RO job

namespace
{
    // The weapon sprite types (the converter's RoPlayerWeaponSprite IDs) shown for each RO weapon type, the first one the job has.  The
    // classic sprites have none for instruments, whips and books
    std::vector<uint32> const WeaponSpriteTypeIDsByWeaponType[ROWOW_WEAPON_TYPE_MAX] =
    {
        { }, { 1, 2 }, { 2, 1 }, { 3, 2 }, { 4, 5 }, { 5, 4 }, { 6, 7 }, { 7, 6 }, { 8 }, { 8 }, { 9 }, { 10 }, { 12, 11 }, { }, { }, { },
        { 11, 12 }, { 13 }, { 14 }, { 16 }, { 15 }, { 14 }, { 17 }, { 9 },
    };
}

void ROWOWMod::LoadIllusionData()
{
    IllusionsBySpellID.clear();
    IllusionSpellIDsByJobKey.clear();
    ItemVisualsByItemID.clear();
    HeadSpellIDsByKey.clear();
    HeadSpellIDs.clear();
    OptionLookSpellIDsByKey.clear();
    OptionLookSpellIDs.clear();
    GarmentSpellIDsByKey.clear();
    GarmentSpellIDs.clear();
    HatEffectSpellIDsByName.clear();
    HatEffectSpellIDs.clear();
    ScriptLookSoundIDsByName.clear();
    if (IsEnabled == false)
        return;

    // The converter creates these tables, so they may not exist yet on a server that hasn't had the ROWOW SQL applied
    QueryResult tableResult = WorldDatabase.Query("SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name IN ('mod_rowow_illusion', 'mod_rowow_illusion_display', 'mod_rowow_item_visual')");
    if (!tableResult || tableResult->Fetch()[0].Get<uint64>() < 3)
    {
        LOG_INFO("module.ROWOW", "ROWOWMod::LoadIllusionData found no RO illusion tables, so RO illusions show without weapons or headgear");
        return;
    }
    if (QueryResult result = WorldDatabase.Query("SELECT `SpellID`, `FormCreatureTemplateID`, `IsMale`, `JobKey` FROM `mod_rowow_illusion`"))
    {
        do
        {
            Field* fields = result->Fetch();
            ROWOWIllusion illusion;
            illusion.SpellID = fields[0].Get<uint32>();
            illusion.FormCreatureTemplateID = fields[1].Get<uint32>();
            illusion.IsMale = fields[2].Get<uint8>() != 0;
            std::string jobKey = fields[3].Get<std::string>();
            illusion.IsRiding = jobKey.size() > 7 && jobKey.compare(jobKey.size() - 7, 7, "_riding") == 0;
            IllusionsBySpellID[illusion.SpellID] = illusion;
            IllusionSpellIDsByJobKey[GetIllusionJobKey(jobKey, illusion.IsMale)] = illusion.SpellID;
        } while (result->NextRow());
    }
    if (QueryResult result = WorldDatabase.Query("SELECT `SpellID`, `WeaponTypeID`, `ShieldTypeID`, `DisplayID` FROM `mod_rowow_illusion_display`"))
    {
        do
        {
            Field* fields = result->Fetch();
            auto illusionIter = IllusionsBySpellID.find(fields[0].Get<uint32>());
            if (illusionIter == IllusionsBySpellID.end())
                continue;
            uint32 shieldTypeID = fields[2].Get<uint32>();
            illusionIter->second.DisplayIDsByKey[ROWOWIllusion::GetIllusionDisplayKey(fields[1].Get<uint32>(), shieldTypeID)] = fields[3].Get<uint32>();
            if (shieldTypeID != 0)
                illusionIter->second.ShieldTypeIDs.insert(shieldTypeID);
        } while (result->NextRow());
    }
    QueryResult weaponTableResult = WorldDatabase.Query("SELECT 1 FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = 'mod_rowow_illusion_weapon'");
    if (weaponTableResult)
    {
        if (QueryResult result = WorldDatabase.Query("SELECT `SpellID`, `ItemSubclass`, `WeaponTypeID` FROM `mod_rowow_illusion_weapon`"))
        {
            do
            {
                Field* fields = result->Fetch();
                auto illusionIter = IllusionsBySpellID.find(fields[0].Get<uint32>());
                if (illusionIter != IllusionsBySpellID.end())
                    illusionIter->second.WeaponTypeIDsByItemSubclass[fields[1].Get<uint8>()] = fields[2].Get<uint32>();
            } while (result->NextRow());
        }
    }
    if (QueryResult result = WorldDatabase.Query("SELECT `ItemTemplateID`, `VisualType`, `ViewID`, `VisualSpellID` FROM `mod_rowow_item_visual`"))
    {
        do
        {
            Field* fields = result->Fetch();
            ROWOWItemVisual itemVisual;
            itemVisual.VisualType = fields[1].Get<uint8>();
            itemVisual.ViewID = fields[2].Get<uint32>();
            itemVisual.VisualSpellID = fields[3].Get<uint32>();
            ItemVisualsByItemID[fields[0].Get<uint32>()] = itemVisual;
        } while (result->NextRow());
    }
    QueryResult headgearTableResult = WorldDatabase.Query("SELECT 1 FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = 'mod_rowow_headgear'");
    if (headgearTableResult)
    {
        if (QueryResult result = WorldDatabase.Query("SELECT `ViewID`, `VisualType`, `IsMale`, `SpellID` FROM `mod_rowow_headgear`"))
        {
            do
            {
                Field* fields = result->Fetch();
                uint32 spellID = fields[3].Get<uint32>();
                HeadgearSpellIDsByKey[GetHeadgearKey(fields[0].Get<uint32>(), fields[1].Get<uint8>(), fields[2].Get<uint8>() != 0)] = spellID;
                HeadgearSpellIDs.insert(spellID);
            } while (result->NextRow());
        }
    }
    QueryResult headTableResult = WorldDatabase.Query("SELECT 1 FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = 'mod_rowow_head'");
    if (headTableResult)
    {
        if (QueryResult result = WorldDatabase.Query("SELECT `Style`, `Color`, `IsMale`, `SpellID` FROM `mod_rowow_head`"))
        {
            do
            {
                Field* fields = result->Fetch();
                uint32 spellID = fields[3].Get<uint32>();
                HeadSpellIDsByKey[GetHeadKey(fields[0].Get<uint32>(), fields[1].Get<uint32>(), fields[2].Get<uint8>() != 0)] = spellID;
                HeadSpellIDs.insert(spellID);
            } while (result->NextRow());
        }
    }
    ScriptLookSpellIDsByKey.clear();
    QueryResult scriptLookTableResult = WorldDatabase.Query("SELECT 1 FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = 'mod_rowow_script_look'");
    if (scriptLookTableResult)
    {
        // The sounds' own IDs came later (R9.6, for map-wide sounds), so a table without them still loads
        QueryResult soundColumnResult = WorldDatabase.Query("SELECT 1 FROM information_schema.columns WHERE table_schema = DATABASE() AND table_name = 'mod_rowow_script_look' AND column_name = 'SoundEntryID'");
        if (QueryResult result = WorldDatabase.Query(soundColumnResult ? "SELECT `Kind`, `Name`, `SpellID`, `SoundEntryID` FROM `mod_rowow_script_look`"
            : "SELECT `Kind`, `Name`, `SpellID`, 0 FROM `mod_rowow_script_look`"))
        {
            do
            {
                Field* fields = result->Fetch();
                uint8 kind = fields[0].Get<uint8>();
                std::string name = fields[1].Get<std::string>();
                ScriptLookSpellIDsByKey[GetScriptLookKey(kind, name)] = fields[2].Get<uint32>();
                if (kind == ROWOW_SCRIPT_LOOK_SOUND && fields[3].Get<uint32>() != 0)
                    ScriptLookSoundIDsByName[name] = fields[3].Get<uint32>();
            } while (result->NextRow());
        }
    }
    QueryResult garmentTableResult = WorldDatabase.Query("SELECT 1 FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = 'mod_rowow_garment'");
    if (garmentTableResult)
    {
        if (QueryResult result = WorldDatabase.Query("SELECT `ViewID`, `IsMale`, `SpellID` FROM `mod_rowow_garment`"))
        {
            do
            {
                Field* fields = result->Fetch();
                uint32 spellID = fields[2].Get<uint32>();
                GarmentSpellIDsByKey[GetGarmentKey(fields[0].Get<uint32>(), fields[1].Get<uint8>() != 0)] = spellID;
                GarmentSpellIDs.insert(spellID);
            } while (result->NextRow());
        }
    }
    QueryResult hatEffectTableResult = WorldDatabase.Query("SELECT 1 FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = 'mod_rowow_hat_effect'");
    if (hatEffectTableResult)
    {
        if (QueryResult result = WorldDatabase.Query("SELECT `Name`, `SpellID` FROM `mod_rowow_hat_effect`"))
        {
            do
            {
                Field* fields = result->Fetch();
                uint32 spellID = fields[1].Get<uint32>();
                HatEffectSpellIDsByName[fields[0].Get<std::string>()] = spellID;
                HatEffectSpellIDs.insert(spellID);
            } while (result->NextRow());
        }
    }
    QueryResult optionLookTableResult = WorldDatabase.Query("SELECT 1 FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = 'mod_rowow_option_look'");
    if (optionLookTableResult)
    {
        if (QueryResult result = WorldDatabase.Query("SELECT `Kind`, `Look`, `SpellID` FROM `mod_rowow_option_look`"))
        {
            do
            {
                Field* fields = result->Fetch();
                uint32 spellID = fields[2].Get<uint32>();
                OptionLookSpellIDsByKey[GetOptionLookKey(fields[0].Get<uint8>(), fields[1].Get<uint32>())] = spellID;
                OptionLookSpellIDs.insert(spellID);
            } while (result->NextRow());
        }
    }
    LOG_INFO("module.ROWOW", "ROWOWMod::LoadIllusionData loaded {} RO illusions, {} item visuals, {} headgear auras, {} head auras, {} option look auras, {} script looks, "
        "{} garment auras and {} hat effect auras", IllusionsBySpellID.size(), ItemVisualsByItemID.size(), HeadgearSpellIDs.size(), HeadSpellIDs.size(), OptionLookSpellIDs.size(),
        ScriptLookSpellIDsByKey.size(), GarmentSpellIDs.size(), HatEffectSpellIDs.size());
}

ROWOWIllusion const* ROWOWMod::GetActiveIllusion(Player* player) const
{
    // The transform shown is the most recent one, which the core tracks by its spell
    uint32 transformSpellID = player->getTransForm();
    if (transformSpellID == 0)
        return nullptr;
    auto illusionIter = IllusionsBySpellID.find(transformSpellID);
    if (illusionIter == IllusionsBySpellID.end())
        return nullptr;
    return &illusionIter->second;
}

uint32 ROWOWMod::GetIllusionDisplayIDForPlayer(Player* player, ROWOWIllusion const& illusion) const
{
    // The weapon drawn while ready and attacking: in the RO form the RO weapon worn in the right hand, otherwise the main hand's, or with
    // none there the ranged slot's.  A weapon this job has no sprite for shows no weapon
    bool isInROForm = IsInROForm(player);
    uint32 weaponTypeID = 0;
    if (isInROForm == true)
    {
        ROWOWItem const* roWeapon = GetROItem(GetIdentityEquippedItem(player, ROWOW_EQUIP_POSITION_HAND_R));
        if (roWeapon != nullptr && roWeapon->IsWeapon == true && roWeapon->WeaponType < ROWOW_WEAPON_TYPE_MAX)
        {
            for (uint32 weaponSpriteTypeID : WeaponSpriteTypeIDsByWeaponType[roWeapon->WeaponType])
            {
                if (illusion.DisplayIDsByKey.find(ROWOWIllusion::GetIllusionDisplayKey(weaponSpriteTypeID, 0)) != illusion.DisplayIDsByKey.end())
                {
                    weaponTypeID = weaponSpriteTypeID;
                    break;
                }
            }
        }
    }
    else
    {
        Item* weaponItem = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
        if (weaponItem == nullptr || weaponItem->GetTemplate()->Class != ITEM_CLASS_WEAPON)
            weaponItem = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
        if (weaponItem != nullptr && weaponItem->GetTemplate()->Class == ITEM_CLASS_WEAPON)
        {
            auto weaponTypeIter = illusion.WeaponTypeIDsByItemSubclass.find(weaponItem->GetTemplate()->SubClass);
            if (weaponTypeIter != illusion.WeaponTypeIDsByItemSubclass.end())
                weaponTypeID = weaponTypeIter->second;
        }
    }

    // The shield drawn while ready: in the RO form the RO shield worn in the left hand, otherwise the off hand's.  An RO shield shows its
    // own type (guard, buckler, shield or mirror shield), and any other the plain shield, each falling back to the types this job has
    uint32 shieldTypeID = 0;
    Item* offHandItem = nullptr;
    bool isShield = false;
    if (isInROForm == true)
    {
        offHandItem = GetIdentityEquippedItem(player, ROWOW_EQUIP_POSITION_HAND_L);
        ROWOWItem const* roOffHand = GetROItem(offHandItem);
        isShield = roOffHand != nullptr && roOffHand->IsArmor == true;
    }
    else
    {
        offHandItem = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
        isShield = offHandItem != nullptr && offHandItem->GetTemplate()->Class == ITEM_CLASS_ARMOR && offHandItem->GetTemplate()->SubClass == ITEM_SUBCLASS_ARMOR_SHIELD;
    }
    if (isShield == true && illusion.ShieldTypeIDs.empty() == false)
    {
        uint32 wantedShieldTypeID = ROWOW_RO_SHIELD_TYPE_SHIELD;
        auto itemVisualIter = ItemVisualsByItemID.find(offHandItem->GetEntry());
        if (itemVisualIter != ItemVisualsByItemID.end() && itemVisualIter->second.VisualType == ROWOW_ITEM_VISUAL_TYPE_SHIELD
            && itemVisualIter->second.ViewID >= ROWOW_RO_SHIELD_TYPE_GUARD && itemVisualIter->second.ViewID <= ROWOW_RO_SHIELD_TYPE_MIRROR_SHIELD)
            wantedShieldTypeID = itemVisualIter->second.ViewID;
        uint32 candidateShieldTypeIDs[5] = { wantedShieldTypeID, ROWOW_RO_SHIELD_TYPE_SHIELD, ROWOW_RO_SHIELD_TYPE_GUARD, ROWOW_RO_SHIELD_TYPE_BUCKLER,
            ROWOW_RO_SHIELD_TYPE_MIRROR_SHIELD };
        for (uint32 candidateShieldTypeID : candidateShieldTypeIDs)
        {
            if (illusion.ShieldTypeIDs.find(candidateShieldTypeID) != illusion.ShieldTypeIDs.end())
            {
                shieldTypeID = candidateShieldTypeID;
                break;
            }
        }
    }

    // The display for the weapon and shield, falling back to holding nothing
    auto displayIter = illusion.DisplayIDsByKey.find(ROWOWIllusion::GetIllusionDisplayKey(weaponTypeID, shieldTypeID));
    if (displayIter != illusion.DisplayIDsByKey.end())
        return displayIter->second;
    displayIter = illusion.DisplayIDsByKey.find(ROWOWIllusion::GetIllusionDisplayKey(0, 0));
    if (displayIter != illusion.DisplayIDsByKey.end())
        return displayIter->second;
    return 0;
}

void ROWOWMod::RefreshIllusionDisplay(Player* player)
{
    if (IsEnabled == false || player == nullptr)
        return;
    ROWOWIllusion const* illusion = GetActiveIllusion(player);
    RefreshIllusionHead(player, illusion);
    RefreshIllusionHeadgear(player, illusion);
    RefreshIllusionOptionLooks(player, illusion);
    RefreshIllusionGarment(player, illusion);
    RefreshIllusionHatEffects(player, illusion);
    if (illusion == nullptr)
        return;

    // The illusion's displays share one sprite model and differ in their weapon and shield textures, which the client swaps on the
    // sprite shown
    uint32 displayID = GetIllusionDisplayIDForPlayer(player, *illusion);
    if (displayID != 0 && player->GetDisplayId() != displayID)
        player->SetDisplayId(displayID);
}

void ROWOWMod::RefreshIllusionHeadgear(Player* player, ROWOWIllusion const* illusion)
{
    if (HeadgearSpellIDs.empty() == true)
        return;

    // The headgear auras wanted: one for each headgear worn, while in an RO illusion.  The RO form shows its RO headgear (upper, middle
    // and lower, a costume over the one in its spot, and one covering several spots shown once, as its own spot), and otherwise the head,
    // neck and shoulder items (the head unless the helm is hidden)
    uint32 wantedSpellIDs[3] = { 0, 0, 0 };
    if (illusion != nullptr && IsInROForm(player) == true)
    {
        ROWOWIdentity* identity = GetIdentity(player);
        for (bool isCostume : { false, true })
        {
            for (auto const& itemGUIDByEquipPosition : identity->ItemGUIDsByEquipPosition)
            {
                uint32 headgearPositions = isCostume == true ? (ROWOW_EQUIP_POSITION_COSTUME_HEAD_TOP | ROWOW_EQUIP_POSITION_COSTUME_HEAD_MID | ROWOW_EQUIP_POSITION_COSTUME_HEAD_LOW)
                    : (ROWOW_EQUIP_POSITION_HEAD_TOP | ROWOW_EQUIP_POSITION_HEAD_MID | ROWOW_EQUIP_POSITION_HEAD_LOW);
                if ((itemGUIDByEquipPosition.first & headgearPositions) == 0)
                    continue;
                Item* item = player->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(itemGUIDByEquipPosition.second));
                if (item == nullptr)
                    continue;
                auto itemVisualIter = ItemVisualsByItemID.find(item->GetEntry());
                if (itemVisualIter == ItemVisualsByItemID.end() || itemVisualIter->second.VisualType < ROWOW_ITEM_VISUAL_TYPE_HEADGEAR_TOP
                    || itemVisualIter->second.VisualType > ROWOW_ITEM_VISUAL_TYPE_HEADGEAR_LOW)
                    continue;
                auto spellIter = HeadgearSpellIDsByKey.find(GetHeadgearKey(itemVisualIter->second.ViewID, itemVisualIter->second.VisualType, illusion->IsMale));
                if (spellIter != HeadgearSpellIDsByKey.end())
                    wantedSpellIDs[itemVisualIter->second.VisualType - ROWOW_ITEM_VISUAL_TYPE_HEADGEAR_TOP] = spellIter->second;
            }
        }
    }
    else if (illusion != nullptr && (IsWorldIdentityLookAura(player, illusion->SpellID) == true || GetWorldIdentityState(player)->LookSpellIDBeingAdded == illusion->SpellID))
    {
        // The Ragnarok identity shown out of the RO world (Nathan 2026-11-09): its Ragnarok Online character's headgear, as the form shows it.  Its
        // items are put away with the form's carried set (or on the character while the Ragnarok page shows them)
        ROWOWIdentity* identity = GetIdentity(player);
        for (bool isCostume : { false, true })
        {
            for (auto const& itemGUIDByEquipPosition : identity->ItemGUIDsByEquipPosition)
            {
                uint32 headgearPositions = isCostume == true ? (ROWOW_EQUIP_POSITION_COSTUME_HEAD_TOP | ROWOW_EQUIP_POSITION_COSTUME_HEAD_MID | ROWOW_EQUIP_POSITION_COSTUME_HEAD_LOW)
                    : (ROWOW_EQUIP_POSITION_HEAD_TOP | ROWOW_EQUIP_POSITION_HEAD_MID | ROWOW_EQUIP_POSITION_HEAD_LOW);
                if ((itemGUIDByEquipPosition.first & headgearPositions) == 0)
                    continue;
                Item* item = player->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(itemGUIDByEquipPosition.second));
                for (ROWOWStowedItem const& stowedItem : identity->StowedItems)
                    if (item == nullptr && stowedItem.StowedItem != nullptr && stowedItem.StowedItem->GetGUID().GetCounter() == itemGUIDByEquipPosition.second)
                        item = stowedItem.StowedItem;
                if (item == nullptr)
                    continue;
                auto itemVisualIter = ItemVisualsByItemID.find(item->GetEntry());
                if (itemVisualIter == ItemVisualsByItemID.end() || itemVisualIter->second.VisualType < ROWOW_ITEM_VISUAL_TYPE_HEADGEAR_TOP
                    || itemVisualIter->second.VisualType > ROWOW_ITEM_VISUAL_TYPE_HEADGEAR_LOW)
                    continue;
                auto spellIter = HeadgearSpellIDsByKey.find(GetHeadgearKey(itemVisualIter->second.ViewID, itemVisualIter->second.VisualType, illusion->IsMale));
                if (spellIter != HeadgearSpellIDsByKey.end())
                    wantedSpellIDs[itemVisualIter->second.VisualType - ROWOW_ITEM_VISUAL_TYPE_HEADGEAR_TOP] = spellIter->second;
            }
        }
    }
    else if (illusion != nullptr)
    {
        uint8 headgearSlots[3] = { EQUIPMENT_SLOT_HEAD, EQUIPMENT_SLOT_NECK, EQUIPMENT_SLOT_SHOULDERS };
        for (uint8 i = 0; i < 3; ++i)
        {
            if (headgearSlots[i] == EQUIPMENT_SLOT_HEAD && player->HasPlayerFlag(PLAYER_FLAGS_HIDE_HELM) == true)
                continue;
            Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, headgearSlots[i]);
            if (item == nullptr)
                continue;
            auto itemVisualIter = ItemVisualsByItemID.find(item->GetEntry());
            if (itemVisualIter == ItemVisualsByItemID.end() || itemVisualIter->second.VisualType < ROWOW_ITEM_VISUAL_TYPE_HEADGEAR_TOP)
                continue;
            auto spellIter = HeadgearSpellIDsByKey.find(GetHeadgearKey(itemVisualIter->second.ViewID, itemVisualIter->second.VisualType, illusion->IsMale));
            if (spellIter != HeadgearSpellIDsByKey.end())
                wantedSpellIDs[i] = spellIter->second;
        }
    }

    // Take off the headgear auras that aren't wanted (they're dummy auras, so only that list needs checking)
    std::vector<uint32> spellIDsToRemove;
    for (AuraEffect const* auraEffect : player->GetAuraEffectsByType(SPELL_AURA_DUMMY))
    {
        uint32 spellID = auraEffect->GetId();
        if (HeadgearSpellIDs.find(spellID) == HeadgearSpellIDs.end())
            continue;
        if (spellID != wantedSpellIDs[0] && spellID != wantedSpellIDs[1] && spellID != wantedSpellIDs[2])
            spellIDsToRemove.push_back(spellID);
    }
    for (uint32 spellID : spellIDsToRemove)
        player->RemoveAurasDueToSpell(spellID);

    // Put on the wanted ones
    for (uint8 i = 0; i < 3; ++i)
        if (wantedSpellIDs[i] != 0 && player->HasAura(wantedSpellIDs[i]) == false)
            player->AddAura(wantedSpellIDs[i], player);
}

void ROWOWMod::RefreshIllusionGarment(Player* player, ROWOWIllusion const* illusion)
{
    if (GarmentSpellIDs.empty() == true)
        return;

    // The garment aura wanted: in the RO form's illusion, the robe of the garment worn, a costume garment over the garment (RO's clif_changelook
    // LOOK_ROBE, the costume's view first)
    uint32 wantedSpellID = 0;
    if (illusion != nullptr && IsInROForm(player) == true)
    {
        for (uint32 garmentPosition : { ROWOW_EQUIP_POSITION_COSTUME_GARMENT, ROWOW_EQUIP_POSITION_GARMENT })
        {
            Item* item = GetIdentityEquippedItem(player, garmentPosition);
            if (item == nullptr)
                continue;
            auto itemVisualIter = ItemVisualsByItemID.find(item->GetEntry());
            if (itemVisualIter == ItemVisualsByItemID.end() || itemVisualIter->second.VisualType != ROWOW_ITEM_VISUAL_TYPE_GARMENT)
                continue;
            auto spellIter = GarmentSpellIDsByKey.find(GetGarmentKey(itemVisualIter->second.ViewID, illusion->IsMale));
            if (spellIter != GarmentSpellIDsByKey.end())
            {
                wantedSpellID = spellIter->second;
                break;
            }
        }
    }

    // Take off the garment auras that aren't wanted (dummy auras, like the headgear's), and put on the wanted one
    std::vector<uint32> spellIDsToRemove;
    for (AuraEffect const* auraEffect : player->GetAuraEffectsByType(SPELL_AURA_DUMMY))
    {
        uint32 spellID = auraEffect->GetId();
        if (spellID != wantedSpellID && GarmentSpellIDs.find(spellID) != GarmentSpellIDs.end())
            spellIDsToRemove.push_back(spellID);
    }
    for (uint32 spellID : spellIDsToRemove)
        player->RemoveAurasDueToSpell(spellID);
    if (wantedSpellID != 0 && player->HasAura(wantedSpellID) == false)
        player->AddAura(wantedSpellID, player);
}

void ROWOWMod::RefreshIllusionHatEffects(Player* player, ROWOWIllusion const* illusion)
{
    if (HatEffectSpellIDs.empty() == true)
        return;

    // The hat effect auras wanted: in the RO form's illusion, every hat effect of the items worn (rAthena's clif_hat_effects, from the
    // items' HatEffects)
    std::unordered_set<uint32> wantedSpellIDs;
    if (illusion != nullptr && IsInROForm(player) == true)
    {
        ROWOWIdentity* identity = GetIdentity(player);
        for (auto const& itemGUIDByEquipPosition : identity->ItemGUIDsByEquipPosition)
        {
            ROWOWItem const* roItem = GetROItem(player->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(itemGUIDByEquipPosition.second)));
            if (roItem == nullptr)
                continue;
            for (std::string const& hatEffectName : roItem->HatEffectNames)
            {
                auto spellIter = HatEffectSpellIDsByName.find(hatEffectName);
                if (spellIter != HatEffectSpellIDsByName.end())
                    wantedSpellIDs.insert(spellIter->second);
            }
        }
    }

    // Take off the hat effect auras that aren't wanted (dummy auras), and put on the wanted ones
    std::vector<uint32> spellIDsToRemove;
    for (AuraEffect const* auraEffect : player->GetAuraEffectsByType(SPELL_AURA_DUMMY))
    {
        uint32 spellID = auraEffect->GetId();
        if (HatEffectSpellIDs.find(spellID) != HatEffectSpellIDs.end() && wantedSpellIDs.find(spellID) == wantedSpellIDs.end())
            spellIDsToRemove.push_back(spellID);
    }
    for (uint32 spellID : spellIDsToRemove)
        player->RemoveAurasDueToSpell(spellID);
    for (uint32 spellID : wantedSpellIDs)
        if (player->HasAura(spellID) == false)
            player->AddAura(spellID, player);
}

uint32 ROWOWMod::GetHeadSpellID(uint32 style, uint32 color, bool isMale) const
{
    // A style or color with no head (rAthena allows style 0) shows the style's own colors, else the first style
    uint32 const candidateKeys[4] = { GetHeadKey(style, color, isMale), GetHeadKey(style, 0, isMale), GetHeadKey(ROWOW_HAIR_STYLE_DEFAULT, color, isMale),
        GetHeadKey(ROWOW_HAIR_STYLE_DEFAULT, 0, isMale) };
    for (uint32 candidateKey : candidateKeys)
    {
        auto spellIter = HeadSpellIDsByKey.find(candidateKey);
        if (spellIter != HeadSpellIDsByKey.end())
            return spellIter->second;
    }
    return 0;
}

void ROWOWMod::RefreshIllusionHead(Player* player, ROWOWIllusion const* illusion)
{
    if (HeadSpellIDs.empty() == true)
        return;

    // The head wanted: the identity's hair in the illusion's gender, while in an RO illusion (in the form or not; a character that never
    // entered the RO world has the identity's first hair, RO's style 1 in its own color)
    uint32 wantedSpellID = 0;
    if (illusion != nullptr)
    {
        ROWOWIdentity const* identity = GetIdentity(player);
        wantedSpellID = GetHeadSpellID(identity->HairStyle, identity->HairColor, illusion->IsMale);
    }

    // Take off the head auras that aren't wanted (dummy auras, like the headgear's), and put on the wanted one
    std::vector<uint32> spellIDsToRemove;
    for (AuraEffect const* auraEffect : player->GetAuraEffectsByType(SPELL_AURA_DUMMY))
    {
        uint32 spellID = auraEffect->GetId();
        if (spellID != wantedSpellID && HeadSpellIDs.find(spellID) != HeadSpellIDs.end())
            spellIDsToRemove.push_back(spellID);
    }
    for (uint32 spellID : spellIDsToRemove)
        player->RemoveAurasDueToSpell(spellID);
    if (wantedSpellID != 0 && player->HasAura(wantedSpellID) == false)
        player->AddAura(wantedSpellID, player);
}

uint32 ROWOWMod::GetCartLook(int32 cartType, uint32 baseLevel)
{
    // The cart RO draws: pre-renewal's carts change with the base level (1-40, 41-65, 66-80, 81-90, 91 and up), and every cart rAthena's
    // scripts hand out (setcart, the Kafras' rental) is type 1, so a type 1 cart shows the look of the owner's level.  Any other type (a
    // GM's .ro setcart, a script's setcart <type>) shows that look
    if (cartType != 1)
        return uint32(std::clamp<int32>(cartType, 1, ROWOW_MAX_CARTS));
    if (baseLevel <= 40)
        return 1;
    if (baseLevel <= 65)
        return 2;
    if (baseLevel <= 80)
        return 3;
    if (baseLevel <= 90)
        return 4;
    return 5;
}

void ROWOWMod::RefreshIllusionOptionLooks(Player* player, ROWOWIllusion const* illusion)
{
    if (OptionLookSpellIDs.empty() == true && OptionIconSpellIDsByOption.empty() == true)
        return;

    // The looks wanted: in the RO form's illusion, the cart in its look (SC_PUSH_CART's val1) and the falcon; with the riding and falcon
    // icons (pc_setoption's clif_status_load of EFST_RIDING and EFST_FALCON); and while devoted, the link from the Crusader (clif_devotion),
    // an aura the Crusader casts, so its beam goes from it
    uint32 wantedSpellIDs[4] = { 0, 0, 0, 0 };
    uint32 linkSpellID = 0;
    Unit* linkSource = nullptr;
    if (illusion != nullptr && IsInROForm(player) == true)
    {
        if (GetSCE(player, ROWOW_SC_DEVOTION) != nullptr)
        {
            auto spellIter = OptionLookSpellIDsByKey.find(GetOptionLookKey(ROWOW_OPTION_LOOK_DEVOTION_LINK, 1));
            linkSource = GetDevotionSource(player, false);
            if (spellIter != OptionLookSpellIDsByKey.end() && linkSource != nullptr && linkSource != player)
                linkSpellID = spellIter->second;
        }
        ROWOWIdentity const* identity = GetIdentity(player);
        // On a WoW mount the cart isn't shown (it hangs from the body, which sits on the mount's back, so it would trail in the air)
        ROWOWStatusChangeEntry const* cartSCE = player->IsMounted() == false ? GetSCE(player, ROWOW_SC_PUSH_CART) : nullptr;
        if (cartSCE != nullptr)
        {
            auto spellIter = OptionLookSpellIDsByKey.find(GetOptionLookKey(ROWOW_OPTION_LOOK_CART, GetCartLook(cartSCE->Val1, identity->BaseLevel)));
            if (spellIter != OptionLookSpellIDsByKey.end())
                wantedSpellIDs[0] = spellIter->second;
        }
        if (PCIsFalcon(player) == true)
        {
            auto spellIter = OptionLookSpellIDsByKey.find(GetOptionLookKey(ROWOW_OPTION_LOOK_FALCON, 1));
            if (spellIter != OptionLookSpellIDsByKey.end())
                wantedSpellIDs[1] = spellIter->second;
            auto iconIter = OptionIconSpellIDsByOption.find(ROWOW_OPTION_FALCON);
            if (iconIter != OptionIconSpellIDsByOption.end())
                wantedSpellIDs[2] = iconIter->second;
        }
        if (PCIsRiding(player) == true)
        {
            auto iconIter = OptionIconSpellIDsByOption.find(ROWOW_OPTION_RIDING);
            if (iconIter != OptionIconSpellIDsByOption.end())
                wantedSpellIDs[3] = iconIter->second;
        }
    }

    // Take off the ones not wanted (the looks are dummy auras; the icons are the status markers' aura type, looked up by spell) and put on
    // the wanted ones
    std::vector<uint32> spellIDsToRemove;
    bool hasLink = false;
    for (AuraEffect const* auraEffect : player->GetAuraEffectsByType(SPELL_AURA_DUMMY))
    {
        uint32 spellID = auraEffect->GetId();
        if (OptionLookSpellIDs.find(spellID) == OptionLookSpellIDs.end())
            continue;
        if (spellID == linkSpellID && linkSource != nullptr && auraEffect->GetCasterGUID() == linkSource->GetGUID())
            hasLink = true;
        else if (std::find(std::begin(wantedSpellIDs), std::end(wantedSpellIDs), spellID) == std::end(wantedSpellIDs))
            spellIDsToRemove.push_back(spellID);
    }
    for (auto const& iconSpellIDByOption : OptionIconSpellIDsByOption)
        if (std::find(std::begin(wantedSpellIDs), std::end(wantedSpellIDs), iconSpellIDByOption.second) == std::end(wantedSpellIDs) && player->HasAura(iconSpellIDByOption.second) == true)
            spellIDsToRemove.push_back(iconSpellIDByOption.second);
    for (uint32 spellID : spellIDsToRemove)
        player->RemoveAurasDueToSpell(spellID);
    if (linkSpellID != 0 && hasLink == false)
        linkSource->AddAura(linkSpellID, player); // Both on the player's map (GetDevotionSource), in its thread
    for (uint8 i = 0; i < 4; ++i)
    {
        if (wantedSpellIDs[i] == 0 || player->HasAura(wantedSpellIDs[i]) == true)
            continue;
        if (i < 2)
        {
            player->AddAura(wantedSpellIDs[i], player);
            continue;
        }

        // An icon lasts as long as the option, and isn't shown dead (as the statuses' ShowStatusChangeIcon)
        if (player->IsAlive() == false)
            continue;
        if (Aura* aura = player->AddAura(wantedSpellIDs[i], player))
        {
            aura->SetMaxDuration(-1);
            aura->SetDuration(-1);
        }
    }
}

bool ROWOWMod::PlayScriptLook(Unit* unit, uint8 kind, std::string const& name)
{
    // The unit casts the look's hidden spell on itself, so every client in sight plays it on the unit
    if (unit == nullptr || unit->IsInWorld() == false)
        return false;
    auto spellIter = ScriptLookSpellIDsByKey.find(GetScriptLookKey(kind, name));
    if (spellIter == ScriptLookSpellIDsByKey.end())
        return false;
    unit->CastSpell(unit, spellIter->second, true);
    return true;
}

bool ROWOWMod::PlayScriptSoundToPlayer(Player* player, std::string const& fileName)
{
    // The sound itself (not its spell), sent to the player alone, so a map-wide soundeffectall is heard once by each player wherever it is
    if (player == nullptr || player->IsInWorld() == false)
        return false;
    auto soundIter = ScriptLookSoundIDsByName.find(fileName);
    if (soundIter == ScriptLookSoundIDsByName.end())
        return false;
    player->PlayDirectSound(soundIter->second, player);
    return true;
}

bool ROWOWMod::ClifEmotion(Unit* unit, int32 type)
{
    // clif_emotion: the emotion (ET_*, the client's emotion sprite action) over the unit
    if (type < 0)
        return false;
    return PlayScriptLook(unit, ROWOW_SCRIPT_LOOK_EMOTION, std::to_string(type));
}

bool ROWOWMod::SetIdentityHair(Player* player, uint32 style, uint32 color)
{
    // The character's one head, every save slot's (Nathan 2026-11-09): kept with the character (at once when it has an identity, else with its first
    // one), shown at once on the illusion, and told to the addon's options
    if (HeadSpellIDsByKey.find(GetHeadKey(style, color, player->getGender() == GENDER_MALE)) == HeadSpellIDsByKey.end())
        return false;
    ROWOWIdentity* identity = GetIdentity(player);
    // Written only when it changed (or was never the shared head): the hair windows send it for every look tried
    bool isChanged = identity->HairStyle != uint8(style) || identity->HairColor != uint8(color) || identity->IsHairShared == false;
    identity->HairStyle = uint8(style);
    identity->HairColor = uint8(color);
    if (isChanged == true)
        SaveSharedHair(player);
    RefreshIllusionDisplay(player);
    SendHairToClient(player);
    return true;
}

void ROWOWMod::SaveSharedHair(Player* player)
{
    // The head changed (.ro hair, a stylist's setlook): the character's row, which every save slot reads its head from, and the slots' own copies.
    // Written now, not with the identity's save: the slot in use may have no identity to save.  With no identity in any slot there's no row yet (the
    // first identity's save makes it, with this head unless its creation picks another), and style 0 (rAthena's allows it) can't be told from no head,
    // so it stays the slot's own
    ROWOWIdentity* identity = GetIdentity(player);
    if (HasAnyIdentity(*identity) == false || identity->HairStyle == 0)
        return;
    identity->IsHairShared = true;
    CharacterDatabase.Execute("UPDATE `mod_rowow_identity_character` SET `hair_style` = {}, `hair_color` = {} WHERE `guid` = {}", identity->HairStyle, identity->HairColor,
        identity->CharacterGUID);
    std::string identityIDs;
    for (ROWOWIdentitySlot const& identitySlot : identity->Slots)
        if (identitySlot.IdentityID != 0)
            identityIDs += fmt::format("{}{}", identityIDs.empty() == true ? "" : ",", identitySlot.IdentityID);
    if (identityIDs.empty() == false)
        CharacterDatabase.Execute("UPDATE `mod_rowow_identity` SET `hair_style` = {}, `hair_color` = {} WHERE `guid` IN ({})", identity->HairStyle, identity->HairColor, identityIDs);
}

void ROWOWMod::SendHairToClient(Player* player)
{
    ROWOWIdentity const* identity = GetIdentity(player);
    std::string addonMessage = fmt::format("ROWOWHAIR\t{}\t{}", identity->HairStyle, identity->HairColor);
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_ADDON, nullptr, nullptr, addonMessage);
    player->SendDirectMessage(&data);
}
