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
//  What homunculi and mercenaries share is a port of rAthena's unit.cpp, skill.cpp and status.cpp parts for them, and of the client's data\ai\ai.lua:
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
//  For more information, see LICENCE in the main folder

#include "Chat.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Item.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedGossip.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "TemporarySummon.h"
#include "Tokenize.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include "ROWOW.h"
#include "ROWOW_Script.h"
#include "ROWOW_Skill.h"

#include <cmath>

using namespace std;

// RO's homunculi and mercenaries (R11).  The server has no AI for them: each client runs one (data\ai\ai.lua) that sends walks and attacks.  Here the master's update runs that
// state machine, with the monsters' unit functions, as the taming pets do.  A follower (ROWOWFollower) is on its master's identity, so only the master's thread touches it.  Its
// creature is on the master's map, a TempSummon no RO monster code runs for: the master's update walks it and swings with it, ROWOWMobData holds its walk and ROWOWMobStatus its battle status

namespace
{
    constexpr uint64 MIN_FOLLOWERTHINKTIME = 100;
    constexpr uint64 FOLLOWER_SEARCH_INTERVAL_IN_MS = 200; // How often it looks around.  (GetActors runs for each of the client's AI calls)
    constexpr uint64 FOLLOWER_TELEPORT_DELAY_IN_MS = 3000; // unit_check_start_teleport_timer: too far from the master this long, it's put beside it
    constexpr uint64 FOLLOWER_INFO_INTERVAL_IN_MS = 1000; // At most this often its window hears of its HP and SP
    constexpr int32 FOLLOWER_SIGHT_RANGE = 20; // IsOutOfSight
    constexpr int32 FOLLOWER_OWNER_RANGE = 3; // ai.lua's distance from the owner it's content with
    constexpr int32 FOLLOWER_MOVE_SPLIT_RANGE = 15; // OnMOVE_CMD: a further walk is cut in two

    // The WoW checks RO's rules replace, for what a follower is told to cast, as a monster's (MOB_SKILL_CAST_FLAGS)
    TriggerCastFlags constexpr FOLLOWER_SKILL_CAST_FLAGS = TriggerCastFlags(TRIGGERED_IGNORE_GCD | TRIGGERED_IGNORE_SPELL_AND_CATEGORY_CD | TRIGGERED_IGNORE_POWER_AND_REAGENT_COST |
        TRIGGERED_IGNORE_SHAPESHIFT | TRIGGERED_IGNORE_CASTER_AURASTATE | TRIGGERED_IGNORE_CASTER_MOUNTED_OR_ON_VEHICLE | TRIGGERED_IGNORE_CASTER_AURAS | TRIGGERED_DONT_REPORT_CAST_ERROR);

    char const* const HomunculusClassNames[] = { "Lif", "Amistr", "Filir", "Vanilmirth", "Lif2", "Amistr2", "Filir2", "Vanilmirth2", "Lif_H", "Amistr_H", "Filir_H", "Vanilmirth_H", "Lif_H2",
        "Amistr_H2", "Filir_H2", "Vanilmirth_H2" };
    char const* const FollowerRaceNames[] = { "Formless", "Undead", "Brute", "Plant", "Insect", "Fish", "Demon", "Demihuman", "Angel", "Dragon" };
    char const* const FollowerSizeNames[] = { "Small", "Medium", "Large" };

    uint32 GetHomunculusClassID(std::string const& name)
    {
        for (uint32 i = 0; i < std::size(HomunculusClassNames); i++)
            if (name == HomunculusClassNames[i])
                return ROWOW_HM_CLASS_BASE + i;
        return 0;
    }

    template <size_t Count>
    int32 GetFollowerNameIndex(char const* const (&names)[Count], std::string const& name, int32 defaultValue)
    {
        for (size_t i = 0; i < Count; i++)
            if (name == names[i])
                return int32(i);
        return defaultValue;
    }

    // ai.lua's GetDistance
    int32 GetAIDistance(int32 x1, int32 y1, int32 x2, int32 y2)
    {
        return int32(std::floor(std::sqrt(double((x1 - x2) * (x1 - x2) + (y1 - y2) * (y1 - y2)))));
    }

    void SendFollowerMessage(Player* player, std::string const& message)
    {
        ChatHandler(player->GetSession()).SendSysMessage(message.c_str());
    }
}

void ROWOWMod::LoadFollowerData()
{
    // homunculus_db, exp_homun, mercenary_db and the creature of each class, after the items and skills they name
    HomunculiByClass.clear();
    HomunculusExpByLevel.clear();
    MercenariesByClass.clear();
    FollowerEntriesByClassID.clear();
    FollowerTypesByEntry.clear();
    if (IsEnabled == false)
        return;
    auto getItemID = [this](std::string name)
    {
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char character) { return char(std::tolower(character)); });
        auto itemIDIter = ItemIDsByAegisName.find(name);
        return itemIDIter != ItemIDsByAegisName.end() ? itemIDIter->second : 0;
    };

    if (ROWOWDataTable const* table = GetDataTable("mod_rowow_db_homunculus"))
    {
        std::map<uint32, ROWOWHomunculusDB> rowsByClass;
        for (size_t rowIndex = 0; rowIndex < table->Rows.size(); rowIndex++)
        {
            uint32 classID = GetHomunculusClassID(table->GetCell(rowIndex, table->GetColumnIndex("Class")));
            if (classID == 0)
                continue;
            ROWOWHomunculusDB& db = rowsByClass[classID];
            if (db.BaseClass == 0)
            {
                db.BaseClass = classID;
                db.EvoClass = GetHomunculusClassID(table->GetCell(rowIndex, table->GetColumnIndex("EvolutionClass")));
                db.Name = table->GetCell(rowIndex, table->GetColumnIndex("Name"));
                std::string food = table->GetCell(rowIndex, table->GetColumnIndex("Food"));
                db.FoodID = getItemID(food.empty() == true ? "Pet_Food" : food);
                db.HungryDelay = int32(table->GetInt(rowIndex, table->GetColumnIndex("HungryDelay"), 60000));
                db.Race = uint8(GetFollowerNameIndex(FollowerRaceNames, table->GetCell(rowIndex, table->GetColumnIndex("Race")), ROWOW_RACE_DEMIHUMAN));
                int32 element = GetElementByName(table->GetCell(rowIndex, table->GetColumnIndex("Element")));
                db.Element = uint8(element >= 0 ? element : ROWOW_ELEMENT_NEUTRAL);
                // HomunculusDatabase::parseBodyNode writes both of the yml's sizes into base_size, EvolutionSize last, and evo_size stays 0: an unevolved homunculus is Medium with range 2
                // and an evolved one Small with range 1, whatever the yml says.  The oracle shows it, so it's kept
                db.BaseSize = uint8(GetFollowerNameIndex(FollowerSizeNames, table->GetCell(rowIndex, table->GetColumnIndex("EvolutionSize")), ROWOW_SIZE_MEDIUM));
                db.EvoSize = ROWOW_SIZE_SMALL;
                db.BaseASPD = int32(table->GetInt(rowIndex, table->GetColumnIndex("AttackDelay"), 700));
                for (int32 i = 0; i < ROWOW_STAT_COUNT; i++)
                    db.Base.Stats[i] = 1;
                db.Base.HP = 1;
                db.Base.SP = 1;
                for (auto const& statusFields : ROWOWDataTable::ParseMapList(table->GetCell(rowIndex, table->GetColumnIndex("Status"))))
                {
                    std::string type;
                    int32 base = 1, growthMin = 0, growthMax = 0, evolutionMin = 0, evolutionMax = 0;
                    for (auto const& field : statusFields)
                    {
                        int32 value = field.first == "Type" ? 0 : atoi(field.second.c_str());
                        if (field.first == "Type")
                            type = field.second;
                        else if (field.first == "Base")
                            base = value;
                        else if (field.first == "GrowthMinimum")
                            growthMin = value;
                        else if (field.first == "GrowthMaximum")
                            growthMax = value;
                        else if (field.first == "EvolutionMinimum")
                            evolutionMin = value;
                        else if (field.first == "EvolutionMaximum")
                            evolutionMax = value;
                    }
                    auto setStat = [&](ROWOWHomStats& stats, int32 value)
                    {
                        if (type == "Hp")
                            stats.HP = value;
                        else if (type == "Sp")
                            stats.SP = value;
                        else if (type == "Str")
                            stats.Stats[ROWOW_STAT_STR] = value;
                        else if (type == "Agi")
                            stats.Stats[ROWOW_STAT_AGI] = value;
                        else if (type == "Vit")
                            stats.Stats[ROWOW_STAT_VIT] = value;
                        else if (type == "Int")
                            stats.Stats[ROWOW_STAT_INT] = value;
                        else if (type == "Dex")
                            stats.Stats[ROWOW_STAT_DEX] = value;
                        else if (type == "Luk")
                            stats.Stats[ROWOW_STAT_LUK] = value;
                    };
                    setStat(db.Base, base);
                    setStat(db.GrowthMin, growthMin);
                    setStat(db.GrowthMax, growthMax);
                    setStat(db.EvolutionMin, evolutionMin);
                    setStat(db.EvolutionMax, evolutionMax);
                }
            }
            ROWOWHomSkillTreeEntry entry;
            entry.SkillID = GetSkillIDByName(table->GetCell(rowIndex, table->GetColumnIndex("SkillTree_Skill")));
            entry.Max = uint16(table->GetInt(rowIndex, table->GetColumnIndex("SkillTree_MaxLevel"), 1));
            entry.NeedLevel = uint16(table->GetInt(rowIndex, table->GetColumnIndex("SkillTree_RequiredLevel"), 0));
            entry.Intimacy = uint32(table->GetInt(rowIndex, table->GetColumnIndex("SkillTree_RequiredIntimacy"), 0)) * 100;
            entry.IsEvolution = table->GetCell(rowIndex, table->GetColumnIndex("SkillTree_RequireEvolution")) == "true";
            for (auto const& needFields : ROWOWDataTable::ParseMapList(table->GetCell(rowIndex, table->GetColumnIndex("SkillTree_Required"))))
            {
                uint16 needID = 0, needLevel = 0;
                for (auto const& field : needFields)
                {
                    if (field.first == "Skill")
                        needID = GetSkillIDByName(field.second);
                    else if (field.first == "Level")
                        needLevel = uint16(atoi(field.second.c_str()));
                }
                entry.Need.emplace_back(needID, needLevel);
            }
            if (entry.SkillID != 0)
                db.SkillTree.push_back(entry);
        }
        for (auto const& rowByClass : rowsByClass)
        {
            HomunculiByClass[rowByClass.second.BaseClass] = rowByClass.second;
            HomunculiByClass[rowByClass.second.EvoClass] = rowByClass.second;
        }
    }
    if (ROWOWDataTable const* table = GetDataTable("mod_rowow_db_homunculus_exp"))
    {
        for (size_t rowIndex = 0; rowIndex < table->Rows.size(); rowIndex++)
        {
            size_t level = size_t(table->GetInt(rowIndex, table->GetColumnIndex("Level"), 0));
            if (HomunculusExpByLevel.size() <= level)
                HomunculusExpByLevel.resize(level + 1, 0);
            HomunculusExpByLevel[level] = uint64(table->GetInt(rowIndex, table->GetColumnIndex("Exp"), 0));
        }
    }
    if (ROWOWDataTable const* table = GetDataTable("mod_rowow_db_mercenary"))
    {
        for (size_t rowIndex = 0; rowIndex < table->Rows.size(); rowIndex++)
        {
            uint32 classID = uint32(table->GetInt(rowIndex, table->GetColumnIndex("Id"), 0));
            ROWOWMercenaryDB& db = MercenariesByClass[classID];
            if (db.ClassID == 0)
            {
                // MercenaryDatabase::parseBodyNode
                db.ClassID = classID;
                db.Name = table->GetCell(rowIndex, table->GetColumnIndex("Name"));
                db.Level = uint32(table->GetInt(rowIndex, table->GetColumnIndex("Level"), 1));
                ROWOWStatus& status = db.Status;
                status.IsPlayer = false;
                status.FollowerType = ROWOW_BL_MER;
                status.Level = db.Level;
                status.MaxHP = uint32(table->GetInt(rowIndex, table->GetColumnIndex("Hp"), 1));
                status.MaxSP = uint32(table->GetInt(rowIndex, table->GetColumnIndex("Sp"), 1));
                status.WeaponAttack = int32(table->GetInt(rowIndex, table->GetColumnIndex("Attack"), 0));
                status.WeaponRefineAttack = status.WeaponAttack + int32(table->GetInt(rowIndex, table->GetColumnIndex("Attack2"), 0));
                status.Def = int32(table->GetInt(rowIndex, table->GetColumnIndex("Defense"), 0));
                status.MDef = int32(table->GetInt(rowIndex, table->GetColumnIndex("MagicDefense"), 0));
                status.Stats[ROWOW_STAT_STR] = int32(table->GetInt(rowIndex, table->GetColumnIndex("Str"), 1));
                status.Stats[ROWOW_STAT_AGI] = int32(table->GetInt(rowIndex, table->GetColumnIndex("Agi"), 1));
                status.Stats[ROWOW_STAT_VIT] = int32(table->GetInt(rowIndex, table->GetColumnIndex("Vit"), 1));
                status.Stats[ROWOW_STAT_INT] = int32(table->GetInt(rowIndex, table->GetColumnIndex("Int"), 1));
                status.Stats[ROWOW_STAT_DEX] = int32(table->GetInt(rowIndex, table->GetColumnIndex("Dex"), 1));
                status.Stats[ROWOW_STAT_LUK] = int32(table->GetInt(rowIndex, table->GetColumnIndex("Luk"), 1));
                status.Range = int32(table->GetInt(rowIndex, table->GetColumnIndex("AttackRange"), 0));
                status.Range2 = int32(table->GetInt(rowIndex, table->GetColumnIndex("SkillRange"), 0));
                status.Range3 = int32(table->GetInt(rowIndex, table->GetColumnIndex("ChaseRange"), 0));
                status.Size = uint8(GetFollowerNameIndex(FollowerSizeNames, table->GetCell(rowIndex, table->GetColumnIndex("Size")), ROWOW_SIZE_SMALL));
                status.Race = uint8(GetFollowerNameIndex(FollowerRaceNames, table->GetCell(rowIndex, table->GetColumnIndex("Race")), ROWOW_RACE_FORMLESS));
                int32 element = GetElementByName(table->GetCell(rowIndex, table->GetColumnIndex("Element")));
                status.DefElement = uint8(element >= 0 ? element : ROWOW_ELEMENT_NEUTRAL);
                status.ElementLevel = uint8(table->GetInt(rowIndex, table->GetColumnIndex("ElementLevel"), 1));
                status.Speed = int32(table->GetInt(rowIndex, table->GetColumnIndex("WalkSpeed"), ROWOW_DEFAULT_WALK_SPEED));
                status.AttackDelay = std::clamp(int32(table->GetInt(rowIndex, table->GetColumnIndex("AttackDelay"), 4000)), ROWOW_MAX_ASPD_NOPC, ROWOW_MIN_ASPD);
                status.AttackMotion = std::clamp(int32(table->GetInt(rowIndex, table->GetColumnIndex("AttackMotion"), 2000)), 1, ROWOW_MIN_ASPD);
                status.DamageMotion = int32(table->GetInt(rowIndex, table->GetColumnIndex("DamageMotion"), 0));
                status.AspdRate = 1000;
            }
            uint16 skillID = GetSkillIDByName(table->GetCell(rowIndex, table->GetColumnIndex("Skills_Name")));
            if (skillID != 0)
                db.Skills.emplace_back(skillID, uint16(table->GetInt(rowIndex, table->GetColumnIndex("Skills_MaxLevel"), 1)));
        }
    }
    if (QueryResult result = WorldDatabase.Query("SELECT 1 FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = 'mod_rowow_follower_look'"))
    {
        if (QueryResult looks = WorldDatabase.Query("SELECT `ClassID`, `CreatureTemplateID` FROM `mod_rowow_follower_look`"))
        {
            do
            {
                Field* fields = looks->Fetch();
                uint32 classID = fields[0].Get<uint32>();
                uint32 entry = fields[1].Get<uint32>();
                FollowerEntriesByClassID[classID] = entry;
                FollowerTypesByEntry[entry] = HomunculiByClass.find(classID) != HomunculiByClass.end() ? ROWOW_BL_HOM : ROWOW_BL_MER;
            } while (looks->NextRow());
        }
    }
    else
        LOG_ERROR("module.ROWOW", "ROWOWMod::LoadFollowerData found no mod_rowow_follower_look table, so homunculi and mercenaries have no creature (run the converter)");
    LOG_INFO("module.ROWOW", "ROWOWMod::LoadFollowerData loaded {} homunculus classes, {} exp levels, {} mercenaries, {} creatures", HomunculiByClass.size(), HomunculusExpByLevel.size(),
        MercenariesByClass.size(), FollowerEntriesByClassID.size());
}

