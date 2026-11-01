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
//  Homunculi are a port of rAthena's homunculus.cpp:
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
//  For more information, see LICENCE in the main folder

#include "Chat.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Item.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "WorldPacket.h"

#include "ROWOW.h"
#include "ROWOW_Skill.h"

using namespace std;

// An Alchemist's homunculus (R11), homunculus.cpp under its names.  A homunculus (homun_data) is ROWOWHomunData on the master's identity, whether it's out, resting or dead, with its row
// (mod_rowow_homunculus, s_homunculus) saved with the character.  What it shares with a mercenary, its creature and AI, is ROWOW_Follower.cpp

namespace
{
    // Intimacy grade, order based on enum e_homun_grade
    uint32 const IntimacyGrades[] = { 100, 400, 1100, 10100, 25100, 75100, 91100 };

    constexpr uint32 HOMUNCULUS_SP_NAME_LENGTH = 23; // NAME_LENGTH

    void SendAddonMessage(Player* player, std::string const& message)
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_ADDON, nullptr, nullptr, message);
        player->SendDirectMessage(&data);
    }

    void SendHomMessage(Player* player, std::string const& message)
    {
        ChatHandler(player->GetSession()).SendSysMessage(message.c_str());
    }
}

ROWOWHomunculusDB const* ROWOWMod::GetHomunculusDB(uint32 classID) const
{
    auto dbIter = HomunculiByClass.find(classID);
    return dbIter != HomunculiByClass.end() ? &dbIter->second : nullptr;
}

uint64 ROWOWMod::HomGetNextExp(int32 level) const
{
    return level >= 0 && size_t(level) < HomunculusExpByLevel.size() ? HomunculusExpByLevel[size_t(level)] : 0;
}

ROWOWHomunData* ROWOWMod::GetHomunculus(Player const* player) const
{
    ROWOWIdentity const* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    return identity != nullptr ? identity->Homunculus.get() : nullptr;
}

bool ROWOWMod::HomIsActive(ROWOWHomunData const* hd)
{
    return hd != nullptr && hd->Vaporize == ROWOW_HOM_ST_ACTIVE && hd->HP > 0;
}

int16 ROWOWMod::HomCheckSkill(Unit const* hd, uint16 skillID) const
{
    // hom_checkskill
    ROWOWFollower const* fd = GetFollowerType(hd) == ROWOW_BL_HOM ? GetFollower(hd) : nullptr;
    if (fd == nullptr)
        return 0;
    ROWOWHomunData const* homunculus = static_cast<ROWOWHomunData const*>(fd);
    auto skillIter = homunculus->Skills.find(skillID);
    return skillIter != homunculus->Skills.end() ? int16(skillIter->second) : 0;
}

uint8 ROWOWMod::HomGetIntimacyGrade(ROWOWHomunData const& hd) const
{
    // hom_intimacy_intimacy2grade
    for (int32 grade = ROWOW_HOMGRADE_LOYAL; grade > ROWOW_HOMGRADE_HATE_WITH_PASSION; grade--)
        if (hd.Intimacy >= IntimacyGrades[grade])
            return uint8(grade);
    return ROWOW_HOMGRADE_HATE_WITH_PASSION;
}

uint32 ROWOWMod::HomIntimacyGrade2Intimacy(uint8 grade) const
{
    return grade <= ROWOW_HOMGRADE_LOYAL ? IntimacyGrades[grade] : 0;
}

uint32 ROWOWMod::HomIncreaseIntimacy(ROWOWHomunData& hd, uint32 value)
{
    // hom_increase_intimacy
    if (BattleConfig.HomunculusFriendlyRate != 100)
        value = value * uint32(BattleConfig.HomunculusFriendlyRate) / 100;
    hd.Intimacy = std::min<uint32>(hd.Intimacy + value, 100000);
    return hd.Intimacy;
}

uint32 ROWOWMod::HomDecreaseIntimacy(ROWOWHomunData& hd, uint32 value)
{
    // hom_decrease_intimacy
    hd.Intimacy = hd.Intimacy >= value ? hd.Intimacy - value : 0;
    return hd.Intimacy;
}

