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
#include <unordered_map>
#include <unordered_set>

class Creature;
class Player;

// Per creature template values from mod_rowow_creature_template
struct ROWOWCreatureTemplate
{
    float AttackRange = 0; // Yards a ranged attacker attacks from, 0 for melee
    uint32 AttackVisualSpellID = 0; // Visual-only spell cast on the target with each attack, 0 for none
};

// An RO illusion spell (from mod_rowow_illusion), with its display for each garment (from mod_rowow_illusion_display)
struct ROWOWIllusion
{
    uint32 SpellID = 0;
    uint32 FormCreatureTemplateID = 0;
    bool IsMale = true;
    std::unordered_map<uint64, uint32> DisplayIDsByKey; // See GetIllusionDisplayKey.  Garment, weapon and shield type 0 are none
    std::unordered_map<uint32, uint32> WeaponTypeIDsByItemSubclass; // The RO weapon type shown for each WoW weapon subclass
    std::unordered_set<uint32> ShieldTypeIDs; // The RO shield types this job has
    std::unordered_map<uint32, uint32> GarmentViewIDsByDisplayID; // Displays with the same garment share a model (they differ in weapon and shield textures)

    static uint64 GetIllusionDisplayKey(uint32 garmentViewID, uint32 weaponTypeID, uint32 shieldTypeID)
    {
        return (uint64(garmentViewID) << 32) | (uint64(weaponTypeID & 0xFFFF) << 16) | (shieldTypeID & 0xFFFF);
    }
};

// What an item draws on an RO illusion when worn (from mod_rowow_item_visual)
#define ROWOW_ITEM_VISUAL_TYPE_GARMENT              1
#define ROWOW_ITEM_VISUAL_TYPE_SHIELD               5

// RO shield types (an RO shield item's view, as the converter numbers them in mod_rowow_illusion_display)
#define ROWOW_RO_SHIELD_TYPE_GUARD                  1
#define ROWOW_RO_SHIELD_TYPE_BUCKLER                2
#define ROWOW_RO_SHIELD_TYPE_SHIELD                 3
#define ROWOW_RO_SHIELD_TYPE_MIRROR_SHIELD          4

// An invisible creature display, shown for a moment between two RO illusion displays of the same model (see RefreshIllusionDisplay)
#define ROWOW_INVISIBLE_DISPLAY_ID                  11686
#define ROWOW_INVISIBLE_DISPLAY_HOLD_MS             100

// Per player: when an RO illusion's display went invisible on its way to another of the same model (0 if it isn't)
struct ROWOWIllusionDisplayState : public DataMap::Base
{
    uint64 InvisibleSinceMS = 0;
};
#define ROWOW_ITEM_VISUAL_TYPE_HEADGEAR_TOP         2
#define ROWOW_ITEM_VISUAL_TYPE_HEADGEAR_MID         3
#define ROWOW_ITEM_VISUAL_TYPE_HEADGEAR_LOW         4
struct ROWOWItemVisual
{
    uint8 VisualType = 0;
    uint32 ViewID = 0; // RO garment sprite ID or headgear accessory ID
    uint32 VisualSpellID = 0; // Aura that attaches the headgear model, 0 for garments
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

    // Loaded once at startup, and only read after that (so the map threads can read it without a lock)
    std::unordered_map<uint32, ROWOWCreatureTemplate> CreatureTemplatesByEntry;
    std::unordered_map<uint32, ROWOWIllusion> IllusionsBySpellID;
    std::unordered_map<uint32, ROWOWItemVisual> ItemVisualsByItemID;
    std::unordered_map<uint64, uint32> HeadgearSpellIDsByKey; // See GetHeadgearKey
    std::unordered_set<uint32> HeadgearSpellIDs;

    void LoadConfigurationFile();
    void LoadCreatureTemplateData();
    void LoadIllusionData();

    // RO illusions: while one is the player's transform, the player shows its display for the garment worn on their back
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
};

#define ROWOW ROWOWMod::instance()

#endif
