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
//  Ported from rAthena (src/map/pc.cpp, itemdb.cpp, script.cpp), pre-renewal code paths:
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
//  For more information, see LICENCE in the main folder

#include "Chat.h"
#include "Creature.h"
#include "GameTime.h"
#include "Group.h"
#include "Item.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "WorldSession.h"

#include "ROWOW.h"
#include "ROWOW_Script.h"

// RO items: the item's own state (refine, broken, identified) in its permanent enchantment, equipping and taking off RO equipment in the
// RO equipment positions (pc_isequip, pc_equippoint, pc_equipitem, pc_unequipitem, pc_checkitem), and using items (pc_isUseitem,
// pc_useitem) through their scripts, with the item commands of rAthena's script engine.  Runs in the player's own thread (its session's
// packets, its map's update); the item data and compiled scripts are loaded at startup and only read after

namespace
{
    // item_db's ammo SubTypes (e_ammo_type) the equipping rules check
    constexpr uint8 AMMO_TYPE_DAGGER = 2;

    // The skills itemskill has a way of its own for: the Fly and Butterfly Wings' Teleport, and the Magnifier's Item Appraisal
    std::pair<char const*, uint16> const ItemSkills[] = { { "AL_TELEPORT", ROWOW_SKILL_AL_TELEPORT }, { "MC_IDENTIFY", ROWOW_SKILL_MC_IDENTIFY } };

    uint16 GetItemSkillID(ROWOWScriptValue const& skill)
    {
        if (skill.IsString == false)
            return uint16(skill.Number);
        for (auto const& itemSkill : ItemSkills)
            if (skill.Text == itemSkill.first)
                return itemSkill.second;
        return ROWOW->GetSkillIDByName(skill.Text);
    }

    std::string ToUpper(std::string text)
    {
        for (char& curChar : text)
            curChar = char(toupper((unsigned char)curChar));
        return text;
    }

    std::string ToLower(std::string text)
    {
        for (char& curChar : text)
            curChar = char(tolower((unsigned char)curChar));
        return text;
    }

    void SendItemMessage(Player* player, std::string const& message)
    {
        ChatHandler(player->GetSession()).SendSysMessage(message);
    }

    // Whether a weapon type shoots ammo of an ammo type (pc_isequip's ammo checks)
    bool IsAmmoForWeaponType(uint8 ammoType, uint8 weaponType)
    {
        switch (ammoType)
        {
            case ROWOW_AMMO_TYPE_ARROW:
                return weaponType == ROWOW_WEAPON_TYPE_BOW || weaponType == ROWOW_WEAPON_TYPE_MUSICAL || weaponType == ROWOW_WEAPON_TYPE_WHIP;
            case ROWOW_AMMO_TYPE_BULLET:
            case ROWOW_AMMO_TYPE_SHELL:
                return weaponType == ROWOW_WEAPON_TYPE_REVOLVER || weaponType == ROWOW_WEAPON_TYPE_RIFLE || weaponType == ROWOW_WEAPON_TYPE_GATLING
                    || weaponType == ROWOW_WEAPON_TYPE_SHOTGUN;
            case ROWOW_AMMO_TYPE_GRENADE:
                return weaponType == ROWOW_WEAPON_TYPE_GRENADE;
            default:
                return true;
        }
    }

    // The item a script's argument names: an item_db Id, or an AegisName
    uint32 GetScriptItemID(ROWOWScriptValue const& item)
    {
        if (item.IsString == false)
            return uint32(item.Number);
        auto itemIDIter = ROWOW->ItemIDsByAegisName.find(ToLower(item.Text));
        return itemIDIter != ROWOW->ItemIDsByAegisName.end() ? itemIDIter->second : 0;
    }

    // The group a script's argument names: an IG_ constant (a symbol), or its name
    std::string GetScriptItemGroupName(ROWOWScriptValue const& group)
    {
        std::string groupName = group.IsString == true ? group.Text : ROWOWSCRIPT->GetSymbolName(group.Number);
        if (groupName.size() > 3 && ToUpper(groupName.substr(0, 3)) == "IG_")
            groupName = groupName.substr(3);
        return ToUpper(groupName);
    }

    int32 GetArgumentNumber(std::vector<ROWOWScriptValue> const& arguments, size_t index, int32 defaultValue)
    {
        return index < arguments.size() ? int32(arguments[index].GetNumber()) : defaultValue;
    }

    // ItemGroupDatabase::get_random_itemsubgroup: an entry of a subgroup picked by its algorithm, or null
    ROWOWItemGroupEntry const* GetRandomItemSubGroupEntry(ROWOWItemSubGroup& subGroup)
    {
        if (subGroup.Entries.empty() == true)
            return nullptr;
        switch (subGroup.Algorithm)
        {
            case ROWOW_ITEM_GROUP_ALGORITHM_DROP:
            {
                // A random entry, which then has its rate (of 10000) to come out
                ROWOWItemGroupEntry const& entry = subGroup.Entries[urand(0, uint32(subGroup.Entries.size() - 1))];
                return urand(1, 10000) <= entry.Rate ? &entry : nullptr;
            }
            case ROWOW_ITEM_GROUP_ALGORITHM_ALL:
                return &subGroup.Entries[urand(0, uint32(subGroup.Entries.size() - 1))];
            case ROWOW_ITEM_GROUP_ALGORITHM_RANDOM:
            {
                // Each entry has its rate's worth of positions, and a random position picks one
                if (subGroup.TotalRate == 0)
                    return nullptr;
                uint32 position = urand(1, subGroup.TotalRate);
                uint32 currentPosition = 1;
                for (ROWOWItemGroupEntry const& entry : subGroup.Entries)
                {
                    currentPosition += entry.Rate;
                    if (currentPosition > position)
                        return &entry;
                }
                return nullptr;
            }
            default:
            {
                // A shared pool: each entry has its rate's worth of positions, one used up each time it comes out, until all are used up and
                // the pool fills again
                if (subGroup.TotalRate == 0)
                    return nullptr;
                uint32 position = urand(1, subGroup.TotalRate - subGroup.TotalGiven);
                uint32 currentPosition = 1;
                for (ROWOWItemGroupEntry& entry : subGroup.Entries)
                {
                    currentPosition += entry.Rate - entry.Given;
                    if (currentPosition > position)
                    {
                        entry.Given++;
                        subGroup.TotalGiven++;
                        if (subGroup.TotalGiven >= subGroup.TotalRate)
                        {
                            subGroup.TotalGiven = 0;
                            for (ROWOWItemGroupEntry& resetEntry : subGroup.Entries)
                                resetEntry.Given = 0;
                        }
                        return &entry;
                    }
                }
                return nullptr;
            }
        }
    }

    // Whether an RO item comes unidentified from a drop or a box (itemdb_isidentified: equipment does)
    bool IsIdentifiedByDefault(ROWOWItem const& roItem)
    {
        return roItem.Type != ROWOW_ITEM_TYPE_WEAPON && roItem.Type != ROWOW_ITEM_TYPE_ARMOR && roItem.Type != ROWOW_ITEM_TYPE_PETARMOR;
    }

    // The weapon type ammunition is checked against (ammo_check_weapon): the status's in the form; out of it, on the character window's Ragnarok page,
    // the status isn't worked out, so the right hand's weapon's (a bow, an instrument, a gun: none of them dual wield types)
    uint8 GetAmmoCheckWeaponType(ROWOWMod const* mod, Player* player, ROWOWIdentity const& identity)
    {
        if (identity.IsInForm == true)
            return identity.Status.WeaponType;
        ROWOWItem const* weapon = mod->GetROItem(mod->GetIdentityEquippedItem(player, ROWOW_EQUIP_POSITION_HAND_R));
        return (weapon != nullptr && weapon->IsWeapon == true) ? weapon->WeaponType : ROWOW_WEAPON_TYPE_FIST;
    }
}

