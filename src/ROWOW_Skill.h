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

#ifndef ROWOW_SKILL_H
#define ROWOW_SKILL_H

#include "Define.h"
#include "ObjectGuid.h"
#include "Position.h"

#include "DataMap.h"
#include "ROWOW_SkillIDs.h"
#include "ROWOW_SkillUnitIDs.h"
#include "ROWOW_StatusIDs.h"
#include <bitset>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// The RO skill engine's shared definitions: rAthena's skill constants (skill.hpp, battle.hpp, map.hpp), a skill's data (skill_db), the
// per-skill classes (rAthena's src/map/skills SkillImpl, which the module mirrors as ROWOWSkillImpl, one class per skill, grouped by job)
// and a unit's status changes (status_change).  Names follow rAthena's so the port can be checked against it

class Unit;
struct ROWOWScript;

// e_skill_nk: a skill's DamageFlags
enum ROWOWSkillNK : uint8
{
    ROWOW_NK_NODAMAGE = 0,
    ROWOW_NK_SPLASH,
    ROWOW_NK_SPLASHSPLIT,
    ROWOW_NK_IGNOREATKCARD,
    ROWOW_NK_IGNOREELEMENT,
    ROWOW_NK_IGNOREDEFENSE,
    ROWOW_NK_IGNOREFLEE,
    ROWOW_NK_IGNOREDEFCARD,
    ROWOW_NK_CRITICAL,
    ROWOW_NK_IGNORELONGCARD,
    ROWOW_NK_SIMPLEDEFENSE,
    ROWOW_NK_MAX,
};

// e_skill_inf: a skill's TargetType (bits)
#define ROWOW_INF_PASSIVE_SKILL                     0x00
#define ROWOW_INF_ATTACK_SKILL                      0x01
#define ROWOW_INF_GROUND_SKILL                      0x02
#define ROWOW_INF_SELF_SKILL                        0x04
#define ROWOW_INF_SUPPORT_SKILL                     0x10
#define ROWOW_INF_TRAP_SKILL                        0x20

// e_skill_inf2: a skill's Flags
enum ROWOWSkillInf2 : uint8
{
    ROWOW_INF2_ISQUEST = 0,
    ROWOW_INF2_ISNPC,
    ROWOW_INF2_ISWEDDING,
    ROWOW_INF2_ISSPIRIT,
    ROWOW_INF2_ISGUILD,
    ROWOW_INF2_ISSONG,
    ROWOW_INF2_ISENSEMBLE,
    ROWOW_INF2_ISTRAP,
    ROWOW_INF2_TARGETSELF,
    ROWOW_INF2_NOTARGETSELF,
    ROWOW_INF2_PARTYONLY,
    ROWOW_INF2_GUILDONLY,
    ROWOW_INF2_NOTARGETENEMY,
    ROWOW_INF2_ISAUTOSHADOWSPELL,
    ROWOW_INF2_ISCHORUS,
    ROWOW_INF2_IGNOREBGREDUCTION,
    ROWOW_INF2_IGNOREGVGREDUCTION,
    ROWOW_INF2_DISABLENEARNPC,
    ROWOW_INF2_TARGETTRAP,
    ROWOW_INF2_IGNORELANDPROTECTOR,
    ROWOW_INF2_ALLOWWHENHIDDEN,
    ROWOW_INF2_ALLOWWHENPERFORMING,
    ROWOW_INF2_TARGETEMPERIUM,
    ROWOW_INF2_IGNOREKAGEHUMI,
    ROWOW_INF2_ALTERRANGEVULTURE,
    ROWOW_INF2_ALTERRANGESNAKEEYE,
    ROWOW_INF2_ALTERRANGESHADOWJUMP,
    ROWOW_INF2_ALTERRANGERADIUS,
    ROWOW_INF2_ALTERRANGERESEARCHTRAP,
    ROWOW_INF2_IGNOREHOVERING,
    ROWOW_INF2_ALLOWONWARG,
    ROWOW_INF2_ALLOWONMADO,
    ROWOW_INF2_TARGETMANHOLE,
    ROWOW_INF2_TARGETHIDDEN,
    ROWOW_INF2_INCREASEDANCEWITHWUGDAMAGE,
    ROWOW_INF2_IGNOREWUGBITE,
    ROWOW_INF2_IGNOREAUTOGUARD,
    ROWOW_INF2_IGNORECICADA,
    ROWOW_INF2_SHOWSCALE,
    ROWOW_INF2_IGNOREGTB,
    ROWOW_INF2_TOGGLEABLE,
    ROWOW_INF2_IGNORENONCRITATKBONUS,
    ROWOW_INF2_MAX,
};

// e_skill_cast_flags: a skill's CastTimeFlags and CastDelayFlags
#define ROWOW_SKILL_CAST_IGNOREDEX                  0x1
#define ROWOW_SKILL_CAST_IGNORESTATUS               0x2
#define ROWOW_SKILL_CAST_IGNOREITEMBONUS            0x4

// e_skill_unit_flag: a ground skill's Unit_Flag
enum ROWOWSkillUnitFlag : uint8
{
    ROWOW_UF_NONE = 0,
    ROWOW_UF_NOENEMY,
    ROWOW_UF_NOREITERATION,
    ROWOW_UF_NOFOOTSET,
    ROWOW_UF_NOOVERLAP,
    ROWOW_UF_PATHCHECK,
    ROWOW_UF_NOPC,
    ROWOW_UF_NOMOB,
    ROWOW_UF_SKILL,
    ROWOW_UF_DANCE,
    ROWOW_UF_ENSEMBLE,
    ROWOW_UF_SONG,
    ROWOW_UF_DUALMODE,
    ROWOW_UF_NOKNOCKBACK,
    ROWOW_UF_RANGEDSINGLEUNIT,
    ROWOW_UF_CRAZYWEEDIMMUNE,
    ROWOW_UF_REMOVEDBYFIRERAIN,
    ROWOW_UF_KNOCKBACKGROUP,
    ROWOW_UF_HIDDENTRAP,
    ROWOW_UF_MAX,
};

// e_skill_display: flags for skill_attack and skill_area_sub
#define ROWOW_SD_LEVEL                              0x1000
#define ROWOW_SD_ANIMATION                          0x2000
#define ROWOW_SD_SPLASH                             0x4000
#define ROWOW_SD_PREAMBLE                           0x8000
#define ROWOW_NPC_EARTHQUAKE_FLAG                   0x800   // NPC_EARTHQUAKE_FLAG: Earthquake's first hit of its unit, which Dodge and Kaupe let through

// e_require_state: a skill's Requires_State
enum ROWOWRequireState : uint8
{
    ROWOW_ST_NONE,
    ROWOW_ST_HIDDEN,
    ROWOW_ST_RIDING,
    ROWOW_ST_FALCON,
    ROWOW_ST_CART,
    ROWOW_ST_SHIELD,
    ROWOW_ST_RECOVER_WEIGHT_RATE,
    ROWOW_ST_MOVE_ENABLE,
    ROWOW_ST_WATER,
    ROWOW_ST_RIDINGDRAGON,
    ROWOW_ST_WUG,
    ROWOW_ST_RIDINGWUG,
    ROWOW_ST_MADO,
    ROWOW_ST_ELEMENTALSPIRIT,
    ROWOW_ST_ELEMENTALSPIRIT2,
    ROWOW_ST_PECO,
    ROWOW_ST_SUNSTANCE,
    ROWOW_ST_MOONSTANCE,
    ROWOW_ST_STARSTANCE,
    ROWOW_ST_UNIVERSESTANCE,
};

