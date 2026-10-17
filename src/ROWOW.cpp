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

#include "Configuration/Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "QueryResult.h"
#include "GridTerrainData.h"
#include "Log.h"
#include "Map.h"
#include "GameTime.h"
#include "Player.h"
#include "SpellAuraEffects.h"
#include "ModelIgnoreFlags.h"

#include "ROWOW.h"

#include <cmath>

using namespace std;

void ROWOWMod::LoadConfigurationFile()
{
    IsEnabled = sConfigMgr->GetOption<bool>("ROWOW.Enabled", true);
    if (IsEnabled == false)
    {
        LOG_INFO("module.ROWOW", "ROWOWMod::LoadConfigurationFile has ROWOW.Enabled as false, so the module is deactivated");
        return;
    }

    ConfigMoveStepDistance = sConfigMgr->GetOption<float>("ROWOW.Creature.MoveStepDistance", 1.0f);
    if (ConfigMoveStepDistance < 0.25f)
        ConfigMoveStepDistance = 0.25f;
    ConfigMoveMaxRisePerStep = sConfigMgr->GetOption<float>("ROWOW.Creature.MoveMaxRisePerStep", 1.2f);
    ConfigMoveFloorSearchHeight = sConfigMgr->GetOption<float>("ROWOW.Creature.MoveFloorSearchHeight", 3.0f);
    ConfigMoveFloorSearchDistance = sConfigMgr->GetOption<float>("ROWOW.Creature.MoveFloorSearchDistance", 30.0f);
    ConfigRandomWalkMaxDistance = sConfigMgr->GetOption<float>("ROWOW.Creature.RandomWalkMaxDistance", 7.35f);
    ConfigRandomWalkMinDelayInMS = sConfigMgr->GetOption<uint32>("ROWOW.Creature.RandomWalkMinDelayInMS", 4000);
    ConfigRandomWalkMaxDelayInMS = sConfigMgr->GetOption<uint32>("ROWOW.Creature.RandomWalkMaxDelayInMS", 5000);
    if (ConfigRandomWalkMaxDelayInMS < ConfigRandomWalkMinDelayInMS)
        ConfigRandomWalkMaxDelayInMS = ConfigRandomWalkMinDelayInMS;
    ConfigChaseRepathIntervalInMS = sConfigMgr->GetOption<uint32>("ROWOW.Creature.ChaseRepathIntervalInMS", 250);
    ConfigChaseRepathTargetMoveDistance = sConfigMgr->GetOption<float>("ROWOW.Creature.ChaseRepathTargetMoveDistance", 1.0f);
    ConfigGroundSnapCheckIntervalInMS = sConfigMgr->GetOption<uint32>("ROWOW.Creature.GroundSnapCheckIntervalInMS", 1000);
    ConfigGroundSnapTolerance = sConfigMgr->GetOption<float>("ROWOW.Creature.GroundSnapTolerance", 0.15f);
    ConfigUnreachableAfterTimeInMS = sConfigMgr->GetOption<uint32>("ROWOW.Creature.UnreachableAfterTimeInMS", 2000);
    ConfigOutOfReachGiveUpTimeInMS = sConfigMgr->GetOption<uint32>("ROWOW.Creature.OutOfReachGiveUpTimeInMS", 8000);
    ConfigRangedAttackApproachFraction = sConfigMgr->GetOption<float>("ROWOW.Creature.RangedAttackApproachFraction", 0.8f);
    if (ConfigRangedAttackApproachFraction < 0.1f)
        ConfigRangedAttackApproachFraction = 0.1f;
    if (ConfigRangedAttackApproachFraction > 1.0f)
        ConfigRangedAttackApproachFraction = 1.0f;
    ConfigCellPathing = sConfigMgr->GetOption<bool>("ROWOW.Creature.CellPathing", true);

    // Event intervals of zero would make the AI's event loop run the same event over and over in one update
    if (ConfigRandomWalkMinDelayInMS < 100)
        ConfigRandomWalkMinDelayInMS = 100;
    if (ConfigRandomWalkMaxDelayInMS < ConfigRandomWalkMinDelayInMS)
        ConfigRandomWalkMaxDelayInMS = ConfigRandomWalkMinDelayInMS;
    if (ConfigChaseRepathIntervalInMS < 50)
        ConfigChaseRepathIntervalInMS = 50;
    if (ConfigGroundSnapCheckIntervalInMS < 100)
        ConfigGroundSnapCheckIntervalInMS = 100;
    if (ConfigOutOfReachGiveUpTimeInMS < 1000)
        ConfigOutOfReachGiveUpTimeInMS = 1000;
}