void ROWOWMod::LoadItemScriptData()
{
    ItemIDsByAegisName.clear();
    ItemGroupsByName.clear();
    MapIDsByMapFlag.clear();
    ROWOWSCRIPT->Clear();
    if (IsEnabled == false)
        return;
    RegisterPetCommands();
    RegisterFollowerCommands();

    // Item names, for the scripts and groups that name items
    for (char const* itemTableName : { ROWOW_DATA_TABLE_ITEM, ROWOW_DATA_TABLE_ITEM_RENEWAL })
    {
        ROWOWDataTable const* itemTable = GetDataTable(itemTableName);
        if (itemTable == nullptr)
            continue;
        int32 idColumnIndex = itemTable->GetColumnIndex("Id");
        int32 aegisNameColumnIndex = itemTable->GetColumnIndex("AegisName");
        for (size_t rowIndex = 0; rowIndex < itemTable->Rows.size(); rowIndex++)
        {
            uint32 itemID = uint32(itemTable->GetInt(rowIndex, idColumnIndex, 0));
            if (ItemTemplateIDsByItemID.find(itemID) != ItemTemplateIDsByItemID.end())
                ItemIDsByAegisName[ToLower(ROWOWDataTable::UnescapeText(itemTable->GetCell(rowIndex, aegisNameColumnIndex)))] = itemID;
        }
    }

    // Item groups (boxes, and the groups of items some rules check, like the wings a noteleport map refuses)
    if (ROWOWDataTable const* itemGroupTable = GetDataTable(ROWOW_DATA_TABLE_ITEM_GROUP))
    {
        int32 groupColumnIndex = itemGroupTable->GetColumnIndex("Group");
        int32 subGroupColumnIndex = itemGroupTable->GetColumnIndex("SubGroups_SubGroup");
        int32 algorithmColumnIndex = itemGroupTable->GetColumnIndex("SubGroups_Algorithm");
        int32 itemColumnIndex = itemGroupTable->GetColumnIndex("SubGroups_List_Item");
        int32 rateColumnIndex = itemGroupTable->GetColumnIndex("SubGroups_List_Rate");
        for (size_t rowIndex = 0; rowIndex < itemGroupTable->Rows.size(); rowIndex++)
        {
            auto itemIDIter = ItemIDsByAegisName.find(ToLower(itemGroupTable->GetCell(rowIndex, itemColumnIndex)));
            if (itemIDIter == ItemIDsByAegisName.end())
                continue;
            ROWOWItemSubGroup& subGroup = ItemGroupsByName[ToUpper(itemGroupTable->GetCell(rowIndex, groupColumnIndex))].SubGroupsByID[uint32(itemGroupTable->GetInt(rowIndex, subGroupColumnIndex, 1))];
            std::string const& algorithm = itemGroupTable->GetCell(rowIndex, algorithmColumnIndex);
            if (algorithm == "All")
                subGroup.Algorithm = ROWOW_ITEM_GROUP_ALGORITHM_ALL;
            else if (algorithm == "Random")
                subGroup.Algorithm = ROWOW_ITEM_GROUP_ALGORITHM_RANDOM;
            else if (algorithm == "Drop")
                subGroup.Algorithm = ROWOW_ITEM_GROUP_ALGORITHM_DROP;
            ROWOWItemGroupEntry entry;
            entry.ItemID = itemIDIter->second;
            entry.Rate = uint32(std::max<int64>(0, itemGroupTable->GetInt(rowIndex, rateColumnIndex, 0)));
            subGroup.Entries.push_back(entry);
            subGroup.TotalRate += entry.Rate;
        }
    }

    // The map flags (monster_noteleport: Intimidate's warp), by any of the maps' RO names (a stitched map's by all its parts)
    for (auto const& mapIDsByFlag : GetMapIDsByMapFlag())
        MapIDsByMapFlag[mapIDsByFlag.first].insert(mapIDsByFlag.second.begin(), mapIDsByFlag.second.end());

    // The script engine's constants: elements, sizes, races and jobs by their numbers, and the rest by name (symbols)
    static char const* const ElementNames[ROWOW_ELEMENT_COUNT] = { "Neutral", "Water", "Earth", "Fire", "Wind", "Poison", "Holy", "Dark", "Ghost", "Undead" };
    for (int64 element = 0; element < ROWOW_ELEMENT_COUNT; element++)
        ROWOWSCRIPT->RegisterConstant(std::string("Ele_") + ElementNames[element], element);
    ROWOWSCRIPT->RegisterConstant("Size_Small", ROWOW_SIZE_SMALL);
    ROWOWSCRIPT->RegisterConstant("Size_Medium", ROWOW_SIZE_MEDIUM);
    ROWOWSCRIPT->RegisterConstant("Size_Large", ROWOW_SIZE_BIG);
    static char const* const RaceNames[] = { "Formless", "Undead", "Brute", "Plant", "Insect", "Fish", "Demon", "DemiHuman", "Angel", "Dragon", "Player_Human", "Player_Doram" };
    for (int64 race = 0; race < int64(std::size(RaceNames)); race++)
        ROWOWSCRIPT->RegisterConstant(std::string("RC_") + RaceNames[race], race);
    for (int64 race2 = 1; race2 < ROWOW_RACE2_MAX; race2++)
        ROWOWSCRIPT->RegisterConstant(std::string("RC2_") + Race2Names[race2], race2);
    for (auto const& jobByID : JobsByID)
        ROWOWSCRIPT->RegisterConstant("Job_" + jobByID.second.Name, jobByID.first);

    // The statuses by their numbers (SC_*, and the Eff_* the bonuses of added statuses name), and the added statuses' flags (ATF_*)
    for (auto const& statusName : ROWOWStatusNames)
        ROWOWSCRIPT->RegisterConstant(std::string("SC_") + statusName.first, statusName.second);
    std::pair<char const*, int32> const EffNames[] = { { "Eff_Stone", ROWOW_SC_STONEWAIT }, { "Eff_Freeze", ROWOW_SC_FREEZE }, { "Eff_Stun", ROWOW_SC_STUN },
        { "Eff_Sleep", ROWOW_SC_SLEEP }, { "Eff_Poison", ROWOW_SC_POISON }, { "Eff_Curse", ROWOW_SC_CURSE }, { "Eff_Silence", ROWOW_SC_SILENCE },
        { "Eff_Confusion", ROWOW_SC_CONFUSION }, { "Eff_Blind", ROWOW_SC_BLIND }, { "Eff_Bleeding", ROWOW_SC_BLEEDING }, { "Eff_DPoison", ROWOW_SC_DPOISON },
        { "Eff_Fear", ROWOW_SC_FEAR }, { "Eff_Burning", ROWOW_SC_BURNING }, { "Eff_Hallucination", ROWOW_SC_HALLUCINATION } };
    for (auto const& effName : EffNames)
        ROWOWSCRIPT->RegisterConstant(effName.first, effName.second);
    std::pair<char const*, int32> const AtfNames[] = { { "ATF_SELF", ROWOW_ATF_SELF }, { "ATF_TARGET", ROWOW_ATF_TARGET }, { "ATF_SHORT", ROWOW_ATF_SHORT },
        { "ATF_LONG", ROWOW_ATF_LONG }, { "ATF_WEAPON", ROWOW_ATF_WEAPON }, { "ATF_MAGIC", ROWOW_ATF_MAGIC }, { "ATF_MISC", ROWOW_ATF_MISC },
        { "ATF_SKILL", ROWOW_ATF_MAGIC | ROWOW_ATF_MISC } };
    for (auto const& atfName : AtfNames)
        ROWOWSCRIPT->RegisterConstant(atfName.first, atfName.second);
    ROWOWSCRIPT->RegisterConstant("INFINITE_TICK", ROWOW_INFINITE_TICK);
    for (char const* symbolPrefix : { "SC_", "EF_", "IG_", "MOBG_", "HAT_EF_", "ITEMID_", "RC_", "RC2_", "Class_", "ATF_", "BF_", "EAJ_", "EAJL_", "Size_", "Ele_", "SP_" })
        ROWOWSCRIPT->RegisterSymbolPrefix(symbolPrefix);

    // The parameters scripts read
    auto registerParameter = [](char const* name, std::function<int64(Player*, ROWOWIdentity&)> getValue)
    {
        ROWOWSCRIPT->RegisterParameter(name, [getValue](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& /*arguments*/)
        {
            if (context.AttachedPlayer == nullptr || context.Identity == nullptr)
                return ROWOWScriptValue::FromNumber(0);
            return ROWOWScriptValue::FromNumber(getValue(context.AttachedPlayer, *context.Identity));
        });
    };
    registerParameter("BaseLevel", [](Player*, ROWOWIdentity& identity) { return int64(identity.BaseLevel); });
    registerParameter("JobLevel", [](Player*, ROWOWIdentity& identity) { return int64(identity.JobLevel); });
    registerParameter("BaseExp", [](Player*, ROWOWIdentity& identity) { return int64(identity.BaseExp); });
    registerParameter("JobExp", [](Player*, ROWOWIdentity& identity) { return int64(identity.JobExp); });
    registerParameter("Class", [](Player*, ROWOWIdentity& identity) { return int64(identity.JobID); });
    registerParameter("Upper", [this](Player*, ROWOWIdentity& identity)
    {
        // 1 transcendent, 2 Baby
        ROWOWJob const* job = GetJob(identity.JobID);
        return int64(job == nullptr ? 0 : (job->IsUpper == true ? 1 : (job->IsBaby == true ? 2 : 0)));
    });
    registerParameter("Sex", [](Player* player, ROWOWIdentity&) { return int64(player->getGender() == GENDER_MALE ? ROWOW_GENDER_MALE : ROWOW_GENDER_FEMALE); });
    registerParameter("Zeny", [](Player* player, ROWOWIdentity&) { return int64(player->GetMoney()); });
    registerParameter("Hp", [](Player* player, ROWOWIdentity&) { return int64(player->GetHealth()); });
    registerParameter("MaxHp", [](Player* player, ROWOWIdentity&) { return int64(player->GetMaxHealth()); });
    registerParameter("Sp", [](Player* player, ROWOWIdentity&) { return int64(player->GetPower(POWER_MANA)); });
    registerParameter("MaxSp", [](Player* player, ROWOWIdentity&) { return int64(player->GetMaxPower(POWER_MANA)); });
    registerParameter("StatusPoint", [](Player*, ROWOWIdentity& identity) { return int64(identity.StatusPoints); });
    registerParameter("SkillPoint", [](Player*, ROWOWIdentity& identity) { return int64(identity.SkillPoints); });

    // The commands item scripts use (rAthena's buildin functions).  Anything else makes an item unusable until it comes in
    ROWOWSCRIPT->RegisterCommand("rand", [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        // rand(<range>) is 0 to range - 1, rand(<min>,<max>) min to max
        int64 minimum = 0;
        int64 maximum = 0;
        if (arguments.size() >= 2)
        {
            minimum = arguments[0].GetNumber();
            maximum = arguments[1].GetNumber();
        }
        else if (arguments.size() == 1)
            maximum = arguments[0].GetNumber() - 1;
        if (maximum < minimum)
            std::swap(minimum, maximum);
        return ROWOWScriptValue::FromNumber(minimum + int64(urand(0, uint32(std::min<int64>(maximum - minimum, int64(UINT32_MAX - 1))))));
    });
    ROWOWSCRIPT->RegisterCommand("itemheal", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        if (context.Potion != nullptr)
        {
            // A thrown potion (potion_flag 1): only what it heals
            context.Potion->HP = GetArgumentNumber(arguments, 0, 0);
            context.Potion->SP = GetArgumentNumber(arguments, 1, 0);
        }
        else if (context.AttachedPlayer != nullptr)
        {
            auto itemIter = ItemsByItemTemplateID.find(context.UsedItemTemplateID);
            PCItemHeal(context.AttachedPlayer, itemIter != ItemsByItemTemplateID.end() ? itemIter->second.ItemID : 0, GetArgumentNumber(arguments, 0, 0), GetArgumentNumber(arguments, 1, 0));
        }
        return ROWOWScriptValue::FromNumber(0);
    });
    ROWOWSCRIPT->RegisterCommand("percentheal", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        if (context.Potion != nullptr)
        {
            context.Potion->PerHP = GetArgumentNumber(arguments, 0, 0);
            context.Potion->PerSP = GetArgumentNumber(arguments, 1, 0);
        }
        else if (context.AttachedPlayer != nullptr)
            PCPercentHeal(context.AttachedPlayer, GetArgumentNumber(arguments, 0, 0), GetArgumentNumber(arguments, 1, 0));
        return ROWOWScriptValue::FromNumber(0);
    });
    ROWOWSCRIPT->RegisterCommand("heal", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        if (context.AttachedPlayer != nullptr)
            StatusHeal(context.AttachedPlayer, GetArgumentNumber(arguments, 0, 0), GetArgumentNumber(arguments, 1, 0));
        return ROWOWScriptValue::FromNumber(0);
    });
    for (char const* startCommandName : { "sc_start", "sc_start2", "sc_start4" })
    {
        // sc_start <type>,<tick>,<val1>{,<rate>,<flag>{,<GID>}}; (sc_start2 adds val2, sc_start4 val2..val4): a status on the attached
        // player, with a 1/10000 rate (100% unless told) and SCSTART_NOAVOID unless told.  No tick takes the status's skill's Duration1 at val1.
        // A worn item's bonus script doesn't start statuses (its status calculation runs it again and again; the throwable items' scripts are
        // their use).  A thrown potion's (potion_flag 1 with a potion_target) goes to its target for half its time, marked thrown (val4 1)
        int32 startType = strstr(startCommandName, "4") != nullptr ? 4 : (strstr(startCommandName, "2") != nullptr ? 2 : 1);
        ROWOWSCRIPT->RegisterCommand(startCommandName, [this, startType](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
        {
            if (context.AttachedPlayer == nullptr || context.BonusStatus != nullptr || arguments.size() < size_t(3 + startType - 1))
                return ROWOWScriptValue::FromNumber(0);
            int32 type = GetArgumentNumber(arguments, 0, ROWOW_SC_NONE);
            int64 tick = arguments[1].GetNumber();
            int32 val1 = GetArgumentNumber(arguments, 2, 0);
            int32 val2 = startType >= 2 ? GetArgumentNumber(arguments, 3, 0) : 0;
            int32 val3 = startType == 4 ? GetArgumentNumber(arguments, 4, 0) : 0;
            int32 val4 = startType == 4 ? GetArgumentNumber(arguments, 5, 0) : 0;
            int32 rate = std::min(GetArgumentNumber(arguments, size_t(2 + startType), 10000), 10000);
            uint8 flag = uint8(GetArgumentNumber(arguments, size_t(3 + startType), ROWOW_SCSTART_NOAVOID));
            uint16 skillID = StatusDBGetSkill(type);
            if (tick == 0 && val1 > 0 && skillID > 0) // When there isn't a duration specified, try to get it from the skill_db
                tick = SkillGetTime(skillID, uint16(val1));
            Unit* bl = context.AttachedPlayer;
            if (context.Potion != nullptr && context.Potion->Target != nullptr)
            {
                // skill.cpp set the potion_target variable for a status change.
                bl = context.Potion->Target;
                tick /= 2; // Thrown potions only last half.
                val4 = 1; // Mark that this was a thrown sc_effect
            }
            StatusChangeStart(context.AttachedPlayer, bl, type, rate, val1, val2, val3, val4, tick, flag);
            return ROWOWScriptValue::FromNumber(0);
        });
    }
    ROWOWSCRIPT->RegisterCommand("sc_end", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // sc_end <type>: ends a status on the attached player, unless it can't be cleared (NoClearbuff and NoForcedEnd)
        if (context.AttachedPlayer == nullptr || context.BonusStatus != nullptr || arguments.empty() == true)
            return ROWOWScriptValue::FromNumber(0);
        int32 type = GetArgumentNumber(arguments, 0, ROWOW_SC_NONE);
        ROWOWStatusChangeDB const* scdb = GetStatusChangeDB(type);
        if (scdb == nullptr || (scdb->Flag[ROWOW_SCF_NOCLEARBUFF] == true && scdb->Flag[ROWOW_SCF_NOFORCEDEND] == true))
            return ROWOWScriptValue::FromNumber(0);
        if (ROWOWStatusChange* sc = GetStatusChange(static_cast<Unit*>(context.AttachedPlayer)))
        {
            // This should help status_change_end force disabling the SC in case it has no limit.
            if (ROWOWStatusChangeEntry* sce = sc->GetSCE(type))
                sce->Val1 = sce->Val2 = sce->Val3 = sce->Val4 = 0;
        }
        StatusChangeEnd(context.AttachedPlayer, type);
        return ROWOWScriptValue::FromNumber(0);
    });
    ROWOWSCRIPT->RegisterCommand("getstatus", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // getstatus(<type>{,<type2>}): 0 or 1 for whether the attached player has the status, or with type2 one of its values (1-4) or the
        // time left (5)
        if (context.AttachedPlayer == nullptr || arguments.empty() == true)
            return ROWOWScriptValue::FromNumber(0);
        ROWOWStatusChangeEntry const* sce = GetSCE(context.AttachedPlayer, GetArgumentNumber(arguments, 0, ROWOW_SC_NONE));
        if (sce == nullptr)
            return ROWOWScriptValue::FromNumber(0);
        switch (GetArgumentNumber(arguments, 1, 0))
        {
            case 1: return ROWOWScriptValue::FromNumber(sce->Val1);
            case 2: return ROWOWScriptValue::FromNumber(sce->Val2);
            case 3: return ROWOWScriptValue::FromNumber(sce->Val3);
            case 4: return ROWOWScriptValue::FromNumber(sce->Val4);
            case 5:
            {
                uint64 tick = GetTick();
                return ROWOWScriptValue::FromNumber(sce->TimerTime > tick ? int64(sce->TimerTime - tick) : 0);
            }
            default: return ROWOWScriptValue::FromNumber(1);
        }
    });
    for (char const* effectCommandName : { "specialeffect", "specialeffect2" })
    {
        ROWOWSCRIPT->RegisterCommand(effectCommandName, [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
        {
            // The effect is only a look: on the item's user, when the converter made it (the NPC scripts' effects, R9.5)
            if (context.AttachedPlayer != nullptr && arguments.empty() == false)
            {
                std::string effectName = arguments[0].IsString == true ? arguments[0].Text : ROWOWSCRIPT->GetSymbolName(arguments[0].GetNumber());
                if (effectName.size() > 3 && effectName.compare(0, 3, "EF_") == 0)
                    PlayScriptLook(context.AttachedPlayer, ROWOW_SCRIPT_LOOK_EFFECT, effectName.substr(3));
            }
            return ROWOWScriptValue::FromNumber(0);
        });
    }
    ROWOWSCRIPT->RegisterCommand("produce", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // produce <item level>: the list of what the player can make of that level (a hammer's weapons, the Mini Furnace's metals)
        if (context.AttachedPlayer != nullptr)
            ClifSkillProduceMixList(context.AttachedPlayer, -1, GetArgumentNumber(arguments, 0, 0));
        return ROWOWScriptValue::FromNumber(0);
    });
    ROWOWSCRIPT->RegisterCommand("cooking", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // cooking <kit level>: a cooking kit's list of dishes
        if (context.AttachedPlayer != nullptr)
            ClifCookingList(context.AttachedPlayer, GetArgumentNumber(arguments, 0, 0));
        return ROWOWScriptValue::FromNumber(0);
    });
    ROWOWSCRIPT->RegisterCommand("buyingstore", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // buyingstore <slots>: a Bulk Buyer Shop License's store (not beside an NPC)
        if (context.AttachedPlayer == nullptr)
            return ROWOWScriptValue::FromNumber(0);
        if (NpcIsNear(context.AttachedPlayer) == true)
        {
            ChatHandler(context.AttachedPlayer->GetSession()).SendNotification("You can't do that this close to an NPC.");
            return ROWOWScriptValue::FromNumber(0);
        }
        BuyingStoreSetup(context.AttachedPlayer, uint8(GetArgumentNumber(arguments, 0, 0)));
        return ROWOWScriptValue::FromNumber(0);
    });
    ROWOWSCRIPT->RegisterCommand("getitem", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        if (context.AttachedPlayer != nullptr && arguments.empty() == false)
            GiveROItem(context.AttachedPlayer, GetScriptItemID(arguments[0]), uint32(std::max(1, GetArgumentNumber(arguments, 1, 1))), true);
        return ROWOWScriptValue::FromNumber(0);
    });
    ROWOWSCRIPT->RegisterCommand("getgroupitem", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        if (context.AttachedPlayer != nullptr && arguments.empty() == false)
            PCGetItemGroup(context.AttachedPlayer, GetScriptItemGroupName(arguments[0]));
        return ROWOWScriptValue::FromNumber(0);
    });
    ROWOWSCRIPT->RegisterCommand("getrandgroupitem", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // getrandgroupitem <group>,<amount>: one item picked from the group's first subgroup, that many of it
        if (context.AttachedPlayer == nullptr || arguments.empty() == true)
            return ROWOWScriptValue::FromNumber(0);
        auto groupIter = ItemGroupsByName.find(GetScriptItemGroupName(arguments[0]));
        if (groupIter == ItemGroupsByName.end() || groupIter->second.SubGroupsByID.empty() == true)
            return ROWOWScriptValue::FromNumber(0);
        uint32 itemID = 0;
        {
            std::lock_guard<std::mutex> lock(ItemGroupSharedPoolMutex);
            ROWOWItemGroupEntry const* entry = GetRandomItemSubGroupEntry(groupIter->second.SubGroupsByID.begin()->second);
            if (entry != nullptr)
                itemID = entry->ItemID;
        }
        if (itemID != 0)
            GiveROItem(context.AttachedPlayer, itemID, uint32(std::max(1, GetArgumentNumber(arguments, 1, 1))), true);
        return ROWOWScriptValue::FromNumber(0);
    });
    ROWOWSCRIPT->RegisterCommand("itemskill", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        if (context.AttachedPlayer == nullptr || arguments.empty() == true)
            return ROWOWScriptValue::FromNumber(0);
        uint16 skillID = GetItemSkillID(arguments[0]);
        int32 skillLevel = GetArgumentNumber(arguments, 1, 1);
        // A delay consume item (the wings, the Magnifier) is used up once its skill has gone off (skill_consume_requirement)
        auto consumeUsedItem = [this, &context]()
        {
            Item* usedItem = context.AttachedPlayer->GetItemByGuid(context.UsedItemGUID);
            ROWOWItem const* usedROItem = GetROItem(usedItem);
            if (usedROItem == nullptr || usedROItem->IsDelayConsume == false)
                return;
            uint32 count = 1;
            context.AttachedPlayer->DestroyItemCount(usedItem, count, true);
        };
        if (skillID == ROWOW_SKILL_AL_TELEPORT)
        {
            // A Fly Wing is level 1 (a random spot), a Butterfly Wing level 3 (the save point, without level 2's menu)
            consumeUsedItem();
            if (skillLevel <= 1)
                TeleportToRandomSpot(context.AttachedPlayer);
            else
                ReturnToSavePoint(context.AttachedPlayer); // With the return tether (ROWOW_ReturnTether.cpp)
        }
        else if (skillID == ROWOW_SKILL_MC_IDENTIFY)
        {
            // The Magnifier's list of items to appraise comes up, and it's used up (kept when there's nothing to appraise)
            SendItemIdentifyList(context.AttachedPlayer);
            if (context.Identity != nullptr && context.Identity->IsIdentifyMenuOpen == true)
                consumeUsedItem();
        }
        else if (GetSkill(skillID) != nullptr)
        {
            // Any other skill (a scroll, a box's blessing): sd->skillitem and clif_item_skill, cast as Abracadabra's pick is, aimed at the selection, with
            // no skill to know and nothing paid but the item
            ROWOWSkillUnitData* ud = GetSkillUnitData(context.AttachedPlayer);
            ud->SkillItemID = skillID;
            ud->SkillItemLevel = uint16(skillLevel);
            if (SkillCastAbraSkill(context.AttachedPlayer, skillID, uint16(skillLevel)) == true)
                consumeUsedItem();
        }
        return ROWOWScriptValue::FromNumber(0);
    }, [](std::vector<ROWOWScriptValue> const& arguments) { return arguments.empty() == false; }); // Skills load after the items' scripts compile
    ROWOWSCRIPT->RegisterCommand("warp", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // warp "<map>",<x>,<y>: "SavePoint" and "Random" are the save point and a random spot on the map
        if (context.AttachedPlayer == nullptr || arguments.empty() == true)
            return ROWOWScriptValue::FromNumber(0);
        WarpPlayerToCell(context.AttachedPlayer, arguments[0].GetText(), GetArgumentNumber(arguments, 1, 0), GetArgumentNumber(arguments, 2, 0));
        return ROWOWScriptValue::FromNumber(0);
    });
    ROWOWSCRIPT->RegisterCommand("getexp", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        if (context.AttachedPlayer != nullptr)
            PCGainExp(context.AttachedPlayer, uint64(std::max(0, GetArgumentNumber(arguments, 0, 0))), uint64(std::max(0, GetArgumentNumber(arguments, 1, 0))));
        return ROWOWScriptValue::FromNumber(0);
    });
    ROWOWSCRIPT->RegisterCommand("countitem", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        if (context.AttachedPlayer == nullptr || arguments.empty() == true)
            return ROWOWScriptValue::FromNumber(0);
        auto itemTemplateIDIter = ItemTemplateIDsByItemID.find(GetScriptItemID(arguments[0]));
        if (itemTemplateIDIter == ItemTemplateIDsByItemID.end())
            return ROWOWScriptValue::FromNumber(0);
        return ROWOWScriptValue::FromNumber(int64(context.AttachedPlayer->GetItemCount(itemTemplateIDIter->second)));
    });
    ROWOWSCRIPT->RegisterCommand("delitem", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        if (context.AttachedPlayer == nullptr || arguments.empty() == true)
            return ROWOWScriptValue::FromNumber(0);
        auto itemTemplateIDIter = ItemTemplateIDsByItemID.find(GetScriptItemID(arguments[0]));
        uint32 amount = uint32(std::max(1, GetArgumentNumber(arguments, 1, 1)));
        if (itemTemplateIDIter == ItemTemplateIDsByItemID.end() || context.AttachedPlayer->GetItemCount(itemTemplateIDIter->second) < amount)
        {
            // rAthena stops the script when there aren't enough
            context.Error = "not enough items to delete";
            context.IsStopped = true;
            return ROWOWScriptValue::FromNumber(0);
        }
        context.AttachedPlayer->DestroyItemCount(itemTemplateIDIter->second, amount, true);
        return ROWOWScriptValue::FromNumber(0);
    });
    ROWOWSCRIPT->RegisterCommand("getskilllv", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        if (context.AttachedPlayer == nullptr || arguments.empty() == true || arguments[0].IsString == true)
            return ROWOWScriptValue::FromNumber(0);
        return ROWOWScriptValue::FromNumber(PCCheckSkill(context.AttachedPlayer, uint16(arguments[0].GetNumber())));
    });
    ROWOWSCRIPT->RegisterCommand("getcharid", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // getcharid(<type>{,"<name>"}): 0 the character, 1 its party, 2 its guild, 3 its account, of the attached player or the online one of that name
        // (the church's marriage.txt finds the groom's account by name: 0 when no such player is online)
        Player* player = context.AttachedPlayer;
        if (arguments.size() > 1)
        {
            std::string name = arguments[1].GetText();
            player = normalizePlayerName(name) == true ? ObjectAccessor::FindPlayerByName(name) : nullptr;
        }
        if (player == nullptr)
            return ROWOWScriptValue::FromNumber(0);
        switch (GetArgumentNumber(arguments, 0, 0))
        {
            case 0: return ROWOWScriptValue::FromNumber(int64(player->GetGUID().GetCounter()));
            case 1:
            {
                Group* group = player->GetGroup();
                return ROWOWScriptValue::FromNumber(group != nullptr ? int64(group->GetGUID().GetCounter()) : 0);
            }
            case 2: return ROWOWScriptValue::FromNumber(int64(player->GetGuildId()));
            case 3: return ROWOWScriptValue::FromNumber(int64(player->GetSession()->GetAccountId()));
            default: return ROWOWScriptValue::FromNumber(0);
        }
    });
    ROWOWSCRIPT->RegisterCommand("strcharinfo", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        if (context.AttachedPlayer == nullptr || GetArgumentNumber(arguments, 0, 0) != 0)
            return ROWOWScriptValue::FromText(std::string());
        return ROWOWScriptValue::FromText(context.AttachedPlayer->GetName());
    });

    // Compile the usable items' scripts
    uint32 runnableScriptCount = 0;
    std::map<std::string, uint32> unavailableCommandCounts;
    if (ROWOWDataTable const* itemTable = GetDataTable(ROWOW_DATA_TABLE_ITEM))
    {
        int32 idColumnIndex = itemTable->GetColumnIndex("Id");
        int32 scriptColumnIndex = itemTable->GetColumnIndex("Script");
        for (size_t rowIndex = 0; rowIndex < itemTable->Rows.size(); rowIndex++)
        {
            auto itemTemplateIDIter = ItemTemplateIDsByItemID.find(uint32(itemTable->GetInt(rowIndex, idColumnIndex, 0)));
            if (itemTemplateIDIter == ItemTemplateIDsByItemID.end())
                continue;
            auto itemIter = ItemsByItemTemplateID.find(itemTemplateIDIter->second);
            if (itemIter == ItemsByItemTemplateID.end())
                continue;
            ROWOWItem& item = itemIter->second;
            if (item.Type != ROWOW_ITEM_TYPE_HEALING && item.Type != ROWOW_ITEM_TYPE_USABLE && item.Type != ROWOW_ITEM_TYPE_DELAYCONSUME && item.Type != ROWOW_ITEM_TYPE_CASH)
                continue;
            item.UseScript = ROWOWSCRIPT->Compile(ROWOWDataTable::UnescapeText(itemTable->GetCell(rowIndex, scriptColumnIndex)), "item " + std::to_string(item.ItemID));
            if (item.UseScript->IsRunnable() == true)
                runnableScriptCount++;
            else if (item.UseScript->Error.empty() == false)
                LOG_ERROR("module.ROWOW", "ROWOWMod::LoadItemScriptData: the script of RO item {} didn't compile: {}", item.ItemID, item.UseScript->Error);
            else
                unavailableCommandCounts[item.UseScript->UnavailableCommands[0]]++;
        }
    }
    std::string unavailableCommandsText;
    for (auto const& unavailableCommandCount : unavailableCommandCounts)
        unavailableCommandsText += fmt::format("{}{} ({})", unavailableCommandsText.empty() == true ? "" : ", ", unavailableCommandCount.first, unavailableCommandCount.second);
    LOG_INFO("module.ROWOW", "ROWOWMod::LoadItemScriptData loaded {} item groups and {} usable RO items whose scripts can run; not yet usable, by the first command missing: {}",
        ItemGroupsByName.size(), runnableScriptCount, unavailableCommandsText.empty() == true ? "none" : unavailableCommandsText);
}

