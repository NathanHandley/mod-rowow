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
//  Ported from rAthena (https://github.com/rathena/rathena), src/map/status.cpp, pre-renewal paths:
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL.  For more information, see LICENCE in rAthena's main folder

#include "Chat.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Unit.h"

#include "ROWOW.h"
#include "ROWOW_Script.h"

using namespace std;

// RO status changes: rAthena's status_change_start (with status_get_sc_def's resistances and status_change_start_post_delay's checks and
// values), status_change_end, status_change_timer and status_change_clear, for the statuses of the pre-renewal jobs, items and monsters (the
// third jobs' and later statuses are left out: nothing pre-renewal starts them).  What a status stops (status.yml's States) is set on the
// unit as WoW's root, silence, pacify and stealth; its icon is a marker aura the converter made (mod_rowow_status_spell) whose duration is
// the status's.  A unit's statuses live on the unit and are only touched in its map's thread

namespace
{
    // status.yml's names for each of rAthena's constants, in their enums' order (without the prefix, upper case and without underscores, as
    // script_get_constant matches them)
    char const* const StateNames[ROWOW_SCS_MAX] = { "NONE", "NOMOVECOND", "NOMOVE", "NOPICKITEMCOND", "NOPICKITEM", "NODROPITEMCOND", "NODROPITEM",
        "NOCASTCOND", "NOCAST", "NOCHAT", "NOCHATCOND", "NOEQUIPITEM", "NOEQUIPITEMCOND", "NOUNEQUIPITEM", "NOUNEQUIPITEMCOND", "NOCONSUMEITEM",
        "NOCONSUMEITEMCOND", "NOATTACK", "NOATTACKCOND", "NOWARP", "NOWARPCOND", "NODEATHPENALTY", "NODEATHPENALTYCOND", "NOINTERACT", "NOINTERACTCOND" };
    char const* const CalcFlagNames[ROWOW_SCB_MAX] = { "NONE", "BASE", "MAXHP", "MAXSP", "STR", "AGI", "VIT", "INT", "DEX", "LUK", "BATK", "WATK", "MATK", "HIT",
        "FLEE", "DEF", "DEF2", "MDEF", "MDEF2", "SPEED", "ASPD", "DSPD", "CRI", "FLEE2", "ATKELE", "DEFELE", "MODE", "SIZE", "RACE", "RANGE", "REGEN", "DYE" };
    char const* const FlagNames[ROWOW_SCF_MAX] = { "NONE", "BLEFFECT", "DISPLAYPC", "NOCLEARBUFF", "NOREMOVEONDEAD", "NODISPELL", "NOCLEARANCE",
        "NOBANISHINGBUSTER", "NOSAVE", "NOSAVEINFINITE", "REMOVEONDAMAGED", "REMOVEONREFRESH", "REMOVEONLUXANIMA", "STOPATTACKING", "STOPCASTING",
        "STOPWALKING", "BOSSRESIST", "MVPRESIST", "SETSTAND", "FAILEDMADO", "DEBUFF", "REMOVEONCHANGEMAP", "REMOVEONMAPWARP", "REMOVECHEMICALPROTECT",
        "OVERLAPIGNORELEVEL", "SENDOPTION", "ONTOUCH", "UNITMOVE", "NONPLAYER", "SENDLOOK", "DISPLAYNPC", "REQUIREWEAPON", "REQUIRESHIELD", "MOBLOSETARGET",
        "REMOVEELEMENTALOPTION", "SUPERNOVICEANGEL", "TAEKWONANGEL", "MADOCANCEL", "MADOENDCANCEL", "RESTARTONMAPWARP", "SPREADEFFECT", "SENDVAL1",
        "SENDVAL2", "SENDVAL3", "NOFORCEDEND", "NOWARNING", "REMOVEONUNEQUIP", "REMOVEONUNEQUIPWEAPON", "REMOVEONUNEQUIPARMOR", "REMOVEONHERMODE",
        "REQUIRENOWEAPON", "REMOVEFROMHOMONWARP", "REMOVEFROMHOMONMAPWARP" };
    std::pair<char const*, uint32> const Opt1Names[] = { { "STONE", ROWOW_OPT1_STONE }, { "FREEZE", ROWOW_OPT1_FREEZE }, { "STUN", ROWOW_OPT1_STUN },
        { "SLEEP", ROWOW_OPT1_SLEEP }, { "STONEWAIT", ROWOW_OPT1_STONEWAIT }, { "BURNING", ROWOW_OPT1_BURNING }, { "IMPRISON", ROWOW_OPT1_IMPRISON } };
    std::pair<char const*, uint32> const Opt2Names[] = { { "POISON", 0x0001 }, { "CURSE", 0x0002 }, { "SILENCE", 0x0004 }, { "CONFUSION", 0x0008 },
        { "BLIND", 0x0010 }, { "ANGELUS", 0x0020 }, { "BLEEDING", 0x0040 }, { "DPOISON", 0x0080 }, { "FEAR", 0x0100 } };
    std::pair<char const*, uint32> const Opt3Names[] = { { "QUICKEN", 0x00000001 }, { "OVERTHRUST", 0x00000002 }, { "ENERGYCOAT", 0x00000004 },
        { "EXPLOSIONSPIRITS", 0x00000008 }, { "STEELBODY", 0x00000010 }, { "BLADESTOP", 0x00000020 }, { "AURABLADE", 0x00000040 },
        { "BERSERK", 0x00000080 }, { "LIGHTBLADE", 0x00000100 }, { "MOONLIT", 0x00000200 }, { "MARIONETTE", 0x00000400 }, { "ASSUMPTIO", 0x00000800 },
        { "WARM", 0x00001000 }, { "KAITE", 0x00002000 }, { "BUNSIN", 0x00004000 }, { "SOULLINK", 0x00008000 }, { "UNDEAD", 0x00010000 },
        { "CONTRACT", 0x00020000 } };
    std::pair<char const*, uint32> const OptionNames[] = { { "SIGHT", 0x00000001 }, { "HIDE", 0x00000002 }, { "CLOAK", 0x00000004 },
        { "CART1", 0x00000008 }, { "FALCON", 0x00000010 }, { "RIDING", 0x00000020 }, { "INVISIBLE", 0x00000040 }, { "CART2", 0x00000080 },
        { "CART3", 0x00000100 }, { "CART4", 0x00000200 }, { "CART5", 0x00000400 }, { "ORCISH", 0x00000800 }, { "WEDDING", 0x00001000 },
        { "RUWACH", 0x00002000 }, { "CHASEWALK", 0x00004000 }, { "FLYING", 0x00008000 }, { "XMAS", 0x00010000 }, { "TRANSFORM", 0x00020000 },
        { "SUMMER", 0x00040000 }, { "DRAGON1", 0x00080000 }, { "WUG", 0x00100000 }, { "WUGRIDER", 0x00200000 }, { "MADOGEAR", 0x00400000 },
        { "DRAGON2", 0x00800000 }, { "DRAGON3", 0x01000000 }, { "DRAGON4", 0x02000000 }, { "DRAGON5", 0x04000000 }, { "HANBOK", 0x08000000 },
        { "OKTOBERFEST", 0x10000000 }, { "SUMMER2", 0x20000000 } };

    // A name as script_get_constant compares it: upper case, and (as the converter's tables keep the yml keys) without underscores
    std::string GetConstantKey(std::string const& name)
    {
        std::string key;
        for (char curChar : name)
            if (curChar != '_')
                key.push_back(char(toupper((unsigned char)curChar)));
        return key;
    }

    int32 GetStatusTypeByName(std::string const& name)
    {
        std::string key = GetConstantKey(name);
        for (auto const& statusName : ROWOWStatusNames)
            if (GetConstantKey(statusName.first) == key)
                return statusName.second;
        return ROWOW_SC_NONE;
    }

    template<size_t N>
    int32 GetNameIndex(char const* const (&names)[N], std::string const& name)
    {
        std::string key = GetConstantKey(name);
        for (size_t index = 0; index < N; index++)
            if (key == GetConstantKey(names[index]))
                return int32(index);
        return -1;
    }

    template<size_t N>
    uint32 GetNamedValue(std::pair<char const*, uint32> const (&names)[N], std::string const& name)
    {
        std::string key = GetConstantKey(name);
        for (auto const& namedValue : names)
            if (key == namedValue.first)
                return namedValue.second;
        return 0;
    }

    // The statuses of a status.yml list (Fail, EndOnStart, EndReturn, EndOnEnd), in their order
    std::vector<int32> ParseStatusList(std::string const& cell)
    {
        std::vector<int32> types;
        for (auto const& field : ROWOWDataTable::ParseMap(cell))
        {
            int32 type = GetStatusTypeByName(field.first);
            if (type == ROWOW_SC_NONE)
                continue;
            if (field.second == "true")
            {
                if (std::find(types.begin(), types.end(), type) == types.end())
                    types.push_back(type);
            }
            else
                types.erase(std::remove(types.begin(), types.end(), type), types.end());
        }
        return types;
    }

    // status_get_sc_interval: how often a ticking status ticks
    int32 StatusGetSCInterval(int32 type)
    {
        switch (type)
        {
            case ROWOW_SC_POISON:
            case ROWOW_SC_LEECHESEND:
            case ROWOW_SC_DPOISON:
            case ROWOW_SC_DEATHHURT:
                return 1000;
            case ROWOW_SC_WINKCHARM:
                return 2250;
            case ROWOW_SC_BURNING:
            case ROWOW_SC_PYREXIA:
                return 3000;
            case ROWOW_SC_MAGICMUSHROOM:
                return 4000;
            case ROWOW_SC_STONE:
                return 5000;
            case ROWOW_SC_BLEEDING:
            case ROWOW_SC_TOXIN:
                return 10000;
            default:
                break;
        }
        return 0;
    }

    // status_get_lv
    int32 GetUnitLevel(ROWOWStatus const* status)
    {
        return status != nullptr ? int32(status->Level) : 1;
    }
}

void ROWOWMod::LoadStatusChangeData()
{
    // StatusDatabase::parseBodyNode and loadingFinished, from mod_rowow_db_status, then the client's marker aura of each status
    StatusChangeDBsByType.clear();
    if (IsEnabled == false)
        return;
    ROWOWDataTable const* statusTable = GetDataTable(ROWOW_DATA_TABLE_STATUS);
    if (statusTable == nullptr)
    {
        LOG_ERROR("module.ROWOW", "ROWOWMod::LoadStatusChangeData found no {} table, so RO statuses do nothing", ROWOW_DATA_TABLE_STATUS);
        return;
    }
    int32 statusColumnIndex = statusTable->GetColumnIndex("Status");
    int32 iconColumnIndex = statusTable->GetColumnIndex("Icon");
    int32 durationLookupColumnIndex = statusTable->GetColumnIndex("DurationLookup");
    int32 statesColumnIndex = statusTable->GetColumnIndex("States");
    int32 calcFlagsColumnIndex = statusTable->GetColumnIndex("CalcFlags");
    int32 optionsColumnIndex = statusTable->GetColumnIndex("Options");
    int32 opt3ColumnIndex = statusTable->GetColumnIndex("Opt3");
    int32 opt2ColumnIndex = statusTable->GetColumnIndex("Opt2");
    int32 opt1ColumnIndex = statusTable->GetColumnIndex("Opt1");
    int32 flagsColumnIndex = statusTable->GetColumnIndex("Flags");
    int32 minRateColumnIndex = statusTable->GetColumnIndex("MinRate");
    int32 minDurationColumnIndex = statusTable->GetColumnIndex("MinDuration");
    int32 endOnEndColumnIndex = statusTable->GetColumnIndex("EndOnEnd");
    int32 scriptColumnIndex = statusTable->GetColumnIndex("Script");
    int32 failColumnIndex = statusTable->GetColumnIndex("Fail");
    int32 endOnStartColumnIndex = statusTable->GetColumnIndex("EndOnStart");
    int32 endReturnColumnIndex = statusTable->GetColumnIndex("EndReturn");
    uint32 unknownCount = 0;
    uint32 scriptCount = 0;
    for (size_t rowIndex = 0; rowIndex < statusTable->Rows.size(); rowIndex++)
    {
        std::string const& statusName = statusTable->GetCell(rowIndex, statusColumnIndex);
        int32 type = GetStatusTypeByName(statusName);
        if (type == ROWOW_SC_NONE)
        {
            unknownCount++;
            continue;
        }
        ROWOWStatusChangeDB& scdb = StatusChangeDBsByType[type];
        scdb.Type = type;
        scdb.Name = statusName;
        scdb.Icon = statusTable->GetCell(rowIndex, iconColumnIndex);
        std::string const& durationLookup = statusTable->GetCell(rowIndex, durationLookupColumnIndex);
        scdb.SkillID = durationLookup.empty() == true ? 0 : GetSkillIDByName(durationLookup);
        for (auto const& field : ROWOWDataTable::ParseMap(statusTable->GetCell(rowIndex, statesColumnIndex)))
        {
            int32 state = GetNameIndex(StateNames, field.first);
            if (state > ROWOW_SCS_NONE)
                scdb.State.set(size_t(state), field.second == "true");
        }
        for (auto const& field : ROWOWDataTable::ParseMap(statusTable->GetCell(rowIndex, calcFlagsColumnIndex)))
        {
            if (GetConstantKey(field.first) == "ALL")
            {
                if (field.second == "true")
                {
                    scdb.CalcFlag.set();
                    scdb.CalcFlag.reset(ROWOW_SCB_DYE);
                }
                else
                    scdb.CalcFlag.reset();
                continue;
            }
            int32 calcFlag = GetNameIndex(CalcFlagNames, field.first);
            if (calcFlag > ROWOW_SCB_NONE)
                scdb.CalcFlag.set(size_t(calcFlag), field.second == "true");
        }
        for (auto const& field : ROWOWDataTable::ParseMap(statusTable->GetCell(rowIndex, flagsColumnIndex)))
        {
            int32 flag = GetNameIndex(FlagNames, field.first);
            if (flag > ROWOW_SCF_NONE)
                scdb.Flag.set(size_t(flag), field.second == "true");
        }
        auto readBits = [&statusTable, rowIndex](int32 columnIndex, auto const& names)
        {
            uint32 bits = 0;
            for (auto const& field : ROWOWDataTable::ParseMap(statusTable->GetCell(rowIndex, columnIndex)))
            {
                uint32 bit = GetNamedValue(names, field.first);
                if (field.second == "true")
                    bits |= bit;
                else
                    bits &= ~bit;
            }
            return bits;
        };
        scdb.Opt1 = GetNamedValue(Opt1Names, statusTable->GetCell(rowIndex, opt1ColumnIndex));
        scdb.Opt2 = readBits(opt2ColumnIndex, Opt2Names);
        scdb.Opt3 = readBits(opt3ColumnIndex, Opt3Names);
        scdb.Look = readBits(optionsColumnIndex, OptionNames);
        scdb.MinRate = int32(statusTable->GetInt(rowIndex, minRateColumnIndex, 0));
        scdb.MinDuration = statusTable->GetInt(rowIndex, minDurationColumnIndex, 1);
        scdb.Fail = ParseStatusList(statusTable->GetCell(rowIndex, failColumnIndex));
        scdb.EndOnStart = ParseStatusList(statusTable->GetCell(rowIndex, endOnStartColumnIndex));
        scdb.EndReturn = ParseStatusList(statusTable->GetCell(rowIndex, endReturnColumnIndex));
        scdb.EndOnEnd = ParseStatusList(statusTable->GetCell(rowIndex, endOnEndColumnIndex));
        std::string script = ROWOWDataTable::UnescapeText(statusTable->GetCell(rowIndex, scriptColumnIndex));
        if (script.empty() == false)
        {
            scdb.Script = ROWOWSCRIPT->Compile(script, "status " + statusName);
            if (scdb.Script->Error.empty() == false)
                LOG_ERROR("module.ROWOW", "ROWOWMod::LoadStatusChangeData: the script of status {} doesn't compile: {}", statusName, scdb.Script->Error);

            // loadingFinished: trigger recalculation for everything for now
            scdb.CalcFlag.set();
            scdb.CalcFlag.reset(ROWOW_SCB_DYE);
            scriptCount++;
        }
    }

    // The marker aura of each status the client shows, from the converter
    uint32 spellCount = 0;
    OptionIconSpellIDsByOption.clear();
    if (QueryResult result = WorldDatabase.Query("SELECT `Status`, `SpellID` FROM `mod_rowow_status_spell`"))
    {
        do
        {
            Field* fields = result->Fetch();
            std::string statusName = fields[0].Get<std::string>();
            uint32 spellID = fields[1].Get<uint32>();

            // The icons of the options RO shows one for (R9.5), which RefreshIllusionOptionLooks keeps
            if (statusName == "Option_Riding" || statusName == "Option_Falcon")
            {
                if (sSpellMgr->GetSpellInfo(spellID) != nullptr)
                    OptionIconSpellIDsByOption[statusName == "Option_Riding" ? ROWOW_OPTION_RIDING : ROWOW_OPTION_FALCON] = spellID;
                continue;
            }
            auto scdbIter = StatusChangeDBsByType.find(GetStatusTypeByName(statusName));
            if (scdbIter == StatusChangeDBsByType.end() || sSpellMgr->GetSpellInfo(spellID) == nullptr)
                continue;
            scdbIter->second.SpellID = spellID;
            spellCount++;
        } while (result->NextRow());
    }
    else
        LOG_ERROR("module.ROWOW", "ROWOWMod::LoadStatusChangeData found no mod_rowow_status_spell rows, so the client shows no RO status icons");
    LOG_INFO("module.ROWOW", "ROWOWMod::LoadStatusChangeData loaded {} RO statuses ({} with a script, {} with an icon, {} unknown to the module)", StatusChangeDBsByType.size(),
        scriptCount, spellCount, unknownCount);
}

ROWOWStatusChangeDB const* ROWOWMod::GetStatusChangeDB(int32 type) const
{
    auto scdbIter = StatusChangeDBsByType.find(type);
    return scdbIter != StatusChangeDBsByType.end() ? &scdbIter->second : nullptr;
}

uint16 ROWOWMod::StatusDBGetSkill(int32 type) const
{
    ROWOWStatusChangeDB const* scdb = GetStatusChangeDB(type);
    return scdb != nullptr ? scdb->SkillID : 0;
}

ROWOWStatusChange* ROWOWMod::GetStatusChange(Unit* unit) const
{
    return unit != nullptr ? unit->CustomData.Get<ROWOWStatusChange>("ROWOWStatusChange") : nullptr;
}

ROWOWStatusChange const* ROWOWMod::GetStatusChange(Unit const* unit) const
{
    return unit != nullptr ? const_cast<Unit*>(unit)->CustomData.Get<ROWOWStatusChange>("ROWOWStatusChange") : nullptr;
}

ROWOWStatusChangeEntry const* ROWOWMod::GetSCE(Unit const* unit, int32 type) const
{
    ROWOWStatusChange const* sc = GetStatusChange(unit);
    return sc != nullptr ? sc->GetSCE(type) : nullptr;
}

int32 ROWOWMod::StatusIsImmune(Unit const* bl) const
{
    // status_isimmune: Wand of Hermode, and a player's magic immunity from the Golden Thief Bug card past gtb_sc_immunity
    if (GetSCE(bl, ROWOW_SC_HERMODE) != nullptr)
        return 100;
    if (bl->IsPlayer() == true)
    {
        int32 noMagicDamage = GetDeferredBonus(bl, "bNoMagicDamage");
        if (noMagicDamage >= BattleConfig.GtbScImmunity)
            return noMagicDamage;
    }
    return 0;
}

bool ROWOWMod::StatusIsEndure(Unit const* bl) const
{
    // status_isendure: whether being hit can stop the unit (infinite_endure, bNoWalkDelay, SC_ENDURE, running)
    ROWOWStatus const* status = GetROStatus(bl);
    if (status != nullptr && (BattleConfig.InfiniteEndure & GetBattleObjectType(*status)) != 0)
        return true;
    if (bl->IsPlayer() == true && GetDeferredBonus(bl, "bNoWalkDelay") != 0)
        return true;
    ROWOWStatusChange const* sc = GetStatusChange(bl);
    return sc != nullptr && (sc->HasSCE(ROWOW_SC_ENDURE) == true || sc->HasSCE(ROWOW_SC_RUN) == true);
}

