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
#include "DatabaseEnvFwd.h"
#include "MoveSplineInitArgs.h"
#include "ObjectGuid.h"
#include "SharedDefines.h"

#include "DataMap.h"
#include <cmath>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Creature;
class Item;
class Player;
class SpellInfo;
class Unit;

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

// A copy of one Ragnarok WorldData table, the rAthena pre-renewal data the RO rules read (from a mod_rowow_db_* table, which the converter
// replaces whole).  Rows are in rAthena's load order and every cell is text, empty for none.  A cell is a scalar, a map ("Key=Value,..."),
// a list of maps ("Key=Value,...;Key=Value,...;") or a list of scalars ("A;B;"), escaped as the converter's RoWorldDataTable does
struct ROWOWDataTable
{
    std::string TableName;
    std::vector<std::string> ColumnNames;
    std::unordered_map<std::string, int32> ColumnIndexByName;
    std::vector<std::vector<std::string>> Rows;

    // The column's index, or -1 when the table has no such column (rAthena never set that field)
    int32 GetColumnIndex(std::string const& columnName) const;
    std::string const& GetCell(size_t rowIndex, int32 columnIndex) const;
    int64 GetInt(size_t rowIndex, int32 columnIndex, int64 defaultValue) const;

    static std::string UnescapeText(std::string const& text);
    static std::vector<std::pair<std::string, std::string>> ParseMap(std::string const& cell);
    static std::vector<std::vector<std::pair<std::string, std::string>>> ParseMapList(std::string const& cell);
    static std::vector<std::string> ParseTextList(std::string const& cell);
};

#define ROWOW_DATA_TABLE_PREFIX                     "mod_rowow_db_"
#define ROWOW_DATA_TABLE_MOB                        "mod_rowow_db_mob"
#define ROWOW_DATA_TABLE_JOB                        "mod_rowow_db_job"
#define ROWOW_DATA_TABLE_STATPOINT                  "mod_rowow_db_statpoint"
#define ROWOW_DATA_TABLE_CHAR_CONFIG                "mod_rowow_db_char_config"

// The fixed RO spells (200000-200099, the converter's RO_SPELL_ID_* values)
#define ROWOW_SPELL_WORLD_TRAVEL                    200000  // Every character's 8 second cast into the RO world and back
#define ROWOW_SPELL_ID_RO_START                     200000  // RO spells are 200000-248999
#define ROWOW_SPELL_ID_RO_END                       248999
#define ROWOW_SPELL_AUTO_ATTACK                     6603    // WoW's Attack, which the RO form keeps for auto attack

// rAthena's job IDs (e_job in mmo.hpp)
#define ROWOW_JOB_NOVICE                            0
#define ROWOW_JOB_UPPER_START                       4001    // Novice_High, the first transcendent job
#define ROWOW_JOB_UPPER_END                         4022    // Paladin2, the last

// RO equipment positions (rAthena's EQP_* bits, which item_db's Locations and start_items_pre use)
#define ROWOW_EQUIP_POSITION_HAND_R                 0x0002  // Weapon
#define ROWOW_EQUIP_POSITION_HAND_L                 0x0020  // Shield

#define ROWOW_STAT_COUNT                            6       // STR, AGI, VIT, INT, DEX, LUK
#define ROWOW_IDENTITY_ACTION_BUTTON_COUNT          144     // MAX_ACTION_BUTTONS

// A pre-renewal job: its rAthena name and the values of rAthena's job database it has (from mod_rowow_db_job)
struct ROWOWJob
{
    uint16 JobID = 0;
    std::string Name; // rAthena's (like Swordman_High), the job database's key
    std::string IllusionJobKey; // The RO illusion of the job (mod_rowow_illusion JobKey, like swordman)
    bool IsUpper = false; // Transcendent (rAthena's JOBL_UPPER)
    uint32 MaxBaseLevel = 99;
    uint32 MaxJobLevel = 10;
    std::vector<uint32> BaseHPByLevel; // Index is the base level less 1
    std::vector<uint32> BaseSPByLevel;
    std::vector<uint64> BaseExpByLevel; // The exp from each level to the next
    std::vector<uint64> JobExpByLevel;
};