bool ROWOWMod::IsROFollower(Unit const* unit) const
{
    return GetFollowerType(unit) != 0;
}

int32 ROWOWMod::GetFollowerType(Unit const* unit) const
{
    if (unit == nullptr || unit->IsCreature() == false)
        return 0;
    auto typeIter = FollowerTypesByEntry.find(unit->ToCreature()->GetEntry());
    return typeIter != FollowerTypesByEntry.end() ? typeIter->second : 0;
}

Player* ROWOWMod::GetFollowerMaster(Unit const* unit) const
{
    ROWOWFollowerOwner const* owner = unit != nullptr && unit->IsCreature() == true ? unit->CustomData.Get<ROWOWFollowerOwner>("ROWOWFollowerOwner") : nullptr;
    if (owner == nullptr || unit->IsInWorld() == false)
        return nullptr;
    Player* master = ObjectAccessor::GetPlayer(*unit, owner->OwnerGUID);
    return master != nullptr && master->IsInWorld() == true ? master : nullptr;
}

ROWOWFollower* ROWOWMod::GetFollower(Player* sd, int32 followerType) const
{
    ROWOWIdentity* identity = sd->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    if (identity == nullptr)
        return nullptr;
    if (followerType == ROWOW_BL_HOM)
        return identity->Homunculus.get();
    return identity->Mercenary.get();
}

ROWOWFollower* ROWOWMod::GetFollower(Unit const* unit) const
{
    Player* master = GetFollowerMaster(unit);
    int32 followerType = GetFollowerType(unit);
    if (master == nullptr || followerType == 0)
        return nullptr;
    ROWOWFollower* fd = GetFollower(master, followerType);
    return fd != nullptr && fd->CreatureGUID == unit->GetGUID() ? fd : nullptr;
}

Creature* ROWOWMod::GetFollowerCreature(Player* sd, ROWOWFollower const& fd) const
{
    return fd.CreatureGUID.IsEmpty() == true || sd->IsInWorld() == false ? nullptr : ObjectAccessor::GetCreature(*sd, fd.CreatureGUID);
}

int32 ROWOWMod::GetFollowerDamageLogFlag(Unit const* attacker) const
{
    // mob_log_damage: a homunculus's damage is logged apart from its master's, a mercenary's as its master's own
    return GetFollowerType(attacker) == ROWOW_BL_HOM ? ROWOW_MDLF_HOMUN : ROWOW_MDLF_NORMAL;
}

void ROWOWMod::LoadIdentityFollowers(Player* player, ROWOWIdentity& identity)
{
    // sd->status.hom_id and mer_id, intif_homunculus_requestload, intif_mercenary_request and the mercenary_owner row
    ObjectGuid::LowType charGUID = player->GetGUID().GetCounter();
    if (QueryResult result = CharacterDatabase.Query("SELECT `Class`, `PrevClass`, `Name`, `Level`, `Exp`, `Intimacy`, `Hunger`, `Str`, `Agi`, `Vit`, `Int`, `Dex`, `Luk`, `HP`, `MaxHP`, `SP`, `MaxSP`, "
        "`SkillPoints`, `IsRenamed`, `Vaporize`, `IsAutoFeed`, `Skills` FROM `mod_rowow_homunculus` WHERE `CharacterGUID` = {}", charGUID))
    {
        Field* fields = result->Fetch();
        uint32 classID = fields[0].Get<uint32>();
        ROWOWHomunculusDB const* db = GetHomunculusDB(classID);
        if (db != nullptr)
        {
            identity.Homunculus = std::make_unique<ROWOWHomunData>();
            ROWOWHomunData& hd = *identity.Homunculus;
            hd.FollowerType = ROWOW_BL_HOM;
            hd.ClassID = classID;
            hd.DB = db;
            hd.PrevClass = fields[1].Get<uint32>();
            hd.Name = fields[2].Get<std::string>();
            hd.Level = fields[3].Get<int32>();
            hd.Exp = fields[4].Get<uint64>();
            hd.Intimacy = fields[5].Get<uint32>();
            hd.Hunger = fields[6].Get<int32>();
            hd.Stats[ROWOW_STAT_STR] = fields[7].Get<int32>();
            hd.Stats[ROWOW_STAT_AGI] = fields[8].Get<int32>();
            hd.Stats[ROWOW_STAT_VIT] = fields[9].Get<int32>();
            hd.Stats[ROWOW_STAT_INT] = fields[10].Get<int32>();
            hd.Stats[ROWOW_STAT_DEX] = fields[11].Get<int32>();
            hd.Stats[ROWOW_STAT_LUK] = fields[12].Get<int32>();
            hd.HP = fields[13].Get<uint32>();
            hd.MaxHP = fields[14].Get<uint32>();
            hd.SP = fields[15].Get<uint32>();
            hd.MaxSP = fields[16].Get<uint32>();
            hd.SkillPoints = fields[17].Get<int32>();
            hd.IsRenamed = fields[18].Get<uint8>() != 0;
            hd.Vaporize = fields[19].Get<int32>();
            hd.IsAutoFeed = fields[20].Get<uint8>() != 0;
            std::string skills = fields[21].Get<std::string>();
            for (std::string_view skill : Acore::Tokenize(skills, ',', false))
            {
                std::vector<std::string_view> parts = Acore::Tokenize(skill, ':', false);
                if (parts.size() == 2)
                    hd.Skills[uint16(atoi(std::string(parts[0]).c_str()))] = uint16(atoi(std::string(parts[1]).c_str()));
            }
            hd.ExpNext = HomGetNextExp(hd.Level);
            hd.HungryTimer = uint32(std::max(hd.DB->HungryDelay, 1));
            StatusCalcHomunculus(hd, true);
        }
    }
    if (QueryResult result = CharacterDatabase.Query("SELECT `Class`, `HP`, `SP`, `KillCount`, `LifeTime` FROM `mod_rowow_mercenary` WHERE `CharacterGUID` = {}", charGUID))
    {
        Field* fields = result->Fetch();
        auto dbIter = MercenariesByClass.find(fields[0].Get<uint32>());
        if (dbIter != MercenariesByClass.end())
        {
            identity.Mercenary = std::make_unique<ROWOWMercData>();
            ROWOWMercData& md = *identity.Mercenary;
            md.FollowerType = ROWOW_BL_MER;
            md.ClassID = dbIter->first;
            md.DB = &dbIter->second;
            md.HP = fields[1].Get<uint32>();
            md.SP = fields[2].Get<uint32>();
            md.KillCount = fields[3].Get<uint32>();
            md.LifeTime = fields[4].Get<int64>();
            StatusCalcMercenary(player, md, true);
        }
    }
    if (QueryResult result = CharacterDatabase.Query("SELECT `ArchFaith`, `ArchCalls`, `SpearFaith`, `SpearCalls`, `SwordFaith`, `SwordCalls` FROM `mod_rowow_mercenary_owner` WHERE `CharacterGUID` = {}", charGUID))
    {
        Field* fields = result->Fetch();
        identity.MercenaryOwner.ArchFaith = fields[0].Get<int32>();
        identity.MercenaryOwner.ArchCalls = fields[1].Get<int32>();
        identity.MercenaryOwner.SpearFaith = fields[2].Get<int32>();
        identity.MercenaryOwner.SpearCalls = fields[3].Get<int32>();
        identity.MercenaryOwner.SwordFaith = fields[4].Get<int32>();
        identity.MercenaryOwner.SwordCalls = fields[5].Get<int32>();
    }
}

