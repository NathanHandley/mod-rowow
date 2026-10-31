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
#include "ROWOW_Npc.h"
#include "ROWOW_Skill.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Creature;
class Item;
class Map;
class Player;
class SpellInfo;
class Unit;
class WorldLocation;
struct CalcDamageInfo;
struct ItemTemplate;
struct Position;
struct ROWOWScript;
struct ROWOWScriptContext;

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
#define ROWOW_HAIR_STYLE_DEFAULT                    1 // The head shown for a hair style or color with no head (RO's first style, its own colors)
struct ROWOWItemVisual
{
    uint8 VisualType = 0;
    uint32 ViewID = 0; // RO headgear accessory ID or shield type
    uint32 VisualSpellID = 0; // Aura that attaches the headgear model, 0 for none
};

// RO's directions from one cell to another (0 north, then counter-clockwise: rAthena's enum directions)
#define ROWOW_RO_DIRECTION_COUNT                    8
#define ROWOW_RO_DIRECTION_WEST                     2
#define ROWOW_RO_DIRECTION_EAST                     6
#define ROWOW_SKILL_CELL_GROUND_SEARCH_HEIGHT       3.0f    // How far above a ground skill's spot the ground search for its units' cells starts
#define ROWOW_SKILL_CELL_GROUND_SEARCH_DISTANCE     10.0f   // How far down it looks

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

// A monster's steal state (mob_data's state.steal_flag and state.steal_coin_flag): the steal tries made on it, UCHAR_MAX once it can't be
// stolen from any more (one item per monster), and whether its zeny was stolen.  Kept on the creature as "ROWOWStealState" for one life;
// only its map's thread touches it
struct ROWOWStealState : public DataMap::Base
{
    uint8 StealFlag = 0;
    bool IsStealCoinFlag = false;
};