// e_battle_check_target
#define ROWOW_BCT_NOONE                             0x000000
#define ROWOW_BCT_SELF                              0x010000
#define ROWOW_BCT_ENEMY                             0x020000
#define ROWOW_BCT_PARTY                             0x040000
#define ROWOW_BCT_GUILDALLY                         0x080000
#define ROWOW_BCT_NEUTRAL                           0x100000
#define ROWOW_BCT_SAMEGUILD                         0x200000
#define ROWOW_BCT_ALL                               0x3F0000
#define ROWOW_BCT_WOS                               0x400000
#define ROWOW_BCT_SLAVE                             (ROWOW_BCT_SELF | ROWOW_BCT_WOS)
#define ROWOW_BCT_GUILD                             (ROWOW_BCT_SAMEGUILD | ROWOW_BCT_GUILDALLY)
#define ROWOW_BCT_NOGUILD                           (ROWOW_BCT_ALL & ~ROWOW_BCT_GUILD)
#define ROWOW_BCT_NOPARTY                           (ROWOW_BCT_ALL & ~ROWOW_BCT_PARTY)
#define ROWOW_BCT_NOENEMY                           (ROWOW_BCT_ALL & ~ROWOW_BCT_ENEMY)
#define ROWOW_BCT_ALLY                              (ROWOW_BCT_PARTY | ROWOW_BCT_GUILD)
#define ROWOW_BCT_FRIEND                            ROWOW_BCT_NOENEMY

// e_damage_type, the ones ROWOW.h doesn't have (a skill's Hit is one of these)
#define ROWOW_DMG_ENDURE                            4
#define ROWOW_DMG_SPLASH                            5
#define ROWOW_DMG_SINGLE                            6
#define ROWOW_DMG_REPEAT                            7
#define ROWOW_DMG_MULTI_HIT_ENDURE                  9
#define ROWOW_DMG_TOUCH                             12
#define ROWOW_DMG_MULTI_HIT_CRITICAL                13
#define ROWOW_DMG_SPLASH_ENDURE                     14

// e_element's special skill elements (a skill's Element)
#define ROWOW_ELEMENT_NONE                          -1
#define ROWOW_ELEMENT_WEAPON                        12      // ELE_WEAPON: the weapon's element
#define ROWOW_ELEMENT_ENDOWED                       13      // ELE_ENDOWED: the endowment status's element
#define ROWOW_ELEMENT_RANDOM                        14      // ELE_RANDOM

#define ROWOW_MAX_SKILL_LEVEL                       13      // MAX_SKILL_LEVEL
#define ROWOW_MAX_SKILL_ITEM_REQUIRE                10
#define ROWOW_MAX_SKILL_STATUS_REQUIRE              3

// e_cast_type: how skill_castend_id hands a skill on (skill_get_casttype)
#define ROWOW_CAST_GROUND                           0
#define ROWOW_CAST_DAMAGE                           1
#define ROWOW_CAST_NODAMAGE                         2

// useskill_fail_cause, the ones the engine sends (clif_skill_fail)
#define ROWOW_USESKILL_FAIL_LEVEL                   0
#define ROWOW_USESKILL_FAIL_SP_INSUFFICIENT         1
#define ROWOW_USESKILL_FAIL_HP_INSUFFICIENT         2
#define ROWOW_USESKILL_FAIL_STUFF_INSUFFICIENT      3
#define ROWOW_USESKILL_FAIL_SKILLINTERVAL           4
#define ROWOW_USESKILL_FAIL_MONEY                   5
#define ROWOW_USESKILL_FAIL_THIS_WEAPON             6
#define ROWOW_USESKILL_FAIL_REDJAMSTONE             7
#define ROWOW_USESKILL_FAIL_BLUEJAMSTONE            8
#define ROWOW_USESKILL_FAIL                         10
#define ROWOW_USESKILL_FAIL_TOTARGET                11
#define ROWOW_USESKILL_FAIL_HOLYWATER               13
#define ROWOW_USESKILL_FAIL_CART                    57
#define ROWOW_USESKILL_FAIL_NEED_ITEM               71
#define ROWOW_USESKILL_FAIL_NEED_EQUIPMENT          72
#define ROWOW_USESKILL_FAIL_SPIRITS                 74
#define ROWOW_USESKILL_FAIL_COINS                   75
#define ROWOW_USESKILL_FAIL_NEED_MORE_BULLET        84
#define ROWOW_USESKILL_FAIL_NO_AMMO                 1000    // clif_arrow_fail(ARROWFAIL_NO_AMMO), sent the same way here
#define ROWOW_USESKILL_FAIL_MAX                     1001    // No message

// e_skill_blown: skill_blown's flags
#define ROWOW_BLOWN_NONE                            0x00
#define ROWOW_BLOWN_DONT_SEND_PACKET                0x01
#define ROWOW_BLOWN_IGNORE_NO_KNOCKBACK             0x02
#define ROWOW_BLOWN_NO_KNOCKBACK_MAP                0x04
#define ROWOW_BLOWN_MD_KNOCKBACK_IMMUNE             0x08
#define ROWOW_BLOWN_TARGET_NO_KNOCKBACK             0x10
#define ROWOW_BLOWN_TARGET_BASILICA                 0x20

// e_skill_copy: who can copy a skill (CopyFlags' Skill)
#define ROWOW_SKILL_COPY_PLAGIARISM                 0x1
#define ROWOW_SKILL_COPY_REPRODUCE                  0x2

// The character variables (char_reg_num) Plagiarism keeps its copy in (SKILL_VAR_PLAGIARISM, SKILL_VAR_PLAGIARISM_LV)
#define ROWOW_SKILL_VAR_PLAGIARISM                  "CLONE_SKILL"
#define ROWOW_SKILL_VAR_PLAGIARISM_LV               "CLONE_SKILL_LV"

#define ROWOW_SECURITY_CASTTIME                     100     // SECURITY_CASTTIME: kept on canact_tick between cast begin and cast end
#define ROWOW_SKILL_QUEUE_MAX_WAIT                  1500    // A player's skill request waits for an after-cast delay this short (ms) instead of failing
#define ROWOW_SKILL_NOCONSUME_REQ                   0x10000000 // SKILL_NOCONSUME_REQ: the castend already used up the requirements
#define ROWOW_MAX_HEAL_SKILL_LEVEL_DEFAULT          11      // battle_config.max_heal_lv's default

// Ground skill units (skill.hpp)
#define ROWOW_SKILLUNITTIMER_INTERVAL               100     // SKILLUNITTIMER_INTERVAL: how often skill_unit_timer runs over a map's units (ms)
#define ROWOW_MC_SKILLBASE                          8201    // MC_SKILLBASE: where a mercenary's skills start
#define ROWOW_MAX_MERCSKILL                         41      // MAX_MERCSKILL
#define ROWOW_MAX_SKILLUNITGROUPTICKSET             25      // MAX_SKILLUNITGROUPTICKSET: the unit groups a unit remembers being hit by
#define ROWOW_MAX_SQUARE_LAYOUT                     7       // MAX_SQUARE_LAYOUT: the square layouts go up to 15x15
#define ROWOW_SKILL_UNIT_GROUP_ID_START             1500    // skill_unit_group_newid starts at MAX_SKILL (group IDs never look like skill IDs)
#define ROWOW_UNIT_NOCONSUME_AMMO                   0x02    // UNIT_NOCONSUME_AMMO: a skill_unitsetting flag (the group doesn't use up the ammo)
#define ROWOW_ICEWALL_WALK_CHECK_CELLS              10      // The most cells apart two of a walk's positions the server sees can be (Ice Wall's crossing check)

// Timer skills (skill.hpp): a skill's later hits, queued on its caster (Jupitel Thunder's and Water Ball's hits, Double Casting's bolt)
#define ROWOW_TIMERSKILL_INTERVAL                   150     // TIMERSKILL_INTERVAL: ms between a timer skill's hits
#define ROWOW_MAX_SKILLTIMERSKILL                   50      // MAX_SKILLTIMERSKILL: the timer skills a unit can have queued

// A Monk's Spirit Spheres (pc.hpp) and a Priest's partners (skill_check_pc_partner)
#define ROWOW_MAX_SPIRITBALL                        15      // MAX_SPIRITBALL: the most Spirit Spheres a player can have
#define ROWOW_MAX_PARTY                             12      // MAX_PARTY: the most partners a partner search keeps