void ROWOWMod::HomCalcSkillTree(ROWOWHomunData& hd)
{
    // hom_calc_skilltree_sub, for the class's tree: what its intimacy, evolution, level and needed skills let it know.  (A prev_class only comes with a mutation, which is renewal's)
    if (hd.DB == nullptr)
        return;
    bool isEvolved = hd.ClassID == hd.DB->EvoClass;
    for (ROWOWHomSkillTreeEntry const& skit : hd.DB->SkillTree)
    {
        if (hd.Skills.find(skit.SkillID) != hd.Skills.end())
            continue; // Skill already known.
        if (skit.Intimacy > 0 && hd.Intimacy < skit.Intimacy)
            continue;
        if (skit.IsEvolution == true && isEvolved == false)
            continue;
        if (skit.NeedLevel > hd.Level)
            continue;
        bool isFail = false;
        for (auto const& need : skit.Need)
        {
            auto needIter = hd.Skills.find(need.first);
            if (need.first > 0 && (needIter == hd.Skills.end() || needIter->second < need.second))
            {
                isFail = true;
                break;
            }
        }
        if (isFail == false)
            hd.Skills[skit.SkillID] = 0;
    }
}

void ROWOWMod::StatusCalcHomunculus(ROWOWHomunData& hd, bool isFirst)
{
    // status_calc_homunculus_.  The stats are hom.str and the like in tenths, its passives add to them, and status_calc_misc works out the rest
    ROWOWStatus& status = hd.BaseStatus;
    for (int32 i = 0; i < ROWOW_STAT_COUNT; i++)
        status.Stats[i] = hd.Stats[i] / 10;
    if (isFirst == true)
    {
        ROWOWHomunculusDB const* db = hd.DB;
        status.IsPlayer = false;
        status.FollowerType = ROWOW_BL_HOM;
        status.HomBaseASPD = db->BaseASPD;
        status.DefElement = db->Element;
        status.ElementLevel = 1;
        status.Race = db->Race;
        status.Class = ROWOW_CLASS_NORMAL;
        status.Size = hd.ClassID == db->EvoClass ? db->EvoSize : db->BaseSize;
        status.Range = 1 + status.Size;
        status.Mode = ROWOW_MODE_CANMOVE | ROWOW_MODE_CANATTACK;
        status.Speed = ROWOW_DEFAULT_WALK_SPEED;
    }
    status.Level = uint32(hd.Level);
    status.AspdRate = 1000;
    int32 const vit = status.Stats[ROWOW_STAT_VIT];
    int32 const int_ = status.Stats[ROWOW_STAT_INT];
    status.Def = std::clamp(hd.Level / 10 + vit / 5, 0, 99);
    status.MDef = std::clamp(hd.Level / 10 + int_ / 5, 0, 99);
    int32 amotion = (1000 - 4 * status.Stats[ROWOW_STAT_AGI] - status.Stats[ROWOW_STAT_DEX]) * status.HomBaseASPD / 1000;
    status.AttackMotion = std::clamp(amotion, ROWOW_MAX_ASPD_NOPC, ROWOW_MIN_ASPD);
    status.AttackDelay = status.AttackMotion; // It seems adelay = amotion for Homunculus.
    status.MaxHP = hd.MaxHP;
    status.MaxSP = hd.MaxSP;

    HomCalcSkillTree(hd);
    auto checkSkill = [&hd](uint16 skillID)
    {
        auto skillIter = hd.Skills.find(skillID);
        return skillIter != hd.Skills.end() ? int32(skillIter->second) : 0;
    };
    int32 skillLevel = 0;
    if ((skillLevel = checkSkill(ROWOW_SKILL_HAMI_SKIN)) > 0)
        status.Def += skillLevel * 4;
    if ((skillLevel = checkSkill(ROWOW_SKILL_HVAN_INSTRUCT)) > 0)
    {
        static int32 const bonusInt[] = { 1, 2, 2, 4, 5 };
        static int32 const bonusStr[] = { 1, 1, 3, 4, 4 };
        status.Stats[ROWOW_STAT_INT] += bonusInt[skillLevel - 1];
        status.Stats[ROWOW_STAT_STR] += bonusStr[skillLevel - 1];
    }
    if ((skillLevel = checkSkill(ROWOW_SKILL_HAMI_SKIN)) > 0)
        status.MaxHP += skillLevel * 2 * status.MaxHP / 100;
    if ((skillLevel = checkSkill(ROWOW_SKILL_HLIF_BRAIN)) > 0)
        status.MaxSP += skillLevel * status.MaxSP / 100;

    status.WeaponAttack = status.Stats[ROWOW_STAT_DEX];
    status.WeaponRefineAttack = status.Stats[ROWOW_STAT_STR] + hd.Level;

    // Non players get the value set
    status.BaseAttack = status.Hit = status.Flee = status.Def2 = status.MDef2 = status.Critical = status.Flee2 = 0;
    StatusCalcMisc(status, uint32(hd.Level));
}