// A monster's Soul Exhale state (mob_data's state.soul_change_flag): a Professor already took its SP.  Kept on the creature as
// "ROWOWSoulChangeState" for one life; only its map's thread touches it
struct ROWOWSoulChangeState : public DataMap::Base
{
    bool IsSoulChangeFlag = false;
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
#define ROWOW_DATA_TABLE_ITEM_COMBO                 "mod_rowow_db_item_combo"
#define ROWOW_DATA_TABLE_REFINE                     "mod_rowow_db_refine"
#define ROWOW_DATA_TABLE_JOB                        "mod_rowow_db_job"
#define ROWOW_DATA_TABLE_STATPOINT                  "mod_rowow_db_statpoint"
#define ROWOW_DATA_TABLE_MAP_FLAG                   "mod_rowow_db_map_flag"
#define ROWOW_DATA_TABLE_BATTLE_CONFIG              "mod_rowow_db_battle_config"
#define ROWOW_DATA_TABLE_CHAR_CONFIG                "mod_rowow_db_char_config"
#define ROWOW_DATA_TABLE_ATTR_FIX                   "mod_rowow_db_attr_fix"
#define ROWOW_DATA_TABLE_SIZE_FIX                   "mod_rowow_db_size_fix"
#define ROWOW_DATA_TABLE_SKILL                      "mod_rowow_db_skill"
#define ROWOW_DATA_TABLE_SKILL_TREE                 "mod_rowow_db_skill_tree"
#define ROWOW_DATA_TABLE_STATUS                     "mod_rowow_db_status"

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

// The items the skills make or use by ID (rAthena's ITEMID_*)
#define ROWOW_ITEMID_HOLY_WATER                     523
#define ROWOW_ITEMID_EMPTY_BOTTLE                   713
#define ROWOW_ITEMID_STONE                          7049
#define ROWOW_ITEMID_POISON_BOTTLE                  678     // ITEMID_POISON_BOTTLE: what Create Deadly Poison makes and Enchant Deadly Poison uses

// rAthena's job IDs (e_job in mmo.hpp)
#define ROWOW_JOB_NOVICE                            0
#define ROWOW_JOB_ACOLYTE                           4
#define ROWOW_JOB_THIEF                             6
#define ROWOW_JOB_MONK                              15
#define ROWOW_JOB_BARD                              19
#define ROWOW_JOB_DANCER                            20
#define ROWOW_JOB_SUPER_NOVICE                      23
#define ROWOW_JOB_GUNSLINGER                        24
#define ROWOW_JOB_NINJA                             25
#define ROWOW_JOB_NOVICE_HIGH                       4001
#define ROWOW_JOB_ACOLYTE_HIGH                      4005
#define ROWOW_JOB_CHAMPION                          4016
#define ROWOW_JOB_CLOWN                             4020
#define ROWOW_JOB_GYPSY                             4021
#define ROWOW_JOB_TAEKWON                           4046
#define ROWOW_JOB_STAR_GLADIATOR                    4047
#define ROWOW_JOB_SOUL_LINKER                       4049
#define ROWOW_JOB_UPPER_START                       4001    // Novice_High, the first transcendent job
#define ROWOW_JOB_UPPER_END                         4022    // Paladin2, the last
#define ROWOW_JOB_BABY                              4023    // The Baby Novice
#define ROWOW_JOB_BABY_ACOLYTE                      4027
#define ROWOW_JOB_BABY_MONK                         4038
#define ROWOW_JOB_BABY_BARD                         4042
#define ROWOW_JOB_BABY_DANCER                       4043
#define ROWOW_JOB_SUPER_BABY                        4045

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

// A forged weapon (rAthena's card[0] CARD0_FORGE, card[1] (star crumbs * 5) << 8 | element) keeps its Star Crumbs and element stone in its
// first card slot's enchantment, the converter's fixed enchantments from 8064 (RO_DBCID_SPELLITEMENCHANTMENT_FORGE_*): 8064 plus five times
// the Star Crumbs (0-3) plus the stone (0 none, 1 Flame Heart, 2 Mystic Frozen, 3 Rough Wind, 4 Great Nature).  Its smith is the item's creator
#define ROWOW_ITEM_FORGE_ENCHANTMENT_START          8064
#define ROWOW_ITEM_FORGE_ELEMENT_COUNT              5
#define ROWOW_ITEM_FORGE_ENCHANTMENT_COUNT          20

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

// The "all" entries of the bonus tables, and the tables' sizes (RC_ALL/RC_MAX, ELE_ALL/ELE_MAX, SZ_ALL/SZ_MAX, CLASS_ALL/CLASS_MAX)
#define ROWOW_RACE_ALL                              12
#define ROWOW_RACE_MAX                              13
#define ROWOW_ELEMENT_ALL                           10
#define ROWOW_ELEMENT_MAX                           11
#define ROWOW_SIZE_ALL                              3
#define ROWOW_SIZE_MAX                              4
#define ROWOW_CLASS_ALL                             5
#define ROWOW_CLASS_MAX                             6

// Whose a bonus is while an item's script runs (rAthena's e_lr_flag): the right hand, a left-hand weapon, the ammo, or a shield
#define ROWOW_LR_FLAG_NONE                          0
#define ROWOW_LR_FLAG_WEAPON                        1
#define ROWOW_LR_FLAG_ARROW                         2
#define ROWOW_LR_FLAG_SHIELD                        3

#define ROWOW_MAX_REFINE                            10
#define ROWOW_MAX_SLOTS                             4       // Card slots on an item

// The monster modes the battle and status rules read (rAthena's e_mode bits).  The AI modes come with the monster AI (R10)
#define ROWOW_MODE_CANMOVE                          0x0000001
#define ROWOW_MODE_CANATTACK                        0x0000080
#define ROWOW_MODE_IGNORE_MELEE                     0x0010000
#define ROWOW_MODE_IGNORE_MAGIC                     0x0020000
#define ROWOW_MODE_IGNORE_RANGED                    0x0040000
#define ROWOW_MODE_MVP                              0x0080000
#define ROWOW_MODE_IGNORE_MISC                      0x0100000
#define ROWOW_MODE_KNOCKBACK_IMMUNE                 0x0200000
#define ROWOW_MODE_DETECTOR                         0x2000000
#define ROWOW_MODE_STATUS_IMMUNE                    0x4000000
#define ROWOW_MODE_SKILL_IMMUNE                     0x8000000
#define ROWOW_MODE_PC                               0x000B7FF // A player's modes: MD_MASK less the monster-only ones (status_calc_pc_)

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
#define ROWOW_BF_WEAPONMASK                         0x000F
#define ROWOW_BF_RANGEMASK                          0x00F0
#define ROWOW_BF_SKILLMASK                          0x0F00
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
    bool IsBaby = false; // A Baby job (rAthena's JOBL_BABY)
    uint16 NormalJobID = 0; // A Baby job's job (its class_ less JOBL_BABY: Baby_Monk's Monk), the job itself for the others
    uint32 MaxBaseLevel = 99;
    uint32 MaxJobLevel = 10;
    std::vector<uint32> BaseHPByLevel; // Index is the base level less 1
    std::vector<uint32> BaseSPByLevel;
    std::vector<uint64> BaseExpByLevel; // The exp from each level to the next
    std::vector<uint64> JobExpByLevel;
    std::vector<std::array<int32, ROWOW_STAT_COUNT>> BonusStatsByJobLevel; // The job's stat bonus at each job level (index is the level less 1)
    int32 BaseASPDByWeaponType[ROWOW_WEAPON_TYPE_MAX]; // Attack motion with each weapon type, 2000 (no attacks) for those it can't use
    int32 ItemJobIndex = -1; // The item_db Jobs name it uses items as (ROWOWItemJobNames), -1 for none
    int32 FirstJobID = -1; // A second job's first job, as a normal job (rAthena's MAPID_FIRSTMASK of a JOBL_2 job), -1 for the others

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
    bool IsNoUseSitting = false; // Can't be used sitting (item_db's NoUse Sitting)
    bool IsContainer = false; // A box, which needs room in the bags (item_db's Container flag)
    bool IsDeadBranch = false;
    bool IsRefineable = false;
    uint64 JobMask = ROWOW_ITEM_JOB_ALL; // The item_db Jobs that may use it (bits by ROWOWItemJobNames)
    uint8 ClassMask = ROWOW_ITEM_CLASS_ALL; // ROWOW_ITEM_CLASS_*
    uint8 Gender = ROWOW_GENDER_BOTH;
    uint32 EquipLevelMin = 0;
    uint32 EquipLevelMax = 0;
    uint32 DelayDuration = 0; // How long after using it before another of it can be used, in milliseconds
    uint8 Slots = 0; // Card slots
    std::shared_ptr<ROWOWScript> UseScript; // A usable item's Script, compiled
    std::shared_ptr<ROWOWScript> BonusScript; // Worn equipment's, a card's or ammo's Script, run by every status calculation
    std::shared_ptr<ROWOWScript> EquipScript; // Run when it's put on, and UnEquipScript when it's taken off
    std::shared_ptr<ROWOWScript> UnEquipScript;
    uint32 CardEnchantmentID = 0; // A card's enchantment, which marks it compounded in an item's card slot
    uint8 WeaponType = ROWOW_WEAPON_TYPE_FIST;
    uint8 AmmoType = ROWOW_AMMO_TYPE_NONE;
    uint32 Locations = 0; // ROWOW_EQUIP_POSITION_* where it's worn
    int32 Attack = 0;
    int32 Defense = 0;
    int32 Range = 0;
    int32 WeaponLevel = 0;
    int32 Weight = 0; // In tenths
    int32 Buy = 0; // The NPC shops' price (zeny)
    bool IsNoTrade = false; // item_db's Trade restrictions the cart and the shops read (itemdb_cantrade, itemdb_cancartstore)
    bool IsNoCart = false;
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

// An item combo (item_combos): worn together, the items give the combo's bonuses too
struct ROWOWItemCombo
{
    std::vector<uint32> ItemIDs;
    std::shared_ptr<ROWOWScript> Script;
};

// The refine bonuses (refine.yml) of armor (type 0) and each weapon level (1-4): the bonus and the random overrefine bonus, in hundredths
#define ROWOW_REFINE_TYPE_COUNT                     5
#define ROWOW_REFINE_COST_NORMAL                    0       // e_refine_cost_type: REFINE_COST_NORMAL, REFINE_COST_OVER10, REFINE_COST_ENRICHED
#define ROWOW_REFINE_COST_ENRICHED                  2
#define ROWOW_REFINE_COST_COUNT                     3
struct ROWOWRefineBonus
{
    int32 Bonus = 0;
    int32 RandomBonus = 0;
    int32 Chance = 0; // The normal refine's chance to reach this level, in 10000 (REFINE_COST_NORMAL's)
    bool HasCost[ROWOW_REFINE_COST_COUNT] = { }; // The refiners' costs by REFINE_COST_* (s_refine_cost): whether there's one, its chance in 10000,
    int32 CostChances[ROWOW_REFINE_COST_COUNT] = { }; // its zeny and its material (item_db Id)
    int32 CostPrices[ROWOW_REFINE_COST_COUNT] = { };
    uint32 CostMaterialIDs[ROWOW_REFINE_COST_COUNT] = { };
};

// A recipe of produce_db (s_skill_produce_db): what it makes, its item level (1-3 weapon levels, 11-20 cooking, 21 smelting, 22 brewing, 23
// Elemental Converters), the skill and level it needs, and its materials (an amount of 0 is a guide that must be held but isn't used up)
struct ROWOWProduce
{
    uint32 NameID = 0;
    int32 ItemLevel = 0;
    uint16 RequireSkillID = 0;
    int32 RequireSkillLevel = 0;
    std::vector<std::pair<uint32, int32>> Materials; // item_db Id, amount
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
    int32 MaxBabyParameter = 80;
    int32 TranscendentStatusPoints = 52; // transcendent_status_points: the status points a change to a transcendent job gives
    int32 DancingWeaponSwitchFix = 1;
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
    int32 SkillUpLimit = 1; // A job's skills can't be raised until the jobs before it used their skill points (pc_calc_skilltree_normalize_job)
    int32 SkillFree = 0; // No skill prerequisites
    int32 QuestSkillLearn = 0; // Quest skills can be learned with skill points
    int32 CastRate = 100; // casting_rate
    int32 CastrateDexScale = 150; // The DEX at which casting is instant
    int32 DelayRate = 100;
    int32 DelayDependOnDex = 0;
    int32 DelayDependOnAgi = 0;
    int32 MinSkillDelayLimit = 100;
    int32 NoSkillDelay = ROWOW_BL_MOB; // Who has no after-cast delay (min_skill_delay_limit instead)
    int32 MaxHeal = 9999;
    int32 MaxHealLv = 11; // From this skill level on, a heal heals max_heal
    int32 DelayBattleDamage = 1; // A hit lands its attack motion after the attack
    int32 KnockbackLeft = 1; // A knockback from the same cell goes east
    int32 SkillrangeByDistance = ROWOW_BL_MOB; // Whose skills are near or far by the distance to the target, not by the skill's range
    int32 SkillrangeFromWeapon = 0; // use_weapon_skill_range
    int32 SkillCasterCheck = 1;
    int32 CardfixMonsterPhysical = 1;
    int32 PathBlownHalt = 1;
    int32 ExpBonusNodamageAttacker = 0;
    int32 HealExp = 0;
    int32 SkillAddRange = 0;
    int32 PCScDefRate = 100; // pc_status_def_rate: how much of a player's status resistance counts
    int32 MobScDefRate = 100;
    int32 PCMaxScDef = 100; // pc_max_status_def: the most resistance there is, in percent
    int32 MobMaxScDef = 100;
    int32 ScCastCancel = 0; // status_cast_cancel: who a stopping status (stun, freeze) stops the cast of (BL_*)
    int32 MaxWalkSpeed = 300; // max_walk_speed: a player's fastest walk, in percent of the default walk speed
    int32 GtbScImmunity = 50; // gtb_sc_immunity: from how much bNoMagicDamage on magic skills stop statuses
    int32 PCCloakCheckType = 1; // player_cloak_check_type: 1 needs a wall, 2 attacking doesn't end it, 4 skills don't
    int32 MonsterCloakCheckType = 4;
    int32 InfiniteEndure = 0; // Whose kind always has endure (BL_*)
    int32 UndeadDetectType = 0; // 0: the undead element, 1: the undead race, 2: either
    int32 DebuffOnLogout = 0; // debuff_on_logout: 1 keeps the debuffs saved over a logout, 2 the buffs
    int32 AutoCounterType = 15; // auto_counter_type: whose Auto Counter is always critical (BL_*)
    int32 EquipNaturalBreakRate = 0;
    int32 EquipSelfBreakRate = 100;
    int32 EquipSkillBreakRate = 100;
    int32 BreakMobEquip = 0; // Whether monsters' "equipment" breaks (their ATK and DEF statuses)
    int32 StormgustKnockback = 1; // Storm Gust pushes back from its center (0: a random way)
    int32 CartRevoKnockback = 1; // Cart Revolution pushes west, as officially
    int32 ArrowShowerKnockback = 1; // Arrow Shower pushes away from where the arrows fall (0: away from the archer)
    int32 PCInvincibleTime = 3000; // player_invincible_time: ms a player can't be targeted after arriving on a map or reviving
    int32 MaxCartWeight = 80000; // In tenths: max_cart_weight (8000) times 10
    int32 VendingMaxValue = 1000000000;
    int32 VendingOverMax = 1; // A sale may take the vendor past the zeny cap (what's over is lost)
    int32 VendingTax = 500; // In hundredths of a percent, on sales of vending_tax_min and up
    int32 VendingTaxMin = 100000000;
    int32 SkillStealMaxTries = 0; // How often a monster can be tried (0: until stolen from)
    int32 AutospellCheckRange = 0; // Autospells check their range to the target
    int32 AutospellStacking = 0; // Several autospells of one skill can go off on the same hit
    int32 SkipTeleportLv1Menu = 0; // Teleport level 1 goes without asking (the menu offers only "Random" then)
    int32 BasicSkillCheck = 1; // Basic Skill gates trading, emotions, sitting, parties and the storage
    int32 NaturalHealSkillInterval = 10000; // natural_heal_skill_interval: how often the regen skills heal (ms)
    int32 DevotionLevelDifference = 10; // devotion_level_difference: the base levels a Crusader and the one it devotes itself to may be apart
    int32 DevotionRDamage = 0; // devotion_rdamage: the chance (%) that damage reflected onto a devoted attacker goes to its Crusader
    int32 DevotionRDamageSkillOnly = 1; // devotion_rdamage_skill_only: an inherited Reflect Shield reflects only skills
    int32 DevotionStandupFix = 1; // devotion_standup_fix: a sitting devoted player (and its Crusader) stands up when hit
    int32 BowlingBashArea = 0; // bowling_bash_area: 0 the gutter lines (40x40 squares), 1 without the demi-gutter bug, else a square around the caster
    int32 GxAllHit = 0; // gx_allhit: Grand Cross hits every target in each cell (else each cell hits three times in all)
    int32 SkillEightpathSameCell = 1; // skill_eightpath_same_cell: a path skill (Brandish Spear) also hits on the caster's own cell
    int32 LandProtectorBehavior = 0; // land_protector_behavior: a magic unit fails on Land Protector by the unit's cell (0) or the target's (1)
    int32 DispelSong = 0; // dispel_song: Dispell also ends the songs on a target still in their area
    int32 ComboDelayRate = 100; // combo_delay_rate: the percent of 300 ms a combo's window lasts past its skill's after-cast delay
    int32 FingerOffensiveType = 0; // finger_offensive_type: 0 Throw Spirit Sphere's spheres hit as one attack, 1 one after another
    int32 PlayerSkillPartnerCheck = 1; // player_skill_partner_check: the skills that need partners (B.S. Sacramenti) look for them
    int32 ResurrectionExp = 0; // resurrection_exp: exp the reviver gets from a higher level player revived (in 10000)
    int32 ExpCostRedemptio = 1; // exp_cost_redemptio: the percent of a level's exp Redemptio needs and costs
    int32 ExpCostRedemptioLimit = 5; // exp_cost_redemptio_limit: the players it revives for the cost to be none
    int32 TrapsSetting = 0; // traps_setting: hidden traps (UF_HIDDENTRAP) are hidden nowhere (0), on the PvP maps (1) or everywhere (2)
    int32 MultiTriggerTrap = 0; // multi_trigger_trap: a trap that goes off hits everything on it before it's spent
    int32 SkillRemoveTrapType = 0; // skill_removetrap_type: Remove Trap gives back the trap (0) or every item it was made of (1)
    int32 RefreshSong = 0; // refresh_song: a song's effect on someone who stepped out is renewed on stepping back in, not by its timer
    int32 WeaponProduceRate = 100; // weapon_produce_rate (wp_rate): the percent of a forge's success chance that counts
    int32 PotionProduceRate = 100; // potion_produce_rate (pp_rate): ... of a brew's
    int32 OrideconResearchFix = 0; // oridecon_research_fix: Oridecon Research also helps forge level 3 weapons
    int32 LandSkillLimit = ROWOW_BL_PC | 0x0008; // land_skill_limit: whose ground skills and summons are limited to their skill's count (BL_PC | BL_HOM)
    int32 AlchemistSummonSetting = 15; // alchemist_summon_setting: 1 players can hit the plants, 2 the Marine Spheres, 4 one plant kind at a time
    int32 AlchemistSummonReward = 1; // alchemist_summon_reward: 1 Marine Spheres drop their items, 2 the plants too
    int32 FirstAttackLootBonus = 30; // first_attack_loot_bonus: the percent of all the damage the first attacker adds for the loot priority
    int32 SkillAddHealRate = 487; // skill_add_heal_rate: the heal skills bHealPower raises (1 Heal, 2 Sanctuary, 4 Aid Potion, 8 Slim Potion Pitcher, 16 Apple of Idun...)
    int32 BackstabBowPenalty = 1; // backstab_bow_penalty: Back Stab with a bow does half its damage
    int32 SwitchRemoveEdp = 2; // switch_remove_edp: (pre-renewal &1) taking off the weapon ends Enchant Deadly Poison, which bare hands can't cast
    int32 AllowEsMagicPC = 0; // allow_es_magic_player: the Soul Linker's Es spells work on players
    int32 AllowSkillWithoutDay = 0; // allow_skill_without_day: the Star Gladiator's sun, moon and star skills work on any day
    int32 SgMiracleSkillRatio = 1; // sg_miracle_skill_ratio: Miracle of the Sun, Moon and Stars' chance on a hit, in 20000 (lower with AGI over 46)
    int32 SgMiracleSkillDuration = 3600000; // sg_miracle_skill_duration
    int32 TaekwonMissionMobname = 0; // taekwon_mission_mobname: also count monsters of the mission's race (1, goblins...) or name (2)
};

// A skill of a job's skill tree (skill_tree.yml's s_skill_tree_entry)
struct ROWOWSkillTreeEntry
{
    uint16 SkillID = 0;
    uint16 MaxLevel = 0;
    uint32 BaseLevel = 0;
    uint32 JobLevel = 0;
    std::vector<std::pair<uint16, uint16>> Needs; // The skills and levels it needs first
    bool IsExcludedFromInherit = false; // A job inheriting the tree doesn't get it
};

// A job's skill tree, its own skills and those of the jobs it inherits from (s_skill_tree once loadingFinished merged them)
struct ROWOWSkillTree
{
    std::vector<uint16> InheritJobIDs;
    std::map<uint16, ROWOWSkillTreeEntry> SkillsByID;
};

// One weapon hand's bonuses (rAthena's weapon_data): what its cards and scripts add against races, elements, sizes and classes
struct ROWOWWeaponBonus
{
    int32 AddRace[ROWOW_RACE_MAX] = { };
    int32 AddEle[ROWOW_ELEMENT_MAX] = { };
    int32 AddSize[ROWOW_SIZE_MAX] = { };
    int32 AddClass[ROWOW_CLASS_MAX] = { };
    uint32 IgnoreDefEle = 0; // Bit masks (of elements, races, classes) whose DEF it ignores
    uint32 IgnoreDefRace = 0;
    uint32 IgnoreDefClass = 0;
    uint32 DefRatioAtkEle = 0; // ... whose DEF raises its damage instead (Ice Pick)
    uint32 DefRatioAtkRace = 0;
    uint32 DefRatioAtkClass = 0;
    int32 Overrefine = 0; // The random bonus past the safe refine
    int32 Star = 0; // star: a forged weapon's Star Crumbs' ATK (5 each, 40 for three)
    std::vector<std::pair<uint32, int32>> AddDamageClass; // add_dmg: percent more damage to a monster (mob_db Id)
};

// A bonus of a system still to come (skills, magic, statuses, drops, autospells), kept by name so that system can read it
struct ROWOWDeferredBonus
{
    std::string Type; // Like bAutoSpell
    std::vector<int64> Values;
    uint8 LRFlag = ROWOW_LR_FLAG_NONE;
};

// A player's bonuses from the worn items, their cards and the item combos (rAthena's bonus, indexed_bonus, special_state and the rates of
// map_session_data), rebuilt by every status calculation.  Monsters have none
struct ROWOWBonus
{
    int32 ParamBonus[ROWOW_STAT_COUNT] = { };
    int32 ParamEquip[ROWOW_STAT_COUNT] = { }; // The part of the stats that came from equipment (more HP and SP, as rAthena gives)
    ROWOWWeaponBonus RightWeapon;
    ROWOWWeaponBonus LeftWeapon;
    int32 ArrowAddRace[ROWOW_RACE_MAX] = { };
    int32 ArrowAddEle[ROWOW_ELEMENT_MAX] = { };
    int32 ArrowAddSize[ROWOW_SIZE_MAX] = { };
    int32 ArrowAddClass[ROWOW_CLASS_MAX] = { };
    int32 SubEle[ROWOW_ELEMENT_MAX] = { }; // subele_script: percent less damage from each element
    int32 SubRace[ROWOW_RACE_MAX] = { };
    int32 SubSize[ROWOW_SIZE_MAX] = { };
    int32 SubClass[ROWOW_CLASS_MAX] = { };
    int32 CriticalAddRace[ROWOW_RACE_MAX] = { }; // In tenths
    int32 IgnoreDefByRace[ROWOW_RACE_MAX] = { }; // Percent of the target's DEF ignored
    int32 IgnoreDefByClass[ROWOW_CLASS_MAX] = { };
    int32 ExpAddRace[ROWOW_RACE_MAX] = { };
    int32 ExpAddClass[ROWOW_CLASS_MAX] = { };
    int32 WeaponAtk[ROWOW_WEAPON_TYPE_MAX] = { };
    int32 WeaponDamageRate[ROWOW_WEAPON_TYPE_MAX] = { };
    std::vector<std::pair<uint32, int32>> AddDefMonster; // add_def: percent less damage from a monster (mob_db Id)
    int32 HP = 0;
    int32 SP = 0;
    int32 HPRate = 100;
    int32 SPRate = 100;
    int32 HPRecovRate = 100;
    int32 SPRecovRate = 100;
    int32 UseSPRate = 100; // dsprate
    int32 CastRate = 100;
    int32 MatkRate = 100;
    int32 CriticalRate = 100;
    int32 HitRate = 100;
    int32 FleeRate = 100;
    int32 Flee2Rate = 100;
    int32 DefRate = 100;
    int32 Def2Rate = 100;
    int32 MdefRate = 100;
    int32 Mdef2Rate = 100;
    int32 AspdAdd = 0; // In tenths of a second of delay, negative is faster
    int32 ArrowHit = 0;
    int32 ArrowCri = 0;
    int32 CriticalRangeAtk = 0; // More critical for ranged attacks, in tenths
    int32 CriticalDef = 0;
    int32 NearAttackDefRate = 0;
    int32 LongAttackDefRate = 0;
    int32 MagicDefRate = 0;
    int32 MiscDefRate = 0;
    int32 DoubleRate = 0;
    int32 DoubleAddRate = 0;
    int32 PerfectHit = 0;
    int32 PerfectHitAdd = 0;
    int32 CritAtkRate = 0;
    int32 LongAttackAtkRate = 0;
    int32 ShortAttackAtkRate = 0;
    int32 SplashRange = 0;
    int32 SplashAddRange = 0;
    bool NoSizeFix = false;
    std::vector<ROWOWAddEffect> AddEff; // addeff: statuses its attacks may give
    std::vector<ROWOWAddEffect> AddEffAtked; // addeff_atked: statuses being hit may give the attacker
    std::vector<ROWOWAddEffectOnSkill> AddEffOnSkill; // addeff_onskill
    std::vector<ROWOWAutoSpell> AutoSpell; // autospell: bAutoSpell, cast as it attacks
    std::vector<ROWOWAutoSpell> AutoSpell2; // autospell2: bAutoSpellWhenHit, as it's hit
    std::vector<ROWOWAutoSpell> AutoSpell3; // autospell3: bAutoSpellOnSkill, as it uses a skill
    std::vector<ROWOWAutoBonus> AutoBonus; // autobonus, autobonus2 and autobonus3: scripts run for a while on the same triggers
    std::vector<ROWOWAutoBonus> AutoBonus2;
    std::vector<ROWOWAutoBonus> AutoBonus3;
    std::unordered_map<int32, int32> ResEff; // reseff: resistance to statuses, in hundredths of a percent, by ROWOW_SC_*
    std::vector<ROWOWDeferredBonus> Deferred;
};

// What natural regen gives (regen_data's flag): HP and SP, and the regen skills' (sregen, ssregen)
#define ROWOW_RGN_HP                                0x01
#define ROWOW_RGN_SP                                0x02
#define ROWOW_RGN_SHP                               0x04
#define ROWOW_RGN_SSP                               0x08

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
    uint8 RegenFlag = ROWOW_RGN_HP | ROWOW_RGN_SP; // What regenerates (regen_data's flag, from status_calc_regen_rate)
    int32 RegenRateHP = 100; // The regen speeds in percent (regen_data's rate), from statuses
    int32 RegenRateSP = 100;
    int32 SkillRegenHP = 0; // The regen skills' HP and SP every natural_heal_skill_interval (sregen: Increase HP and SP Recovery)
    int32 SkillRegenSP = 0;
    int32 SitSkillRegenHP = 0; // ... only while sitting (ssregen: Spiritual Cadence, and a Taekwon's Peaceful and Happy Break at rest)
    int32 SitSkillRegenSP = 0;
    bool IsRegenWalk = false; // regen_data's state.walk: HP regenerates while walking (Moving HP-Recovery), at half speed
    int32 Speed = ROWOW_DEFAULT_WALK_SPEED; // Walk speed, ms per cell
    ROWOWBonus Bonus; // A player's, from its gear
};