// A Hunter's traps and a Bard's or Dancer's songs (skill.hpp, item.hpp)
#define ROWOW_ITEMID_TRAP                           1065    // ITEMID_TRAP: Booby Trap, what a trap is made of and gives back
#define ROWOW_ITEMID_TRAP_ALLOY                     7940    // ITEMID_TRAP_ALLOY: Special Alloy Trap (a Ranger's, with Research Trap)
#define ROWOW_TRAP_USED_LIMIT                       1500    // How long a sprung trap stays (as UNT_USED_TRAPS) before it goes

// e_dance_overlap: what skill_dance_overlap does with the song or dance units on a cell
enum ROWOWDanceOverlap : uint8
{
    ROWOW_OVERLAP_REMOVE = 0, // OVERLAP_REMOVE: the unit goes; the units it turned into Dissonance turn back
    ROWOW_OVERLAP_SET, // OVERLAP_SET: a song and a dance on one cell turn into Dissonance (or Ugly Dance)
    ROWOW_OVERLAP_COUNT, // OVERLAP_COUNT: only count the units that overlap
};

// What a skill level needs and uses up (rAthena's s_skill_condition, skill_get_requirement)
struct ROWOWSkillCondition
{
    int32 HP = 0;
    int32 SP = 0;
    int32 Zeny = 0;
    int32 SpiritBall = 0;
    uint32 Weapon = 0; // Bits of ROWOW_WEAPON_TYPE_*
    uint32 Ammo = 0; // Bits of ROWOW_AMMO_TYPE_*
    int32 AmmoAmount = 0;
    int32 State = 0; // ROWOW_ST_*
    uint32 ItemIDs[ROWOW_MAX_SKILL_ITEM_REQUIRE] = { }; // item_db Ids
    int32 ItemAmounts[ROWOW_MAX_SKILL_ITEM_REQUIRE] = { };
    std::vector<int32> Statuses;
    std::vector<uint32> EquipItemIDs;
};

// skill_area_temp: what a skill's area search tells the calls it makes on each unit it finds (the count of targets, the first target,
// the cast spot).  rAthena keeps it in a global; here it lives on the caster (ROWOWSkillUnitData) and is put back after each search, so a
// search inside another (an autospell) leaves the outer one's alone
struct ROWOWSkillAreaTemp
{
    int32 Values[8] = { };
    ObjectGuid TargetGUID; // skill_area_temp[1]: the unit the skill was cast on
};

// skill_unit_group_tickset: when a unit group may next hit a unit (by group, or by skill for UF_NOOVERLAP skills)
struct ROWOWSkillUnitGroupTickset
{
    uint64 Tick = 0;
    int32 ID = 0;
};

// skill_timerskill: a hit (or other later part) of a skill queued on its caster for a tick, at a target or (no target) a cell of its map.
// type is the skill's own value (the attack type, or the hit's number)
struct ROWOWSkillTimerSkill
{
    uint32 ID = 0; // Which queued entry the caster's event is for (rAthena's timer id)
    uint64 Tick = 0;
    ObjectGuid TargetGUID;
    uint16 SkillID = 0;
    uint16 SkillLevel = 0;
    uint32 MapID = 0;
    int32 X = 0;
    int32 Y = 0;
    int32 Type = 0;
    int32 Flag = 0;
};

// A unit's skill use (rAthena's unit_data parts): when it may act again, the skill it's casting, its skill cooldowns, its ground skill
// unit groups and the unit groups' ticks on it.  Kept on the unit as "ROWOWSkillUnitData"; only the unit's own map thread touches it
struct ROWOWSkillUnitData : public DataMap::Base
{
    uint64 CanActTime = 0; // canact_tick: no skill can start before it (the after-cast delay, game time in ms)
    uint16 SkillID = 0; // skill_id and skill_lv of the cast in progress, 0 when none
    uint16 SkillLevel = 0;
    ObjectGuid SkillTargetGUID; // skilltarget
    float SkillZ = 0; // The ground height of a ground skill's spot (skillx and skilly are its cell), which its units are placed at
    bool IsSkillCastCancel = false; // state.skillcastcancel: damage stops the cast
    bool IsSkillTimer = false; // skilltimer: a cast with a cast time is running (Free Cast's slowness)
    std::unordered_map<uint16, uint64> CooldownEndTimesBySkillID; // blockskill (skill_blockpc_start): when each skill can be used again
    ROWOWSkillAreaTemp AreaTemp;
    std::vector<int32> SkillUnitGroupIDs; // skillunits: its ground skills' unit groups (on its map, the only one they can be on)
    ROWOWSkillUnitGroupTickset SkillUnitTicks[ROWOW_MAX_SKILLUNITGROUPTICKSET] = { }; // skillunittick
    bool IsSkillUnitCellSet = false; // The cell skill_unit_move last placed it on (the cell it walks out of next)
    uint32 SkillUnitMapID = 0;
    uint32 SkillUnitInstanceID = 0;
    int32 SkillUnitCellX = 0;
    int32 SkillUnitCellY = 0;
    uint64 SkillBlownTick = 0; // When skill_blown last moved it (it left its cell then, though WoW moves it over the next moments)
    bool IsAutoCast = false; // state.autocast: an autospell is being cast (it costs no SP)
    std::vector<uint32> AutoSpellLocks; // The bAutoSpellOnSkill autospells going (their lock), by trigger skill << 16 | skill
    std::vector<ObjectGuid> BowlingTargetGUIDs; // bowling_db: the units its Bowling Bash in progress has hit (kept per caster, not global)
    std::vector<ROWOWSkillTimerSkill> TimerSkills; // skilltimerskill: its timer skills waiting for their tick (each an event on the unit)
    uint32 NextTimerSkillID = 0;
    uint16 SkillIDOld = 0; // sd->skill_id_old and skill_lv_old: the skill whose cast Cast Cancel stopped
    uint16 SkillLevelOld = 0;
    uint16 SkillItemID = 0; // sd->skillitem and skillitemlv: a skill it may cast once without knowing it (Abracadabra's pick), at that level
    uint16 SkillItemLevel = 0;
    bool IsAbraFlag = false; // state.abra_flag: the skill item is Abracadabra's (its requirements aren't checked or paid)
    bool IsSkillLookCast = false; // A skill spell is being cast only for its look (clif_skill_poseffect, clif_skill_nodamage): the engine did the rest
    std::vector<uint64> SpiritBallEndTimes; // spirit_timer: when each of a player's Spirit Spheres (spiritball, their count) goes, soonest first
    int32 SpiritBallOld = 0; // spiritball_old: the spheres it had when a skill that uses them started (Throw Spirit Sphere, Asura Strike)
    std::vector<ObjectGuid> PartnerGUIDs; // skill_check_pc_partner's p_sd: the partners its last search found (kept per caster, not global)
    uint16 SkillIDDance = 0; // sd->skill_id_dance and skill_lv_dance: its last song or dance, which Encore performs again
    uint16 SkillLevelDance = 0;
    uint16 EncoreSkillID = 0; // The dance Encore starts for it (unit_skilluse_id from the server, not a client request: no after-cast delay check)
    std::string Message; // sd->message: what its next Talkie Box says (clif_parse_UseSkillToPosMoreInfo's text, here `.ro message`)
    int32 PotionHP = 0; // potion_hp and potion_sp: the heal its Slim Potion Pitcher gives each it lands on (rAthena's globals, kept per caster)
    int32 PotionSP = 0;
    std::vector<ObjectGuid> SummonGUIDs; // The monsters it summoned that answer to it (master_id): an Alchemist's plants and Marine Spheres
    bool IsRunning = false; // state.running: a Taekwon's Sprint is moving it (unit_run)
    // A player's skill asked for during a short after-cast delay (a normal attack's act delay, mostly), cast for it once the delay ends.  RO's
    // client never sends a skill then, but WoW's client does whenever the button is pressed, so the request waits instead of failing
    uint32 QueuedSkillSpellID = 0; // The skill level's spell, 0 when none waits
    ObjectGuid QueuedSkillTargetGUID; // The unit it was asked for at (none for a ground or self skill)
    bool IsQueuedSkillDestSet = false; // A ground skill's spot
    uint32 QueuedSkillMapID = 0;
    Position QueuedSkillDest;
    uint64 QueuedSkillTime = 0; // When it was asked for
    bool IsQueuedSkillCasting = false; // The queued skill is being cast now: it fails as any other request would, not queued again
};