void ROWOWMod::SaveIdentityFollowers(Player* player, ROWOWIdentity& identity, CharacterDatabaseTransaction trans)
{
    // The vitals are the master's update's copy: a save can come with the master out of the world
    ObjectGuid::LowType charGUID = player->GetGUID().GetCounter();
    if (identity.Homunculus != nullptr)
        HomSave(*identity.Homunculus, charGUID, trans);
    if (identity.Mercenary != nullptr)
        MercSave(*identity.Mercenary, charGUID, trans);
    ROWOWMercenaryOwner const& owner = identity.MercenaryOwner;
    trans->Append("REPLACE INTO `mod_rowow_mercenary_owner` (`CharacterGUID`, `ArchFaith`, `ArchCalls`, `SpearFaith`, `SpearCalls`, `SwordFaith`, `SwordCalls`) VALUES ({}, {}, {}, {}, {}, {}, {})",
        charGUID, owner.ArchFaith, owner.ArchCalls, owner.SpearFaith, owner.SpearCalls, owner.SwordFaith, owner.SwordCalls);
}

void ROWOWMod::FollowerSetSpeed(Creature* md, int32 speed)
{
    // status.speed: ms a cell.  WoW walks the creature's spline at its speed rate, so that's set to the same pace, as a pet's
    if (speed <= 0)
        return;
    float yardsPerSecond = CellSize * 1000.f / float(speed);
    md->SetSpeed(MOVE_WALK, yardsPerSecond / baseMoveSpeed[MOVE_WALK], true);
    md->SetSpeed(MOVE_RUN, yardsPerSecond / baseMoveSpeed[MOVE_RUN], true);
}

void ROWOWMod::StatusCalcBLFollower(Creature* md, ROWOWMobStatus* mobStatus)
{
    // The end of status_calc_bl_ for a homunculus or mercenary: what its battle status sets on the creature
    ROWOWStatus const& status = mobStatus->Status;
    md->SetMaxPower(POWER_MANA, status.MaxSP);
    if (md->GetPower(POWER_MANA) > status.MaxSP)
        md->SetPower(POWER_MANA, status.MaxSP);
    FollowerSetSpeed(md, status.Speed);
}

void ROWOWMod::FollowerRefreshStatus(Player* sd, ROWOWFollower& fd)
{
    // status_calc_bl_ after the base status changed: a level up, a skill, an evolution
    Creature* md = GetFollowerCreature(sd, fd);
    if (md == nullptr)
        return;
    md->CustomData.GetDefault<ROWOWMobBaseStatus>("ROWOWMobBaseStatus")->Status = fd.BaseStatus;
    md->SetLevel(uint8(std::clamp<uint32>(fd.BaseStatus.Level, 1, 255)));
    StatusCalcBL(md, std::bitset<ROWOW_SCB_MAX>().set());
    md->SetCreateHealth(md->GetMaxHealth());
    md->SetStatFlatModifier(UNIT_MOD_HEALTH, BASE_VALUE, float(md->GetMaxHealth()));
}

void ROWOWMod::FollowerSpawn(Player* sd, ROWOWFollower& fd)
{
    // map_addblock and clif_spawn: its creature beside the master, 2 cells behind its facing (unit_calc_pos), with its HP and SP
    if (GetFollowerCreature(sd, fd) != nullptr || sd->IsInWorld() == false || IsInROForm(sd) == false)
        return;
    auto entryIter = FollowerEntriesByClassID.find(fd.ClassID);
    if (entryIter == FollowerEntriesByClassID.end())
    {
        LOG_ERROR("module.ROWOW", "ROWOWMod::FollowerSpawn: class {} has no creature (run the converter)", fd.ClassID);
        return;
    }
    float worldX = 0, worldY = 0, worldZ = 0;
    GetCellPositionBesideMaster(sd, worldX, worldY, worldZ);
    TempSummon* md = sd->SummonCreature(entryIter->second, worldX, worldY, worldZ, sd->GetOrientation(), TEMPSUMMON_MANUAL_DESPAWN);
    if (md == nullptr)
        return;
    md->SetCreatorGUID(sd->GetGUID());
    md->SetReactState(REACT_PASSIVE);
    md->SetFaction(sd->GetFaction()); // Its master's friends and enemies, as a pet has
    md->SetUnitFlag(UNIT_FLAG_PLAYER_CONTROLLED);
    md->SetRegeneratingHealth(false); // RO's own regen, FollowerNaturalHeal
    md->SetRegeneratingPower(false);
    md->setPowerType(POWER_MANA);
    md->SetCombatMovement(false);
    ROWOWFollowerOwner* owner = md->CustomData.GetDefault<ROWOWFollowerOwner>("ROWOWFollowerOwner");
    owner->OwnerGUID = sd->GetGUID();
    owner->FollowerType = fd.FollowerType;
    md->CustomData.GetDefault<ROWOWMobBaseStatus>("ROWOWMobBaseStatus")->Status = fd.BaseStatus;
    fd.CreatureGUID = md->GetGUID();
    fd.TeleportTime = 0;
    fd.State = ROWOW_AI_IDLE_ST;
    fd.Enemy.Clear();
    fd.Skill = 0;
    fd.ReservedMoves.clear();
    uint64 tick = GetTick();
    fd.RegenHPTime = tick;
    fd.RegenSPTime = tick;
    fd.NextThinkTime = tick;
    fd.NextSearchTime = tick;
    uint32 hp = fd.HP;
    uint32 sp = fd.SP;
    md->SetMaxHealth(fd.BaseStatus.MaxHP);
    FollowerRefreshStatus(sd, fd);
    md->SetHealth(std::min(hp, md->GetMaxHealth()));
    md->SetPower(POWER_MANA, std::min(sp, md->GetMaxPower(POWER_MANA)));
    if (fd.FollowerType == ROWOW_BL_HOM)
    {
        // A homunculus's own name goes by the pet name query, as a script monster's (the number is the creature's)
        ROWOWHomunData const& hd = static_cast<ROWOWHomunData const&>(fd);
        if (hd.Name != md->GetName())
        {
            md->SetName(hd.Name);
            md->SetUInt32Value(UNIT_FIELD_PETNUMBER, 0x80000000 | (md->GetGUID().GetCounter() & 0x7FFFFFFF));
            md->SetUInt32Value(UNIT_FIELD_PET_NAME_TIMESTAMP, uint32(GameTime::GetGameTime().count()));
        }
    }
}

void ROWOWMod::FollowerSaveVitals(Player* sd, ROWOWFollower& fd)
{
    Creature* md = GetFollowerCreature(sd, fd);
    if (md == nullptr || md->IsAlive() == false)
        return;
    fd.HP = md->GetHealth();
    fd.SP = md->GetPower(POWER_MANA);
}

void ROWOWMod::FollowerUnspawn(Player* sd, ROWOWFollower& fd)
{
    FollowerSaveVitals(sd, fd);
    if (Creature* md = GetFollowerCreature(sd, fd))
    {
        UnitSkillCastCancel(md, 0);
        SkillClearUnitGroup(md);
        SkillClearTimerSkill(md);
        md->CustomData.Erase("ROWOWFollowerOwner");
        md->DespawnOrUnsummon();
    }
    fd.CreatureGUID.Clear();
}

void ROWOWMod::FollowerArrive(Player* sd)
{
    // clif_parse_LoadEndAck: it arrives with its master, at its master's speed (status_calc_bl(SCB_SPEED)), and its timers run again
    ROWOWIdentity* identity = GetIdentity(sd);
    if (ROWOWHomunData* hd = identity->Homunculus.get())
    {
        if (HomIsActive(hd) == true)
            FollowerSpawn(sd, *hd);
        ClifHomInfo(sd);
    }
    if (identity->Mercenary != nullptr)
    {
        FollowerSpawn(sd, *identity->Mercenary);
        ClifMercenaryInfo(sd);
    }
}

void ROWOWMod::FollowerLeaveMap(Player* sd, bool isChangeMap)
{
    // unit_remove_map_pc, and pc_setpos for a homunculus: its skills' delays end with the warp (hom_delay_reset_warp, on in pre-renewal).  What it has of statuses
    // goes with its creature
    ROWOWIdentity* identity = sd->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    if (identity == nullptr)
        return;
    if (ROWOWHomunData* hd = identity->Homunculus.get())
    {
        if (isChangeMap == true && BattleConfig.HomDelayResetWarp != 0)
            hd->SkillBlocks.clear();
        FollowerUnspawn(sd, *hd);
    }
    if (ROWOWMercData* md = identity->Mercenary.get())
        FollowerUnspawn(sd, *md);
}

void ROWOWMod::FollowerMasterWarp(Player* sd, float x, float y, float z, float orientation)
{
    // pc_setpos on the same map: they go with their master.  A homunculus loses the statuses that end on a warp and its skills' delays (hom_delay_reset_warp)
    for (int32 followerType : { ROWOW_BL_HOM, ROWOW_BL_MER })
    {
        ROWOWFollower* fd = GetFollower(sd, followerType);
        Creature* md = fd != nullptr ? GetFollowerCreature(sd, *fd) : nullptr;
        if (md == nullptr || md->IsAlive() == false)
            continue;
        UnitStopWalking(md);
        GetMobData(md)->AttackTargetID.Clear();
        md->AttackStop();
        fd->State = ROWOW_AI_IDLE_ST;
        fd->Enemy.Clear();
        fd->Skill = 0;
        fd->ReservedMoves.clear();
        fd->TeleportTime = 0;
        if (followerType == ROWOW_BL_HOM)
        {
            if (ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(md)))
            {
                std::vector<int32> types;
                for (auto const& entryByType : sc->EntriesByType)
                    if (ROWOWStatusChangeDB const* scdb = GetStatusChangeDB(entryByType.first); scdb != nullptr && scdb->Flag[ROWOW_SCF_REMOVEFROMHOMONWARP] == true)
                        types.push_back(entryByType.first);
                for (int32 type : types)
                    StatusChangeEnd(md, type);
            }
            if (BattleConfig.HomDelayResetWarp != 0)
                fd->SkillBlocks.clear();
        }
        md->NearTeleportTo(x, y, z, orientation);
    }
}