// A monster's own status while statuses change it (its battle status; the mob_db one stays its base status), kept on the creature as
// "ROWOWMobStatus" from its first status change.  Only the creature's map thread touches it
struct ROWOWMobStatus : public DataMap::Base
{
    ROWOWStatus Status;
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
    int32 BlewCount = 0; // blewcount: the cells a skill hit knocks the target back
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

// An item in the identity's cart (rAthena's items_cart entry), kept as what makes the RO item: its template, count and enchantments (its
// refine, broken and unidentified state, and its cards), since a cart item isn't an item in the bags
#define ROWOW_MAX_CART                              100     // MAX_CART
#define ROWOW_MAX_CARTS                             5       // MAX_CARTS: the cart looks (pre-renewal's five)
#define ROWOW_ITEM_ENCHANTMENT_SLOT_COUNT           12      // MAX_ENCHANTMENT_SLOT, the WoW item's enchantment slots
struct ROWOWCartItem
{
    uint32 ItemTemplateID = 0; // 0 for an empty slot
    uint32 Count = 0;
    std::array<uint32, ROWOW_ITEM_ENCHANTMENT_SLOT_COUNT> Enchantments = { };
};

// A shop a Merchant opened from its cart (rAthena's s_vending): the cart slot, how many and the price of each item for sale
#define ROWOW_MAX_VENDING                           12      // MAX_VENDING
struct ROWOWVendingItem
{
    uint16 CartSlot = 0;
    uint32 Amount = 0;
    uint32 Value = 0; // Zeny each
};

// A memorized Warp Portal destination (rAthena's s_point_str memo_point)
#define ROWOW_MAX_MEMOPOINTS                        3       // MAX_MEMOPOINTS
struct ROWOWMemoPoint
{
    uint32 MapID = 0; // 0 for none
    int32 CellX = 0;
    int32 CellY = 0;
};

// What opening the guild storage gives (e_guild_storage_open, storage.hpp)
enum ROWOWGuildStorageOpen : int32
{
    ROWOW_GSTORAGE_OPEN = 0,
    ROWOW_GSTORAGE_STORAGE_ALREADY_OPEN = 1,
    ROWOW_GSTORAGE_ALREADY_OPEN = 2,
    ROWOW_GSTORAGE_NO_GUILD = 3,
    ROWOW_GSTORAGE_NO_STORAGE = 4, // Here: the guild has no bank tab
    ROWOW_GSTORAGE_NO_PERMISSION = 5,
};

// The quest log (ROWOW_Quest.cpp, R8): rAthena's quest_db (the converter's mod_rowow_db_quest, from RoQuests.csv) and a character's quests
#define ROWOW_MAX_QUEST_OBJECTIVES                  3       // MAX_QUEST_OBJECTIVES

enum ROWOWQuestState : uint8
{
    ROWOW_QUEST_STATE_INACTIVE = 0, // Q_INACTIVE: the player turned it off in the quest window
    ROWOW_QUEST_STATE_ACTIVE = 1,   // Q_ACTIVE
    ROWOW_QUEST_STATE_COMPLETE = 2, // Q_COMPLETE: kept (checkquest still finds it), but no longer in the window
};

enum ROWOWQuestCheckType : uint8
{
    ROWOW_QUEST_CHECK_HAVEQUEST = 0, // HAVEQUEST: the quest's state
    ROWOW_QUEST_CHECK_PLAYTIME = 1,  // PLAYTIME: 2 when its time is up, 1 when complete, else 0
    ROWOW_QUEST_CHECK_HUNTING = 2,   // HUNTING: 2 when its hunting is done, 1 when its time is up, else 0
};

// A hunting target of a quest (s_quest_objective): kills of the monster to make
struct ROWOWQuestObjective
{
    uint32 MobID = 0;
    int32 Count = 0;
};

// A quest of quest_db (s_quest_db)
struct ROWOWQuestData
{
    uint32 ID = 0;
    std::string Title;
    int64 Time = 0; // Seconds it lasts (TimeLimit "+1d2h"), or the time of day it ends at with IsTimeAt; 0 for no limit
    bool IsTimeAt = false;
    int32 TimeWeek = -1; // The day of the week a time of day is on, -1 for any
    std::vector<ROWOWQuestObjective> Objectives;
};

// A quest in a character's quest log (rAthena's struct quest, mod_rowow_identity_quest)
struct ROWOWQuestEntry
{
    uint32 QuestID = 0;
    uint32 Time = 0; // When it runs out (Unix time), 0 for no limit
    uint8 State = ROWOW_QUEST_STATE_ACTIVE;
    int32 Counts[ROWOW_MAX_QUEST_OBJECTIVES] = { 0, 0, 0 }; // The kills made of each objective
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
    std::string SentCardsMessage; // ... and of the cards in the bags' items
    uint32 EquipmentCheckTimer = 0;
    uint32 CanUseItemTime = 0; // canuseitem_tick: when the next item can be used (game time, milliseconds)
    uint64 CanEquipTick = 0; // canequip_tick: no equipment goes on before it (Desperado), in the skill engine's ticks
    std::unordered_map<uint32, uint32> ItemDelayEndTimes; // By item_db Id: when each item with a reuse delay can be used again
    bool IsIdentifyMenuOpen = false; // A Magnifier's list of items to appraise is up (menuskill_id MC_IDENTIFY)
    uint8 StorageFlag = 0; // state.storage_flag: a Kafra opened its storage (1, the WoW bank) on this map, until it leaves the map
    std::map<uint16, uint16> SkillLevelsByID; // The skills learned (mod_rowow_identity_skill), and their levels
    bool AreSkillsChanged = false;
    std::set<uint16> TreeSkillIDs; // The skills of the tree it may use and raise now, learned or not (pc_calc_skilltree sets their id)
    uint16 CloneSkillID = 0; // cloneskill_idx: the skill Plagiarism copied (SKILL_FLAG_PLAGIARIZED, from CLONE_SKILL while it knows Plagiarism)
    uint16 CloneSkillLevel = 0; // ... and its level (CLONE_SKILL_LV, at most Plagiarism's)
    std::vector<uint32> SkillSpellIDs; // The learned skills' spells, which the character knows while in the form
    std::string SentSkillsMessage; // What the client's RO interface was last told of the skills
    ROWOWScriptVariables Variables; // Its character variables (rAthena's char_reg_num and char_reg_str, mod_rowow_identity_variable), like
                                    // jobchange_level and the NPC scripts' quest variables; saved when ChangedElements has any
    ROWOWScriptVariables AccountVariables; // Its account's (#name and ##name, mod_rowow_account_variable), loaded and saved with it
    ROWOWScriptVariables PlayerVariables; // @name: the NPC scripts' variables until logout
    std::unique_ptr<ROWOWScriptContext> NpcRun; // The NPC script run talking with it (one at a time), waiting for its answer
    std::deque<ROWOWNpcEvent> QueuedNpcEvents; // eventqueue: events for it that came during its run, run after it (npc_event_dequeue)
    uint32 NpcTouchMapID = 0; // The cell its OnTouch areas were last checked on (they're checked when it moves to another)
    int32 NpcTouchX = -1;
    int32 NpcTouchY = -1;
    std::vector<uint32> AreaNpcIDs; // areanpc: the NPCs whose OnTouch (or click) it set off and whose area it's still on
    std::vector<uint32> OnTouchNpcIDs; // npc_ontouch_: the NPCs whose OnTouch_ it set off and whose area it's still on
    uint32 NpcRangeCheckTimer = 0;
    uint32 WaitingRoomCheckTimer = 0;
    std::string SentWaitingRoomMessage; // What its client was last told of the waiting rooms in sight (cleared to send them again)
    int64 KillerRID = 0; // killerrid, killedrid and killedgid: who last killed it, and what it last killed (the die and kill events read them)
    int64 KilledRID = 0;
    int64 KilledGID = 0;
    std::vector<ROWOWQuestEntry> Quests; // quest_log: the quest log, the active and inactive quests first, then the completed ones
    bool AreQuestsChanged = false;
    std::array<ROWOWCartItem, ROWOW_MAX_CART> CartItems; // Its cart's items (mod_rowow_identity_cart), by cart slot
    int32 CartNum = 0; // cart_num: the slots in use
    int32 CartWeight = 0; // cart_weight and cart_weight_max, in tenths (item_db's Weight)
    int32 CartWeightMax = 0;
    bool AreCartItemsChanged = false;
    std::string SentCartMessage; // What the client's RO interface was last told of the cart
    std::array<ROWOWMemoPoint, ROWOW_MAX_MEMOPOINTS> MemoPoints; // memo_point (mod_rowow_identity_memo)
    bool AreMemoPointsChanged = false;
    uint16 MenuSkillID = 0; // menuskill_id and menuskill_val: a skill's menu is open (the Warp Portal or Teleport destinations, the Pushcart's
    int32 MenuSkillValue = 0; // shop setup), and what it was cast with (Warp Portal's cell, x << 16 | y)
    int32 MenuSkillValue2 = 0; // menuskill_val2: Weapon Repair's level
    ObjectGuid MenuSkillTargetGUID; // menuskill_val as a unit: whose items Weapon Repair's list shows
    float MenuSkillZ = 0; // The ground height of a Warp Portal's cell
    bool IsVending = false; // state.vending: its shop is open
    std::string VendingTitle; // message
    std::vector<ROWOWVendingItem> VendingItems; // vending (vend_num items)
    uint8 VendSkillLevel = 0; // vend_skill_lv
    std::vector<ROWOWActiveAutoBonus> ActiveAutoBonuses; // The autobonuses going (their active timers)
    uint64 InvincibleEndTime = 0; // invincible_timer: no monster or skill targets it until then (after arriving on a map or reviving), 0 for none
    float InvinciblePositionX = 0; // Where it stood when it got it, which moving away from ends it
    float InvinciblePositionY = 0;
    bool IsSitting = false; // pc_issit as skill_sit last saw it (sitting down and standing up, from the player's update)
    bool IsGangsterParadise = false; // state.gangsterparadise: sitting by another sitting Rogue, monsters leave it alone
    bool IsTaekwonRest = false; // state.rest: a Taekwon sitting with another (Peaceful Break, Happy Break's regen)
    std::map<uint16, uint16> TempSkillLevelsByID; // SKILL_FLAG_TEMPORARY: skills it has for now, unsaved and not raised (a spirit link's, a Bard's or
                                                  // Dancer's linked songs), worked out by pc_calc_skilltree
    uint8 SuperNoviceCallFlag = 0; // state.snovice_call_flag: how far a Super Novice is in the Guardian Angel prayer (its lines said)
    uint16 SpiritSkillID = 0; // The spirit linking it (its SC_SPIRIT's val2), which pc_calc_skilltree reads for the spirit skills
    bool IsSuperNoviceDeadFlag = false; // state.snovice_dead_flag: its angel saved it from death, until its next base level
    bool IsSellingToShop = false; // A sell packet is being handled: its money gets Overcharge
    uint32 Option = 0; // status.option: the options kept with the identity (a Knight's or Crusader's Peco, ROWOW_OPTION_RIDING), saved with it
    std::array<ObjectGuid, ROWOW_MAX_DEVOTION> DevotionGUIDs; // devotion: the players its Devotion protects, by slot (a slot holds while that
                                                              // player's SC_DEVOTION names this one and the slot)

    bool WasInFormAtLoad = false; // Saved in the form: logged out in the RO world, or the server stopped with the character in it

    ROWOWStatus BaseStatus; // base_status: from the job, stats, gear and skills (status_calc_pc_sub)
    ROWOWStatus Status; // battle_status: the base status with the statuses' changes (status_calc_bl_main); what the RO rules read
    std::vector<ROWOWSavedStatusChange> SavedStatusChanges; // The statuses kept while out of the form (sc_data), and as last saved
    bool AreStatusChangesChanged = false;
    uint32 RegenHPTimer = 0; // Time since the last natural HP and SP regen tick
    uint32 RegenSPTimer = 0;
    uint32 SkillRegenHPTimer = 0; // The regen skills' ticks (sregen and ssregen's tick), in ms scaled by the regen rate
    uint32 SkillRegenSPTimer = 0;
    uint32 SitSkillRegenHPTimer = 0;
    uint32 SitSkillRegenSPTimer = 0;

    // A form change made before the character was in the world (arriving on a map), finished once it's in the world
    bool IsFormArrivalPending = false;
    bool IsFormDeparturePending = false;
    bool IsNewIdentityPending = false; // The first entry, whose start items are given once in the world
    bool IsArrivalFloorCheckPending = false; // Arrived on an RO map: the next update checks it isn't left in the air (CheckArrivalFloor)

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

#define ROWOW_ARRIVAL_FLOOR_TOLERANCE               3.0f    // Yards above the floor an arrival may be before it's put down on it
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
    std::unordered_map<uint32, uint32> HeadSpellIDsByKey; // See GetHeadKey
    std::unordered_set<uint32> HeadSpellIDs;
    std::unordered_map<uint32, ROWOWMapGrid> MapGridsByMapID;
    std::unordered_map<std::string, ROWOWDataTable> DataTablesByName; // The rAthena data, by table name (ROWOW_DATA_TABLE_*)
    std::unordered_map<uint32, uint32> MobIDsByCreatureEntry; // The RO monster (mob_db Id) behind each RO creature template
    std::unordered_map<uint32, size_t> MobRowIndexesByMobID; // Rows of the mob table
    std::unordered_map<uint16, ROWOWJob> JobsByID;
    std::unordered_map<uint32, uint32> StatusPointsByBaseLevel; // Total status points at each base level (rAthena's statpoint.yml)
    std::unordered_map<std::string, uint32> MapIDsByMapName; // RO map names (rAthena's, including its aliases), lowercase
    std::unordered_map<uint32, std::string> MapNamesByMapID; // Each RO map's own name (not an alias), lowercase
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
    std::unordered_map<uint32, uint32> CardItemIDsByEnchantmentID;
    std::vector<ROWOWItemCombo> ItemCombos;
    std::unordered_map<uint32, std::vector<size_t>> ItemComboIndexesByItemID;
    ROWOWRefineBonus RefineBonuses[ROWOW_REFINE_TYPE_COUNT][ROWOW_MAX_REFINE + 1];
    std::map<std::string, std::set<uint32>> UnknownBonusItemIDs; // Bonus types no rule takes yet, and the items that have them (startup)
    uint32 CompilingItemID = 0; // The item whose script is being compiled (startup)
    std::unordered_map<uint16, ROWOWSkill> SkillsByID;
    std::unordered_map<std::string, uint16> SkillIDsByName; // By the skill_db Name in uppercase
    std::unordered_map<uint32, uint16> SkillIDsBySpellID; // The skill each RO skill spell is a level of
    std::unordered_map<uint32, uint16> SkillLevelsBySpellID; // ... and which level
    std::unordered_map<uint16, ROWOWSkillTree> SkillTreesByJobID;
    std::unordered_map<uint16, std::unique_ptr<ROWOWSkillImpl>> SkillImplsBySkillID; // Each skill's own rules (rAthena's skill_db impl)
    std::unordered_map<int32, ROWOWStatusChangeDB> StatusChangeDBsByType; // status.yml (status_db), by ROWOW_SC_*

    void LoadConfigurationFile();
    void LoadDataTables();
    void LoadCreatureMobData();
    void LoadIdentityData();
    void LoadStatusData();
    void LoadBattleData();
    void LoadCreatureTemplateData();
    void LoadIllusionData();
    void LoadMapGridData();

    // The RO cell grid of a map, or null when it isn't an RO map
    ROWOWMapGrid const* GetMapGrid(uint32 mapID) const;

    // RO's direction from one cell to another (rAthena's map_calc_dir_xy), east when they're the same cell
    static uint8 GetRODirection(float deltaX, float deltaY);
    static int32 MapCheckDir(int32 sDir, int32 tDir); // map_check_dir: 0 when tDir is sDir or one of its two neighbours, 1 otherwise

    // RO illusions: while one is the player's transform, the player shows its display for the weapon and shield held
    ROWOWIllusion const* GetActiveIllusion(Player* player) const;
    uint32 GetIllusionDisplayIDForPlayer(Player* player, ROWOWIllusion const& illusion) const;
    void RefreshIllusionDisplay(Player* player);

    // RO illusions show the headgear worn in the head, neck and shoulder slots through hidden auras that attach the headgear models
    static uint64 GetHeadgearKey(uint32 viewID, uint8 visualType, bool isMale) { return (uint64(viewID) << 16) | (uint64(visualType) << 1) | (isMale == true ? 1 : 0); }
    void RefreshIllusionHeadgear(Player* player, ROWOWIllusion const* illusion);

