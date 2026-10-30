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
#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Creature;
class Item;
class Player;
class SpellInfo;
class Unit;
struct CalcDamageInfo;
struct ROWOWScript;

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
#define ROWOW_DATA_TABLE_ITEM                       "mod_rowow_db_item"
#define ROWOW_DATA_TABLE_ITEM_RENEWAL               "mod_rowow_db_item_renewal"
#define ROWOW_DATA_TABLE_ITEM_GROUP                 "mod_rowow_db_item_group"
#define ROWOW_DATA_TABLE_JOB                        "mod_rowow_db_job"
#define ROWOW_DATA_TABLE_STATPOINT                  "mod_rowow_db_statpoint"
#define ROWOW_DATA_TABLE_MAP_FLAG                   "mod_rowow_db_map_flag"
#define ROWOW_DATA_TABLE_BATTLE_CONFIG              "mod_rowow_db_battle_config"
#define ROWOW_DATA_TABLE_CHAR_CONFIG                "mod_rowow_db_char_config"
#define ROWOW_DATA_TABLE_ATTR_FIX                   "mod_rowow_db_attr_fix"
#define ROWOW_DATA_TABLE_SIZE_FIX                   "mod_rowow_db_size_fix"

// The fixed RO spells (200000-200099, the converter's RO_SPELL_ID_* values)
#define ROWOW_SPELL_WORLD_TRAVEL                    200000  // Every character's 8 second cast into the RO world and back
#define ROWOW_SPELL_BASE_LEVEL_UP                   200001  // RO's base level up effect (the angel and levelup.wav)
#define ROWOW_SPELL_JOB_LEVEL_UP                    200002  // RO's job level up effect
#define ROWOW_SPELL_WEAPON_HIT_SOUND_START          200003  // RO's weapon hit sounds, 200003-200010 (see GetWeaponHitSoundSpellID)
#define ROWOW_SPELL_USE_ITEM                        200011  // What every usable RO item casts when used, which runs the item's RO script
#define ROWOW_SPELL_ID_FIXED_END                    200099  // The fixed RO spells end here (generated ones follow)
#define ROWOW_SPELL_ID_RO_START                     200000  // RO spells are 200000-248999
#define ROWOW_SPELL_ID_RO_END                       248999
#define ROWOW_SPELL_AUTO_ATTACK                     6603    // WoW's Attack, which the RO form keeps for auto attack

// rAthena's job IDs (e_job in mmo.hpp)
#define ROWOW_JOB_NOVICE                            0
#define ROWOW_JOB_NOVICE_HIGH                       4001
#define ROWOW_JOB_UPPER_START                       4001    // Novice_High, the first transcendent job
#define ROWOW_JOB_UPPER_END                         4022    // Paladin2, the last

// RO equipment positions (rAthena's EQP_* bits, which item_db's Locations and start_items_pre use)
#define ROWOW_EQUIP_POSITION_HEAD_LOW               0x0001
#define ROWOW_EQUIP_POSITION_HAND_R                 0x0002  // Weapon
#define ROWOW_EQUIP_POSITION_GARMENT                0x0004
#define ROWOW_EQUIP_POSITION_ACC_L                  0x0008
#define ROWOW_EQUIP_POSITION_ARMOR                  0x0010
#define ROWOW_EQUIP_POSITION_HAND_L                 0x0020  // Shield
#define ROWOW_EQUIP_POSITION_SHOES                  0x0040
#define ROWOW_EQUIP_POSITION_ACC_R                  0x0080
#define ROWOW_EQUIP_POSITION_HEAD_TOP               0x0100
#define ROWOW_EQUIP_POSITION_HEAD_MID               0x0200
#define ROWOW_EQUIP_POSITION_COSTUME_HEAD_TOP       0x0400  // The costume positions only change the look (renewal's, for the post-renewal looks)
#define ROWOW_EQUIP_POSITION_COSTUME_HEAD_MID       0x0800
#define ROWOW_EQUIP_POSITION_COSTUME_HEAD_LOW       0x1000
#define ROWOW_EQUIP_POSITION_COSTUME_GARMENT        0x2000
#define ROWOW_EQUIP_POSITION_AMMO                   0x8000
#define ROWOW_EQUIP_POSITION_ARMS                   (ROWOW_EQUIP_POSITION_HAND_R | ROWOW_EQUIP_POSITION_HAND_L)
#define ROWOW_EQUIP_POSITION_ACC                    (ROWOW_EQUIP_POSITION_ACC_L | ROWOW_EQUIP_POSITION_ACC_R)
#define ROWOW_EQUIP_POSITION_COSTUME                (ROWOW_EQUIP_POSITION_COSTUME_HEAD_TOP | ROWOW_EQUIP_POSITION_COSTUME_HEAD_MID | ROWOW_EQUIP_POSITION_COSTUME_HEAD_LOW | ROWOW_EQUIP_POSITION_COSTUME_GARMENT)

// What pc_isequip says of equipping an item (e_ITEM_EQUIP_ACK)
#define ROWOW_EQUIP_ACK_OK                          0
#define ROWOW_EQUIP_ACK_FAIL                        1
#define ROWOW_EQUIP_ACK_FAIL_LEVEL                  2

// An RO item's own state (refine level, broken, unidentified) is in its permanent enchantment: the converter's fixed enchantments from
// 8000 (RO_DBCID_SPELLITEMENCHANTMENT_ITEM_STATE_*), 8000 plus the refine level, plus 16 when broken, plus 32 when unidentified
#define ROWOW_ITEM_STATE_ENCHANTMENT_START          8000
#define ROWOW_ITEM_STATE_REFINE_MASK                0x0F
#define ROWOW_ITEM_STATE_BROKEN                     16
#define ROWOW_ITEM_STATE_UNIDENTIFIED               32
#define ROWOW_ITEM_STATE_COUNT                      64