void ROWOWMod::HomSave(ROWOWHomunData const& hd, ObjectGuid::LowType charGUID, CharacterDatabaseTransaction trans) const
{
    // hom_save.  (Its blocks don't outlive a save: they're seconds, and it needs its master's map to act)
    std::string name = hd.Name;
    CharacterDatabase.EscapeString(name);
    std::string skills;
    for (auto const& skill : hd.Skills)
        skills += fmt::format("{}{}:{}", skills.empty() == true ? "" : ",", skill.first, skill.second);
    trans->Append("REPLACE INTO `mod_rowow_homunculus` (`CharacterGUID`, `Class`, `PrevClass`, `Name`, `Level`, `Exp`, `Intimacy`, `Hunger`, `Str`, `Agi`, `Vit`, `Int`, `Dex`, `Luk`, `HP`, `MaxHP`, `SP`, `MaxSP`, "
        "`SkillPoints`, `IsRenamed`, `Vaporize`, `IsAutoFeed`, `Skills`) VALUES ({}, {}, {}, '{}', {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, '{}')", charGUID, hd.ClassID, hd.PrevClass, name,
        hd.Level, hd.Exp, hd.Intimacy, hd.Hunger, hd.Stats[ROWOW_STAT_STR], hd.Stats[ROWOW_STAT_AGI], hd.Stats[ROWOW_STAT_VIT], hd.Stats[ROWOW_STAT_INT], hd.Stats[ROWOW_STAT_DEX], hd.Stats[ROWOW_STAT_LUK], hd.HP,
        hd.MaxHP, hd.SP, hd.MaxSP, hd.SkillPoints, hd.IsRenamed == true ? 1 : 0, hd.Vaporize, hd.IsAutoFeed == true ? 1 : 0, skills);
}

bool ROWOWMod::HomCall(Player* sd)
{
    // hom_call: a character with none creates a random one (hom_create_request and hom_recv_data), one at rest comes back to its master
    ROWOWIdentity* identity = GetIdentity(sd);
    if (identity->Homunculus == nullptr)
    {
        uint32 classID = ROWOW_HM_CLASS_BASE + urand(0, 7);
        ROWOWHomunculusDB const* db = GetHomunculusDB(classID);
        if (db == nullptr)
            return false;
        identity->Homunculus = std::make_unique<ROWOWHomunData>();
        HomCreate(sd, *identity->Homunculus, *db);
        return true;
    }
    ROWOWHomunData& hd = *identity->Homunculus;
    if (hd.Vaporize == ROWOW_HOM_ST_ACTIVE)
        return false; // Can't use this if homun wasn't vaporized.
    if (hd.Vaporize == ROWOW_HOM_ST_MORPH)
        return false; // Can't call homunculus (morph state).

    // hom_init_timers
    int32 hungerDelay = (BattleConfig.HomunculusStarvingRate > 0 && hd.Hunger <= BattleConfig.HomunculusStarvingRate) ? BattleConfig.HomunculusStarvingDelay : hd.DB->HungryDelay;
    hd.HungryTimer = uint32(std::max(hungerDelay, 1));
    hd.Vaporize = ROWOW_HOM_ST_ACTIVE;
    FollowerSpawn(sd, hd);
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    HomSave(hd, sd->GetGUID().GetCounter(), trans);
    CharacterDatabase.CommitTransaction(trans);
    ClifHomInfo(sd);
    return true;
}

bool ROWOWMod::HomVaporize(Player* sd, int32 flag)
{
    // hom_vaporize: flag HOM_ST_REST needs 80% of its HP.  Its statuses and delays end with it, its timers stop, and it's put away with its state
    ROWOWHomunData* hd = GetHomunculus(sd);
    if (hd == nullptr || hd->Vaporize != ROWOW_HOM_ST_ACTIVE)
        return false;
    FollowerSaveVitals(sd, *hd);
    Creature* md = GetFollowerCreature(sd, *hd);
    if (hd->HP == 0 || md == nullptr)
        return false; // Can't vaporize a dead homun.
    if (flag == ROWOW_HOM_ST_REST && uint64(hd->HP) * 100 / std::max<uint32>(md->GetMaxHealth(), 1) < 80)
        return false;
    hd->Vaporize = flag != ROWOW_HOM_ST_ACTIVE ? flag : ROWOW_HOM_ST_REST;
    if (BattleConfig.HomDelayResetVaporize != 0)
        hd->SkillBlocks.clear();
    StatusChangeClear(md, 1);
    FollowerUnspawn(sd, *hd);
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    HomSave(*hd, sd->GetGUID().GetCounter(), trans);
    CharacterDatabase.CommitTransaction(trans);
    ClifHomInfo(sd);
    return true;
}