    // RO illusions show the head (the identity's hair style and color, RO's own head outside one) through a hidden aura that attaches
    // the head model, like headgear (R9.1).  Hair is the identity's: the stylists' setlook and the Options > Video > Ragnarok choice
    // (.ro hair) set it, and the RO_Identity addon hears it under the ROWOWHAIR prefix
    static uint32 GetHeadKey(uint32 style, uint32 color, bool isMale) { return (style << 16) | (color << 1) | (isMale == true ? 1 : 0); }
    uint32 GetHeadSpellID(uint32 style, uint32 color, bool isMale) const;
    void RefreshIllusionHead(Player* player, ROWOWIllusion const* illusion);
    bool SetIdentityHair(Player* player, uint32 style, uint32 color); // False if there's no head for it
    void SendHairToClient(Player* player);

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
    uint16 GetNonBabyJobID(uint16 jobID) const; // The job less JOBL_BABY (Baby_Monk's Monk, the job itself for the others)
    uint16 GetBabyJobID(uint16 normalJobID) const; // The Baby job of a normal job (Monk's Baby_Monk), 0 for none
    static uint8 GetItemClassMask(ROWOWJob const& job); // pc_isItemClass's class_upper bit of a job (ROWOW_ITEM_CLASS_*)
    uint32 GetMapIDByMapName(std::string const& mapName) const;
    bool IsROItemTemplate(uint32 itemTemplateID) const { return ItemTemplateIDs.find(itemTemplateID) != ItemTemplateIDs.end(); }

    // The middle of an RO cell of an RO map, on the ground.  False when the map has no cell grid or no ground there
    bool GetCellPosition(uint32 mapID, int32 cellX, int32 cellY, ROWOWPosition& positionOut) const;
    void CheckArrivalFloor(Player* player); // After an arrival on an RO map: down on the floor if it's in the air above it

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
    void RemoveROFormAuras(Player* player); // The job's look, its head and headgear
    bool IsSpellKeptInROForm(SpellInfo const* spellInfo) const;
    bool IsROCompanionPetSpell(SpellInfo const* spellInfo) const; // An RO companion pet's summon, which the form keeps
    bool IsROCompanionPetCarrier(ItemTemplate const* itemTemplate) const; // An item teaching an RO companion pet, which the form can use

    // RO status (ROWOW_Status.cpp): the port of rAthena's pre-renewal status calculation for a player (status.cpp)
    void CalculateIdentityStatus(Player* player, ROWOWIdentity& identity) const; // status_calc_pc_sub
    int32 StatusBaseAmotionPC(ROWOWIdentity const& identity, ROWOWStatus const& status) const; // status_base_amotion_pc
    int32 StatusBaseAtk(ROWOWStatus const& status) const; // status_base_atk
    void StatusCalcMisc(ROWOWStatus& status, uint32 level) const; // status_calc_misc
    static int32 GetBattleObjectType(ROWOWStatus const& status) { return status.IsPlayer == true ? ROWOW_BL_PC : ROWOW_BL_MOB; }
    static int32 GetElementByName(std::string const& name); // An element (Ele_* without the prefix, any case), or -1
    uint32 StatusCalcMaxHPPC(ROWOWIdentity const& identity, ROWOWStatus const& status, ROWOWStatusChange const* sc) const; // status_calc_maxhp_pc
    uint32 StatusCalcMaxSPPC(ROWOWIdentity const& identity, ROWOWStatus const& status, ROWOWStatusChange const* sc) const; // status_calc_maxsp_pc
    void StatusCalcRegen(Unit const* unit, ROWOWStatus& status) const; // status_calc_regen, for a player (its regen skills when the unit is given)
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
    void PCLostExp(Player* player, uint64 baseExp, uint64 jobExp); // pc_lostexp

    // An RO monster's exp (mob.cpp): who hurt it how much (mob_log_damage), and its exp shared out to them when it dies (mob_dead)
    void MobLogDamage(Creature* creature, Unit* attacker, uint32 damage);
    void MobDeadExp(Creature* creature, Unit* killer);

    // RO natural HP and SP regen (status_natural_heal), in place of WoW's while in the form
    void UpdateNaturalRegen(Player* player, uint32 diff);

    // RO battle (ROWOW_Battle.cpp): the port of rAthena's pre-renewal battle code (battle.cpp) for combat between RO combatants: the normal
    // attack and the weapon, magic and misc attacks of skills.  Statuses (R6.4) and the skill cases of rAthena's switches (R6.5) plug into
    // the marked places
    ROWOWStatus const* GetROStatus(Unit const* unit) const; // status_get_status_data: a player's form, or a monster's mob_db status
    int32 PCCheckSkill(Unit const* unit, uint16 skillID) const; // pc_checkskill: the level a player in the form knows a skill at
    void ApplyMobStatus(Creature* creature); // A spawning RO monster's HP, SP and attack delay (status_calc_mob)
    bool CalculateMeleeDamage(Unit* attacker, Unit* victim, CalcDamageInfo* damageInfo); // An auto attack swing between RO combatants
    uint8 BattleWeaponAttack(Unit* src, Unit* target, ROWOWDamage& wdOut); // battle_weapon_attack, a normal attack
    bool HasNormalAttackAmmo(Player* player) const; // Whether a bow or gun has its ammo (true for every other weapon)
    // The attacks change the statuses they meet (Kyrie Eleison's hits, Lex Aeterna, Auto Counter), so they aren't const
    ROWOWDamage BattleCalcAttack(int32 attackType, Unit* src, Unit* target, uint16 skillID, uint16 skillLevel, int32 flag); // battle_calc_attack
    ROWOWDamage BattleCalcWeaponAttack(Unit* src, Unit* target, uint16 skillID, uint16 skillLevel, int32 wflag); // battle_calc_weapon_attack
    ROWOWDamage BattleCalcMagicAttack(Unit* src, Unit* target, uint16 skillID, uint16 skillLevel, int32 mflag); // battle_calc_magic_attack
    ROWOWDamage BattleCalcMiscAttack(Unit* src, Unit* target, uint16 skillID, uint16 skillLevel, int32 mflag); // battle_calc_misc_attack
    static bool BattleCheckUndead(int32 race, int32 element); // battle_check_undead
    bool IsSkillUsingArrow(Unit const* src, uint16 skillID) const; // is_skill_using_arrow (sd->state.arrow_atk: a bow or gun's normal attack, or a skill using ammo)
    int64 BattleAttrFix(Unit* src, Unit* target, int64 damage, int32 atkElement, int32 defElement, int32 defElementLevel, int32 flag); // battle_attr_fix (either unit may be null)
    int32 BattleGetSpiritBallDamage(ROWOWDamage const& wd, Unit const* src, uint16 skillID) const; // battle_get_spiritball_damage
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
    bool GiveROItem(Player* player, uint32 itemID, uint32 amount, bool isIdentified, Item** itemOut = nullptr); // pc_additem, mailing what doesn't fit
    Item* PCSearchInventory(Player* player, uint32 itemID) const; // pc_search_inventory: an item of an item_db Id in the bags (identified, not worn)
    void SkillIdentify(Player* player, Item* item); // skill_identify
    void SendItemIdentifyList(Player* player); // clif_item_identify_list
    void TeleportToRandomSpot(Player* player); // A random spot on the map (AL_TELEPORT level 1), at one of its monsters' homes
    bool MapSearchRandomSpot(Map* map, Position& positionOut) const; // map_search_freecell over the whole map: one of its monsters' homes
    void TeleportToSavePoint(Player* player); // The identity's save point (AL_TELEPORT level 2 and up)

    // RO bonuses and cards (ROWOW_Bonus.cpp): the port of rAthena's pc_bonus..pc_bonus5 and the item part of status_calc_pc_sub, the
    // scripts of the worn items, their cards and combos, refine, card compounding (pc_insert_card) and battle_calc_cardfix
    void LoadBonusData();
    void StatusCalcPCItems(Player* player, ROWOWIdentity const& identity, ROWOWStatus& status) const; // Runs the gear's scripts into status
    void PCBonus(ROWOWScriptContext& context, std::string const& type, std::vector<int64> const& values) const; // pc_bonus..pc_bonus5
    static uint32 GetItemCard(Item const* item, uint8 slot); // The card (item_db Id) in a card slot, 0 for none
    void SetItemCard(Player* player, Item* item, uint8 slot, uint32 cardItemID) const;
    bool PCInsertCard(Player* player, Item* card, Item* equipment); // pc_insert_card
    void SendCardTargetsToClient(Player* player, Item* card); // clif_use_card: the items a card can go into (ROWOWCARDLIST)
    void SendItemCardsToClient(Player* player, ROWOWIdentity& identity, bool isForced); // The cards in the bags' items (ROWOWCARDS)
    int64 BattleCalcCardFix(int32 attackType, Unit* src, Unit* target, std::bitset<ROWOW_NK_MAX> nk, uint16 skillID, int32 rhEle, int32 lhEle, int64 damage, int32 left,
        int32 flag) const; // battle_calc_cardfix
    void RunItemScript(Player* player, ROWOWScript const* script, Item const* item); // An EquipScript or UnEquipScript

    // Keeps the worn items true to the bags (one sold, moved to the bank or destroyed is no longer worn) and tells the client's RO interface
    // where each worn item is, under the ROWOWEQUIP prefix, when that changes
    void UpdateIdentityEquipment(Player* player, ROWOWIdentity& identity);
    void SendEquipmentToClient(Player* player, ROWOWIdentity& identity, bool isForced);
    static bool GetClientItemPosition(Item const* item, uint32& bagOut, uint32& slotOut); // The client's bag (0-4) and slot (from 1)
    static Item* GetItemByClientPosition(Player* player, uint32 bag, uint32 slot);

    // RO skills (ROWOW_Skills.cpp): the port of rAthena's skill tree and skill learning (pc.cpp).  The identity's learned skills are
    // known as their WoW spells while in the form, granted for the form only (never saved with the character's own spells)
    void LoadSkillData();
    void LoadSkillRow(ROWOWDataTable const& skillTable, size_t rowIndex, ROWOWSkill& skill) const; // A skill_db row's values for the engine
    ROWOWSkill const* GetSkill(uint16 skillID) const;
    uint16 GetSkillIDByName(std::string const& skillName) const; // 0 for none
    ROWOWSkillTreeEntry const* GetSkillTreeEntry(uint16 jobID, uint16 skillID) const;
    int32 PCCheckSkill(ROWOWIdentity const& identity, uint16 skillID) const; // pc_checkskill
    uint16 SkillTreeGetMax(uint16 skillID, uint16 jobID) const; // skill_tree_get_max
    uint32 PCCalcSkillPoint(ROWOWIdentity const& identity) const; // pc_calc_skillpoint: the points spent
    uint16 PCCalcSkillTreeNormalizeJob(ROWOWIdentity const& identity) const; // pc_calc_skilltree_normalize_job: the job whose tree it may use
    void PCCalcSkillTree(ROWOWIdentity& identity) const; // pc_calc_skilltree
    bool PCSkillUp(Player* player, uint16 skillID); // pc_skillup
    void PCSkill(Player* player, uint16 skillID, uint16 level); // pc_skill (ADDSKILL_PERMANENT): a quest NPC or a GM sets a skill's level
    void PCJobChange(Player* player, uint16 jobID); // pc_jobchange
    int64 PCJobIDToMapID(int32 jobID) const; // pc_jobid2mapid: the job's class (MAPID, EAJ_), -1 for none
    int32 PCMapIDToJobID(uint64 mapID, bool isMale) const; // pc_mapid2jobid: -1 for none
    int32 PCResetSkill(Player* player); // pc_resetskill (flag 1): the skill points it gave back
    void PCResetState(ROWOWIdentity& identity) const; // pc_resetstate: the stats back to 1 and their points back (the caller refreshes the form)
    void PCResetLvl(Player* player, int32 type); // pc_resetlvl: rebirth (1), or base and job level 1 (2), base (3) or job (4) level 1
    std::vector<uint16> GetSkillTreeJobIDs(uint16 jobID) const; // The jobs whose skills the skill window shows, Novice first
    int64 GetIdentityVariable(ROWOWIdentity const& identity, std::string const& name) const;
    void SetIdentityVariable(ROWOWIdentity& identity, std::string const& name, int64 value) const;
    void RefreshIdentitySkills(Player* player, ROWOWIdentity& identity); // The tree, then the skill spells the form knows
    void GrantIdentitySkillSpells(Player* player, ROWOWIdentity& identity); // The learned skills' spells while in the form, none otherwise
    bool IsIdentitySkillSpell(Player* player, uint32 spellID) const; // A spell of a skill the form knows
    void SendSkillsToClient(Player* player, ROWOWIdentity& identity, bool isForced); // The skill window's tree (ROWOWSKILLS)