// RO weapon types (rAthena's weapon_type in pc.hpp)
#define ROWOW_WEAPON_TYPE_FIST                      0
#define ROWOW_WEAPON_TYPE_DAGGER                    1
#define ROWOW_WEAPON_TYPE_1HSWORD                   2
#define ROWOW_WEAPON_TYPE_2HSWORD                   3
#define ROWOW_WEAPON_TYPE_1HSPEAR                   4
#define ROWOW_WEAPON_TYPE_2HSPEAR                   5
#define ROWOW_WEAPON_TYPE_1HAXE                     6
#define ROWOW_WEAPON_TYPE_2HAXE                     7
#define ROWOW_WEAPON_TYPE_MACE                      8
#define ROWOW_WEAPON_TYPE_2HMACE                    9
#define ROWOW_WEAPON_TYPE_STAFF                     10
#define ROWOW_WEAPON_TYPE_BOW                       11
#define ROWOW_WEAPON_TYPE_KNUCKLE                   12
#define ROWOW_WEAPON_TYPE_MUSICAL                   13
#define ROWOW_WEAPON_TYPE_WHIP                      14
#define ROWOW_WEAPON_TYPE_BOOK                      15
#define ROWOW_WEAPON_TYPE_KATAR                     16
#define ROWOW_WEAPON_TYPE_REVOLVER                  17
#define ROWOW_WEAPON_TYPE_RIFLE                     18
#define ROWOW_WEAPON_TYPE_GATLING                   19
#define ROWOW_WEAPON_TYPE_SHOTGUN                   20
#define ROWOW_WEAPON_TYPE_GRENADE                   21
#define ROWOW_WEAPON_TYPE_2HSTAFF                   23
#define ROWOW_WEAPON_TYPE_MAX                       24      // MAX_WEAPON_TYPE; the dual wield types follow
#define ROWOW_WEAPON_TYPE_DOUBLE_DD                 25      // Two daggers
#define ROWOW_WEAPON_TYPE_DOUBLE_SS                 26      // Two swords
#define ROWOW_WEAPON_TYPE_DOUBLE_AA                 27      // Two axes
#define ROWOW_WEAPON_TYPE_DOUBLE_DS                 28      // Dagger and sword
#define ROWOW_WEAPON_TYPE_DOUBLE_DA                 29      // Dagger and axe
#define ROWOW_WEAPON_TYPE_DOUBLE_SA                 30      // Sword and axe

// The six stats, in rAthena's order (PARAM_STR..PARAM_LUK)
#define ROWOW_STAT_STR                              0
#define ROWOW_STAT_AGI                              1
#define ROWOW_STAT_VIT                              2
#define ROWOW_STAT_INT                              3
#define ROWOW_STAT_DEX                              4
#define ROWOW_STAT_LUK                              5
#define ROWOW_STAT_COUNT                            6
#define ROWOW_STAT_MAX_RAISE                        99      // The most one raise asks for (the stat cap stops it sooner)
#define ROWOW_IDENTITY_ACTION_BUTTON_COUNT          144     // MAX_ACTION_BUTTONS
#define ROWOW_ATTACK_TYPE_COUNT                     3       // MAX_ATTACK (the main hand, off hand and ranged attack times)

// RO elements, sizes, races and monster classes (rAthena's e_element, e_size, e_race and e_classAE)
#define ROWOW_ELEMENT_NEUTRAL                       0
#define ROWOW_ELEMENT_WATER                         1
#define ROWOW_ELEMENT_EARTH                         2
#define ROWOW_ELEMENT_FIRE                          3
#define ROWOW_ELEMENT_WIND                          4
#define ROWOW_ELEMENT_POISON                        5
#define ROWOW_ELEMENT_HOLY                          6
#define ROWOW_ELEMENT_DARK                          7
#define ROWOW_ELEMENT_GHOST                         8
#define ROWOW_ELEMENT_UNDEAD                        9
#define ROWOW_ELEMENT_COUNT                         10      // ELE_ALL
#define ROWOW_ELEMENT_LEVEL_MAX                     4
#define ROWOW_SIZE_SMALL                            0
#define ROWOW_SIZE_MEDIUM                           1
#define ROWOW_SIZE_BIG                              2
#define ROWOW_SIZE_COUNT                            3
#define ROWOW_RACE_FORMLESS                         0
#define ROWOW_RACE_UNDEAD                           1
#define ROWOW_RACE_BRUTE                            2
#define ROWOW_RACE_PLANT                            3
#define ROWOW_RACE_INSECT                           4
#define ROWOW_RACE_FISH                             5
#define ROWOW_RACE_DEMON                            6
#define ROWOW_RACE_DEMIHUMAN                        7
#define ROWOW_RACE_ANGEL                            8
#define ROWOW_RACE_DRAGON                           9
#define ROWOW_RACE_PLAYER_HUMAN                     10
#define ROWOW_RACE_PLAYER_DORAM                     11
#define ROWOW_CLASS_NORMAL                          0
#define ROWOW_CLASS_BOSS                            1
#define ROWOW_CLASS_GUARDIAN                        2
#define ROWOW_CLASS_BATTLEFIELD                     3
#define ROWOW_CLASS_EVENT                           4

// The monster modes the battle rules read so far (rAthena's e_mode bits).  The AI modes come with the monster AI (R10)
#define ROWOW_MODE_IGNORE_MELEE                     0x0010000
#define ROWOW_MODE_IGNORE_MAGIC                     0x0020000
#define ROWOW_MODE_IGNORE_RANGED                    0x0040000
#define ROWOW_MODE_MVP                              0x0080000
#define ROWOW_MODE_IGNORE_MISC                      0x0100000