// The Star Gladiator's places and hatreds, and the Taekwon's mission (pc.hpp): kept in character variables, as rAthena's
#define ROWOW_MAX_PC_FEELHATE                       3       // MAX_PC_FEELHATE: the sun, the moon and the star
#define ROWOW_AREA_SIZE                             14      // AREA_SIZE (battle_config.area_size): the cells a unit sees around it, as far as a run goes at once
#define ROWOW_TKMISSIONID_VAR                       "TK_MISSION_ID"
#define ROWOW_TKMISSIONCOUNT_VAR                    "TK_MISSION_COUNT"
#define ROWOW_PCDIECOUNTER_VAR                      "PC_DIE_COUNTER"
#define ROWOW_SG_FEEL_VARS                          { "PC_FEEL_SUN", "PC_FEEL_MOON", "PC_FEEL_STAR" }
#define ROWOW_SG_HATE_VARS                          { "PC_HATE_MOB_SUN", "PC_HATE_MOB_MOON", "PC_HATE_MOB_STAR" }

// A monster's special AI (e_mob_ai: special_state.ai): AI_NONE a monster of its own, AI_FLORA a summoned plant that attacks its master's
// enemies, AI_SPHERE a summoned Marine Sphere that blows up when hit
#define ROWOW_AI_NONE                               0
#define ROWOW_AI_ATTACK                             1
#define ROWOW_AI_SPHERE                             2
#define ROWOW_AI_FLORA                              3

// The monsters skills summon (rAthena's e_mob_bosstype / MOBID_*)
#define ROWOW_MOBID_RED_PLANT                       1078
#define ROWOW_MOBID_BLUE_PLANT                      1079
#define ROWOW_MOBID_GREEN_PLANT                     1080
#define ROWOW_MOBID_YELLOW_PLANT                    1081
#define ROWOW_MOBID_WHITE_PLANT                     1082
#define ROWOW_MOBID_SHINING_PLANT                   1083
#define ROWOW_MOBID_BLACK_MUSHROOM                  1084
#define ROWOW_MOBID_MARINE_SPHERE                   1142
#define ROWOW_MOBID_G_PARASITE                      1555
#define ROWOW_MOBID_G_FLORA                         1575
#define ROWOW_MOBID_G_HYDRA                         1579
#define ROWOW_MOBID_G_MANDRAGORA                    1589
#define ROWOW_MOBID_G_GEOGRAPHER                    1590

// A monster a skill summoned (mob_once_spawn_sub's master_id and special_state.ai), kept on the creature as "ROWOWMobSummon".  Only the
// creature's map thread touches it
struct ROWOWMobSummon : public DataMap::Base
{
    ObjectGuid MasterGUID; // master_id: who it answers to (on its map), empty for none
    uint8 AI = ROWOW_AI_NONE; // ROWOW_AI_*
};

// skill_unit: one cell of a ground skill (a Fire Wall's cell, a Safety Wall, a trap), at an RO cell of its group's map
struct ROWOWSkillUnit
{
    int32 X = 0;
    int32 Y = 0;
    bool IsAlive = false;
    int32 Val1 = 0;
    int32 Val2 = 0;
    bool IsHidden = false;
    int32 Range = 0; // Cells around it it reaches (-1: disabled, not yet deleted)
    int64 Limit = 0; // ms after the group's tick it lasts
    float Z = 0; // The ground height of its cell
    ObjectGuid VisualGUID; // The dynamic object that shows it (the skill's unit spell), when each cell shows
};

// s_skill_unit_group: a ground skill's units (its cells) and what they share.  A map's groups are kept on the map, its caster has their IDs;
// only the map's thread touches them
struct ROWOWSkillUnitGroup
{
    ObjectGuid SrcGUID; // src_id: the caster
    int32 GroupID = 0;
    int32 TargetFlag = ROWOW_BCT_NOONE; // BCT_* for battle_check_target
    int32 BLFlag = 0; // bl_flag: the kinds of unit it affects (ROWOW_BL_PC, ROWOW_BL_MOB)
    uint64 Tick = 0; // When it was made
    int64 Limit = 0; // How long it lasts, in ms after the tick
    int32 Interval = 0; // How often its units hit a unit standing in them (ms, -1 for units that only act on stepping in and out)
    uint16 SkillID = 0;
    uint16 SkillLevel = 0;
    int32 Val1 = 0;
    int32 Val2 = 0;
    int32 Val3 = 0;
    std::string ValStr; // Talkie Box's and Graffiti's message
    ObjectGuid Val2GUID; // val2 when it holds a unit (the one Spider Web caught)
    uint16 UnitID = 0; // ROWOW_UNT_*
    int32 LinkGroupID = 0; // A group deleted with it
    int32 AliveCount = 0;
    uint32 ItemID = 0; // The item a trap was made of (item_db Id)
    bool IsAmmoConsume = false; // state.ammo_consume: the ammo is used up when the group goes
    uint8 SongDance = 0; // state.song_dance: 0x1 a song or dance, 0x2 an ensemble
    bool IsGuildAura = false;
    uint32 MapID = 0;
    uint32 InstanceID = 0;
    std::vector<ROWOWSkillUnit> Units;
    ObjectGuid VisualGUID; // The dynamic object that shows the whole group, when its cells don't show one by one
    std::vector<ObjectGuid> ViewerGUIDs; // The players Graffiti's message was shown to as it came into their view (clif_graffiti)
};

// A map's ground skill unit groups (rAthena's skillunit_group_db and the BL_SKILL blocks of the map), kept on the map as
// "ROWOWMapSkillUnits".  Only the map's thread touches it
struct ROWOWMapSkillUnits : public DataMap::Base
{
    std::map<int32, std::shared_ptr<ROWOWSkillUnitGroup>> GroupsByID;
    std::unordered_map<int64, std::vector<std::pair<int32, int32>>> UnitsByCell; // An RO cell's units: (group ID, unit index)
    int32 NextGroupID = ROWOW_SKILL_UNIT_GROUP_ID_START; // skill_unit_group_newid
    uint64 NextTimerTick = 0; // When skill_unit_timer is next due on the map (game time in ms)
};

// A skill's units' cells around the skill's spot (s_skill_unit_layout: x east, y north)
struct ROWOWSkillUnitLayout
{
    std::vector<std::pair<int16, int16>> Cells;
};

// skill_area_sub's function: what to do with each unit an area search finds (skill_castend_damage_id, skill_castend_nodamage_id, or count it)
enum class ROWOWSkillAreaFunc : uint8
{
    CastendDamageID,
    CastendNoDamageID,
    Count,
};

// An item a skill uses up (Requires_ItemCost), at one level or all
struct ROWOWSkillItemRequirement
{
    uint32 ItemID = 0; // item_db Id
    int32 Amount = 0;
    uint16 Level = 0; // 0 for every level
};

// An RO skill (rAthena's s_skill_db), loaded from mod_rowow_db_skill.  Per-level values are indexed by the level less 1 and hold
// ROWOW_MAX_SKILL_LEVEL levels: a single value fills them all, and levels a per-level list doesn't name take the last named level's value
struct ROWOWSkill
{
    uint16 SkillID = 0;
    std::string Name; // The skill_db Name (SM_BASH)
    std::string Description; // Bash
    uint16 MaxLevel = 1;
    bool IsPassive = false; // No TargetType (rAthena's skill_get_inf is 0)
    bool IsQuest = false; // INF2_ISQUEST: given by a quest, not learned with skill points
    bool IsWedding = false; // INF2_ISWEDDING
    bool IsSpirit = false; // INF2_ISSPIRIT: shown on the tree only while a Soul Linker's spirit links the job
    std::vector<uint32> SpellIDsByLevel; // The WoW spell of each level (index is the level less 1), from mod_rowow_skill_spell