uint32 ROWOWMod::GetItemRefine(Item const* item)
{
    uint32 enchantmentID = item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT);
    if (enchantmentID <= ROWOW_ITEM_STATE_ENCHANTMENT_START || enchantmentID >= ROWOW_ITEM_STATE_ENCHANTMENT_START + ROWOW_ITEM_STATE_COUNT)
        return 0;
    return (enchantmentID - ROWOW_ITEM_STATE_ENCHANTMENT_START) & ROWOW_ITEM_STATE_REFINE_MASK;
}

bool ROWOWMod::IsItemBroken(Item const* item)
{
    uint32 enchantmentID = item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT);
    if (enchantmentID <= ROWOW_ITEM_STATE_ENCHANTMENT_START || enchantmentID >= ROWOW_ITEM_STATE_ENCHANTMENT_START + ROWOW_ITEM_STATE_COUNT)
        return false;
    return ((enchantmentID - ROWOW_ITEM_STATE_ENCHANTMENT_START) & ROWOW_ITEM_STATE_BROKEN) != 0;
}

bool ROWOWMod::IsItemIdentified(Item const* item)
{
    uint32 enchantmentID = item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT);
    if (enchantmentID <= ROWOW_ITEM_STATE_ENCHANTMENT_START || enchantmentID >= ROWOW_ITEM_STATE_ENCHANTMENT_START + ROWOW_ITEM_STATE_COUNT)
        return true;
    return ((enchantmentID - ROWOW_ITEM_STATE_ENCHANTMENT_START) & ROWOW_ITEM_STATE_UNIDENTIFIED) == 0;
}