// rAthena's object types (BL_*), as the battle config values name them
#define ROWOW_BL_PC                                 0x0001
#define ROWOW_BL_MOB                                0x0002

// An attack's kind and reach (rAthena's e_battle_flag), how the client shows it (e_damage_type), and what came of it (damage_lv)
#define ROWOW_BF_WEAPON                             0x0001
#define ROWOW_BF_MAGIC                              0x0002
#define ROWOW_BF_MISC                               0x0004
#define ROWOW_BF_SHORT                              0x0010
#define ROWOW_BF_LONG                               0x0040
#define ROWOW_BF_SKILL                              0x0100
#define ROWOW_BF_NORMAL                             0x0200
#define ROWOW_DMG_NORMAL                            0
#define ROWOW_DMG_MULTI_HIT                         8
#define ROWOW_DMG_CRITICAL                          10
#define ROWOW_DMG_LUCY_DODGE                        11
#define ROWOW_ATK_NONE                              0       // No attack (no ammo)
#define ROWOW_ATK_LUCKY                             1       // Perfect dodge
#define ROWOW_ATK_FLEE                              2       // Missed (FLEE)
#define ROWOW_ATK_MISS                              3       // Hit for nothing
#define ROWOW_ATK_BLOCK                             4
#define ROWOW_ATK_DEF                               5       // Hit

// battle_calc_base_damage's flags (rAthena's e_base_damage_flag)
#define ROWOW_BDMG_NONE                             0x0000
#define ROWOW_BDMG_CRIT                             0x0001
#define ROWOW_BDMG_ARROW                            0x0002
#define ROWOW_BDMG_MAGIC                            0x0004
#define ROWOW_BDMG_NOSIZE                           0x0008

// The skills a normal attack reads (rAthena's skill IDs).  The identity learns skills in R6, so until then none is known
#define ROWOW_SKILL_SM_SWORD                        2
#define ROWOW_SKILL_SM_TWOHAND                      3
#define ROWOW_SKILL_AL_DP                           22
#define ROWOW_SKILL_AL_DEMONBANE                    23
#define ROWOW_SKILL_TF_DOUBLE                       48
#define ROWOW_SKILL_KN_SPEARMASTERY                 55
#define ROWOW_SKILL_PR_MACEMASTERY                  65
#define ROWOW_SKILL_BS_HILTBINDING                  105
#define ROWOW_SKILL_BS_WEAPONRESEARCH               107
#define ROWOW_SKILL_HT_BEASTBANE                    126
#define ROWOW_SKILL_AS_RIGHT                        132
#define ROWOW_SKILL_AS_LEFT                         133
#define ROWOW_SKILL_AS_KATAR                        134
#define ROWOW_SKILL_AM_AXEMASTERY                   226
#define ROWOW_SKILL_MO_IRONHAND                     259
#define ROWOW_SKILL_SA_ADVANCEDBOOK                 274
#define ROWOW_SKILL_BA_MUSICALLESSON                315
#define ROWOW_SKILL_DC_DANCINGLESSON                323
#define ROWOW_SKILL_TK_RUN                          411
#define ROWOW_SKILL_GS_CHAINACTION                  511

#define ROWOW_MAX_LEVEL_BASE_EXP                    99999999    // rAthena's MAX_LEVEL_BASE_EXP, the base exp kept at the max level
#define ROWOW_MAX_LEVEL_JOB_EXP                     999999999   // MAX_LEVEL_JOB_EXP
#define ROWOW_DAMAGE_LOG_SIZE                       20          // DAMAGELOG_SIZE, the attackers a monster remembers

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
    std::vector<std::array<int32, ROWOW_STAT_COUNT>> BonusStatsByJobLevel; // The job's stat bonus at each job level (index is the level less 1)
    int32 BaseASPDByWeaponType[ROWOW_WEAPON_TYPE_MAX]; // Attack motion with each weapon type, 2000 (no attacks) for those it can't use
    int32 ItemJobIndex = -1; // The item_db Jobs name it uses items as (ROWOWItemJobNames), -1 for none

    ROWOWJob() { std::fill(std::begin(BaseASPDByWeaponType), std::end(BaseASPDByWeaponType), 2000); }
};

// rAthena's item types (e_item_types)
#define ROWOW_ITEM_TYPE_HEALING                     0
#define ROWOW_ITEM_TYPE_USABLE                      2
#define ROWOW_ITEM_TYPE_ETC                         3
#define ROWOW_ITEM_TYPE_ARMOR                       4
#define ROWOW_ITEM_TYPE_WEAPON                      5
#define ROWOW_ITEM_TYPE_CARD                        6
#define ROWOW_ITEM_TYPE_PETEGG                      7
#define ROWOW_ITEM_TYPE_PETARMOR                    8
#define ROWOW_ITEM_TYPE_AMMO                        10
#define ROWOW_ITEM_TYPE_DELAYCONSUME                11
#define ROWOW_ITEM_TYPE_CASH                        18

// item_db's job classes (rAthena's e_item_job): which tiers of a job may use an item.  Pre-renewal has the normal and upper ones
#define ROWOW_ITEM_CLASS_NORMAL                     0x01
#define ROWOW_ITEM_CLASS_UPPER                      0x02
#define ROWOW_ITEM_CLASS_BABY                       0x04
#define ROWOW_ITEM_CLASS_ALL                        0xFF

// item_db's genders (rAthena's e_sex)
#define ROWOW_GENDER_FEMALE                         0
#define ROWOW_GENDER_MALE                           1
#define ROWOW_GENDER_BOTH                           2

// item_db's Jobs names, whose bit in an item's JobMask is their index here (the jobs a pre-renewal job counts as, ROWOWJob::ItemJobIndex)
#define ROWOW_ITEM_JOB_COUNT                        25
#define ROWOW_ITEM_JOB_ALL                          ((uint64(1) << ROWOW_ITEM_JOB_COUNT) - 1)
extern char const* const ROWOWItemJobNames[ROWOW_ITEM_JOB_COUNT];