    // skill_get_type, skill_get_inf, skill_get_hit, skill_get_nk, skill_get_inf2
    int32 AttackType = 0; // Type: ROWOW_BF_WEAPON, ROWOW_BF_MAGIC or ROWOW_BF_MISC, 0 for none
    int32 Inf = ROWOW_INF_PASSIVE_SKILL; // TargetType
    int32 Hit = 0; // Hit: ROWOW_DMG_NORMAL, ROWOW_DMG_SINGLE or ROWOW_DMG_MULTI_HIT
    std::bitset<ROWOW_NK_MAX> NK; // DamageFlags
    std::bitset<ROWOW_INF2_MAX> Inf2; // Flags
    int32 CastTimeFlags = 0; // ROWOW_SKILL_CAST_*
    int32 CastDelayFlags = 0;
    bool IsCastCancel = true; // CastCancel: damage interrupts the cast
    int32 CastDefenseReduction = 0;
    int32 ActiveInstance = 0; // skill_get_maxcount, by level below
    int32 StatusID = ROWOW_SC_NONE; // skill_get_sc: the status the skill gives (Status)
    uint8 CopyOption = 0; // copyable.option: ROWOW_SKILL_COPY_* (CopyFlags' Skill), who can copy it (Plagiarism)

    // Per level (skill_get_range, skill_get_num, skill_get_ele, skill_get_splash, skill_get_blewcount, skill_get_cast, skill_get_delay,
    // skill_get_walkdelay, skill_get_cooldown, skill_get_time, skill_get_time2, skill_get_maxcount)
    int32 Range[ROWOW_MAX_SKILL_LEVEL] = { }; // Cells; negative is a melee range that weapon range bonuses don't raise
    int32 HitCount[ROWOW_MAX_SKILL_LEVEL] = { }; // num: negative hits are shown as one hit of the total
    int32 Element[ROWOW_MAX_SKILL_LEVEL] = { }; // ROWOW_ELEMENT_* (with the special skill elements)
    int32 SplashArea[ROWOW_MAX_SKILL_LEVEL] = { }; // Cells around the target
    int32 Knockback[ROWOW_MAX_SKILL_LEVEL] = { }; // Cells
    int32 CastTime[ROWOW_MAX_SKILL_LEVEL] = { }; // ms
    int32 AfterCastActDelay[ROWOW_MAX_SKILL_LEVEL] = { }; // ms
    int32 AfterCastWalkDelay[ROWOW_MAX_SKILL_LEVEL] = { }; // ms
    int32 Cooldown[ROWOW_MAX_SKILL_LEVEL] = { }; // ms
    int32 Duration1[ROWOW_MAX_SKILL_LEVEL] = { }; // ms
    int32 Duration2[ROWOW_MAX_SKILL_LEVEL] = { }; // ms
    int32 MaxCount[ROWOW_MAX_SKILL_LEVEL] = { }; // ActiveInstance

    // Requirements (skill_get_hp, skill_get_sp, ...), per level
    int32 HPCost[ROWOW_MAX_SKILL_LEVEL] = { };
    int32 SPCost[ROWOW_MAX_SKILL_LEVEL] = { };
    int32 HPRateCost[ROWOW_MAX_SKILL_LEVEL] = { };
    int32 SPRateCost[ROWOW_MAX_SKILL_LEVEL] = { };
    int32 ZenyCost[ROWOW_MAX_SKILL_LEVEL] = { };
    int32 SpiritSphereCost[ROWOW_MAX_SKILL_LEVEL] = { };
    int32 AmmoAmount[ROWOW_MAX_SKILL_LEVEL] = { };
    uint32 RequiredWeapons = 0; // Bits of ROWOW_WEAPON_TYPE_* (1 << type), 0 for any
    uint32 RequiredAmmo = 0; // Bits of ROWOW_AMMO_TYPE_*, 0 for none
    int32 RequiredState = ROWOW_ST_NONE;
    std::vector<int32> RequiredStatuses; // ROWOW_SC_*
    std::vector<ROWOWSkillItemRequirement> RequiredItems;
    std::vector<uint32> RequiredEquipment; // item_db Ids, one of which must be worn

    // Its ground units (skill_get_unit_*)
    std::string UnitID; // Unit_Id (Firewall, Safetywall...), empty for none
    std::string UnitAlternateID;
    uint16 UnitTypeID = 0; // unit_id: Unit_Id as ROWOW_UNT_*, 0 for none
    uint16 UnitTypeID2 = 0; // unit_id2: Unit_AlternateId
    int32 UnitLayout[ROWOW_MAX_SKILL_LEVEL] = { }; // unit_layout_type: a square layout's size, or (-1 in skill_db) the index of its own layout
    int32 UnitRange[ROWOW_MAX_SKILL_LEVEL] = { };
    int32 UnitInterval = 0; // ms
    int32 UnitTarget = ROWOW_BCT_NOONE; // BCT_* (skill_get_unit_target)
    int32 UnitBLTarget = 0; // ROWOW_BL_* (skill_get_unit_bl_target)
    std::bitset<ROWOW_UF_MAX> UnitFlags;
    std::vector<uint32> UnitSpellIDsByLevel; // The spell that shows each level's units on the ground, 0 for none (mod_rowow_skill_unit_spell)
    std::vector<bool> IsUnitSpellPerCellByLevel; // ... on each cell, or once over the whole group
    std::vector<uint32> SpringSpellIDsByLevel; // A trap's spring look, cast on what it goes off on

    // Clamps a level into the per-level arrays (skill_get_lv)
    static int32 GetLevelIndex(uint16 level) { return level == 0 ? 0 : (level > ROWOW_MAX_SKILL_LEVEL ? ROWOW_MAX_SKILL_LEVEL - 1 : level - 1); }

    // skill_get_lv: a per-level value.  Past the arrays (a monster's skill can go that high) a value above 1 goes on by the steps of the last levels
    static int32 GetLevelValue(int32 const (&values)[ROWOW_MAX_SKILL_LEVEL], uint16 level)
    {
        int32 index = GetLevelIndex(level);
        if (level > ROWOW_MAX_SKILL_LEVEL && values[index] > 1 && index > 1)
        {
            int32 a = values[index - 2];
            int32 b = values[index - 1];
            int32 c = values[index];
            return c + ((level - ROWOW_MAX_SKILL_LEVEL + 1) * (b - a) / 2) + ((level - ROWOW_MAX_SKILL_LEVEL) * (c - b) / 2);
        }
        return values[index];
    }
};

// A skill Abracadabra can cast (abra_db's s_skill_abra_db), and its chance (in 10000) at each of Abracadabra's levels
struct ROWOWAbraSkill
{
    uint16 SkillID = 0;
    int32 Per[ROWOW_MAX_SKILL_LEVEL] = { };
};

// e_scs_flag: the states a status puts its unit in (status.yml's States), and the conditional ones the status code decides
enum ROWOWStatusState : uint8
{
    ROWOW_SCS_NONE = 0,
    ROWOW_SCS_NOMOVECOND,
    ROWOW_SCS_NOMOVE,
    ROWOW_SCS_NOPICKITEMCOND,
    ROWOW_SCS_NOPICKITEM,
    ROWOW_SCS_NODROPITEMCOND,
    ROWOW_SCS_NODROPITEM,
    ROWOW_SCS_NOCASTCOND,
    ROWOW_SCS_NOCAST,
    ROWOW_SCS_NOCHAT,
    ROWOW_SCS_NOCHATCOND,
    ROWOW_SCS_NOEQUIPITEM,
    ROWOW_SCS_NOEQUIPITEMCOND,
    ROWOW_SCS_NOUNEQUIPITEM,
    ROWOW_SCS_NOUNEQUIPITEMCOND,
    ROWOW_SCS_NOCONSUMEITEM,
    ROWOW_SCS_NOCONSUMEITEMCOND,
    ROWOW_SCS_NOATTACK,
    ROWOW_SCS_NOATTACKCOND,
    ROWOW_SCS_NOWARP,
    ROWOW_SCS_NOWARPCOND,
    ROWOW_SCS_NODEATHPENALTY,
    ROWOW_SCS_NODEATHPENALTYCOND,
    ROWOW_SCS_NOINTERACT,
    ROWOW_SCS_NOINTERACTCOND,
    ROWOW_SCS_MAX,
};