void ROWOWMod::LoadCreatureTemplateData()
{
    CreatureTemplatesByEntry.clear();
    if (IsEnabled == false)
        return;

    // The converter creates the table, so it may not exist yet on a server that hasn't had the ROWOW SQL applied
    QueryResult tableResult = WorldDatabase.Query("SELECT 1 FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = 'mod_rowow_creature_template'");
    if (!tableResult)
    {
        LOG_INFO("module.ROWOW", "ROWOWMod::LoadCreatureTemplateData found no mod_rowow_creature_template table, so every creature is a melee attacker");
        return;
    }
    QueryResult result = WorldDatabase.Query("SELECT `CreatureTemplateID`, `AttackRange`, `AttackVisualSpellID` FROM `mod_rowow_creature_template`");
    if (!result)
        return;
    do
    {
        Field* fields = result->Fetch();
        ROWOWCreatureTemplate creatureTemplate;
        creatureTemplate.AttackRange = fields[1].Get<float>();
        creatureTemplate.AttackVisualSpellID = fields[2].Get<uint32>();
        CreatureTemplatesByEntry[fields[0].Get<uint32>()] = creatureTemplate;
    } while (result->NextRow());
    LOG_INFO("module.ROWOW", "ROWOWMod::LoadCreatureTemplateData loaded {} creature templates", CreatureTemplatesByEntry.size());
}

void ROWOWMod::LoadSkillEffectData()
{
    SkillEffectsBySpellID.clear();
    if (IsEnabled == false)
        return;

    // The converter creates the table, so it may not exist yet on a server that hasn't had the ROWOW SQL applied
    QueryResult tableResult = WorldDatabase.Query("SELECT 1 FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = 'mod_rowow_skill_effect'");
    if (!tableResult)
    {
        LOG_INFO("module.ROWOW", "ROWOWMod::LoadSkillEffectData found no mod_rowow_skill_effect table, so RO skills have no scripted parts");
        return;
    }
    QueryResult result = WorldDatabase.Query("SELECT `SpellID`, `EffectType`, `TriggerSpellID`, `Chance`, `ChancePerCasterLevel`, `IsChanceByLevelDifference`, `ValueA`, `ValueB` FROM `mod_rowow_skill_effect` ORDER BY `SpellID`, `EffectIndex`");
    if (!result)
        return;
    uint32 effectCount = 0;
    do
    {
        Field* fields = result->Fetch();
        ROWOWSkillEffect skillEffect;
        skillEffect.EffectType = fields[1].Get<uint8>();
        skillEffect.TriggerSpellID = fields[2].Get<uint32>();
        skillEffect.Chance = fields[3].Get<float>();
        skillEffect.ChancePerCasterLevel = fields[4].Get<float>();
        skillEffect.IsChanceByLevelDifference = fields[5].Get<uint8>() != 0;
        skillEffect.ValueA = fields[6].Get<int32>();
        skillEffect.ValueB = fields[7].Get<int32>();
        SkillEffectsBySpellID[fields[0].Get<uint32>()].push_back(skillEffect);
        effectCount++;
    } while (result->NextRow());
    LOG_INFO("module.ROWOW", "ROWOWMod::LoadSkillEffectData loaded {} skill script parts for {} spells", effectCount, SkillEffectsBySpellID.size());
}