// A position in the world (an RO identity's save point, last RO position and WoW return position)
struct ROWOWPosition
{
    bool IsSet = false;
    uint32 MapID = 0;
    float X = 0;
    float Y = 0;
    float Z = 0;
    float O = 0;
};

// An item a new RO identity starts with (rAthena's start_items_pre)
struct ROWOWStartItem
{
    uint32 ItemTemplateID = 0;
    uint32 Amount = 1;
    uint32 EquipPosition = 0; // ROWOW_EQUIP_POSITION_*, 0 for none
};

// A player's RO identity (mod_rowow_identity and the tables beside it), kept on the player while logged in.  Only the player's own
// thread (its session's and map's updates) touches it
struct ROWOWIdentity : public DataMap::Base
{
    bool IsLoaded = false;
    bool Exists = false; // Made on the first entry into the RO world
    uint16 JobID = ROWOW_JOB_NOVICE;
    uint32 BaseLevel = 1;
    uint32 BaseExp = 0;
    uint32 JobLevel = 1;
    uint32 JobExp = 0;
    uint32 StatusPoints = 0;
    uint32 SkillPoints = 0;
    uint32 Stats[ROWOW_STAT_COUNT] = { 1, 1, 1, 1, 1, 1 };
    uint32 HP = 0;
    uint32 SP = 0;
    uint32 Zeny = 0;
    ROWOWPosition SavePoint;
    ROWOWPosition LastPosition; // Where the identity last left the RO world, which the world travel spell goes back to
    ROWOWPosition ReturnPosition; // Where the character entered the RO world from, which the world travel spell returns to
    uint8 HairStyle = 1;
    uint8 HairColor = 0;
    std::map<uint8, uint32> ActionsByButton; // The RO form's own action bars (packed action and type, as SMSG_ACTION_BUTTONS sends them)
    bool AreActionsChanged = false;
    std::map<uint32, ObjectGuid::LowType> ItemGUIDsByEquipPosition; // The RO equipment worn
    bool IsEquipmentChanged = false;

    bool WasInFormAtLoad = false; // Saved in the form: logged out in the RO world, or the server stopped with the character in it

    // While in the form: the character's own values, put back on leaving and saved in place of the form's
    bool IsInForm = false;
    uint8 WoWLevel = 0;
    uint32 WoWXP = 0;
    uint32 WoWMoney = 0;
    uint32 WoWHealth = 0;
    uint32 WoWPowers[MAX_POWERS] = { };
    uint8 WoWPowerType = 0;
    std::vector<uint32> HiddenSpellIDs; // The character's spells the client was told to forget for the form
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
    std::unordered_map<std::string, ROWOWDataTable> DataTablesByName; // The rAthena data, by table name (ROWOW_DATA_TABLE_*)
    std::unordered_map<uint32, uint32> MobIDsByCreatureEntry; // The RO monster (mob_db Id) behind each RO creature template
    std::unordered_map<uint32, size_t> MobRowIndexesByMobID; // Rows of the mob table
    std::unordered_map<uint16, ROWOWJob> JobsByID;
    std::unordered_map<uint32, uint32> StatusPointsByBaseLevel; // Total status points at each base level (rAthena's statpoint.yml)
    std::unordered_map<std::string, uint32> MapIDsByMapName; // RO map names (rAthena's, including its aliases), lowercase
    std::unordered_map<uint32, uint32> ItemTemplateIDsByItemID; // The item template of each converted RO item (item_db Id)
    std::unordered_set<uint32> ItemTemplateIDs; // Every RO item template
    std::unordered_map<uint64, uint32> IllusionSpellIDsByJobKey; // See GetIllusionJobKey
    uint32 StartMapID = 0; // Where a new RO identity starts (rAthena's start_point_pre), 0 when unknown
    int32 StartCellX = 0;
    int32 StartCellY = 0;
    std::vector<ROWOWStartItem> StartItems;
    uint32 StartZeny = 0;