void ROWOWMod::FollowerMasterDead(Player* sd)
{
    // pc_dead: a homunculus with 80% of its HP goes to rest, a mercenary leaves ("Your mercenary soldier has ran away", no loyalty lost)
    ROWOWHomunData* hd = GetHomunculus(sd);
    if (HomIsActive(hd) == true && BattleConfig.HomunculusAutoVapor != 0)
    {
        FollowerSaveVitals(sd, *hd);
        ROWOWStatus const* status = GetFollowerCreature(sd, *hd) != nullptr ? GetROStatus(GetFollowerCreature(sd, *hd)) : nullptr;
        if (status != nullptr && uint64(hd->HP) * 100 / std::max<uint32>(status->MaxHP, 1) >= uint64(BattleConfig.HomunculusAutoVapor))
            HomVaporize(sd, ROWOW_HOM_ST_ACTIVE);
    }
    if (GetMercenary(sd) != nullptr)
        MercenaryDelete(sd, 3);
}

void ROWOWMod::FollowerDead(Creature* md)
{
    // status_damage's end for a homunculus (hom_dead) or mercenary (mercenary_dead), from its creature's death.  The creature goes after its dying frames
    Player* sd = GetFollowerMaster(md);
    int32 followerType = GetFollowerType(md);
    ROWOWFollower* fd = sd != nullptr ? GetFollower(md) : nullptr;
    if (fd == nullptr)
    {
        md->DespawnOrUnsummon(Milliseconds(ROWOW_FOLLOWER_CORPSE_TIME_IN_MS));
        return;
    }
    fd->HP = 0;
    fd->CreatureGUID.Clear();
    SkillClearUnitGroup(md);
    SkillClearTimerSkill(md);
    md->CustomData.Erase("ROWOWFollowerOwner");
    md->DespawnOrUnsummon(Milliseconds(ROWOW_FOLLOWER_CORPSE_TIME_IN_MS));
    if (followerType == ROWOW_BL_HOM)
        HomDeadOwn(sd, static_cast<ROWOWHomunData&>(*fd));
    else
        MercenaryDelete(sd, 1); // Your mercenary soldier has been killed.
}

void ROWOWMod::UpdateFollowers(Player* sd, uint32 diff)
{
    ROWOWIdentity* identity = GetIdentity(sd);
    uint64 tick = GetTick();
    auto update = [this, sd, tick, identity](ROWOWFollower& fd, bool isOut)
    {
        // This holds the follower: a delete from anything it calls waits until it's let go (below)
        struct FollowerHold
        {
            ROWOWIdentity* Identity;
            explicit FollowerHold(ROWOWIdentity* identity) : Identity(identity) { Identity->IsFollowerHeld = true; }
            ~FollowerHold() { Identity->IsFollowerHeld = false; }
        } followerHold(identity);
        Creature* md = GetFollowerCreature(sd, fd);
        if (md == nullptr)
        {
            if (isOut == true)
                FollowerSpawn(sd, fd); // Its creature went (a grid that unloaded takes it)
            return;
        }
        if (md->IsAlive() == false)
            return;
        fd.HP = md->GetHealth();
        fd.SP = md->GetPower(POWER_MANA);
        if ((fd.HP != fd.SentHP || fd.SP != fd.SentSP) && tick >= fd.NextInfoTime)
        {
            fd.SentHP = fd.HP;
            fd.SentSP = fd.SP;
            fd.NextInfoTime = tick + FOLLOWER_INFO_INTERVAL_IN_MS;
            if (fd.FollowerType == ROWOW_BL_HOM)
                ClifHomInfo(sd);
            else
                ClifMercenaryInfo(sd);
        }
        ROWOWMobData* mob = GetMobData(md);
        if (mob->IsWalking == true && md->GetMotionMaster()->HasMovementGeneratorType(ESCORT_MOTION_TYPE) == false)
            mob->IsWalking = false;
        FollowerAttackTimerSub(sd, fd, md, mob, tick);
        FollowerNaturalHeal(sd, fd, md, mob, tick);
        if (tick < fd.NextThinkTime)
            return;
        fd.NextThinkTime = tick + MIN_FOLLOWERTHINKTIME;
        FollowerTeleportCheck(sd, fd, md, tick);
        if (GetFollowerCreature(sd, fd) != nullptr)
            FollowerAI(sd, fd, md, tick);
    };
    if (ROWOWHomunData* hd = identity->Homunculus.get())
    {
        bool isOut = HomIsActive(hd);
        if (isOut == true)
            HomHungry(sd, *hd, diff);
        // The hunger may have cost it its life
        if (identity->Homunculus != nullptr)
            update(*identity->Homunculus, HomIsActive(identity->Homunculus.get()));
        FollowerDeletePending(sd, *identity);
    }
    if (ROWOWMercData* md = identity->Mercenary.get())
    {
        MercenaryUpdate(sd, *md, diff);
        if (identity->Mercenary != nullptr)
            update(*identity->Mercenary, true);
        FollowerDeletePending(sd, *identity);
    }
}

void ROWOWMod::FollowerDeletePending(Player* sd, ROWOWIdentity& identity)
{
    // The deletes that came while UpdateFollowers held a follower
    if (identity.IsHomunculusDeletePending == true)
    {
        identity.IsHomunculusDeletePending = false;
        HomDelete(sd);
    }
    if (identity.MercenaryDeleteReply >= 0)
    {
        int32 reply = identity.MercenaryDeleteReply;
        identity.MercenaryDeleteReply = -1;
        MercenaryDelete(sd, reply);
    }
}

void ROWOWMod::FollowerTeleportCheck(Player* sd, ROWOWFollower& fd, Creature* md, uint64 tick)
{
    // unit_check_start_teleport_timer: too far from the master for 3 seconds, it's put beside it
    int32 maxDistance = fd.FollowerType == ROWOW_BL_HOM ? ROWOW_AREA_SIZE : ROWOW_MAX_MER_DISTANCE;
    if (CheckDistanceBL(sd, md, maxDistance) == true)
    {
        fd.TeleportTime = 0;
        return;
    }
    if (fd.TeleportTime == 0)
    {
        fd.TeleportTime = tick + FOLLOWER_TELEPORT_DELAY_IN_MS;
        return;
    }
    if (tick < fd.TeleportTime)
        return;
    fd.TeleportTime = 0;
    UnitStopWalking(md);
    GetMobData(md)->AttackTargetID.Clear();
    md->NearTeleportTo(sd->GetPositionX(), sd->GetPositionY(), sd->GetPositionZ(), sd->GetOrientation());
    fd.State = ROWOW_AI_IDLE_ST;
    fd.Enemy.Clear();
}

void ROWOWMod::FollowerMoveToOwner(Player* sd, ROWOWFollower& /*fd*/, Creature* md)
{
    // clif_parse_HomMoveToMaster: unit_calc_pos and unit_walktoxy
    int32 x0 = 0, y0 = 0, tx = 0, ty = 0, toX = 0, toY = 0;
    GetUnitCell(md, x0, y0);
    GetUnitCell(sd, tx, ty);
    if (UnitCalcPos(md->GetMap(), x0, y0, tx, ty, UnitGetDir(sd), toX, toY) == true)
        UnitWalkToXY(md, toX, toY);
}

Unit* ROWOWMod::FollowerGetOwnerEnemy(Player* sd, Creature* md) const
{
    // GetOwnerEnemy: the closest monster that has the owner as its target.  (ai.lua's other half, a player swinging at the owner, needs a PvP map)
    Unit* result = nullptr;
    int32 minDistance = 100;
    int32 x0 = 0, y0 = 0;
    GetUnitCell(md, x0, y0);
    MapForEachInRange(md, ROWOW_AREA_SIZE, [&](Unit* bl)
    {
        Creature* monster = bl->ToCreature();
        if (monster == nullptr || bl == md || GetROMobID(monster) == 0 || bl->IsAlive() == false)
            return 0;
        ROWOWMobData const* mob = monster->CustomData.Get<ROWOWMobData>("ROWOWMobData");
        if (mob == nullptr || mob->TargetID != sd->GetGUID())
            return 0;
        int32 x = 0, y = 0;
        GetUnitCell(bl, x, y);
        int32 distance = GetAIDistance(x0, y0, x, y);
        if (distance < minDistance)
        {
            result = bl;
            minDistance = distance;
        }
        return 0;
    });
    return result;
}

Unit* ROWOWMod::FollowerGetMyEnemy(ROWOWFollower const& fd, Creature* md) const
{
    // GetMyEnemy: the two temperaments.  GetMyEnemyA, what Lif and Amistr (and a mercenary) do: only what targets it.  GetMyEnemyB, Filir's and Vanilmirth's: the closest monster at all.
    // AI_M.lua isn't in this client, so a mercenary's temperament is the passive one
    uint32 homunculusType = (fd.ClassID - ROWOW_HM_CLASS_BASE) % 4;
    bool isAggressive = fd.FollowerType == ROWOW_BL_HOM && homunculusType >= 2;
    Unit* result = nullptr;
    int32 minDistance = 100;
    int32 x0 = 0, y0 = 0;
    GetUnitCell(md, x0, y0);
    MapForEachInRange(md, ROWOW_AREA_SIZE, [&](Unit* bl)
    {
        Creature* monster = bl->ToCreature();
        if (monster == nullptr || bl == md || GetROMobID(monster) == 0 || bl->IsAlive() == false || BattleCheckTarget(md, bl, ROWOW_BCT_ENEMY) <= 0)
            return 0;
        if (isAggressive == false)
        {
            ROWOWMobData const* mob = monster->CustomData.Get<ROWOWMobData>("ROWOWMobData");
            if (mob == nullptr || mob->TargetID != md->GetGUID())
                return 0;
        }
        int32 x = 0, y = 0;
        GetUnitCell(bl, x, y);
        int32 distance = GetAIDistance(x0, y0, x, y);
        if (distance < minDistance)
        {
            result = bl;
            minDistance = distance;
        }
        return 0;
    });
    return result;
}

bool ROWOWMod::FollowerIsInSight(Creature* md, Unit const* target, int32 range) const
{
    // IsInAttackSight's distance
    int32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    GetUnitCell(md, x0, y0);
    GetUnitCell(target, x1, y1);
    return range >= GetAIDistance(x0, y0, x1, y1);
}