void ROWOWMod::SetItemState(Player* player, Item* item, uint32 refine, bool isBroken, bool isIdentified)
{
    // The state is the enchantment's offset from the first state enchantment, which is never used itself (no state is no enchantment)
    uint32 state = std::min<uint32>(refine, ROWOW_ITEM_STATE_REFINE_MASK) | (isBroken == true ? ROWOW_ITEM_STATE_BROKEN : 0) | (isIdentified == false ? ROWOW_ITEM_STATE_UNIDENTIFIED : 0);
    uint32 enchantmentID = state != 0 ? ROWOW_ITEM_STATE_ENCHANTMENT_START + state : 0;
    if (item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT) == enchantmentID)
        return;
    item->SetEnchantment(PERM_ENCHANTMENT_SLOT, enchantmentID, 0, 0);
    item->SetState(ITEM_CHANGED, player);
}

uint32 ROWOWMod::GetIdentityEquipPosition(ROWOWIdentity const& identity, ObjectGuid::LowType itemGUID) const
{
    uint32 equipPosition = 0;
    for (auto const& itemGUIDByEquipPosition : identity.ItemGUIDsByEquipPosition)
        if (itemGUIDByEquipPosition.second == itemGUID)
            equipPosition |= itemGUIDByEquipPosition.first;
    return equipPosition;
}

uint8 ROWOWMod::PCIsEquip(Player* player, ROWOWIdentity const& identity, Item const* item) const
{
    // pc_isequip
    ROWOWItem const* roItem = GetROItem(item);
    if (roItem == nullptr || roItem->Locations == 0)
        return ROWOW_EQUIP_ACK_FAIL;
    if (identity.SpiritSkillID == ROWOW_SKILL_SL_SUPERNOVICE)
    {
        // Spirit of Super Novice equip bonuses. [Skotlex]
        if (identity.BaseLevel >= 90 && (roItem->Locations & (ROWOW_EQUIP_POSITION_HEAD_TOP | ROWOW_EQUIP_POSITION_HEAD_MID | ROWOW_EQUIP_POSITION_HEAD_LOW)) != 0)
            return ROWOW_EQUIP_ACK_OK; // Can equip all helms
        if (identity.BaseLevel >= 96 && (roItem->Locations & ROWOW_EQUIP_POSITION_ARMS) != 0 && roItem->Type == ROWOW_ITEM_TYPE_WEAPON && roItem->WeaponLevel == 4)
        {
            switch (roItem->WeaponType)
            {
                // In weapons, the look determines type of weapon: all level 4 daggers, 1H swords, axes, maces and staves
                case ROWOW_WEAPON_TYPE_DAGGER:
                case ROWOW_WEAPON_TYPE_1HSWORD:
                case ROWOW_WEAPON_TYPE_1HAXE:
                case ROWOW_WEAPON_TYPE_2HAXE: // All level 4 - 2H Axes (works, but low ASPD)
                case ROWOW_WEAPON_TYPE_MACE:
                case ROWOW_WEAPON_TYPE_STAFF:
                case ROWOW_WEAPON_TYPE_2HSTAFF:
                    return ROWOW_EQUIP_ACK_OK;
            }
        }
    }
    if (roItem->EquipLevelMin != 0 && identity.BaseLevel < roItem->EquipLevelMin)
        return ROWOW_EQUIP_ACK_FAIL_LEVEL;
    if (roItem->EquipLevelMax != 0 && identity.BaseLevel > roItem->EquipLevelMax)
        return ROWOW_EQUIP_ACK_FAIL_LEVEL;
    uint8 gender = player->getGender() == GENDER_MALE ? ROWOW_GENDER_MALE : ROWOW_GENDER_FEMALE;
    if (roItem->Gender != ROWOW_GENDER_BOTH && roItem->Gender != gender)
        return ROWOW_EQUIP_ACK_FAIL;

    // Broken equipment
    if (IsItemBroken(item) == true)
        return ROWOW_EQUIP_ACK_FAIL;

    // Ammo needs its weapon (ammo_check_weapon), and the Venom Knives the skill that throws them
    if ((roItem->Locations & ROWOW_EQUIP_POSITION_AMMO) != 0)
    {
        if (roItem->AmmoType == AMMO_TYPE_DAGGER && PCCheckSkill(player, ROWOW_SKILL_AS_VENOMKNIFE) == 0)
            return ROWOW_EQUIP_ACK_FAIL;
        if (BattleConfig.AmmoCheckWeapon != 0 && IsAmmoForWeaponType(roItem->AmmoType, GetAmmoCheckWeaponType(this, player, identity)) == false)
            return ROWOW_EQUIP_ACK_FAIL;
    }

    // Not equipable by class (pc_job_can_use_item and pc_isItemClass)
    ROWOWJob const* job = GetJob(identity.JobID);
    if (job == nullptr || job->ItemJobIndex < 0 || (roItem->JobMask & (uint64(1) << job->ItemJobIndex)) == 0)
        return ROWOW_EQUIP_ACK_FAIL;
    if ((roItem->ClassMask & GetItemClassMask(*job)) == 0)
        return ROWOW_EQUIP_ACK_FAIL;
    return ROWOW_EQUIP_ACK_OK;
}