int64 ROWOWMod::StatusGetSCDef(Unit const* src, Unit const* bl, int32 type, int32 rate, int64 tick, uint8 flag) const
{
    // status_get_sc_def: the target's resistance to a status.  Resistance rate: 10000 = 100%.  sc_def lowers the chance by a rate and
    // sc_def2 by a fixed amount (after the rate); tick_def and tick_def2 the same for the duration (tick_def -1: use sc_def)
    int32 scDef = 0;
    int32 tickDef = -1;
    int32 scDef2 = 0;
    int32 tickDef2 = 0;
    if (src == nullptr)
        return tick != 0 ? tick : 1; // This should not happen in current implementation, but leave it anyway

    // Skills (magic type) that are blocked by Golden Thief Bug card or Wand of Hermode
    if (StatusIsImmune(bl) != 0)
    {
        ROWOWSkillUnitData const* ud = const_cast<Unit*>(src)->CustomData.Get<ROWOWSkillUnitData>("ROWOWSkillUnitData");
        ROWOWSkill const* skill = GetSkill(ud != nullptr ? ud->SkillID : 0);
        if (skill == nullptr) // Check for ground-type skills using the status when a player moves through units
            skill = GetSkill(StatusDBGetSkill(type));
        if (skill != nullptr && skill->AttackType == ROWOW_BF_MAGIC && skill->Inf2[ROWOW_INF2_IGNOREGTB] == false &&
            (skill->Inf == ROWOW_INF_ATTACK_SKILL || skill->Inf == ROWOW_INF_GROUND_SKILL || skill->Inf == ROWOW_INF_SUPPORT_SKILL || // Target skills should get blocked even when cast on self
            (skill->Inf == ROWOW_INF_SELF_SKILL && src != bl))) // Self skills should get blocked on all targets except self
            return 0;
    }

    Player const* sd = bl->ToPlayer();
    ROWOWStatus const* status = GetROStatus(bl);
    ROWOWStatus const* statusSrc = GetROStatus(src);
    if (status == nullptr || statusSrc == nullptr)
        return 0;
    ROWOWStatusChange const* sc = GetStatusChange(bl);
    if (sc != nullptr && sc->IsEmpty() == true)
        sc = nullptr;
    int32 const* stats = status->Stats;
    int32 blLevel = GetUnitLevel(status);
    int32 srcLevel = GetUnitLevel(statusSrc);

    switch (type)
    {
        case ROWOW_SC_POISON:
        case ROWOW_SC_DPOISON:
            scDef = stats[ROWOW_STAT_VIT] * 100;
            scDef2 = stats[ROWOW_STAT_LUK] * 10 + blLevel * 10 - srcLevel * 10;
            if (sd != nullptr)
            {
                // For players: 60000 - 450*vit - 100*luk
                tickDef = stats[ROWOW_STAT_VIT] * 75;
                tickDef2 = stats[ROWOW_STAT_LUK] * 100;
            }
            else
            {
                // For monsters: 30000 - 200*vit
                tick /= 2;
                tickDef = (stats[ROWOW_STAT_VIT] * 200) / 3;
            }
            break;
        case ROWOW_SC_STUN:
        case ROWOW_SC_SILENCE:
        case ROWOW_SC_BLEEDING:
            scDef = stats[ROWOW_STAT_VIT] * 100;
            scDef2 = stats[ROWOW_STAT_LUK] * 10 + blLevel * 10 - srcLevel * 10;
            tickDef2 = stats[ROWOW_STAT_LUK] * 10;
            break;
        case ROWOW_SC_SLEEP:
            scDef = stats[ROWOW_STAT_INT] * 100;
            scDef2 = stats[ROWOW_STAT_LUK] * 10 + blLevel * 10 - srcLevel * 10;
            tickDef2 = stats[ROWOW_STAT_LUK] * 10;
            break;
        case ROWOW_SC_STONEWAIT:
            scDef = status->MDef * 100;
            scDef2 = stats[ROWOW_STAT_LUK] * 10 + blLevel * 10 - srcLevel * 10;
            tickDef = 0; // No duration reduction
            break;
        case ROWOW_SC_FREEZE:
            scDef = status->MDef * 100;
            scDef2 = stats[ROWOW_STAT_LUK] * 10 + blLevel * 10 - srcLevel * 10;
            tickDef2 = statusSrc->Stats[ROWOW_STAT_LUK] * -10; // Caster can increase final duration with luk
            break;
        case ROWOW_SC_CURSE:
            // Special property: immunity when luk is zero
            if (stats[ROWOW_STAT_LUK] == 0)
                return 0;
            scDef = stats[ROWOW_STAT_LUK] * 100;
            scDef2 = stats[ROWOW_STAT_LUK] * 10 - srcLevel * 10; // Curse only has a level penalty and no resistance
            tickDef = stats[ROWOW_STAT_VIT] * 100;
            tickDef2 = stats[ROWOW_STAT_LUK] * 10;
            break;
        case ROWOW_SC_BLIND:
            scDef = (stats[ROWOW_STAT_VIT] + stats[ROWOW_STAT_INT]) * 50;
            scDef2 = stats[ROWOW_STAT_LUK] * 10 + blLevel * 10 - srcLevel * 10;
            tickDef2 = stats[ROWOW_STAT_LUK] * 10;
            break;
        case ROWOW_SC_CONFUSION:
            scDef = (stats[ROWOW_STAT_STR] + stats[ROWOW_STAT_INT]) * 50;
            scDef2 = srcLevel * 10 - blLevel * 10 - stats[ROWOW_STAT_LUK] * 10; // Reversed sc_def2
            tickDef2 = stats[ROWOW_STAT_LUK] * 10;
            break;
        case ROWOW_SC_DECREASEAGI:
            if (sd != nullptr)
                tick /= 2; // Half duration for players.
            scDef2 = status->MDef * 100;
            break;
        case ROWOW_SC_JOINTBEAT:
            tickDef2 = 1000 * ((stats[ROWOW_STAT_LUK] / 2 + stats[ROWOW_STAT_AGI] / 5) / 2); // (50 * LUK / 100 + 20 * AGI / 100) / 2
            break;
        case ROWOW_SC_TOXIN:
        case ROWOW_SC_PARALYSE:
        case ROWOW_SC_VENOMBLEED:
        case ROWOW_SC_MAGICMUSHROOM:
        case ROWOW_SC_DEATHHURT:
        case ROWOW_SC_PYREXIA:
        case ROWOW_SC_LEECHESEND:
            tickDef2 = (stats[ROWOW_STAT_VIT] + stats[ROWOW_STAT_LUK]) * 500;
            break;
        case ROWOW_SC_OBLIVIONCURSE: // 100% - (100 - 0.8 x INT)
            scDef = std::max(stats[ROWOW_STAT_INT] * 80, 500); // minimum of 5% resist
            tickDef = 0;
            tickDef2 = (stats[ROWOW_STAT_VIT] + stats[ROWOW_STAT_LUK]) * 500;
            break;
        default:
            // Effect that cannot be reduced? Likely a buff.
            if (!(int32(urand(0, 9999)) < rate))
                return 0;
            return tick != 0 ? tick : 1;
    }

    if (sd != nullptr)
    {
        if (BattleConfig.PCScDefRate != 100)
        {
            scDef = scDef * BattleConfig.PCScDefRate / 100;
            scDef2 = scDef2 * BattleConfig.PCScDefRate / 100;
        }
        scDef = std::min(scDef, BattleConfig.PCMaxScDef * 100);
        scDef2 = std::min(scDef2, BattleConfig.PCMaxScDef * 100);
        if (BattleConfig.PCScDefRate != 100)
        {
            tickDef = tickDef * BattleConfig.PCScDefRate / 100;
            tickDef2 = tickDef2 * BattleConfig.PCScDefRate / 100;
        }
    }
    else
    {
        if (BattleConfig.MobScDefRate != 100)
        {
            scDef = scDef * BattleConfig.MobScDefRate / 100;
            scDef2 = scDef2 * BattleConfig.MobScDefRate / 100;
        }
        scDef = std::min(scDef, BattleConfig.MobMaxScDef * 100);
        scDef2 = std::min(scDef2, BattleConfig.MobMaxScDef * 100);
        if (BattleConfig.MobScDefRate != 100)
        {
            tickDef = tickDef * BattleConfig.MobScDefRate / 100;
            tickDef2 = tickDef2 * BattleConfig.MobScDefRate / 100;
        }
    }

    if (sc != nullptr)
    {
        if (ROWOWStatusChangeEntry const* sce = sc->GetSCE(ROWOW_SC_SCRESIST))
            scDef += sce->Val1 * 100; // Status resist
        else if (ROWOWStatusChangeEntry const* siegfried = sc->GetSCE(ROWOW_SC_SIEGFRIED))
            scDef += siegfried->Val3 * 100; // Status resistance.
        else if (ROWOWStatusChangeEntry const* leechesEnd = sc->GetSCE(ROWOW_SC_LEECHESEND); leechesEnd != nullptr && leechesEnd->Val3 == 0)
        {
            if (type == ROWOW_SC_BLIND || type == ROWOW_SC_STUN)
                return 0; // Immune
        }
        else if (ROWOWStatusChangeEntry const* oblivion = sc->GetSCE(ROWOW_SC_OBLIVIONCURSE); oblivion != nullptr && oblivion->Val3 == 0)
        {
            if (type == ROWOW_SC_SILENCE || type == ROWOW_SC_CURSE)
                return 0; // Immune
        }
    }

    // When tick def not set, reduction is the same for both.
    if (tickDef == -1)
        tickDef = scDef;

    // Natural resistance
    if ((flag & ROWOW_SCSTART_NORATEDEF) == 0)
    {
        rate -= rate * scDef / 10000;
        rate -= scDef2;

        // Item resistance (only applies to rate%)
        if (sd != nullptr)
        {
            auto resEffIter = status->Bonus.ResEff.find(type);
            if (resEffIter != status->Bonus.ResEff.end())
                rate -= rate * resEffIter->second / 10000;
        }

        // Aegis accuracy
        if (rate > 0 && rate % 10 != 0)
            rate += (10 - rate % 10);
    }

    // Cap minimum rate
    ROWOWStatusChangeDB const* scdb = GetStatusChangeDB(type);
    int64 minDuration = scdb != nullptr ? scdb->MinDuration : 1;
    rate = std::max(rate, scdb != nullptr ? scdb->MinRate : 0);
    if (rate < 10000 && (rate <= 0 || !(int32(urand(0, 9999)) < rate)))
        return 0;

    // Duration cannot be reduced
    if ((flag & ROWOW_SCSTART_NOTICKDEF) != 0)
        return std::max<int64>(tick, minDuration);
    tick -= tick * tickDef / 10000;
    tick -= tickDef2;
    return std::max<int64>(tick, minDuration);
}

bool ROWOWMod::StatusChangeStart(Unit* src, Unit* bl, int32 type, int32 rate, int32 val1, int32 val2, int32 val3, int32 val4, int64 duration, uint8 flag, int32 delay)
{
    // status_change_start: the checks of the status's own rules and the target's resistances, then (after the delay, as an event on the
    // target) status_change_start_post_delay.  rate is in hundredths of a percent (sc_start's rate x 100)
    ROWOWStatusChangeDB const* scdb = GetStatusChangeDB(type);
    if (bl == nullptr || scdb == nullptr)
        return false;
    ROWOWStatus const* status = GetROStatus(bl);
    if (status == nullptr)
        return false; // Unable to receive status changes

    // Scripted status changes only work for players for the time being
    if (scdb->Script != nullptr && bl->IsPlayer() == false)
        return false;
    if (bl->IsAlive() == false)
        return false;
    ROWOWStatusChange* sc = bl->CustomData.GetDefault<ROWOWStatusChange>("ROWOWStatusChange");

    // Check for Boss resistances
    if ((status->Mode & ROWOW_MODE_STATUS_IMMUNE) != 0 && (flag & ROWOW_SCSTART_NOAVOID) == 0 && scdb->Flag[ROWOW_SCF_BOSSRESIST] == true)
        return false;

    // Check for MVP resistance
    if ((status->Mode & ROWOW_MODE_MVP) != 0 && (flag & ROWOW_SCSTART_NOAVOID) == 0 && scdb->Flag[ROWOW_SCF_MVPRESIST] == true)
        return false;

    // End the SCs from the list and immediately return.  If anything in this list is removed, the rest is ignored.
    if (scdb->EndReturn.empty() == false)
    {
        bool isRemoved = false;
        for (int32 removeType : scdb->EndReturn)
        {
            if (sc->HasSCE(removeType) == true)
            {
                StatusChangeEnd(bl, removeType);
                isRemoved = true;
            }
        }
        if (isRemoved == true) // Something was removed, don't give the status
            return true; // Return true so that sc_start can be checked as success
    }

    // Check failing SCs from list.  Don't let OPT1 that have RemoveOnDamaged start a new effect in the same attack.
    for (int32 failType : scdb->Fail)
        if (sc->HasSCE(failType) == true || sc->LastEffect == failType)
            return false;

    // Adjust tick according to status resistances
    if ((flag & (ROWOW_SCSTART_NOAVOID | ROWOW_SCSTART_LOADED)) == 0)
    {
        duration = StatusGetSCDef(src, bl, type, rate, duration, flag);
        if (duration == 0)
            return false;
    }
    int32 tick = int32(std::clamp<int64>(duration, INT32_MIN, INT32_MAX));

    // Type-specific checks that need to happen before the delay
    switch (type)
    {
        case ROWOW_SC_STONE:
        case ROWOW_SC_STONEWAIT:
        case ROWOW_SC_FREEZE:
            if ((flag & ROWOW_SCSTART_NOAVOID) == 0)
            {
                // These status changes fail on any opt1 status change, usually specified in status.yml.  Undead race has opt1=undead
                // without a related status change, so we need to hardcode it here
                if (BattleConfig.UndeadDetectType == 0 && status->Race == ROWOW_RACE_UNDEAD)
                    return false;
                // Undead are immune to Freeze/Stone
                if (BattleCheckUndead(status->Race, status->DefElement) == true)
                    return false;
            }
            if (type == ROWOW_SC_STONEWAIT)
            {
                // Stonewait has a unique handling where the delay is actually the duration until stone kicks in
                val3 = std::max<int32>(1, tick - delay); // Petrify time
                tick = delay;
                delay = 0;
            }
            break;
        case ROWOW_SC_BLEEDING:
            // Bleeding always starts immediately
            delay = 0;
            break;
        case ROWOW_SC_BURNING:
            // Level 2 Fire Element is immune
            if (status->DefElement == ROWOW_ELEMENT_FIRE && status->ElementLevel == 2)
                return false;
            break;
    }

    // If there is no delay, we proceed immediately.  Otherwise the start waits for the delay (status_change_start_timer), as an event of
    // the target's own, which looks the source up again
    if (delay <= 0)
        return StatusChangeStartPostDelay(src, bl, type, val1, val2, val3, val4, tick, flag);
    ObjectGuid srcGUID = src != nullptr ? src->GetGUID() : ObjectGuid::Empty;
    bl->m_Events.AddEventAtOffset([this, srcGUID, bl, type, val1, val2, val3, val4, tick, flag]()
    {
        Unit* delayedSrc = srcGUID.IsEmpty() == true ? nullptr : ObjectAccessor::GetUnit(*bl, srcGUID);
        if (bl->IsInWorld() == true && bl->IsAlive() == true)
            StatusChangeStartPostDelay(delayedSrc, bl, type, val1, val2, val3, val4, tick, flag);
    }, Milliseconds(delay));

    // Assume success
    return true;
}