void ROWOWMod::FollowerAI(Player* sd, ROWOWFollower& fd, Creature* md, uint64 tick)
{
    // ai.lua's AI(), one call: the state it's in, with Move as unit_walktoxy, Attack as what FollowerAttackTimerSub swings at, and SkillObject and SkillGround as its skill requests
    ROWOWMobData* mob = GetMobData(md);
    ROWOWStatus const* status = GetROStatus(md);
    if (status == nullptr)
        return;
    int32 x = 0, y = 0;
    GetUnitCell(md, x, y);
    int32 ownerX = 0, ownerY = 0;
    GetUnitCell(sd, ownerX, ownerY);
    int32 ownerDistance = GetAIDistance(x, y, ownerX, ownerY);
    Unit* enemy = fd.Enemy.IsEmpty() == false ? ObjectAccessor::GetUnit(*md, fd.Enemy) : nullptr;
    bool isEnemyGone = enemy == nullptr || enemy->IsInWorld() == false || enemy->GetMap() != md->GetMap();
    auto doMove = [&](int32 toX, int32 toY)
    {
        // OnMOVE_CMD: a walk past 15 cells is cut in two, the rest reserved
        if (std::abs(toX - x) + std::abs(toY - y) > FOLLOWER_MOVE_SPLIT_RANGE)
        {
            fd.ReservedMoves.insert(fd.ReservedMoves.begin(), std::make_pair(toX, toY));
            toX = (toX + x) / 2;
            toY = (toY + y) / 2;
        }
        UnitWalkToXY(md, toX, toY);
        fd.State = ROWOW_AI_MOVE_CMD_ST;
        fd.DestX = toX;
        fd.DestY = toY;
        fd.Enemy.Clear();
        fd.Skill = 0;
    };

    switch (fd.State)
    {
        case ROWOW_AI_IDLE_ST:
        {
            if (fd.ReservedMoves.empty() == false)
            {
                std::pair<int32, int32> move = fd.ReservedMoves.front();
                fd.ReservedMoves.erase(fd.ReservedMoves.begin());
                doMove(move.first, move.second);
                return;
            }
            if (tick >= fd.NextSearchTime)
            {
                fd.NextSearchTime = tick + FOLLOWER_SEARCH_INTERVAL_IN_MS;
                Unit* object = FollowerGetOwnerEnemy(sd, md); // MYOWNER_ATTACKED_IN
                if (object == nullptr)
                    object = FollowerGetMyEnemy(fd, md); // ATTACKED_IN
                if (object != nullptr)
                {
                    fd.State = ROWOW_AI_CHASE_ST;
                    fd.Enemy = object->GetGUID();
                    return;
                }
            }
            if (ownerDistance > FOLLOWER_OWNER_RANGE) // MYOWNER_OUTSIGNT_IN
                fd.State = ROWOW_AI_FOLLOW_ST;
            break;
        }
        case ROWOW_AI_FOLLOW_ST:
            if (ownerDistance <= FOLLOWER_OWNER_RANGE) // DESTINATION_ARRIVED_IN
                fd.State = ROWOW_AI_IDLE_ST;
            else if (mob->IsWalking == false)
                FollowerMoveToOwner(sd, fd, md);
            break;
        case ROWOW_AI_CHASE_ST:
        {
            int32 enemyX = 0, enemyY = 0;
            if (isEnemyGone == false)
                GetUnitCell(enemy, enemyX, enemyY);
            if (isEnemyGone == true || GetAIDistance(x, y, enemyX, enemyY) > FOLLOWER_SIGHT_RANGE) // ENEMY_OUTSIGHT_IN
            {
                fd.State = ROWOW_AI_IDLE_ST;
                fd.Enemy.Clear();
                fd.DestX = 0;
                fd.DestY = 0;
                return;
            }
            int32 range = fd.Skill != 0 ? SkillGetRange2(md, fd.Skill, fd.SkillLevel, false) : status->Range;
            if (FollowerIsInSight(md, enemy, range) == true) // ENEMY_INATTACKSIGHT_IN
            {
                fd.State = ROWOW_AI_ATTACK_ST;
                return;
            }
            if (fd.DestX != enemyX || fd.DestY != enemyY || mob->IsWalking == false) // DESTCHANGED_IN
            {
                fd.DestX = enemyX;
                fd.DestY = enemyY;
                int32 toX = 0, toY = 0;
                if (UnitCanReachBL(md, enemy, 0, toX, toY) == true)
                    UnitWalkToXY(md, toX, toY);
            }
            break;
        }
        case ROWOW_AI_ATTACK_ST:
        {
            int32 enemyX = 0, enemyY = 0;
            if (isEnemyGone == false)
                GetUnitCell(enemy, enemyX, enemyY);
            if (isEnemyGone == true || GetAIDistance(x, y, enemyX, enemyY) > FOLLOWER_SIGHT_RANGE || (enemy->IsAlive() == false && fd.Skill == 0)) // ENEMY_OUTSIGHT_IN, ENEMY_DEAD_IN
            {
                mob->AttackTargetID.Clear();
                md->AttackStop();
                fd.State = ROWOW_AI_IDLE_ST;
                fd.Enemy.Clear();
                fd.Skill = 0;
                return;
            }
            int32 range = fd.Skill != 0 ? SkillGetRange2(md, fd.Skill, fd.SkillLevel, false) : status->Range;
            if (FollowerIsInSight(md, enemy, range) == false) // ENEMY_OUTATTACKSIGHT_IN
            {
                mob->AttackTargetID.Clear();
                fd.State = ROWOW_AI_CHASE_ST;
                fd.DestX = enemyX;
                fd.DestY = enemyY;
                int32 toX = 0, toY = 0;
                if (UnitCanReachBL(md, enemy, 0, toX, toY) == true)
                    UnitWalkToXY(md, toX, toY);
                return;
            }
            if (fd.Skill == 0)
            {
                mob->AttackTargetID = enemy->GetGUID();
                if (md->GetVictim() != enemy)
                {
                    md->SetNoCallAssistance(true);
                    md->Attack(enemy, true);
                }
            }
            else
            {
                uint16 skillID = fd.Skill;
                uint16 skillLevel = fd.SkillLevel;
                fd.Skill = 0;
                FollowerSkillUseID(sd, fd, md, skillID, skillLevel, enemy);
                if (GetFollowerCreature(sd, fd) != nullptr && (enemy == md || enemy == sd || enemy->IsAlive() == false || BattleCheckTarget(md, enemy, ROWOW_BCT_ENEMY) <= 0))
                {
                    fd.State = ROWOW_AI_IDLE_ST;
                    fd.Enemy.Clear();
                }
            }
            break;
        }
        case ROWOW_AI_MOVE_CMD_ST:
            if ((x == fd.DestX && y == fd.DestY) || mob->IsWalking == false) // DESTINATION_ARRIVED_IN
                fd.State = ROWOW_AI_IDLE_ST;
            break;
        case ROWOW_AI_HOLD_CMD_ST:
        {
            if (isEnemyGone == false)
            {
                if (enemy->IsAlive() == true && FollowerIsInSight(md, enemy, status->Range) == true)
                {
                    mob->AttackTargetID = enemy->GetGUID();
                    if (md->GetVictim() != enemy)
                        md->Attack(enemy, true);
                }
                else
                {
                    mob->AttackTargetID.Clear();
                    md->AttackStop();
                    fd.Enemy.Clear();
                }
                return;
            }
            fd.Enemy.Clear();
            if (tick >= fd.NextSearchTime)
            {
                fd.NextSearchTime = tick + FOLLOWER_SEARCH_INTERVAL_IN_MS;
                Unit* object = FollowerGetOwnerEnemy(sd, md);
                if (object == nullptr)
                    object = FollowerGetMyEnemy(fd, md);
                if (object != nullptr)
                    fd.Enemy = object->GetGUID();
            }
            break;
        }
        case ROWOW_AI_SKILL_AREA_CMD_ST:
            if (GetAIDistance(x, y, fd.DestX, fd.DestY) <= SkillGetRange2(md, fd.Skill, fd.SkillLevel, false)) // DESTARRIVED_IN
            {
                uint16 skillID = fd.Skill;
                uint16 skillLevel = fd.SkillLevel;
                fd.Skill = 0;
                fd.State = ROWOW_AI_IDLE_ST;
                UnitStopWalking(md);
                FollowerSkillUsePos(sd, fd, md, skillID, skillLevel, fd.DestX, fd.DestY, fd.DestZ);
            }
            else if (mob->IsWalking == false)
            {
                fd.Skill = 0;
                fd.State = ROWOW_AI_IDLE_ST;
            }
            break;
        case ROWOW_AI_FOLLOW_CMD_ST:
            // OnFOLLOW_CMD_ST: it stays by its owner and fights nothing, ai.lua's standby
            if (ownerDistance <= FOLLOWER_OWNER_RANGE)
                return;
            if (mob->IsWalking == false || GetAIDistance(ownerX, ownerY, fd.DestX, fd.DestY) > FOLLOWER_OWNER_RANGE)
            {
                FollowerMoveToOwner(sd, fd, md);
                fd.DestX = ownerX;
                fd.DestY = ownerY;
            }
            break;
        default:
            break;
    }
}

void ROWOWMod::FollowerAttackTimerSub(Player* /*sd*/, ROWOWFollower& fd, Creature* md, ROWOWMobData* mob, uint64 tick)
{
    // unit_attack_timer_sub, each update on WoW's attack timer (set to its attack delay), as a monster's: the swing at what the AI attacks when the delay is over.  (Each Attack of ai.lua
    // asks for one swing, unit_attack's attack_continue off.  The AI asks again until the target's gone)
    if (mob->AttackTargetID.IsEmpty() == true || md->isAttackReady() == false || md->HasUnitState(UNIT_STATE_CASTING) == true)
        return;
    Unit* target = ObjectAccessor::GetUnit(*md, mob->AttackTargetID);
    ROWOWStatus const* sstatus = GetROStatus(md);
    if (target == nullptr || sstatus == nullptr || target->GetMap() != md->GetMap() || target->IsAlive() == false)
    {
        mob->AttackTargetID.Clear();
        return;
    }
    ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(md));
    if (BattleCheckTarget(md, target, ROWOW_BCT_ENEMY) <= 0 || StatusCheckSkillUse(md, target, 0, 0) == false || (sc != nullptr && sc->Cant.Attack == true))
    {
        mob->AttackTargetID.Clear();
        fd.Enemy.Clear();
        fd.State = ROWOW_AI_IDLE_ST;
        return;
    }

    // Attacking when under cast delay has restrictions
    if (BattleConfig.SDelayAttackEnable == 0 && GetSkillUnitData(md)->CanActTime > tick)
        return;
    int32 range = sstatus->Range;
    if (target->isMoving() == true)
        range++; // Extra range when chasing
    if (BattleCheckRange(md, target, range) == false)
        return;
    if (mob->IsWalking == true)
        UnitStopWalking(md);
    if (md->movespline->Finalized() == true && md->HasInArc(float(M_PI) / 4.0f, target) == false && IsMobMovementBlocked(md, true) == false)
        md->SetFacingToObject(target);
    md->AttackerStateUpdate(target);
    md->resetAttackTimer();
}