int32 ROWOWMod::PCEquipPoint(Player* player, ROWOWIdentity const& identity, ROWOWItem const& roItem) const
{
    // pc_equippoint_sub: an Assassin (or anyone with Lefthand Mastery) holds a dagger, sword or axe in either hand
    int32 equipPosition = int32(roItem.Locations);
    if (roItem.IsWeapon == true && equipPosition == ROWOW_EQUIP_POSITION_HAND_R
        && (roItem.WeaponType == ROWOW_WEAPON_TYPE_DAGGER || roItem.WeaponType == ROWOW_WEAPON_TYPE_1HSWORD || roItem.WeaponType == ROWOW_WEAPON_TYPE_1HAXE))
    {
        ROWOWJob const* job = GetJob(identity.JobID);
        bool isAssassin = job != nullptr && job->ItemJobIndex >= 0 && strcmp(ROWOWItemJobNames[job->ItemJobIndex], "Assassin") == 0;
        if (isAssassin == true || PCCheckSkill(player, ROWOW_SKILL_AS_LEFT) > 0)
            return ROWOW_EQUIP_POSITION_ARMS;
    }
    return equipPosition;
}

bool ROWOWMod::PCEquipItem(Player* player, Item* item, int32 reqPos)
{
    // pc_equipitem.  Out of the form, on the character window's Ragnarok page, the item is only put in its place: its scripts, its status and the
    // statuses it needs wait for the RO world, where the form's status is worked out with what it wears (RefreshROForm)
    ROWOWIdentity* identity = GetIdentity(player);
    ROWOWItem const* roItem = GetROItem(item);
    if (IsROSetCarried(player) == false || roItem == nullptr || (identity->IsInForm == true && PCCantAct(player) == true)) // clif_parse_EquipItem's pc_cant_act2
        return false;
    int32 pos = PCEquipPoint(player, *identity, *roItem);
    uint8 result = PCIsEquip(player, *identity, item);
    if (result != ROWOW_EQUIP_ACK_OK)
    {
        if ((roItem->Locations & ROWOW_EQUIP_POSITION_AMMO) != 0 && IsAmmoForWeaponType(roItem->AmmoType, GetAmmoCheckWeaponType(this, player, *identity)) == false)
            SendItemMessage(player, roItem->AmmoType == ROWOW_AMMO_TYPE_ARROW ? "You need a bow equipped to equip arrows." : "This ammunition doesn't fit the weapon equipped.");
        else
            SendItemMessage(player, result == ROWOW_EQUIP_ACK_FAIL_LEVEL ? "You can't equip this item at your current level." : "You can't put this item on.");
        return false;
    }
    if ((pos & reqPos) == 0 || GetIdentityEquipPosition(*identity, item->GetGUID().GetCounter()) != 0)
        return false;
    if (identity->CanEquipTick > GetTick())
    {
        SendItemMessage(player, "You can't put this item on.");
        return false;
    }

    // pc_isequip's statuses: a stripped place can't be dressed again until its strip ends.  (Not in PCIsEquip, which PCCheckItem asks too:
    // rAthena's pc_checkitem doesn't take off what these forbid, so Brandish Spear's own SC_STRIPWEAPON leaves the spear in hand)
    if (ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(player)))
    {
        if (((roItem->Locations & ROWOW_EQUIP_POSITION_ARMS) != 0 && roItem->IsWeapon == true && sc->HasSCE(ROWOW_SC_STRIPWEAPON) == true) || // Also works with left-hand weapons [DracoRPG]
            ((roItem->Locations & ROWOW_EQUIP_POSITION_HAND_L) != 0 && roItem->IsArmor == true && sc->HasSCE(ROWOW_SC_STRIPSHIELD) == true) ||
            ((roItem->Locations & ROWOW_EQUIP_POSITION_ARMOR) != 0 && sc->HasSCE(ROWOW_SC_STRIPARMOR) == true) ||
            ((roItem->Locations & ROWOW_EQUIP_POSITION_HEAD_TOP) != 0 && sc->HasSCE(ROWOW_SC_STRIPHELM) == true))
        {
            SendItemMessage(player, "You can't put this item on.");
            return false;
        }
    }

    // Accessories go in one of the two positions, and a weapon either hand can hold in the one asked for (or the free one)
    if (pos == ROWOW_EQUIP_POSITION_ACC)
    {
        pos = reqPos & ROWOW_EQUIP_POSITION_ACC;
        if (pos == ROWOW_EQUIP_POSITION_ACC)
            pos = (GetIdentityEquippedItem(player, ROWOW_EQUIP_POSITION_ACC_R) != nullptr && GetIdentityEquippedItem(player, ROWOW_EQUIP_POSITION_ACC_L) == nullptr) ? ROWOW_EQUIP_POSITION_ACC_L : ROWOW_EQUIP_POSITION_ACC_R;
    }
    else if (pos == ROWOW_EQUIP_POSITION_ARMS && roItem->Locations == ROWOW_EQUIP_POSITION_HAND_R)
    {
        pos = reqPos & ROWOW_EQUIP_POSITION_ARMS;
        if (pos == ROWOW_EQUIP_POSITION_ARMS)
            pos = GetIdentityEquippedItem(player, ROWOW_EQUIP_POSITION_HAND_R) != nullptr ? ROWOW_EQUIP_POSITION_HAND_L : ROWOW_EQUIP_POSITION_HAND_R;
    }

    // Take off what's worn there (keeping the ammo, which is checked against the new weapon below), then wear it
    std::vector<uint32> equipPositionsToClear;
    for (auto const& itemGUIDByEquipPosition : identity->ItemGUIDsByEquipPosition)
        if ((itemGUIDByEquipPosition.first & uint32(pos)) != 0)
            equipPositionsToClear.push_back(itemGUIDByEquipPosition.first);
    for (uint32 equipPositionToClear : equipPositionsToClear)
        PCUnequipItem(player, equipPositionToClear, 2 | 4);
    identity->ItemGUIDsByEquipPosition[uint32(pos)] = item->GetGUID().GetCounter();
    identity->IsEquipmentChanged = true;

    // A new weapon takes off ammo it doesn't shoot (ammo_unequip)
    if (BattleConfig.AmmoUnequip != 0 && (pos & ROWOW_EQUIP_POSITION_ARMS) != 0 && roItem->IsWeapon == true)
    {
        ROWOWItem const* ammoItem = GetROItem(GetIdentityEquippedItem(player, ROWOW_EQUIP_POSITION_AMMO));
        if (ammoItem != nullptr && IsAmmoForWeaponType(ammoItem->AmmoType, roItem->WeaponType) == false)
            PCUnequipItem(player, ROWOW_EQUIP_POSITION_AMMO, 2 | 4);
    }

    if (identity->IsInForm == false)
    {
        SendEquipmentToClient(player, *identity, false); // The Ragnarok page's slots
        return true;
    }
    RefreshROForm(player);
    PCCheckAllowSkill(player); // Check if status changes should be halted (with the new weapon's status)

    // The item's EquipScript, then its cards'
    RunItemScript(player, roItem->EquipScript.get(), item);
    for (uint8 slot = 0; slot < ROWOW_MAX_SLOTS; slot++)
    {
        auto cardTemplateIDIter = ItemTemplateIDsByItemID.find(GetItemCard(item, slot));
        auto cardIter = cardTemplateIDIter != ItemTemplateIDsByItemID.end() ? ItemsByItemTemplateID.find(cardTemplateIDIter->second) : ItemsByItemTemplateID.end();
        if (cardIter != ItemsByItemTemplateID.end())
            RunItemScript(player, cardIter->second.EquipScript.get(), item);
    }
    return true;
}

bool ROWOWMod::PCUnequipItem(Player* player, uint32 equipPosition, int32 flag)
{
    // pc_unequipitem: flag 1 recalculates the status after, 4 keeps the ammo (that's checked by the caller)
    ROWOWIdentity* identity = GetIdentity(player);
    auto itemGUIDIter = identity->ItemGUIDsByEquipPosition.begin();
    for (; itemGUIDIter != identity->ItemGUIDsByEquipPosition.end(); ++itemGUIDIter)
        if ((itemGUIDIter->first & equipPosition) != 0)
            break;
    if (itemGUIDIter == identity->ItemGUIDsByEquipPosition.end())
        return false;
    uint32 pos = itemGUIDIter->first;
    Item* item = player->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(itemGUIDIter->second));
    ROWOWItem const* roItem = GetROItem(item);
    identity->ItemGUIDsByEquipPosition.erase(itemGUIDIter);
    identity->IsEquipmentChanged = true;

    // pc_unequipitem_sub: the item's UnEquipScript, and its cards' (in the form: on the Ragnarok page nothing ran when it was put on)
    if (roItem != nullptr && identity->IsInForm == true)
    {
        RunItemScript(player, roItem->UnEquipScript.get(), item);
        for (uint8 slot = 0; slot < ROWOW_MAX_SLOTS; slot++)
        {
            auto cardTemplateIDIter = ItemTemplateIDsByItemID.find(GetItemCard(item, slot));
            auto cardIter = cardTemplateIDIter != ItemTemplateIDsByItemID.end() ? ItemsByItemTemplateID.find(cardTemplateIDIter->second) : ItemsByItemTemplateID.end();
            if (cardIter != ItemsByItemTemplateID.end())
                RunItemScript(player, cardIter->second.UnEquipScript.get(), item);
        }
    }

    // Taking off a bow or gun takes off its ammo (ammo_unequip)
    if ((pos & ROWOW_EQUIP_POSITION_ARMS) != 0 && roItem != nullptr && roItem->IsWeapon == true && BattleConfig.AmmoUnequip != 0 && (flag & 4) == 0)
    {
        switch (roItem->WeaponType)
        {
            case ROWOW_WEAPON_TYPE_BOW:
            case ROWOW_WEAPON_TYPE_MUSICAL:
            case ROWOW_WEAPON_TYPE_WHIP:
            case ROWOW_WEAPON_TYPE_REVOLVER:
            case ROWOW_WEAPON_TYPE_RIFLE:
            case ROWOW_WEAPON_TYPE_GATLING:
            case ROWOW_WEAPON_TYPE_SHOTGUN:
            case ROWOW_WEAPON_TYPE_GRENADE:
                identity->ItemGUIDsByEquipPosition.erase(ROWOW_EQUIP_POSITION_AMMO);
                break;
        }
    }
    // Taking off the weapon ends Enchant Deadly Poison (switch_remove_edp&1, pre-renewal's bit)
    if ((pos & ROWOW_EQUIP_POSITION_HAND_R) != 0 && (BattleConfig.SwitchRemoveEdp & 1) != 0 && identity->IsInForm == true)
        StatusChangeEnd(player, ROWOW_SC_EDP);
    if (identity->IsInForm == false)
    {
        if ((flag & 2) == 0)
            SendEquipmentToClient(player, *identity, false); // The Ragnarok page's slots (an equip taking it off sends them itself)
        return true;
    }
    if ((flag & 1) != 0)
    {
        RefreshROForm(player);
        if ((flag & 4) == 0)
            PCCheckAllowSkill(player); // With the status the item no longer gives
    }
    return true;
}