    // The RO skill engine (ROWOW_SkillEngine.cpp): the port of rAthena's skill use (skill.cpp, and unit.cpp's casting).  An RO skill level is
    // a WoW spell that only shows the cast; ROWOW_SkillSpellScript hands it here: the cast conditions and costs, the cast time and after-cast
    // delay, the castend dispatch to the skill's ROWOWSkillImpl (one class per skill, registered by job), skill_attack with its delivery and
    // knockback, area searches in RO cells and healing.  Any RO combatant casts through it (a player in the form or an RO monster); it runs
    // in the map thread the caster and its targets share, and only reads the skill data loaded at startup
    void RegisterSkillImpl(ROWOWSkillImpl* impl); // Takes it over (startup only)
    ROWOWSkillImpl const* GetSkillImpl(uint16 skillID) const;
    bool GetSkillOfSpell(uint32 spellID, uint16& skillIDOut, uint16& skillLevelOut) const; // The skill and level of an RO skill spell
    uint32 GetSkillSpellID(uint16 skillID, uint16 skillLevel) const; // 0 when the level has no spell
    static uint64 GetTick(); // gettick: game time in ms
    ROWOWSkillUnitData* GetSkillUnitData(Unit* unit) const; // unit_bl2ud
    Unit* MapID2BL(Unit* origin, int32 id) const; // map_id2bl for an id a status keeps (a GUID's counter), near the origin
    void GetUnitCell(Unit const* unit, int32& xOut, int32& yOut) const; // The RO cell a unit stands on
    void GetWorldCell(uint32 mapID, float worldX, float worldY, int32& xOut, int32& yOut) const;
    int32 DistanceBL(Unit const* src, Unit const* target) const; // distance_bl, in cells
    int32 DistanceMathBL(Unit const* src, Unit const* target) const; // distance_math_bl, in cells
    int32 SkillGetRange2(Unit const* unit, uint16 skillID, uint16 skillLevel, bool isServer) const; // skill_get_range2
    uint8 SkillGetCastType(uint16 skillID) const; // skill_get_casttype
    int32 BattleCheckTarget(Unit const* src, Unit const* target, int32 flag) const; // battle_check_target
    bool StatusCheckSkillUse(Unit const* src, Unit const* target, uint16 skillID, int32 flag) const; // status_check_skilluse
    int32 MapForEachInArea(Unit* origin, int32 x0, int32 y0, int32 x1, int32 y1, std::function<int32(Unit*)> const& func) const; // map_foreachinarea (BL_CHAR)
    int32 MapForEachInRange(Unit* center, int32 range, std::function<int32(Unit*)> const& func) const; // map_foreachinallrange (BL_CHAR)
    int32 MapForEachInDir(Unit* origin, int32 x0, int32 y0, int32 x1, int32 y1, int32 range, int32 length, int32 offset,
        std::function<int32(Unit*)> const& func) const; // map_foreachindir (BL_CHAR): a path of cells from x0,y0 toward x1,y1
    int32 SkillAreaSub(Unit* bl, Unit* src, uint16 skillID, uint16 skillLevel, uint64 tick, int32 flag, ROWOWSkillAreaFunc func); // skill_area_sub
    int32 PartyForEachSameMap(Player* sd, int32 range, std::function<int32(Unit*)> const& func) const; // party_foreachsamemap (ROWOW_Skills.cpp)
    bool PCCheckWeaponType(Unit const* unit, uint32 weapons) const; // pc_check_weapontype: whether the player holds one of the weapon types
    void PCCheckAllowSkill(Player* player); // pc_checkallowskill: ends the statuses the equipment worn no longer allows
    ROWOWSkillCondition SkillGetRequirement(Player* sd, uint16 skillID, uint16 skillLevel) const; // skill_get_requirement
    bool SkillCheckConditionCastBegin(Player* sd, uint16 skillID, uint16 skillLevel); // skill_check_condition_castbegin
    bool SkillCheckConditionCastEnd(Player* sd, uint16 skillID, uint16 skillLevel); // skill_check_condition_castend
    void SkillConsumeRequirement(Player* sd, uint16 skillID, uint16 skillLevel, int16 type); // skill_consume_requirement
    int32 SkillCastFix(Unit* bl, uint16 skillID, uint16 skillLevel, bool isCastBegin = false); // skill_castfix (isCastBegin: the cast starts, Foresight counts it)
    int32 SkillCastFixSC(Unit* bl, double time, uint8 flag); // skill_castfix_sc
    int32 SkillDelayFix(Unit* bl, uint16 skillID, uint16 skillLevel) const; // skill_delayfix
    int32 SkillCastendDamageID(Unit* src, Unit* bl, uint16 skillID, uint16 skillLevel, uint64 tick, int32 flag); // skill_castend_damage_id
    int32 SkillCastendNoDamageID(Unit* src, Unit* bl, uint16 skillID, uint16 skillLevel, uint64 tick, int32 flag); // skill_castend_nodamage_id
    int32 SkillCastendPos2(Unit* src, int32 x, int32 y, uint16 skillID, uint16 skillLevel, uint64 tick, int32 flag); // skill_castend_pos2
    int64 SkillAttack(int32 attackType, Unit* src, Unit* dsrc, Unit* bl, uint16 skillID, uint16 skillLevel, uint64 tick, int32 flag,
        ROWOWSkillUnit const* dsrcUnit = nullptr); // skill_attack (dsrcUnit: a ground skill's unit is the damage's source, dsrc being its caster)
    void SkillAttackBlow(Unit* src, Unit* dsrc, Unit* target, uint8 blewcount, uint16 skillID, uint16 skillLevel, int64 damage, uint64 tick, int32 flag,
        ROWOWSkillUnit const* dsrcUnit = nullptr); // skill_attack_blow
    static int32 SkillIsCombo(uint16 skillID); // skill_is_combo: 1 a combo skill, 2 one that auto-targets even with a target set, 0 none
    void SkillCombo(Unit* src, Unit* dsrc, Unit* bl, uint16 skillID, uint16 skillLevel, uint64 tick); // skill_combo: the combo window a hit opens
    bool UnitCancelCombo(Unit* bl); // unit_cancel_combo: the combo window ended (a Taekwon kick, a wrong kick tried)
    int32 SkillBlown(Unit* src, Unit* target, int32 count, int8 dir, int32 flag, ROWOWSkillUnit const* srcUnit = nullptr); // skill_blown, in RO cells (from a unit's cell)
    bool UnitMovePos(Unit* bl, int32 x, int32 y); // skill_check_unit_movepos and unit_movepos: the unit slides onto an RO cell at once
    bool UnitJumpPos(Unit* bl, int32 x, int32 y); // unit_movepos without its path check: the unit jumps onto an RO cell (Leap)
    void BattleConsumeAmmo(Player* sd, uint16 skillID, uint16 skillLevel); // battle_consume_ammo: a normal attack's or skill's ammo
    int32 SkillAdditionalEffect(Unit* src, Unit* bl, uint16 skillID, uint16 skillLevel, int32 attackType, uint8 dmgLv, uint64 tick); // skill_additional_effect
    int32 SkillCounterAdditionalEffect(Unit* src, Unit* bl, uint16 skillID, uint16 skillLevel, int32 attackType, uint64 tick); // skill_counter_additional_effect
    int32 SkillCalcHeal(Unit* src, Unit* target, uint16 skillID, uint16 skillLevel, bool heal) const; // skill_calc_heal
    int64 StatusHeal(Unit* bl, int64 hp, int64 sp, int32 flag, Unit* src, uint32 spellID); // status_heal, shown as the spell's heal
    void BattleDelayDamage(uint64 tick, int32 amotion, Unit* src, Unit* target, int32 attackType, uint16 skillID, uint16 skillLevel, int64 damage, uint8 dmgLv,
        int16 div, bool additionalEffects, bool isCritical); // battle_delay_damage
    void BattleDamage(Unit* src, Unit* target, int64 damage, int16 div, uint16 skillLevel, uint16 skillID, uint8 dmgLv, int32 attackType, bool additionalEffects,
        uint64 tick, bool isCritical); // battle_damage
    void ClifSkillFail(Player* sd, uint16 skillID, int32 cause, int32 btype = 0, uint32 itemID = 0) const; // clif_skill_fail: RO's message
    bool BattleCheckComa(Unit const* src, Unit const* target, int32 attackType) const; // battle_check_coma: the coma bonuses' chance
    void UnitSetAttackDelay(Unit* unit, uint64 tick, bool isCastBegin); // unit_set_attackdelay, for a normal attack or a cast's start

    // The cast as ROWOW_SkillSpellScript drives it: unit_skilluse_id2/unit_skilluse_pos2 when it starts (the cast time comes from the core's
    // cast time hook), then skill_castend_id/skill_castend_pos when it ends.  Damage taken stops it (unit_skillcastcancel)
    SpellCastResult UnitSkillUse(Unit* src, Unit* target, WorldLocation const* dest, uint16 skillID, uint16 skillLevel);
    Unit* UnitSkillTarget(Unit* src, Unit* target, Unit* pickedTarget, uint16 skillID); // unit_skilluse_id2's target: a combo's (auto-target), Blade Stop's
    int32 UnitSkillCastTimeFix(Unit* src, Unit* target, uint16 skillID, uint16 skillLevel, int32 casttime); // unit_skilluse_id2's cast time switch (-1: instant)
    int32 UnitSkillCastTime(Unit* src, Unit* target, uint16 skillID, uint16 skillLevel); // The cast time, and the delays set at cast begin
    SpellCastResult SkillCastEndCheck(Unit* src, Unit* target, WorldLocation const* dest, uint16 skillID, uint16 skillLevel);
    void SkillCastEnd(Unit* src, Unit* target, WorldLocation const* dest, uint16 skillID, uint16 skillLevel);
    void UnitSkillCastCancel(Unit* bl, int32 type); // unit_skillcastcancel
    void UpdateSkillCastMove(Player* player); // A walk during a cast stops it, but with Free Cast (unit_walktoxy's check), from the player's update
    void SendSkillDelayToClient(Unit* unit, uint16 skillID) const; // The after-cast delay and the skill's cooldown, on the action bars

    // Ground skill units (ROWOW_SkillUnit.cpp): the port of rAthena's skill units (skill.cpp's skill_unitsetting, skill_initunitgroup,
    // skill_initunit, skill_delunit, skill_delunitgroup, skill_unit_onplace, skill_unit_onplace_timer, skill_unit_onout, skill_unit_onleft,
    // skill_unit_effect, skill_unit_timer and skill_unit_move).  A map's unit groups live on the map; skill_unit_timer runs from the map's
    // update every 100 ms, and skill_unit_move from each RO combatant's update when its cell changes.  A unit shows on the ground as a
    // dynamic object of the skill's unit spell (each cell, or one over the group)
    std::vector<ROWOWSkillUnitLayout> SkillUnitLayouts; // skill_unit_layout: the square layouts, then the skills' own (startup)
    int32 FirewallUnitPos = 0; // firewall_unit_pos and icewall_unit_pos: their 8 layouts by direction
    int32 IcewallUnitPos = 0;
    void LoadSkillUnitData(); // skill_init_unit_layout and the units' spells (after LoadSkillData)
    ROWOWMapSkillUnits* GetMapSkillUnits(Map* map, bool isCreated) const;
    std::shared_ptr<ROWOWSkillUnitGroup> SkillID2Group(Map* map, int32 groupID) const; // skill_id2group, on the map
    ROWOWSkillUnitLayout const* SkillGetUnitLayout(uint16 skillID, uint16 skillLevel, Unit const* src, int32 x, int32 y) const; // skill_get_unit_layout
    int32 SkillGetUnitLayoutType(uint16 skillID, uint16 skillLevel) const; // skill_get_unit_layout_type
    std::shared_ptr<ROWOWSkillUnitGroup> SkillUnitSetting(Unit* src, uint16 skillID, uint16 skillLevel, int32 x, int32 y, int32 flag); // skill_unitsetting
    std::shared_ptr<ROWOWSkillUnitGroup> SkillInitUnitGroup(Unit* src, int32 count, uint16 skillID, uint16 skillLevel, uint16 unitID, int64 limit,
        int32 interval); // skill_initunitgroup
    ROWOWSkillUnit* SkillInitUnit(Unit* src, ROWOWSkillUnitGroup& group, int32 index, int32 x, int32 y, int32 val1, int32 val2, bool hidden, int32 range,
        int64 limit); // skill_initunit
    void SkillDelUnit(Map* map, ROWOWSkillUnitGroup& group, int32 index); // skill_delunit
    void SkillDelUnitGroup(Map* map, std::shared_ptr<ROWOWSkillUnitGroup> group); // skill_delunitgroup
    void SkillClearUnitGroup(Unit* src); // skill_clear_unitgroup
    ROWOWSkillUnitGroupTickset* SkillUnitGroupTicksetSearch(Unit* bl, ROWOWSkillUnitGroup const& group, uint64 tick); // skill_unitgrouptickset_search
    int32 SkillUnitOnPlace(Map* map, ROWOWSkillUnitGroup& group, int32 index, Unit* bl, uint64 tick); // skill_unit_onplace
    int32 SkillUnitOnPlaceTimer(Map* map, ROWOWSkillUnitGroup& group, int32 index, Unit* bl, uint64 tick); // skill_unit_onplace_timer
    int32 SkillUnitOnOut(ROWOWSkillUnitGroup& group, int32 index, Unit* bl, uint64 tick); // skill_unit_onout
    int32 SkillUnitOnLeft(uint16 skillID, Unit* bl, uint64 tick); // skill_unit_onleft
    void SkillUnitEffect(Map* map, Unit* bl, ROWOWSkillUnitGroup& group, int32 index, uint64 tick, int32 flag); // skill_unit_effect
    void SkillUnitTimer(Map* map, uint32 diff); // skill_unit_timer, from the map's update
    void SkillUnitMove(Unit* bl, uint64 tick, int32 flag, int32 cellX, int32 cellY, std::vector<uint16>& skillUnitCell); // skill_unit_move, at a cell of the unit's map
    void UpdateSkillUnitMove(Unit* unit); // skill_unit_move as the unit's cell changes, from its update
    int32 BattleCheckTargetSkillUnit(ROWOWSkillUnitGroup const& group, Map* map, Unit const* target, int32 flag) const; // battle_check_target from a skill unit
    int32 SkillCheckUnitRange(Unit* bl, int32 x, int32 y, uint16 skillID, uint16 skillLevel); // skill_check_unit_range
    int32 SkillCheckUnitRange2(Unit* bl, int32 x, int32 y, uint16 skillID, uint16 skillLevel); // skill_check_unit_range2 (UF_NOFOOTSET)
    bool SkillPosMaxcountCheck(Unit* src, int32 x, int32 y, uint16 skillID, uint16 skillLevel, bool displayFailure); // skill_pos_maxcount_check
    int32 SkillCellOverlap(Map* map, ROWOWSkillUnitGroup& group, int32 index, uint16 skillID, int32& alive); // skill_cell_overlap
    int32 MapForEachSkillUnitInArea(Map* map, int32 x0, int32 y0, int32 x1, int32 y1, std::function<int32(ROWOWSkillUnitGroup&, int32)> const& func); // map_foreachinallarea (BL_SKILL)
    int32 MapForEachInCellArea(Map* map, uint32 phaseMask, int32 x0, int32 y0, int32 x1, int32 y1, std::function<int32(Unit*)> const& func) const; // map_foreachinallarea (BL_CHAR), by cells
    bool GetCellWorldPosition(uint32 mapID, int32 cellX, int32 cellY, float& xOut, float& yOut) const; // An RO cell's middle in WoW's coordinates
    static uint8 UnitGetDir(Unit const* unit); // unit_getdir: the RO direction a unit faces

    // The cart (ROWOW_Cart.cpp): the port of rAthena's cart (pc.cpp's pc_setcart, pc_cart_additem, pc_cart_delitem, pc_putitemtocart,
    // pc_getitemfromcart, status_calc_cart_weight), the Pushcart skill's status (SC_PUSH_CART) and the cart window (ROWOWCART).  Its items
    // are kept as their templates and enchantments in mod_rowow_identity_cart, and only touched in the player's own thread
    bool PCIsCartOn(Unit const* unit) const; // pc_iscarton
    bool PCSetCart(Player* player, int32 type); // pc_setcart
    void StatusCalcCartWeight(Player* player, ROWOWIdentity& identity) const; // status_calc_cart_weight
    int32 PCCartAddItem(Player* player, ROWOWIdentity& identity, ROWOWCartItem const& item, int32 amount); // pc_cart_additem (e_additem_result)
    void PCCartDelItem(Player* player, ROWOWIdentity& identity, int32 slot, int32 amount); // pc_cart_delitem
    void PCPutItemToCart(Player* player, Item* item, int32 amount); // pc_putitemtocart
    bool PCGetItemFromCart(Player* player, int32 slot, int32 amount); // pc_getitemfromcart
    void SendCartToClient(Player* player, ROWOWIdentity& identity, bool isForced); // clif_cartlist and SP_CARTINFO (ROWOWCART)
    void LoadIdentityCart(Player* player, ROWOWIdentity& identity) const;
    void SaveIdentityCart(Player* player, ROWOWIdentity& identity, CharacterDatabaseTransaction trans) const;

    // Vending (ROWOW_Vending.cpp): the port of rAthena's vending.cpp: a Merchant opens a shop of its cart's items (vending_openvending),
    // others list it (vending_vendinglistreq) and buy from it (vending_purchasereq), and it closes (vending_closevending).  The buyer and
    // the vendor are on the same map; everything runs in that map's thread
    void VendingOpenVendingReq(Player* player, uint8 skillLevel); // clif_openvendingreq (ROWOWVENDREQ)
    bool VendingOpenVending(Player* player, std::string const& title, std::vector<ROWOWVendingItem> const& items); // vending_openvending
    void VendingCloseVending(Player* player); // vending_closevending
    void VendingVendingListReq(Player* player, Player* vendor); // vending_vendinglistreq (ROWOWVENDLIST)
    void VendingPurchaseReq(Player* player, Player* vendor, std::vector<std::pair<uint16, uint32>> const& purchases); // vending_purchasereq (index, amount)

    // Warp Portal memo points and the warp point menu (ROWOW_Warp.cpp): pc_memo, clif_skill_warppoint (ROWOWWARP) and skill_castend_map
    bool PCMemo(Player* player, int32 pos); // pc_memo
    void ClifSkillWarpPoint(Player* player, uint16 skillID, uint16 skillLevel, std::vector<uint32> const& mapIDs); // clif_skill_warppoint
    void SkillCastendMap(Player* player, uint16 skillID, std::string const& mapName); // skill_castend_map
    void LoadIdentityMemo(Player* player, ROWOWIdentity& identity) const;
    void SaveIdentityMemo(Player* player, ROWOWIdentity& identity, CharacterDatabaseTransaction trans) const;

    // Stealing (pc.cpp's pc_steal_item)
    bool PCStealItem(Player* player, Unit* bl, uint16 skillLevel); // pc_steal_item