void ROWOWMod::HomDeadOwn(Player* sd, ROWOWHomunData& hd)
{
    // hom_dead: there's no intimacy penalties on death (from Tharis).  It stays dead with its master until Resurrect
    hd.HP = 0;
    ClifHomInfo(sd);
}

void ROWOWMod::HomDelete(Player* sd)
{
    // hom_delete and unit_free: it's gone for good, its row with it, and the next Call Homunculus makes a new one
    ROWOWIdentity* identity = GetIdentity(sd);
    if (identity->Homunculus == nullptr)
        return;
    FollowerUnspawn(sd, *identity->Homunculus);
    CharacterDatabase.Execute("DELETE FROM `mod_rowow_homunculus` WHERE `CharacterGUID` = {}", sd->GetGUID().GetCounter());
    identity->Homunculus.reset();
    ClifHomInfo(sd);
}

int32 ROWOWMod::HomRessurect(Player* sd, uint8 per, int32 /*x*/, int32 /*y*/)
{
    // hom_ressurect: a dead homunculus (a resting one needs calling) back beside its master, with per% of its HP (status_revive) and its timers.  The random cell the skill picks
    // is beside its caster: FollowerSpawn places it beside its master
    ROWOWHomunData* hd = GetHomunculus(sd);
    if (hd == nullptr)
        return 0; // no homunculus
    if (hd->Vaporize == ROWOW_HOM_ST_REST)
        return 0; // vaporized homunculi need to be 'called'
    if (hd->HP > 0)
        return 0; // already alive
    int32 hungerDelay = (BattleConfig.HomunculusStarvingRate > 0 && hd->Hunger <= BattleConfig.HomunculusStarvingRate) ? BattleConfig.HomunculusStarvingDelay : hd->DB->HungryDelay;
    hd->HungryTimer = uint32(std::max(hungerDelay, 1));
    hd->HP = std::max<uint32>(uint32(uint64(hd->BaseStatus.MaxHP) * per / 100), 1);
    FollowerSpawn(sd, *hd);
    ClifHomInfo(sd);
    return 1;
}

void ROWOWMod::HomSkillUp(Player* sd, ROWOWHomunData& hd, uint16 skillID)
{
    // hom_skillup: a point on a skill it knows, up to the class's maximum for it, and a level high enough
    if (hd.Vaporize != ROWOW_HOM_ST_ACTIVE)
        return;
    auto skillIter = hd.Skills.find(skillID);
    if (hd.SkillPoints <= 0 || skillIter == hd.Skills.end())
        return;
    for (ROWOWHomSkillTreeEntry const& skit : hd.DB->SkillTree)
    {
        if (skit.SkillID != skillID)
            continue;
        if (hd.Level >= skit.NeedLevel && skillIter->second < skit.Max)
        {
            skillIter->second++;
            hd.SkillPoints--;
            StatusCalcHomunculus(hd, false);
            FollowerRefreshStatus(sd, hd);
            ClifHomInfo(sd);
        }
        break;
    }
}

bool ROWOWMod::HomLevelUp(Player* sd, ROWOWHomunData& hd)
{
    // hom_levelup: a level, a skill point each third one, and rnd_value of the class's growth, the decimals of the stats' discarded as Aegis does
    if (hd.Level >= BattleConfig.HomMaxLevel || hd.ExpNext == 0 || hd.Exp < hd.ExpNext)
        return false;
    hd.Level++;
    if ((hd.Level % 3) == 0)
        hd.SkillPoints++; // 1 skillpoint each 3 base level
    hd.Exp -= hd.ExpNext;
    hd.ExpNext = HomGetNextExp(hd.Level);
    ROWOWHomStats const& min = hd.DB->GrowthMin;
    ROWOWHomStats const& max = hd.DB->GrowthMax;
    int32 growthHP = irand(min.HP, max.HP);
    int32 growthSP = irand(min.SP, max.SP);
    int32 growth[ROWOW_STAT_COUNT];
    for (int32 i = 0; i < ROWOW_STAT_COUNT; i++)
    {
        growth[i] = irand(min.Stats[i], max.Stats[i]);
        growth[i] -= growth[i] % 10; // Aegis discards the decimals in the stat growth values!
        hd.Stats[i] += growth[i];
    }
    hd.MaxHP += uint32(growthHP);
    hd.MaxSP += uint32(growthSP);
    StatusCalcHomunculus(hd, false);
    FollowerRefreshStatus(sd, hd);
    ClifHomInfo(sd);
    if (BattleConfig.HomunculusShowGrowth != 0)
        SendHomMessage(sd, fmt::format("Growth: hp:{} sp:{} str({:.2f}) agi({:.2f}) vit({:.2f}) int({:.2f}) dex({:.2f}) luk({:.2f}) ", growthHP, growthSP, growth[ROWOW_STAT_STR] / 10.0, growth[ROWOW_STAT_AGI] / 10.0,
            growth[ROWOW_STAT_VIT] / 10.0, growth[ROWOW_STAT_INT] / 10.0, growth[ROWOW_STAT_DEX] / 10.0, growth[ROWOW_STAT_LUK] / 10.0));
    return true;
}