bool ROWOWMod::StatusChangeStartPostDelay(Unit* src, Unit* bl, int32 type, int32 val1, int32 val2, int32 val3, int32 val4, int32 tick, uint8 flag)
{
    // status_change_start_post_delay: applies the status and runs everything that comes with it
    Player* sd = bl->ToPlayer();
    ROWOWStatusChange* sc = bl->CustomData.GetDefault<ROWOWStatusChange>("ROWOWStatusChange");
    ROWOWStatus const* status = GetROStatus(bl);
    ROWOWStatusChangeDB const* scdb = GetStatusChangeDB(type);
    if (status == nullptr || scdb == nullptr)
        return false;
    bool undeadFlag = BattleCheckUndead(status->Race, status->DefElement);

    // Check for immunities / sc fails
    switch (type)
    {
        case ROWOW_SC_SIGNUMCRUCIS:
            // Only affects demons and undead element (but not players)
            if ((undeadFlag == false && status->Race != ROWOW_RACE_DEMON) || bl->IsPlayer() == true)
                return false;
            break;
        case ROWOW_SC_KYRIE:
            if (bl->IsCreature() == true)
                return false;
            break;
        case ROWOW_SC_ADRENALINE:
        case ROWOW_SC_ADRENALINE2:
        {
            // pc_check_weapontype with the skill's weapons
            ROWOWSkill const* skill = GetSkill(type == ROWOW_SC_ADRENALINE ? ROWOW_SKILL_BS_ADRENALINE : ROWOW_SKILL_BS_ADRENALINE2);
            if (sd != nullptr && skill != nullptr && skill->RequiredWeapons != 0 && (skill->RequiredWeapons & (1u << status->WeaponType)) == 0 &&
                (skill->RequiredWeapons & (1u << status->RightWeaponType)) == 0)
                return false;
            break;
        }
        case ROWOW_SC_CLOAKING:
            // Avoid cloaking with no wall and low skill level. [Skotlex]  Due to the cloaking card, we have to check the wall versus to known
            // skill level rather than the used one. [Skotlex]
            if (sd != nullptr && PCCheckSkill(sd, ROWOW_SKILL_AS_CLOAKING) < 3 && SkillCheckCloaking(bl) == false)
                return false;
            break;
        case ROWOW_SC_MODECHANGE:
        {
            ROWOWStatus const* bstatus = GetROBaseStatus(bl);
            if (bstatus == nullptr)
                return false;
            if (ROWOWStatusChangeEntry const* sce = sc->GetSCE(type))
            {
                // Pile up with previous values.
                if (val2 == 0)
                    val2 = sce->Val2;
                val3 |= sce->Val3;
                val4 |= sce->Val4;
            }
            uint32 mode = val2 != 0 ? uint32(val2) : bstatus->Mode; // Base mode
            if (val4 != 0)
                mode &= ~uint32(val4); // Del mode
            if (val3 != 0)
                mode |= uint32(val3); // Add mode
            if (mode == bstatus->Mode)
            {
                // No change.
                if (sc->HasSCE(type) == true) // Abort previous status
                    return StatusChangeEnd(bl, type);
                return false;
            }
            break;
        }
        // Strip skills, need to divest something or it fails.
        case ROWOW_SC_STRIPWEAPON:
            if (val2 == 1)
                val2 = 0; // Brandish Spear/Bowling Bash effet. Do not take weapon off.
            else if (sd != nullptr && (flag & ROWOW_SCSTART_LOADED) == 0)
            {
                // Apply sc anyway if loading saved sc_data
                if (GetDeferredBonus(sd, "bUnstripableWeapon") != 0 || GetDeferredBonus(sd, "bUnstripable") != 0)
                    return false;
                uint8 successFlag = 0;
                Item* leftItem = GetIdentityEquippedItem(sd, ROWOW_EQUIP_POSITION_HAND_L);
                ROWOWItem const* leftROItem = GetROItem(leftItem);
                if (leftROItem != nullptr && leftROItem->IsWeapon == true)
                {
                    successFlag |= 1;
                    PCUnequipItem(sd, ROWOW_EQUIP_POSITION_HAND_L, 3); // Left-hand weapon
                }
                Item* rightItem = GetIdentityEquippedItem(sd, ROWOW_EQUIP_POSITION_HAND_R);
                ROWOWItem const* rightROItem = GetROItem(rightItem);
                if (rightROItem != nullptr && rightROItem->IsWeapon == true)
                {
                    successFlag |= 2;
                    PCUnequipItem(sd, ROWOW_EQUIP_POSITION_HAND_R, 3);
                }
                if (successFlag == 0)
                    return false;
            }
            if (tick == 1)
                return true; // Minimal duration: Only strip without causing the SC
            break;
        case ROWOW_SC_STRIPSHIELD:
            if (val2 == 1)
                val2 = 0; // GX effect. Do not take shield off..
            else if (sd != nullptr && (flag & ROWOW_SCSTART_LOADED) == 0)
            {
                if (GetDeferredBonus(sd, "bUnstripableShield") != 0 || GetDeferredBonus(sd, "bUnstripable") != 0)
                    return false;
                ROWOWItem const* leftROItem = GetROItem(GetIdentityEquippedItem(sd, ROWOW_EQUIP_POSITION_HAND_L));
                if (leftROItem == nullptr || leftROItem->IsArmor == false)
                    return false;
                PCUnequipItem(sd, ROWOW_EQUIP_POSITION_HAND_L, 3);
            }
            if (tick == 1)
                return true; // Minimal duration: Only strip without causing the SC
            break;
        case ROWOW_SC_STRIPARMOR:
        case ROWOW_SC_STRIPHELM:
            if (sd != nullptr && (flag & ROWOW_SCSTART_LOADED) == 0)
            {
                bool isArmor = type == ROWOW_SC_STRIPARMOR;
                if (GetDeferredBonus(sd, isArmor == true ? "bUnstripableArmor" : "bUnstripableHelm") != 0 || GetDeferredBonus(sd, "bUnstripable") != 0)
                    return false;
                uint32 position = isArmor == true ? ROWOW_EQUIP_POSITION_ARMOR : ROWOW_EQUIP_POSITION_HEAD_TOP;
                if (GetIdentityEquippedItem(sd, position) == nullptr)
                    return false;
                PCUnequipItem(sd, position, 3);
            }
            if (tick == 1)
                return true; // Minimal duration: Only strip without causing the SC
            break;
        case ROWOW_SC_MERC_FLEEUP:
        case ROWOW_SC_MERC_ATKUP:
        case ROWOW_SC_MERC_HPUP:
        case ROWOW_SC_MERC_SPUP:
        case ROWOW_SC_MERC_HITUP:
            if (GetFollowerType(bl) != ROWOW_BL_MER)
                return false; // Stats only for Mercenaries
            break;
        case ROWOW_SC_STRFOOD:
        case ROWOW_SC_AGIFOOD:
        case ROWOW_SC_VITFOOD:
        case ROWOW_SC_INTFOOD:
        case ROWOW_SC_DEXFOOD:
        case ROWOW_SC_LUKFOOD:
        {
            int32 cashType = ROWOW_SC_FOOD_STR_CASH + (type - ROWOW_SC_STRFOOD);
            if (ROWOWStatusChangeEntry const* cashSce = sc->GetSCE(cashType); cashSce != nullptr && cashSce->Val1 > val1)
                return false;
            break;
        }
        case ROWOW_SC_FOOD_STR_CASH:
        case ROWOW_SC_FOOD_AGI_CASH:
        case ROWOW_SC_FOOD_VIT_CASH:
        case ROWOW_SC_FOOD_INT_CASH:
        case ROWOW_SC_FOOD_DEX_CASH:
        case ROWOW_SC_FOOD_LUK_CASH:
        {
            int32 foodType = ROWOW_SC_STRFOOD + (type - ROWOW_SC_FOOD_STR_CASH);
            if (ROWOWStatusChangeEntry const* foodSce = sc->GetSCE(foodType); foodSce != nullptr && foodSce->Val1 > val1)
                return false;
            break;
        }
        case ROWOW_SC_TOXIN:
        case ROWOW_SC_PARALYSE:
        case ROWOW_SC_VENOMBLEED:
        case ROWOW_SC_MAGICMUSHROOM:
        case ROWOW_SC_DEATHHURT:
        case ROWOW_SC_PYREXIA:
        case ROWOW_SC_OBLIVIONCURSE:
        case ROWOW_SC_LEECHESEND:
            if (val3 == 0) // Don't display icon on self
                flag |= ROWOW_SCSTART_NOICON;
            for (int32 i = ROWOW_SC_TOXIN; i <= ROWOW_SC_LEECHESEND; i++)
            {
                ROWOWStatusChangeEntry const* otherSce = sc->GetSCE(i);
                if (otherSce != nullptr && otherSce->Val3 == 1) // It doesn't stack or even renew on the target
                    return false;
                else if (otherSce != nullptr && otherSce->Val3 == 0)
                    StatusChangeEnd(bl, i); // End the bonus part on the caster
            }
            break;
        case ROWOW_SC_SPIRIT:
        {
            // A spirit links only the job it's of (its second job, transcendent or not; Bard and Dancer share one).  The Spirit of Rebirth
            // links a transcendent first job below base level 70
            if (sd == nullptr)
                return false; // Status change is only applicable for players
            ROWOWIdentity const* identity = GetIdentity(sd);
            int32 jobID = GetNonBabyJobID(identity->JobID); // MAPID_SECONDMASK leaves out JOBL_BABY: a Baby is linked as its job
            int32 secondJobID = jobID >= ROWOW_JOB_UPPER_START && jobID <= ROWOW_JOB_UPPER_END ? jobID - ROWOW_JOB_UPPER_START + 1 : jobID; // MAPID_SECONDMASK
            int32 targetClass = -1;
            switch (val2)
            {
                case ROWOW_SKILL_SL_ALCHEMIST: targetClass = 18; break;
                case ROWOW_SKILL_SL_ASSASIN: targetClass = 12; break;
                case ROWOW_SKILL_SL_BARDDANCER: targetClass = secondJobID == ROWOW_JOB_DANCER ? ROWOW_JOB_DANCER : ROWOW_JOB_BARD; break; // MAPID_BARDDANCER
                case ROWOW_SKILL_SL_BLACKSMITH: targetClass = 10; break;
                case ROWOW_SKILL_SL_CRUSADER: targetClass = 14; break;
                case ROWOW_SKILL_SL_HUNTER: targetClass = 11; break;
                case ROWOW_SKILL_SL_KNIGHT: targetClass = 7; break;
                case ROWOW_SKILL_SL_MONK: targetClass = ROWOW_JOB_MONK; break;
                case ROWOW_SKILL_SL_PRIEST: targetClass = 8; break;
                case ROWOW_SKILL_SL_ROGUE: targetClass = 17; break;
                case ROWOW_SKILL_SL_SAGE: targetClass = 16; break;
                case ROWOW_SKILL_SL_SOULLINKER: targetClass = ROWOW_JOB_SOUL_LINKER; break;
                case ROWOW_SKILL_SL_STAR: targetClass = ROWOW_JOB_STAR_GLADIATOR; break;
                case ROWOW_SKILL_SL_SUPERNOVICE: targetClass = ROWOW_JOB_SUPER_NOVICE; break;
                case ROWOW_SKILL_SL_WIZARD: targetClass = 9; break;
                case ROWOW_SKILL_SL_HIGH:
                    if (identity->BaseLevel >= 70)
                        return false;
                    if (jobID < ROWOW_JOB_UPPER_START + 1 || jobID > ROWOW_JOB_UPPER_START + 6)
                        return false; // Only the transcendent first jobs (Swordman_High to Thief_High) are allowed.
                    secondJobID = jobID; // Set these to pass the check below.
                    targetClass = jobID;
                    break;
                default:
                    LOG_ERROR("module.ROWOW", "Unknown skill id {} for SC_SPIRIT.", val2);
                    return false;
            }
            if (secondJobID != targetClass)
                return false;
            break;
        }
    }

    // Check for OPT1 stacking
    if (sc->Opt1 > ROWOW_OPT1_NONE && scdb->Opt1 > ROWOW_OPT1_NONE)
    {
        std::vector<int32> opt1Types;
        for (auto const& entryByType : sc->EntriesByType)
        {
            ROWOWStatusChangeDB const* otherScdb = GetStatusChangeDB(entryByType.first);
            if (otherScdb != nullptr && otherScdb->Opt1 > ROWOW_OPT1_NONE)
                opt1Types.push_back(entryByType.first);
        }
        for (int32 opt1Type : opt1Types)
            StatusChangeEnd(bl, opt1Type);
    }

    // Before overlapping fail, one must check for status cured.  End the SCs from the list
    std::vector<int32> endList = scdb->EndOnStart;
    if (type == ROWOW_SC_BERSERK && val3 == ROWOW_SC__BLOODYLUST) // There is some reasons that using SC_BERSERK first before SC__BLOODYLUST itself on Akinari's fix
        if (ROWOWStatusChangeDB const* bloodyLust = GetStatusChangeDB(ROWOW_SC__BLOODYLUST))
            endList = bloodyLust->EndOnStart;
    for (int32 removeType : endList)
    {
        ROWOWStatusChangeEntry* removeSce = sc->GetSCE(removeType);
        if (removeSce == nullptr)
            continue;
        if (removeType == ROWOW_SC_BERSERK)
            removeSce->Val2 = 0; // Mark to not lose hp
        StatusChangeEnd(bl, removeType);
    }

    // List of hardcoded status cured.
    switch (type)
    {
        case ROWOW_SC_BLESSING:
            if (bl->IsPlayer() == true)
            {
                // Remove Curse first, Stone is only removed if the target is not cursed
                if (sc->HasSCE(ROWOW_SC_CURSE) == true)
                {
                    StatusChangeEnd(bl, ROWOW_SC_CURSE);
                    return true; // End Curse and do not give stat boost
                }
                else if (sc->HasSCE(ROWOW_SC_STONE) == true)
                {
                    StatusChangeEnd(bl, ROWOW_SC_STONE);
                    return true; // End Stone and do not give stat boost
                }
            }
            if (ROWOWStatusChangeEntry const* spirit = sc->GetSCE(ROWOW_SC_SPIRIT); spirit != nullptr && spirit->Val2 == ROWOW_SKILL_SL_HIGH)
                StatusChangeEnd(bl, ROWOW_SC_SPIRIT);
            break;
        case ROWOW_SC_INCREASEAGI:
            if (ROWOWStatusChangeEntry const* spirit = sc->GetSCE(ROWOW_SC_SPIRIT); spirit != nullptr && spirit->Val2 == ROWOW_SKILL_SL_HIGH)
                StatusChangeEnd(bl, ROWOW_SC_SPIRIT);
            break;
        case ROWOW_SC_DELUGE:
            if (sc->HasSCE(ROWOW_SC_FOGWALL) == true && sc->HasSCE(ROWOW_SC_BLIND) == true)
                StatusChangeEnd(bl, ROWOW_SC_BLIND);
            break;
        case ROWOW_SC_SILENCE:
            if (ROWOWStatusChangeEntry const* gospel = sc->GetSCE(ROWOW_SC_GOSPEL); gospel != nullptr && gospel->Val4 == ROWOW_BCT_SELF)
                StatusChangeEnd(bl, ROWOW_SC_GOSPEL);
            break;
        case ROWOW_SC_IMPOSITIO:
            if (ROWOWStatusChangeEntry const* impositio = sc->GetSCE(ROWOW_SC_IMPOSITIO); impositio != nullptr && impositio->Val1 > val1) // Replace higher level effect for lower.
                StatusChangeEnd(bl, ROWOW_SC_IMPOSITIO);
            break;
        case ROWOW_SC_ENDURE:
            if (sd != nullptr && GetDeferredBonus(sd, "bNoWalkDelay") != 0)
                return true;
            break;
        default:
            break;
    }

    // Check for overlapping fails
    if (ROWOWStatusChangeEntry* sce = sc->GetSCE(type))
    {
        switch (type)
        {
            case ROWOW_SC_ADRENALINE:
            case ROWOW_SC_ADRENALINE2:
            case ROWOW_SC_WEAPONPERFECTION:
            case ROWOW_SC_OVERTHRUST:
                if (sce->Val2 > val2)
                    return false;
                break;
            case ROWOW_SC_GOSPEL:
                // Must not override a casting gospel char.
                if (sce->Val4 == ROWOW_BCT_SELF)
                    return false;
                if (sce->Val1 > val1)
                    return true;
                break;
            case ROWOW_SC_ENDURE:
                if (sce->Val4 != 0 && val4 == 0)
                    return true; // Don't let you override infinite endure.
                if (sce->Val1 > val1)
                    return true;
                break;
            case ROWOW_SC_LEADERSHIP:
            case ROWOW_SC_GLORYWOUNDS:
            case ROWOW_SC_SOULCOLD:
            case ROWOW_SC_HAWKEYES:
                if (sce->Val4 != 0 && val4 == 0) // You cannot override master guild aura
                    return false;
                break;
            case ROWOW_SC_JOINTBEAT:
                if ((sce->Val2 & ROWOW_BREAK_NECK) != 0)
                    return false; // BREAK_NECK cannot be stacked with new breaks until the status is over.
                val2 |= sce->Val2; // Stackable ailments
                [[fallthrough]];
            default:
                if (scdb->Flag[ROWOW_SCF_OVERLAPIGNORELEVEL] == true)
                    break;
                if (sce->Val1 > val1)
                    return true; // Return true to not mess up skill animations. [Skotlex]
        }
    }

    int32 tickTime = 0;
    if ((flag & ROWOW_SCSTART_LOADED) == 0) // &4 - Do not parse val settings when loading SCs
    {
        if (StatusChangeStartValues(src, bl, type, val1, val2, val3, val4, tick, tickTime, flag) == false)
            return false;
    }
    else
    {
        // Special considerations when loading SC data.
        switch (type)
        {
            case ROWOW_SC_STONE:
            case ROWOW_SC_STONEWAIT:
            case ROWOW_SC_POISON:
            case ROWOW_SC_DPOISON:
            case ROWOW_SC_BLEEDING:
            case ROWOW_SC_BURNING:
            case ROWOW_SC_TOXIN:
                tickTime = tick;
                tick = tickTime + std::max(val4, 0);
                break;
            case ROWOW_SC_DEATHHURT:
                if (val3 == 1)
                    break;
                tickTime = tick;
                tick = tickTime + std::max(val4, 0);
                [[fallthrough]];
            case ROWOW_SC_MAGICMUSHROOM:
            case ROWOW_SC_PYREXIA:
            case ROWOW_SC_LEECHESEND:
                if (val3 == 0)
                    break;
                tickTime = tick;
                tick = tickTime + std::max(val4, 0);
                break;
        }
    }

    // SC that force player to stand if is sitting
    if (scdb->Flag[ROWOW_SCF_SETSTAND] == true && sd != nullptr && sd->IsSitState() == true)
        sd->SetStandState(UNIT_STAND_STATE_STAND);

    // SC that make stop attacking [LuzZza]
    if (scdb->Flag[ROWOW_SCF_STOPATTACKING] == true)
        bl->AttackStop();

    // SC that make stop walking (unit_stop_walking; RO's knockback immune don't stop)
    if (scdb->Flag[ROWOW_SCF_STOPWALKING] == true)
    {
        switch (type)
        {
            case ROWOW_SC_FREEZE:
            case ROWOW_SC_STUN:
            case ROWOW_SC_STONE:
                if (sc->HasSCE(ROWOW_SC_DANCING) == true)
                {
                    bl->StopMoving();
                    StatusChangeEnd(bl, ROWOW_SC_DANCING);
                }
                break;
            default:
                if (bl->IsCreature() == false || (status->Mode & ROWOW_MODE_KNOCKBACK_IMMUNE) == 0)
                    bl->StopMoving();
                break;
        }
    }

    // SC that make stop casting
    if ((BattleConfig.ScCastCancel & GetBattleObjectType(*status)) != 0 && scdb->Flag[ROWOW_SCF_STOPCASTING] == true)
        UnitSkillCastCancel(bl, 0);

    if (scdb->Opt1 != ROWOW_OPT1_NONE)
        sc->Opt1 = scdb->Opt1;
    sc->Opt2 |= scdb->Opt2;
    sc->Opt3 |= scdb->Opt3;
    sc->Option |= scdb->Look;

    // The duration the icon shows; scs with interval ticks keep the interval in tick_time, so that the actual duration is sent first
    uint64 now = GetTick();
    uint64 endTime = tick >= 0 ? now + uint64(tick) : 0;
    if (tickTime != 0)
        tick = tickTime;

    bool isNew = sc->HasSCE(type) == false;
    ROWOWStatusChangeEntry& sce = sc->EntriesByType[type];
    sce.Val1 = val1;
    sce.Val2 = val2;
    sce.Val3 = val3;
    sce.Val4 = val4;
    sce.TimerTime = tick >= 0 ? now + uint64(tick) : 0; // Infinite duration has no timer
    sce.EndTime = endTime;
    sce.SourceGUID = src != nullptr ? src->GetGUID() : ObjectGuid::Empty;
    if ((flag & ROWOW_SCSTART_NOICON) == 0)
        ShowStatusChangeIcon(bl, *scdb, sce);

    std::bitset<ROWOW_SCB_MAX> calcFlag = scdb->CalcFlag;
    calcFlag.reset(ROWOW_SCB_DYE);
    if (calcFlag.any() == true)
        StatusCalcBL(bl, calcFlag);

    // Non-zero
    if (isNew == true && scdb->State.any() == true)
        StatusCalcState(bl, *sc, *scdb, true);
    UpdateStatusChangeOptions(bl, *sc);
    if (sd != nullptr)
        StatusChangeSaveNeeded(sd);

    // 1st thing to execute when loading status
    switch (type)
    {
        case ROWOW_SC_BERSERK:
        {
            ROWOWStatusChangeEntry* berserk = sc->GetSCE(type);
            ROWOWStatus const* battleStatus = GetROStatus(bl);
            if (berserk->Val2 == 0)
            {
                // Don't heal if already set.  Do not use percent_heal as this healing must override BERSERK's block.
                StatusHeal(bl, battleStatus->MaxHP, 0, 1, bl, 0);
                bl->SetPower(POWER_MANA, 0); // Damage all SP
            }
            berserk->Val2 = int32(5 * battleStatus->MaxHP / 100);
            break;
        }
        case ROWOW_SC_MERC_HPUP:
            bl->SetHealth(bl->GetMaxHealth()); // Recover Full HP
            break;
        case ROWOW_SC_MERC_SPUP:
            bl->SetPower(POWER_MANA, bl->GetMaxPower(POWER_MANA)); // Recover Full SP
            break;
        case ROWOW_SC_RUN:
            GetSkillUnitData(bl)->IsRunning = UnitRun(bl);
            break;
        case ROWOW_SC_COMBO:
            // A Taekwon's kick readied: its stance's look (clif_skill_nodamage)
            switch (sc->GetSCE(type)->Val1)
            {
                case ROWOW_SKILL_TK_STORMKICK: ClifSkillNoDamage(bl, bl, ROWOW_SKILL_TK_READYSTORM, 1); break;
                case ROWOW_SKILL_TK_DOWNKICK: ClifSkillNoDamage(bl, bl, ROWOW_SKILL_TK_READYDOWN, 1); break;
                case ROWOW_SKILL_TK_TURNKICK: ClifSkillNoDamage(bl, bl, ROWOW_SKILL_TK_READYTURN, 1); break;
                case ROWOW_SKILL_TK_COUNTER: ClifSkillNoDamage(bl, bl, ROWOW_SKILL_TK_READYCOUNTER, 1); break;
            }
            break;
        case ROWOW_SC_SPIRIT:
            // status_calc_pc's pc_calc_skilltree: the linked job's spirit skills (and a Bard's or Dancer's linked songs)
            if (sd != nullptr)
            {
                ROWOWIdentity* identity = GetIdentity(sd);
                identity->SpiritSkillID = uint16(val2);
                RefreshIdentitySkills(sd, *identity);
                SendSkillsToClient(sd, *identity, false);
            }
            break;
    }
    return true;
}