// e_scb_flag: the parts of a unit's status a status changes (status.yml's CalcFlags), which status_calc_bl works out again
enum ROWOWStatusCalcFlag : uint8
{
    ROWOW_SCB_NONE = 0,
    ROWOW_SCB_BASE,
    ROWOW_SCB_MAXHP,
    ROWOW_SCB_MAXSP,
    ROWOW_SCB_STR,
    ROWOW_SCB_AGI,
    ROWOW_SCB_VIT,
    ROWOW_SCB_INT,
    ROWOW_SCB_DEX,
    ROWOW_SCB_LUK,
    ROWOW_SCB_BATK,
    ROWOW_SCB_WATK,
    ROWOW_SCB_MATK,
    ROWOW_SCB_HIT,
    ROWOW_SCB_FLEE,
    ROWOW_SCB_DEF,
    ROWOW_SCB_DEF2,
    ROWOW_SCB_MDEF,
    ROWOW_SCB_MDEF2,
    ROWOW_SCB_SPEED,
    ROWOW_SCB_ASPD,
    ROWOW_SCB_DSPD,
    ROWOW_SCB_CRI,
    ROWOW_SCB_FLEE2,
    ROWOW_SCB_ATK_ELE,
    ROWOW_SCB_DEF_ELE,
    ROWOW_SCB_MODE,
    ROWOW_SCB_SIZE,
    ROWOW_SCB_RACE,
    ROWOW_SCB_RANGE,
    ROWOW_SCB_REGEN,
    ROWOW_SCB_DYE,
    ROWOW_SCB_MAX,
};

// e_status_change_flag: a status's Flags in status.yml
enum ROWOWStatusFlag : uint8
{
    ROWOW_SCF_NONE = 0,
    ROWOW_SCF_BLEFFECT,
    ROWOW_SCF_DISPLAYPC,
    ROWOW_SCF_NOCLEARBUFF,
    ROWOW_SCF_NOREMOVEONDEAD,
    ROWOW_SCF_NODISPELL,
    ROWOW_SCF_NOCLEARANCE,
    ROWOW_SCF_NOBANISHINGBUSTER,
    ROWOW_SCF_NOSAVE,
    ROWOW_SCF_NOSAVEINFINITE,
    ROWOW_SCF_REMOVEONDAMAGED,
    ROWOW_SCF_REMOVEONREFRESH,
    ROWOW_SCF_REMOVEONLUXANIMA,
    ROWOW_SCF_STOPATTACKING,
    ROWOW_SCF_STOPCASTING,
    ROWOW_SCF_STOPWALKING,
    ROWOW_SCF_BOSSRESIST,
    ROWOW_SCF_MVPRESIST,
    ROWOW_SCF_SETSTAND,
    ROWOW_SCF_FAILEDMADO,
    ROWOW_SCF_DEBUFF,
    ROWOW_SCF_REMOVEONCHANGEMAP,
    ROWOW_SCF_REMOVEONMAPWARP,
    ROWOW_SCF_REMOVECHEMICALPROTECT,
    ROWOW_SCF_OVERLAPIGNORELEVEL,
    ROWOW_SCF_SENDOPTION,
    ROWOW_SCF_ONTOUCH,
    ROWOW_SCF_UNITMOVE,
    ROWOW_SCF_NONPLAYER,
    ROWOW_SCF_SENDLOOK,
    ROWOW_SCF_DISPLAYNPC,
    ROWOW_SCF_REQUIREWEAPON,
    ROWOW_SCF_REQUIRESHIELD,
    ROWOW_SCF_MOBLOSETARGET,
    ROWOW_SCF_REMOVEELEMENTALOPTION,
    ROWOW_SCF_SUPERNOVICEANGEL,
    ROWOW_SCF_TAEKWONANGEL,
    ROWOW_SCF_MADOCANCEL,
    ROWOW_SCF_MADOENDCANCEL,
    ROWOW_SCF_RESTARTONMAPWARP,
    ROWOW_SCF_SPREADEFFECT,
    ROWOW_SCF_SENDVAL1,
    ROWOW_SCF_SENDVAL2,
    ROWOW_SCF_SENDVAL3,
    ROWOW_SCF_NOFORCEDEND,
    ROWOW_SCF_NOWARNING,
    ROWOW_SCF_REMOVEONUNEQUIP,
    ROWOW_SCF_REMOVEONUNEQUIPWEAPON,
    ROWOW_SCF_REMOVEONUNEQUIPARMOR,
    ROWOW_SCF_REMOVEONHERMODE,
    ROWOW_SCF_REQUIRENOWEAPON,
    ROWOW_SCF_REMOVEFROMHOMONWARP,
    ROWOW_SCF_REMOVEFROMHOMONMAPWARP,
    ROWOW_SCF_MAX,
};

// e_sc_opt1: the one "stopped" state a unit can be in (status.yml's Opt1)
#define ROWOW_OPT1_NONE                             0
#define ROWOW_OPT1_STONE                            1
#define ROWOW_OPT1_FREEZE                           2
#define ROWOW_OPT1_STUN                             3
#define ROWOW_OPT1_SLEEP                            4
#define ROWOW_OPT1_STONEWAIT                        6
#define ROWOW_OPT1_BURNING                          7
#define ROWOW_OPT1_IMPRISON                         8

// e_option: the looks a status gives (status.yml's Options), the ones the rules read
#define ROWOW_OPTION_SIGHT                          0x00000001
#define ROWOW_OPTION_HIDE                           0x00000002
#define ROWOW_OPTION_CLOAK                          0x00000004
#define ROWOW_OPTION_FALCON                         0x00000010  // A Hunter's falcon, kept with the identity (status.option)
#define ROWOW_OPTION_RIDING                         0x00000020  // A Knight's or Crusader's Peco, kept with the identity (status.option)
#define ROWOW_OPTION_WEDDING                        0x00001000  // SC_WEDDING's outfit (RO's JOB_WEDDING body)
#define ROWOW_OPTION_RUWACH                         0x00002000
#define ROWOW_OPTION_CHASEWALK                      0x00004000
#define ROWOW_OPTION_XMAS                           0x00010000  // SC_XMAS's outfit (JOB_XMAS, the Santa body)
#define ROWOW_OPTION_SUMMER                         0x00040000  // SC_SUMMER's outfit (JOB_SUMMER)
#define ROWOW_OPTION_HANBOK                         0x08000000
#define ROWOW_OPTION_OKTOBERFEST                    0x10000000
#define ROWOW_OPTION_SUMMER2                        0x20000000
// OPTION_COSTUME: an outfit is on, so no weapon or shield is drawn and no skill may be used (clif_parse_UseSkillToId / ToPos)
#define ROWOW_OPTION_COSTUME                        (ROWOW_OPTION_WEDDING | ROWOW_OPTION_XMAS | ROWOW_OPTION_SUMMER | ROWOW_OPTION_HANBOK | ROWOW_OPTION_OKTOBERFEST | ROWOW_OPTION_SUMMER2)

// The option looks the converter attaches to RO illusions (mod_rowow_option_look Kind)
#define ROWOW_OPTION_LOOK_CART                      1
#define ROWOW_OPTION_LOOK_FALCON                    2
#define ROWOW_OPTION_LOOK_DEVOTION_LINK             3   // The beam from a Crusader to each player it devotes (the aura's caster is the Crusader)