void ROWOWMod::FollowerNaturalHeal(Player* /*sd*/, ROWOWFollower& fd, Creature* md, ROWOWMobData const* mob, uint64 tick)
{
    // status_natural_heal for a homunculus (HP every 2 seconds, SP every 4) and a mercenary (8 and 6, and it walks on).  A walking homunculus doesn't regenerate, nothing does while hidden
    ROWOWStatus const* status = GetROStatus(md);
    if (status == nullptr)
        return;
    uint8 flag = status->RegenFlag;
    ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(md));
    if (sc != nullptr && (sc->Option & (ROWOW_OPTION_HIDE | ROWOW_OPTION_CLOAK | ROWOW_OPTION_CHASEWALK)) != 0)
        flag = 0;
    if (mob->IsWalking == true && fd.FollowerType == ROWOW_BL_HOM)
        flag &= ~(ROWOW_RGN_HP | ROWOW_RGN_SP);

    // The tick interval rAthena takes off to round to its timer
    constexpr int32 NATURAL_HEAL_HALF_INTERVAL = 250;
    if ((flag & ROWOW_RGN_HP) != 0)
    {
        int32 rate = int32(BattleConfig.NaturalHealHPInterval / (status->RegenRateHP / 100.));
        rate = fd.FollowerType == ROWOW_BL_HOM ? rate / 3 : (rate * 4) / 3; // 2 seconds instead of 6 seconds, 8 seconds instead of 6 seconds
        rate -= NATURAL_HEAL_HALF_INTERVAL;
        if (fd.RegenHPTime + uint64(std::max(rate, 0)) <= tick)
        {
            fd.RegenHPTime = tick;
            if (md->GetHealth() < md->GetMaxHealth())
                md->SetHealth(std::min<uint32>(md->GetMaxHealth(), md->GetHealth() + uint32(status->RegenHP)));
        }
    }
    else
        fd.RegenHPTime = tick;
    if ((flag & ROWOW_RGN_SP) != 0)
    {
        int32 rate = int32(BattleConfig.NaturalHealSPInterval / (status->RegenRateSP / 100.));
        rate = fd.FollowerType == ROWOW_BL_HOM ? rate / 2 : (rate * 3) / 4; // 4 seconds instead of 8 seconds, 6 seconds instead of 8 seconds
        rate -= NATURAL_HEAL_HALF_INTERVAL;
        if (fd.RegenSPTime + uint64(std::max(rate, 0)) <= tick)
        {
            fd.RegenSPTime = tick;
            if (md->GetPower(POWER_MANA) < md->GetMaxPower(POWER_MANA))
                md->SetPower(POWER_MANA, std::min<uint32>(md->GetMaxPower(POWER_MANA), md->GetPower(POWER_MANA) + uint32(status->RegenSP)));
        }
    }
    else
        fd.RegenSPTime = tick;
}

bool ROWOWMod::FollowerSkillIsNotOk(Player* sd, ROWOWFollower& fd, uint16 skillID)
{
    // skill_isNotOk_hom and skill_isNotOk_mercenary: its own block, a homunculus's intimacy rules, and the map's rules of skill_isNotOk, as its master's
    uint64 tick = GetTick();
    auto blockIter = fd.SkillBlocks.find(skillID);
    if (blockIter != fd.SkillBlocks.end() && blockIter->second > tick)
    {
        ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_SKILLINTERVAL);
        return true;
    }
    if (SkillIsNotOk(sd, skillID) == true)
        return true;
    if (fd.FollowerType != ROWOW_BL_HOM)
        return false;
    ROWOWHomunData const& hd = static_cast<ROWOWHomunData const&>(fd);
    switch (skillID)
    {
        case ROWOW_SKILL_HFLI_SBR44:
            if (HomGetIntimacyGrade(hd) <= ROWOW_HOMGRADE_HATE_WITH_PASSION)
            {
                ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_LEVEL); // USESKILL_FAIL_RELATIONGRADE
                return true;
            }
            break;
        case ROWOW_SKILL_HVAN_EXPLOSION:
            if (hd.Intimacy < uint32(BattleConfig.HvanExplosionIntimate))
            {
                ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_LEVEL); // USESKILL_FAIL_RELATIONGRADE
                return true;
            }
            break;
        default:
            break;
    }
    return false;
}

void ROWOWMod::FollowerSkillUseID(Player* sd, ROWOWFollower& fd, Creature* md, uint16 skillID, uint16 skillLevel, Unit* target)
{
    // clif_parse_UseSkillToId_homun and _mercenary, then unit_skilluse_id: a real cast of the skill's shell, its checks in the engine, as a monster's.  mob->FollowerSkill names the level
    if (FollowerSkillIsNotOk(sd, fd, skillID) == true)
    {
        ClifEmotion(md, ROWOW_EMOTION_THINK);
        return;
    }
    ROWOWSkill const* skill = GetSkill(skillID);
    if (skill == nullptr || target == nullptr)
        return;
    if (target != md && (skill->Inf & ROWOW_INF_SELF_SKILL) != 0)
        target = md;
    ROWOWSkillUnitData* ud = GetSkillUnitData(md);
    uint64 tick = GetTick();
    if (ud->SkillID != 0)
        return;
    if (tick < ud->CanActTime)
    {
        ClifEmotion(md, ROWOW_EMOTION_THINK);
        ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_SKILLINTERVAL);
        return;
    }
    uint16 level = fd.FollowerType == ROWOW_BL_HOM ? uint16(HomCheckSkill(md, skillID)) : MercenaryCheckSkill(md, skillID);
    skillLevel = std::min(skillLevel, level);
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(GetSkillSpellID(skillID, 1)); // Its one spell.  The engine goes by mob->FollowerSkill's level
    if (skillLevel == 0 || spellInfo == nullptr)
        return;

    // unit_skilluse_id2's target for the skills a homunculus uses on its master
    if (fd.FollowerType == ROWOW_BL_HOM)
        target = UnitSkillTarget(md, target, target, skillID);
    ROWOWMobData* mob = GetMobData(md);
    UnitStopWalking(md);
    mob->FollowerSkill = ROWOWMobSkill();
    mob->FollowerSkill.SkillID = skillID;
    mob->FollowerSkill.SkillLevel = skillLevel;
    mob->FollowerSkill.CastTime = SkillCastFix(md, skillID, skillLevel, true);
    mob->FollowerSkill.IsCancel = skill->IsCastCancel;
    SpellCastTargets targets;
    targets.SetUnitTarget(target);
    SpellCastResult result = md->CastSpell(targets, spellInfo, nullptr, FOLLOWER_SKILL_CAST_FLAGS);
    if (result != SPELL_CAST_OK)
        mob->FollowerSkill.SkillID = 0;
}

void ROWOWMod::FollowerSkillUsePos(Player* sd, ROWOWFollower& fd, Creature* md, uint16 skillID, uint16 skillLevel, int32 x, int32 y, float z)
{
    // clif_parse_UseSkillToPos_homun and _mercenary, then unit_skilluse_pos
    if (FollowerSkillIsNotOk(sd, fd, skillID) == true)
        return;
    ROWOWSkillUnitData* ud = GetSkillUnitData(md);
    uint64 tick = GetTick();
    if (ud->SkillID != 0 || GetSCE(md, ROWOW_SC_BASILICA) != nullptr)
        return;
    if (tick < ud->CanActTime)
    {
        ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_SKILLINTERVAL);
        return;
    }
    uint16 level = fd.FollowerType == ROWOW_BL_HOM ? uint16(HomCheckSkill(md, skillID)) : MercenaryCheckSkill(md, skillID);
    skillLevel = std::min(skillLevel, level);
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(GetSkillSpellID(skillID, 1)); // Its one spell.  The engine goes by mob->FollowerSkill's level
    float worldX = 0, worldY = 0;
    if (skillLevel == 0 || spellInfo == nullptr || GetCellWorldPosition(md->GetMapId(), x, y, worldX, worldY) == false)
        return;
    ROWOWSkill const* skill = GetSkill(skillID);
    ROWOWMobData* mob = GetMobData(md);
    UnitStopWalking(md);
    mob->FollowerSkill = ROWOWMobSkill();
    mob->FollowerSkill.SkillID = skillID;
    mob->FollowerSkill.SkillLevel = skillLevel;
    mob->FollowerSkill.CastTime = SkillCastFix(md, skillID, skillLevel, true);
    mob->FollowerSkill.IsCancel = skill != nullptr && skill->IsCastCancel == true;
    SpellCastTargets targets;
    targets.SetDst(worldX, worldY, z, md->GetOrientation());
    SpellCastResult result = md->CastSpell(targets, spellInfo, nullptr, FOLLOWER_SKILL_CAST_FLAGS);
    if (result != SPELL_CAST_OK)
        mob->FollowerSkill.SkillID = 0;
}

void ROWOWMod::FollowerClearSkill(Unit* bl)
{
    ROWOWMobData* mob = bl->CustomData.Get<ROWOWMobData>("ROWOWMobData");
    if (mob != nullptr)
        mob->FollowerSkill.SkillID = 0;
}

bool ROWOWMod::FollowerSkillCheckCondition(Unit* bl, uint16 skillID, uint16 skillLevel, int32 type)
{
    // skill_check_condition_mercenary: type&1 pays too, type&2 pays the items alone, without looking at HP and SP
    Player* sd = GetFollowerMaster(bl);
    ROWOWSkill const* skill = GetSkill(skillID);
    ROWOWStatus const* status = GetROStatus(bl);
    if (sd == nullptr || skill == nullptr || status == nullptr)
        return false;
    int32 levelIndex = ROWOWSkill::GetLevelIndex(skillLevel);
    int64 hp = skill->HPCost[levelIndex];
    int64 sp = skill->SPCost[levelIndex];
    int32 hpRate = skill->HPRateCost[levelIndex];
    int32 spRate = skill->SPRateCost[levelIndex];
    if (hpRate > 0)
        hp += int64(bl->GetHealth()) * hpRate / 100;
    else
        hp += int64(status->MaxHP) * (-hpRate) / 100;
    if (spRate > 0)
        sp += int64(bl->GetPower(POWER_MANA)) * spRate / 100;
    else
        sp += int64(status->MaxSP) * (-spRate) / 100;
    if ((type & 2) == 0)
    {
        if (hp > 0 && int64(bl->GetHealth()) <= hp)
        {
            ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_HP_INSUFFICIENT);
            return false;
        }
        if (sp > 0 && int64(bl->GetPower(POWER_MANA)) <= sp)
        {
            ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_SP_INSUFFICIENT);
            return false;
        }
    }
    if (type == 0)
    {
        if (skill->RequiredState == ROWOW_ST_MOVE_ENABLE)
        {
            Creature* md = bl->ToCreature();
            if (md == nullptr || UnitCanMove(md, GetMobData(md), GetTick()) == false)
            {
                ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_LEVEL);
                return false;
            }
        }
    }
    if ((type & 1) == 0)
        return true;

    // Check item existences, then consume them, not out of a trade
    if (skill->RequiredItems.empty() == false && PCCantAct(sd) == true)
    {
        ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_LEVEL);
        return false;
    }
    std::vector<std::pair<Item*, uint32>> items;
    for (ROWOWSkillItemRequirement const& requirement : skill->RequiredItems)
    {
        if (requirement.ItemID == 0 || (requirement.Level != 0 && requirement.Level != skillLevel))
            continue;
        Item* item = PCSearchInventory(sd, requirement.ItemID);
        if (item == nullptr || item->GetCount() < uint32(requirement.Amount))
        {
            ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_LEVEL);
            return false;
        }
        items.emplace_back(item, uint32(requirement.Amount));
    }
    for (auto& item : items)
    {
        uint32 count = item.second;
        sd->DestroyItemCount(item.first, count, true);
    }
    if ((type & 2) != 0)
        return true;
    if (sp > 0 || hp > 0)
        StatusZap(bl, uint32(hp), uint32(sp));
    return true;
}