bool ROWOWMod::StatusChangeStartValues(Unit* src, Unit* bl, int32 type, int32& val1, int32& val2, int32& val3, int32& val4, int32& tick, int32& tickTime,
    uint8 flag)
{
    // status_change_start_post_delay's big switch: each status's values and timer, for the statuses of the pre-renewal jobs, items and
    // monsters.  False when the status doesn't start after all
    Player* sd = bl->ToPlayer();
    ROWOWStatusChange* sc = GetStatusChange(bl);
    ROWOWStatus const* status = GetROStatus(bl);
    ROWOWStatusChangeDB const* scdb = GetStatusChangeDB(type);
    bool undeadFlag = BattleCheckUndead(status->Race, status->DefElement);
    switch (type)
    {
        /* Permanent effects */
        case ROWOW_SC_AETERNA:
        case ROWOW_SC_MODECHANGE:
        case ROWOW_SC_WEIGHT50:
        case ROWOW_SC_WEIGHT90:
        case ROWOW_SC_BROKENWEAPON:
        case ROWOW_SC_BROKENARMOR:
        case ROWOW_SC_READYSTORM:
        case ROWOW_SC_READYDOWN:
        case ROWOW_SC_READYCOUNTER:
        case ROWOW_SC_READYTURN:
        case ROWOW_SC_DODGE:
        case ROWOW_SC_PUSH_CART:
            tick = ROWOW_INFINITE_TICK;
            break;

        case ROWOW_SC_KEEPING:
        case ROWOW_SC_BARRIER:
        {
            // The unit can't act until it ends (canact_tick; its attack, walk and endure ticks have no WoW counterpart here)
            ROWOWSkillUnitData* ud = GetSkillUnitData(bl);
            ud->CanActTime = GetTick() + uint64(std::max(0, tick));
            break;
        }
        case ROWOW_SC_DECREASEAGI:
        case ROWOW_SC_INCREASEAGI:
            val2 = 2 + val1; // Agi change
            break;
        case ROWOW_SC_ENDURE:
            val2 = 7; // Hit-count [Celest]
            // A Crusader passes it on to the players it devotes itself to (there's no War of Emperium or battleground here)
            if ((flag & ROWOW_SCSTART_NOAVOID) == 0 && sd != nullptr && val4 == 0)
                StatusChangeStartDevoted(src, sd, type, val1, val2, val3, val4, tick, ROWOW_SCSTART_NOAVOID | ROWOW_SCSTART_NOICON);
            if (val4 != 0)
                tick = ROWOW_INFINITE_TICK;
            break;
        case ROWOW_SC_AUTOBERSERK:
            tickTime = 100; // Check to start/end provoke every interval
            tick = ROWOW_INFINITE_TICK;
            break;
        case ROWOW_SC_SIGNUMCRUCIS:
            val2 = 10 + 4 * val1; // Def reduction
            tick = ROWOW_INFINITE_TICK;
            break;
        case ROWOW_SC_MAXIMIZEPOWER:
            if (bl->IsPlayer() == true)
            {
                // BL_CONSUME: the unit can use SP
                tickTime = val2 = tick > 0 ? tick : 10000; // SP consumption interval
                tick = ROWOW_INFINITE_TICK; // Duration sent to the client should be infinite
            }
            else
                tick = 10000; // If unit cannot consume SP, the duration is fixed to 10 seconds
            break;
        case ROWOW_SC_EDP:
            val2 = (val1 + 1) / 2 + 2; // Chance to Poison enemies.
            val3 = 50 * (val1 + 1); // Damage increase (+50 +50*lv%)
            break;
        case ROWOW_SC_POISONREACT:
            val2 = val1 / 2; // Number of Envenom autocasts
            val3 = 50; // Chance to autocast Envenom on hit / Poison chance of attack after block
            val4 = 0; // 0: Poison Block Mode; 1: Damage Boost Mode
            break;
        case ROWOW_SC_MAGICROD:
            val2 = val1 * 20; // SP gained
            break;
        case ROWOW_SC_KYRIE:
            if (val4 != 0)
            {
                // Formulas for Praefatio
                val2 = int32(status->MaxHP * (val1 * 2 + 10) / 100) + val4 * 2; // %Max HP to absorb
                val3 = 6 + val1; // Hits
            }
            else
            {
                // Formulas for Kyrie Eleison
                val2 = int32(status->MaxHP * (val1 * 2 + 10) / 100);
                val3 = (val1 / 2 + 5);
            }
            break;
        case ROWOW_SC_MAGICPOWER:
            val3 = 5 * val1; // Matk% increase
            val2 = 1; // Lasts 1 invocation
            val4 = 0; // 0 = ready to be used, 1 = activated and running
            break;
        case ROWOW_SC_SACRIFICE:
            val2 = 5; // Lasts 5 hits
            tick = ROWOW_INFINITE_TICK;
            break;
        case ROWOW_SC_ENCPOISON:
            val2 = 250 + 50 * val1; // Poisoning Chance (2.5+0.5%) in 1/10000 rate
            break;
        case ROWOW_SC_ELEMENTALCHANGE:
            // val1 : Element Lvl (if called by skill lvl 1, takes random value between 1 and 4)
            // val2 : Element (When no element, random one is picked)
            // val3 : 0 = called by skill 1 = called by script (fixed level)
            if (val2 == 0)
                val2 = int32(urand(0, ROWOW_ELEMENT_COUNT - 1));
            if (val1 == 1 && val3 == 0)
                val1 = 1 + int32(urand(0, 3));
            else if (val1 > 4)
                val1 = 4; // Max Level
            val3 = 0; // Not need to keep this info.
            break;
        case ROWOW_SC_PROVIDENCE:
            val2 = val1 * 5; // Race/Ele resist
            break;
        case ROWOW_SC_REFLECTSHIELD:
            val2 = 10 + val1 * 3; // %Dmg reflected
            // val4 used to mark if reflect shield is an inheritance bonus from Devotion
            if ((flag & ROWOW_SCSTART_NOAVOID) == 0 && sd != nullptr)
                StatusChangeStartDevoted(src, sd, type, val1, val2, 0, 1, tick, ROWOW_SCSTART_NOAVOID | ROWOW_SCSTART_NOICON);
            break;
        case ROWOW_SC_STRIPWEAPON:
            if (sd == nullptr) // Watk reduction
                val2 = 25;
            break;
        case ROWOW_SC_STRIPSHIELD:
            if (sd == nullptr) // Def reduction
                val2 = 15;
            break;
        case ROWOW_SC_STRIPARMOR:
            if (sd == nullptr) // Vit reduction
                val2 = 40;
            break;
        case ROWOW_SC_STRIPHELM:
            if (sd == nullptr) // Int reduction
                val2 = 40;
            break;
        case ROWOW_SC_AUTOSPELL:
            // Val1 Skill LV of Autospell, Val2 Skill ID to cast, Val3 Max Lv to cast
            val4 = 5 + val1 * 2; // Chance of casting
            break;
        case ROWOW_SC_VOLCANO:
        {
            static int8 const enchantEff[] = { 10, 14, 17, 19, 20 }; // Enchant addition
            uint8 i = uint8(std::max((val1 - 1) % 5, 0));
            val2 = val1 * 10; // Watk increase
            if (status->DefElement != ROWOW_ELEMENT_FIRE)
                val2 = 0;
            val3 = enchantEff[i];
            break;
        }
        case ROWOW_SC_VIOLENTGALE:
        {
            static int8 const enchantEff[] = { 10, 14, 17, 19, 20 }; // Enchant addition
            uint8 i = uint8(std::max((val1 - 1) % 5, 0));
            val2 = val1 * 3; // Flee increase
            if (status->DefElement != ROWOW_ELEMENT_WIND)
                val2 = 0;
            val3 = enchantEff[i];
            break;
        }
        case ROWOW_SC_DELUGE:
        {
            static int8 const delugeEff[] = { 5, 9, 12, 14, 15 }; // HP addition rate n/100
            static int8 const enchantEff[] = { 10, 14, 17, 19, 20 }; // Enchant addition
            uint8 i = uint8(std::max((val1 - 1) % 5, 0));
            val2 = delugeEff[i]; // HP increase
            if (status->DefElement != ROWOW_ELEMENT_WATER)
                val2 = 0;
            val3 = enchantEff[i];
            break;
        }
        case ROWOW_SC_SUITON:
        {
            ROWOWIdentity const* identity = sd != nullptr ? sd->CustomData.Get<ROWOWIdentity>("ROWOWIdentity") : nullptr;
            ROWOWJob const* job = identity != nullptr ? GetJob(identity->JobID) : nullptr;
            if (val2 == 0 || (job != nullptr && (job->JobID == ROWOW_JOB_NINJA || job->FirstJobID == ROWOW_JOB_NINJA)))
            {
                // No penalties.
                val2 = 0; // Agi penalty
                val3 = 0; // Walk speed penalty
                break;
            }
            val3 = 50;
            val2 = 3 * ((val1 + 1) / 3);
            if (val1 > 4)
                val2--;
            // Suiton is a special case, stop effect is forced and only happens when target enters it
            if (bl->IsCreature() == false || (status->Mode & ROWOW_MODE_KNOCKBACK_IMMUNE) == 0)
                bl->StopMoving();
            break;
        }
        case ROWOW_SC_ONEHAND:
        case ROWOW_SC_TWOHANDQUICKEN:
            val2 = 300;
            if (val1 > 10) // For boss casted skills [Skotlex]
                val2 += 20 * (val1 - 10);
            break;
        case ROWOW_SC_MERC_QUICKEN:
            val2 = 300;
            break;
        case ROWOW_SC_SPEARQUICKEN:
            val2 = 200 + 10 * val1;
            break;
        case ROWOW_SC_DANCING:
            // val1 : Skill ID + LV, val2 : Skill Group of the Dance, val3 : Brings the skill_lv (merged into val1 here), val4 : Partner
            val1 |= (val3 << 16);
            val3 = tick / 1000; // Tick duration
            tickTime = 1000; // [GodLesZ] tick time
            break;
        case ROWOW_SC_LONGING:
            val2 = 500 - 100 * val1; // Aspd penalty.
            break;
        case ROWOW_SC_EXPLOSIONSPIRITS:
            val2 = 75 + 25 * val1; // Cri bonus
            break;
        case ROWOW_SC_ASPDPOTION0:
        case ROWOW_SC_ASPDPOTION1:
        case ROWOW_SC_ASPDPOTION2:
        case ROWOW_SC_ASPDPOTION3:
            val2 = 50 * (2 + type - ROWOW_SC_ASPDPOTION0);
            break;
        case ROWOW_SC_DPOISON:
        {
            // Lose 10/15% of your life as long as it doesn't brings life below 25%
            uint32 maxHP = bl->GetMaxHealth();
            if (bl->GetHealth() > maxHP / 4)
            {
                uint32 diff = maxHP * (bl->IsPlayer() == true ? 10 : 15) / 100;
                if (bl->GetHealth() - std::min(diff, bl->GetHealth()) < maxHP / 4)
                    diff = bl->GetHealth() - (maxHP / 4);
                StatusZap(bl, diff, 0);
            }
            [[fallthrough]];
        }
        case ROWOW_SC_STONE:
        case ROWOW_SC_POISON:
        case ROWOW_SC_BLEEDING:
        case ROWOW_SC_BURNING:
        case ROWOW_SC_WINKCHARM:
            tickTime = StatusGetSCInterval(type);
            val4 = tick - tickTime; // Remaining time
            break;
        case ROWOW_SC_TOXIN:
            if (val3 == 1) // Target
                tickTime = StatusGetSCInterval(type);
            else // Caster
                tickTime = 1000;
            val4 = tick - tickTime; // Remaining time
            break;
        case ROWOW_SC_DEATHHURT:
            if (val3 == 1)
                break;
            tickTime = StatusGetSCInterval(type);
            val4 = tick - tickTime; // Remaining time
            break;
        case ROWOW_SC_LEECHESEND:
            if (val3 == 0)
                break;
            tickTime = StatusGetSCInterval(type);
            val4 = tick - tickTime; // Remaining time
            break;
        case ROWOW_SC_PYREXIA:
            if (val3 == 1)
            {
                // Target.  Causes blind for duration of pyrexia, unreducable and unavoidable, but can be healed with e.g. green potion
                StatusChangeStart(src, bl, ROWOW_SC_BLIND, 10000, val1, 0, 0, 0, tick, ROWOW_SCSTART_NOAVOID | ROWOW_SCSTART_NOTICKDEF | ROWOW_SCSTART_NORATEDEF);
                tickTime = StatusGetSCInterval(type);
                val4 = tick - tickTime; // Remaining time
            }
            else // Caster
                val2 = 15; // CRIT % and ATK % increase
            break;
        case ROWOW_SC_VENOMBLEED:
            if (val3 == 0) // Caster
                val2 = 30; // Reflect damage % reduction
            break;
        case ROWOW_SC_MAGICMUSHROOM:
            if (val3 == 1)
            {
                // Target
                tickTime = StatusGetSCInterval(type);
                val4 = tick - tickTime; // Remaining time
            }
            else // Caster
                val2 = 10; // After-cast delay % reduction
            break;
        case ROWOW_SC_HIDING:
            val2 = tick / 1000;
            tickTime = 1000; // [GodLesZ] tick time
            val3 = 0; // Unused, previously speed adjustment
            val4 = val1 + 3; // Seconds before SP substraction happen.
            break;
        case ROWOW_SC_CHASEWALK:
            val2 = tick > 0 ? tick : 10000; // Interval at which SP is drained.
            val3 = 35 - 5 * val1; // Speed adjustment.
            if (ROWOWStatusChangeEntry const* spirit = sc->GetSCE(ROWOW_SC_SPIRIT); spirit != nullptr && spirit->Val2 == ROWOW_SKILL_SL_ROGUE)
                val3 -= 40;
            val4 = 10 + val1 * 2; // SP cost.
            break;
        case ROWOW_SC_CLOAKING:
            if (bl->IsPlayer() == true)
            {
                tickTime = val2 = tick > 0 ? tick : 10000; // SP consumption interval
                tick = ROWOW_INFINITE_TICK; // Duration sent to the client should be infinite
            }
            else
            {
                // If unit cannot consume SP, there are no walk penalties and the duration is fixed to 10 seconds
                val1 = 10;
                tick = 10000;
            }
            val3 = 0; // Unused, previously walk speed adjustment
            // val4&1 signals the presence of a wall.  val4&2 makes cloak not end on normal attacks [Skotlex].  val4&4 makes cloak not end on
            // using skills
            if (bl->IsPlayer() == true)
                val4 |= BattleConfig.PCCloakCheckType & 7;
            else
                val4 |= BattleConfig.MonsterCloakCheckType & 7;
            break;
        case ROWOW_SC_SIGHT: // splash status
        case ROWOW_SC_RUWACH:
        case ROWOW_SC_SIGHTBLASTER:
        {
            ROWOWSkill const* skill = GetSkill(uint16(val2)); // Val2 should bring the skill-id.
            val3 = skill != nullptr ? ROWOWSkill::GetLevelValue(skill->SplashArea, uint16(val1)) : 0;
            val2 = tick / 20;
            tickTime = 20; // [GodLesZ] tick time
            break;
        }
        case ROWOW_SC_AUTOGUARD:
            if ((flag & ROWOW_SCSTART_NOAVOID) == 0)
            {
                val2 = 0;
                for (int32 i = 0; i < val1; i++)
                {
                    int32 t = 5 - (i / 2);
                    val2 += (t < 0) ? 1 : t;
                }
                if (sd != nullptr)
                    StatusChangeStartDevoted(src, sd, type, val1, val2, 0, 0, tick, ROWOW_SCSTART_NOAVOID | ROWOW_SCSTART_NOICON);
            }
            break;
        case ROWOW_SC_DEFENDER:
            if ((flag & ROWOW_SCSTART_NOAVOID) == 0)
            {
                val2 = 5 + 15 * val1; // Damage reduction
                val3 = 0; // Unused, previously speed adjustment
                val4 = 250 - 50 * val1; // Aspd adjustment
                // See if there are devoted characters, and pass the status to them. [Skotlex]
                if (sd != nullptr)
                    StatusChangeStartDevoted(src, sd, type, val1, val2, val3, val4, tick, ROWOW_SCSTART_NOAVOID);
            }
            break;
        case ROWOW_SC_DEVOTION:
        {
            // val1 the Crusader (its GUID), val2 its slot, val3 the link's reach.  Inherits status from source: Auto Guard, Defender, Reflect
            // Shield and Endure, each for its skill's full time (and not its values: they start as the devoted's own with NOAVOID)
            Player* d_bl = ObjectAccessor::GetPlayer(bl->GetMap(), ObjectGuid::Create<HighGuid::Player>(uint32(val1)));
            if (d_bl == nullptr)
                break;
            ROWOWStatusChange const* d_sc = GetStatusChange(static_cast<Unit const*>(d_bl));
            if (d_sc != nullptr && d_sc->IsEmpty() == false)
            {
                int32 const types[] = { ROWOW_SC_AUTOGUARD, ROWOW_SC_DEFENDER, ROWOW_SC_REFLECTSHIELD, ROWOW_SC_ENDURE };
                for (int32 i = 3; i >= 0; i--)
                {
                    int32 type2 = types[i];
                    if (ROWOWStatusChangeEntry const* sce2 = d_sc->GetSCE(type2))
                        StatusChangeStart(d_bl, bl, type2, 10000, sce2->Val1, 0, 0, (type2 == ROWOW_SC_REFLECTSHIELD ? 1 : 0),
                            SkillGetTime(StatusDBGetSkill(type2), uint16(sce2->Val1)),
                            (type2 == ROWOW_SC_DEFENDER) ? ROWOW_SCSTART_NOAVOID : (ROWOW_SCSTART_NOAVOID | ROWOW_SCSTART_NOICON));
                }
            }
            break;
        }
        case ROWOW_SC_TENSIONRELAX:
            if (sd != nullptr)
                sd->SetStandState(UNIT_STAND_STATE_SIT); // pc_setsit
            val2 = 12; // SP cost
            tickTime = 10000; // Decrease at 10secs intervals.
            val3 = tick / tickTime;
            tick = ROWOW_INFINITE_TICK; // Duration sent to the client should be infinite
            break;
        case ROWOW_SC_PARRYING:
            val2 = 20 + val1 * 3; // Block Chance
            break;
        case ROWOW_SC_WINDWALK:
            val2 = (val1 + 1) / 2; // Flee bonus is 1/1/2/2/3/3/4/4/5/5
            break;
        case ROWOW_SC_JOINTBEAT:
            if ((val2 & ROWOW_BREAK_NECK) != 0)
                SCStart2(src, bl, ROWOW_SC_BLEEDING, 100, val1, val3, SkillGetTime2(StatusDBGetSkill(type), uint16(val1)));
            break;
        case ROWOW_SC_BERSERK:
            if (val3 == ROWOW_SC__BLOODYLUST)
                SCStart(src, bl, val3, 100, val1, tick);
            else
                SCStart4(src, bl, ROWOW_SC_ENDURE, 100, 10, 0, 0, 1, tick);
            // HP healing is performing after the calc_status call.  Val2 holds HP penalty
            if (val4 == 0)
                val4 = SkillGetTime2(StatusDBGetSkill(type), uint16(val1));
            if (val4 == 0)
                val4 = 10000; // Val4 holds damage interval
            val3 = tick / val4; // val3 holds skill duration
            tickTime = val4; // [GodLesZ] tick time
            break;
        case ROWOW_SC_GOSPEL:
            if (val4 == ROWOW_BCT_SELF)
            {
                // Self effect
                val2 = tick / 10000;
                tickTime = 10000; // [GodLesZ] tick time
                StatusChangeClearBuffs(bl, ROWOW_SCCB_BUFFS | ROWOW_SCCB_DEBUFFS | ROWOW_SCCB_CHEM_PROTECT); // Remove buffs/debuffs
            }
            break;
        case ROWOW_SC_MARIONETTE:
        {
            ROWOWStatus const* bstatus = GetROBaseStatus(bl);
            ROWOWIdentity const* identity = sd != nullptr ? sd->CustomData.Get<ROWOWIdentity>("ROWOWIdentity") : nullptr;
            auto getStat = [identity, bstatus](uint8 statIndex) { return (identity != nullptr ? int32(identity->Stats[statIndex]) : bstatus->Stats[statIndex]) / 2; };
            val3 = 0;
            val4 = 0;
            val3 |= std::clamp(getStat(ROWOW_STAT_STR), 0, 0xFF) << 16;
            val3 |= std::clamp(getStat(ROWOW_STAT_AGI), 0, 0xFF) << 8;
            val3 |= std::clamp(getStat(ROWOW_STAT_VIT), 0, 0xFF);
            val4 |= std::clamp(getStat(ROWOW_STAT_INT), 0, 0xFF) << 16;
            val4 |= std::clamp(getStat(ROWOW_STAT_DEX), 0, 0xFF) << 8;
            val4 |= std::clamp(getStat(ROWOW_STAT_LUK), 0, 0xFF);
            break;
        }
        case ROWOW_SC_SPIRIT:
            // 1st Transcendent Spirit works similar to Marionette Control: each stat raised to base level - 10 (50 at most)
            if (sd != nullptr && val2 == ROWOW_SKILL_SL_HIGH)
            {
                ROWOWStatus const* status2 = GetROBaseStatus(bl);
                int32 maxStat = std::min<int32>(int32(status2->Level) - 10, 50);
                val3 = 0;
                val4 = 0;
                val3 |= std::clamp(std::max(0, maxStat - status2->Stats[ROWOW_STAT_STR]), 0, 0xFF) << 16;
                val3 |= std::clamp(std::max(0, maxStat - status2->Stats[ROWOW_STAT_AGI]), 0, 0xFF) << 8;
                val3 |= std::clamp(std::max(0, maxStat - status2->Stats[ROWOW_STAT_VIT]), 0, 0xFF);
                val4 |= std::clamp(std::max(0, maxStat - status2->Stats[ROWOW_STAT_INT]), 0, 0xFF) << 16;
                val4 |= std::clamp(std::max(0, maxStat - status2->Stats[ROWOW_STAT_DEX]), 0, 0xFF) << 8;
                val4 |= std::clamp(std::max(0, maxStat - status2->Stats[ROWOW_STAT_LUK]), 0, 0xFF);
            }
            break;
        case ROWOW_SC_MARIONETTE2:
        {
            // Fetch caster information (val1 is the caster's low GUID, on the same map)
            Unit* pbl = src;
            ROWOWStatusChangeEntry const* psce = pbl != nullptr ? GetSCE(pbl, ROWOW_SC_MARIONETTE) : nullptr;
            if (psce == nullptr)
                return false;
            int32 maxStat = BattleConfig.MaxParameter; // Cap to 99 (default)
            int32 const* stats = status->Stats; // Fetch target's stats (battle status)
            val3 = 0;
            val4 = 0;
            val3 |= std::clamp(std::min((psce->Val3 >> 16) & 0xFF, maxStat - stats[ROWOW_STAT_STR]), 0, 0xFF) << 16;
            val3 |= std::clamp(std::min((psce->Val3 >> 8) & 0xFF, maxStat - stats[ROWOW_STAT_AGI]), 0, 0xFF) << 8;
            val3 |= std::clamp(std::min(psce->Val3 & 0xFF, maxStat - stats[ROWOW_STAT_VIT]), 0, 0xFF);
            val4 |= std::clamp(std::min((psce->Val4 >> 16) & 0xFF, maxStat - stats[ROWOW_STAT_INT]), 0, 0xFF) << 16;
            val4 |= std::clamp(std::min((psce->Val4 >> 8) & 0xFF, maxStat - stats[ROWOW_STAT_DEX]), 0, 0xFF) << 8;
            val4 |= std::clamp(std::min(psce->Val4 & 0xFF, maxStat - stats[ROWOW_STAT_LUK]), 0, 0xFF);
            break;
        }
        case ROWOW_SC_REJECTSWORD:
            val2 = 15 * val1; // Reflect chance
            val3 = 3; // Reflections
            tick = ROWOW_INFINITE_TICK;
            break;
        case ROWOW_SC_CLOSECONFINE2:
        {
            // val2 is the Rogue holding it (its GUID's counter), who is held in place too for a second longer
            Unit* src2 = val2 != 0 ? MapID2BL(bl, val2) : nullptr;
            if (src2 == nullptr)
                return false; // Status failed.
            ROWOWStatusChange* sc2 = GetStatusChange(src2);
            ROWOWStatusChangeEntry* sce2 = sc2 != nullptr ? sc2->GetSCE(ROWOW_SC_CLOSECONFINE) : nullptr;
            if (sce2 == nullptr)
            {
                // Start lock on caster.
                val3 = 10; // Flee increase
                SCStart4(src2, src2, ROWOW_SC_CLOSECONFINE, 100, val1, 1, val3, 0, int64(tick) + 1000);
            }
            else
            {
                // Increase count of locked enemies and refresh time.
                sce2->Val2++;
                sce2->TimerTime = GetTick() + uint64(tick) + 1000;
                sce2->EndTime = sce2->TimerTime;
                if (ROWOWStatusChangeDB const* scdb2 = GetStatusChangeDB(ROWOW_SC_CLOSECONFINE))
                    ShowStatusChangeIcon(src2, *scdb2, *sce2);
            }
            break;
        }
        case ROWOW_SC_SPLASHER:
            // The blast comes when val4's countdown (the skill's Duration1) runs out, half a second at a time after the first second; the icon
            // shows the whole wait (rAthena's countdown message is commented out)
            tickTime = tick;
            tick = tick + val4 - 500;
            break;
        case ROWOW_SC_MEMORIZE:
            val2 = 5; // Memorized casts.
            tick = ROWOW_INFINITE_TICK;
            break;
        case ROWOW_SC_GRAVITATION:
            val2 = 50 * val1; // aspd reduction
            break;
        case ROWOW_SC_REGENERATION:
            if (val1 == 1)
                val2 = 2;
            else
                val2 = val1; // HP Regerenation rate: 200% 200% 300%
            val3 = val1; // SP Regeneration Rate: 100% 200% 300%
            // If val4 comes set, this blocks regen rather than increase it.
            break;
        case ROWOW_SC_COMA:
            // Coma. Sends a char to 1HP. If val2, do not zap sp
            StatusZap(bl, bl->GetHealth() > 0 ? bl->GetHealth() - 1 : 0, val2 != 0 ? 0 : (bl->GetPower(POWER_MANA) > 0 ? bl->GetPower(POWER_MANA) - 1 : 0));
            return false;
        case ROWOW_SC_KAITE:
            val2 = 1 + val1 / 5; // Number of bounces: 1 + skill_lv/5
            break;
        case ROWOW_SC_KAUPE:
            switch (val1)
            {
                case 3: // 33*3 + 1 -> 100%
                    val2++;
                    [[fallthrough]];
                case 1:
                case 2: // 33, 66%
                    val2 += 33 * val1;
                    val3 = 1; // Dodge 1 attack total.
                    break;
                default: // Custom. For high level mob usage, higher level means more blocks. [Skotlex]
                    val2 = 100;
                    val3 = val1 - 2;
                    break;
            }
            break;
        case ROWOW_SC_COMBO:
        {
            // val1: Skill ID, val2: When given, target (for autotargetting skills), val3: When set, this combo time should NOT delay
            // attack/movement, val3: If set to 2 this combo will delay ONLY attack, val3: TK: Last used kick, val4: TK: Combo time.  (The
            // combo's walk delay has no WoW counterpart; the attack delay is the swing timer)
            if (val3 == 0 || val3 == 2)
            {
                tick += 300 * BattleConfig.ComboDelayRate / 100;
                if (Player* comboPlayer = bl->ToPlayer())
                    comboPlayer->setAttackTimer(BASE_ATTACK, std::max<uint32>(comboPlayer->getAttackTimer(BASE_ATTACK), uint32(std::max(0, tick))));
            }
            val3 = 0;
            val4 = tick;
            break;
        }
        case ROWOW_SC_EARTHSCROLL:
            val2 = 11 - val1; // Chance to consume: 11-skill_lv%
            break;
        case ROWOW_SC_RUN:
        {
            // Store time at which you started running.  Note: this int64 value is stored in two separate int32 variables (FIXME)
            uint64 currentTick = GetTick();
            val3 = int32(currentTick & 0x00000000ffffffffLL);
            val4 = int32((currentTick & 0xffffffff00000000LL) >> 32);
            tick = ROWOW_INFINITE_TICK;
            break;
        }
        case ROWOW_SC_KAAHI:
            val2 = 200 * val1; // HP heal
            val3 = 5 * val1; // SP cost
            break;
        case ROWOW_SC_BLESSING:
            if (bl->IsPlayer() == true || (undeadFlag == false && status->Race != ROWOW_RACE_DEMON))
                val2 = val1;
            else
                val2 = 0; // 0 -> Half stat.
            break;
        case ROWOW_SC_TRICKDEAD:
            tick = ROWOW_INFINITE_TICK;
            break;
        case ROWOW_SC_CONCENTRATE:
            val2 = 2 + val1;
            if (sd != nullptr)
            {
                // Store the card-bonus data that should not count in the %
                ROWOWStatus const* bstatus = GetROBaseStatus(bl);
                val3 = bstatus != nullptr ? bstatus->Bonus.ParamBonus[ROWOW_STAT_AGI] : 0; // Agi
                val4 = bstatus != nullptr ? bstatus->Bonus.ParamBonus[ROWOW_STAT_DEX] : 0; // Dex
            }
            else
                val3 = val4 = 0;
            break;
        case ROWOW_SC_MAXOVERTHRUST:
            val2 = 20 * val1; // Power increase
            break;
        case ROWOW_SC_OVERTHRUST:
        case ROWOW_SC_ADRENALINE2:
        case ROWOW_SC_ADRENALINE:
        case ROWOW_SC_WEAPONPERFECTION:
        {
            Player* srcPlayer = src != nullptr ? src->ToPlayer() : nullptr;
            if (type == ROWOW_SC_OVERTHRUST)
                val3 = (val2 != 0) ? 5 * val1 : 5; // val2 holds if it was casted on self, or is bonus received from others.  Power increase
            else if (type == ROWOW_SC_ADRENALINE2 || type == ROWOW_SC_ADRENALINE)
                val3 = (val2 != 0) ? 300 : 200; // Aspd increase
            if (srcPlayer != nullptr && PCCheckSkill(srcPlayer, ROWOW_SKILL_BS_HILTBINDING) > 0)
                tick += tick / 10; // If caster has Hilt Binding, duration increases by 10%
            break;
        }
        case ROWOW_SC_CONCENTRATION:
            val2 = 5 * val1; // Batk/Watk Increase
            val4 = 5 * val1; // Def reduction
            val3 = 10 * val1; // Hit Increase
            SCStart(src, bl, ROWOW_SC_ENDURE, 100, 1, tick); // Level 1 Endure effect
            break;
        case ROWOW_SC_ANGELUS:
            val2 = 5 * val1; // def increase
            break;
        case ROWOW_SC_IMPOSITIO:
            val2 = 5 * val1; // WATK/MATK increase
            break;
        case ROWOW_SC_MELTDOWN:
            val2 = 100 * val1; // Chance to break weapon
            val3 = 70 * val1; // Change to break armor
            break;
        case ROWOW_SC_TRUESIGHT:
            val2 = 10 * val1; // Critical increase
            val3 = 3 * val1; // Hit increase
            break;
        case ROWOW_SC_SUN_COMFORT:
            val2 = (int32(status->Level) + status->Stats[ROWOW_STAT_DEX] + status->Stats[ROWOW_STAT_LUK]) / 2; // def increase
            break;
        case ROWOW_SC_MOON_COMFORT:
            val2 = (int32(status->Level) + status->Stats[ROWOW_STAT_DEX] + status->Stats[ROWOW_STAT_LUK]) / 10; // flee increase
            break;
        case ROWOW_SC_STAR_COMFORT:
            val2 = (int32(status->Level) + status->Stats[ROWOW_STAT_DEX] + status->Stats[ROWOW_STAT_LUK]); // Aspd increase
            break;
        case ROWOW_SC_QUAGMIRE:
            val2 = (sd != nullptr ? 5 : 10) * val1; // Agi/Dex decrease.
            break;

        // gs_something1 [Vicious]
        case ROWOW_SC_GATLINGFEVER:
            val2 = 20 * val1; // Aspd increase
            val3 = 20 + 10 * val1; // Atk increase
            val4 = 5 * val1; // Flee decrease
            break;
        case ROWOW_SC_FLING:
            if (bl->IsPlayer() == true)
                val2 = 0; // No armor reduction to players.
            else
                val2 = 5 * val1; // Def reduction
            val3 = 5 * val1; // Def2 reduction
            break;
        case ROWOW_SC_PROVOKE:
            if (src != nullptr && src->IsPlayer() == false && val1 == 10)
            {
                val2 = 0; // 0% Atk increase
                val3 = 100; // 100% Def reduction
            }
            else
            {
                val2 = 2 + 3 * val1; // Atk increase
                val3 = 5 + 5 * val1; // Def reduction
            }
            // val4 signals autoprovoke.
            break;
        case ROWOW_SC_AVOID:
            val2 = (status != nullptr && status->FollowerType == ROWOW_BL_HOM ? 40 : 10) * val1; // Speed change rate.
            break;
        case ROWOW_SC_DEFENCE:
            val2 = 2 * val1; // Vit bonus for players / Def bonus for homunculus
            break;
        case ROWOW_SC_BLOODLUST:
            val2 = 20 + (10 * val1); // Atk rate change
            val3 = 9 * val1; // Leech chance.  It's actually 9 * level on both pre-re and renewal, despite the description
            val4 = 20; // Leech percent
            break;
        case ROWOW_SC_FLEET:
            val2 = 30 * val1; // Aspd change
            val3 = 5 + 5 * val1; // bAtk/wAtk rate change
            break;
        case ROWOW_SC_MINDBREAKER:
            val2 = 20 * val1; // matk increase.
            val3 = 12 * val1; // mdef2 reduction.
            break;
        case ROWOW_SC_UTSUSEMI:
        {
            val2 = (val1 + 1) / 2; // Number of hits blocked
            ROWOWSkill const* utsusemi = GetSkill(ROWOW_SKILL_NJ_UTSUSEMI);
            val3 = utsusemi != nullptr ? ROWOWSkill::GetLevelValue(utsusemi->Knockback, uint16(val1)) : 0; // knockback value.
            break;
        }
        case ROWOW_SC_BUNSINJYUTSU:
            val2 = (val1 + 1) / 2; // Number of hits blocked
            break;
        case ROWOW_SC_CHANGE:
            val2 = 30 * val1; // Vit increase
            val3 = 20 * val1; // Int increase
            break;
        case ROWOW_SC_SWOO:
            if ((status->Mode & ROWOW_MODE_STATUS_IMMUNE) != 0)
                tick /= 5; // !TODO: Reduce skill's duration. But for how long?
            break;
        case ROWOW_SC_ARMOR:
            // NPC_DEFENDER:
            val2 = 8; // Damage will be divided by this value
            // Attack requirements to be blocked:
            val3 = ROWOW_BF_LONG; // Range
            val4 = ROWOW_BF_WEAPON | ROWOW_BF_MISC; // Type
            break;
        case ROWOW_SC_ENCHANTARMS:
            // Make sure the received element is valid.
            if (val1 >= ROWOW_ELEMENT_COUNT)
                val1 = val1 % ROWOW_ELEMENT_COUNT;
            else if (val1 < 0)
                val1 = int32(urand(0, ROWOW_ELEMENT_COUNT - 1));
            break;
        case ROWOW_SC_CRITICALWOUND:
            // Level 1 ~ 5 & 6 ~ 10 has different duration.  Level 6 ~ 10 use effect of level 1 ~ 5
            val1 = 1 + ((val1 - 1) % 5);
            val2 = 20 * val1; // Heal effectiveness decrease
            break;
        case ROWOW_SC_WEAPONBREAKER:
            val2 = val1 * 2 * 100; // Chance to break weapon
            break;
        case ROWOW_SC_MAGICMIRROR:
            // Level 1 ~ 5 & 6 ~ 10 has different duration.  Level 6 ~ 10 use effect of level 1 ~ 5
            val1 = 1 + ((val1 - 1) % 5);
            [[fallthrough]];
        case ROWOW_SC_SLOWCAST:
            val2 = 20 * val1; // Magic reflection/cast rate
            break;
        case ROWOW_SC_ARMORCHANGE:
            if (val2 == ROWOW_SKILL_NPC_ANTIMAGIC)
            {
                // Boost mdef
                val2 = -20;
                val3 = 20;
            }
            else
            {
                // Boost def
                val2 = 20;
                val3 = -20;
            }
            // Level 1 ~ 5 & 6 ~ 10 has different duration.  Level 6 ~ 10 use effect of level 1 ~ 5
            val1 = 1 + ((val1 - 1) % 5);
            val2 *= val1; // 20% per level
            val3 *= val1;
            break;
        case ROWOW_SC_EXPBOOST:
        case ROWOW_SC_JEXPBOOST:
            if (val1 < 1)
                return false;
            break;
        case ROWOW_SC_SUFFRAGIUM:
            val2 = 15 * val1; // Speed cast decrease
            break;
        case ROWOW_SC_INCHEALRATE:
            if (val1 < 1)
                val1 = 1;
            break;
        case ROWOW_SC_DOUBLECAST:
            val2 = 30 + 10 * val1; // Trigger rate
            break;
        case ROWOW_SC_KAIZEL:
            val2 = 10 * val1; // % of life to be revived with
            break;
        case ROWOW_SC_REBIRTH:
            val2 = 20 * val1; // % of life to be revived with
            break;
        case ROWOW_SC_INVINCIBLE:
            val2 = 100; // ATKpercent increase
            val3 = 50; // Speed increase
            val4 = 700; // ASPD increase
            break;

        /* General */
        case ROWOW_SC_FEAR:
            SCStart(src, bl, ROWOW_SC_ANKLE, 100, 0, 2000);
            break;

        /* Items (their statuses sit among the later jobs' in rAthena's switch) */
        case ROWOW_SC_STOMACHACHE:
            val2 = 8; // SP consume.
            val4 = tick / 10000;
            tickTime = 10000; // [GodLesZ] tick time
            break;
        case ROWOW_SC_PROMOTE_HEALTH_RESERCH:
        {
            // val1: 1 = Regular Potion, 2 = Thrown Potion; val2: 1 = Small Potion, 2 = Medium Potion, 3 = Large Potion; val3: MaxHP Increase
            // By Fixed Amount.  If potion was normally used, take the user's BaseLv; if it was thrown at someone, the thrower's
            if (val1 == 1)
                val3 = 1000 * val2 - 500 + GetUnitLevel(status) * 10 / 3;
            else if (val1 == 2)
                val3 = 1000 * val2 - 500 + GetUnitLevel(GetROStatus(src)) * 10 / 3;
            if (val3 <= 0) // Prevents a negeative value from happening
                val3 = 0;
            break;
        }
        case ROWOW_SC_ENERGY_DRINK_RESERCH:
            // val3: MaxSP Increase By Percentage Amount
            if (val1 == 1)
                val3 = GetUnitLevel(status) / 10 + 5 * val2 - 10;
            else if (val1 == 2)
                val3 = GetUnitLevel(GetROStatus(src)) / 10 + 5 * val2 - 10;
            if (val3 <= 0) // Prevents a negeative value from happening
                val3 = 0;
            break;

        /* The Bard's and Dancer's songs: their values come from their ground units (skill_unitsetting); the icon shows them as endless */
        case ROWOW_SC_APPLEIDUN:
        {
            Player* srcPlayer = src != nullptr ? src->ToPlayer() : nullptr;
            ROWOWStatus const* sstatus = GetROStatus(src);
            val2 = (5 + 2 * val1) + ((sstatus != nullptr ? sstatus->Stats[ROWOW_STAT_VIT] : 0) / 10); // HP Rate: (5 + 2 * skill_lv) + (vit/10) + (BA_MUSICALLESSON level)
            if (srcPlayer != nullptr)
                val2 += PCCheckSkill(srcPlayer, ROWOW_SKILL_BA_MUSICALLESSON) / 2;
            [[fallthrough]];
        }
        case ROWOW_SC_WHISTLE:
        case ROWOW_SC_ASSNCROS:
        case ROWOW_SC_POEMBRAGI:
        case ROWOW_SC_HUMMING:
        case ROWOW_SC_DONTFORGETME:
        case ROWOW_SC_FORTUNE:
        case ROWOW_SC_SERVICE4U:
            // Display icon as infinite
            tickTime = tick;
            tick = ROWOW_INFINITE_TICK;
            break;

        default:
            if (scdb->CalcFlag.none() == true && scdb->SkillID == 0 && scdb->Icon.empty() == true && scdb->Opt1 == ROWOW_OPT1_NONE && scdb->Opt2 == 0 &&
                scdb->State.none() == true && scdb->Flag.none() == true && scdb->EndOnStart.empty() == true && scdb->EndReturn.empty() == true &&
                scdb->Fail.empty() == true && scdb->EndOnEnd.empty() == true)
                return false; // Status change with no calc, no icon, and no skill associated...?
    }
    return true;
}