// What an RO item is to the RO rules (from mod_rowow_db_item, and mod_rowow_db_item_renewal for the post-renewal looks)
#define ROWOW_AMMO_TYPE_NONE                        0
#define ROWOW_AMMO_TYPE_ARROW                       1       // rAthena's e_ammo_type
#define ROWOW_AMMO_TYPE_BULLET                      3
#define ROWOW_AMMO_TYPE_SHELL                       4
#define ROWOW_AMMO_TYPE_GRENADE                     5
struct ROWOWItem
{
    uint32 ItemID = 0; // item_db Id
    uint32 ItemTemplateID = 0;
    uint8 Type = ROWOW_ITEM_TYPE_ETC; // ROWOW_ITEM_TYPE_*
    bool IsWeapon = false;
    bool IsArmor = false;
    bool IsAmmo = false;
    bool IsRenewal = false; // A stat-less post-renewal look
    bool IsDelayConsume = false; // Used up by what its script does, not when used (the Magnifier)
    bool IsContainer = false; // A box, which needs room in the bags (item_db's Container flag)
    bool IsDeadBranch = false;
    bool IsRefineable = false;
    uint64 JobMask = ROWOW_ITEM_JOB_ALL; // The item_db Jobs that may use it (bits by ROWOWItemJobNames)
    uint8 ClassMask = ROWOW_ITEM_CLASS_ALL; // ROWOW_ITEM_CLASS_*
    uint8 Gender = ROWOW_GENDER_BOTH;
    uint32 EquipLevelMin = 0;
    uint32 EquipLevelMax = 0;
    uint32 DelayDuration = 0; // How long after using it before another of it can be used, in milliseconds
    std::shared_ptr<ROWOWScript> UseScript; // A usable item's Script, compiled
    uint8 WeaponType = ROWOW_WEAPON_TYPE_FIST;
    uint8 AmmoType = ROWOW_AMMO_TYPE_NONE;
    uint32 Locations = 0; // ROWOW_EQUIP_POSITION_* where it's worn
    int32 Attack = 0;
    int32 Defense = 0;
    int32 Range = 0;
    int32 WeaponLevel = 0;
    int32 AmmoElement = -1; // The element an ammo's script gives its attacks (bonus bAtkEle), -1 for none.  The other scripts are R5's
};

// An item group (rAthena's item_group_db, by its name without IG_): its subgroups, each giving one item picked at random, or all of them
#define ROWOW_ITEM_GROUP_ALGORITHM_SHAREDPOOL       0       // item_group_db's Algorithm (e_group_algorithm_type)
#define ROWOW_ITEM_GROUP_ALGORITHM_ALL              1
#define ROWOW_ITEM_GROUP_ALGORITHM_RANDOM           2
#define ROWOW_ITEM_GROUP_ALGORITHM_DROP             3
struct ROWOWItemGroupEntry
{
    uint32 ItemID = 0;
    uint32 Rate = 0;
    uint32 Given = 0; // How often a shared pool has given it out since it last filled up
};
struct ROWOWItemSubGroup
{
    uint8 Algorithm = ROWOW_ITEM_GROUP_ALGORITHM_SHAREDPOOL;
    std::vector<ROWOWItemGroupEntry> Entries;
    uint32 TotalRate = 0;
    uint32 TotalGiven = 0;
};
struct ROWOWItemGroup
{
    std::map<uint32, ROWOWItemSubGroup> SubGroupsByID;
};

// The rAthena battle config values (conf/battle) the RO rules use (from mod_rowow_db_battle_config), with rAthena's defaults
struct ROWOWBattleConfig
{
    int32 BaseExpRate = 100;
    int32 JobExpRate = 100;
    bool MultiLevelUp = false;
    int32 MultiLevelUpBase = 0;
    int32 MultiLevelUpJob = 0;
    int32 MaxExpGainRate = 0;
    int32 ExpCalcType = 0;
    int32 ExpBonusAttacker = 25;
    int32 ExpBonusMaxAttacker = 12;
    int32 DeathPenaltyType = 1;
    int32 DeathPenaltyBase = 100;
    int32 DeathPenaltyJob = 100;
    int32 DeathPenaltyMaxLevel = 0;
    int32 HPRate = 100;
    int32 SPRate = 100;
    int32 RestartHPRate = 0;
    int32 RestartSPRate = 0;
    int32 MaxHPLevel99 = 330000;
    int32 MaxHPLevel150 = 660000;
    int32 MaxHP = 1100000;
    int32 MaxSP = 1000000;
    int32 MaxAspdAmotion = 200; // max_aspd as rAthena keeps it once loaded: the shortest attack delay (2000 - max_aspd * 10) * 2
    int32 MaxParameter = 99;
    int32 MaxTransParameter = 99;
    int32 MaxDef = 99;
    int32 OverDefBonus = 0;
    int32 WeaponDefenseType = 0;
    int32 MagicDefenseType = 0;
    int32 PCDamageDelayRate = 100;
    int32 NaturalHealHPInterval = 6000;
    int32 NaturalHealSPInterval = 8000;
    int32 EnableBaseAtk = ROWOW_BL_PC | 0x0008; // BL_PC | BL_HOM: monsters have no base attack
    int32 EnableCritical = ROWOW_BL_PC | 0x0010; // BL_PC | BL_MER: monsters don't crit
    int32 EnablePerfectFlee = ROWOW_BL_PC;
    int32 MobCriticalRate = 100;
    int32 MinHitRate = 5;
    int32 MaxHitRate = 100;
    int32 AgiPenaltyType = 1;
    int32 AgiPenaltyTarget = ROWOW_BL_PC;
    int32 AgiPenaltyCount = 3;
    int32 AgiPenaltyNum = 10;
    int32 VitPenaltyType = 1;
    int32 VitPenaltyTarget = ROWOW_BL_PC;
    int32 VitPenaltyCount = 3;
    int32 VitPenaltyNum = 5;
    int32 AttackAttrNone = ROWOW_BL_MOB | 0x0004 | 0x0008; // BL_MOB | BL_PET | BL_HOM: their normal attacks are non-elemental
    int32 AttrRecover = 1;
    int32 SkillMinDamage = 0;
    int32 ArrowDecrement = 1;
    int32 AmmoCheckWeapon = 1;
    int32 MonsterHPRate = 100;
    int32 MvpHPRate = 100;
    int32 MonsterDamageDelayRate = 100;
    int32 AreaSize = 14; // The cells around a unit it sees (area_size)
    int32 ItemUseInterval = 100;
    int32 AmmoUnequip = 1;
};