void ROWOWMod::LoadSkillCellData()
{
    SkillCellLayoutsBySpellID.clear();
    if (IsEnabled == false)
        return;

    // The converter creates the table, so it may not exist yet on a server that hasn't had the ROWOW SQL applied
    QueryResult tableResult = WorldDatabase.Query("SELECT 1 FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = 'mod_rowow_skill_cell'");
    if (!tableResult)
    {
        LOG_INFO("module.ROWOW", "ROWOWMod::LoadSkillCellData found no mod_rowow_skill_cell table, so RO ground skills have no cell layouts");
        return;
    }
    QueryResult result = WorldDatabase.Query("SELECT `SpellID`, `Direction`, `OffsetX`, `OffsetY` FROM `mod_rowow_skill_cell` ORDER BY `SpellID`, `Direction`, `CellIndex`");
    if (!result)
        return;
    uint32 cellCount = 0;
    do
    {
        Field* fields = result->Fetch();
        uint8 direction = fields[1].Get<uint8>();
        ROWOWSkillCellLayout& cellLayout = SkillCellLayoutsBySpellID[fields[0].Get<uint32>()];
        if (direction == ROWOW_RO_DIRECTION_ANY)
            direction = 0;
        else if (direction < ROWOW_RO_DIRECTION_COUNT)
            cellLayout.IsDirectional = true;
        else
        {
            LOG_ERROR("module.ROWOW", "ROWOWMod::LoadSkillCellData has a cell with direction {} for spell {}, which isn't a direction", direction, fields[0].Get<uint32>());
            continue;
        }
        cellLayout.CellsByDirection[direction].emplace_back(fields[2].Get<int16>(), fields[3].Get<int16>());
        cellCount++;
    } while (result->NextRow());
    LOG_INFO("module.ROWOW", "ROWOWMod::LoadSkillCellData loaded {} skill cells for {} spells", cellCount, SkillCellLayoutsBySpellID.size());
}

void ROWOWMod::LoadMapGridData()
{
    MapGridsByMapID.clear();
    if (IsEnabled == false)
        return;

    // The converter creates the table, so it may not exist yet on a server that hasn't had the ROWOW SQL applied
    QueryResult tableResult = WorldDatabase.Query("SELECT 1 FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = 'mod_rowow_map_grid'");
    if (!tableResult)
    {
        LOG_INFO("module.ROWOW", "ROWOWMod::LoadMapGridData found no mod_rowow_map_grid table, so no map has an RO cell grid");
        return;
    }
    QueryResult result = WorldDatabase.Query("SELECT `MapID`, `OriginX`, `OriginY`, `CellSize` FROM `mod_rowow_map_grid`");
    if (!result)
        return;
    do
    {
        Field* fields = result->Fetch();
        ROWOWMapGrid mapGrid;
        mapGrid.OriginX = fields[1].Get<float>();
        mapGrid.OriginY = fields[2].Get<float>();
        mapGrid.CellSize = fields[3].Get<float>();
        if (mapGrid.CellSize < 0.1f)
        {
            LOG_ERROR("module.ROWOW", "ROWOWMod::LoadMapGridData has a cell size of {} for map {}, so it's skipped", mapGrid.CellSize, fields[0].Get<uint32>());
            continue;
        }
        CellSize = mapGrid.CellSize;
        MapGridsByMapID[fields[0].Get<uint32>()] = mapGrid;
    } while (result->NextRow());
    LOG_INFO("module.ROWOW", "ROWOWMod::LoadMapGridData loaded the cell grids of {} maps", MapGridsByMapID.size());
}

ROWOWMapGrid const* ROWOWMod::GetMapGrid(uint32 mapID) const
{
    auto mapGridIter = MapGridsByMapID.find(mapID);
    if (mapGridIter == MapGridsByMapID.end())
        return nullptr;
    return &mapGridIter->second;
}

ROWOWSkillCellLayout const* ROWOWMod::GetSkillCellLayout(uint32 spellID) const
{
    auto cellLayoutIter = SkillCellLayoutsBySpellID.find(spellID);
    if (cellLayoutIter == SkillCellLayoutsBySpellID.end())
        return nullptr;
    return &cellLayoutIter->second;
}