bool ROWOWMod::StatusChangeEnd(Unit* bl, int32 type, bool isTimer)
{
    // status_change_end: removes the status and undoes what it did.  isTimer is a natural expiration (status_change_timer's tid)
    ROWOWStatusChange* sc = GetStatusChange(bl);
    if (sc == nullptr)
        return false;
    ROWOWStatusChangeEntry* sce = sc->GetSCE(type);
    if (sce == nullptr)
        return false;
    if (isTimer == false && type == ROWOW_SC_ENDURE && sce->Val4 != 0)
        return false; // Do not end infinite endure.
    if (isTimer == false && type == ROWOW_SC_SPIDERWEB)
    {
        // Delete the unit group first to expire found in the status change (the webs are val2-4): the status stays while another web holds it
        uint64 tick = GetTick();
        std::shared_ptr<ROWOWSkillUnitGroup> group;
        int32 pos = 1;
        if (sce->Val2 != 0 && (group = SkillID2Group(bl->GetMap(), sce->Val2)) == nullptr)
            sce->Val2 = 0;
        for (int32 i = 2; i <= 3; i++)
        {
            int32& groupID = i == 2 ? sce->Val3 : sce->Val4;
            if (groupID == 0)
                continue;
            std::shared_ptr<ROWOWSkillUnitGroup> group2 = SkillID2Group(bl->GetMap(), groupID);
            if (group2 == nullptr)
                groupID = 0;
            else if (group == nullptr || (group->Limit - int64(tick - group->Tick)) > (group2->Limit - int64(tick - group2->Tick)))
            {
                group = group2;
                pos = i;
            }
        }
        if (pos == 1)
            sce->Val2 = 0;
        else if (pos == 2)
            sce->Val3 = 0;
        else
            sce->Val4 = 0;
        if (group != nullptr)
            SkillDelUnitGroup(bl->GetMap(), group);
        sce = sc->GetSCE(type);
        if (sce == nullptr)
            return true;
        if (bl->IsAlive() == true && (sce->Val2 != 0 || sce->Val3 != 0 || sce->Val4 != 0))
            return false; // Don't end the status change yet as there are still unit groups associated with it
    }

    // Store a copy, if access is required later
    int32 val1 = sce->Val1;
    int32 val2 = sce->Val2;
    int32 val3 = sce->Val3;
    int32 val4 = sce->Val4;
    sc->EntriesByType.erase(type);

    ROWOWStatusChangeDB const* scdb = GetStatusChangeDB(type);
    if (scdb == nullptr)
        return false;
    if (scdb->State.any() == true)
        StatusCalcState(bl, *sc, *scdb, false);
    Player* sd = bl->ToPlayer();
    std::bitset<ROWOW_SCB_MAX> calcFlag = scdb->CalcFlag;

    switch (type)
    {
        case ROWOW_SC_KEEPING:
        case ROWOW_SC_BARRIER:
            GetSkillUnitData(bl)->CanActTime = GetTick();
            break;
        case ROWOW_SC_RUN:
        {
            // The Taekwon's Sprint ends in a Spurt when it ran for at most a second with empty hands (not when it ran into something:
            // unit_run_hit stops the running first)
            ROWOWSkillUnitData* ud = GetSkillUnitData(bl);
            bool beginSpurt = ud->IsRunning;
            ud->IsRunning = false;
            bl->StopMoving();
            uint64 startTick = uint64(uint32(val3)) | (uint64(uint32(val4)) << 32);
            ROWOWStatus const* status = GetROStatus(bl);
            if (beginSpurt == true && val1 >= 7 && GetTick() - startTick <= 1000 && (sd == nullptr || (status != nullptr &&
                status->RightWeaponType == ROWOW_WEAPON_TYPE_FIST && status->LeftWeaponType == ROWOW_WEAPON_TYPE_FIST)))
                SCStart(bl, bl, ROWOW_SC_SPURT, 100, val1, SkillGetTime2(scdb->SkillID, uint16(val1)));
            break;
        }
        case ROWOW_SC_WARM:
            if (val4 != 0 && bl->IsInWorld() == true)
            {
                // Clear the group.
                if (std::shared_ptr<ROWOWSkillUnitGroup> group = SkillID2Group(bl->GetMap(), val4)) // Might have been cleared before status ended, e.g. land protector
                    SkillDelUnitGroup(bl->GetMap(), group);
            }
            break;
        case ROWOW_SC_SPIRIT:
            if (sd != nullptr)
            {
                ROWOWIdentity* identity = GetIdentity(sd);
                identity->SpiritSkillID = 0;
                RefreshIdentitySkills(sd, *identity);
                SendSkillsToClient(sd, *identity, false);
            }
            break;
        case ROWOW_SC_AUTOBERSERK:
            if (ROWOWStatusChangeEntry const* provoke = sc->GetSCE(ROWOW_SC_PROVOKE); provoke != nullptr && provoke->Val4 == 1)
                StatusChangeEnd(bl, ROWOW_SC_PROVOKE);
            break;
        case ROWOW_SC_CONCENTRATION:
            if (ROWOWStatusChangeEntry const* endure = sc->GetSCE(ROWOW_SC_ENDURE); endure != nullptr && endure->Val4 == 0)
                StatusChangeEnd(bl, ROWOW_SC_ENDURE);
            break;
        case ROWOW_SC_BERSERK:
            if (bl->GetHealth() > 200 && sc->HasSCE(ROWOW_SC__BLOODYLUST) == true)
            {
                bl->SetHealth(bl->GetMaxHealth());
                StatusChangeEnd(bl, ROWOW_SC__BLOODYLUST);
            }
            else if (bl->GetHealth() > 100 && val2 != 0) // If val2 is removed, no HP penalty (dispelled?) [Skotlex]
                bl->SetHealth(100);
            if (ROWOWStatusChangeEntry* endure = sc->GetSCE(ROWOW_SC_ENDURE); endure != nullptr && endure->Val4 != 0)
            {
                endure->Val4 = 0;
                StatusChangeEnd(bl, ROWOW_SC_ENDURE);
            }
            SCStart4(bl, bl, ROWOW_SC_REGENERATION, 100, 10, 0, 0, ROWOW_RGN_HP | ROWOW_RGN_SP, SkillGetTime(ROWOW_SKILL_LK_BERSERK, uint16(val1)));
            break;
        case ROWOW_SC_MARIONETTE:
        case ROWOW_SC_MARIONETTE2: // Marionette target
            if (val1 != 0 && bl->IsInWorld() == true)
            {
                // Check for partner and end their marionette status as well
                int32 type2 = type == ROWOW_SC_MARIONETTE ? ROWOW_SC_MARIONETTE2 : ROWOW_SC_MARIONETTE;
                Unit* pbl = MapID2BL(bl, val1);
                ROWOWStatusChange* sc2 = pbl != nullptr ? GetStatusChange(pbl) : nullptr;
                if (ROWOWStatusChangeEntry* sce2 = sc2 != nullptr ? sc2->GetSCE(type2) : nullptr)
                {
                    sce2->Val1 = 0;
                    StatusChangeEnd(pbl, type2);
                }
            }
            break;
        case ROWOW_SC_DANCING:
            if (bl->IsInWorld() == false)
                break;
            // End status on partner as well (an ensemble's: val4 names it, and the partner's names this one back; Wand of Hermode's val4 is its
            // group, which a player's id here could match, so the partner must name this one too)
            if (val4 != 0 && val4 != ROWOW_BCT_SELF)
            {
                Player* dsd = ObjectAccessor::GetPlayer(bl->GetMap(), ObjectGuid::Create<HighGuid::Player>(uint32(val4)));
                ROWOWStatusChange* dsc = dsd != nullptr ? GetStatusChange(dsd) : nullptr;
                if (ROWOWStatusChangeEntry* dsce = dsc != nullptr ? dsc->GetSCE(ROWOW_SC_DANCING) : nullptr; dsce != nullptr &&
                    dsce->Val4 == int32(bl->GetGUID().GetCounter()))
                {
                    // This will prevent recursive loops.
                    dsce->Val2 = 0;
                    dsce->Val4 = ROWOW_BCT_SELF;
                    StatusChangeEnd(dsd, ROWOW_SC_DANCING);
                }
            }
            // Erase associated land skill
            if (val2 != 0)
                if (std::shared_ptr<ROWOWSkillUnitGroup> group = SkillID2Group(bl->GetMap(), val2))
                    SkillDelUnitGroup(bl->GetMap(), group);
            break;
        case ROWOW_SC_HERMODE:
            if (val3 == ROWOW_BCT_SELF)
                SkillClearUnitGroup(bl);
            break;
        case ROWOW_SC_CHANGE:
            if (isTimer == false)
                break;
            // "lose almost all their HP and SP" on natural expiration.
            bl->SetHealth(std::min<uint32>(10, bl->GetMaxHealth()));
            bl->SetPower(POWER_MANA, std::min<uint32>(10, bl->GetMaxPower(POWER_MANA)));
            break;
        case ROWOW_SC_STOP:
            // NPC_STOP: the partner's ends with it
            if (val2 != 0)
            {
                Unit* tbl = MapID2BL(bl, val2);
                ROWOWStatusChangeEntry const* tsce = tbl != nullptr ? GetSCE(tbl, ROWOW_SC_STOP) : nullptr;
                if (tsce != nullptr && tsce->Val2 == int32(bl->GetGUID().GetCounter()))
                    StatusChangeEnd(tbl, ROWOW_SC_STOP);
            }
            break;
        case ROWOW_SC_TENSIONRELAX:
            if (sd != nullptr)
                sd->SetStandState(UNIT_STAND_STATE_STAND); // pc_setstand
            break;
        case ROWOW_SC_ENDURE:
        case ROWOW_SC_DEFENDER:
        case ROWOW_SC_REFLECTSHIELD:
        case ROWOW_SC_AUTOGUARD:
            // Clear Status from others: the players a Crusader devotes itself to lose what it passed on
            if (sd != nullptr)
            {
                for (int32 i = 0; i < ROWOW_MAX_DEVOTION; i++)
                    if (Player* tsd = GetDevotedPlayer(sd, i); tsd != nullptr && GetSCE(tsd, type) != nullptr)
                        StatusChangeEnd(tsd, type);
            }
            break;
        case ROWOW_SC_DEVOTION:
        {
            // The Crusader's slot is free again (when the Crusader is here: a slot no longer named is free anyway, see GetDevotedPlayer)
            Player* d_bl = bl->IsInWorld() == true ? ObjectAccessor::GetPlayer(bl->GetMap(), ObjectGuid::Create<HighGuid::Player>(uint32(val1))) : nullptr;
            ROWOWIdentity* identity = d_bl != nullptr ? d_bl->CustomData.Get<ROWOWIdentity>("ROWOWIdentity") : nullptr;
            if (identity != nullptr && val2 >= 0 && val2 < ROWOW_MAX_DEVOTION && identity->DevotionGUIDs[val2] == bl->GetGUID())
                identity->DevotionGUIDs[val2].Clear();
            if (Player* master = bl->ToPlayer())
                if (ROWOWMercData* mercenary = GetMercenary(master); mercenary != nullptr && mercenary->CreatureGUID.GetCounter() == uint32(val1))
                    mercenary->IsDevotion = false;
            break;
        }
        case ROWOW_SC_GOSPEL:
            if (val3 != 0 && bl->IsInWorld() == true)
            {
                // Clear the group.
                if (std::shared_ptr<ROWOWSkillUnitGroup> group = SkillID2Group(bl->GetMap(), val3))
                    SkillDelUnitGroup(bl->GetMap(), group);
            }
            break;
        case ROWOW_SC_BASILICA: // Clear the skill area. [Skotlex]
            if (val3 != 0 && val4 == int32(bl->GetGUID().GetCounter()) && bl->IsInWorld() == true)
            {
                if (std::shared_ptr<ROWOWSkillUnitGroup> group = SkillID2Group(bl->GetMap(), val3))
                    SkillDelUnitGroup(bl->GetMap(), group);
            }
            break;
        case ROWOW_SC_SPLASHER:
            // The countdown ran out: the caster's Venom Splasher goes off on the target and around it
            if (isTimer == true && bl->IsInWorld() == true)
                if (Unit* src = MapID2BL(bl, val3))
                    SkillCastendDamageID(src, bl, uint16(val2), uint16(val1), GetTick(), ROWOW_SD_LEVEL);
            break;
        case ROWOW_SC_CLOSECONFINE2:
            if (val2 != 0 && bl->IsInWorld() == true)
            {
                // If status was already ended, do nothing.  Decrease count (rAthena counts the holder's val1, its level, down: one hold frees it)
                Unit* src = MapID2BL(bl, val2);
                ROWOWStatusChange* sc2 = src != nullptr ? GetStatusChange(src) : nullptr;
                if (ROWOWStatusChangeEntry* sce2 = sc2 != nullptr ? sc2->GetSCE(ROWOW_SC_CLOSECONFINE) : nullptr; sce2 != nullptr && --sce2->Val1 <= 0)
                    StatusChangeEnd(src, ROWOW_SC_CLOSECONFINE); // No more holds, free him up.
            }
            [[fallthrough]];
        case ROWOW_SC_CLOSECONFINE:
            if (val2 > 0 && bl->IsInWorld() == true)
            {
                // Caster has been unlocked... nearby chars need to be unlocked (in reach of the skill and of a Back Slide out of it)
                int32 range = 1 + SkillGetRange2(bl, scdb->SkillID, uint16(val1), true) + SkillGetRange2(bl, ROWOW_SKILL_TF_BACKSLIDING, 1, true);
                int32 x = 0, y = 0;
                GetUnitCell(bl, x, y);
                uint64 tick = GetTick();
                MapForEachInArea(bl, x - range, y - range, x + range, y + range, [this, bl, type, tick](Unit* target)
                {
                    StatusChangeTimerSub(target, bl, nullptr, type, tick);
                    return 0;
                });
            }
            break;
        case ROWOW_SC_BLADESTOP:
            if (val4 != 0 && bl->IsInWorld() == true)
            {
                // Stop the status for the other guy of bladestop as well
                Unit* tbl = MapID2BL(bl, val4);
                ROWOWStatusChange* tsc = tbl != nullptr ? GetStatusChange(tbl) : nullptr;
                if (ROWOWStatusChangeEntry* tsce = tsc != nullptr ? tsc->GetSCE(ROWOW_SC_BLADESTOP) : nullptr)
                {
                    tsce->Val4 = 0;
                    StatusChangeEnd(tbl, ROWOW_SC_BLADESTOP);
                }
            }
            break;
    }

    // End statuses found in the EndOnEnd list.
    for (int32 endType : scdb->EndOnEnd)
        StatusChangeEnd(bl, endType);

    // Reset the options as needed
    if (scdb->Opt1 != ROWOW_OPT1_NONE)
        sc->Opt1 = ROWOW_OPT1_NONE;
    if (scdb->Opt2 != 0)
        sc->Opt2 &= ~scdb->Opt2;
    if (scdb->Opt3 != 0)
        sc->Opt3 &= ~scdb->Opt3;
    if (scdb->Look != 0)
        sc->Option &= ~scdb->Look;

    // On Aegis, when turning off a status change, first goes the sc packet, then the option packet.
    HideStatusChangeIcon(bl, *scdb);
    UpdateStatusChangeOptions(bl, *sc);
    calcFlag.reset(ROWOW_SCB_DYE);
    if (calcFlag.any() == true)
        StatusCalcBL(bl, calcFlag);
    if (sd != nullptr)
        StatusChangeSaveNeeded(sd);

    // Needed to be here to make sure OPT1_STONEWAIT has been cleared from the target (only on natural expiration of the stone wait timer)
    if (type == ROWOW_SC_STONEWAIT && isTimer == true)
        StatusChangeStart(bl, bl, ROWOW_SC_STONE, 100, val1, val2, 0, 0, val3, ROWOW_SCSTART_NOAVOID);
    return true;
}