// An RO combatant's status (rAthena's status_data): a player's from status_calc_pc_sub, the stats with their bonuses and what follows
// from them, or a monster's from its mob_db row.  Gear scripts and cards (R5), skills (R6) and statuses come in with their milestones
struct ROWOWStatus
{
    bool IsPlayer = true; // A player's (BL_PC), or a monster's (BL_MOB)
    uint32 Level = 1;
    int32 Stats[ROWOW_STAT_COUNT] = { };
    int32 JobBonusStats[ROWOW_STAT_COUNT] = { }; // The part of the stats that's the job bonus
    uint32 MaxHP = 1;
    uint32 MaxSP = 1;
    int32 BaseAttack = 0; // batk
    int32 WeaponAttack = 0; // rhw.atk (a monster's lowest ATK)
    int32 WeaponRefineAttack = 0; // rhw.atk2: a player's refine bonus, a monster's highest ATK
    int32 LeftWeaponAttack = 0; // lhw.atk
    int32 LeftWeaponRefineAttack = 0;
    uint8 WeaponElement = ROWOW_ELEMENT_NEUTRAL; // rhw.ele
    uint8 LeftWeaponElement = ROWOW_ELEMENT_NEUTRAL; // lhw.ele
    int32 WeaponLevel = 0; // The right hand weapon's level, 0 for none
    int32 LeftWeaponLevel = 0;
    int32 WeaponSizeMods[ROWOW_SIZE_COUNT] = { 100, 100, 100 }; // right_weapon.atkmods, the weapon type's damage against each size
    int32 LeftWeaponSizeMods[ROWOW_SIZE_COUNT] = { 100, 100, 100 };
    bool IsArrowAttack = false; // state.arrow_atk: a bow or a gun, whose normal attack shoots the ammo
    int32 ArrowAttack = 0; // bonus.arrow_atk, the ammo's ATK
    int32 ArrowElement = ROWOW_ELEMENT_NEUTRAL; // bonus.arrow_ele, the ammo's element on a weapon that isn't a bow or gun (neutral for none)
    uint8 AmmoType = ROWOW_AMMO_TYPE_NONE; // The worn ammo's type
    uint8 Race = ROWOW_RACE_PLAYER_HUMAN;
    uint8 Size = ROWOW_SIZE_MEDIUM;
    uint8 DefElement = ROWOW_ELEMENT_NEUTRAL; // def_ele
    uint8 ElementLevel = 1; // ele_lv
    uint8 Class = ROWOW_CLASS_NORMAL; // class_
    uint32 Mode = 0; // ROWOW_MODE_*
    int32 DamageTaken = 100; // A monster's damagetaken: the percent of the damage dealt to it that it takes
    int32 MagicAttackMin = 0;
    int32 MagicAttackMax = 0;
    int32 Def = 0; // Hard DEF (gear)
    int32 Def2 = 0; // Soft DEF (VIT)
    int32 MDef = 0;
    int32 MDef2 = 0;
    int32 Hit = 0;
    int32 Flee = 0;
    int32 Flee2 = 0; // Perfect dodge, in tenths
    int32 Critical = 0; // In tenths
    int32 AttackMotion = 2000; // amotion, in milliseconds
    int32 AttackDelay = 4000; // adelay, the time between attacks
    int32 DamageMotion = 800;
    int32 AspdRate = 1000;
    int32 Range = 1; // Attack range in cells
    uint8 WeaponType = ROWOW_WEAPON_TYPE_FIST; // status.weapon, which is a dual wield type when two weapons are held
    uint8 RightWeaponType = ROWOW_WEAPON_TYPE_FIST; // weapontype1
    uint8 LeftWeaponType = ROWOW_WEAPON_TYPE_FIST; // weapontype2
    int32 RegenHP = 1; // HP and SP each natural regen tick (status_calc_regen)
    int32 RegenSP = 1;
};

// One attack's result (rAthena's struct Damage)
struct ROWOWDamage
{
    int64 Damage = 0; // The right hand's
    int64 Damage2 = 0; // The left hand's
    int64 BaseDamage = 0; // What a normal attack would deal
    int16 Div = 1; // div_, the hits
    int32 AMotion = 0;
    int32 DMotion = 0;
    uint8 Type = ROWOW_DMG_NORMAL; // ROWOW_DMG_*
    uint8 DmgLv = ROWOW_ATK_DEF; // ROWOW_ATK_*
    int32 Flag = 0; // ROWOW_BF_*
    int32 MiscFlag = 0;
};

