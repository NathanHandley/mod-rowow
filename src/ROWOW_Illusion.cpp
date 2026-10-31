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
            IllusionsBySpellID[illusion.SpellID] = illusion;
            IllusionSpellIDsByJobKey[GetIllusionJobKey(fields[3].Get<std::string>(), illusion.IsMale)] = illusion.SpellID;
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
    LOG_INFO("module.ROWOW", "ROWOWMod::LoadIllusionData loaded {} RO illusions, {} item visuals, {} headgear auras and {} head auras", IllusionsBySpellID.size(),
        ItemVisualsByItemID.size(), HeadgearSpellIDs.size(), HeadSpellIDs.size());
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

bool ROWOWMod::SetIdentityHair(Player* player, uint32 style, uint32 color)
{
    // Kept with the identity (saved with it once it exists), shown at once on the illusion, and told to the addon's options
    if (HeadSpellIDsByKey.find(GetHeadKey(style, color, player->getGender() == GENDER_MALE)) == HeadSpellIDsByKey.end())
        return false;
    ROWOWIdentity* identity = GetIdentity(player);
    identity->HairStyle = uint8(style);
    identity->HairColor = uint8(color);
    RefreshIllusionDisplay(player);
    SendHairToClient(player);
    return true;
}

void ROWOWMod::SendHairToClient(Player* player)
{
    ROWOWIdentity const* identity = GetIdentity(player);
    std::string addonMessage = fmt::format("ROWOWHAIR\t{}\t{}", identity->HairStyle, identity->HairColor);
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_ADDON, nullptr, nullptr, addonMessage);
    player->SendDirectMessage(&data);
}