int32 ROWOWMod::StatusChangeClear(Unit* bl, int32 type)
{
    // status_change_clear: ends every status but those listed.  type 0: killed (the NoRemoveOnDead ones stay), 1: leaving (logout), 3: not the
    // ones that don't clear (NoClearbuff)
    ROWOWStatusChange* sc = GetStatusChange(bl);
    if (sc == nullptr || sc->IsEmpty() == true)
        return 0;
    std::vector<int32> types;
    for (auto const& entryByType : sc->EntriesByType)
        types.push_back(entryByType.first);
    for (int32 statusType : types)
    {
        ROWOWStatusChangeEntry* sce = sc->GetSCE(statusType);
        ROWOWStatusChangeDB const* scdb = GetStatusChangeDB(statusType);
        if (sce == nullptr || scdb == nullptr)
            continue;
        if (type == 0 && scdb->Flag[ROWOW_SCF_NOREMOVEONDEAD] == true)
        {
            // Type 0: PC killed.  Only when its Holy or Dark does SC_ELEMENTALCHANGE not dispell on death
            if (statusType != ROWOW_SC_ELEMENTALCHANGE || sce->Val2 == ROWOW_ELEMENT_HOLY || sce->Val2 == ROWOW_ELEMENT_DARK)
                continue;
        }
        if (type == 3 && scdb->Flag[ROWOW_SCF_NOCLEARBUFF] == true)
            continue;
        if (type != 0 && statusType == ROWOW_SC_ENDURE)
            sce->Val4 = 0; // Leaving ends even an infinite endure
        StatusChangeEnd(bl, statusType);
        if (type == 1 && sc->HasSCE(statusType) == true)
        {
            // If for some reason status_change_end decides to still keep the status when quitting. [Skotlex]
            sc->EntriesByType.erase(statusType);
            if (ROWOWStatusChangeDB const* keptScdb = GetStatusChangeDB(statusType))
                HideStatusChangeIcon(bl, *keptScdb);
        }
    }
    sc->Opt1 = 0;
    sc->Opt2 = 0;
    sc->Opt3 = 0;
    UpdateStatusChangeOptions(bl, *sc);
    return 1;
}

void ROWOWMod::StatusChangeClearBuffs(Unit* bl, uint8 type)
{
    // status_change_clear_buffs: clears buffs with specified flag and type
    ROWOWStatusChange* sc = GetStatusChange(bl);
    if (sc == nullptr || sc->IsEmpty() == true)
        return;
    std::vector<int32> types;
    for (auto const& entryByType : sc->EntriesByType)
        types.push_back(entryByType.first);
    for (int32 statusType : types)
    {
        ROWOWStatusChangeEntry* sce = sc->GetSCE(statusType);
        ROWOWStatusChangeDB const* scdb = GetStatusChangeDB(statusType);
        if (sce == nullptr || scdb == nullptr)
            continue;
        std::bitset<ROWOW_SCF_MAX> const& flag = scdb->Flag;
        bool end = false;
        // Skip status with SCF_NOCLEARBUFF, no matter what
        if (flag[ROWOW_SCF_NOCLEARBUFF] == true)
            continue;
        if ((type & ROWOW_SCCB_LUXANIMA) != 0 && flag[ROWOW_SCF_REMOVEONLUXANIMA] == true)
            end = true;
        else if ((type & ROWOW_SCCB_CHEM_PROTECT) != 0 && flag[ROWOW_SCF_REMOVECHEMICALPROTECT] == true) // Clears AM_CP_ARMOR/HELP/SHIELD/WEAPON
            end = true;
        else if ((type & ROWOW_SCCB_REFRESH) != 0 && flag[ROWOW_SCF_REMOVEONREFRESH] == true)
            end = true;
        else if ((type & ROWOW_SCCB_DEBUFFS) != 0 && flag[ROWOW_SCF_DEBUFF] == true) // Clears debuffs
            end = true;
        else if ((type & ROWOW_SCCB_BUFFS) != 0 && flag[ROWOW_SCF_DEBUFF] == false) // Clears buffs - skip if it is a debuff
            end = true;
        else if ((type & ROWOW_SCCB_HERMODE) != 0 && flag[ROWOW_SCF_REMOVEONHERMODE] == true) // Cleared by CG_HERMODE
            end = true;
        if (statusType == ROWOW_SC_BERSERK) // Mark to not lose HP
            sce->Val2 = 0;
        if (end == true)
            StatusChangeEnd(bl, statusType);
    }
}