void ROWOWMod::HomGainExp(Player* sd, ROWOWHomunData& hd, uint64 exp)
{
    // hom_gainexp: a level up heals it fully.  At its top level it keeps no exp
    if (hd.Vaporize != ROWOW_HOM_ST_ACTIVE)
        return;
    if (hd.ExpNext == 0 || hd.Level >= BattleConfig.HomMaxLevel)
    {
        hd.Exp = 0;
        return;
    }
    hd.Exp += exp;
    if (hd.Exp < hd.ExpNext)
    {
        ClifHomInfo(sd);
        return;
    }

    // Do the levelup(s)
    while (hd.Exp > hd.ExpNext)
        if (HomLevelUp(sd, hd) == false)
            break;
    if (hd.ExpNext == 0)
        hd.Exp = 0;
    if (Creature* md = GetFollowerCreature(sd, hd))
    {
        PlayScriptLook(md, ROWOW_SCRIPT_LOOK_EFFECT, "HO_UP");
        StatusCalcHomunculus(hd, false);
        FollowerRefreshStatus(sd, hd);
        md->SetHealth(md->GetMaxHealth());
        md->SetPower(POWER_MANA, md->GetMaxPower(POWER_MANA));
    }
    ClifHomInfo(sd);
}

bool ROWOWMod::HomEvolution(Player* sd, ROWOWHomunData& hd)
{
    // hom_evolution: the class's evolved form, rnd_value of what evolving adds on the stats it has, and intimacy back to homunculus_evo_intimacy_reset
    Creature* md = GetFollowerCreature(sd, hd);
    if (hd.DB->EvoClass == 0 || hd.ClassID == hd.DB->EvoClass || md == nullptr)
    {
        if (md != nullptr)
            ClifEmotion(md, ROWOW_EMOTION_SCRATCH);
        return false;
    }
    uint32 evoClass = hd.DB->EvoClass;
    ROWOWHomunculusDB const* evoDB = GetHomunculusDB(evoClass);
    auto entryIter = FollowerEntriesByClassID.find(evoClass);
    if (evoDB == nullptr || entryIter == FollowerEntriesByClassID.end())
        return false;
    hd.DB = evoDB;
    hd.ClassID = evoClass;
    ROWOWHomStats const& min = hd.DB->EvolutionMin;
    ROWOWHomStats const& max = hd.DB->EvolutionMax;
    hd.MaxHP += uint32(irand(min.HP, max.HP));
    hd.MaxSP += uint32(irand(min.SP, max.SP));
    for (int32 i = 0; i < ROWOW_STAT_COUNT; i++)
        hd.Stats[i] += 10 * irand(min.Stats[i], max.Stats[i]);
    hd.Intimacy = uint32(BattleConfig.HomunculusEvoIntimacyReset);

    // clif_class_change: a new look for the same homunculus.  status_calc flag&1 reloads its HP and SP
    FollowerSaveVitals(sd, hd);
    FollowerUnspawn(sd, hd);
    StatusCalcHomunculus(hd, true);
    FollowerSpawn(sd, hd);
    ClifHomInfo(sd);
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    HomSave(hd, sd->GetGUID().GetCounter(), trans);
    CharacterDatabase.CommitTransaction(trans);
    return true;
}