    // Making items with skills (ROWOW_Produce.cpp): skill_produce_mix with produce_db (forging, smelting, brewing, Holy Water, the Elemental
    // Converters) and its list (ROWOWPRODUCE), mending and refining weapons (skill_repairweapon, skill_weaponrefine and their lists,
    // ROWOWREPAIR and ROWOWREFINE), and a forged weapon's state.  Arrow Crafting's skill_arrow_create with its list, ROWOWARROW
    std::vector<ROWOWProduce> ProduceDB; // skill_produce_db (startup)
    void LoadProduceData(); // produce_db, after the items
    int32 SkillCanProduceMix(Player* player, uint32 nameID, int32 trigger, int32 qty); // skill_can_produce_mix: the recipe's index plus 1, 0 for none
    bool SkillProduceMix(Player* player, uint16 skillID, uint32 nameID, int32 slot1, int32 slot2, int32 slot3, int32 qty, int32 produceIndex); // skill_produce_mix
    int32 SkillProduceMixRate(Player* player, uint16 skillID, uint32 nameID, int32 sc, int32 ele); // skill_produce_mix's make_per for one try (in 10000)
    void ClifSkillProduceMixList(Player* player, int32 skillID, int32 trigger); // clif_skill_produce_mix_list (ROWOWPRODUCE; skillID -1 for an item's list)
    void ClifParseProduceMix(Player* player, uint32 nameID, uint32 slot1, uint32 slot2, uint32 slot3); // clif_parse_ProduceMix: the list's pick
    void ClifItemRepairList(Player* sd, Player* dstsd, uint16 skillLevel); // clif_item_repair_list (ROWOWREPAIR)
    void SkillRepairWeapon(Player* sd, ObjectGuid::LowType itemGUID); // skill_repairweapon: the repair list's pick
    void ClifItemRefineList(Player* sd); // clif_item_refine_list (ROWOWREFINE)
    void SkillWeaponRefine(Player* sd, ObjectGuid::LowType itemGUID); // skill_weaponrefine: the refine list's pick
    static bool GetItemForge(Item const* item, int32& starCrumbsOut, int32& elementOut); // A forged weapon's Star Crumbs and element (ROWOW_ELEMENT_*)
    void ClifArrowCreateList(Player* player); // clif_arrow_create_list
    bool SkillArrowCreate(Player* player, uint32 nameID); // skill_arrow_create

    // Autospells and autobonuses (ROWOW_AutoSpell.cpp): the items' bAutoSpell, bAutoSpellWhenHit and bAutoSpellOnSkill bonuses (the
    // autospell parts of skill_additional_effect, skill_counter_additional_effect and skill_onskillusage) and autobonus/autobonus2/
    // autobonus3 (pc_addautobonus, pc_exeautobonus, pc_endautobonus): an item's script run for a while, compiled at startup
    void SkillAutoSpellAdditionalEffect(Unit* src, Unit* bl, uint16 skillID, int32 attackType, uint64 tick); // skill_additional_effect's autospells
    void SkillAutoSpellCounterAdditionalEffect(Unit* src, Unit* bl, uint16 skillID, int32 attackType, uint64 tick); // skill_counter_additional_effect's
    void SkillOnSkillUsage(Player* player, Unit* bl, uint16 skillID, uint64 tick); // skill_onskillusage
    bool SkillAutoCast(Unit* src, Unit* target, uint16 skillID, uint16 skillLevel, uint64 tick); // An autospell's cast: the castend with no SP cost
    void PCExeAutoBonus(Player* player, ROWOWAutoBonus const& autoBonus); // pc_exeautobonus
    void UpdateAutoBonuses(Player* player); // pc_endautobonus as the bonuses run out, from the player's update
    void StatusCalcPCAutoBonuses(Player* player, ROWOWIdentity const& identity, ROWOWStatus& status) const; // The running autobonuses' scripts
    std::shared_ptr<ROWOWScript> GetAutoBonusScript(std::string const& text) const; // An autobonus's script, compiled at startup
    std::set<std::string> AutoBonusScriptTexts; // The autobonus scripts the item scripts name (script_add_autobonus), compiled once at startup
    std::unordered_map<std::string, std::shared_ptr<ROWOWScript>> AutoBonusScriptsByText;

    // Equipment breaking (skill_break_equip, and the bBreakWeaponRate/bBreakArmorRate parts of skill_additional_effect)
    int32 SkillBreakEquip(Unit* src, Unit* bl, uint32 where, int32 rate, int32 flag); // skill_break_equip (rate in 1/10000)

    // pc_setinvincibletimer and pc_delinvincibletimer: after arriving on a map or reviving, no monster or skill targets the player until it
    // runs out, it moves or it acts
    void PCSetInvincibleTimer(Player* player);
    void PCDelInvincibleTimer(Player* player);
    void UpdateInvincibleTimer(Player* player);
    bool PCIsInvincible(Unit const* unit) const;

    // Riding a Peco (pc_isriding, pc_setriding): a Knight's or Crusader's option, kept with the identity.  Its status reads it (speed, ASPD
    // with Cavalier Mastery, the spear's size modifier), as do Spear Mastery and the skills that need it (ST_RIDING, ST_PECO)
    bool PCIsRiding(Unit const* unit) const; // pc_isriding
    void PCSetRiding(Player* player, bool isRiding); // pc_setriding, with pc_setoption's status calculation

    // Devotion (the Crusader's sd->devotion and the devoted unit's SC_DEVOTION): the Crusader keeps a slot per devoted player, the devoted
    // player's status names the Crusader and the slot.  Both are on one map whenever either reads the other; a slot whose player no longer
    // names it is free (rAthena clears it from status_change_end, which here may run where the Crusader's map isn't)
    Player* GetDevotionSource(Unit const* bl, bool isRangeChecked) const; // The devoting Crusader on the unit's map whose slot names it (and in reach)
    Player* GetDevotedPlayer(Player* sd, int32 slot); // The player a slot devotes the Crusader to, on its map, or null (and the slot is let go)
    int32 PCDevotionSlot(Player* sd, ObjectGuid targetGUID, int32 count); // unit_skilluse_id2's slot search: the target's own, else a free one, -1
    void PCDelDevotions(Player* sd); // pc_dead's: every devotion of the Crusader ends
    Unit* BattleCheckDevotion(Unit* bl) const; // battle_check_devotion: the Crusader who takes reflected damage (devotion_rdamage), or null
    void BattleFixDamage(Unit* src, Unit* target, int64 damage, uint16 skillID); // battle_fix_damage: the damage alone, shown on the target

    // Timer skills (skill_addtimerskill, skill_timerskill, skill_cleartimerskill): a skill's later hits, each an event on its caster that runs in
    // the caster's update (Jupitel Thunder's and Water Ball's hits, Double Casting's second bolt)
    int32 SkillAddTimerSkill(Unit* src, uint64 tick, ObjectGuid targetGUID, int32 x, int32 y, uint16 skillID, uint16 skillLevel, int32 type, int32 flag);
    void SkillTimerSkill(Unit* src, uint32 timerSkillID); // skill_timerskill
    void SkillClearTimerSkill(Unit* src); // skill_cleartimerskill

    // The Mage line's supporting systems (ROWOW_SkillSupport.cpp): Abracadabra's abra_db and its skill item, Hindsight's spell menu, the
    // Elemental Converter list, Sense's monster window, Mystical Amplification's toggle, the elemental fields, the cells Ice Wall and Land
    // Protector change, and a skill's look shown alone.  All of it runs in the caster's map thread
    std::vector<ROWOWAbraSkill> AbraSkills; // abra_db (startup)
    void LoadAbraData(); // abra_db, after LoadSkillData
    bool SkillCastAbraSkill(Unit* src, uint16 skillID, uint16 skillLevel); // Abracadabra's pick cast (a player's as its skill item, clif_item_skill)
    bool MobClassChange(Creature* md, uint32 mobID); // mob_class_change
    uint32 MobGetRandomID(std::string const& groupName, bool isDBRate) const; // mob_get_random_id (RMF_DB_RATE), by mob_summon group
    uint32 GetMobCreatureEntry(uint32 mobID) const; // The RO creature template of a monster (mob_db Id), 0 for none
    void SkillToggleMagicPower(Unit* bl, uint16 skillID); // skill_toggle_magicpower
    void ClifAutospell(Player* sd, uint16 skillLevel); // clif_autospell: Hindsight's spell menu (ROWOWAUTOSPELL)
    int32 SkillAutospell(Player* sd, uint16 skillID); // skill_autospell: the spell picked
    void ClifElementalConverterList(Player* sd); // clif_elementalconverter_list (ROWOWCONVERTER)
    void ClifSkillEstimation(Player* sd, Creature* creature); // clif_skill_estimation: Sense's monster window, as system messages
    std::shared_ptr<ROWOWSkillUnitGroup> SkillLocateElementField(Unit* bl); // skill_locate_element_field
    int32 SkillClearGroup(Unit* bl, int32 flag); // skill_clear_group
    std::shared_ptr<ROWOWSkillUnitGroup> MapFindSkillUnitOnCell(Unit* target, int32 x, int32 y, uint16 skillID, int32 flag, int32& indexOut); // map_find_skill_unit_oncell
    bool MapGetCellLandProtector(Map* map, int32 x, int32 y) const; // map_getcell(CELL_CHKLANDPROTECTOR)
    bool MapGetCellIceWall(Map* map, int32 x, int32 y) const; // map_getcell(CELL_CHKICEWALL): an Ice Wall cell can't be walked on
    void ClifSkillPosEffect(Unit* src, uint16 skillID, uint16 skillLevel, int32 x, int32 y, float z); // clif_skill_poseffect: the skill's look on a cell
    void ClifSkillNoDamage(Unit* src, Unit* target, uint16 skillID, uint16 skillLevel); // clif_skill_nodamage: the skill's look on a unit
    bool SkillCheckConditionSCRequired(Player* sd, uint16 skillID, ROWOWSkillCondition const& require); // skill_check_condition_sc_required
    int64 StatusPercentChange(Unit* src, Unit* target, int8 hpRate, int8 spRate, uint8 flag); // status_percent_change (0 heal, 1 damage, 2 damage not killing)
    void StatusSetSP(Unit* bl, uint32 sp); // status_set_sp
    void StatusKill(Unit* bl); // status_kill

    // The Acolyte line's supporting systems (ROWOW_SkillSupport.cpp): a player's Spirit Spheres (pc_addspiritball, pc_delspiritball and their
    // timers, shown by the RO_Identity addon, ROWOWSPHERES), reviving (status_revive), the partners a Priest's B.S. Sacramenti needs
    // (skill_check_pc_partner), and Basilica's cells (pc_cell_basilica, in ROWOW_SkillUnit.cpp).  All of it runs in the player's own map thread
    int32 PCGetSpiritBall(Unit const* unit) const; // sd->spiritball: a player's Spirit Spheres, 0 for a monster
    void PCAddSpiritBall(Player* sd, int64 interval, int32 max); // pc_addspiritball
    void PCDelSpiritBall(Player* sd, int32 count, int32 type); // pc_delspiritball (type 1: the client isn't told)
    void UpdateSpiritBalls(Player* sd); // pc_spiritball_timer as the spheres run out, from the player's update
    void SendSpiritBallsToClient(Player* sd); // clif_spiritball (ROWOWSPHERES)
    bool StatusRevive(Unit* bl, uint8 perHP, uint8 perSP); // status_revive (with pc_revive for a player)
    bool MapGetCellBasilica(Map* map, int32 x, int32 y) const; // map_getcell(CELL_CHKBASILICA)
    void PCCellBasilica(Player* sd); // pc_cell_basilica: SC_BASILICA while the player stands on a Basilica cell
    int32 SkillCheckPCPartner(Player* sd, uint16 skillID, uint16& skillLevel, int32 range, int32 castFlag); // skill_check_pc_partner
    void SkillExtremityFistEnd(Unit* src, Unit* target, uint16 skillLevel); // What Asura Strike leaves its user with, hit or failed

    // The Archer line's supporting systems (ROWOW_SkillSupport.cpp and ROWOW_SkillUnit.cpp): a Hunter's falcon (status.option, kept with the
    // identity), the traps' revealing, springing and the item they give back, and the songs and dances: the unit group that follows its
    // performer (skill_unit_move_unit_group), Dissonance where a song and a dance meet (skill_dance_overlap, skill_dance_switch), and the
    // ensembles' partner (skill_check_pc_partner).  All of it runs in the map thread of the units involved
    bool PCIsBardDancer(Unit const* unit) const; // (class_&MAPID_SECONDMASK) == MAPID_BARDDANCER: a Bard, Dancer, Clown or Gypsy in the form
    bool PCIsFalcon(Unit const* unit) const; // pc_isfalcon
    void PCSetFalcon(Player* player, bool isFalcon); // pc_setfalcon, with pc_setoption's EFST_FALCON message
    void SkillRevealTrapInArea(Unit* src, int32 range, int32 x, int32 y); // skill_reveal_trap_inarea
    int32 SkillTrapSplash(Map* map, Unit* bl, ROWOWSkillUnitGroup& group, int32 index, uint64 tick); // skill_trap_splash
    void ClifChangeTrapLook(Map* map, ROWOWSkillUnitGroup& group, int32 index, Unit* target); // clif_changetraplook (UNT_USED_TRAPS): the trap's spring look on what set it off
    void SkillTrapGiveBack(Unit* src, ROWOWSkillUnitGroup const& group); // The trap item an expired or removed trap leaves (map_addflooritem / pc_additem)
    int32 SkillDanceOverlap(Map* map, ROWOWSkillUnitGroup& group, int32 index, uint8 flag); // skill_dance_overlap
    bool SkillDanceSwitch(ROWOWSkillUnitGroup& group, int32 index, bool revert); // skill_dance_switch
    void SkillUnitMoveUnitGroup(Map* map, std::shared_ptr<ROWOWSkillUnitGroup> group, int32 dx, int32 dy); // skill_unit_move_unit_group
    void ClifTalkieBox(Map* map, ROWOWSkillUnit const& unit, std::string const& message); // clif_talkiebox: the trap's message to those around it

    // The Merchant line's supporting systems (ROWOW_SkillSupport.cpp): the monsters an Alchemist summons (mob_once_spawn_sub with a master:
    // the plants of Summon Flora and Plant Cultivation, the Marine Sphere), whose master is their side in battle_check_target, and Ore
    // Discovery's ore.  All of it runs in the map thread of the units involved
    static uint32 const FloraSummonMobIDs[5]; // Summon Flora's plants by level (Mandragora, Hydra, Flora, Parasite, Geographer)
    Creature* MobOnceSpawnSub(Unit* src, int32 x, int32 y, uint32 mobID, uint8 ai, int64 duration); // mob_once_spawn_sub + mob_spawn, deleted after duration
    Unit const* BattleGetMaster(Unit const* bl) const; // battle_get_master: the player a summoned monster answers to (on its map), or itself
    int32 SkillCheckConditionMobMaster(Player* sd, uint16 skillID, uint32 mobClass, int32& countOut); // skill_check_condition_mob_master_sub's count
    void MobDeadFindingOre(Creature* md); // mob_dead's Ore Discovery: an ore in the loot for the player with the loot priority who knows it
    void UpdateMobSummon(Creature* md); // A summoned monster's own AI: a plant's search for its master's enemies, a hit Marine Sphere's blast
    int32 PCSkillHealBonus(Unit const* unit, uint16 skillID) const; // pc_skillheal_bonus: bHealPower (by skill_add_heal_rate) and bSkillHeal
    int32 PCSkillHeal2Bonus(Unit const* unit, uint16 skillID) const; // pc_skillheal2_bonus: bHealPower2 and bSkillHeal2

    // The Thief line's supporting systems (ROWOW_SkillSupport.cpp): Plagiarism's copy (skill_isCopyable, skill_do_copy, kept in the CLONE_SKILL
    // variables), the strip skills (skill_strip_equip), Intimidate's warp (unit_warp to a random cell), Gangster's Paradise (skill_sit) and
    // Graffiti's message.  All of it runs in the map thread of the units involved
    int8 SkillIsCopyable(Player* sd, uint16 skillID) const; // skill_isCopyable: 1 Plagiarism can copy it, 0 not
    void SkillDoCopy(Unit* src, Unit* bl, uint16 skillID, uint16 skillLevel); // skill_do_copy: the skill that hit a Rogue becomes its copy
    void PCSkillPlagiarismReset(Player* sd); // pc_skill_plagiarism_reset (Plagiarism's): the copy forgotten
    bool SkillStripEquip(Unit* src, Unit* target, uint16 skillID, uint16 skillLevel); // skill_strip_equip
    bool IsUnitWarpForbidden(Unit const* bl) const; // unit_warp's map flags (noteleport, monster_noteleport)
    bool UnitWarpRandom(Unit* bl, Position& destOut); // unit_warp(bl, -1, -1, -1, CLR_TELEPORT): to a random spot of its map (where it goes)
    bool UnitWarpNear(Unit* bl, Position const& center); // unit_warp to a free cell next to a spot of its map (map_search_freecell 1 x 1)
    void SkillSit(Player* sd, bool sitting); // skill_sit: Gangster's Paradise for the Rogues sitting together
    void UpdateSkillSit(Player* sd); // skill_sit as the player sits down or stands up, from its update
    bool MobCanTarget(Creature const* md, Unit const* target) const; // mob_ai_sub_hard's target checks the WoW AI doesn't make (Gangster's Paradise)
    void ClifGraffiti(Map* map, ROWOWSkillUnitGroup& group); // clif_graffiti: Graffiti's message to the players it comes into view of

