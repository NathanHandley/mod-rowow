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

#ifndef ROWOW_H
#define ROWOW_H

#include "Common.h"
#include "MoveSplineInitArgs.h"

#include "DataMap.h"
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Creature;
class Player;

// Per creature template values from mod_rowow_creature_template
struct ROWOWCreatureTemplate
{
    float AttackRange = 0; // Yards a ranged attacker attacks from, 0 for melee
    uint32 AttackVisualSpellID = 0; // Visual-only spell cast on the target with each attack, 0 for none
};

// An RO illusion spell (from mod_rowow_illusion), with its display for each weapon and shield type (from mod_rowow_illusion_display).
// The displays share one sprite model, and differ in their weapon and shield textures
struct ROWOWIllusion
{
    uint32 SpellID = 0;
    uint32 FormCreatureTemplateID = 0;
    bool IsMale = true;
    std::unordered_map<uint32, uint32> DisplayIDsByKey; // See GetIllusionDisplayKey.  Weapon and shield type 0 are none
    std::unordered_map<uint32, uint32> WeaponTypeIDsByItemSubclass; // The RO weapon type shown for each WoW weapon subclass
    std::unordered_set<uint32> ShieldTypeIDs; // The RO shield types this job has

    static uint32 GetIllusionDisplayKey(uint32 weaponTypeID, uint32 shieldTypeID)
    {
        return ((weaponTypeID & 0xFFFF) << 16) | (shieldTypeID & 0xFFFF);
    }
};

// What an item draws on an RO illusion when worn (from mod_rowow_item_visual)
#define ROWOW_ITEM_VISUAL_TYPE_SHIELD               5

// RO shield types (an RO shield item's view, as the converter numbers them in mod_rowow_illusion_display)
#define ROWOW_RO_SHIELD_TYPE_GUARD                  1
#define ROWOW_RO_SHIELD_TYPE_BUCKLER                2
#define ROWOW_RO_SHIELD_TYPE_SHIELD                 3
#define ROWOW_RO_SHIELD_TYPE_MIRROR_SHIELD          4

#define ROWOW_ITEM_VISUAL_TYPE_HEADGEAR_TOP         2
#define ROWOW_ITEM_VISUAL_TYPE_HEADGEAR_MID         3
#define ROWOW_ITEM_VISUAL_TYPE_HEADGEAR_LOW         4
struct ROWOWItemVisual
{
    uint8 VisualType = 0;
    uint32 ViewID = 0; // RO headgear accessory ID or shield type
    uint32 VisualSpellID = 0; // Aura that attaches the headgear model, 0 for none
};

// What the RO skill script does for a skill spell (from mod_rowow_skill_effect)
#define ROWOW_SKILL_EFFECT_STATUS_CHANCE            1
#define ROWOW_SKILL_EFFECT_ZENY_COST                2
#define ROWOW_SKILL_EFFECT_STEAL                    3
#define ROWOW_SKILL_EFFECT_TELEPORT_RANDOM          4
#define ROWOW_SKILL_EFFECT_CELL_LAYOUT              5
#define ROWOW_SKILL_EFFECT_STEAL_ZENY               6
#define ROWOW_SKILL_EFFECT_TRAP                     7
struct ROWOWSkillEffect
{
    uint8 EffectType = 0;
    uint32 TriggerSpellID = 0; // Cast on the target when the chance succeeds
    float Chance = 0; // Percent
    float ChancePerCasterLevel = 0;
    bool IsChanceByLevelDifference = false; // Plus the caster's level less the target's
    int32 ValueA = 0; // Zeny cost (copper)
    int32 ValueB = 0;
};

// The cells an RO ground skill puts its cell spell on (from mod_rowow_skill_cell), as RO cell offsets from the target cell (x east,
// y north).  A wall's cells turn with RO's direction from the caster to the target (0 north, then counter-clockwise)
#define ROWOW_RO_DIRECTION_COUNT                    8
#define ROWOW_RO_DIRECTION_ANY                      255
#define ROWOW_RO_DIRECTION_EAST                     6
#define ROWOW_SKILL_CELL_GROUND_SEARCH_HEIGHT       3.0f    // How far above the target spot the ground search for a skill cell starts
#define ROWOW_SKILL_CELL_GROUND_SEARCH_DISTANCE     10.0f   // How far down it looks
struct ROWOWSkillCellLayout
{
    bool IsDirectional = false;
    std::vector<std::pair<int16, int16>> CellsByDirection[ROWOW_RO_DIRECTION_COUNT]; // Only the first is used when it isn't directional
};

// The RO cell grid of an RO map (from mod_rowow_map_grid).  RO's x runs against WoW's Y, and RO's y along WoW's X
struct ROWOWMapGrid
{
    float OriginX = 0; // WoW position of the corner of cell 0,0
    float OriginY = 0;
    float CellSize = 1.05f; // Yards

    int32 GetCellX(float worldY) const { return int32(std::floor((OriginY - worldY) / CellSize)); }
    int32 GetCellY(float worldX) const { return int32(std::floor((worldX - OriginX) / CellSize)); }
    float GetCellCenterWorldX(int32 cellY) const { return OriginX + (float(cellY) + 0.5f) * CellSize; }
    float GetCellCenterWorldY(int32 cellX) const { return OriginY - (float(cellX) + 0.5f) * CellSize; }
};

// Per creature: whether a player has stolen from it (RO allows one successful steal per monster)
struct ROWOWStealState : public DataMap::Base
{
    bool IsStolenFrom = false;
    bool IsZenyStolenFrom = false;
};