uint8 ROWOWMod::GetRODirection(float deltaX, float deltaY)
{
    // RO's directions: 0 north, 1 north-west, 2 west, 3 south-west, 4 south, 5 south-east, 6 east, 7 north-east (x east, y north)
    if (deltaX == 0 && deltaY == 0)
        return ROWOW_RO_DIRECTION_EAST;
    if (deltaX >= 0 && deltaY >= 0)
    {
        if (deltaX >= deltaY * 3)
            return 6;
        if (deltaX * 3 < deltaY)
            return 0;
        return 7;
    }
    if (deltaX >= 0 && deltaY <= 0)
    {
        if (deltaX >= -deltaY * 3)
            return 6;
        if (deltaX * 3 < -deltaY)
            return 4;
        return 5;
    }
    if (deltaX <= 0 && deltaY <= 0)
    {
        if (deltaX * 3 >= deltaY)
            return 4;
        if (deltaX < deltaY * 3)
            return 2;
        return 3;
    }
    if (-deltaX * 3 <= deltaY)
        return 0;
    if (-deltaX > deltaY * 3)
        return 2;
    return 1;
}

std::vector<ROWOWSkillEffect> const* ROWOWMod::GetSkillEffects(uint32 spellID) const
{
    auto skillEffectsIter = SkillEffectsBySpellID.find(spellID);
    if (skillEffectsIter == SkillEffectsBySpellID.end())
        return nullptr;
    return &skillEffectsIter->second;
}