    // The expanded jobs' supporting systems (ROWOW_SkillSupport.cpp): a Taekwon's running (unit_run), its mission and rests, the Star
    // Gladiator's places and hatreds (character variables, the place confirmed through the RO_Identity addon's ROWOWFEEL menu), the Soul
    // Linker's spirits (the linked jobs' spirit skills, pc_calc_skilltree's), the Super Novice's angel, and party_skill_check.  All of it runs in
    // the map thread of the units involved
    int32 MapCountOnCell(Unit* origin, int32 x, int32 y) const; // map_count_oncell (BL_PC | BL_NPC | BL_MOB): the RO combatants on a cell
    bool MapSearchFreeCell(Unit* center, int32 range, int32& xOut, int32& yOut) const; // map_search_freecell: a random walkable cell around a unit
    bool UnitRun(Unit* bl); // unit_run: a running unit (SC_RUN) moves on the way it runs, or bounces off what stops it (false)
    void UnitRunHit(Unit* bl); // unit_run_hit: the run ends against something, knocked back a cell or more
    void UpdateUnitRun(Unit* bl); // unit_walktoxy_timer's "keep trying to run", from the unit's update once a run's move ended
    void StatusSetHP(Unit* bl, uint32 hp); // status_set_hp
    uint32 PCGetFeelMap(ROWOWIdentity const& identity, int32 pos) const; // feel_map: the RO map of the sun, moon or star (0 none)
    int32 PCGetHateMob(ROWOWIdentity const& identity, int32 pos) const; // hate_mob: the monster (or job) hated, -1 none
    bool PCSetHateMob(Player* sd, int32 pos, Unit* bl); // pc_set_hate_mob
    void ClifFeelReq(Player* sd, uint16 skillLevel); // clif_feel_req: the RO_Identity addon asks to confirm the place (ROWOWFEEL)
    void ClifParseFeelSaveOk(Player* sd); // clif_parse_FeelSaveOk: the place confirmed (.ro feel)
    void ClifFeelInfo(Player* sd, int32 pos, int32 type) const; // clif_feel_info (type 1: registered now, 0: shown)
    void ClifHateInfo(Player* sd, int32 pos, int32 hateClass, int32 type) const; // clif_hate_info
    void ClifMissionInfo(Player* sd, uint32 mobID, int32 progress) const; // clif_mission_info
    void PCResetFeel(Player* sd); // pc_resetfeel
    void PCResetHate(Player* sd); // pc_resethate
    bool SGIsDay(int32 pos) const; // sg_info's day_func: is_day_of_sun, is_day_of_moon, is_day_of_star (or allow_skill_without_day)
    void PCSetDieCounter(Player* sd, int64 value); // pc_setparam(SP_PCDIECOUNTER): a Super Novice that never died keeps its +10 stats
    void MobDeadSGTaekwon(Creature* md, Player* sd, int32& bonus); // mob_dead's Star Gladiator's blessing (more exp) and the Taekwon mission's kill count
    void PartySkillCheck(Player* sd, uint16 skillID, uint16 skillLevel); // party_skill_check: Star Gladiator's Friend on a party member's combo
    bool StatusDamageRevive(Unit* bl); // status_damage's Kaizel and pc_dead's Super Novice angel: up again in place of dying
    void PCSuperNoviceCall(Player* sd, std::string const& message); // The Guardian Angel prayer's lines (clif_parse_NoviceExplosionSpirits)
    void PCLevelUpAngel(Player* sd); // pc_checkbaselevelup's Super Novice and Taekwon angels: their buffs at each base level
    void SkillTaekwonRest(Player* sd, bool sitting); // skill_sit's flag 2: Taekwons sitting together rest (state.rest)

    // Merchant prices (pc_modifybuyvalue, pc_modifysellvalue)
    int32 PCModifyBuyValue(Player const* player, int32 origValue) const;
    int32 PCModifySellValue(Player const* player, int32 origValue) const;

    // The skill bonuses of worn items (pc_skillatk_bonus, pc_skillheal_bonus...), kept as deferred bonuses by name (bSkillAtk,...)
    int32 GetDeferredBonus(Unit const* unit, char const* type) const; // The sum of a one-value bonus (bDelayRate), 0 for a monster
    int32 GetDeferredBonus2(Unit const* unit, char const* type, int64 key) const; // The sum of a bonus2's value for one key (a skill, a race...)
    bool HasDeferredBonusKey(Unit const* unit, char const* type, int64 key) const; // Whether a bonus names the key (bIgnoreMdefRace,RC_Demon)

    // RO status changes (ROWOW_StatusChange.cpp): the port of rAthena's status changes (status.cpp): status_change_start with its
    // resistances, status_change_end, status_change_timer, the states that stop a unit (through WoW's root, silence and pacify), and the
    // client's icons (a marker aura per status).  A unit's statuses live on the unit and are only touched in its map's thread
    void LoadStatusChangeData();
    ROWOWStatusChangeDB const* GetStatusChangeDB(int32 type) const; // status_db.find
    uint16 StatusDBGetSkill(int32 type) const; // status_db.getSkill: the skill whose durations a status takes
    ROWOWStatusChange* GetStatusChange(Unit* unit) const; // status_get_sc, null when the unit has none
    ROWOWStatusChange const* GetStatusChange(Unit const* unit) const;
    ROWOWStatusChangeEntry const* GetSCE(Unit const* unit, int32 type) const; // The unit's status of that type, or null
    bool StatusChangeStart(Unit* src, Unit* bl, int32 type, int32 rate, int32 val1, int32 val2, int32 val3, int32 val4, int64 duration, uint8 flag,
        int32 delay = 0); // status_change_start
    bool SCStart(Unit* src, Unit* bl, int32 type, int32 rate, int32 val1, int64 duration, int32 delay = 0) { return StatusChangeStart(src, bl, type, 100 * rate, val1, 0, 0, 0, duration, ROWOW_SCSTART_NONE, delay); } // sc_start
    bool SCStart2(Unit* src, Unit* bl, int32 type, int32 rate, int32 val1, int32 val2, int64 duration, int32 delay = 0) { return StatusChangeStart(src, bl, type, 100 * rate, val1, val2, 0, 0, duration, ROWOW_SCSTART_NONE, delay); }
    bool SCStart4(Unit* src, Unit* bl, int32 type, int32 rate, int32 val1, int32 val2, int32 val3, int32 val4, int64 duration, int32 delay = 0) { return StatusChangeStart(src, bl, type, 100 * rate, val1, val2, val3, val4, duration, ROWOW_SCSTART_NONE, delay); }
    bool StatusChangeEnd(Unit* bl, int32 type, bool isTimer = false); // status_change_end (tid != INVALID_TIMER when isTimer)
    int32 StatusChangeClear(Unit* bl, int32 type); // status_change_clear: 0 on death, 1 leaving (logout)
    int64 StatusGetSCDef(Unit const* src, Unit const* bl, int32 type, int32 rate, int64 tick, uint8 flag) const; // status_get_sc_def
    int32 StatusIsImmune(Unit const* bl) const; // status_isimmune (Golden Thief Bug's magic immunity)
    bool StatusIsEndure(Unit const* bl) const; // status_isendure
    void UpdateStatusChanges(Unit* unit); // The unit's own status_change_timer runs, from its update
    void StatusDamaged(Unit* attacker, Unit* target, uint32 damage); // status_damage's statuses: what being hit ends (sleep, freeze, stone)
    bool StatusCheckVisibility(Unit const* src, Unit const* target) const; // status_check_visibility (hiding, cloaking)
    void SendStatusChangesToClient(Unit* unit); // The marker auras for every status (after a form change or a respawn)
    void StatusChangeTimerSub(Unit* bl, Unit* src, ROWOWStatusChangeEntry const* sce, int32 type, uint64 tick); // status_change_timer_sub
    void StatusChangeClearBuffs(Unit* bl, uint8 type); // status_change_clear_buffs

    // The status calculation of statuses (ROWOW_StatusCalc.cpp): status_calc_bl_ and status_calc_bl_main with the status_calc_* of each
    // value, the hp and sp bonuses of statuses, regen rates and the WoW speed that follows the RO walk speed
    void StatusCalcBL(Unit* unit, std::bitset<ROWOW_SCB_MAX> flag); // status_calc_bl_: works the unit's status out again and updates it
    void StatusCalcBLMain(Unit const* unit, ROWOWStatus const& baseStatus, ROWOWStatusChange const* sc, ROWOWStatus& status) const; // status_calc_bl_main (SCB_ALL)
    int32 StatusGetHPBonus(ROWOWStatusChange const* sc, int32 type) const; // status_get_hpbonus
    int32 StatusGetSPBonus(ROWOWStatusChange const* sc, int32 type) const; // status_get_spbonus
    void StatusCalcRegenRate(Player const* sd, ROWOWStatusChange const* sc, ROWOWStatus& status) const; // status_calc_regen_rate
    void ApplyROSpeed(Unit* unit, uint8 moveType, float& speed) const; // The unit's RO walk speed on its WoW speed (OnUnitUpdateSpeed)
    static int32 StatusCalcAttackElement(ROWOWStatusChange const* sc, int32 element); // status_calc_attack_element (status_get_attack_sc_element with 0)
    ROWOWStatus const* GetROBaseStatus(Unit const* unit) const; // status_get_base_status: a player's base status, a monster's mob_db one
    int64 SkillGetTime(uint16 skillID, uint16 skillLevel) const; // skill_get_time: a skill level's Duration1
    int64 SkillGetTime2(uint16 skillID, uint16 skillLevel) const; // skill_get_time2: its Duration2
    int32 SkillGetSplash(uint16 skillID, uint16 skillLevel) const; // skill_get_splash: its SplashArea, a negative one AREA_SIZE
    bool SkillCheckCloaking(Unit* bl, ROWOWStatusChangeEntry* sce = nullptr); // skill_check_cloaking: whether a wall is next to the unit
    bool IsCellNoPass(Unit const* unit, ROWOWMapGrid const& mapGrid, int32 cellX, int32 cellY) const; // map_getcell(CELL_CHKNOPASS), near the unit
    bool MapRandomDir(Unit const* bl, int32& x, int32& y) const; // map_random_dir (Confusion's walk), in RO cells
    void StatusZap(Unit* bl, uint32 hp, uint32 sp); // status_zap
    bool StatusCharge(Unit* bl, int64 hp, int64 sp); // status_charge

    // The statuses of the identity while out of the form and in the character database (sc_data): kept on leaving the RO world (RO's
    // logout), and started again on entering it (as on login)
    void StoreIdentityStatusChanges(Player* player, ROWOWIdentity& identity);
    void SaveIdentityStatusChanges(Player* player, ROWOWIdentity& identity, CharacterDatabaseTransaction trans);
    void LeaveStatusChanges(Player* player, ROWOWIdentity& identity);
    void RestoreStatusChanges(Player* player, ROWOWIdentity& identity);
    void StatusChangeClearOnChangeMap(Unit* bl); // status_change_clear_onChangeMap

    // The creature template's values, or null if it has none (a melee creature with no attack visual)
    ROWOWCreatureTemplate const* GetCreatureTemplate(uint32 creatureEntry) const;

    // Finds the ground under a point, searching down from a little above the reference height.  Returns false when there is no ground
    bool GetGroundZ(Creature* creature, float x, float y, float referenceZ, float& groundZOut);
    bool GetGroundZ(Creature* creature, float x, float y, float referenceZ, float searchHeight, float& groundZOut);

    // Builds a path from the creature toward a target, one small step at a time with every step pulled onto the ground, so the
    // creature follows the terrain like an RO monster does.  The path stops early at a wall, a cliff, or missing ground, and
    // stops short of the target by stopShortDistance.  Returns false if not even one step could be made
    bool BuildGroundContouredPath(Creature* creature, float targetX, float targetY, float stopShortDistance, Movement::PointsArray& pathNodesOut);

    // RO NPC scripts (ROWOW_NpcScript.cpp, R7): rAthena's NPC script files read at startup into NPCs and functions, the NPC runs (one per
    // player, waiting for its answers), NPC timers and events, and the variables the scripts keep.  Runs with a player happen in that
    // player's thread (its session's commands, its map's update); runs with none (OnInit, timers, events) in the world update, which
    // also applies what scripts changed of other NPCs (shown, hidden) to their creatures
    void LoadNpcScriptData(); // After every other script command is registered
    void RegisterNpcScriptCommands(); // ROWOW_ScriptCommands.cpp
    void UpdateNpcScripts(uint32 diff); // The world update
    ROWOWNpc const* GetNpc(uint32 npcID) const;
    ROWOWNpc const* GetNpcByName(std::string const& name, uint32 preferredMapID = 0) const; // By exname, or else by the shown name
    ROWOWNpc const* GetCreatureNpc(Creature const* creature) const; // The NPC a creature stands for, or null
    // Talking to the NPC (npc_click: its start, or a label), the dialog going to the RO NPC window (or the dialog given)
    bool StartNpcRun(Player* player, ROWOWNpc const* npc, std::string const& label = std::string(), ROWOWScriptDialog* dialog = nullptr);
    void NpcDialogNext(Player* player); // The answers: Next, a menu choice (1 up, 255 cancels), an input, the Close button
    void NpcDialogSelect(Player* player, int32 choice);
    void NpcDialogInput(Player* player, std::string const& value);
    void NpcDialogClose(Player* player);
    void EndNpcRun(Player* player); // Drops the player's run (cancel, logout)
    void SetupNpcRunContext(ROWOWScriptContext& context, Player* player, ROWOWNpc const* npc);
    bool IsItemUseBlockedByNpc(Player* player); // Its run turned items off (disable_items), so it can't use, put on or take off items
    void QueueNpcEvent(ROWOWNpc const* npc, std::string const& label); // Runs it with no player at the next world update (donpcevent)
    bool QueueNpcEvent(std::string const& eventName); // "<NPC>::<label>", false when there's no such NPC or label
    bool DoNpcEvent(std::string const& eventName); // donpcevent: now in the world update, else queued (QueueNpcEvent)
    void RunNpcEvent(ROWOWNpc const* npc, uint32 instructionIndex, Player* player = nullptr); // Now, in the world update (or the player's thread)
    void RunPlayerNpcEvent(Player* player, ROWOWNpc const* npc, uint32 instructionIndex, bool isTouch = false); // npc_event_sub: now, or after its run
    void RunPlayerEvent(Player* player, uint8 eventType); // npc_script_event: an ROWOW_NPC_EVENT_* label of every NPC that has it, for the player
    void UpdatePlayerNpcScripts(Player* player, uint32 diff); // Its sleeping run, queued events, OnTouch areas and the dialog's range (its update)
    void CheckNpcTouchAreas(Player* player); // npc_touchnext_areanpc and npc_touch_area_allnpc at the cell it's on
    void TouchNpcArea(Player* player, ROWOWNpc const* npc); // npc_touch_areanpc's script part: OnTouch_, else OnTouch, else a click
    void ForgetNpcTouches(Player* player); // Leaving the world: the OnTouch_ NPCs it held are free
    bool IsHidingFromNpcs(Player* player) const; // pc_ishiding: hidden, cloaked or chase walking
    void SetNpcTimerPlayer(ROWOWNpc const* npc, ObjectGuid playerGUID); // attachnpctimer (detachnpctimer: an empty GUID)