// The looks the NPC scripts show (mod_rowow_script_look Kind)
#define ROWOW_SCRIPT_LOOK_EMOTION                   1
#define ROWOW_SCRIPT_LOOK_EFFECT                    2
#define ROWOW_SCRIPT_LOOK_SOUND                     3
#define ROWOW_SCRIPT_LOOK_ITEM_USE                  4   // Not a look of its own: the effect look an RO item's use plays (by its item_db Id)

// e_status_change_start_flags
#define ROWOW_SCSTART_NONE                          0x00
#define ROWOW_SCSTART_NOAVOID                       0x01    // Cannot be avoided (it has to start)
#define ROWOW_SCSTART_NOTICKDEF                     0x02    // Duration isn't reduced (by statuses or bonuses)
#define ROWOW_SCSTART_LOADED                        0x04    // Loaded from the saved statuses: the values stay as they were
#define ROWOW_SCSTART_NORATEDEF                     0x08    // Rate isn't reduced (by statuses or bonuses)
#define ROWOW_SCSTART_NOICON                        0x10    // No icon

// e_status_change_clear_buff_flags: what status_change_clear_buffs clears
#define ROWOW_SCCB_BUFFS                            0x01
#define ROWOW_SCCB_DEBUFFS                          0x02
#define ROWOW_SCCB_REFRESH                          0x04
#define ROWOW_SCCB_CHEM_PROTECT                     0x08
#define ROWOW_SCCB_LUXANIMA                         0x10
#define ROWOW_SCCB_HERMODE                          0x20

// How stealthed WoW makes a hidden unit (Hiding, Cloaking): past any WoW detection but the detecting RO monsters' (ROWOW_MODE_DETECTOR)
#define ROWOW_HIDING_STEALTH_VALUE                  1000

// e_status_bonus: status_get_hpbonus/status_get_spbonus's kinds
#define ROWOW_STATUS_BONUS_FIX                      0
#define ROWOW_STATUS_BONUS_RATE                     1
#define ROWOW_STATUS_BONUS_ITEM                     2       // status_get_spbonus_item: the rate of usable items' statuses

// e_atk_flag (ATF_*): who an item's added status goes to, on what kind of attack
#define ROWOW_ATF_SELF                              0x01
#define ROWOW_ATF_TARGET                            0x02
#define ROWOW_ATF_SHORT                             0x04
#define ROWOW_ATF_LONG                              0x08
#define ROWOW_ATF_WEAPON                            0x10
#define ROWOW_ATF_MAGIC                             0x20
#define ROWOW_ATF_MISC                              0x40

// e_break_location: SC_JOINTBEAT's broken joints
#define ROWOW_BREAK_ANKLE                           0x01
#define ROWOW_BREAK_WRIST                           0x02
#define ROWOW_BREAK_KNEE                            0x04
#define ROWOW_BREAK_SHOULDER                        0x08
#define ROWOW_BREAK_WAIST                           0x10
#define ROWOW_BREAK_NECK                            0x20
#define ROWOW_BREAK_FLAGS                           (ROWOW_BREAK_ANKLE | ROWOW_BREAK_WRIST | ROWOW_BREAK_KNEE | ROWOW_BREAK_SHOULDER | ROWOW_BREAK_WAIST | ROWOW_BREAK_NECK)

#define ROWOW_MAX_DEVOTION                          5       // MAX_DEVOTION: the players a Crusader can devote itself to at once

#define ROWOW_INFINITE_TICK                         -1      // INFINITE_TICK: a status with no end
#define ROWOW_MIN_WALK_SPEED                        20      // MIN_WALK_SPEED and MAX_WALK_SPEED: a walk speed's bounds (ms per cell)
#define ROWOW_MAX_WALK_SPEED                        1000
#define ROWOW_DEFAULT_WALK_SPEED                    150     // DEFAULT_WALK_SPEED: a player's, the speed of WoW's run speed
#define ROWOW_MIN_ASPD                              8000    // MIN_ASPD: the slowest attack delay (pre-renewal)
#define ROWOW_MAX_ASPD_NOPC                         100     // MAX_ASPD_NOPC: a monster's fastest

// A status of status.yml (rAthena's s_status_change_db), loaded from mod_rowow_db_status
struct ROWOWStatusChangeDB
{
    int32 Type = -1; // ROWOW_SC_*
    std::string Name; // status.yml's Status (Twohandquicken)
    std::string Icon; // Its EFST_ name, empty for none
    uint16 SkillID = 0; // DurationLookup: the skill whose durations it takes (status_db.getSkill)
    std::bitset<ROWOW_SCS_MAX> State;
    std::bitset<ROWOW_SCB_MAX> CalcFlag;
    std::bitset<ROWOW_SCF_MAX> Flag;
    uint32 Opt1 = ROWOW_OPT1_NONE;
    uint32 Opt2 = 0;
    uint32 Opt3 = 0;
    uint32 Look = 0; // Options (OPTION_*)
    int32 MinRate = 0; // Hundredths of a percent
    int64 MinDuration = 1; // ms
    std::vector<int32> Fail; // Statuses that stop it starting
    std::vector<int32> EndOnStart; // Statuses it ends when it starts
    std::vector<int32> EndReturn; // Statuses it ends instead of starting
    std::vector<int32> EndOnEnd; // Statuses that end with it
    std::shared_ptr<ROWOWScript> Script; // Its bonuses (bonus bStr,getstatus(SC_STRFOOD,1)), run by the player's status calculation
    uint32 SpellID = 0; // The marker aura the client shows it by (mod_rowow_status_spell), 0 for none
};

// One status change on a unit (status_change_entry): its values, its timer (status_change_timer: when it next ticks or ends) and when it
// runs out (what its icon shows).  Game time in ms
struct ROWOWStatusChangeEntry
{
    int32 Val1 = 0;
    int32 Val2 = 0;
    int32 Val3 = 0;
    int32 Val4 = 0;
    uint64 TimerTime = 0; // sce->timer: when status_change_timer next runs for it, 0 for an endless one (INVALID_TIMER)
    uint64 EndTime = 0; // When it runs out, 0 for an endless one
    ObjectGuid SourceGUID; // Who gave it
};

// What a unit's statuses keep it from doing (status_change's cant)
struct ROWOWStatusChangeCant
{
    bool Move = false;
    bool PickUp = false;
    bool Drop = false;
    bool Cast = false;
    bool Chat = false;
    bool Equip = false;
    bool Unequip = false;
    bool Consume = false;
    bool Attack = false;
    bool Warp = false;
    bool DeathPenalty = false;
    bool Interact = false;
};

// A unit's status changes (rAthena's status_change), kept on the unit as "ROWOWStatusChange".  Only the unit's own map thread touches it
struct ROWOWStatusChange : public DataMap::Base
{
    std::map<int32, ROWOWStatusChangeEntry> EntriesByType; // By ROWOW_SC_*
    uint32 Option = 0; // option, opt1, opt2, opt3
    uint32 Opt1 = 0;
    uint32 Opt2 = 0;
    uint32 Opt3 = 0;
    ROWOWStatusChangeCant Cant;
    int32 LastEffect = -1; // lastEffect: a stopped state that damage just ended, still blocking its Fail list for the rest of that attack
    uint64 LastEffectTime = 0; // When lastEffect is cleared (lastEffectTimer)
    bool IsRootSet = false; // The WoW states standing for Cant (and the hiding options) that are set on the unit now
    bool IsSilenceSet = false;
    bool IsPacifySet = false;
    bool IsStealthSet = false;
    uint64 FlinchEndTime = 0; // A player rooted by a hit until then (PCDamageFlinch, MergedWorlds ro-flinch), 0 when not
    int32 LastSpeed = 0; // The walk speed the unit's WoW speed was last set for (status_data's speed), 0 before any
    bool IsPassiveDamage = false; // A status_zap is being dealt (passive damage, status_damage's flag&1)
    int32 CloakCellX = 0; // The cell Cloaking last checked its wall at
    int32 CloakCellY = 0;
    int32 SGCounter = 0; // sg_counter: the Storm Gust hits it took since its last freeze (any Storm Gust's: the third freezes)