void ROWOWMod::StatusChangeTimer(Unit* bl, int32 type, uint64 tick)
{
    // status_change_timer: a status's timer ran out.  A ticking status ticks and waits for its next interval, the rest end
    ROWOWStatusChange* sc = GetStatusChange(bl);
    ROWOWStatusChangeEntry* sce = sc != nullptr ? sc->GetSCE(type) : nullptr;
    if (sce == nullptr)
        return;
    int32 interval = StatusGetSCInterval(type);
    Player* sd = bl->ToPlayer();
    auto scTimerNext = [sce](uint64 time) { sce->TimerTime = time; };
    uint32 maxHP = bl->GetMaxHealth();
    uint32 hp = bl->GetHealth();

    switch (type)
    {
        case ROWOW_SC_MAXIMIZEPOWER:
        case ROWOW_SC_CLOAKING:
            if (StatusCharge(bl, 0, 1) == false)
                break; // Not enough SP to continue.
            scTimerNext(uint64(sce->Val2) + tick);
            return;

        case ROWOW_SC_CHASEWALK:
            if (StatusCharge(bl, 0, sce->Val4) == false)
                break; // Not enough SP to continue.
            if (sc->HasSCE(ROWOW_SC_CHASEWALK2) == false)
            {
                ROWOWStatusChangeEntry const* spirit = sc->GetSCE(ROWOW_SC_SPIRIT);
                int64 bonusTime = (spirit != nullptr && spirit->Val2 == ROWOW_SKILL_SL_ROGUE ? 10 : 1); // SL bonus -> x10 duration
                SCStart(bl, bl, ROWOW_SC_CHASEWALK2, 100, 1 << (sce->Val1 - 1), bonusTime * SkillGetTime2(StatusDBGetSkill(type), uint16(sce->Val1)));
                sce = sc->GetSCE(type);
                if (sce == nullptr)
                    return;
            }
            sce->TimerTime = uint64(sce->Val2) + tick;
            return;

        case ROWOW_SC_HIDING:
            if (--(sce->Val2) > 0)
            {
                if (sce->Val2 % sce->Val4 == 0 && StatusCharge(bl, 0, 1) == false)
                    break; // Fail if it's time to substract SP and there isn't.
                scTimerNext(1000 + tick);
                return;
            }
            break;

        case ROWOW_SC_SIGHT:
        case ROWOW_SC_RUWACH:
        case ROWOW_SC_SIGHTBLASTER:
        {
            // Each 20 ms, the units around are revealed (status_change_timer_sub), and the hidden traps (skill_reveal_trap_inarea)
            ROWOWStatusChangeEntry sceCopy = *sce;
            Unit* src = bl;
            MapForEachInRange(bl, sceCopy.Val3, [this, src, &sceCopy, type, tick](Unit* target)
            {
                StatusChangeTimerSub(target, src, &sceCopy, type, tick);
                return 0;
            });
            if (type != ROWOW_SC_SIGHTBLASTER)
            {
                int32 x = 0, y = 0;
                GetUnitCell(bl, x, y);
                SkillRevealTrapInArea(bl, sceCopy.Val3, x, y);
            }
            sce = sc->GetSCE(type);
            if (sce == nullptr)
                return;
            if (--(sce->Val2) > 0)
            {
                sce->Val4 += 20; // Use for Shadow Form 2 seconds checking.
                scTimerNext(20 + tick);
                return;
            }
            break;
        }

        case ROWOW_SC_AUTOBERSERK:
            if (ROWOWStatusChangeEntry const* provoke = sc->GetSCE(ROWOW_SC_PROVOKE); provoke != nullptr && provoke->Val4 == 1)
            {
                // End infinite provoke granted by auto berserk
                if (hp > maxHP / 4)
                    StatusChangeEnd(bl, ROWOW_SC_PROVOKE);
            }
            else if (hp <= maxHP / 4)
                SCStart4(bl, bl, ROWOW_SC_PROVOKE, 100, 10, 0, 0, 1, ROWOW_INFINITE_TICK); // Start infinite provoke granted by auto berserk
            // Repeat check every interval
            sce = sc->GetSCE(type);
            if (sce != nullptr)
                sce->TimerTime = 100 + tick;
            return;

        case ROWOW_SC_STONE:
            if (sce->Val4 >= 0 && hp > maxHP / 4)
                StatusZap(bl, std::max<uint32>(1, maxHP / 100), 0); // status_percent_damage: 1% of the max HP
            break;

        case ROWOW_SC_POISON:
        case ROWOW_SC_DPOISON:
            if (sce->Val4 >= 0 && sc->HasSCE(ROWOW_SC_SLOWPOISON) == false)
            {
                uint32 damage = 0;
                if (sd != nullptr)
                    damage = (type == ROWOW_SC_DPOISON) ? 2 + maxHP / 50 : 2 + maxHP * 3 / 200;
                else
                    damage = (type == ROWOW_SC_DPOISON) ? 2 + maxHP / 100 : 2 + maxHP / 200;
                if (hp > std::max(maxHP / 4, damage)) // Stop damaging after 25% HP left.
                    StatusZap(bl, damage, 0);
            }
            break;

        case ROWOW_SC_BLEEDING:
            if (sce->Val4 >= 0)
            {
                uint32 damage = urand(0, 599) + 200;
                if (sd == nullptr && damage >= hp)
                    damage = hp > 0 ? hp - 1 : 0; // No deadly damage for monsters
                StatusZap(bl, damage, 0);
            }
            break;

        case ROWOW_SC_TOXIN:
            if (sce->Val4 >= 0)
            {
                // Damage is every 10 seconds including 3%sp drain.
                if (sce->Val3 == 1) // Target
                    StatusZap(bl, 1, bl->GetMaxPower(POWER_MANA) * 3 / 100);
                else
                {
                    // Caster.  Assign here since status_get_sc_internval() contains the target interval.
                    interval = 1000;
                    if (bl->GetPower(POWER_MANA) < bl->GetMaxPower(POWER_MANA))
                        StatusHeal(bl, 0, bl->GetMaxPower(POWER_MANA) / 100, 1, bl, 0);
                }
            }
            break;

        case ROWOW_SC_PYREXIA:
            if (sce->Val4 >= 0)
            {
                StatusZap(bl, 100, 0);
                UnitSkillCastCancel(bl, 2);
            }
            break;

        case ROWOW_SC_LEECHESEND:
            if (sce->Val4 >= 0)
            {
                ROWOWStatus const* status = GetROStatus(bl);
                int64 damage = int64(status != nullptr ? status->Stats[ROWOW_STAT_VIT] : 0) * (sce->Val1 - 3) + int32(maxHP) / 100; // {Target VIT x (New Poison Research Skill Level - 3)} + (Target HP/100)
                StatusZap(bl, uint32(std::max<int64>(0, damage)), 0);
                UnitSkillCastCancel(bl, 2);
            }
            break;

        case ROWOW_SC_DEATHHURT:
            if (sce->Val4 >= 0 && hp < maxHP)
                StatusHeal(bl, maxHP / 100, 0, 1, bl, 0);
            break;

        case ROWOW_SC_TENSIONRELAX:
            if (maxHP > hp && --(sce->Val3) >= 0)
            {
                scTimerNext(10000 + tick);
                return;
            }
            break;

        case ROWOW_SC_DANCING:
        {
            // SP consumption by time of dancing skills: 1 SP every few seconds by the dance (Moonlight Petals 4 a level every 10), tripled under
            // Longing for Freedom; it ends when the SP runs out
            int32 s = 0;
            int32 sp = 1;
            if (--sce->Val3 <= 0)
                break;
            switch (sce->Val1 & 0xFFFF)
            {
                case ROWOW_SKILL_BD_RICHMANKIM:
                case ROWOW_SKILL_BD_DRUMBATTLEFIELD:
                case ROWOW_SKILL_BD_RINGNIBELUNGEN:
                case ROWOW_SKILL_BD_SIEGFRIED:
                case ROWOW_SKILL_BA_DISSONANCE:
                case ROWOW_SKILL_BA_ASSASSINCROSS:
                case ROWOW_SKILL_DC_UGLYDANCE:
                    s = 3;
                    break;
                case ROWOW_SKILL_BD_LULLABY:
                case ROWOW_SKILL_BD_ETERNALCHAOS:
                case ROWOW_SKILL_BD_ROKISWEIL:
                case ROWOW_SKILL_DC_FORTUNEKISS:
                    s = 4;
                    break;
                case ROWOW_SKILL_CG_HERMODE:
                case ROWOW_SKILL_BD_INTOABYSS:
                case ROWOW_SKILL_BA_WHISTLE:
                case ROWOW_SKILL_DC_HUMMING:
                case ROWOW_SKILL_BA_POEMBRAGI:
                case ROWOW_SKILL_DC_SERVICEFORYOU:
                    s = 5;
                    break;
                case ROWOW_SKILL_BA_APPLEIDUN:
                    s = 6;
                    break;
                case ROWOW_SKILL_CG_MOONLIT:
                    // Moonlit's cost is 4sp*skill_lv [Skotlex]
                    sp = 4 * (sce->Val1 >> 16);
                    // Upkeep is also every 10 secs.
                    [[fallthrough]];
                case ROWOW_SKILL_DC_DONTFORGETME:
                    s = 10;
                    break;
            }
            if (s != 0 && sce->Val3 % s == 0)
            {
                if (sc->HasSCE(ROWOW_SC_LONGING) == true)
                    sp *= 3;
                if (StatusCharge(bl, 0, sp) == false)
                    break;
                // If no more SP left, the effect ends immediately
                if (bl->GetPower(POWER_MANA) == 0)
                    break;
            }
            scTimerNext(1000 + tick);
            return;
        }

        case ROWOW_SC_WHISTLE:
        case ROWOW_SC_ASSNCROS:
        case ROWOW_SC_POEMBRAGI:
        case ROWOW_SC_APPLEIDUN:
        case ROWOW_SC_HUMMING:
        case ROWOW_SC_DONTFORGETME:
        case ROWOW_SC_FORTUNE:
        case ROWOW_SC_SERVICE4U:
            if (BattleConfig.RefreshSong == 0 && bl->IsAlive() == true)
            {
                // Most songs have no interval, but they can expire while the character is still standing in the area of effect.  We need to
                // make sure to restore the duration here if that's the case
                ROWOWStatusChangeDB const* scdb = GetStatusChangeDB(type);
                if (scdb == nullptr)
                    break;
                int32 x = 0, y = 0, index = -1;
                GetUnitCell(bl, x, y);
                std::shared_ptr<ROWOWSkillUnitGroup> group = MapFindSkillUnitOnCell(bl, x, y, scdb->SkillID, 1, index);
                // No longer in area
                if (group == nullptr || group->Units[index].IsAlive == false)
                    break;
                // Dissonance marker is set
                if ((group->Units[index].Val2 & (1 << ROWOW_UF_ENSEMBLE)) != 0)
                    break;
                // Standing in the area, but cannot affect self
                ROWOWStatusChangeEntry const* spirit = sc->GetSCE(ROWOW_SC_SPIRIT);
                if (group->SrcGUID == bl->GetGUID() && (spirit == nullptr || spirit->Val2 != ROWOW_SKILL_SL_BARDDANCER))
                    break;
                // Restore pseudo-infinite duration
                sce->Val4 = 0;
                scTimerNext(uint64(group->Units[index].Limit) + tick);
                return;
            }
            break;

        case ROWOW_SC_MARIONETTE:
        case ROWOW_SC_MARIONETTE2:
        {
            // Lasts on while the two stay within 7 cells of each other
            Unit* pbl = MapID2BL(bl, sce->Val1);
            if (pbl != nullptr && DistanceBL(bl, pbl) <= 7)
            {
                scTimerNext(1000 + tick);
                return;
            }
            break;
        }

        case ROWOW_SC_BERSERK:
            // 5% every 10 seconds [DracoRPG]
            if (--(sce->Val3) > 0 && StatusCharge(bl, sce->Val2, 0) == true && bl->GetHealth() > 100)
            {
                scTimerNext(uint64(sce->Val4) + tick);
                return;
            }
            break;

        case ROWOW_SC_GOSPEL:
            if (sce->Val4 == ROWOW_BCT_SELF && --(sce->Val2) > 0)
            {
                int32 gospelHP = (sce->Val1 > 5) ? 45 : 30;
                int32 gospelSP = (sce->Val1 > 5) ? 35 : 20;
                if (StatusCharge(bl, gospelHP, gospelSP) == false)
                    break;
                scTimerNext(10000 + tick);
                return;
            }
            break;

        case ROWOW_SC_BLIND:
            if (sc->HasSCE(ROWOW_SC_FOGWALL) == true)
            {
                // Blind lasts forever while you are standing on the fog.
                scTimerNext(5000 + tick);
                return;
            }
            break;

        case ROWOW_SC_SPLASHER:
            // Venom Splasher's countdown
            if ((sce->Val4 -= 500) > 0)
            {
                scTimerNext(500 + tick);
                return;
            }
            break;

        case ROWOW_SC_STOMACHACHE:
            if (--(sce->Val4) >= 0)
            {
                StatusCharge(bl, 0, sce->Val2); // Reduce 8 every 10 seconds.
                if (sd != nullptr && sd->IsSitState() == false) // Force to sit every 10 seconds.
                    sd->SetStandState(UNIT_STAND_STATE_SIT);
                scTimerNext(10000 + tick);
                return;
            }
            break;
    }

    // If status has an interval and there is at least 100ms remaining time, wait for next interval
    sce = sc->GetSCE(type);
    if (interval > 0 && sce != nullptr && sce->Val4 >= 100)
    {
        scTimerNext(uint64(std::min(sce->Val4, interval)) + tick);
        sce->Val4 -= interval;
        return;
    }

    // Default for all non-handled control paths is to end the status
    StatusChangeEnd(bl, type, true);
}

void ROWOWMod::StatusChangeTimerSub(Unit* bl, Unit* src, ROWOWStatusChangeEntry const* sce, int32 type, uint64 tick)
{
    // status_change_timer_sub: what Sight, Ruwach and Sight Blaster do to each unit around (revealing the hidden), and a Close Confine holder
    // letting go of those it held
    if (bl->IsAlive() == false)
        return;
    ROWOWStatusChange* tsc = GetStatusChange(bl);
    switch (type)
    {
        case ROWOW_SC_CLOSECONFINE:
            // Lock char has released the hold on everyone...
            if (ROWOWStatusChangeEntry* sce2 = tsc != nullptr ? tsc->GetSCE(ROWOW_SC_CLOSECONFINE2) : nullptr; sce2 != nullptr && sce2->Val2 == int32(src->GetGUID().GetCounter()))
            {
                sce2->Val2 = 0;
                StatusChangeEnd(bl, ROWOW_SC_CLOSECONFINE2);
            }
            break;
        case ROWOW_SC_SIGHT: // Reveal hidden ennemy on 3*3 range
        case ROWOW_SC_CONCENTRATE:
            StatusChangeEnd(bl, ROWOW_SC_HIDING);
            StatusChangeEnd(bl, ROWOW_SC_CLOAKING);
            break;
        case ROWOW_SC_RUWACH: // Reveal hidden target and deal little dammages if enemy
            if (tsc != nullptr && (tsc->HasSCE(ROWOW_SC_HIDING) == true || tsc->HasSCE(ROWOW_SC_CLOAKING) == true))
            {
                StatusChangeEnd(bl, ROWOW_SC_HIDING);
                StatusChangeEnd(bl, ROWOW_SC_CLOAKING);
                if (BattleCheckTarget(src, bl, ROWOW_BCT_ENEMY) > 0)
                    SkillAttack(ROWOW_BF_MAGIC, src, src, bl, ROWOW_SKILL_AL_RUWACH, 1, tick, 0);
            }
            break;
        case ROWOW_SC_SIGHTBLASTER:
            if (BattleCheckTarget(src, bl, ROWOW_BCT_ENEMY) > 0 && StatusCheckSkillUse(src, bl, ROWOW_SKILL_WZ_SIGHTBLASTER, 2) == true && sce != nullptr)
            {
                if (SkillAttack(ROWOW_BF_MAGIC, src, src, bl, ROWOW_SKILL_WZ_SIGHTBLASTER, uint16(sce->Val1), tick, 0x1000000) != 0)
                    if (ROWOWStatusChangeEntry* sourceSce = GetStatusChange(src) != nullptr ? GetStatusChange(src)->GetSCE(ROWOW_SC_SIGHTBLASTER) : nullptr)
                        sourceSce->Val2 = 0; // This signals it to end.
            }
            break;
    }
}