    void SetNpcEnabled(ROWOWNpc const* npc, bool isEnabled); // enablenpc and disablenpc
    void SetNpcHidden(ROWOWNpc const* npc, bool isHidden); // hideoffnpc and hideonnpc
    void NpcTimerStart(ROWOWNpc const* npc, bool isFromZero); // initnpctimer (from 0) and startnpctimer
    void NpcTimerStop(ROWOWNpc const* npc); // stopnpctimer
    void NpcTimerSet(ROWOWNpc const* npc, int64 tick); // setnpctimer
    int64 NpcTimerGet(ROWOWNpc const* npc, int32 type); // getnpctimer: 0 the tick, 1 whether it runs, 2 its labels
    void LoadIdentityScriptVariables(Player* player, ROWOWIdentity& identity); // The character and account variables
    void SaveIdentityScriptVariables(Player* player, ROWOWIdentity& identity, CharacterDatabaseTransaction trans);
    std::vector<std::unique_ptr<ROWOWNpc>> NpcsByID; // By ID - 1.  Loaded once at startup, the state under NpcStateMutex
    std::unordered_map<std::string, ROWOWNpc*> NpcsByUniqueName; // Lowercase exname
    std::unordered_multimap<std::string, ROWOWNpc*> NpcsByName; // Lowercase shown name
    std::unordered_map<ObjectGuid::LowType, ROWOWNpc*> NpcsByCreatureSpawnID;
    std::unordered_map<std::string, int64> BattleFlagsByName; // getbattleflag: the battle config's values by name (lowercase)
    std::mutex NpcStateMutex; // The NPCs' shown state and timers, and the queues below, which map threads change
    std::vector<ROWOWNpcEvent> PendingNpcEvents;
    std::set<uint32> NpcVisibilityChangedIDs; // NPCs whose creatures the next world update shows or hides
    std::set<uint32> RunningNpcTimerIDs;
    std::vector<std::unique_ptr<ROWOWScriptContext>> SleepingNpcRuns; // Runs with no player waiting on sleep (world update only)
    std::vector<ROWOWNpcAnnouncement> PendingNpcAnnouncements; // Under NpcStateMutex
    std::unordered_map<ObjectGuid, ROWOWScriptMob> ScriptMobsByGUID; // Living script monsters, under NpcStateMutex
    std::vector<ROWOWScriptMobKill> PendingScriptMobKills; // Under NpcStateMutex
    Creature* SpawnScriptMob(Map* map, uint32 mobID, int32 x, int32 y, std::string const& name, std::string const& event); // monster: on the map's thread (or with maps idle)
    bool OnScriptMobDeath(Creature* creature, Unit* killer); // The event its death runs, with the killer attached (false: it had none)
    std::vector<ROWOWPendingScriptMob> PendingScriptMobs; // Under NpcStateMutex: monsters for maps not loaded yet
    std::vector<ROWOWNpcMapWarp> PendingNpcMapWarps; // Under NpcStateMutex: mapwarp and areawarp, for the world update
    std::unordered_map<uint32, uint32> MapUserCounts; // Under NpcStateMutex: the players on each RO map as of the last world update (getmapusers)
    void WarpPlayerToCell(Player* player, std::string const& mapName, int32 x, int32 y); // The warp command's destination: a cell, SavePoint or Random
    std::unordered_map<uint32, std::vector<uint32>> TouchNpcIDsByMapID; // The NPCs with an OnTouch area, by WoW map (startup data)
    std::vector<ROWOWNpcEvent> PlayerEventLabels[ROWOW_NPC_EVENT_COUNT]; // script_event: each player event's labels (startup data)
    std::unordered_map<std::string, std::vector<ROWOWNpcEvent>> ClockEventsByLabel; // OnClock/OnHour/OnMinute/On<day>/OnDay labels, lowercase
    int64 LastClockMinute = 0; // The minute (since the epoch) npc_event_do_clock last ran for
    // The quest log (ROWOW_Quest.cpp, R8): rAthena's quest.cpp.  quest_db is startup data; a character's quest log is its identity's, only
    // touched in its own thread (its runs, its session's commands, kills on its map)
    void LoadQuestData();
    ROWOWQuestData const* QuestSearch(uint32 questID) const; // quest_search
    int32 QuestAdd(Player* sd, uint32 questID); // quest_add: 0, or -1 when it can't
    int32 QuestChange(Player* sd, uint32 qid1, uint32 qid2); // quest_change
    int32 QuestDelete(Player* sd, uint32 questID); // quest_delete
    int32 QuestUpdateStatus(Player* sd, uint32 questID, uint8 status); // quest_update_status
    int32 QuestCheck(ROWOWIdentity const& identity, uint32 questID, uint8 type) const; // quest_check: -1 when it doesn't have it
    void QuestUpdateObjective(Player* sd, Creature* md); // quest_update_objective: a kill counted for its hunting quests
    void QuestKillUpdate(Creature* md, Player* sd); // mob_dead's part: the killer's (or each party member's in range) hunting
    void SendQuestsToClient(Player* player); // The quest window's list (ROWOWQUEST)
    void LoadIdentityQuests(Player* player, ROWOWIdentity& identity);
    void SaveIdentityQuests(Player* player, ROWOWIdentity& identity, CharacterDatabaseTransaction trans);
    std::unordered_map<uint32, ROWOWQuestData> QuestsByID;
    // Kafra storage (ROWOW_Storage.cpp, R8.2): rAthena's storage.cpp opens the character's own WoW bank, and its guild's bank when the guild has
    // one (plan section 8 item 7), each with the player as its own banker as AzerothCore's bank command does
    void StorageStorageOpen(Player* player); // storage_storageopen: the WoW bank window
    int32 StorageGuildStorageOpen(Player* player); // storage_guild_storageopen: ROWOW_GSTORAGE_*; the guild bank window opens at the next world update
    void StorageClose(Player* player); // Leaving the map: the storage ends (storage_storageclose, storage_guild_storageclose)
    void OpenPendingGuildStorages(); // The world update, with the maps idle: the guild bank windows asked for
    std::set<ObjectGuid> PendingGuildStorageOpens; // Under NpcStateMutex
    // Waiting rooms (ROWOW_WaitingRoom.cpp, R8.3): rAthena's chat.cpp for the rooms NPCs own, the job tests' queues.  The rooms are under
    // NpcStateMutex; a player's join and leave run in its thread, the rooms' events in the world update
    void RegisterWaitingRoomCommands(); // From RegisterNpcScriptCommands
    void ChatCreateNpcChat(ROWOWNpc const* npc, std::string const& title, int32 limit, int32 trigger, std::string const& event, int64 zeny, int32 minLevel,
        int32 maxLevel); // chat_createnpcchat
    void ChatDeleteNpcChat(ROWOWNpc const* npc); // chat_deletenpcchat
    int32 ChatJoinChat(Player* player, uint32 npcID); // chat_joinchat: rAthena's e_refuse_enter_room (3 ENTERROOM_SUCCESS), saying why not
    int32 ChatJoinChatSub(Player* player, uint32 npcID);
    bool ChatLeaveChat(Player* player, bool isKicked); // chat_leavechat: false when it was in none
    void ChatTriggerEvent(ROWOWNpc const* npc); // chat_triggerevent
    void ChatSetEventEnabled(ROWOWNpc const* npc, bool isEnabled); // chat_enableevent and chat_disableevent
    void ChatNpcKickAll(ROWOWNpc const* npc); // chat_npckickall
    void WarpWaitingPlayer(ObjectGuid const& playerGUID, int64 zeny, std::string const& mapName, int32 x, int32 y); // warpwaitingpc's, maps idle
    void WarpPendingWaitingPlayers(); // The world update: warpwaitingpc's warps from map threads
    void UpdatePlayerWaitingRooms(Player* player, uint32 diff); // Its update: the rooms in sight to its client, and walking out of its room
    std::map<uint32, ROWOWWaitingRoom> WaitingRoomsByNpcID; // Under NpcStateMutex
    std::vector<ROWOWWaitingRoomWarp> PendingWaitingRoomWarps; // Under NpcStateMutex
    static thread_local bool IsInNpcWorldUpdate; // The world update's NPC part is running on this thread (the maps are idle)

    ROWOWChatScriptDialog ChatScriptDialog; // For GM testing (.ro npc talk -chat)
    ROWOWWindowScriptDialog WindowScriptDialog; // The RO NPC window: every player run's dialog
    uint32 GlobalVariableSaveTimer = 0;

private:
    // The rest of the status change port (ROWOW_StatusChange.cpp)
    bool StatusChangeStartPostDelay(Unit* src, Unit* bl, int32 type, int32 val1, int32 val2, int32 val3, int32 val4, int32 tick, uint8 flag); // status_change_start_post_delay
    bool StatusChangeStartValues(Unit* src, Unit* bl, int32 type, int32& val1, int32& val2, int32& val3, int32& val4, int32& tick, int32& tickTime,
        uint8 flag); // status_change_start_post_delay's switch of each status's values
    void StatusChangeStartDevoted(Unit* src, Player* sd, int32 type, int32 val1, int32 val2, int32 val3, int32 val4, int64 tick, uint8 flag); // A Crusader's status passed on to its devoted
    void StatusChangeTimer(Unit* bl, int32 type, uint64 tick); // status_change_timer
    void StatusCalcState(Unit* bl, ROWOWStatusChange& sc, ROWOWStatusChangeDB const& scdb, bool start); // status_calc_state
    void UpdateStatusChangeOptions(Unit* bl, ROWOWStatusChange& sc); // The states and options of the statuses on the unit, through WoW
    void ShowStatusChangeIcon(Unit* bl, ROWOWStatusChangeDB const& scdb, ROWOWStatusChangeEntry const& sce); // clif_status_change (on)
    void HideStatusChangeIcon(Unit* bl, ROWOWStatusChangeDB const& scdb); // clif_status_change (off)
    void StatusChangeSaveNeeded(Player* player);

    // The rest of the RO battle port (ROWOW_Battle.cpp), with rAthena's function names
    ROWOWDamage InitializeWeaponData(Unit const* src, Unit const* target, uint16 skillID, uint16 skillLevel, int32 wflag) const; // initialize_weapon_data
    bool IsAttackCritical(ROWOWDamage& wd, Unit* src, Unit const* target, uint16 skillID, uint16 skillLevel, bool firstCall); // is_attack_critical (ends Auto Counter)
    bool IsAttackHitting(ROWOWDamage& wd, Unit* src, Unit* target, uint16 skillID, uint16 skillLevel, bool firstCall); // is_attack_hitting
    bool AttackIgnoresDef(ROWOWDamage& wd, Unit* src, Unit const* target, uint16 skillID, uint16 skillLevel, bool isRightHand); // attack_ignores_def
    int32 IsAttackPiercing(ROWOWDamage& wd, Unit* src, Unit const* target, uint16 skillID, uint16 skillLevel, bool isRightHand); // is_attack_piercing
    bool IsInfiniteDefense(Unit const* target, int32 flag) const; // is_infinite_defense
    bool IsAttackRightHanded(Unit const* src, uint16 skillID) const; // is_attack_right_handed
    bool IsAttackLeftHanded(Unit const* src, uint16 skillID) const; // is_attack_left_handed
    int32 BattleGetWeaponElement(ROWOWDamage const& wd, Unit const* src, Unit const* target, uint16 skillID, uint16 skillLevel, bool isRightHand,
        bool isCalcForDamageOnly) const; // battle_get_weapon_element
    void BattleCalcMultiAttack(ROWOWDamage& wd, Unit* src, Unit* target, uint16 skillID, uint16 skillLevel) const; // battle_calc_multi_attack
    void BattleCalcSkillBaseDamage(ROWOWDamage& wd, Unit* src, Unit* target, uint16 skillID, uint16 skillLevel); // battle_calc_skill_base_damage
    int64 BattleCalcBaseDamage(Unit const* src, ROWOWStatus const& status, bool isLeftHand, uint8 targetSize, int32 flag) const; // battle_calc_base_damage
    void BattleCalcDefenseReduction(ROWOWDamage& wd, Unit* src, Unit* target, uint16 skillID, uint16 skillLevel); // battle_calc_defense_reduction
    void BattleCalcAttackPostDefense(ROWOWDamage& wd, Unit* src, Unit* target, uint16 skillID, uint16 skillLevel) const; // battle_calc_attack_post_defense
    void BattleCalcAttackMasteries(ROWOWDamage& wd, Unit* src, Unit* target, uint16 skillID, uint16 skillLevel) const; // battle_calc_attack_masteries
    int64 BattleAddMastery(Unit const* src, Unit const* target, int64 damage, bool isLeftHand) const; // battle_addmastery
    void BattleCalcElementDamage(ROWOWDamage& wd, Unit* src, Unit* target, uint16 skillID, uint16 skillLevel); // battle_calc_element_damage
    void BattleCalcAttackPlant(ROWOWDamage& wd, Unit* src, Unit* target, uint16 skillID, uint16 skillLevel); // battle_calc_attack_plant
    void BattleApplyDivFix(ROWOWDamage& wd, uint16 skillID) const; // battle_apply_div_fix
    void BattleCalcAttackLeftRightHands(ROWOWDamage& wd, Unit* src, Unit* target, uint16 skillID, uint16 skillLevel) const; // battle_calc_attack_left_right_hands
    int64 BattleCalcDamage(Unit* src, Unit* target, ROWOWDamage& d, int64 damage, uint16 skillID, uint16 skillLevel); // battle_calc_damage
    bool BattleStatusBlockDamage(Unit* src, Unit* target, ROWOWStatusChange* sc, ROWOWDamage& d, int64& damage, uint16 skillID, uint16 skillLevel); // battle_status_block_damage
    void BattleCalcWeaponFinalAtkModifiers(ROWOWDamage& wd, Unit* src, Unit* target, uint16 skillID, uint16 skillLevel); // battle_calc_weapon_final_atk_modifiers
    int64 BattleCalcReturnDamage(Unit* tbl, Unit* src, int64 damage, int32 flag, uint16 skillID, bool isStatusReflect) const; // battle_calc_return_damage
    void BattleDoReflect(int32 attackType, ROWOWDamage& wd, Unit* src, Unit* target, uint16 skillID, uint16 skillLevel); // battle_do_reflect
    int32 BattleGetAtkPercent(Unit const* bl, uint16 skillID, ROWOWStatusChange const& sc) const; // battle_get_atkpercent
    void BattleAttackSCBonus(ROWOWDamage& wd, Unit* src, Unit* target, uint16 skillID, uint16 skillLevel) const; // battle_attack_sc_bonus
    void BattleMinDamage(ROWOWDamage& wd, Unit const* src, uint16 skillID, int64 minimum) const; // battle_min_damage
    int32 BattleRangeType(Unit const* src, Unit const* target, uint16 skillID, uint16 skillLevel) const; // battle_range_type
    int32 BattleBlewCountBonus(Unit const* src, uint16 skillID) const; // battle_blewcount_bonus
    std::bitset<ROWOW_NK_MAX> BattleSkillGetDamageProperties(uint16 skillID, int32 isSplash) const; // battle_skill_get_damage_properties
    int32 BattleCalcAttackSkillRatio(ROWOWDamage& wd, Unit* src, Unit* target, uint16 skillID, uint16 skillLevel); // battle_calc_attack_skill_ratio (uses Poison React up)
    int64 BattleCalcSkillConstantAddition(ROWOWDamage& wd, Unit* src, Unit* target, uint16 skillID, uint16 skillLevel) const; // battle_calc_skill_constant_addition
    static bool BattleSkillStacksMasteriesVVS(uint16 skillID, bool isRefineOrStar); // battle_skill_stacks_masteries_vvs (BCHK_REFINE/BCHK_STAR, or BCHK_ALL)
    int32 BattleGetMagicElement(ROWOWDamage const& d, Unit const* src, Unit const* target, uint16 skillID, uint16 skillLevel) const; // battle_get_magic_element
    int32 BattleGetMiscElement(ROWOWDamage const& d, Unit const* src, Unit const* target, uint16 skillID, uint16 skillLevel) const; // battle_get_misc_element

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