void ROWOWMod::PCCheckAllowSkill(Player* player)
{
    // pc_checkallowskill: the statuses of skills needing a weapon type (a dance needs its instrument or whip, with dancing_weaponswitch_fix),
    // no weapon, or a shield end when the equipment no longer has it.  Defender only needs a shield on a Crusader
    ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(player));
    ROWOWStatus const* status = GetROStatus(player);
    if (sc == nullptr || sc->IsEmpty() == true || status == nullptr)
        return;
    ROWOWItem const* shield = GetROItem(GetIdentityEquippedItem(player, ROWOW_EQUIP_POSITION_HAND_L));
    bool hasShield = shield != nullptr && shield->IsArmor == true;
    ROWOWJob const* job = GetJob(GetIdentity(player)->JobID);
    bool isCrusader = job != nullptr && strcmp(job->ItemJobIndex >= 0 ? ROWOWItemJobNames[job->ItemJobIndex] : "", "Crusader") == 0; // MAPID_CRUSADER
    std::vector<int32> types;
    for (auto const& statusByType : StatusChangeDBsByType)
    {
        ROWOWStatusChangeDB const& scdb = statusByType.second;
        if (sc->HasSCE(scdb.Type) == false)
            continue;
        if (scdb.Flag.test(ROWOW_SCF_REQUIREWEAPON) == true && (scdb.Type != ROWOW_SC_DANCING || BattleConfig.DancingWeaponSwitchFix != 0))
        {
            ROWOWSkill const* skill = GetSkill(scdb.SkillID);
            if (skill != nullptr && skill->RequiredWeapons != 0 && PCCheckWeaponType(player, skill->RequiredWeapons) == false)
                types.push_back(scdb.Type);
        }
        if (scdb.Flag.test(ROWOW_SCF_REQUIRENOWEAPON) == true && status->RightWeaponType != ROWOW_WEAPON_TYPE_FIST)
            types.push_back(scdb.Type);
        if (scdb.Flag.test(ROWOW_SCF_REQUIRESHIELD) == true && (scdb.Type != ROWOW_SC_DEFENDER || isCrusader == true) && hasShield == false)
            types.push_back(scdb.Type);
    }
    for (int32 type : types)
        StatusChangeEnd(player, type);
}

bool ROWOWMod::PCCheckItem(Player* player, ROWOWIdentity& identity)
{
    // pc_checkitem: what can no longer be worn (a lower level, another job) comes off, as does an item no longer in the bags
    bool isChanged = false;
    for (auto itemGUIDIter = identity.ItemGUIDsByEquipPosition.begin(); itemGUIDIter != identity.ItemGUIDsByEquipPosition.end();)
    {
        Item* item = player->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(itemGUIDIter->second));
        if (item != nullptr && Player::IsInventoryPos(item->GetPos()) == true && PCIsEquip(player, identity, item) == ROWOW_EQUIP_ACK_OK)
        {
            ++itemGUIDIter;
            continue;
        }
        itemGUIDIter = identity.ItemGUIDsByEquipPosition.erase(itemGUIDIter);
        isChanged = true;
    }
    if (isChanged == true)
        identity.IsEquipmentChanged = true;
    return isChanged;
}

bool ROWOWMod::IsItemInGroup(std::string const& groupName, uint32 itemID) const
{
    // itemdb_group.item_exists, by the uppercase group name
    auto groupIter = ItemGroupsByName.find(groupName);
    if (groupIter == ItemGroupsByName.end())
        return false;
    for (auto const& subGroup : groupIter->second.SubGroupsByID)
        for (ROWOWItemGroupEntry const& entry : subGroup.second.Entries)
            if (entry.ItemID == itemID)
                return true;
    return false;
}

bool ROWOWMod::PCIsUseItem(Player* player, Item const* item, bool isReporting)
{
    // pc_isUseitem
    ROWOWItem const* roItem = GetROItem(item);
    ROWOWIdentity* identity = GetIdentity(player);
    if (roItem == nullptr || identity->IsInForm == false)
        return false;
    if (roItem->Type == ROWOW_ITEM_TYPE_PETARMOR)
        return identity->Pet != nullptr; // clif_parse_EquipItem: the pet's, not a script's
    if (roItem->Type != ROWOW_ITEM_TYPE_HEALING && roItem->Type != ROWOW_ITEM_TYPE_USABLE && roItem->Type != ROWOW_ITEM_TYPE_DELAYCONSUME && roItem->Type != ROWOW_ITEM_TYPE_CASH)
        return false;
    if (roItem->UseScript == nullptr || roItem->UseScript->IsRunnable() == false)
    {
        if (isReporting == true)
            SendItemMessage(player, "This item can't be used yet.");
        return false;
    }

    // pc_isUseitem's mercenary items: its potions need it (and it awake, and the level the two high ones ask), and a scroll can't be used with one under contract
    if (ROWOWMercData const* mercenary = identity->Mercenary.get(); roItem->ItemID == ROWOW_ITEMID_MERCENARY_RED_POTION || roItem->ItemID == ROWOW_ITEMID_MERCENARY_BLUE_POTION ||
        roItem->ItemID == ROWOW_ITEMID_M_CENTER_POTION || roItem->ItemID == ROWOW_ITEMID_M_AWAKENING_POTION || roItem->ItemID == ROWOW_ITEMID_M_BERSERK_POTION)
    {
        if (mercenary == nullptr)
            return false;
        if (Creature* md = GetFollowerCreature(player, *mercenary))
            if (ROWOWStatusChange const* msc = GetStatusChange(static_cast<Unit const*>(md)); msc != nullptr && msc->Cant.Consume == true)
                return false;
        if (roItem->ItemID == ROWOW_ITEMID_M_AWAKENING_POTION && mercenary->DB->Level < 40)
            return false;
        if (roItem->ItemID == ROWOW_ITEMID_M_BERSERK_POTION && mercenary->DB->Level < 80)
            return false;
    }
    if (identity->Mercenary != nullptr && IsItemInGroup("MERCENARY", roItem->ItemID) == true)
        return false; // Mercenary Scrolls

    // A dialog that turned items off (disable_items: npc_id with no npc_item_flag)
    if (IsItemUseBlockedByNpc(player) == true)
        return false;

    // clif_parse_UseItem: pc_istrading or a chat room
    if (PCCantAct(player) == true)
    {
        if (isReporting == true)
            SendItemMessage(player, "You are busy.");
        return false;
    }

    // Statuses that keep the player from using items (Hiding, Stomach Ache...)
    if (ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(player)); sc != nullptr && sc->Cant.Consume == true)
        return false;

    // Prevent mass item usage, and items with their own reuse delay (pc_itemcd_check)
    uint32 now = uint32(GameTime::GetGameTimeMS().count());
    if (int32(identity->CanUseItemTime - now) > 0)
        return false;

    // Items that can't be used sitting (item_usage.sitting: MSI_CANT_USE_WHEN_SITDOWN), and silently the delay consume ones, which are skills
    // (pc_useitem)
    if (roItem->IsNoUseSitting == true && player->IsSitState() == true)
    {
        if (isReporting == true)
            SendItemMessage(player, "You cannot use this item while sitting.");
        return false;
    }
    if (roItem->IsDelayConsume == true && player->IsSitState() == true)
        return false;
    auto delayIter = identity->ItemDelayEndTimes.find(roItem->ItemID);
    if (delayIter != identity->ItemDelayEndTimes.end() && int32(delayIter->second - now) > 0)
    {
        if (isReporting == true)
            SendItemMessage(player, fmt::format("This item can be used again in {} seconds.", (delayIter->second - now + 999) / 1000));
        return false;
    }

    // Map restrictions: Dead Branches, and the wings a map without teleporting or returning refuses
    uint32 mapKey = GetUnitMapKey(player);
    auto hasMapFlag = [this, mapKey](char const* flag)
    {
        return HasMapFlag(mapKey, flag);
    };
    auto isInItemGroup = [this, roItem](char const* groupName)
    {
        auto groupIter = ItemGroupsByName.find(groupName);
        if (groupIter == ItemGroupsByName.end())
            return false;
        for (auto const& subGroup : groupIter->second.SubGroupsByID)
            for (ROWOWItemGroupEntry const& entry : subGroup.second.Entries)
                if (entry.ItemID == roItem->ItemID)
                    return true;
        return false;
    };
    if (roItem->IsDeadBranch == true && hasMapFlag("nobranch") == true)
        return false;
    if (isInItemGroup("MF_NOTELEPORT") == true && hasMapFlag("noteleport") == true)
    {
        if (isReporting == true)
            SendItemMessage(player, "Unable to Teleport in this area.");
        return false;
    }
    if (isInItemGroup("MF_NORETURN") == true && hasMapFlag("noreturn") == true)
        return false;

    // A box needs room for what comes out of it
    if (roItem->IsContainer == true && player->GetFreeInventorySpace() == 0)
    {
        if (isReporting == true)
            SendItemMessage(player, "You can't open this while your bags are full.");
        return false;
    }

    // Gender, level, job and class
    uint8 gender = player->getGender() == GENDER_MALE ? ROWOW_GENDER_MALE : ROWOW_GENDER_FEMALE;
    ROWOWJob const* job = GetJob(identity->JobID);
    if ((roItem->Gender != ROWOW_GENDER_BOTH && roItem->Gender != gender) || (roItem->EquipLevelMin != 0 && identity->BaseLevel < roItem->EquipLevelMin)
        || (roItem->EquipLevelMax != 0 && identity->BaseLevel > roItem->EquipLevelMax) || job == nullptr || job->ItemJobIndex < 0
        || (roItem->JobMask & (uint64(1) << job->ItemJobIndex)) == 0 || (roItem->ClassMask & GetItemClassMask(*job)) == 0)
    {
        if (isReporting == true)
            SendItemMessage(player, "You can't use this item.");
        return false;
    }
    return true;
}

bool ROWOWMod::ItemDBIsRestricted(Item const* item, bool ROWOWItem::* restriction) const
{
    // itemdb_isrestricted: the item's own Trade flag, or a card's in it.  (rAthena's GM override and TradePartner have nothing to act on: no row sets Override, and
    // its default of 100 is past every group level, and no row sets TradePartner)
    ROWOWItem const* roItem = GetROItem(item);
    if (roItem == nullptr)
        return false;
    if (roItem->*restriction == true)
        return true;
    int32 starCrumbs = 0, forgeElement = 0;
    if (roItem->Slots == 0 || GetItemForge(item, starCrumbs, forgeElement) == true) // itemdb_isspecial
        return false;
    for (uint8 slot = 0; slot < roItem->Slots; slot++)
    {
        auto cardTemplateIDIter = ItemTemplateIDsByItemID.find(GetItemCard(item, slot));
        ROWOWItem const* card = cardTemplateIDIter != ItemTemplateIDsByItemID.end() ? GetROItemByItemTemplateID(cardTemplateIDIter->second) : nullptr;
        if (card != nullptr && card->*restriction == true)
            return true;
    }
    return false;
}

void ROWOWMod::WarpPlayerToCell(Player* player, std::string const& mapName, int32 x, int32 y)
{
    // pc_setpos for a script's warp: "SavePoint" and "Random" are the save point and a random spot on the map.  revive_onwarp: a dead
    // player is revived before it's warped (the Knight test's arena warps a player who died in one stage into the next)
    if (BattleConfig.ReviveOnWarp != 0 && player->IsAlive() == false)
        PCSetRestartValue(player);
    if (mapName == "SavePoint" || mapName == "Save")
    {
        TeleportToSavePoint(player);
        PlayTeleportLooks(player); // buildin_warp's save point and random spot are CLR_TELEPORT (a map's cells are CLR_OUTSIGHT: no looks)
    }
    else if (mapName == "Random")
        TeleportToRandomSpot(player);
    else
    {
        ROWOWPosition destination;
        int32 mapX = x, mapY = y;
        uint32 mapID = ResolveMapCell(mapName, mapX, mapY);
        if (GetCellPosition(mapID, mapX, mapY, destination) == true)
            player->TeleportTo(destination.MapID, destination.X, destination.Y, destination.Z, player->GetOrientation());
        else
            LOG_ERROR("module.ROWOW", "ROWOWMod warp: no RO cell {},{} on map '{}'", x, y, mapName);
    }
}