bool ROWOWMod::HomShuffle(Player* sd, ROWOWHomunData& hd)
{
    // hom_shuffle: its stats rolled again from level 1 to the level it has.  Skills, points and exp stay, and it's healed
    int32 level = hd.Level;
    uint64 exp = hd.Exp;
    std::unordered_map<uint16, uint16> skills = hd.Skills;
    int32 skillPoints = hd.SkillPoints;
    ROWOWHomStats const& base = hd.DB->Base;
    hd.Level = 1;
    hd.MaxHP = uint32(base.HP);
    hd.MaxSP = uint32(base.SP);
    for (int32 i = 0; i < ROWOW_STAT_COUNT; i++)
        hd.Stats[i] = base.Stats[i] * 10;
    hd.Exp = 0;
    hd.ExpNext = HomGetNextExp(1);
    hd.Skills.clear();
    hd.SkillPoints = 0;
    for (int32 i = 1; i < level && hd.ExpNext != 0; i++)
    {
        hd.Exp += hd.ExpNext;
        if (HomLevelUp(sd, hd) == false)
            break;
    }
    if (hd.ClassID == hd.DB->EvoClass)
    {
        ROWOWHomStats const& min = hd.DB->EvolutionMin;
        ROWOWHomStats const& max = hd.DB->EvolutionMax;
        hd.MaxHP += uint32(irand(min.HP, max.HP));
        hd.MaxSP += uint32(irand(min.SP, max.SP));
        for (int32 i = 0; i < ROWOW_STAT_COUNT; i++)
            hd.Stats[i] += 10 * irand(min.Stats[i], max.Stats[i]);
    }
    hd.Exp = exp;
    hd.Skills = skills;
    hd.SkillPoints = skillPoints;
    StatusCalcHomunculus(hd, false);
    FollowerRefreshStatus(sd, hd);
    if (Creature* md = GetFollowerCreature(sd, hd))
    {
        md->SetHealth(md->GetMaxHealth());
        md->SetPower(POWER_MANA, md->GetMaxPower(POWER_MANA));
        PlayScriptLook(md, ROWOW_SCRIPT_LOOK_EFFECT, "HO_UP");
    }
    ClifHomInfo(sd);
    return true;
}

void ROWOWMod::HomHungry(Player* sd, ROWOWHomunData& hd, uint32 diff)
{
    // hom_hungry: its hungry timer, which only runs while it's out in the RO world.  Starving costs it its intimacy, down to losing it
    if (hd.HungryTimer > diff)
    {
        hd.HungryTimer -= diff;
        return;
    }
    Creature* md = GetFollowerCreature(sd, hd);
    hd.Hunger--;
    if (md != nullptr)
    {
        if (hd.Hunger <= 10)
            ClifEmotion(md, ROWOW_EMOTION_FRET);
        else if (hd.Hunger == 25)
            ClifEmotion(md, ROWOW_EMOTION_SCRATCH);
        else if (hd.Hunger == 75)
            ClifEmotion(md, ROWOW_EMOTION_OK);
    }
    if (BattleConfig.FeatureHomunculusAutofeed != 0 && hd.IsAutoFeed == true && hd.Hunger <= BattleConfig.FeatureHomunculusAutofeedRate)
        HomFood(sd, hd);
    if (GetHomunculus(sd) != &hd)
        return; // Overfed and gone
    if (hd.Hunger < 0)
    {
        hd.Hunger = 0;

        // Delete the homunculus if intimacy <= 100
        if (HomDecreaseIntimacy(hd, 100) == 0)
        {
            SendHomMessage(sd, fmt::format("{} has run away.", hd.Name));
            HomDelete(sd);
            return;
        }
    }
    ClifHomInfo(sd);
    int32 hungerDelay = (BattleConfig.HomunculusStarvingRate > 0 && hd.Hunger <= BattleConfig.HomunculusStarvingRate) ? BattleConfig.HomunculusStarvingDelay : hd.DB->HungryDelay; // Every 20 seconds if hunger <= 10
    hd.HungryTimer = uint32(std::max(hungerDelay, 1));
}