void ROWOWMod::FollowerSetSkillBlock(Unit* bl, uint16 skillID, uint16 skillLevel)
{
    // skill_castend_id's skill_blockhomun_start, on the skill's after-cast delay in pre-renewal, and skill_blockmerc_start, on its cooldown
    ROWOWFollower* fd = GetFollower(bl);
    ROWOWSkill const* skill = GetSkill(skillID);
    if (fd == nullptr || skill == nullptr)
        return;
    int32 levelIndex = ROWOWSkill::GetLevelIndex(skillLevel);
    int64 block = fd->FollowerType == ROWOW_BL_HOM ? int64(skill->AfterCastActDelay[levelIndex]) : int64(skill->Cooldown[levelIndex]);
    if (block > 0)
        fd->SkillBlocks[skillID] = GetTick() + uint64(block);
}

void ROWOWMod::FollowerOrderFollow(Player* sd, int32 followerType)
{
    // OnFOLLOW_CMD: Alt+T.  Stand by and follow, or back to its own mind
    ROWOWFollower* fd = GetFollower(sd, followerType);
    Creature* md = fd != nullptr ? GetFollowerCreature(sd, *fd) : nullptr;
    if (md == nullptr)
        return;
    ROWOWMobData* mob = GetMobData(md);
    fd->ReservedMoves.clear();
    mob->AttackTargetID.Clear();
    md->AttackStop();
    fd->Enemy.Clear();
    fd->Skill = 0;
    if (fd->State == ROWOW_AI_FOLLOW_CMD_ST)
    {
        fd->State = ROWOW_AI_IDLE_ST;
        return;
    }
    FollowerMoveToOwner(sd, *fd, md);
    fd->State = ROWOW_AI_FOLLOW_CMD_ST;
    GetUnitCell(sd, fd->DestX, fd->DestY);
}

void ROWOWMod::FollowerOrderHold(Player* sd, int32 followerType)
{
    // OnHOLD_CMD: it stays where it stands and swings at what comes in reach
    ROWOWFollower* fd = GetFollower(sd, followerType);
    Creature* md = fd != nullptr ? GetFollowerCreature(sd, *fd) : nullptr;
    if (md == nullptr)
        return;
    fd->ReservedMoves.clear();
    UnitStopWalking(md);
    GetMobData(md)->AttackTargetID.Clear();
    md->AttackStop();
    fd->DestX = 0;
    fd->DestY = 0;
    fd->Enemy.Clear();
    fd->Skill = 0;
    fd->State = ROWOW_AI_HOLD_CMD_ST;
}

void ROWOWMod::FollowerOrderAttack(Player* sd, int32 followerType, Unit* target)
{
    // OnATTACK_OBJECT_CMD: Alt and a click on a monster
    ROWOWFollower* fd = GetFollower(sd, followerType);
    Creature* md = fd != nullptr ? GetFollowerCreature(sd, *fd) : nullptr;
    if (md == nullptr || target == nullptr || target == md)
        return;
    if (BattleCheckTarget(md, target, ROWOW_BCT_ENEMY) <= 0)
        return;
    fd->ReservedMoves.clear();
    fd->Skill = 0;
    fd->Enemy = target->GetGUID();
    fd->DestX = 0;
    fd->DestY = 0;
    fd->State = ROWOW_AI_CHASE_ST;
}

void ROWOWMod::FollowerOrderMove(Player* sd, int32 followerType, float x, float y, float z)
{
    // OnMOVE_CMD, or OnSKILL_AREA_CMD for a ground skill waiting for its spot: Alt and a click on the ground
    ROWOWFollower* fd = GetFollower(sd, followerType);
    Creature* md = fd != nullptr ? GetFollowerCreature(sd, *fd) : nullptr;
    if (md == nullptr)
        return;
    int32 cellX = 0, cellY = 0;
    GetWorldCell(md->GetMapId(), x, y, cellX, cellY);
    ROWOWMobData* mob = GetMobData(md);
    mob->AttackTargetID.Clear();
    md->AttackStop();
    fd->ReservedMoves.clear();
    fd->Enemy.Clear();
    int32 x0 = 0, y0 = 0;
    GetUnitCell(md, x0, y0);
    if (fd->Skill != 0 && fd->State == ROWOW_AI_SKILL_AREA_CMD_ST)
    {
        // unit_skilluse_pos's reach is the skill's, and the AI walks into it
        fd->DestX = cellX;
        fd->DestY = cellY;
        fd->DestZ = z;
        UnitWalkToXY(md, cellX, cellY);
        if (mob->IsWalking == false && GetAIDistance(x0, y0, cellX, cellY) > SkillGetRange2(md, fd->Skill, fd->SkillLevel, false))
            fd->State = ROWOW_AI_IDLE_ST;
        return;
    }
    if (std::abs(cellX - x0) + std::abs(cellY - y0) > FOLLOWER_MOVE_SPLIT_RANGE)
    {
        fd->ReservedMoves.emplace_back(cellX, cellY);
        cellX = (cellX + x0) / 2;
        cellY = (cellY + y0) / 2;
    }
    UnitWalkToXY(md, cellX, cellY);
    fd->State = ROWOW_AI_MOVE_CMD_ST;
    fd->DestX = cellX;
    fd->DestY = cellY;
    fd->Skill = 0;
}

void ROWOWMod::FollowerOrderSkill(Player* sd, int32 followerType, uint16 skillID, uint16 skillLevel, Unit* target)
{
    // OnSKILL_OBJECT_CMD, or the first half of OnSKILL_AREA_CMD: a ground skill waits for the spot its window's cursor picks (FollowerOrderMove)
    ROWOWFollower* fd = GetFollower(sd, followerType);
    Creature* md = fd != nullptr ? GetFollowerCreature(sd, *fd) : nullptr;
    ROWOWSkill const* skill = GetSkill(skillID);
    if (md == nullptr || skill == nullptr)
        return;
    if ((skill->Inf & ROWOW_INF_GROUND_SKILL) != 0)
    {
        fd->Skill = skillID;
        fd->SkillLevel = skillLevel;
        fd->State = ROWOW_AI_SKILL_AREA_CMD_ST;
        fd->DestX = 0;
        fd->DestY = 0;
        GetUnitCell(md, fd->DestX, fd->DestY);
        FollowerSendGroundOrder(sd, followerType);
        return;
    }
    if ((skill->Inf & ROWOW_INF_SELF_SKILL) != 0)
        target = md;
    else if (target == nullptr || (fd->FollowerType == ROWOW_BL_HOM && (skill->Inf & ROWOW_INF_SUPPORT_SKILL) != 0 && target != sd && target != md))
        target = sd;
    fd->ReservedMoves.clear();
    GetMobData(md)->AttackTargetID.Clear();
    fd->Skill = skillID;
    fd->SkillLevel = skillLevel;
    fd->Enemy = target->GetGUID();
    fd->DestX = 0;
    fd->DestY = 0;
    fd->State = ROWOW_AI_CHASE_ST;
}

void ROWOWMod::FollowerSendGroundOrder(Player* sd, int32 followerType)
{
    // The ground cursor is the client's: the window casts ROWOW_SPELL_FOLLOWER_ORDER, whose spot comes back to FollowerOrderMove
    GetIdentity(sd)->FollowerOrderType = followerType;
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_ADDON, nullptr, nullptr, "ROWOWFOLLOWER\tG");
    sd->SendDirectMessage(&data);
}

std::string ROWOWMod::GetFollowerStatusDump(Player* sd, int32 followerType)
{
    // The format of the oracle's @oraclehomstatus and @oraclemerstatus, keys oracle_status_print's
    ROWOWFollower* fd = GetFollower(sd, followerType);
    Creature* md = fd != nullptr ? GetFollowerCreature(sd, *fd) : nullptr;
    ROWOWStatus const* s = md != nullptr ? GetROStatus(md) : nullptr;
    if (s == nullptr)
        return "no follower out";
    return fmt::format("str={} agi={} vit={} int={} dex={} luk={} maxhp={} maxsp={} hp={} sp={} batk={} watk={} watk2={} matk_min={} matk_max={} range={} def={} def2={} mdef={} mdef2={} hit={} flee={} flee2={} "
        "cri={} def_ele={} ele_lv={} race={} size={} class={} amotion={} adelay={} dmotion={} aspd_rate={} speed={} mode={} lv={} regen_hp={} regen_sp={}", s->Stats[ROWOW_STAT_STR], s->Stats[ROWOW_STAT_AGI],
        s->Stats[ROWOW_STAT_VIT], s->Stats[ROWOW_STAT_INT], s->Stats[ROWOW_STAT_DEX], s->Stats[ROWOW_STAT_LUK], s->MaxHP, s->MaxSP, md->GetHealth(), md->GetPower(POWER_MANA), s->BaseAttack, s->WeaponAttack,
        s->WeaponRefineAttack, s->MagicAttackMin, s->MagicAttackMax, s->Range, s->Def, s->Def2, s->MDef, s->MDef2, s->Hit, s->Flee, s->Flee2, s->Critical, uint32(s->DefElement), uint32(s->ElementLevel),
        uint32(s->Race), uint32(s->Size), uint32(s->Class), s->AttackMotion, s->AttackDelay, s->DamageMotion, s->AspdRate, s->Speed, s->Mode, s->Level, s->RegenHP, s->RegenSP);
}