// The attackers an RO monster remembers (rAthena's dmglog), for sharing its exp
struct ROWOWDamageLogEntry
{
    ObjectGuid PlayerGUID;
    uint64 Damage = 0;
};
struct ROWOWDamageLog : public DataMap::Base
{
    std::deque<ROWOWDamageLogEntry> Entries; // Oldest first
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
    std::map<uint32, ObjectGuid::LowType> ItemGUIDsByEquipPosition; // The RO equipment worn (in the bags, as RO's inventory holds it)
    bool IsEquipmentChanged = false;
    std::string SentEquipmentMessage; // What the client's RO interface was last told of the equipment
    uint32 EquipmentCheckTimer = 0;
    uint32 CanUseItemTime = 0; // canuseitem_tick: when the next item can be used (game time, milliseconds)
    std::unordered_map<uint32, uint32> ItemDelayEndTimes; // By item_db Id: when each item with a reuse delay can be used again
    bool IsIdentifyMenuOpen = false; // A Magnifier's list of items to appraise is up (menuskill_id MC_IDENTIFY)

    bool WasInFormAtLoad = false; // Saved in the form: logged out in the RO world, or the server stopped with the character in it

    ROWOWStatus Status; // Recalculated whenever what it comes from changes (CalculateIdentityStatus)
    uint32 RegenHPTimer = 0; // Time since the last natural HP and SP regen tick
    uint32 RegenSPTimer = 0;

    // A form change made before the character was in the world (arriving on a map), finished once it's in the world
    bool IsFormArrivalPending = false;
    bool IsFormDeparturePending = false;
    bool IsNewIdentityPending = false; // The first entry, whose start items are given once in the world

    // While in the form: the character's own values, put back on leaving and saved in place of the form's
    bool IsInForm = false;
    uint8 WoWLevel = 0;
    uint32 WoWXP = 0;
    uint32 WoWMoney = 0;
    uint32 WoWHealth = 0;
    uint32 WoWPowers[MAX_POWERS] = { };
    uint8 WoWPowerType = 0;
    uint32 WoWAttackTimes[ROWOW_ATTACK_TYPE_COUNT] = { };
    std::vector<uint32> HiddenSpellIDs; // The character's spells the client was told to forget for the form
    std::vector<uint32> SuspendedPassiveSpellIDs; // The passive auras of those, taken off for the form and cast again on leaving it
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
    std::unordered_map<uint32, ROWOWItem> ItemsByItemTemplateID;
    ROWOWBattleConfig BattleConfig;
    std::unordered_set<uint32> NoPenaltyMapIDs; // RO maps with rAthena's nopenalty map flag (no exp lost on death)
    int32 SizeFixByWeaponType[ROWOW_WEAPON_TYPE_MAX][ROWOW_SIZE_COUNT] = { }; // size_fix.yml: each weapon type's damage against each size, in percent
    int32 AttrFix[ROWOW_ELEMENT_LEVEL_MAX][ROWOW_ELEMENT_COUNT][ROWOW_ELEMENT_COUNT] = { }; // attr_fix.yml: by defense element level, attack element and defense element
    std::unordered_map<uint32, ROWOWStatus> MobStatusesByMobID; // Each RO monster's status, as rAthena's mob_db loading leaves it
    std::unordered_map<std::string, uint32> ItemIDsByAegisName; // Every converted RO item, by its lowercase AegisName
    std::unordered_map<std::string, ROWOWItemGroup> ItemGroupsByName; // By the uppercase group name (IG_ without the prefix)
    std::mutex ItemGroupSharedPoolMutex; // The shared pools' given counts are shared by every map thread
    std::unordered_map<std::string, std::unordered_set<uint32>> MapIDsByMapFlag; // The RO maps with each map flag the item rules read

    void LoadConfigurationFile();
    void LoadDataTables();
    void LoadCreatureMobData();
    void LoadIdentityData();
    void LoadStatusData();
    void LoadBattleData();
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
    static std::string GetJobNameKey(std::string const& jobName); // A job name as rAthena matches it (Super_Novice is also Supernovice)
    ROWOWJob const* GetJob(uint16 jobID) const;
    uint32 GetMapIDByMapName(std::string const& mapName) const;
    bool IsROItemTemplate(uint32 itemTemplateID) const { return ItemTemplateIDs.find(itemTemplateID) != ItemTemplateIDs.end(); }

    // The middle of an RO cell of an RO map, on the ground.  False when the map has no cell grid or no ground there
    bool GetCellPosition(uint32 mapID, int32 cellX, int32 cellY, ROWOWPosition& positionOut) const;

    // The player's identity, loaded on first use (a row of defaults until the identity exists)
    ROWOWIdentity* GetIdentity(Player* player);
    void SaveIdentity(Player* player, CharacterDatabaseTransaction trans);

    // Turning into the form and back, on arriving in and leaving the RO world (and at login).  These change the character's values,
    // spells and action bars, which works before the character is added to the map, so the client is first sent the character already
    // changed (no Warcraft level up effect from the level change).  The rest (the look, buffs, start items) follows once the character
    // is in the world, in CompleteROFormChange, at once when it already is
    void EnterROForm(Player* player);
    void LeaveROForm(Player* player);
    void CompleteROFormChange(Player* player);
    void RefreshROForm(Player* player); // After the identity's job, level, stats or equipment change
    void RefreshROFormValues(Player* player, ROWOWIdentity& identity); // The level, exp bar, status and maximums
    void RefreshROFormLook(Player* player); // The job's look
    void RemoveROFormAuras(Player* player); // The job's look and its headgear
    bool IsSpellKeptInROForm(SpellInfo const* spellInfo) const;