void ROWOWMod::StatusCalcState(Unit* bl, ROWOWStatusChange& sc, ROWOWStatusChangeDB const& scdb, bool start)
{
    // status_calc_state: what the statuses keep the unit from doing (status_change's cant), from status.yml's States and the conditional
    // ones decided here, then those states on the unit through WoW
    if (sc.IsEmpty() == true)
        sc.Cant = ROWOWStatusChangeCant();
    else
    {
        // status_calc_state_sub: starting an unconditional state sets it; otherwise it's on while any status still has it
        auto calcStateSub = [this, bl, &sc, &scdb, start](bool& restriction, uint8 unconditional, uint8 conditional,
            std::function<bool(int32, ROWOWStatusChangeEntry const&)> const& isConditionMet)
        {
            if (start == true && scdb.State[unconditional] == true)
            {
                restriction = true;
                return;
            }
            restriction = false;
            for (auto const& entryByType : sc.EntriesByType)
            {
                ROWOWStatusChangeDB const* otherScdb = GetStatusChangeDB(entryByType.first);
                if (otherScdb == nullptr)
                    continue;
                if (otherScdb->State[unconditional] == true || (otherScdb->State[conditional] == true && isConditionMet(entryByType.first, entryByType.second) == true))
                {
                    restriction = true;
                    return;
                }
            }
        };
        auto noConditions = [](int32, ROWOWStatusChangeEntry const&) { return false; };

        // Can't move
        if (scdb.State[ROWOW_SCS_NOMOVE] == true || scdb.State[ROWOW_SCS_NOMOVECOND] == true)
        {
            calcStateSub(sc.Cant.Move, ROWOW_SCS_NOMOVE, ROWOW_SCS_NOMOVECOND, [this, bl, &sc](int32 type, ROWOWStatusChangeEntry const& sce)
            {
                switch (type)
                {
                    case ROWOW_SC_GOSPEL:
                        return sce.Val4 == ROWOW_BCT_SELF; // Cannot move while gospel is in effect
                    case ROWOW_SC_BASILICA:
                        return sce.Val4 == int32(bl->GetGUID().GetCounter()); // Basilica caster cannot move
                    case ROWOW_SC_GRAVITATION:
                        return sce.Val3 == ROWOW_BCT_SELF;
                    case ROWOW_SC_FEAR:
                        return sce.Val2 > 0;
                    case ROWOW_SC_HIDING:
                        return bl->IsPlayer() == false || PCCheckSkill(bl, ROWOW_SKILL_RG_TUNNELDRIVE) <= 0;
                    case ROWOW_SC_DANCING:
                        // In Pre-Renewal all Ensambles should apply the restriction.  Unless you have SC_LONGING, then the restiction only
                        // applies for CG_MOONLIT and CG_HERMODE
                        if (sce.Val4 == 0)
                            return false;
                        return sc.HasSCE(ROWOW_SC_LONGING) == false || (sce.Val1 & 0xFFFF) == ROWOW_SKILL_CG_MOONLIT || (sce.Val1 & 0xFFFF) == ROWOW_SKILL_CG_HERMODE;
                    default:
                        return false; // SC_LONGING is just defined with SCS_NOMOVECOND to trigger a recalculation with the conditions under SC_DANCING above
                }
            });
        }

        // Can't use skills
        if (scdb.State[ROWOW_SCS_NOCAST] == true || scdb.State[ROWOW_SCS_NOCASTCOND] == true)
        {
            calcStateSub(sc.Cant.Cast, ROWOW_SCS_NOCAST, ROWOW_SCS_NOCASTCOND, [bl](int32 type, ROWOWStatusChangeEntry const& sce)
            {
                switch (type)
                {
                    case ROWOW_SC_WINKCHARM:
                        return bl->IsPlayer() == false;
                    case ROWOW_SC_OBLIVIONCURSE:
                        return sce.Val3 == 1;
                    default:
                        return false;
                }
            });
        }

        // Can't chat
        if (scdb.State[ROWOW_SCS_NOCHAT] == true || scdb.State[ROWOW_SCS_NOCHATCOND] == true)
            calcStateSub(sc.Cant.Chat, ROWOW_SCS_NOCHAT, ROWOW_SCS_NOCHATCOND, noConditions);

        // Can't attack
        if (scdb.State[ROWOW_SCS_NOATTACK] == true || scdb.State[ROWOW_SCS_NOATTACKCOND] == true)
        {
            calcStateSub(sc.Cant.Attack, ROWOW_SCS_NOATTACK, ROWOW_SCS_NOATTACKCOND, [&sc](int32 type, ROWOWStatusChangeEntry const& sce)
            {
                // Normal songs prevent attacks; Ensembles prevent attacks as long Longing is not active
                return type == ROWOW_SC_DANCING && (sce.Val4 == 0 || sc.HasSCE(ROWOW_SC_LONGING) == false);
            });
        }

        // Can't warp
        if (scdb.State[ROWOW_SCS_NOWARP] == true || scdb.State[ROWOW_SCS_NOWARPCOND] == true)
            calcStateSub(sc.Cant.Warp, ROWOW_SCS_NOWARP, ROWOW_SCS_NOWARPCOND, noConditions);

        // Player-only states
        if (bl->IsPlayer() == true)
        {
            if (scdb.State[ROWOW_SCS_NOPICKITEM] == true || scdb.State[ROWOW_SCS_NOPICKITEMCOND] == true)
                calcStateSub(sc.Cant.PickUp, ROWOW_SCS_NOPICKITEM, ROWOW_SCS_NOPICKITEMCOND, noConditions);
            if (scdb.State[ROWOW_SCS_NODROPITEM] == true || scdb.State[ROWOW_SCS_NODROPITEMCOND] == true)
                calcStateSub(sc.Cant.Drop, ROWOW_SCS_NODROPITEM, ROWOW_SCS_NODROPITEMCOND, noConditions);
            if (scdb.State[ROWOW_SCS_NOEQUIPITEM] == true || scdb.State[ROWOW_SCS_NOEQUIPITEMCOND] == true)
                calcStateSub(sc.Cant.Equip, ROWOW_SCS_NOEQUIPITEM, ROWOW_SCS_NOEQUIPITEMCOND, noConditions);
            if (scdb.State[ROWOW_SCS_NOUNEQUIPITEM] == true || scdb.State[ROWOW_SCS_NOUNEQUIPITEMCOND] == true)
                calcStateSub(sc.Cant.Unequip, ROWOW_SCS_NOUNEQUIPITEM, ROWOW_SCS_NOUNEQUIPITEMCOND, noConditions);
            if (scdb.State[ROWOW_SCS_NOCONSUMEITEM] == true || scdb.State[ROWOW_SCS_NOCONSUMEITEMCOND] == true)
            {
                calcStateSub(sc.Cant.Consume, ROWOW_SCS_NOCONSUMEITEM, ROWOW_SCS_NOCONSUMEITEMCOND, [](int32 type, ROWOWStatusChangeEntry const& sce)
                {
                    return type == ROWOW_SC_GRAVITATION && sce.Val3 == ROWOW_BCT_SELF;
                });
            }
            if (scdb.State[ROWOW_SCS_NODEATHPENALTY] == true || scdb.State[ROWOW_SCS_NODEATHPENALTYCOND] == true)
                calcStateSub(sc.Cant.DeathPenalty, ROWOW_SCS_NODEATHPENALTY, ROWOW_SCS_NODEATHPENALTYCOND, noConditions);
            if (scdb.State[ROWOW_SCS_NOINTERACT] == true || scdb.State[ROWOW_SCS_NOINTERACTCOND] == true)
                calcStateSub(sc.Cant.Interact, ROWOW_SCS_NOINTERACT, ROWOW_SCS_NOINTERACTCOND, noConditions);
        }
    }
    UpdateStatusChangeOptions(bl, sc);
}

void ROWOWMod::UpdateStatusChangeOptions(Unit* bl, ROWOWStatusChange& sc)
{
    // What the client is told of the statuses' states (clif_changeoption's part): a unit that can't move is rooted, one that can't use skills
    // is silenced, one that can't attack is pacified (WoW's own states, set without an aura), and the hidden ones (Hiding, Cloaking, Chase
    // Walk) are stealthed, so only detectors (and Ruwach and Sight, which end it) find them
    bool isDead = bl->IsAlive() == false;
    bool shouldRoot = sc.Cant.Move == true && isDead == false;
    if (shouldRoot != sc.IsRootSet)
    {
        sc.IsRootSet = shouldRoot;
        bl->SetControlled(shouldRoot, UNIT_STATE_ROOT);
    }
    bool shouldSilence = sc.Cant.Cast == true && isDead == false;
    if (shouldSilence != sc.IsSilenceSet)
    {
        sc.IsSilenceSet = shouldSilence;
        if (shouldSilence == true)
            bl->SetUnitFlag(UNIT_FLAG_SILENCED);
        else
            bl->RemoveUnitFlag(UNIT_FLAG_SILENCED);
    }
    bool shouldPacify = sc.Cant.Attack == true && isDead == false;
    if (shouldPacify != sc.IsPacifySet)
    {
        sc.IsPacifySet = shouldPacify;
        if (shouldPacify == true)
        {
            bl->SetUnitFlag(UNIT_FLAG_PACIFIED);
            bl->AttackStop();
        }
        else
            bl->RemoveUnitFlag(UNIT_FLAG_PACIFIED);
    }
    bool shouldStealth = (sc.Option & (ROWOW_OPTION_HIDE | ROWOW_OPTION_CLOAK | ROWOW_OPTION_CHASEWALK)) != 0 && isDead == false;
    if (shouldStealth != sc.IsStealthSet)
    {
        sc.IsStealthSet = shouldStealth;
        if (shouldStealth == true)
        {
            bl->m_stealth.AddFlag(STEALTH_GENERAL);
            bl->m_stealth.AddValue(STEALTH_GENERAL, ROWOW_HIDING_STEALTH_VALUE);
            bl->SetStandFlags(UNIT_STAND_FLAGS_CREEP);
            if (bl->IsPlayer() == true)
                bl->SetByteFlag(PLAYER_FIELD_BYTES2, 3, PLAYER_FIELD_BYTE2_STEALTH);
            bl->AttackStop();
        }
        else
        {
            bl->m_stealth.AddValue(STEALTH_GENERAL, -ROWOW_HIDING_STEALTH_VALUE);
            if (bl->m_stealth.GetValue(STEALTH_GENERAL) <= 0)
            {
                bl->m_stealth.DelFlag(STEALTH_GENERAL);
                bl->RemoveStandFlags(UNIT_STAND_FLAGS_CREEP);
                if (bl->IsPlayer() == true)
                    bl->RemoveByteFlag(PLAYER_FIELD_BYTES2, 3, PLAYER_FIELD_BYTE2_STEALTH);
            }
        }
        if (bl->IsInWorld() == true)
            bl->UpdateObjectVisibility(true, true);
    }
}

void ROWOWMod::ShowStatusChangeIcon(Unit* bl, ROWOWStatusChangeDB const& scdb, ROWOWStatusChangeEntry const& sce)
{
    // clif_status_change: the status's marker aura, running for what's left of the status (endless for an endless one)
    if (scdb.SpellID == 0 || bl->IsAlive() == false)
        return;
    Aura* aura = bl->GetAura(scdb.SpellID);
    if (aura == nullptr)
        aura = bl->AddAura(scdb.SpellID, bl);
    if (aura == nullptr)
        return;
    uint64 now = GetTick();
    int32 duration = sce.EndTime == 0 ? -1 : int32(std::min<uint64>(sce.EndTime > now ? sce.EndTime - now : 1, uint64(INT32_MAX)));
    aura->SetMaxDuration(duration);
    aura->SetDuration(duration);
}

void ROWOWMod::HideStatusChangeIcon(Unit* bl, ROWOWStatusChangeDB const& scdb)
{
    if (scdb.SpellID != 0)
        bl->RemoveAurasDueToSpell(scdb.SpellID);
}

void ROWOWMod::SendStatusChangesToClient(Unit* unit)
{
    // Every status's marker aura again, and none for a status the unit doesn't have (a form change, a respawn, or markers the character
    // saved with its own auras)
    ROWOWStatusChange* sc = GetStatusChange(unit);
    for (auto const& scdbByType : StatusChangeDBsByType)
    {
        ROWOWStatusChangeEntry const* sce = sc != nullptr ? sc->GetSCE(scdbByType.first) : nullptr;
        if (sce != nullptr)
            ShowStatusChangeIcon(unit, scdbByType.second, *sce);
        else if (scdbByType.second.SpellID != 0 && unit->HasAura(scdbByType.second.SpellID) == true)
            HideStatusChangeIcon(unit, scdbByType.second);
    }
    if (sc != nullptr)
    {
        // A form change can leave the WoW states behind: set them again from the statuses
        sc->IsRootSet = sc->IsSilenceSet = sc->IsPacifySet = false;
        sc->IsStealthSet = false;
        if (unit->HasUnitState(UNIT_STATE_ROOT) == true && sc->Cant.Move == false)
            unit->SetControlled(false, UNIT_STATE_ROOT);
        unit->RemoveUnitFlag(UNIT_FLAG_SILENCED);
        unit->RemoveUnitFlag(UNIT_FLAG_PACIFIED);
        UpdateStatusChangeOptions(unit, *sc);
    }
}

void ROWOWMod::UpdateStatusChanges(Unit* unit)
{
    // status_change_timer for each of the unit's statuses whose timer ran out, in the unit's own update.  Death clears them (status_damage's
    // status_change_clear), lastEffect is cleared 10 ms after it was set (status_clear_lastEffect_timer), and Cloaking follows the walls
    ROWOWStatusChange* sc = GetStatusChange(unit);
    if (sc == nullptr)
        return;
    uint64 tick = GetTick();
    if (sc->LastEffect != -1 && sc->LastEffectTime <= tick)
        sc->LastEffect = -1;
    if (sc->IsEmpty() == true)
        return;
    if (unit->IsAlive() == false)
    {
        StatusChangeClear(unit, 0);
        return;
    }

    // map_moveblock: Cloaking checks its wall each time the unit steps onto another cell
    if (ROWOWStatusChangeEntry* sce = sc->GetSCE(ROWOW_SC_CLOAKING))
    {
        int32 cellX = 0, cellY = 0;
        GetUnitCell(unit, cellX, cellY);
        if (cellX != sc->CloakCellX || cellY != sc->CloakCellY)
        {
            sc->CloakCellX = cellX;
            sc->CloakCellY = cellY;
            SkillCheckCloaking(unit, sce);
        }
    }
    std::vector<int32> dueTypes;
    for (auto const& entryByType : sc->EntriesByType)
        if (entryByType.second.TimerTime != 0 && entryByType.second.TimerTime <= tick)
            dueTypes.push_back(entryByType.first);
    for (int32 type : dueTypes)
    {
        ROWOWStatusChangeEntry const* sce = sc->GetSCE(type);
        if (sce == nullptr || sce->TimerTime == 0 || sce->TimerTime > tick)
            continue;
        StatusChangeTimer(unit, type, sce->TimerTime);
        if (unit->IsAlive() == false)
            return;
    }
}

void ROWOWMod::StatusDamaged(Unit* attacker, Unit* target, uint32 damage)
{
    // status_damage's statuses, when a hit lands on an RO combatant: statuses that end when hit (sleep, freeze, stone...), Endure's count of
    // hits from monsters, and Gravitation and Dancing broken off
    ROWOWStatusChange* sc = GetStatusChange(target);
    if (sc == nullptr || sc->IsEmpty() == true)
        return;
    std::vector<int32> types;
    for (auto const& entryByType : sc->EntriesByType)
        types.push_back(entryByType.first);
    for (int32 type : types)
    {
        // For non-players, Wink Charm ends only when damage was dealt (e.g. Wink Charm does not end itself).  For players, it ends even if no
        // damage was dealt (e.g. Provoke ends it on players but not on monsters).  Other status changes end even on 0 damage
        if (type == ROWOW_SC_WINKCHARM && target->IsPlayer() == false && damage == 0)
            continue;
        ROWOWStatusChangeDB const* scdb = GetStatusChangeDB(type);
        if (scdb == nullptr || scdb->Flag[ROWOW_SCF_REMOVEONDAMAGED] == false || sc->HasSCE(type) == false)
            continue;
        // A status change that gets broken by damage should still be considered when calculating if a status change can be applied or not
        // (for the same attack)
        if (sc->Opt1 > ROWOW_OPT1_NONE && sc->LastEffect == -1)
        {
            sc->LastEffectTime = GetTick() + 10;
            sc->LastEffect = type;
        }
        StatusChangeEnd(target, type);
    }
    if (ROWOWStatusChangeEntry* endure = sc->GetSCE(ROWOW_SC_ENDURE); endure != nullptr && endure->Val4 == 0)
    {
        // Endure count is only reduced by non-players on non-gvg maps.  val4 signals infinite endure.
        if (attacker != nullptr && attacker->IsPlayer() == false && --(endure->Val2) <= 0)
            StatusChangeEnd(target, ROWOW_SC_ENDURE);
    }
    if (sc->HasSCE(ROWOW_SC_DANCING) == true && damage > target->GetMaxHealth() / 4)
        StatusChangeEnd(target, ROWOW_SC_DANCING);
}

bool ROWOWMod::StatusCheckVisibility(Unit const* src, Unit const* target) const
{
    // status_check_visibility: whether the src can see the target (hidden ones only by detectors)
    ROWOWStatusChange const* tsc = GetStatusChange(target);
    if (tsc == nullptr || (tsc->Option & (ROWOW_OPTION_HIDE | ROWOW_OPTION_CLOAK | ROWOW_OPTION_CHASEWALK)) == 0)
        return true;
    ROWOWStatus const* status = GetROStatus(src);
    return status != nullptr && ((status->Mode & ROWOW_MODE_DETECTOR) != 0 || status->Class == ROWOW_CLASS_BOSS);
}

void ROWOWMod::StatusChangeSaveNeeded(Player* player)
{
    // The identity's statuses changed, so its next save writes them (sc_data)
    if (ROWOWIdentity* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity"))
        identity->AreStatusChangesChanged = true;
}

void ROWOWMod::StoreIdentityStatusChanges(Player* player, ROWOWIdentity& identity)
{
    // chrif_save_scdata: what's left of each status the identity keeps (not NoSave, nor endless NoSaveInfinite), as rAthena saves on logout
    ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(player));
    identity.SavedStatusChanges.clear();
    if (sc == nullptr)
        return;
    uint64 tick = GetTick();
    for (auto const& entryByType : sc->EntriesByType)
    {
        ROWOWStatusChangeDB const* scdb = GetStatusChangeDB(entryByType.first);
        if (scdb == nullptr || scdb->Flag[ROWOW_SCF_NOSAVE] == true)
            continue;
        ROWOWStatusChangeEntry const& sce = entryByType.second;
        if (sce.TimerTime == 0 && scdb->Flag[ROWOW_SCF_NOSAVEINFINITE] == true)
            continue;
        ROWOWSavedStatusChange saved;
        saved.Type = entryByType.first;
        saved.Tick = sce.TimerTime == 0 ? -1 : int64(sce.TimerTime > tick ? sce.TimerTime - tick : 0);
        saved.Val1 = sce.Val1;
        saved.Val2 = sce.Val2;
        saved.Val3 = sce.Val3;
        saved.Val4 = sce.Val4;
        identity.SavedStatusChanges.push_back(saved);
    }
}

void ROWOWMod::SaveIdentityStatusChanges(Player* player, ROWOWIdentity& identity, CharacterDatabaseTransaction trans)
{
    // The identity's statuses into mod_rowow_identity_status: the form's current ones, or the ones kept while out of it
    if (identity.IsInForm == true)
        StoreIdentityStatusChanges(player, identity);
    else if (identity.AreStatusChangesChanged == false)
        return;
    uint32 guid = player->GetGUID().GetCounter();
    trans->Append("DELETE FROM `mod_rowow_identity_status` WHERE `guid` = {}", guid);
    for (ROWOWSavedStatusChange const& saved : identity.SavedStatusChanges)
        trans->Append("INSERT INTO `mod_rowow_identity_status` (`guid`, `type`, `tick`, `val1`, `val2`, `val3`, `val4`) VALUES ({}, {}, {}, {}, {}, {}, {})", guid,
            saved.Type, saved.Tick, saved.Val1, saved.Val2, saved.Val3, saved.Val4);
    identity.AreStatusChangesChanged = false;
}

void ROWOWMod::LeaveStatusChanges(Player* player, ROWOWIdentity& identity)
{
    // Leaving the RO world is RO's logout (map_quit): the statuses are kept as they are (sc_data), then cleared off the character
    // (status_change_clear type 1).  debuff_on_logout 0 keeps neither debuffs nor buffs from ending while away
    StoreIdentityStatusChanges(player, identity);
    identity.AreStatusChangesChanged = true;
    StatusChangeClear(player, 1);
}

void ROWOWMod::RestoreStatusChanges(Player* player, ROWOWIdentity& identity)
{
    // status_change_start of each saved status (as chrif_load_scdata does on login), with what was left of it, once the form is in the world
    std::vector<ROWOWSavedStatusChange> savedStatusChanges = identity.SavedStatusChanges;
    for (ROWOWSavedStatusChange const& saved : savedStatusChanges)
        StatusChangeStart(player, player, saved.Type, 10000, saved.Val1, saved.Val2, saved.Val3, saved.Val4, saved.Tick,
            ROWOW_SCSTART_NOAVOID | ROWOW_SCSTART_NOTICKDEF | ROWOW_SCSTART_LOADED | ROWOW_SCSTART_NORATEDEF);
    SendStatusChangesToClient(player);
}

void ROWOWMod::StatusChangeClearOnChangeMap(Unit* bl)
{
    // status_change_clear_onChangeMap: the statuses that end when the unit goes to another map
    ROWOWStatusChange* sc = GetStatusChange(bl);
    if (sc == nullptr || sc->IsEmpty() == true)
        return;
    std::vector<int32> types;
    for (auto const& entryByType : sc->EntriesByType)
    {
        ROWOWStatusChangeDB const* scdb = GetStatusChangeDB(entryByType.first);
        if (scdb != nullptr && scdb->Flag[ROWOW_SCF_REMOVEONCHANGEMAP] == true)
            types.push_back(entryByType.first);
    }
    for (int32 type : types)
        StatusChangeEnd(bl, type);
}

void ROWOWMod::StatusZap(Unit* bl, uint32 hp, uint32 sp)
{
    // status_zap: HP and SP lost without an attacker and without a number shown (the damage of poison, bleeding, stone).  It can kill
    if (bl->IsAlive() == false)
        return;
    if (sp > 0 && bl->GetMaxPower(POWER_MANA) > 0)
        bl->SetPower(POWER_MANA, bl->GetPower(POWER_MANA) > sp ? bl->GetPower(POWER_MANA) - sp : 0);
    if (hp == 0)
        return;
    // Passive damage (status_damage's flag&1): it ends no status that ends when hit, and stops no cast
    ROWOWStatusChange* sc = bl->CustomData.GetDefault<ROWOWStatusChange>("ROWOWStatusChange");
    sc->IsPassiveDamage = true;
    Unit::DealDamage(bl, bl, hp, nullptr, SELF_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
    sc->IsPassiveDamage = false;
}

bool ROWOWMod::StatusCharge(Unit* bl, int64 hp, int64 sp)
{
    // status_charge: pays HP and SP, false when there isn't enough (HP can't be paid down to 0)
    if (hp > 0 && int64(bl->GetHealth()) <= hp)
        return false;
    if (sp > 0 && int64(bl->GetPower(POWER_MANA)) < sp)
        return false;
    if (hp > 0)
        bl->SetHealth(bl->GetHealth() - uint32(hp));
    if (sp > 0)
        bl->SetPower(POWER_MANA, bl->GetPower(POWER_MANA) - uint32(sp));
    return true;
}