    ROWOWStatusChangeEntry* GetSCE(int32 type)
    {
        auto entryIter = EntriesByType.find(type);
        return entryIter != EntriesByType.end() ? &entryIter->second : nullptr;
    }
    ROWOWStatusChangeEntry const* GetSCE(int32 type) const
    {
        auto entryIter = EntriesByType.find(type);
        return entryIter != EntriesByType.end() ? &entryIter->second : nullptr;
    }
    bool HasSCE(int32 type) const { return EntriesByType.find(type) != EntriesByType.end(); }
    bool IsEmpty() const { return EntriesByType.empty() == true; }
};

// A status kept with the identity (rAthena's sc_data): its values and what was left on its timer (-1 for an endless one)
struct ROWOWSavedStatusChange
{
    int32 Type = -1;
    int64 Tick = 0;
    int32 Val1 = 0;
    int32 Val2 = 0;
    int32 Val3 = 0;
    int32 Val4 = 0;
};

// A status an item adds to attacks (rAthena's s_addeffect: bAddEff, bAddEff2), or to the attacker when hit (bAddEffWhenHit)
struct ROWOWAddEffect
{
    int32 Type = -1; // ROWOW_SC_*
    int32 Rate = 0; // Hundredths of a percent
    int32 ArrowRate = 0;
    uint8 Flag = 0; // ROWOW_ATF_*
    uint32 Duration = 0; // ms
};

// An autospell (s_autospell): a skill an item casts at a rate in 1000 on attacks of its battle flags (bAutoSpell), when hit (bAutoSpellWhenHit)
// or on using a skill (bAutoSpellOnSkill: TriggerSkillID)
#define ROWOW_AUTOSPELL_FORCE_SELF                  0x0
#define ROWOW_AUTOSPELL_FORCE_TARGET                0x1
#define ROWOW_AUTOSPELL_FORCE_RANDOM_LEVEL          0x2
#define ROWOW_AUTOSPELL_FORCE_ALL                   0x3
struct ROWOWAutoSpell
{
    uint16 SkillID = 0;
    uint16 Level = 0;
    int16 Rate = 0;
    int16 BattleFlag = 0; // ROWOW_BF_*
    uint32 CardID = 0; // The card that gives it, so two of one card don't stack (autospell_stacking)
    uint8 Flag = 0; // ROWOW_AUTOSPELL_FORCE_*
    uint16 TriggerSkillID = 0;
};

// An autobonus (s_autobonus): a bonus script run for a while (Duration, ms) at a rate in 1000 on attacks of its type (AtkType: BF_* flags, or
// autobonus3's skill), and the script run when it starts (an effect).  Pos is where its items are worn
struct ROWOWAutoBonus
{
    std::string BonusScript;
    int16 Rate = 0;
    uint32 Duration = 0;
    uint16 AtkType = 0;
    std::string OtherScript;
    uint32 Pos = 0;
};

// An autobonus going (its active timer): until EndTime its script counts in the status
struct ROWOWActiveAutoBonus
{
    std::string BonusScript;
    uint32 Pos = 0;
    uint64 EndTime = 0;
};

// A status an item adds to one skill's hits (s_addeffectonskill: bAddEffOnSkill)
struct ROWOWAddEffectOnSkill
{
    int32 Type = -1;
    int32 Rate = 0;
    uint16 SkillID = 0;
    uint8 Target = 0; // ROWOW_ATF_SELF or ROWOW_ATF_TARGET
    uint32 Duration = 0;
};

struct ROWOWDamage;

// One skill's own rules (rAthena's SkillImpl): each virtual replaces that skill's case in the switch it names.  The defaults do nothing.
// src and targets are RO combatants on the same map (a player in the form or an RO monster); x and y are RO cells of the caster's map
class ROWOWSkillImpl
{
public:
    explicit ROWOWSkillImpl(uint16 skillID) : SkillID(skillID) { }
    virtual ~ROWOWSkillImpl() = default;
    uint16 GetSkillID() const { return SkillID; }

    virtual void CastendNoDamageID(Unit* /*src*/, Unit* /*target*/, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*flag*/) const { } // skill_castend_nodamage_id
    virtual void CastendDamageID(Unit* /*src*/, Unit* /*target*/, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*flag*/) const { } // skill_castend_damage_id
    virtual void CastendPos2(Unit* /*src*/, int32 /*x*/, int32 /*y*/, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*flag*/) const { } // skill_castend_pos2
    virtual void CalculateSkillRatio(ROWOWDamage const* /*damage*/, Unit const* /*src*/, Unit const* /*target*/, uint16 /*skillLevel*/, int32& /*baseSkillRatio*/, int32 /*mflag*/) const { } // battle_calc_*_skill_ratio
    virtual void ModifyHitRate(int16& /*hitRate*/, Unit const* /*src*/, Unit const* /*target*/, uint16 /*skillLevel*/) const { }
    virtual void ApplyAdditionalEffects(Unit* /*src*/, Unit* /*target*/, uint16 /*skillLevel*/, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const { } // skill_additional_effect
    virtual void ApplyCounterAdditionalEffects(Unit* /*src*/, Unit* /*target*/, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*attackType*/) const { } // skill_counter_additional_effect
    virtual void ModifyDamageData(ROWOWDamage& /*damage*/, Unit const& /*src*/, Unit const& /*target*/, uint16 /*skillLevel*/) const { }
    virtual void ModifyElement(ROWOWDamage const& /*damage*/, Unit const& /*src*/, Unit const& /*target*/, uint16 /*skillLevel*/, int32& /*element*/, int32 /*flag*/) const { }

protected:
    uint16 SkillID;
};

// StatusSkillImpl: gives the skill's status (skill_get_sc) to the target for the skill's Duration1, or ends it when running and asked to
class ROWOWStatusSkillImpl : public ROWOWSkillImpl
{
public:
    ROWOWStatusSkillImpl(uint16 skillID, bool isEndIfRunning = false) : ROWOWSkillImpl(skillID), IsEndIfRunning(isEndIfRunning) { }
    void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override;

private:
    bool IsEndIfRunning;
};

// WeaponSkillImpl: a weapon attack on the target (skill_attack with BF_WEAPON)
class ROWOWWeaponSkillImpl : public ROWOWSkillImpl
{
public:
    explicit ROWOWWeaponSkillImpl(uint16 skillID) : ROWOWSkillImpl(skillID) { }
    void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override;
};

// SkillImplRecursiveDamageSplash: hits everything around the target (or the ground spot), each through CastendDamageID with flag & 1
class ROWOWRecursiveDamageSplashSkillImpl : public ROWOWSkillImpl
{
public:
    explicit ROWOWRecursiveDamageSplashSkillImpl(uint16 skillID) : ROWOWSkillImpl(skillID) { }
    void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override;
    void CastendPos2(Unit* src, int32 x, int32 y, uint16 skillLevel, uint64 tick, int32& flag) const override;
    virtual int16 GetSearchSize(Unit* src, uint16 skillLevel) const;
    virtual int16 GetSplashSearchSize(Unit* src, uint16 skillLevel) const;
    virtual void SplashSearch(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32 flag) const;
    virtual int64 SplashDamage(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32 flag) const;
};

// Each job's skill classes (rAthena's skill_factory_<job>), one file per job (ROWOW_Skills<Job>.cpp), registered at startup
void AddROWOWSkillsNovice();
void AddROWOWSkillsSwordman();
void AddROWOWSkillsMage();
void AddROWOWSkillsAcolyte();
void AddROWOWSkillsArcher();
void AddROWOWSkillsMerchant();
void AddROWOWSkillsThief();
void AddROWOWSkillsTaekwon();
void AddROWOWSkillsGunslinger();
void AddROWOWSkillsNinja();
void AddROWOWSkillsNpc();
void AddROWOWSkillsWedding(); // ROWOW_Family.cpp
void AddROWOWSkillsHomunculus();
void AddROWOWSkillsMercenary();

#endif