    // RO status (ROWOW_Status.cpp): the port of rAthena's pre-renewal status calculation for a player (status.cpp)
    void CalculateIdentityStatus(Player* player, ROWOWIdentity& identity) const; // status_calc_pc_sub
    int32 StatusBaseAmotionPC(ROWOWIdentity const& identity, ROWOWStatus const& status) const; // status_base_amotion_pc
    int32 StatusBaseAtk(ROWOWStatus const& status) const; // status_base_atk
    void StatusCalcMisc(ROWOWStatus& status, uint32 level) const; // status_calc_misc
    static int32 GetBattleObjectType(ROWOWStatus const& status) { return status.IsPlayer == true ? ROWOW_BL_PC : ROWOW_BL_MOB; }
    static int32 GetElementByName(std::string const& name); // An element (Ele_* without the prefix, any case), or -1
    uint32 StatusCalcMaxHPPC(ROWOWIdentity const& identity, int32 vit) const; // status_calc_maxhp_pc
    uint32 StatusCalcMaxSPPC(ROWOWIdentity const& identity, int32 int_) const; // status_calc_maxsp_pc
    static void StatusCalcRegen(ROWOWStatus& status); // status_calc_regen, for a player
    ROWOWItem const* GetROItem(Item const* item) const;

    // Status points (pc.cpp): what raising a stat costs, the most it can go to, and raising it
    int32 PCNeedStatusPoint(ROWOWIdentity const& identity, uint8 statIndex, int32 value) const; // pc_need_status_point
    int32 PCMaxParameter(ROWOWIdentity const& identity) const; // pc_maxparameter
    bool PCStatusUp(Player* player, uint8 statIndex, int32 increase); // pc_statusup

    // Experience (pc.cpp): gaining it, levelling up and losing it on death
    uint64 PCNextBaseExp(ROWOWIdentity const& identity) const; // pc_nextbaseexp
    uint64 PCNextJobExp(ROWOWIdentity const& identity) const; // pc_nextjobexp
    bool PCIsMaxBaseLevel(ROWOWIdentity const& identity) const; // pc_is_maxbaselv
    bool PCIsMaxJobLevel(ROWOWIdentity const& identity) const; // pc_is_maxjoblv
    void PCGainExp(Player* player, uint64 baseExp, uint64 jobExp); // pc_gainexp
    bool PCCheckBaseLevelUp(Player* player); // pc_checkbaselevelup
    bool PCCheckJobLevelUp(Player* player); // pc_checkjoblevelup
    void PCDeadPenalty(Player* player); // The exp penalty part of pc_dead

    // An RO monster's exp (mob.cpp): who hurt it how much (mob_log_damage), and its exp shared out to them when it dies (mob_dead)
    void MobLogDamage(Creature* creature, Unit* attacker, uint32 damage);
    void MobDeadExp(Creature* creature);

    // RO natural HP and SP regen (status_natural_heal), in place of WoW's while in the form
    void UpdateNaturalRegen(Player* player, uint32 diff);

    // RO battle (ROWOW_Battle.cpp): the port of rAthena's pre-renewal battle code (battle.cpp) for combat between RO combatants, so far
    // the normal attack.  Skills (R6), cards (R5) and statuses (R6) plug into the marked places
    ROWOWStatus const* GetROStatus(Unit const* unit) const; // status_get_status_data: a player's form, or a monster's mob_db status
    int32 PCCheckSkill(Unit const* unit, uint16 skillID) const; // pc_checkskill: the level a player in the form knows a skill at (0 until R6)
    void ApplyMobStatus(Creature* creature) const; // A spawning RO monster's HP, SP and attack delay (status_calc_mob)
    bool CalculateMeleeDamage(Unit* attacker, Unit* victim, CalcDamageInfo* damageInfo); // An auto attack swing between RO combatants
    uint8 BattleWeaponAttack(Unit* src, Unit* target, ROWOWDamage& wdOut); // battle_weapon_attack, a normal attack
    bool HasNormalAttackAmmo(Player* player) const; // Whether a bow or gun has its ammo (true for every other weapon)
    ROWOWDamage BattleCalcAttack(int32 attackType, Unit* src, Unit* target, uint16 skillID, uint16 skillLevel, int32 flag) const; // battle_calc_attack
    ROWOWDamage BattleCalcWeaponAttack(Unit* src, Unit* target, uint16 skillID, uint16 skillLevel, int32 wflag) const; // battle_calc_weapon_attack
    int64 BattleAttrFix(Unit const* src, Unit const* target, int64 damage, int32 atkElement, int32 defElement, int32 defElementLevel, int32 flag) const; // battle_attr_fix
    float GetAutoAttackExtraRange(Player* player, Unit const* victim) const; // How much further than melee range the form's weapon reaches
    static uint32 GetWeaponHitSoundSpellID(uint8 weaponType); // The spell that plays RO's hit sound of a weapon type on its caster

    // Tells the client's RO interface the identity's levels, exp, stats and status (the RO_Identity addon, ROWOWSTATUS prefix)
    void SendROStatusToClient(Player* player);

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