uint32 ROWOWMod::GetItemIDOfScriptValue(ROWOWScriptValue const& item) const
{
    return GetScriptItemID(item);
}

void ROWOWMod::PCUseItem(Player* player, ObjectGuid itemGUID)
{
    // pc_useitem: after the use item spell (which can't use the item up itself, as the spell still holds it), so the item is found again
    Item* item = player->GetItemByGuid(itemGUID);
    if (item == nullptr || PCIsUseItem(player, item, true) == false)
        return;
    ROWOWItem const* roItem = GetROItem(item);
    ROWOWIdentity* identity = GetIdentity(player);
    if (roItem->Type == ROWOW_ITEM_TYPE_PETARMOR)
    {
        PetEquipItem(player, item);
        return;
    }

    // Used up now, unless what its script does uses it up (delay consume), and the next item use waits item_use_interval
    uint32 itemTemplateID = item->GetEntry();
    uint32 now = uint32(GameTime::GetGameTimeMS().count());
    identity->CanUseItemTime = now + uint32(std::max(0, BattleConfig.ItemUseInterval));
    if (roItem->DelayDuration > 0)
        identity->ItemDelayEndTimes[roItem->ItemID] = now + roItem->DelayDuration;
    ObjectGuid::LowType creatorGUID = item->GetGuidValue(ITEM_FIELD_CREATOR).GetCounter();
    if (roItem->IsDelayConsume == false)
    {
        uint32 count = 1;
        player->DestroyItemCount(item, count, true);
    }

    // Famous player's potions have 50% more efficiency
    identity->IsFamousPotion = creatorGUID != 0 && PCFameRank(creatorGUID, ROWOW_FAME_ALCHEMIST) != 0;

    // clif_useitemack: the RO client plays the item's use effect (a potion's swirl, RO's client item table: the converter's RoItemUseEffects.csv) on the
    // player for everyone around, before the script runs
    PlayScriptLook(player, ROWOW_SCRIPT_LOOK_ITEM_USE, std::to_string(roItem->ItemID));
    ROWOWScriptContext context;
    context.AttachedPlayer = player;
    context.Identity = identity;
    context.UsedItemGUID = itemGUID;
    context.UsedItemTemplateID = itemTemplateID;
    ROWOWSCRIPT->Run(*roItem->UseScript, context);
    identity->IsFamousPotion = false;
}

void ROWOWMod::PCItemHeal(Player* player, uint32 /*itemID*/, int32 hp, int32 sp)
{
    // pc_itemheal: VIT (and Increase HP Recovery, Potion Research) adds to HP potions, INT (Increase SP Recovery) to SP ones, and the
    // statuses that lower or block healing.  The item heal rate bonuses come with the bonus engine (R5)
    ROWOWIdentity* identity = GetIdentity(player);
    ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(player));
    if (hp != 0)
    {
        int32 bonus = 100 + identity->Status.Stats[ROWOW_STAT_VIT] * 2 + PCCheckSkill(player, ROWOW_SKILL_SM_RECOVERY) * 10 + PCCheckSkill(player, ROWOW_SKILL_AM_LEARNINGPOTION) * 5;
        // A potion produced by an Alchemist in the Fame Top 10 gets +50% effect [DracoRPG]
        if (identity->IsFamousPotion == true)
        {
            bonus += bonus * 50 / 100;
            if (ROWOWStatusChangeEntry const* spirit = sc != nullptr ? sc->GetSCE(ROWOW_SC_SPIRIT) : nullptr; spirit != nullptr && spirit->Val2 == ROWOW_SKILL_SL_ROGUE)
                bonus += bonus; // Receive an additional +100% effect from ranked potions to HP only
        }
        // Recovery Potion
        if (ROWOWStatusChangeEntry const* sce = sc != nullptr ? sc->GetSCE(ROWOW_SC_INCHEALRATE) : nullptr)
            bonus += bonus * sce->Val1 / 100;
        int32 boostedHP = hp * bonus / 100; // Overflow check
        if (bonus != 100 && boostedHP > hp)
            hp = boostedHP;
    }
    if (sp != 0)
    {
        int32 bonus = 100 + identity->Status.Stats[ROWOW_STAT_INT] * 2 + PCCheckSkill(player, ROWOW_SKILL_MG_SRECOVERY) * 10 + PCCheckSkill(player, ROWOW_SKILL_AM_LEARNINGPOTION) * 5;
        // A potion produced by an Alchemist in the Fame Top 10 gets +50% effect [DracoRPG]
        if (identity->IsFamousPotion == true)
            bonus += bonus * 50 / 100;
        int32 boostedSP = sp * bonus / 100;
        if (bonus != 100 && boostedSP > sp)
            sp = boostedSP;
    }
    if (sc != nullptr && sc->IsEmpty() == false)
    {
        // Critical Wound and Death Hurt stack
        int32 penalty = 0;
        if (ROWOWStatusChangeEntry const* sce = sc->GetSCE(ROWOW_SC_CRITICALWOUND))
            penalty += sce->Val2;
        if (ROWOWStatusChangeEntry const* sce = sc->GetSCE(ROWOW_SC_DEATHHURT); sce != nullptr && sce->Val3 == 1)
            penalty += 20;
        if (sc->HasSCE(ROWOW_SC_NORECOVER_STATE) == true)
            penalty = 100;
        if (penalty > 0)
        {
            hp -= hp * penalty / 100;
            sp -= sp * penalty / 100;
        }
    }
    StatusHeal(player, hp, sp);
}

void ROWOWMod::PCPercentHeal(Player* player, int32 hp, int32 sp)
{
    // pc_percentheal: percents of the maximum (status_percent_change), at least 1 when there's any; negative ones hurt, and only -100 kills
    hp = std::clamp(hp, -100, 100);
    sp = std::clamp(sp, -100, 100);
    auto getPercentAmount = [](uint32 maximum, int32 percent) -> int64
    {
        if (percent == 0)
            return 0;
        int64 amount = std::abs(percent) >= 100 ? int64(maximum) : (maximum > 10000 ? int64(maximum / 100) * std::abs(percent) : int64(maximum) * std::abs(percent) / 100);
        return std::max<int64>(1, amount);
    };
    int64 hpAmount = getPercentAmount(player->GetMaxHealth(), hp);
    int64 spAmount = getPercentAmount(player->GetMaxPower(POWER_MANA), sp);
    StatusHeal(player, hp >= 0 ? hpAmount : 0, sp >= 0 ? spAmount : 0);
    if (hp < 0)
    {
        int64 newHealth = int64(player->GetHealth()) - hpAmount;
        player->SetHealth(uint32(std::max<int64>(hp == -100 ? 0 : 1, newHealth)));
    }
    if (sp < 0)
        player->SetPower(POWER_MANA, uint32(std::max<int64>(0, int64(player->GetPower(POWER_MANA)) - spAmount)));
}

void ROWOWMod::StatusHeal(Player* player, int64 hp, int64 sp)
{
    // status_heal: the HP heal shows as RO's green heal number (the client's RO combat text, from the heal log)
    if (player->IsAlive() == false)
        return;
    if (hp > 0)
    {
        uint32 heal = uint32(std::min<int64>(hp, int64(player->GetMaxHealth() - player->GetHealth())));
        if (heal > 0)
        {
            HealInfo healInfo(player, player, heal, sSpellMgr->GetSpellInfo(ROWOW_SPELL_USE_ITEM), SPELL_SCHOOL_MASK_NORMAL);
            player->HealBySpell(healInfo);
        }
    }
    if (sp > 0)
        player->SetPower(POWER_MANA, uint32(std::min<int64>(int64(player->GetPower(POWER_MANA)) + sp, int64(player->GetMaxPower(POWER_MANA)))));
}

bool ROWOWMod::PCGetItemGroup(Player* player, std::string const& groupName)
{
    // ItemGroupDatabase::pc_get_itemgroup: each subgroup gives one item picked by its algorithm, or every item for the All algorithm.
    // Equipment comes unidentified (pc_get_itemgroup_sub with itemdb_isidentified)
    auto groupIter = ItemGroupsByName.find(groupName);
    if (groupIter == ItemGroupsByName.end())
    {
        LOG_ERROR("module.ROWOW", "ROWOWMod::PCGetItemGroup: no item group '{}'", groupName);
        return false;
    }
    std::vector<uint32> itemIDs;
    {
        std::lock_guard<std::mutex> lock(ItemGroupSharedPoolMutex);
        for (auto& subGroup : groupIter->second.SubGroupsByID)
        {
            if (subGroup.second.Algorithm == ROWOW_ITEM_GROUP_ALGORITHM_ALL)
            {
                for (ROWOWItemGroupEntry const& entry : subGroup.second.Entries)
                    itemIDs.push_back(entry.ItemID);
            }
            else if (ROWOWItemGroupEntry const* entry = GetRandomItemSubGroupEntry(subGroup.second))
                itemIDs.push_back(entry->ItemID);
        }
    }
    for (uint32 itemID : itemIDs)
    {
        auto itemTemplateIDIter = ItemTemplateIDsByItemID.find(itemID);
        auto itemIter = itemTemplateIDIter != ItemTemplateIDsByItemID.end() ? ItemsByItemTemplateID.find(itemTemplateIDIter->second) : ItemsByItemTemplateID.end();
        GiveROItem(player, itemID, 1, itemIter == ItemsByItemTemplateID.end() || IsIdentifiedByDefault(itemIter->second) == true);
    }
    return true;
}

bool ROWOWMod::ItemDBIsIdentified(uint32 itemID) const
{
    // itemdb_isidentified: equipment comes unidentified
    auto itemTemplateIDIter = ItemTemplateIDsByItemID.find(itemID);
    if (itemTemplateIDIter == ItemTemplateIDsByItemID.end())
        return true;
    auto itemIter = ItemsByItemTemplateID.find(itemTemplateIDIter->second);
    return itemIter == ItemsByItemTemplateID.end() || IsIdentifiedByDefault(itemIter->second) == true;
}

ROWOWItem const* ROWOWMod::GetROItemByItemTemplateID(uint32 itemTemplateID) const
{
    auto itemIter = ItemsByItemTemplateID.find(itemTemplateID);
    return itemIter != ItemsByItemTemplateID.end() ? &itemIter->second : nullptr;
}

std::string ROWOWMod::GetROItemName(uint32 itemID) const
{
    auto itemTemplateIDIter = ItemTemplateIDsByItemID.find(itemID);
    ItemTemplate const* itemTemplate = itemTemplateIDIter != ItemTemplateIDsByItemID.end() ? sObjectMgr->GetItemTemplate(itemTemplateIDIter->second) : nullptr;
    return itemTemplate != nullptr ? itemTemplate->Name1 : std::to_string(itemID);
}

bool ROWOWMod::GiveROItem(Player* player, uint32 itemID, uint32 amount, bool isIdentified, Item** itemOut)
{
    // pc_additem: into the bags, and what doesn't fit lands at the player's feet (off an RO map, where nothing lies on the ground, it comes by mail).  The
    // item stored (none when nothing fit) is given back for the caller to change
    auto itemTemplateIDIter = ItemTemplateIDsByItemID.find(itemID);
    if (itemTemplateIDIter == ItemTemplateIDsByItemID.end() || amount == 0)
    {
        LOG_ERROR("module.ROWOW", "ROWOWMod::GiveROItem: RO item {} wasn't converted", itemID);
        return false;
    }
    uint32 itemTemplateID = itemTemplateIDIter->second;
    ItemPosCountVec destination;
    uint32 noSpaceCount = 0;
    player->CanStoreNewItem(NULL_BAG, NULL_SLOT, destination, itemTemplateID, amount, &noSpaceCount);
    uint32 storedCount = amount - noSpaceCount;
    if (storedCount > 0)
    {
        if (Item* item = player->StoreNewItem(destination, itemTemplateID, true))
        {
            if (itemOut != nullptr)
                *itemOut = item;
            if (isIdentified == false)
                SetItemState(player, item, 0, false, false);
            player->SendNewItem(item, storedCount, true, false);
        }
    }
    if (noSpaceCount > 0)
    {
        SendItemMessage(player, "You cannot get the item.");
        if (IsROMap(player->GetMapId()) == true)
            PCDropItemAtFeet(player, itemTemplateID, noSpaceCount, isIdentified);
        else
            player->SendItemRetrievalMail(itemTemplateID, noSpaceCount);
    }
    return true;
}