#define ROWOW_MOVE_MAX_PATH_NODES                   256     // Longest contoured path (in steps) built for one move
#define ROWOW_MOVE_LINE_OF_SIGHT_HEIGHT             1.0f    // Height above the ground that each step checks for a wall at

class ROWOWMod
{
private:
    ROWOWMod() {}

public:
    static ROWOWMod* instance()
    {
        static ROWOWMod instance;
        return &instance;
    }

    // Config
    bool IsEnabled = true;
    float ConfigMoveStepDistance = 1.0f;
    float ConfigMoveMaxRisePerStep = 1.2f;
    float ConfigMoveFloorSearchHeight = 3.0f;
    float ConfigMoveFloorSearchDistance = 30.0f;
    float ConfigRandomWalkMaxDistance = 7.35f;
    uint32 ConfigRandomWalkMinDelayInMS = 4000;
    uint32 ConfigRandomWalkMaxDelayInMS = 5000;
    uint32 ConfigChaseRepathIntervalInMS = 250;
    float ConfigChaseRepathTargetMoveDistance = 1.0f;
    uint32 ConfigGroundSnapCheckIntervalInMS = 1000;
    float ConfigGroundSnapTolerance = 0.15f;
    uint32 ConfigUnreachableAfterTimeInMS = 2000;
    uint32 ConfigOutOfReachGiveUpTimeInMS = 8000;
    float ConfigRangedAttackApproachFraction = 0.8f;
    bool ConfigCellPathing = true;
    float CellSize = 1.05f; // Yards in an RO cell (from the map grids), for placing skill cells off the RO maps

    // Loaded once at startup, and only read after that (so the map threads can read it without a lock)
    std::unordered_map<uint32, ROWOWCreatureTemplate> CreatureTemplatesByEntry;
    std::unordered_map<uint32, ROWOWIllusion> IllusionsBySpellID;
    std::unordered_map<uint32, ROWOWItemVisual> ItemVisualsByItemID;
    std::unordered_map<uint64, uint32> HeadgearSpellIDsByKey; // See GetHeadgearKey
    std::unordered_set<uint32> HeadgearSpellIDs;
    std::unordered_map<uint32, std::vector<ROWOWSkillEffect>> SkillEffectsBySpellID;
    std::unordered_map<uint32, ROWOWSkillCellLayout> SkillCellLayoutsBySpellID;
    std::unordered_map<uint32, ROWOWMapGrid> MapGridsByMapID;

    void LoadConfigurationFile();
    void LoadCreatureTemplateData();
    void LoadIllusionData();
    void LoadSkillEffectData();
    void LoadSkillCellData();
    void LoadMapGridData();

    // The RO cell grid of a map, or null when it isn't an RO map
    ROWOWMapGrid const* GetMapGrid(uint32 mapID) const;

    // The cells of a skill spell with a cell layout, or null if it has none
    ROWOWSkillCellLayout const* GetSkillCellLayout(uint32 spellID) const;

    // RO's direction from one cell to another (rAthena's map_calc_dir_xy), east when they're the same cell
    static uint8 GetRODirection(float deltaX, float deltaY);

    // The script parts of an RO skill spell, or null if it has none
    std::vector<ROWOWSkillEffect> const* GetSkillEffects(uint32 spellID) const;

    // RO illusions: while one is the player's transform, the player shows its display for the weapon and shield held
    ROWOWIllusion const* GetActiveIllusion(Player* player) const;
    uint32 GetIllusionDisplayIDForPlayer(Player* player, ROWOWIllusion const& illusion) const;
    void RefreshIllusionDisplay(Player* player);

    // RO illusions show the headgear worn in the head, neck and shoulder slots through hidden auras that attach the headgear models
    static uint64 GetHeadgearKey(uint32 viewID, uint8 visualType, bool isMale) { return (uint64(viewID) << 16) | (uint64(visualType) << 1) | (isMale == true ? 1 : 0); }
    void RefreshIllusionHeadgear(Player* player, ROWOWIllusion const* illusion);

    // The creature template's values, or null if it has none (a melee creature with no attack visual)
    ROWOWCreatureTemplate const* GetCreatureTemplate(uint32 creatureEntry) const;

    // Finds the ground under a point, searching down from a little above the reference height.  Returns false when there is no ground
    bool GetGroundZ(Creature* creature, float x, float y, float referenceZ, float& groundZOut);
    bool GetGroundZ(Creature* creature, float x, float y, float referenceZ, float searchHeight, float& groundZOut);

    // Builds a path from the creature toward a target, one small step at a time with every step pulled onto the ground, so the
    // creature follows the terrain like an RO monster does.  The path stops early at a wall, a cliff, or missing ground, and
    // stops short of the target by stopShortDistance.  Returns false if not even one step could be made
    bool BuildGroundContouredPath(Creature* creature, float targetX, float targetY, float stopShortDistance, Movement::PointsArray& pathNodesOut);

private:
    // On an RO map (with cell pathing on), the path goes from cell center to cell center as an RO monster walks: diagonally until
    // in line with the target's cell, then straight, stepping around what's in the way where it can
    bool BuildCellPath(Creature* creature, ROWOWMapGrid const& mapGrid, float targetX, float targetY, float stopShortDistance, Movement::PointsArray& pathNodesOut);

    // Whether a creature can step from one spot to another on the ground (no wall, cliff or missing ground), and the ground height there
    bool CanStepTo(Creature* creature, float priorX, float priorY, float priorZ, float stepX, float stepY, float stepDistance, float& stepZOut);

    // The ground height under a creature to start a path from
    float GetPathStartGroundZ(Creature* creature);
};

#define ROWOW ROWOWMod::instance()

#endif