    // RO items (ROWOW_Items.cpp): the port of rAthena's equipment and item use (pc.cpp), and the item's own state.  Worn items stay in the
    // bags, as RO's inventory holds them, and the identity remembers where each is worn
    void LoadItemScriptData(); // Registers the script engine's commands and compiles the usable items' scripts
    static uint32 GetItemRefine(Item const* item);
    static bool IsItemBroken(Item const* item);
    static bool IsItemIdentified(Item const* item);
    static void SetItemState(Player* player, Item* item, uint32 refine, bool isBroken, bool isIdentified);
    uint32 GetIdentityEquipPosition(ROWOWIdentity const& identity, ObjectGuid::LowType itemGUID) const; // Where an item is worn, 0 for not
    uint8 PCIsEquip(Player* player, ROWOWIdentity const& identity, Item const* item) const; // pc_isequip
    int32 PCEquipPoint(Player* player, ROWOWIdentity const& identity, ROWOWItem const& roItem) const; // pc_equippoint
    bool PCEquipItem(Player* player, Item* item, int32 reqPos); // pc_equipitem
    bool PCUnequipItem(Player* player, uint32 equipPosition, int32 flag); // pc_unequipitem, by a position the item is worn in
    bool PCCheckItem(Player* player, ROWOWIdentity& identity); // pc_checkitem: takes off what can no longer be worn, true when anything was
    bool PCIsUseItem(Player* player, Item const* item, bool isReporting); // pc_isUseitem
    void PCUseItem(Player* player, ObjectGuid itemGUID); // pc_useitem, after the use item spell
    void PCItemHeal(Player* player, uint32 itemID, int32 hp, int32 sp); // pc_itemheal
    void PCPercentHeal(Player* player, int32 hp, int32 sp); // pc_percentheal
    void StatusHeal(Player* player, int64 hp, int64 sp); // status_heal on a player
    bool PCGetItemGroup(Player* player, std::string const& groupName); // ItemGroupDatabase::pc_get_itemgroup
    bool GiveROItem(Player* player, uint32 itemID, uint32 amount, bool isIdentified); // pc_additem, mailing what doesn't fit
    void SkillIdentify(Player* player, Item* item); // skill_identify
    void SendItemIdentifyList(Player* player); // clif_item_identify_list
    void TeleportToRandomSpot(Player* player); // A random spot on the map (AL_TELEPORT level 1), at one of its monsters' homes
    void TeleportToSavePoint(Player* player); // The identity's save point (AL_TELEPORT level 2 and up)

    // Keeps the worn items true to the bags (one sold, moved to the bank or destroyed is no longer worn) and tells the client's RO interface
    // where each worn item is, under the ROWOWEQUIP prefix, when that changes
    void UpdateIdentityEquipment(Player* player, ROWOWIdentity& identity);
    void SendEquipmentToClient(Player* player, ROWOWIdentity& identity, bool isForced);
    static bool GetClientItemPosition(Item const* item, uint32& bagOut, uint32& slotOut); // The client's bag (0-4) and slot (from 1)
    static Item* GetItemByClientPosition(Player* player, uint32 bag, uint32 slot);

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
    // The rest of the RO battle port (ROWOW_Battle.cpp), with rAthena's function names
    ROWOWDamage InitializeWeaponData(Unit const* src, Unit const* target, uint16 skillID, uint16 skillLevel, int32 wflag) const; // initialize_weapon_data
    bool IsAttackCritical(ROWOWDamage& wd, Unit* src, Unit const* target, uint16 skillID, uint16 skillLevel, bool firstCall) const; // is_attack_critical
    bool IsAttackHitting(ROWOWDamage& wd, Unit* src, Unit* target, uint16 skillID, uint16 skillLevel, bool firstCall) const; // is_attack_hitting
    bool AttackIgnoresDef(ROWOWDamage& wd, Unit* src, Unit const* target, uint16 skillID, uint16 skillLevel, bool isRightHand) const; // attack_ignores_def
    int32 IsAttackPiercing(ROWOWDamage& wd, Unit* src, Unit const* target, uint16 skillID, uint16 skillLevel, bool isRightHand) const; // is_attack_piercing
    bool IsInfiniteDefense(Unit const* target, int32 flag) const; // is_infinite_defense
    bool IsSkillUsingArrow(Unit const* src, uint16 skillID) const; // is_skill_using_arrow
    bool IsAttackRightHanded(Unit const* src, uint16 skillID) const; // is_attack_right_handed
    bool IsAttackLeftHanded(Unit const* src, uint16 skillID) const; // is_attack_left_handed
    int32 BattleGetWeaponElement(ROWOWDamage const& wd, Unit const* src, Unit const* target, uint16 skillID, uint16 skillLevel, bool isRightHand) const; // battle_get_weapon_element
    void BattleCalcMultiAttack(ROWOWDamage& wd, Unit* src, Unit* target, uint16 skillID, uint16 skillLevel) const; // battle_calc_multi_attack
    void BattleCalcSkillBaseDamage(ROWOWDamage& wd, Unit* src, Unit* target, uint16 skillID, uint16 skillLevel) const; // battle_calc_skill_base_damage
    int64 BattleCalcBaseDamage(Unit const* src, ROWOWStatus const& status, bool isLeftHand, uint8 targetSize, int32 flag) const; // battle_calc_base_damage
    void BattleCalcDefenseReduction(ROWOWDamage& wd, Unit* src, Unit* target, uint16 skillID, uint16 skillLevel) const; // battle_calc_defense_reduction
    void BattleCalcAttackPostDefense(ROWOWDamage& wd, Unit* src, Unit* target, uint16 skillID, uint16 skillLevel) const; // battle_calc_attack_post_defense
    void BattleCalcAttackMasteries(ROWOWDamage& wd, Unit* src, Unit* target, uint16 skillID, uint16 skillLevel) const; // battle_calc_attack_masteries
    int64 BattleAddMastery(Unit const* src, Unit const* target, int64 damage, bool isLeftHand) const; // battle_addmastery
    void BattleCalcElementDamage(ROWOWDamage& wd, Unit* src, Unit* target, uint16 skillID, uint16 skillLevel) const; // battle_calc_element_damage
    void BattleCalcAttackPlant(ROWOWDamage& wd, Unit* src, Unit* target, uint16 skillID, uint16 skillLevel) const; // battle_calc_attack_plant
    void BattleApplyDivFix(ROWOWDamage& wd, uint16 skillID) const; // battle_apply_div_fix
    void BattleCalcAttackLeftRightHands(ROWOWDamage& wd, Unit* src, Unit* target, uint16 skillID, uint16 skillLevel) const; // battle_calc_attack_left_right_hands
    int64 BattleCalcDamage(Unit* src, Unit* target, ROWOWDamage const& d, int64 damage, uint16 skillID, uint16 skillLevel) const; // battle_calc_damage
    void BattleMinDamage(ROWOWDamage& wd, Unit const* src, uint16 skillID, int64 minimum) const; // battle_min_damage

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