bool ROWOWMod::HomFood(Player* sd, ROWOWHomunData& hd)
{
    // hom_food: its food from the bags, what it does to its intimacy going by its hunger.  False when it fed.  (rAthena's return reads the other way)
    if (hd.Vaporize != ROWOW_HOM_ST_ACTIVE)
        return true;
    Item* food = PCSearchInventory(sd, hd.DB->FoodID);
    if (food == nullptr)
    {
        ItemTemplate const* foodTemplate = nullptr;
        auto foodTemplateIDIter = ItemTemplateIDsByItemID.find(hd.DB->FoodID);
        if (foodTemplateIDIter != ItemTemplateIDsByItemID.end())
            foodTemplate = sObjectMgr->GetItemTemplate(foodTemplateIDIter->second);
        SendHomMessage(sd, fmt::format("Feeding failed: you have no {}.", foodTemplate != nullptr ? foodTemplate->Name1 : "food"));
        return true;
    }
    uint32 count = 1;
    sd->DestroyItemCount(food, count, true);
    int32 emotion = ROWOW_EMOTION_DELIGHT;
    if (hd.Hunger >= 91)
    {
        HomDecreaseIntimacy(hd, 50);
        emotion = ROWOW_EMOTION_KEK;
    }
    else if (hd.Hunger >= 76)
    {
        HomDecreaseIntimacy(hd, 5);
        emotion = ROWOW_EMOTION_PROFUSELY_SWEAT;
    }
    else if (hd.Hunger >= 26)
        HomIncreaseIntimacy(hd, 75);
    else if (hd.Hunger >= 11)
        HomIncreaseIntimacy(hd, 100);
    else
        HomIncreaseIntimacy(hd, 50);
    hd.Hunger = std::min(hd.Hunger + 10, 100); // dunno increase value for each food
    HomCalcSkillTree(hd); // clif_send_homdata(SP_INTIMATE): an evolved homunculus's last skill comes with Loyal
    if (Creature* md = GetFollowerCreature(sd, hd))
        ClifEmotion(md, emotion);
    ClifHomInfo(sd);
    SendHomMessage(sd, "Fed.");

    // Too much food :/
    if (hd.Intimacy == 0)
    {
        SendHomMessage(sd, fmt::format("{} has run away.", hd.Name));
        HomDelete(sd);
    }
    return false;
}

void ROWOWMod::HomMenu(Player* sd, int32 type)
{
    // hom_menu: 0 its information, 1 feed, 2 delete
    ROWOWHomunData* hd = GetHomunculus(sd);
    if (HomIsActive(hd) == false)
    {
        ClifHomInfo(sd);
        return;
    }
    switch (type)
    {
        case 0: ClifHomInfo(sd); break;
        case 1: HomFood(sd, *hd); break;
        case 2: HomDelete(sd); break;
        default: break;
    }
}

void ROWOWMod::HomChangeName(Player* sd, std::string const& name)
{
    // hom_change_name and hom_change_name_ack: once, unless hom_rename allows more, and no control characters
    ROWOWHomunData* hd = GetHomunculus(sd);
    if (HomIsActive(hd) == false || (hd->IsRenamed == true && BattleConfig.HomRename == 0))
        return;
    bool isBadName = name.empty() == true || name.size() > HOMUNCULUS_SP_NAME_LENGTH;
    for (unsigned char character : name)
        if ((character & 0xe0) == 0 || character == 0x7f)
            isBadName = true;
    if (isBadName == true)
    {
        SendHomMessage(sd, "You cannot use this name.");
        return;
    }
    hd->Name = name;
    hd->IsRenamed = true;
    if (Creature* md = GetFollowerCreature(sd, *hd))
    {
        md->SetName(name);
        md->SetUInt32Value(UNIT_FIELD_PETNUMBER, 0x80000000 | (md->GetGUID().GetCounter() & 0x7FFFFFFF));
        md->SetUInt32Value(UNIT_FIELD_PET_NAME_TIMESTAMP, uint32(GameTime::GetGameTime().count()));
    }
    ClifHomInfo(sd);
}

bool ROWOWMod::HomGMSetClass(Player* sd, uint32 classID)
{
    // For tests, @makehomun: replaces its homunculus with a new one of the class, or with the evolved form's base class
    ROWOWHomunculusDB const* db = GetHomunculusDB(classID);
    if (db == nullptr || db->BaseClass != classID)
        return false;
    HomDelete(sd);
    ROWOWIdentity* identity = GetIdentity(sd);
    identity->Homunculus = std::make_unique<ROWOWHomunData>();
    ROWOWHomunData& hd = *identity->Homunculus;
    HomCreate(sd, hd, *db);
    return true;
}

void ROWOWMod::HomGMRefresh(Player* sd, ROWOWHomunData& hd)
{
    // For tests: its status again from what was set on it, with full HP and SP
    FollowerUnspawn(sd, hd);
    hd.ExpNext = HomGetNextExp(hd.Level);
    StatusCalcHomunculus(hd, true);
    hd.HP = hd.BaseStatus.MaxHP;
    hd.SP = hd.BaseStatus.MaxSP;
    FollowerSpawn(sd, hd);
    ClifHomInfo(sd);
}