void ROWOWMod::RegisterFollowerCommands()
{
    // homevolution and homshuffle (homunculus_evolution, homunculus_shuffle), checkhomcall, gethominfo, addhomintimacy, and mercenary_*, getmercinfo: the script commands the
    // Stone of Sage, the mercenary scrolls and the Mercenary Managers use
    ROWOWSCRIPT->RegisterConstant("ARCH_MERC_GUILD", 0);
    ROWOWSCRIPT->RegisterConstant("SPEAR_MERC_GUILD", 1);
    ROWOWSCRIPT->RegisterConstant("SWORD_MERC_GUILD", 2);
    auto homEvolution = [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const&)
    {
        Player* sd = context.AttachedPlayer;
        ROWOWHomunData* hd = sd != nullptr ? GetHomunculus(sd) : nullptr;
        if (HomIsActive(hd) == true)
        {
            if (hd->Intimacy >= uint32(BattleConfig.HomunculusEvoIntimacyNeed))
                HomEvolution(sd, *hd);
            else if (Creature* md = GetFollowerCreature(sd, *hd))
                ClifEmotion(md, ROWOW_EMOTION_SWEAT);
        }
        return ROWOWScriptValue::FromNumber(0);
    };
    ROWOWSCRIPT->RegisterCommand("homevolution", homEvolution);
    ROWOWSCRIPT->RegisterCommand("homunculus_evolution", homEvolution);
    auto homShuffle = [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const&)
    {
        Player* sd = context.AttachedPlayer;
        ROWOWHomunData* hd = sd != nullptr ? GetHomunculus(sd) : nullptr;
        if (HomIsActive(hd) == true)
            HomShuffle(sd, *hd);
        return ROWOWScriptValue::FromNumber(0);
    };
    ROWOWSCRIPT->RegisterCommand("homshuffle", homShuffle);
    ROWOWSCRIPT->RegisterCommand("homunculus_shuffle", homShuffle);
    ROWOWSCRIPT->RegisterCommand("checkhomcall", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const&)
    {
        ROWOWHomunData const* hd = context.AttachedPlayer != nullptr ? GetHomunculus(context.AttachedPlayer) : nullptr;
        return ROWOWScriptValue::FromNumber(hd == nullptr ? -1 : hd->Vaporize);
    });
    ROWOWSCRIPT->RegisterCommand("gethominfo", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        int64 type = arguments.empty() == false ? arguments[0].GetNumber() : 0;
        ROWOWHomunData const* hd = context.AttachedPlayer != nullptr ? GetHomunculus(context.AttachedPlayer) : nullptr;
        if (hd == nullptr)
            return type == 2 ? ROWOWScriptValue::FromText("null") : ROWOWScriptValue::FromNumber(0);
        switch (type)
        {
            case 0: return ROWOWScriptValue::FromNumber(hd->ClassID);
            case 1: return ROWOWScriptValue::FromNumber(hd->ClassID);
            case 2: return ROWOWScriptValue::FromText(hd->Name);
            case 3: return ROWOWScriptValue::FromNumber(hd->Intimacy);
            case 4: return ROWOWScriptValue::FromNumber(hd->Hunger);
            case 5: return ROWOWScriptValue::FromNumber(hd->IsRenamed == true ? 1 : 0);
            case 6: return ROWOWScriptValue::FromNumber(hd->Level);
            case 7: return ROWOWScriptValue::FromNumber(int64(hd->CreatureGUID.GetCounter()));
            default: return ROWOWScriptValue::FromNumber(0);
        }
    });
    ROWOWSCRIPT->RegisterCommand("addhomintimacy", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        ROWOWHomunData* hd = context.AttachedPlayer != nullptr ? GetHomunculus(context.AttachedPlayer) : nullptr;
        int64 value = arguments.empty() == false ? arguments[0].GetNumber() : 0;
        if (hd != nullptr && value > 0)
            HomIncreaseIntimacy(*hd, uint32(value));
        else if (hd != nullptr && value < 0)
            HomDecreaseIntimacy(*hd, uint32(-value));
        if (hd != nullptr && value != 0)
            ClifHomInfo(context.AttachedPlayer);
        return ROWOWScriptValue::FromNumber(0);
    });
    ROWOWSCRIPT->RegisterCommand("mercenary_create", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        if (context.AttachedPlayer != nullptr && arguments.size() >= 2 && GetMercenary(context.AttachedPlayer) == nullptr)
            MercenaryCreate(context.AttachedPlayer, uint32(arguments[0].GetNumber()), uint32(arguments[1].GetNumber()));
        return ROWOWScriptValue::FromNumber(0);
    });
    ROWOWSCRIPT->RegisterCommand("mercenary_delete", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        if (context.AttachedPlayer != nullptr && GetMercenary(context.AttachedPlayer) != nullptr)
            MercenaryDelete(context.AttachedPlayer, arguments.size() >= 2 ? int32(arguments[1].GetNumber()) : 0);
        return ROWOWScriptValue::FromNumber(0);
    });
    ROWOWSCRIPT->RegisterCommand("mercenary_heal", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        ROWOWMercData* md = context.AttachedPlayer != nullptr ? GetMercenary(context.AttachedPlayer) : nullptr;
        Creature* creature = md != nullptr ? GetFollowerCreature(context.AttachedPlayer, *md) : nullptr;
        if (creature != nullptr && arguments.size() >= 2)
            StatusHeal(creature, arguments[0].GetNumber(), arguments[1].GetNumber(), 0, creature, 0);
        return ROWOWScriptValue::FromNumber(0);
    });
    ROWOWSCRIPT->RegisterCommand("mercenary_sc_start", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        ROWOWMercData* md = context.AttachedPlayer != nullptr ? GetMercenary(context.AttachedPlayer) : nullptr;
        Creature* creature = md != nullptr ? GetFollowerCreature(context.AttachedPlayer, *md) : nullptr;
        if (creature != nullptr && arguments.size() >= 3)
            StatusChangeStart(creature, creature, int32(arguments[0].GetNumber()), 10000, int32(arguments[2].GetNumber()), 0, 0, 0, arguments[1].GetNumber(), ROWOW_SCSTART_NOAVOID);
        return ROWOWScriptValue::FromNumber(0);
    });
    auto getGuildValue = [this](Player* sd, int32 guild, bool isFaith)
    {
        ROWOWMercenaryOwner const& owner = GetIdentity(sd)->MercenaryOwner;
        switch (guild)
        {
            case 0: return isFaith == true ? owner.ArchFaith : owner.ArchCalls;
            case 1: return isFaith == true ? owner.SpearFaith : owner.SpearCalls;
            case 2: return isFaith == true ? owner.SwordFaith : owner.SwordCalls;
            default: return 0;
        }
    };
    auto setGuildValue = [this](Player* sd, int32 guild, bool isFaith, int32 value)
    {
        ROWOWMercenaryOwner& owner = GetIdentity(sd)->MercenaryOwner;
        int32* field = nullptr;
        switch (guild)
        {
            case 0: field = isFaith == true ? &owner.ArchFaith : &owner.ArchCalls; break;
            case 1: field = isFaith == true ? &owner.SpearFaith : &owner.SpearCalls; break;
            case 2: field = isFaith == true ? &owner.SwordFaith : &owner.SwordCalls; break;
            default: return;
        }
        *field = std::clamp<int64>(int64(*field) + value, 0, isFaith == true ? SHRT_MAX : INT_MAX);
    };
    ROWOWSCRIPT->RegisterCommand("mercenary_get_calls", [getGuildValue](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        return ROWOWScriptValue::FromNumber(context.AttachedPlayer != nullptr && arguments.empty() == false ? getGuildValue(context.AttachedPlayer, int32(arguments[0].GetNumber()), false) : 0);
    });
    ROWOWSCRIPT->RegisterCommand("mercenary_set_calls", [setGuildValue](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        if (context.AttachedPlayer != nullptr && arguments.size() >= 2)
            setGuildValue(context.AttachedPlayer, int32(arguments[0].GetNumber()), false, int32(arguments[1].GetNumber()));
        return ROWOWScriptValue::FromNumber(0);
    });
    ROWOWSCRIPT->RegisterCommand("mercenary_get_faith", [getGuildValue](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        return ROWOWScriptValue::FromNumber(context.AttachedPlayer != nullptr && arguments.empty() == false ? getGuildValue(context.AttachedPlayer, int32(arguments[0].GetNumber()), true) : 0);
    });
    ROWOWSCRIPT->RegisterCommand("mercenary_set_faith", [setGuildValue](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        if (context.AttachedPlayer != nullptr && arguments.size() >= 2)
            setGuildValue(context.AttachedPlayer, int32(arguments[0].GetNumber()), true, int32(arguments[1].GetNumber()));
        return ROWOWScriptValue::FromNumber(0);
    });
    ROWOWSCRIPT->RegisterCommand("getmercinfo", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        int64 type = arguments.empty() == false ? arguments[0].GetNumber() : 0;
        ROWOWMercData const* md = context.AttachedPlayer != nullptr ? GetMercenary(context.AttachedPlayer) : nullptr;
        if (md == nullptr)
            return type == 2 ? ROWOWScriptValue::FromText("") : ROWOWScriptValue::FromNumber(0);
        switch (type)
        {
            case 0: return ROWOWScriptValue::FromNumber(md->ClassID);
            case 1: return ROWOWScriptValue::FromNumber(md->ClassID);
            case 2: return ROWOWScriptValue::FromText(md->DB->Name);
            case 3: return ROWOWScriptValue::FromNumber(MercenaryGetFaith(context.AttachedPlayer));
            case 4: return ROWOWScriptValue::FromNumber(MercenaryGetCalls(context.AttachedPlayer));
            case 5: return ROWOWScriptValue::FromNumber(md->KillCount);
            case 6: return ROWOWScriptValue::FromNumber(md->LifeTime);
            case 7: return ROWOWScriptValue::FromNumber(md->DB->Level);
            case 8: return ROWOWScriptValue::FromNumber(int64(md->CreatureGUID.GetCounter()));
            default: return ROWOWScriptValue::FromNumber(0);
        }
    });
}

// A homunculus's or mercenary's creature: no AI of its own, its master's update runs it (FollowerAI).  A click opens its window, for its master.  Its death is RO's: hom_dead, mercenary_dead
class ROWOW_FollowerCreatureScript : public CreatureScript
{
public:
    ROWOW_FollowerCreatureScript() : CreatureScript("ROWOW_FollowerCreatureScript") {}

    struct ROWOW_FollowerCreatureAI : public CreatureAI
    {
        explicit ROWOW_FollowerCreatureAI(Creature* creature) : CreatureAI(creature) {}

        void UpdateAI(uint32 /*diff*/) override { }
        void MoveInLineOfSight(Unit* /*who*/) override { }
        void AttackStart(Unit* /*who*/) override { }
        void EnterEvadeMode(EvadeReason /*why*/) override { }
        void JustDied(Unit* /*killer*/) override
        {
            if (ROWOW->IsEnabled == true)
                ROWOW->FollowerDead(me);
        }
    };

    CreatureAI* GetAI(Creature* creature) const override
    {
        return new ROWOW_FollowerCreatureAI(creature);
    }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        CloseGossipMenuFor(player);
        ROWOWFollowerOwner const* owner = creature->CustomData.Get<ROWOWFollowerOwner>("ROWOWFollowerOwner");
        if (ROWOW->IsEnabled == true && owner != nullptr && owner->OwnerGUID == player->GetGUID())
        {
            if (owner->FollowerType == ROWOW_BL_HOM)
                ROWOW->ClifHomInfo(player);
            else
                ROWOW->ClifMercenaryInfo(player);
            WorldPacket data;
            ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_ADDON, nullptr, nullptr, owner->FollowerType == ROWOW_BL_HOM ? "ROWOWFOLLOWER\tO\tH" : "ROWOWFOLLOWER\tO\tM");
            player->SendDirectMessage(&data);
        }
        return true;
    }
};

// The spot on the ground for a homunculus or mercenary: RO's Alt and a click, which the window asks the cursor for.  A move, or the ground skill waiting for it
class ROWOW_FollowerOrderSpellScript : public SpellScript
{
    PrepareSpellScript(ROWOW_FollowerOrderSpellScript);

    void HandleOrder(SpellEffIndex /*effIndex*/)
    {
        Player* player = GetCaster() != nullptr ? GetCaster()->ToPlayer() : nullptr;
        WorldLocation const* dest = GetExplTargetDest();
        if (player == nullptr || dest == nullptr || ROWOW->IsEnabled == false || ROWOW->IsInROForm(player) == false)
            return;
        ROWOW->FollowerOrderMove(player, ROWOW->GetIdentity(player)->FollowerOrderType, dest->GetPositionX(), dest->GetPositionY(), dest->GetPositionZ());
    }

    void Register() override
    {
        OnEffectHit += SpellEffectFn(ROWOW_FollowerOrderSpellScript::HandleOrder, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

void AddROWOWFollowerScripts()
{
    new ROWOW_FollowerCreatureScript();
    RegisterSpellScript(ROWOW_FollowerOrderSpellScript);
}