    void LoadConfigurationFile();
    void LoadDataTables();
    void LoadCreatureMobData();
    void LoadIdentityData();
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

    // A table of rAthena data (ROWOW_DATA_TABLE_*), or null when the converter hasn't written it
    ROWOWDataTable const* GetDataTable(std::string const& tableName) const;

    // RO combatants: an RO monster, or a player in the RO identity form.  Combat between RO combatants follows RO's rules (mod-rowow's
    // port of rAthena's pre-renewal battle code), not WoW's
    bool IsROCombatant(Unit const* unit) const;
    bool IsROMap(uint32 mapID) const { return MapGridsByMapID.find(mapID) != MapGridsByMapID.end(); }

    // The RO monster (mob_db Id) a creature is, or 0 when it isn't an RO monster (RO town NPCs aren't)
    uint32 GetROMobID(Creature const* creature) const;

    // The mob table row of an RO monster, or -1 when it has none
    int64 GetMobRowIndex(uint32 mobID) const;

    // Whether a player is in the RO identity form, the only way a player is in the RO world
    bool IsInROForm(Player const* player) const;

    // RO identity: one per character, which the character turns into in the RO world (its job's look, level, HP/SP, zeny, spells and
    // action bars) and back out of when leaving it.  The character row keeps the character's own values throughout
    static uint64 GetIllusionJobKey(std::string const& jobKey, bool isMale);
    ROWOWJob const* GetJob(uint16 jobID) const;
    uint32 GetMapIDByMapName(std::string const& mapName) const;
    bool IsROItemTemplate(uint32 itemTemplateID) const { return ItemTemplateIDs.find(itemTemplateID) != ItemTemplateIDs.end(); }

    // The middle of an RO cell of an RO map, on the ground.  False when the map has no cell grid or no ground there
    bool GetCellPosition(uint32 mapID, int32 cellX, int32 cellY, ROWOWPosition& positionOut) const;

    // The player's identity, loaded on first use (a row of defaults until the identity exists)
    ROWOWIdentity* GetIdentity(Player* player);
    void SaveIdentity(Player* player, CharacterDatabaseTransaction trans);
    uint32 GetIdentityMaxHP(ROWOWIdentity const& identity) const;
    uint32 GetIdentityMaxSP(ROWOWIdentity const& identity) const;

    // Turning into the form and back, on arriving in and leaving the RO world (and at login)
    void EnterROForm(Player* player);
    void LeaveROForm(Player* player);
    void RefreshROForm(Player* player); // After the identity's job or level changes
    void RemoveROFormAuras(Player* player); // The job's look and its headgear
    bool IsSpellKeptInROForm(SpellInfo const* spellInfo) const;

    // The world travel spell: into the RO world (to where the identity last left it, or the start point), or back to where the
    // character came from
    bool GetWorldTravelDestination(Player* player, ROWOWPosition& destinationOut);

    // An RO death's "return to save point": back to life at the save point with full HP and SP
    void RespawnAtSavePoint(Player* player);

    // Tells the client's RO interface (the RO_Identity addon) whether the player is in the form, under the ROWOWFORM prefix
    void SendROFormStateToClient(Player* player) const;

    // Tells the client to forget the character's spells and shows the form's action bars.  A far teleport sends the character's own
    // again (Player::SendInitialPacketsBeforeAddToMap), so this follows every arrival on a new map in the form
    void SendROFormClientSpellsAndActions(Player* player, ROWOWIdentity const& identity) const;

    // Puts the form's action bars on the client and keeps the changes the client makes to them
    void SendIdentityActionButtons(Player* player, ROWOWIdentity const& identity) const;
    void SetIdentityActionButton(Player* player, uint8 button, uint32 packedAction);

    // The RO item worn in an RO equipment position, or null
    Item* GetIdentityEquippedItem(Player* player, uint32 equipPosition) const;

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