void ROWOWMod::HomCreate(Player* sd, ROWOWHomunData& hd, ROWOWHomunculusDB const& db)
{
    // hom_create_request and hom_recv_data: a new homunculus at level 1, at full HP and SP (status_percent_heal), out at once
    hd.FollowerType = ROWOW_BL_HOM;
    hd.ClassID = db.BaseClass;
    hd.DB = &db;
    hd.Name = db.Name;
    hd.Level = 1;
    hd.Hunger = 32; // 32%
    hd.Intimacy = 2100; // 21/1000
    hd.MaxHP = uint32(db.Base.HP);
    hd.MaxSP = uint32(db.Base.SP);
    for (int32 i = 0; i < ROWOW_STAT_COUNT; i++)
        hd.Stats[i] = db.Base.Stats[i] * 10;
    hd.ExpNext = HomGetNextExp(1);
    hd.HungryTimer = uint32(std::max(db.HungryDelay, 1));
    StatusCalcHomunculus(hd, true);
    hd.HP = hd.BaseStatus.MaxHP;
    hd.SP = hd.BaseStatus.MaxSP;
    FollowerSpawn(sd, hd);
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    HomSave(hd, sd->GetGUID().GetCounter(), trans);
    CharacterDatabase.CommitTransaction(trans);
    ClifHomInfo(sd);
}

void ROWOWMod::HomSetMaxLevels(Player* sd, int32 level)
{
    // For tests: levels it up as exp would
    ROWOWHomunData* hd = GetHomunculus(sd);
    if (hd == nullptr)
        return;
    while (hd->Level < level && hd->ExpNext != 0)
    {
        hd->Exp = hd->ExpNext;
        if (HomLevelUp(sd, *hd) == false)
            break;
    }
    hd->Exp = 0;
}

void ROWOWMod::ClifHomInfo(Player* sd)
{
    // clif_hominfo, clif_homskillinfoblock and clif_send_homdata for the RO_Identity addon's homunculus window: "ROWOWFOLLOWER\tH\t<fields>\t<skills>" or "ROWOWFOLLOWER\tH" when there's none.
    // A skill is <id>:<level>:<max>:<upgradable>
    ROWOWHomunData* hd = GetHomunculus(sd);
    if (hd == nullptr)
    {
        SendAddonMessage(sd, "ROWOWFOLLOWER\tH");
        return;
    }
    Creature* md = GetFollowerCreature(sd, *hd);
    ROWOWStatus const* status = md != nullptr ? GetROStatus(md) : &hd->BaseStatus;
    if (status == nullptr)
        status = &hd->BaseStatus;
    std::string skills;
    for (ROWOWHomSkillTreeEntry const& skit : hd->DB->SkillTree)
    {
        auto skillIter = hd->Skills.find(skit.SkillID);
        if (skillIter == hd->Skills.end())
            continue;
        bool isUpgradable = hd->Level >= skit.NeedLevel && skillIter->second < skit.Max;
        skills += fmt::format("{}{}:{}:{}:{}", skills.empty() == true ? "" : ",", skit.SkillID, skillIter->second, skit.Max, isUpgradable == true ? 1 : 0);
    }
    uint32 hp = md != nullptr && md->IsAlive() == true ? md->GetHealth() : hd->HP;
    uint32 sp = md != nullptr && md->IsAlive() == true ? md->GetPower(POWER_MANA) : hd->SP;
    int32 crit = (BattleConfig.HomSetting & ROWOW_HOMSET_DISPLAY_LUK) != 0 ? status->Stats[ROWOW_STAT_LUK] / 3 + 1 : status->Critical / 10;
    SendAddonMessage(sd, fmt::format("ROWOWFOLLOWER\tH\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}", hd->Name, hd->ClassID, hd->Level, hd->Exp,
        (hd->Level >= BattleConfig.HomMaxLevel ? 0 : hd->ExpNext), hp, status->MaxHP, sp, status->MaxSP, hd->Hunger, hd->Intimacy / 100, status->WeaponRefineAttack + status->BaseAttack, status->MagicAttackMax,
        status->Hit, crit, status->Def + status->Stats[ROWOW_STAT_VIT], status->MDef, status->Flee, status->AttackMotion, hd->SkillPoints, (hd->IsRenamed == true && BattleConfig.HomRename == 0) ? 1 : 0, hd->Vaporize,
        (hd->HP > 0) ? 1 : 0, hd->IsAutoFeed == true ? 1 : 0, (md != nullptr ? 1 : 0), hd->ClassID == hd->DB->EvoClass ? 1 : 0, skills));
}