void ROWOWMod::SendItemIdentifyList(Player* player)
{
    // clif_item_identify_list: the unidentified items in the bags, for the client's RO interface to list (ROWOWIDENTIFY, "<bag>:<slot>,...")
    ROWOWIdentity* identity = GetIdentity(player);
    std::string positions;
    for (uint8 bag = 0; bag <= 4; bag++)
    {
        for (uint32 slot = 1; slot <= MAX_BAG_SIZE; slot++)
        {
            Item* item = GetItemByClientPosition(player, bag, slot);
            if (item == nullptr || GetROItem(item) == nullptr || IsItemIdentified(item) == true)
                continue;
            positions += fmt::format("{}{}:{}", positions.empty() == true ? "" : ",", bag, slot);
        }
    }
    identity->IsIdentifyMenuOpen = positions.empty() == false;
    if (identity->IsIdentifyMenuOpen == false)
        return;
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_ADDON, nullptr, nullptr, "ROWOWIDENTIFY\t" + positions);
    player->SendDirectMessage(&data);
}

void ROWOWMod::SkillIdentify(Player* player, Item* item)
{
    // skill_identify: the item picked from the Magnifier's list
    ROWOWIdentity* identity = GetIdentity(player);
    if (identity->IsIdentifyMenuOpen == false)
        return;
    identity->IsIdentifyMenuOpen = false;
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_ADDON, nullptr, nullptr, "ROWOWIDENTIFY\t"); // The list closes
    player->SendDirectMessage(&data);
    if (item == nullptr || GetROItem(item) == nullptr || IsItemIdentified(item) == true)
        return;
    SetItemState(player, item, GetItemRefine(item), IsItemBroken(item), true);
    SendItemMessage(player, "Item Appraisal has completed successfully.");
}

void ROWOWMod::TeleportToRandomSpot(Player* player)
{
    // RO's random teleport lands on a free cell anywhere on the map (on a stitched map, the part the player is in)
    Position position;
    uint32 mapKey = GetUnitMapKey(player);
    if (MapSearchRandomSpot(player->GetMap(), mapKey, position) == false)
        return;
    if (mapKey != player->GetMapId())
        GetIdentity(player)->IsArrivalFloorCheckPending = true; // A part's spot is a cell's ground (GetCellPosition), as a warp's
    PlayTeleportLooks(player); // pc_randomwarp(sd, CLR_TELEPORT)
    player->NearTeleportTo(position.GetPositionX(), position.GetPositionY(), position.GetPositionZ(), player->GetOrientation());
}

void ROWOWMod::PlayTeleportLooks(Player* player)
{
    // clif_clearunit_area(CLR_TELEPORT) and the arrival's clif_spawn: the RO client plays EF_TELEPORTATION2 where the player vanishes, for those around, and
    // EF_ENTRY2 on it where it appears.  The vanish is left on the spot by an unseen trigger (the player is gone at once), cast a moment after it's
    // there so the clients around know it first.  The entry plays in the player's update once it has landed, seen by the player too (RO's client shows
    // both to the others only, but here the player gets to see its own teleport go)
    if (player == nullptr || player->IsInWorld() == false || IsInROForm(player) == false)
        return;
    GetIdentity(player)->IsTeleportEntryLookPending = true;
    if (ScriptLookSpellIDsByKey.find(GetScriptLookKey(ROWOW_SCRIPT_LOOK_EFFECT, "TELEPORTATION2")) == ScriptLookSpellIDsByKey.end())
        return;
    Creature* trigger = player->SummonTrigger(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), player->GetOrientation(),
        ROWOW_TELEPORT_LOOK_TRIGGER_DURATION_IN_MS);
    if (trigger == nullptr)
        return;
    trigger->m_Events.AddEventAtOffset([trigger]()
    {
        ROWOW->PlayScriptLook(trigger, ROWOW_SCRIPT_LOOK_EFFECT, "TELEPORTATION2");
    }, Milliseconds(ROWOW_TELEPORT_LOOK_DELAY_IN_MS));
}

void ROWOWMod::UpdateTeleportEntryLook(Player* player)
{
    // The player's update (in the form): once the teleport has landed, the entry effect plays on it a moment later, when the clients around know it's there
    ROWOWIdentity* identity = GetIdentity(player);
    if (identity->IsTeleportEntryLookPending == false || player->IsBeingTeleported() == true || player->IsInWorld() == false)
        return;
    identity->IsTeleportEntryLookPending = false;
    if (IsROMap(player->GetMapId()) == false)
        return;
    ObjectGuid playerGUID = player->GetGUID();
    player->m_Events.AddEventAtOffset([player, playerGUID]()
    {
        if (player->IsInWorld() == true && player->GetGUID() == playerGUID && ROWOW->IsInROForm(player) == true)
            ROWOW->PlayScriptLook(player, ROWOW_SCRIPT_LOOK_EFFECT, "ENTRY2");
    }, Milliseconds(ROWOW_TELEPORT_LOOK_DELAY_IN_MS));
}

bool ROWOWMod::MapSearchRandomSpot(Map* map, uint32 mapKey, Position& positionOut) const
{
    // map_search_freecell over the whole map: one of the map's monsters' homes is a spot known to be walkable (a little above the ground, so
    // arriving there never starts under it).  On the unit's map's thread
    if (map == nullptr || map->Instanceable() == true)
        return false;

    // A part of a stitched map (a map key other than the map's) is a map of its own, its monsters' homes a few among the whole map's: as
    // map_search_freecell(-1, -1), a free cell of the part (xs * ys tries, 500 at most), on the server's cells' ground
    if (mapKey != map->GetId())
    {
        auto partIter = MapPartsByMapKey.find(mapKey);
        ROWOWMapGrid const* mapGrid = GetMapGrid(map->GetId());
        if (partIter != MapPartsByMapKey.end() && partIter->second->MapID == map->GetId() && partIter->second->HasArea() == true && mapGrid != nullptr &&
            mapGrid->HasCells() == true)
        {
            ROWOWMapPart const* part = partIter->second;
            int32 x = 0, y = 0;
            ROWOWPosition position;
            if (MapSearchFreeCellOfSpawn(*mapGrid, map, -1, -1, -1, -1, std::min(part->CellWidth * part->CellHeight, 500), x, y, part) == false ||
                GetCellPosition(map->GetId(), x, y, position) == false)
                return false;
            positionOut.Relocate(position.X, position.Y, position.Z);
            return true;
        }
    }

    // Off every part of a stitched map (past its outer edge): no map of its own to land on, and a home anywhere on the stitched map is another part
    if (mapKey == map->GetId() && MapPartsByMapID.find(map->GetId()) != MapPartsByMapID.end())
        return false;
    Map::CreatureBySpawnIdContainer const& creaturesBySpawnID = map->GetCreatureBySpawnIdStore();
    if (creaturesBySpawnID.empty() == true)
        return false;
    uint32 index = urand(0, uint32(creaturesBySpawnID.size() - 1));
    Map::CreatureBySpawnIdContainer::const_iterator creatureIter = creaturesBySpawnID.begin();
    std::advance(creatureIter, index);
    Creature* creature = creatureIter->second;
    if (creature == nullptr)
        return false;
    Position const& homePosition = creature->GetHomePosition();
    positionOut.Relocate(homePosition.GetPositionX(), homePosition.GetPositionY(), homePosition.GetPositionZ() + 0.5f);
    return true;
}

void ROWOWMod::TeleportToSavePoint(Player* player)
{
    ROWOWIdentity* identity = GetIdentity(player);
    ROWOWPosition savePoint = identity->SavePoint;
    if (savePoint.IsSet == false && GetCellPosition(StartMapID, StartCellX, StartCellY, savePoint) == false)
        return;
    player->TeleportTo(savePoint.MapID, savePoint.X, savePoint.Y, savePoint.Z, savePoint.O);
}

bool ROWOWMod::GetClientItemPosition(Item const* item, uint32& bagOut, uint32& slotOut)
{
    // The client numbers the backpack 0 and the bags 1-4, and their slots from 1
    uint8 bag = item->GetBagSlot();
    uint8 slot = item->GetSlot();
    if (bag == INVENTORY_SLOT_BAG_0 && slot >= INVENTORY_SLOT_ITEM_START && slot < INVENTORY_SLOT_ITEM_END)
    {
        bagOut = 0;
        slotOut = uint32(slot - INVENTORY_SLOT_ITEM_START) + 1;
        return true;
    }
    if (bag >= INVENTORY_SLOT_BAG_START && bag < INVENTORY_SLOT_BAG_END)
    {
        bagOut = uint32(bag - INVENTORY_SLOT_BAG_START) + 1;
        slotOut = uint32(slot) + 1;
        return true;
    }
    return false;
}

Item* ROWOWMod::GetItemByClientPosition(Player* player, uint32 bag, uint32 slot)
{
    if (slot == 0)
        return nullptr;
    if (bag == 0)
    {
        if (slot > uint32(INVENTORY_SLOT_ITEM_END - INVENTORY_SLOT_ITEM_START))
            return nullptr;
        return player->GetItemByPos(INVENTORY_SLOT_BAG_0, uint8(INVENTORY_SLOT_ITEM_START + slot - 1));
    }
    if (bag > uint32(INVENTORY_SLOT_BAG_END - INVENTORY_SLOT_BAG_START) || slot > MAX_BAG_SIZE)
        return nullptr;
    return player->GetItemByPos(uint8(INVENTORY_SLOT_BAG_START + bag - 1), uint8(slot - 1));
}

void ROWOWMod::UpdateIdentityEquipment(Player* player, ROWOWIdentity& identity)
{
    if (PCCheckItem(player, identity) == true)
        RefreshROForm(player);
    else
        SendEquipmentToClient(player, identity, false);
    SendItemCardsToClient(player, identity, false);
}

void ROWOWMod::SendEquipmentToClient(Player* player, ROWOWIdentity& identity, bool isForced)
{
    // ROWOWEQUIP: "<equipment positions>:<bag>:<slot>,..." for each worn item
    std::string message = "ROWOWEQUIP\t";
    bool isFirst = true;
    for (auto const& itemGUIDByEquipPosition : identity.ItemGUIDsByEquipPosition)
    {
        Item* item = player->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(itemGUIDByEquipPosition.second));
        uint32 bag = 0;
        uint32 slot = 0;
        if (item == nullptr || GetClientItemPosition(item, bag, slot) == false)
            continue;
        message += fmt::format("{}{}:{}:{}", isFirst == true ? "" : ",", itemGUIDByEquipPosition.first, bag, slot);
        isFirst = false;
    }
    if (isForced == false && message == identity.SentEquipmentMessage)
        return;
    identity.SentEquipmentMessage = message;
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_ADDON, nullptr, nullptr, message);
    player->SendDirectMessage(&data);
}