void ROWOWMod::LoadIllusionData()
{
    IllusionsBySpellID.clear();
    ItemVisualsByItemID.clear();
    if (IsEnabled == false)
        return;

    // The converter creates these tables, so they may not exist yet on a server that hasn't had the ROWOW SQL applied
    QueryResult tableResult = WorldDatabase.Query("SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name IN ('mod_rowow_illusion', 'mod_rowow_illusion_display', 'mod_rowow_item_visual')");
    if (!tableResult || tableResult->Fetch()[0].Get<uint64>() < 3)
    {
        LOG_INFO("module.ROWOW", "ROWOWMod::LoadIllusionData found no RO illusion tables, so RO illusions show without garments");
        return;
    }
    if (QueryResult result = WorldDatabase.Query("SELECT `SpellID`, `FormCreatureTemplateID`, `IsMale` FROM `mod_rowow_illusion`"))
    {
        do
        {
            Field* fields = result->Fetch();
            ROWOWIllusion illusion;
            illusion.SpellID = fields[0].Get<uint32>();
            illusion.FormCreatureTemplateID = fields[1].Get<uint32>();
            illusion.IsMale = fields[2].Get<uint8>() != 0;
            IllusionsBySpellID[illusion.SpellID] = illusion;
        } while (result->NextRow());
    }
    if (QueryResult result = WorldDatabase.Query("SELECT `SpellID`, `GarmentViewID`, `WeaponTypeID`, `ShieldTypeID`, `DisplayID` FROM `mod_rowow_illusion_display`"))
    {
        do
        {
            Field* fields = result->Fetch();
            auto illusionIter = IllusionsBySpellID.find(fields[0].Get<uint32>());
            if (illusionIter == IllusionsBySpellID.end())
                continue;
            uint32 shieldTypeID = fields[3].Get<uint32>();
            illusionIter->second.DisplayIDsByKey[ROWOWIllusion::GetIllusionDisplayKey(fields[1].Get<uint32>(), fields[2].Get<uint32>(), shieldTypeID)] = fields[4].Get<uint32>();
            illusionIter->second.GarmentViewIDsByDisplayID[fields[4].Get<uint32>()] = fields[1].Get<uint32>();
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
    LOG_INFO("module.ROWOW", "ROWOWMod::LoadIllusionData loaded {} RO illusions, {} item visuals and {} headgear auras", IllusionsBySpellID.size(), ItemVisualsByItemID.size(), HeadgearSpellIDs.size());
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
    // The garment on the back, unless the cloak is hidden.  A garment this job has no sprite for shows no garment, as in RO
    uint32 garmentViewID = 0;
    if (player->HasPlayerFlag(PLAYER_FLAGS_HIDE_CLOAK) == false)
    {
        if (Item* backItem = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_BACK))
        {
            auto itemVisualIter = ItemVisualsByItemID.find(backItem->GetEntry());
            if (itemVisualIter != ItemVisualsByItemID.end() && itemVisualIter->second.VisualType == ROWOW_ITEM_VISUAL_TYPE_GARMENT)
                garmentViewID = itemVisualIter->second.ViewID;
        }
    }
    // The weapon drawn while ready and attacking: the main hand's, or with none there the ranged slot's.  A weapon this job has no
    // sprite for shows no weapon
    uint32 weaponTypeID = 0;
    Item* weaponItem = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    if (weaponItem == nullptr || weaponItem->GetTemplate()->Class != ITEM_CLASS_WEAPON)
        weaponItem = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
    if (weaponItem != nullptr && weaponItem->GetTemplate()->Class == ITEM_CLASS_WEAPON)
    {
        auto weaponTypeIter = illusion.WeaponTypeIDsByItemSubclass.find(weaponItem->GetTemplate()->SubClass);
        if (weaponTypeIter != illusion.WeaponTypeIDsByItemSubclass.end())
            weaponTypeID = weaponTypeIter->second;
    }

    // The shield drawn while ready: the off hand's.  An RO shield shows its own type (guard, buckler, shield or mirror shield), and any
    // other the plain shield, each falling back to the types this job has
    uint32 shieldTypeID = 0;
    Item* offHandItem = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
    if (offHandItem != nullptr && offHandItem->GetTemplate()->Class == ITEM_CLASS_ARMOR && offHandItem->GetTemplate()->SubClass == ITEM_SUBCLASS_ARMOR_SHIELD
        && illusion.ShieldTypeIDs.empty() == false)
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

    // The display for the garment, weapon and shield, falling back to no garment
    uint64 candidateKeys[2] = { ROWOWIllusion::GetIllusionDisplayKey(garmentViewID, weaponTypeID, shieldTypeID),
        ROWOWIllusion::GetIllusionDisplayKey(0, weaponTypeID, shieldTypeID) };
    for (uint64 candidateKey : candidateKeys)
    {
        auto displayIter = illusion.DisplayIDsByKey.find(candidateKey);
        if (displayIter != illusion.DisplayIDsByKey.end())
            return displayIter->second;
    }
    auto displayIter = illusion.DisplayIDsByKey.find(ROWOWIllusion::GetIllusionDisplayKey(0, 0, 0));
    if (displayIter != illusion.DisplayIDsByKey.end())
        return displayIter->second;
    return 0;
}

void ROWOWMod::RefreshIllusionDisplay(Player* player)
{
    if (IsEnabled == false || player == nullptr)
        return;
    ROWOWIllusion const* illusion = GetActiveIllusion(player);
    RefreshIllusionHeadgear(player, illusion);
    if (illusion == nullptr)
        return;
    uint32 displayID = GetIllusionDisplayIDForPlayer(player, *illusion);
    uint32 currentDisplayID = player->GetDisplayId();
    ROWOWIllusionDisplayState* displayState = player->CustomData.GetDefault<ROWOWIllusionDisplayState>("ROWOWIllusionDisplayState");
    if (displayID == 0 || currentDisplayID == displayID)
    {
        displayState->InvisibleSinceMS = 0;
        return;
    }

    // Displays of the same model differ only in their weapon and shield textures, which the client doesn't reload when the display
    // changes to another of the same model, even one update after an invisible display.  So the change goes through the invisible
    // display held long enough for the client to have swapped models
    uint64 nowMS = GameTime::GetGameTimeMS().count();
    if (currentDisplayID == ROWOW_INVISIBLE_DISPLAY_ID && displayState->InvisibleSinceMS != 0)
    {
        if (nowMS - displayState->InvisibleSinceMS < ROWOW_INVISIBLE_DISPLAY_HOLD_MS)
            return;
        displayState->InvisibleSinceMS = 0;
        player->SetDisplayId(displayID);
        return;
    }
    auto currentGarmentIter = illusion->GarmentViewIDsByDisplayID.find(currentDisplayID);
    auto wantedGarmentIter = illusion->GarmentViewIDsByDisplayID.find(displayID);
    if (currentGarmentIter != illusion->GarmentViewIDsByDisplayID.end() && wantedGarmentIter != illusion->GarmentViewIDsByDisplayID.end()
        && currentGarmentIter->second == wantedGarmentIter->second)
    {
        displayState->InvisibleSinceMS = nowMS;
        player->SetDisplayId(ROWOW_INVISIBLE_DISPLAY_ID);
    }
    else
        player->SetDisplayId(displayID);
}

void ROWOWMod::RefreshIllusionHeadgear(Player* player, ROWOWIllusion const* illusion)
{
    if (HeadgearSpellIDs.empty() == true)
        return;

    // The headgear auras wanted: one for each headgear worn (the top one unless the helm is hidden), while in an RO illusion
    uint32 wantedSpellIDs[3] = { 0, 0, 0 };
    if (illusion != nullptr)
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

ROWOWCreatureTemplate const* ROWOWMod::GetCreatureTemplate(uint32 creatureEntry) const
{
    auto creatureTemplateIter = CreatureTemplatesByEntry.find(creatureEntry);
    if (creatureTemplateIter == CreatureTemplatesByEntry.end())
        return nullptr;
    return &creatureTemplateIter->second;
}

bool ROWOWMod::GetGroundZ(Creature* creature, float x, float y, float referenceZ, float& groundZOut)
{
    return GetGroundZ(creature, x, y, referenceZ, ConfigMoveFloorSearchHeight, groundZOut);
}

bool ROWOWMod::GetGroundZ(Creature* creature, float x, float y, float referenceZ, float searchHeight, float& groundZOut)
{
    float groundZ = creature->GetMap()->GetHeight(creature->GetPhaseMask(), x, y, referenceZ + searchHeight, true, ConfigMoveFloorSearchDistance);
    if (groundZ <= INVALID_HEIGHT)
        return false;
    groundZOut = groundZ;
    return true;
}

bool ROWOWMod::BuildGroundContouredPath(Creature* creature, float targetX, float targetY, float stopShortDistance, Movement::PointsArray& pathNodesOut)
{
    pathNodesOut.clear();

    if (ConfigCellPathing == true)
    {
        ROWOWMapGrid const* mapGrid = GetMapGrid(creature->GetMapId());
        if (mapGrid != nullptr)
            return BuildCellPath(creature, *mapGrid, targetX, targetY, stopShortDistance, pathNodesOut);
    }

    float startX = creature->GetPositionX();
    float startY = creature->GetPositionY();
    float deltaX = targetX - startX;
    float deltaY = targetY - startY;
    float distance = std::sqrt(deltaX * deltaX + deltaY * deltaY);
    float travelDistance = distance - stopShortDistance;
    if (distance < 0.01f || travelDistance < 0.1f)
        return false;
    float directionX = deltaX / distance;
    float directionY = deltaY / distance;

    // Step along the line, pulling each step onto the ground
    uint32 stepCount = uint32(std::ceil(travelDistance / ConfigMoveStepDistance));
    if (stepCount > ROWOW_MOVE_MAX_PATH_NODES)
        stepCount = ROWOW_MOVE_MAX_PATH_NODES;
    float stepDistance = travelDistance / float(stepCount);
    float startZ = GetPathStartGroundZ(creature);
    pathNodesOut.emplace_back(startX, startY, creature->GetPositionZ());
    float priorX = startX;
    float priorY = startY;
    float priorZ = startZ;
    for (uint32 stepIndex = 1; stepIndex <= stepCount; ++stepIndex)
    {
        float stepX = startX + directionX * stepDistance * float(stepIndex);
        float stepY = startY + directionY * stepDistance * float(stepIndex);
        float stepZ = 0;
        if (CanStepTo(creature, priorX, priorY, priorZ, stepX, stepY, stepDistance, stepZ) == false)
            break;
        pathNodesOut.emplace_back(stepX, stepY, stepZ);
        priorX = stepX;
        priorY = stepY;
        priorZ = stepZ;
    }

    if (pathNodesOut.size() < 2)
    {
        pathNodesOut.clear();
        return false;
    }
    return true;
}

bool ROWOWMod::BuildCellPath(Creature* creature, ROWOWMapGrid const& mapGrid, float targetX, float targetY, float stopShortDistance, Movement::PointsArray& pathNodesOut)
{
    float startX = creature->GetPositionX();
    float startY = creature->GetPositionY();
    int32 cellX = mapGrid.GetCellX(startY);
    int32 cellY = mapGrid.GetCellY(startX);
    int32 targetCellX = mapGrid.GetCellX(targetY);
    int32 targetCellY = mapGrid.GetCellY(targetX);
    float startZ = GetPathStartGroundZ(creature);
    pathNodesOut.emplace_back(startX, startY, creature->GetPositionZ());

    // Already in the target's cell: a creature off the cell's center (after a knockback or a fear) steps onto it
    float priorX = startX;
    float priorY = startY;
    float priorZ = startZ;
    if (cellX == targetCellX && cellY == targetCellY)
    {
        float cellCenterX = mapGrid.GetCellCenterWorldX(cellY);
        float cellCenterY = mapGrid.GetCellCenterWorldY(cellX);
        float centerDistance = std::sqrt((cellCenterX - startX) * (cellCenterX - startX) + (cellCenterY - startY) * (cellCenterY - startY));
        float centerZ = 0;
        if (centerDistance > 0.1f && CanStepTo(creature, priorX, priorY, priorZ, cellCenterX, cellCenterY, centerDistance, centerZ) == true)
            pathNodesOut.emplace_back(cellCenterX, cellCenterY, centerZ);
    }

    // Diagonal steps while the target's cell is off in both directions, then straight.  A blocked step tries the steps to either side of
    // it (still toward the target), never going back onto a cell already stepped on
    std::vector<std::pair<int32, int32>> visitedCells;
    visitedCells.emplace_back(cellX, cellY);
    while (pathNodesOut.size() < ROWOW_MOVE_MAX_PATH_NODES)
    {
        if (cellX == targetCellX && cellY == targetCellY)
            break;
        float nodeDeltaX = targetX - priorX;
        float nodeDeltaY = targetY - priorY;
        if (stopShortDistance > 0 && pathNodesOut.size() > 1 && std::sqrt(nodeDeltaX * nodeDeltaX + nodeDeltaY * nodeDeltaY) <= stopShortDistance)
            break;
        int32 stepCellX = targetCellX > cellX ? 1 : (targetCellX < cellX ? -1 : 0);
        int32 stepCellY = targetCellY > cellY ? 1 : (targetCellY < cellY ? -1 : 0);
        std::pair<int32, int32> candidateSteps[3];
        candidateSteps[0] = std::make_pair(stepCellX, stepCellY);
        if (stepCellX != 0 && stepCellY != 0)
        {
            candidateSteps[1] = std::make_pair(stepCellX, 0);
            candidateSteps[2] = std::make_pair(0, stepCellY);
        }
        else if (stepCellX != 0)
        {
            candidateSteps[1] = std::make_pair(stepCellX, 1);
            candidateSteps[2] = std::make_pair(stepCellX, -1);
        }
        else
        {
            candidateSteps[1] = std::make_pair(1, stepCellY);
            candidateSteps[2] = std::make_pair(-1, stepCellY);
        }
        bool hasStepped = false;
        for (uint8 candidateIndex = 0; candidateIndex < 3; ++candidateIndex)
        {
            int32 nextCellX = cellX + candidateSteps[candidateIndex].first;
            int32 nextCellY = cellY + candidateSteps[candidateIndex].second;
            bool isVisited = false;
            for (std::pair<int32, int32> const& visitedCell : visitedCells)
            {
                if (visitedCell.first == nextCellX && visitedCell.second == nextCellY)
                {
                    isVisited = true;
                    break;
                }
            }
            if (isVisited == true)
                continue;
            float stepX = mapGrid.GetCellCenterWorldX(nextCellY);
            float stepY = mapGrid.GetCellCenterWorldY(nextCellX);
            float stepDistance = std::sqrt((stepX - priorX) * (stepX - priorX) + (stepY - priorY) * (stepY - priorY));
            float stepZ = 0;
            if (CanStepTo(creature, priorX, priorY, priorZ, stepX, stepY, stepDistance, stepZ) == false)
                continue;

            // As in RO, a diagonal step needs both cells beside it open, so it can't squeeze between two blocked corners
            if (candidateSteps[candidateIndex].first != 0 && candidateSteps[candidateIndex].second != 0)
            {
                float sideAX = mapGrid.GetCellCenterWorldX(cellY);
                float sideAY = mapGrid.GetCellCenterWorldY(nextCellX);
                float sideBX = mapGrid.GetCellCenterWorldX(nextCellY);
                float sideBY = mapGrid.GetCellCenterWorldY(cellX);
                float sideDistanceA = std::sqrt((sideAX - priorX) * (sideAX - priorX) + (sideAY - priorY) * (sideAY - priorY));
                float sideDistanceB = std::sqrt((sideBX - priorX) * (sideBX - priorX) + (sideBY - priorY) * (sideBY - priorY));
                float sideZ = 0;
                if (CanStepTo(creature, priorX, priorY, priorZ, sideAX, sideAY, sideDistanceA, sideZ) == false
                    || CanStepTo(creature, priorX, priorY, priorZ, sideBX, sideBY, sideDistanceB, sideZ) == false)
                    continue;
            }
            pathNodesOut.emplace_back(stepX, stepY, stepZ);
            visitedCells.emplace_back(nextCellX, nextCellY);
            cellX = nextCellX;
            cellY = nextCellY;
            priorX = stepX;
            priorY = stepY;
            priorZ = stepZ;
            hasStepped = true;
            break;
        }
        if (hasStepped == false)
            break;
    }

    if (pathNodesOut.size() < 2)
    {
        pathNodesOut.clear();
        return false;
    }
    return true;
}

bool ROWOWMod::CanStepTo(Creature* creature, float priorX, float priorY, float priorZ, float stepX, float stepY, float stepDistance, float& stepZOut)
{
    // Search from a step's climb above the prior step, so a low arch or deck overhead isn't taken for the ground
    float stepZ = 0;
    if (GetGroundZ(creature, stepX, stepY, priorZ, ConfigMoveMaxRisePerStep * stepDistance + 0.5f, stepZ) == false)
        return false;

    // Too steep up or down is a wall or a cliff, which RO wouldn't let a monster walk over
    if (std::fabs(stepZ - priorZ) > ConfigMoveMaxRisePerStep * stepDistance)
        return false;

    // Something solid in the way (a building or a fence)
    if (creature->GetMap()->isInLineOfSight(priorX, priorY, priorZ + ROWOW_MOVE_LINE_OF_SIGHT_HEIGHT, stepX, stepY, stepZ + ROWOW_MOVE_LINE_OF_SIGHT_HEIGHT,
        creature->GetPhaseMask(), LINEOFSIGHT_CHECK_VMAP, VMAP::ModelIgnoreFlags::Nothing) == false)
        return false;
    stepZOut = stepZ;
    return true;
}

float ROWOWMod::GetPathStartGroundZ(Creature* creature)
{
    // Steps are compared against the ground under the creature, not its own height, since other movement (a fear, a knockback)
    // can leave it a little off the ground.  Ground far above it is an overhang (a deck, a table), not what it stands on
    float startX = creature->GetPositionX();
    float startY = creature->GetPositionY();
    float startZ = creature->GetPositionZ();
    float startGroundZ = startZ;
    if (GetGroundZ(creature, startX, startY, startZ, startGroundZ) == false)
        return startZ;
    if (startGroundZ > startZ + ConfigMoveMaxRisePerStep)
    {
        if (GetGroundZ(creature, startX, startY, startZ, 0.5f, startGroundZ) == false)
            return startZ;
    }
    return startGroundZ;
}
