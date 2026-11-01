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
//  Taming pets are a port of rAthena's pet.cpp:
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
//  For more information, see LICENCE in the main folder

#include "Chat.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Item.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PassiveAI.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedGossip.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "TemporarySummon.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include "ROWOW.h"
#include "ROWOW_Script.h"
#include "ROWOW_Skill.h"

using namespace std;

// RO's taming pets (R11), rAthena's pet.cpp under its names.  They aren't RO's companions (minipets, IsROCompanionPetSpell): a taming item catches a monster into
// an egg, a Pet Incubator hatches it, and the pet follows its master, hungers and grows fond of it.  At Loyal its script is a bonus on the master.
//
// A pet (pet_data) is ROWOWPetData on the master's identity, so only the master's thread touches it.  Its creature only shows it: it's on the master's map and
// runs no AI of its own, the master's update walks it (pet_ai_hard's map_foreachpc), as RO's pets are.  A pet's row (mod_rowow_pet, s_pet) stays when it's in its egg.
//
// An egg is a real item.  Its pet is named on the item (its card[1..2]), as an enchantment ID past any SpellItemEnchantment row on a slot the client doesn't show, so
// the bank, the guild bank, a trade and the cart all keep it.  As rAthena's clients before 2018 had it, hatching takes the egg away and returning to the egg makes
// a new one: the bags never hold a pet that's out

namespace
{
    constexpr uint32 MIN_PETTHINKTIME = 100;
    constexpr uint64 MIN_RANDOMWALKTIME = 4000;
    constexpr uint32 PET_STARVING_HUNGRY_DELAY_IN_MS = 20000; // While starving, it's every 20 seconds
    constexpr uint64 PET_TELEPORT_DELAY_IN_MS = 3000; // unit_check_start_teleport_timer: too far from the master this long, it's put beside it
    constexpr uint64 PET_PERFORMANCE_STOP_IN_MS = 2000;
    constexpr int32 MAX_PETLOOT_SIZE = 30;

    // ZC_PET_ACT's acts, data\pettalktable.xml's
    enum PetTalkAct : uint8
    {
        PET_TALK_FEEDING = 0,
        PET_TALK_HUNTING = 1,
        PET_TALK_DANGER = 2,
        PET_TALK_DEAD = 3,
        PET_TALK_STAND = 4,
        PET_TALK_SPECIAL = 5,
        PET_TALK_LEVELUP = 6,
        PET_TALK_PERFORMANCE_1 = 7,
        PET_TALK_CONNECT = 10,
    };

    void SendAddonMessage(Player* player, std::string const& message)
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_ADDON, nullptr, nullptr, message);
        player->SendDirectMessage(&data);
    }

    void SendPetMessage(Player* player, std::string const& message)
    {
        ChatHandler(player->GetSession()).SendSysMessage(message.c_str());
    }

    // check_distance, with no CIRCULAR_AREA
    bool CheckDistance(int32 dx, int32 dy, int32 distance)
    {
        return std::max(std::abs(dx), std::abs(dy)) <= distance;
    }

    std::string ToLowerText(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char character) { return char(std::tolower(character)); });
        return text;
    }

    uint64 GetPetTalkKey(uint32 mobID, uint8 hunger, uint8 act)
    {
        return (uint64(mobID) << 16) | (uint64(hunger) << 8) | act;
    }
}

void ROWOWMod::LoadPetData()
{
    // pet_db, after the items (which it names) and the bonus engine (its scripts are bonus scripts)
    PetsByMobID.clear();
    PetLookEntriesByKey.clear();
    PetTalkLinesByKey.clear();
    if (IsEnabled == false)
        return;
    ROWOWDataTable const* petTable = GetDataTable("mod_rowow_db_pet");
    ROWOWDataTable const* mobTable = GetDataTable(ROWOW_DATA_TABLE_MOB);
    if (petTable == nullptr || mobTable == nullptr)
        return;
    std::unordered_map<std::string, uint32> mobIDsByAegisName;
    int32 mobIDColumnIndex = mobTable->GetColumnIndex("Id");
    int32 mobAegisColumnIndex = mobTable->GetColumnIndex("AegisName");
    for (size_t rowIndex = 0; rowIndex < mobTable->Rows.size(); rowIndex++)
        mobIDsByAegisName[ToLowerText(ROWOWDataTable::UnescapeText(mobTable->GetCell(rowIndex, mobAegisColumnIndex)))] = uint32(mobTable->GetInt(rowIndex, mobIDColumnIndex, 0));
    auto getItemID = [this](std::string const& name)
    {
        auto itemIDIter = ItemIDsByAegisName.find(ToLowerText(name));
        return itemIDIter != ItemIDsByAegisName.end() ? itemIDIter->second : 0;
    };

    uint32 unrunnableCount = 0;
    for (size_t rowIndex = 0; rowIndex < petTable->Rows.size(); rowIndex++)
    {
        auto mobIDIter = mobIDsByAegisName.find(ToLowerText(ROWOWDataTable::UnescapeText(petTable->GetCell(rowIndex, petTable->GetColumnIndex("Mob")))));
        if (mobIDIter == mobIDsByAegisName.end())
            continue;
        ROWOWPetDB& pet = PetsByMobID[mobIDIter->second];
        pet.MobID = mobIDIter->second;
        pet.ItemID = getItemID(petTable->GetCell(rowIndex, petTable->GetColumnIndex("TameItem")));
        pet.EggID = getItemID(petTable->GetCell(rowIndex, petTable->GetColumnIndex("EggItem")));
        pet.AcceID = getItemID(petTable->GetCell(rowIndex, petTable->GetColumnIndex("EquipItem")));
        pet.FoodID = getItemID(petTable->GetCell(rowIndex, petTable->GetColumnIndex("FoodItem")));
        pet.Fullness = int32(petTable->GetInt(rowIndex, petTable->GetColumnIndex("Fullness"), 0));
        pet.HungryDelay = int32(petTable->GetInt(rowIndex, petTable->GetColumnIndex("HungryDelay"), 60)) * 1000;
        pet.Intimate = int32(petTable->GetInt(rowIndex, petTable->GetColumnIndex("IntimacyStart"), 250));
        pet.RHungry = int32(petTable->GetInt(rowIndex, petTable->GetColumnIndex("IntimacyFed"), 50));
        pet.RFull = int32(petTable->GetInt(rowIndex, petTable->GetColumnIndex("IntimacyOverfed"), -100));
        pet.Die = int32(petTable->GetInt(rowIndex, petTable->GetColumnIndex("IntimacyOwnerDie"), -20));
        pet.Capture = int32(petTable->GetInt(rowIndex, petTable->GetColumnIndex("CaptureRate"), 0));
        pet.IsSpecialPerformance = petTable->GetCell(rowIndex, petTable->GetColumnIndex("SpecialPerformance")) != "false";
        std::string script = ROWOWDataTable::UnescapeText(petTable->GetCell(rowIndex, petTable->GetColumnIndex("Script")));
        if (script.empty() == false)
        {
            pet.BonusScript = ROWOWSCRIPT->Compile(script, "pet " + std::to_string(pet.MobID));
            if (pet.BonusScript->Error.empty() == false || pet.BonusScript->IsRunnable() == false)
            {
                LOG_ERROR("module.ROWOW", "ROWOWMod::LoadPetData: the script of pet {} can't run: {}", pet.MobID, pet.BonusScript->Error);
                unrunnableCount++;
            }
        }
    }

    // The creatures, by pet and accessory, and what the pets say, which the converter took from the client
    if (QueryResult result = WorldDatabase.Query("SELECT `MobID`, `EquipItemID`, `CreatureTemplateID` FROM `mod_rowow_pet_look`"))
    {
        do
        {
            Field* fields = result->Fetch();
            PetLookEntriesByKey[(uint64(fields[0].Get<uint32>()) << 32) | fields[1].Get<uint32>()] = fields[2].Get<uint32>();
        } while (result->NextRow());
    }
    if (QueryResult result = WorldDatabase.Query("SELECT `MobID`, `Hunger`, `Act`, `Text` FROM `mod_rowow_pet_talk` ORDER BY `MobID`, `Hunger`, `Act`, `Line`"))
    {
        do
        {
            Field* fields = result->Fetch();
            PetTalkLinesByKey[GetPetTalkKey(fields[0].Get<uint32>(), fields[1].Get<uint8>(), fields[2].Get<uint8>())].push_back(fields[3].Get<std::string>());
        } while (result->NextRow());
    }
    if (QueryResult result = CharacterDatabase.Query("SELECT MAX(`PetID`) FROM `mod_rowow_pet`"))
        LastPetID = result->Fetch()[0].Get<uint32>();
    LOG_INFO("module.ROWOW", "ROWOWMod::LoadPetData loaded {} pets ({} scripts can't run), {} looks, the talk of {} acts", PetsByMobID.size(), unrunnableCount, PetLookEntriesByKey.size(),
        PetTalkLinesByKey.size());
}

void ROWOWMod::RegisterPetCommands()
{
    // pet (catchpet), bpet (birthpet), makepet, getpetinfo, petloot: the script commands of pets
    ROWOWSCRIPT->RegisterConstant("PETINFO_ID", 0);
    ROWOWSCRIPT->RegisterConstant("PETINFO_CLASS", 1);
    ROWOWSCRIPT->RegisterConstant("PETINFO_NAME", 2);
    ROWOWSCRIPT->RegisterConstant("PETINFO_INTIMATE", 3);
    ROWOWSCRIPT->RegisterConstant("PETINFO_HUNGRY", 4);
    ROWOWSCRIPT->RegisterConstant("PETINFO_RENAMED", 5);
    ROWOWSCRIPT->RegisterConstant("PETINFO_LEVEL", 6);
    ROWOWSCRIPT->RegisterConstant("PETINFO_BLOCKID", 7);
    ROWOWSCRIPT->RegisterConstant("PETINFO_EGGID", 8);
    ROWOWSCRIPT->RegisterConstant("PETINFO_FOODID", 9);
    ROWOWSCRIPT->RegisterConstant("PET_INTIMATE_NONE", ROWOW_PET_INTIMATE_NONE);
    ROWOWSCRIPT->RegisterConstant("PET_INTIMATE_AWKWARD", ROWOW_PET_INTIMATE_AWKWARD);
    ROWOWSCRIPT->RegisterConstant("PET_INTIMATE_SHY", ROWOW_PET_INTIMATE_SHY);
    ROWOWSCRIPT->RegisterConstant("PET_INTIMATE_NEUTRAL", ROWOW_PET_INTIMATE_NEUTRAL);
    ROWOWSCRIPT->RegisterConstant("PET_INTIMATE_CORDIAL", ROWOW_PET_INTIMATE_CORDIAL);
    ROWOWSCRIPT->RegisterConstant("PET_INTIMATE_LOYAL", ROWOW_PET_INTIMATE_LOYAL);
    ROWOWSCRIPT->RegisterConstant("PET_INTIMATE_MAX", ROWOW_PET_INTIMATE_MAX);

    auto catchPet = [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // catchpet ({<item id>{,<flag>}}): the item it's run from
        uint32 itemID = !arguments.empty() ? uint32(arguments[0].GetNumber()) : 0;
        if (itemID == 0)
        {
            ROWOWItem const* roItem = GetROItemByItemTemplateID(context.UsedItemTemplateID);
            itemID = roItem != nullptr ? roItem->ItemID : 0;
        }
        if (context.AttachedPlayer != nullptr)
            PetCatchProcessStart(context.AttachedPlayer, itemID, arguments.size() > 1 ? uint8(arguments[1].GetNumber()) : uint8(ROWOW_PET_CATCH_NORMAL));
        return ROWOWScriptValue::FromNumber(0);
    };
    ROWOWSCRIPT->RegisterCommand("pet", catchPet);
    ROWOWSCRIPT->RegisterCommand("catchpet", catchPet);
    auto birthPet = [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const&)
    {
        // birthpet: do not send egg list, when you already have a pet
        if (context.AttachedPlayer != nullptr && GetIdentity(context.AttachedPlayer)->Pet == nullptr)
            ClifSendEgg(context.AttachedPlayer);
        return ROWOWScriptValue::FromNumber(0);
    };
    ROWOWSCRIPT->RegisterCommand("bpet", birthPet);
    ROWOWSCRIPT->RegisterCommand("birthpet", birthPet);
    ROWOWSCRIPT->RegisterCommand("makepet", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // makepet <mob id>
        auto petIter = PetsByMobID.find(arguments.empty() == false ? uint32(arguments[0].GetNumber()) : 0);
        if (context.AttachedPlayer != nullptr && petIter != PetsByMobID.end())
            PetCreateEgg(context.AttachedPlayer, petIter->second);
        return ROWOWScriptValue::FromNumber(0);
    });
    ROWOWSCRIPT->RegisterCommand("getpetinfo", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // getpetinfo(<type>): 0, or "null" for the name, with no pet out
        int64 type = arguments.empty() == false ? arguments[0].GetNumber() : 0;
        ROWOWPetData const* pd = context.Identity != nullptr ? context.Identity->Pet.get() : nullptr;
        if (pd == nullptr)
            return type == 2 ? ROWOWScriptValue::FromText("null") : ROWOWScriptValue::FromNumber(0);
        switch (type)
        {
            case 0: return ROWOWScriptValue::FromNumber(pd->Pet.PetID);
            case 1: return ROWOWScriptValue::FromNumber(pd->Pet.MobID);
            case 2: return ROWOWScriptValue::FromText(pd->Pet.Name);
            case 3: return ROWOWScriptValue::FromNumber(pd->Pet.Intimate);
            case 4: return ROWOWScriptValue::FromNumber(pd->Pet.Hungry);
            case 5: return ROWOWScriptValue::FromNumber(pd->Pet.IsRenamed == true ? 1 : 0);
            case 6: return ROWOWScriptValue::FromNumber(pd->Pet.Level);
            case 7: return ROWOWScriptValue::FromNumber(int64(pd->CreatureGUID.GetCounter()));
            case 8: return ROWOWScriptValue::FromNumber(pd->Pet.EggID);
            case 9: return ROWOWScriptValue::FromNumber(pd->DB != nullptr ? pd->DB->FoodID : 0);
            default: return ROWOWScriptValue::FromNumber(0);
        }
    });
    ROWOWSCRIPT->RegisterCommand("petloot", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        if (context.AttachedPlayer != nullptr && arguments.empty() == false)
            PetLoot(context.AttachedPlayer, int32(arguments[0].GetNumber()));
        return ROWOWScriptValue::FromNumber(0);
    });
}

ROWOWPetData* ROWOWMod::GetPet(Player const* player) const
{
    ROWOWIdentity const* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    return identity != nullptr ? identity->Pet.get() : nullptr;
}

uint32 ROWOWMod::GetEggPetID(Item const* item)
{
    // card[0] == CARD0_PET: the pet in card[1] and card[2]
    uint32 enchantment = item->GetEnchantmentId(ROWOW_ITEM_PET_ENCHANTMENT_SLOT);
    return enchantment > ROWOW_ITEM_PET_ENCHANTMENT_START ? enchantment - ROWOW_ITEM_PET_ENCHANTMENT_START : 0;
}

uint32 ROWOWMod::GetPetLookEntry(uint32 mobID, uint32 equipItemID) const
{
    // status_set_viewdata: the accessory's look, for the pet's own accessory
    auto lookIter = PetLookEntriesByKey.find((uint64(mobID) << 32) | equipItemID);
    if (lookIter == PetLookEntriesByKey.end())
        lookIter = PetLookEntriesByKey.find(uint64(mobID) << 32);
    return lookIter != PetLookEntriesByKey.end() ? lookIter->second : 0;
}

int32 ROWOWMod::PetHungryVal(ROWOWPetData const& pd)
{
    // pet_hungry_val
    if (pd.Pet.Hungry > ROWOW_PET_HUNGRY_SATISFIED)
        return 4;
    if (pd.Pet.Hungry > ROWOW_PET_HUNGRY_NEUTRAL)
        return 3;
    if (pd.Pet.Hungry > ROWOW_PET_HUNGRY_HUNGRY)
        return 2;
    if (pd.Pet.Hungry > ROWOW_PET_HUNGRY_VERY_HUNGRY)
        return 1;
    return 0;
}

Creature* ROWOWMod::GetPetCreature(Player* sd, ROWOWPetData const& pd) const
{
    return pd.CreatureGUID.IsEmpty() == true ? nullptr : ObjectAccessor::GetCreature(*sd, pd.CreatureGUID);
}

void ROWOWMod::ClifPetTalk(Player* sd, ROWOWPetData& pd, uint8 act)
{
    // clif_pet_emotion: a line of data\pettalktable.xml for its act and hunger, over the pet.  The client picks the moment in RO, here the module
    Creature* md = GetPetCreature(sd, pd);
    if (md == nullptr)
        return;
    auto linesIter = PetTalkLinesByKey.find(GetPetTalkKey(pd.Pet.MobID, uint8(PetHungryVal(pd)), act));
    if (linesIter == PetTalkLinesByKey.end() || linesIter->second.empty() == true)
        return;
    md->Say(linesIter->second[urand(0, uint32(linesIter->second.size() - 1))], LANG_UNIVERSAL);
}

void ROWOWMod::ClifSendPetStatus(Player* sd)
{
    // clif_send_petstatus and clif_send_petdata for the RO_Identity addon's pet window: "ROWOWPET\tS\t<name>\t<level>\t<hungry>\t<intimate>\t<equip>\t<mob>\t<renamed>\t<food>", or
    // "ROWOWPET\tN" when there's no pet out.  (bModified: the name can't change again)
    ROWOWPetData const* pd = GetPet(sd);
    if (pd == nullptr)
    {
        SendAddonMessage(sd, "ROWOWPET\tN");
        return;
    }
    ItemTemplate const* equipTemplate = nullptr;
    auto equipTemplateIDIter = ItemTemplateIDsByItemID.find(pd->Pet.EquipID);
    if (equipTemplateIDIter != ItemTemplateIDsByItemID.end())
        equipTemplate = sObjectMgr->GetItemTemplate(equipTemplateIDIter->second);
    ItemTemplate const* foodTemplate = nullptr;
    auto foodTemplateIDIter = ItemTemplateIDsByItemID.find(pd->DB != nullptr ? pd->DB->FoodID : 0);
    if (foodTemplateIDIter != ItemTemplateIDsByItemID.end())
        foodTemplate = sObjectMgr->GetItemTemplate(foodTemplateIDIter->second);
    SendAddonMessage(sd, fmt::format("ROWOWPET\tS\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}", pd->Pet.Name, pd->Pet.Level, pd->Pet.Hungry, pd->Pet.Intimate, equipTemplate != nullptr ? equipTemplate->Name1 : "",
        pd->Pet.MobID, (pd->Pet.IsRenamed == true && BattleConfig.PetRename == 0) ? 1 : 0, foodTemplate != nullptr ? foodTemplate->Name1 : ""));
}

void ROWOWMod::PetSetIntimate(Player* sd, ROWOWPetData& pd, int32 value)
{
    // pet_set_intimate: bounded by PET_INTIMATE_NONE and PET_INTIMATE_MAX.  The egg isn't in the bags while it's out, so losing the pet has none to delete
    pd.Pet.Intimate = std::clamp(value, int32(ROWOW_PET_INTIMATE_NONE), int32(ROWOW_PET_INTIMATE_MAX));
    if (pd.Pet.Intimate <= ROWOW_PET_INTIMATE_NONE)
        SendPetMessage(sd, fmt::format("{} has run away.", pd.Pet.Name));
    if (pd.DB != nullptr && pd.DB->BonusScript != nullptr)
        RefreshROForm(sd);
}

void ROWOWMod::PetHungry(Player* sd, ROWOWPetData& pd, uint32 diff)
{
    // pet_hungry: its hungry timer, which only runs while it's out in the RO world
    if (pd.HungryTimer > diff)
    {
        pd.HungryTimer -= diff;
        return;
    }
    if (pd.Pet.Intimate <= ROWOW_PET_INTIMATE_NONE)
    {
        pd.HungryTimer = PET_STARVING_HUNGRY_DELAY_IN_MS;
        return; // You lost the pet already, the rest is irrelevant.
    }
    int32 interval = pd.DB->HungryDelay;
    if (BattleConfig.PetHungryDelayRate != 100)
        interval = (pd.DB->HungryDelay * BattleConfig.PetHungryDelayRate) / 100;
    pd.Pet.Hungry -= pd.DB->Fullness;
    if (pd.Pet.Hungry < ROWOW_PET_HUNGRY_NONE)
    {
        pd.Pet.Hungry = ROWOW_PET_HUNGRY_NONE;
        PetSetIntimate(sd, pd, pd.Pet.Intimate + pd.DB->HungryIntimacyDec);
        if (pd.Pet.Intimate <= ROWOW_PET_INTIMATE_NONE)
            PetSetSpeed(sd, pd, PetGetWalkSpeed(sd));
        interval = PET_STARVING_HUNGRY_DELAY_IN_MS;
    }
    ClifSendPetStatus(sd);
    if (pd.Pet.Hungry <= ROWOW_PET_HUNGRY_HUNGRY)
        ClifPetTalk(sd, pd, PET_TALK_STAND);
    pd.HungryTimer = uint32(std::max(interval, 1));
}

int32 ROWOWMod::PetGetWalkSpeed(Player const* sd) const
{
    // pet_data::get_pet_walk_speed: 1 the master's, 2 DEFAULT_WALK_SPEED, 3 mob_db's
    ROWOWPetData const* pd = GetPet(sd);
    switch (BattleConfig.PetWalkSpeed)
    {
        case 2: return 150;
        case 3:
        {
            auto mobStatusIter = pd != nullptr ? MobStatusesByMobID.find(pd->Pet.MobID) : MobStatusesByMobID.end();
            if (mobStatusIter != MobStatusesByMobID.end())
                return mobStatusIter->second.Speed;
            break;
        }
        default: break;
    }
    ROWOWStatus const* status = GetROStatus(sd);
    return status != nullptr ? status->Speed : 150;
}

void ROWOWMod::PetSetSpeed(Player* sd, ROWOWPetData& pd, int32 speed)
{
    // pd->status.speed: ms a cell.  WoW walks the creature's spline at its speed rate, so that's set to the same pace
    Creature* md = GetPetCreature(sd, pd);
    if (md == nullptr || speed <= 0)
        return;
    pd.Speed = speed;
    md->CustomData.GetDefault<ROWOWMobStatus>("ROWOWMobStatus")->Status.Speed = speed;
    float yardsPerSecond = CellSize * 1000.f / float(speed);
    md->SetSpeed(MOVE_WALK, yardsPerSecond / baseMoveSpeed[MOVE_WALK], true);
    md->SetSpeed(MOVE_RUN, yardsPerSecond / baseMoveSpeed[MOVE_RUN], true);
}

void ROWOWMod::PetSpawn(Player* sd, ROWOWPetData& pd)
{
    // pet_data_init's placing and clif_spawn: 2 cells behind the master, on the ground near its height
    if (GetPetCreature(sd, pd) != nullptr || sd->IsInWorld() == false || IsInROForm(sd) == false)
        return;
    uint32 entry = GetPetLookEntry(pd.Pet.MobID, pd.Pet.EquipID);
    if (entry == 0)
    {
        LOG_ERROR("module.ROWOW", "ROWOWMod::PetSpawn: pet {} has no creature (run the converter)", pd.Pet.MobID);
        return;
    }
    int32 x = 0, y = 0, toX = 0, toY = 0;
    GetUnitCell(sd, x, y);
    float worldX = sd->GetPositionX(), worldY = sd->GetPositionY(), worldZ = sd->GetPositionZ();
    if (UnitCalcPos(sd->GetMap(), x, y, x, y, UnitGetDir(sd), toX, toY) == true)
    {
        float cellX = 0, cellY = 0;
        if (GetCellWorldPosition(sd->GetMapId(), toX, toY, cellX, cellY) == true)
        {
            float cellZ = sd->GetMap()->GetHeight(sd->GetPhaseMask(), cellX, cellY, sd->GetPositionZ() + ROWOW_SKILL_CELL_GROUND_SEARCH_HEIGHT, true, ROWOW_SKILL_CELL_GROUND_SEARCH_DISTANCE);
            if (cellZ > INVALID_HEIGHT)
            {
                worldX = cellX;
                worldY = cellY;
                worldZ = cellZ;
            }
        }
    }
    TempSummon* md = sd->SummonCreature(entry, worldX, worldY, worldZ, sd->GetOrientation(), TEMPSUMMON_MANUAL_DESPAWN);
    if (md == nullptr)
        return;
    md->SetCreatorGUID(sd->GetGUID());
    md->SetReactState(REACT_PASSIVE);
    md->SetImmuneToAll(true);
    md->CustomData.GetDefault<ROWOWPetOwner>("ROWOWPetOwner")->OwnerGUID = sd->GetGUID();
    ROWOWMobStatus* status = md->CustomData.GetDefault<ROWOWMobStatus>("ROWOWMobStatus");
    status->Status.Mode = ROWOW_MODE_CANMOVE;
    pd.CreatureGUID = md->GetGUID();
    pd.TeleportTime = 0;
    PetSetSpeed(sd, pd, PetGetWalkSpeed(sd));
    PetRefreshName(sd, pd);
}

void ROWOWMod::PetRefreshName(Player* sd, ROWOWPetData const& pd)
{
    // A pet's own name goes by the pet name query, as a script monster's (the number is the creature's)
    Creature* md = GetPetCreature(sd, pd);
    if (md == nullptr || pd.Pet.IsRenamed == false || pd.Pet.Name == md->GetName())
        return;
    md->SetName(pd.Pet.Name);
    md->SetUInt32Value(UNIT_FIELD_PETNUMBER, 0x80000000 | (md->GetGUID().GetCounter() & 0x7FFFFFFF));
    md->SetUInt32Value(UNIT_FIELD_PET_NAME_TIMESTAMP, uint32(GameTime::GetGameTime().count()));
}

void ROWOWMod::PetRefreshLook(Player* sd, ROWOWPetData const& pd)
{
    // status_set_viewdata and CHANGESTATEPET_ACCESSORY: the look of the pet in its accessory, the same creature going on
    Creature* md = GetPetCreature(sd, pd);
    CreatureTemplate const* cinfo = sObjectMgr->GetCreatureTemplate(GetPetLookEntry(pd.Pet.MobID, pd.Pet.EquipID));
    if (md == nullptr || cinfo == nullptr)
        return;
    if (CreatureModel const* model = cinfo->GetFirstValidModel())
    {
        md->SetDisplayId(model->CreatureDisplayID);
        md->SetNativeDisplayId(model->CreatureDisplayID);
    }
}

void ROWOWMod::PetUnspawn(Player* sd, ROWOWPetData& pd)
{
    if (Creature* md = GetPetCreature(sd, pd))
        md->DespawnOrUnsummon();
    pd.CreatureGUID.Clear();
}

void ROWOWMod::PetArrive(Player* sd)
{
    // pc_setpos and clif_parse_LoadEndAck for the pet that's out: it arrives with its master
    ROWOWIdentity* identity = GetIdentity(sd);
    if (identity->Pet == nullptr)
        return;
    if (identity->Pet->Pet.Intimate <= ROWOW_PET_INTIMATE_NONE)
    {
        PetFree(sd);
        return;
    }
    PetSpawn(sd, *identity->Pet);
    ClifSendPetStatus(sd);
    ClifPetTalk(sd, *identity->Pet, PET_TALK_CONNECT);
}

void ROWOWMod::PetLeaveMap(Player* sd)
{
    // unit_remove_map: a lost pet goes for good once its master leaves the map or the game; another waits for the master
    ROWOWIdentity* identity = sd->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    if (identity == nullptr || identity->Pet == nullptr)
        return;
    if (identity->Pet->Pet.Intimate <= ROWOW_PET_INTIMATE_NONE)
    {
        PetFree(sd);
        return;
    }
    PetLootItemDrop(sd, *identity->Pet, sd);
    PetUnspawn(sd, *identity->Pet);
}

void ROWOWMod::PetFree(Player* sd)
{
    // unit_free with a lost pet: its row goes, and the status bonus with it
    ROWOWIdentity* identity = GetIdentity(sd);
    if (identity->Pet == nullptr)
        return;
    PetLootItemDrop(sd, *identity->Pet, nullptr);
    PetUnspawn(sd, *identity->Pet);
    CharacterDatabase.Execute("DELETE FROM `mod_rowow_pet` WHERE `PetID` = {}", identity->Pet->Pet.PetID);
    identity->Pet.reset();
    ClifSendPetStatus(sd);
}

void ROWOWMod::SavePet(ROWOWPet const& pet, CharacterDatabaseTransaction trans) const
{
    std::string name = pet.Name;
    CharacterDatabase.EscapeString(name);
    trans->Append("UPDATE `mod_rowow_pet` SET `OwnerGUID` = {}, `Name` = '{}', `EquipID` = {}, `Intimate` = {}, `Hungry` = {}, `IsRenamed` = {}, `IsIncubate` = {} WHERE `PetID` = {}",
        pet.OwnerGUID, name, pet.EquipID, pet.Intimate, pet.Hungry, pet.IsRenamed == true ? 1 : 0, pet.IsIncubate == true ? 1 : 0, pet.PetID);
}

void ROWOWMod::SaveIdentityPet(Player* /*player*/, ROWOWIdentity& identity, CharacterDatabaseTransaction trans) const
{
    if (identity.Pet != nullptr)
        SavePet(identity.Pet->Pet, trans);
}

void ROWOWMod::LoadIdentityPet(Player* player, ROWOWIdentity& identity)
{
    // sd->status.pet_id and intif_request_petdata: the pet that's out, which comes back with its master
    QueryResult result = CharacterDatabase.Query("SELECT `PetID`, `OwnerGUID`, `MobID`, `Name`, `Level`, `EggID`, `EquipID`, `Intimate`, `Hungry`, `IsRenamed`, `IsIncubate` FROM `mod_rowow_pet` "
        "WHERE `OwnerGUID` = {} AND `IsIncubate` = 0", player->GetGUID().GetCounter());
    if (result == nullptr)
        return;
    ROWOWPet pet;
    if (ReadPetRow(result->Fetch(), pet) == true)
        PetDataInit(identity, pet);
}

bool ROWOWMod::ReadPetRow(Field const* fields, ROWOWPet& petOut) const
{
    petOut.PetID = fields[0].Get<uint32>();
    petOut.OwnerGUID = fields[1].Get<uint32>();
    petOut.MobID = fields[2].Get<uint32>();
    petOut.Name = fields[3].Get<std::string>();
    petOut.Level = fields[4].Get<uint32>();
    petOut.EggID = fields[5].Get<uint32>();
    petOut.EquipID = fields[6].Get<uint32>();
    petOut.Intimate = fields[7].Get<int32>();
    petOut.Hungry = fields[8].Get<int32>();
    petOut.IsRenamed = fields[9].Get<uint8>() != 0;
    petOut.IsIncubate = fields[10].Get<uint8>() != 0;
    return PetsByMobID.find(petOut.MobID) != PetsByMobID.end();
}

void ROWOWMod::PetDataInit(ROWOWIdentity& identity, ROWOWPet const& pet)
{
    // pet_data_init, without the placing (PetSpawn)
    identity.Pet = std::make_unique<ROWOWPetData>();
    identity.Pet->Pet = pet;
    identity.Pet->DB = &PetsByMobID.at(pet.MobID);
    int32 interval = identity.Pet->DB->HungryDelay;
    if (BattleConfig.PetHungryDelayRate != 100)
        interval = identity.Pet->DB->HungryDelay * BattleConfig.PetHungryDelayRate / 100;
    if (pet.Hungry <= ROWOW_PET_HUNGRY_NONE)
        interval = PET_STARVING_HUNGRY_DELAY_IN_MS;
    identity.Pet->HungryTimer = uint32(std::max(interval, 1));
    identity.Pet->LastThinkTime = GetTick();
    auto mobStatusIter = MobStatusesByMobID.find(pet.MobID);
    if (mobStatusIter != MobStatusesByMobID.end())
    {
        identity.Pet->Range2 = mobStatusIter->second.Range2;
        identity.Pet->Range3 = mobStatusIter->second.Range3;
    }
}

bool ROWOWMod::PetCreateEgg(Player* sd, ROWOWPetDB const& db)
{
    // intif_create_pet and pet_get_egg: a new row, and the egg naming it in the bags.  False when the bags have no room
    auto eggTemplateIDIter = ItemTemplateIDsByItemID.find(db.EggID);
    ROWOWDataTable const* mobTable = GetDataTable(ROWOW_DATA_TABLE_MOB);
    int64 mobRowIndex = GetMobRowIndex(db.MobID);
    if (eggTemplateIDIter == ItemTemplateIDsByItemID.end() || mobTable == nullptr || mobRowIndex < 0)
        return false;
    Item* egg = nullptr;
    if (PCAddItem(sd, eggTemplateIDIter->second, 1, true, &egg) == false)
    {
        SendPetMessage(sd, "You cannot get the item.");
        return false;
    }
    uint32 petID = ++LastPetID;
    egg->SetEnchantment(ROWOW_ITEM_PET_ENCHANTMENT_SLOT, ROWOW_ITEM_PET_ENCHANTMENT_START + petID, 0, 0);
    std::string name = ROWOWDataTable::UnescapeText(mobTable->GetCell(size_t(mobRowIndex), mobTable->GetColumnIndex("Name")));
    CharacterDatabase.EscapeString(name);
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    trans->Append("INSERT INTO `mod_rowow_pet` (`PetID`, `OwnerGUID`, `MobID`, `Name`, `Level`, `EggID`, `EquipID`, `Intimate`, `Hungry`, `IsRenamed`, `IsIncubate`) VALUES ({}, {}, {}, '{}', {}, {}, 0, {}, {}, 0, 1)",
        petID, sd->GetGUID().GetCounter(), db.MobID, name, uint32(mobTable->GetInt(size_t(mobRowIndex), mobTable->GetColumnIndex("Level"), 1)), db.EggID, db.Intimate, ROWOW_PET_HUNGRY_STUFFED);
    sd->SaveInventoryAndGoldToDB(trans);
    CharacterDatabase.CommitTransaction(trans);
    return true;
}

void ROWOWMod::ClifSendEgg(Player* sd)
{
    // clif_sendegg: the eggs in the bags for the pet window to pick from, as RO's egg list by their items: "ROWOWPET\tE\t<item GUID>:<item entry>,...".  Run in the player's thread, from
    // the Pet Incubator's script
    std::string eggs;
    for (uint8 bag = 0; bag <= 4; bag++)
    {
        for (uint32 slot = 1; slot <= MAX_BAG_SIZE; slot++)
        {
            Item* item = GetItemByClientPosition(sd, bag, slot);
            ROWOWItem const* roItem = GetROItem(item);
            if (roItem == nullptr || roItem->Type != ROWOW_ITEM_TYPE_PETEGG)
                continue;
            if (GetEggPetID(item) == 0)
                continue;
            if (eggs.empty() == false)
                eggs += ",";
            eggs += fmt::format("{}:{}", item->GetGUID().GetCounter(), item->GetEntry());
        }
    }
    ROWOWIdentity* identity = GetIdentity(sd);
    identity->MenuSkillID = ROWOW_SKILL_SA_TAMINGMONSTER;
    identity->MenuSkillValue = -1;
    SendAddonMessage(sd, "ROWOWPET\tE\t" + eggs);
}

void ROWOWMod::PetSelectEgg(Player* sd, ObjectGuid::LowType itemGUID)
{
    // clif_parse_SelectEgg, pet_select_egg, pet_recv_petdata and pet_birth_process, as a chat command (the world thread, with the maps idle): the egg is taken away and the pet
    // comes out.  The egg's going and the row's coming out are one transaction, so a crash can't leave both or neither
    ROWOWIdentity* identity = GetIdentity(sd);
    if (identity->MenuSkillID != ROWOW_SKILL_SA_TAMINGMONSTER || identity->MenuSkillValue != -1)
        return;
    identity->MenuSkillID = 0;
    identity->MenuSkillValue = 0;
    if (identity->Pet != nullptr || sd->GetTradeData() != nullptr) // The player have trade in progress.
        return;
    Item* egg = sd->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(itemGUID));
    ROWOWItem const* roItem = GetROItem(egg);
    uint32 petID = egg != nullptr ? GetEggPetID(egg) : 0;
    if (roItem == nullptr || roItem->Type != ROWOW_ITEM_TYPE_PETEGG || petID == 0)
        return;
    QueryResult result = CharacterDatabase.Query("SELECT `PetID`, `OwnerGUID`, `MobID`, `Name`, `Level`, `EggID`, `EquipID`, `Intimate`, `Hungry`, `IsRenamed`, `IsIncubate` FROM `mod_rowow_pet` WHERE `PetID` = {}", petID);
    ROWOWPet pet;
    if (result == nullptr || ReadPetRow(result->Fetch(), pet) == false || pet.IsIncubate == false)
    {
        LOG_ERROR("module.ROWOW", "ROWOWMod::PetSelectEgg: the egg of {} names pet {}, which has no row in its egg", sd->GetName(), petID);
        return;
    }
    pet.IsIncubate = false;
    pet.OwnerGUID = sd->GetGUID().GetCounter();
    uint32 count = 1;
    sd->DestroyItemCount(egg, count, true);
    PetDataInit(*identity, pet);
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    SavePet(pet, trans);
    sd->SaveInventoryAndGoldToDB(trans);
    CharacterDatabase.CommitTransaction(trans);
    PetSpawn(sd, *identity->Pet);
    if (identity->Pet->DB->BonusScript != nullptr)
        RefreshROForm(sd);
    ClifSendPetStatus(sd);
    SendAddonMessage(sd, "ROWOWPET\tO");
    ClifPetTalk(sd, *identity->Pet, PET_TALK_CONNECT);
}

bool ROWOWMod::PetReturnEgg(Player* sd)
{
    // pet_return_egg, with the egg made anew.  The bags need room for it, or it stays out
    ROWOWIdentity* identity = GetIdentity(sd);
    ROWOWPetData* pd = identity->Pet.get();
    if (pd == nullptr)
        return false;
    auto eggTemplateIDIter = ItemTemplateIDsByItemID.find(pd->Pet.EggID);
    if (eggTemplateIDIter == ItemTemplateIDsByItemID.end())
        return false;
    PetLootItemDrop(sd, *pd, sd);
    Item* egg = nullptr;
    if (PCAddItem(sd, eggTemplateIDIter->second, 1, true, &egg) == false)
    {
        SendPetMessage(sd, "You cannot get the item.");
        return false;
    }
    egg->SetEnchantment(ROWOW_ITEM_PET_ENCHANTMENT_SLOT, ROWOW_ITEM_PET_ENCHANTMENT_START + pd->Pet.PetID, 0, 0);
    pd->Pet.IsIncubate = true;
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    SavePet(pd->Pet, trans);
    sd->SaveInventoryAndGoldToDB(trans);
    CharacterDatabase.CommitTransaction(trans);
    PetUnspawn(sd, *pd);
    bool hasBonus = pd->DB->BonusScript != nullptr;
    identity->Pet.reset();
    if (hasBonus == true)
        RefreshROForm(sd);
    ClifSendPetStatus(sd);
    return true;
}

void ROWOWMod::PetCatchProcessStart(Player* sd, uint32 itemID, uint8 flag)
{
    // pet_catch_process_start: the item's spell picked the target already, which is RO's capture cursor (clif_catch_process).  Beastly Hypnosis has no spell of the
    // taming item's, so it takes the player's selection
    ROWOWIdentity* identity = GetIdentity(sd);
    auto mapIDsIter = MapIDsByMapFlag.find("nopetcapture");
    if (mapIDsIter != MapIDsByMapFlag.end() && mapIDsIter->second.find(sd->GetMapId()) != mapIDsIter->second.end())
    {
        SendPetMessage(sd, "You can't catch any pet on this map.");
        return;
    }
    ObjectGuid targetGUID = identity->PetCatchTargetGUID.IsEmpty() == false ? identity->PetCatchTargetGUID : sd->GetTarget();
    identity->PetCatchTargetGUID.Clear();
    PetCatchProcessEnd(sd, ObjectAccessor::GetCreature(*sd, targetGUID), itemID, flag);
}

int32 ROWOWMod::PetCatchRate(Player const* sd, ROWOWPetDB const& pet, Creature const* md) const
{
    // pet_catch_process_end's pet_catch_rate, in 10000.  The legacy formula is pet_legacy_formula's
    int32 hpPercent = md->GetMaxHealth() != 0 ? int32(uint64(md->GetHealth()) * 100 / md->GetMaxHealth()) : 100;
    int32 rate = 0;
    if (BattleConfig.PetLegacyFormula != 0)
    {
        ROWOWIdentity const* identity = sd->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
        rate = (pet.Capture + (int32(identity->BaseLevel) - int32(md->GetLevel())) * 30 + identity->Status.Stats[ROWOW_STAT_LUK] * 20) * (200 - hpPercent) / 100;
    }
    else
        rate = pet.Capture + ((100 - hpPercent) * pet.Capture) / 100;
    rate = std::max(rate, 1);
    if (BattleConfig.PetCatchRate != 100)
        rate = (rate * BattleConfig.PetCatchRate) / 100;
    return rate;
}

void ROWOWMod::PetCatchProcessEnd(Player* sd, Creature* md, uint32 itemID, uint8 flag)
{
    // pet_catch_process_end, and clif_pet_roulette's result for the pet window: "ROWOWPET\tR\t<1 caught, 0 not>"
    auto rouletteFail = [this, sd]()
    {
        SendAddonMessage(sd, "ROWOWPET\tR\t0");
        SendPetMessage(sd, "The capture failed.");
    };
    if (md == nullptr || md->IsAlive() == false || md->IsInWorld() == false || md->GetMap() != sd->GetMap() || IsROCombatant(md) == false)
    {
        rouletteFail();
        return;
    }
    auto petIter = PetsByMobID.find(GetROMobID(md));
    if (petIter == PetsByMobID.end())
    {
        rouletteFail();
        return;
    }
    ROWOWPetDB const& pet = petIter->second;
    ROWOWStatus const* mdStatus = GetROStatus(md);
    switch (flag)
    {
        case ROWOW_PET_CATCH_NORMAL:
            // If the taming item used is different from the taming item according to the pet database
            if (itemID != pet.ItemID)
            {
                rouletteFail();
                return;
            }
            break;
        case ROWOW_PET_CATCH_UNIVERSAL_NO_BOSS:
            // used for universal lures (except bosses for now).
            if (mdStatus != nullptr && (mdStatus->Mode & ROWOW_MODE_STATUS_IMMUNE) != 0)
            {
                rouletteFail();
                return;
            }
            break;
        default: // No checks, catch anything.
            break;
    }
    if (BattleConfig.PetDistanceCheck != 0 && DistanceBL(sd, md) > BattleConfig.PetDistanceCheck)
    {
        rouletteFail();
        return;
    }
    if (sd->GetFreeInventorySpace() == 0)
    {
        rouletteFail();
        SendPetMessage(sd, "You cannot get the item.");
        return;
    }
    if (BattleConfig.PetHideCheck != 0)
    {
        ROWOWStatusChange const* tsc = GetStatusChange(static_cast<Unit const*>(md));
        if (tsc != nullptr && (tsc->HasSCE(ROWOW_SC_HIDING) == true || tsc->HasSCE(ROWOW_SC_CLOAKING) == true))
        {
            rouletteFail();
            return;
        }
    }
    if (uint32(rand32() % 10000) >= uint32(PetCatchRate(sd, pet, md)))
    {
        rouletteFail();
        return;
    }
    StatusKill(md); // mob_dead's type 3: no exp and no drops
    SendAddonMessage(sd, "ROWOWPET\tR\t1");
    SendPetMessage(sd, "The capture succeeded.");
    PetCreateEgg(sd, pet);
}

void ROWOWMod::PetMenu(Player* sd, int32 menuNum)
{
    // pet_menu.  You lost the pet already, with no intimacy
    ROWOWPetData* pd = GetPet(sd);
    if (pd == nullptr)
    {
        ClifSendPetStatus(sd);
        return;
    }
    if (pd->Pet.Intimate <= ROWOW_PET_INTIMATE_NONE)
        return;
    switch (menuNum)
    {
        case 0: ClifSendPetStatus(sd); break;
        case 1: PetFood(sd, *pd); break;
        case 2: PetPerformance(sd, *pd); break;
        case 3: PetReturnEgg(sd); break;
        case 4: PetUnequipItem(sd, *pd); break;
        default: break;
    }
}

void ROWOWMod::PetFood(Player* sd, ROWOWPetData& pd)
{
    // pet_food: it takes its food from the bags.  More when it's hungry than when it's only peckish, and overfeeding it costs its master's standing
    Item* food = PCSearchInventory(sd, pd.DB->FoodID);
    if (food == nullptr)
    {
        ItemTemplate const* foodTemplate = nullptr;
        auto foodTemplateIDIter = ItemTemplateIDsByItemID.find(pd.DB->FoodID);
        if (foodTemplateIDIter != ItemTemplateIDsByItemID.end())
            foodTemplate = sObjectMgr->GetItemTemplate(foodTemplateIDIter->second);
        SendPetMessage(sd, fmt::format("Feeding failed: you have no {}.", foodTemplate != nullptr ? foodTemplate->Name1 : "food"));
        return;
    }
    uint32 count = 1;
    sd->DestroyItemCount(food, count, true);

    if (pd.Pet.Hungry > ROWOW_PET_HUNGRY_SATISFIED)
    {
        PetSetIntimate(sd, pd, pd.Pet.Intimate + pd.DB->RFull);
        if (pd.Pet.Intimate <= ROWOW_PET_INTIMATE_NONE)
            PetSetSpeed(sd, pd, PetGetWalkSpeed(sd));
    }
    else
    {
        int32 k = pd.DB->RHungry;
        if (BattleConfig.PetFriendlyRate != 100)
            k = (pd.DB->RHungry * BattleConfig.PetFriendlyRate) / 100;
        if (pd.Pet.Hungry > ROWOW_PET_HUNGRY_NEUTRAL)
        {
            k /= 2;
            k = std::max(k, 1);
        }
        PetSetIntimate(sd, pd, pd.Pet.Intimate + k);
    }
    pd.Pet.Hungry = std::min(pd.Pet.Hungry + pd.DB->HungerIncrease, int32(ROWOW_PET_HUNGRY_STUFFED));
    ClifSendPetStatus(sd);
    ClifPetTalk(sd, pd, PET_TALK_FEEDING);
}

void ROWOWMod::PetPerformance(Player* sd, ROWOWPetData& pd)
{
    // pet_performance and CHANGESTATEPET_PERFORMANCE: data = 1~3 normal, 4 special.  It stops for it and drops what it carries
    Creature* md = GetPetCreature(sd, pd);
    if (md == nullptr)
        return;
    UnitStopWalking(md);
    GetMobData(md)->CanMoveTime = GetTick() + PET_PERFORMANCE_STOP_IN_MS;
    int32 num = 1;
    if (pd.Pet.Intimate > ROWOW_PET_INTIMATE_LOYAL)
        num = pd.DB->IsSpecialPerformance == true ? 4 : 3;
    else if (pd.Pet.Intimate > ROWOW_PET_INTIMATE_CORDIAL)
        num = 2;
    int32 performance = urand(1, num);
    md->HandleEmoteCommand(EMOTE_ONESHOT_ATTACK1H);
    ClifPetTalk(sd, pd, performance == 4 ? PET_TALK_SPECIAL : uint8(PET_TALK_PERFORMANCE_1 + performance - 1));
    PetLootItemDrop(sd, pd, nullptr);
}

bool ROWOWMod::PetEquipItem(Player* sd, Item* item)
{
    // pet_equipitem: using the accessory (clif_parse_EquipItem) puts it on the pet it's for
    ROWOWPetData* pd = GetPet(sd);
    ROWOWItem const* roItem = GetROItem(item);
    if (pd == nullptr || roItem == nullptr || pd->DB->AcceID == 0 || roItem->ItemID != pd->DB->AcceID || pd->Pet.EquipID != 0)
    {
        SendPetMessage(sd, "You can't put this on your pet.");
        return false;
    }
    uint32 count = 1;
    pd->Pet.EquipID = roItem->ItemID;
    sd->DestroyItemCount(item, count, true);
    PetRefreshLook(sd, *pd);
    ClifSendPetStatus(sd);
    return true;
}

void ROWOWMod::PetUnequipItem(Player* sd, ROWOWPetData& pd)
{
    // pet_unequipitem: back to the bags.  On official servers the item is destroyed if you don't have enough space (pet_unequip_destroy)
    if (pd.Pet.EquipID == 0)
        return;
    auto itemTemplateIDIter = ItemTemplateIDsByItemID.find(pd.Pet.EquipID);
    if (itemTemplateIDIter == ItemTemplateIDsByItemID.end())
        return;
    if (PCAddItem(sd, itemTemplateIDIter->second, 1, true) == false)
    {
        SendPetMessage(sd, "You cannot get the item.");
        if (BattleConfig.PetUnequipDestroy == 0)
            return; // Don't unequip (and don't destroy) the item if failed to add it to the inventory
    }
    pd.Pet.EquipID = 0;
    PetRefreshLook(sd, pd);
    ClifSendPetStatus(sd);
}

void ROWOWMod::PetChangeName(Player* sd, std::string const& name)
{
    // pet_change_name and pet_change_name_ack: one rename, unless pet_rename allows more, and no control characters
    ROWOWPetData* pd = GetPet(sd);
    if (pd == nullptr || (pd->Pet.IsRenamed == true && BattleConfig.PetRename == 0))
        return;
    bool isBadName = name.empty() == true || name.size() > ROWOW_PET_NAME_LENGTH;
    for (unsigned char character : name)
        if ((character & 0xe0) == 0 || character == 0x7f)
            isBadName = true;
    if (isBadName == true)
    {
        SendPetMessage(sd, "You cannot use this name for your pet.");
        ClifSendPetStatus(sd);
        return;
    }
    pd->Pet.Name = name;
    pd->Pet.IsRenamed = true;
    PetRefreshName(sd, *pd);
    ClifSendPetStatus(sd);
}

void ROWOWMod::PetOwnerDead(Player* sd)
{
    // pc_dead: its master's death costs it some fondness, but on a nopenalty map
    ROWOWPetData* pd = GetPet(sd);
    if (pd == nullptr)
        return;
    if (NoPenaltyMapIDs.find(sd->GetMapId()) == NoPenaltyMapIDs.end())
    {
        PetSetIntimate(sd, *pd, pd->Pet.Intimate + pd->DB->Die);
        ClifSendPetStatus(sd);
    }
    pd->LootTargetID.Clear();
    ClifPetTalk(sd, *pd, PET_TALK_DEAD);
}

void ROWOWMod::PetOwnerLevelUp(Player* sd)
{
    if (ROWOWPetData* pd = GetPet(sd))
        ClifPetTalk(sd, *pd, PET_TALK_LEVELUP);
}

void ROWOWMod::PetLoot(Player* sd, int32 max)
{
    // petloot: it gathers items up to max, dropping any it held.  Let'em loot at least 1 item.  (No pet_db row has it in pre-renewal: it would be in a SupportScript, which
    // pet_status_support keeps off; a GM or a script can still give it)
    ROWOWPetData* pd = GetPet(sd);
    if (pd == nullptr)
        return;
    PetLootItemDrop(sd, *pd, sd);
    pd->LootMax = std::clamp(max, 1, MAX_PETLOOT_SIZE);
}

void ROWOWMod::PetLootItemDrop(Player* sd, ROWOWPetData& pd, Player* receiver)
{
    // pet_lootitem_drop: into the receiver's bags, or what doesn't fit (and all of it with no receiver) at its feet, free for all.  10 seconds before it picks up again
    if (pd.LootItems.empty() == true)
        return;
    Creature* md = GetPetCreature(sd, pd);
    int32 x = 0, y = 0;
    GetUnitCell(md != nullptr ? static_cast<Unit*>(md) : static_cast<Unit*>(sd), x, y);
    for (ROWOWDropItem const& item : pd.LootItems)
    {
        if (receiver != nullptr && PCAddItem(receiver, item.ItemTemplateID, item.Amount, item.IsIdentified) == true)
            continue;
        MapAddFloorItem(sd->GetMap(), item, x, y, ObjectGuid::Empty, ObjectGuid::Empty, ObjectGuid::Empty, ROWOW_FLOORITEM_NO_DROP_CHECK, -1, false);
    }
    pd.LootItems.clear();
    pd.CanActTime = GetTick() + 10000; // prevent picked up during 10*1000ms
}

ROWOWFloorItem const* ROWOWMod::PetLootSearch(Player const* sd, Creature* md, ROWOWPetData const& pd, int32 range) const
{
    // pet_ai_sub_hard_lootsearch: the closest floor item in range that's no one's or its master's, within half its range of sight
    ROWOWMapFloorItems const* floorItems = md->GetMap()->CustomData.Get<ROWOWMapFloorItems>("ROWOWMapFloorItems");
    if (floorItems == nullptr)
        return nullptr;
    int32 x = 0, y = 0;
    GetUnitCell(md, x, y);
    ROWOWFloorItem const* target = nullptr;
    int32 targetDistance = 0;
    for (auto const& floorItem : floorItems->ItemsByGUID)
    {
        ROWOWFloorItem const& fitem = floorItem.second;
        if (CheckDistance(fitem.X - x, fitem.Y - y, range) == false)
            continue;
        if (fitem.FirstGetCharID.IsEmpty() == false && fitem.FirstGetCharID != sd->GetGUID())
            continue;
        int32 distance = std::max(std::abs(fitem.X - x), std::abs(fitem.Y - y));
        if ((target == nullptr || targetDistance > distance) && UnitCanReachCell(md, fitem.X, fitem.Y, pd.Range2) == true) // New target closer than previous one.
        {
            target = &fitem;
            targetDistance = distance;
        }
    }
    return target;
}

void ROWOWMod::PetRandomWalk(Creature* md, ROWOWMobData* mob, ROWOWPetData& pd, uint64 tick)
{
    // pet_randomwalk: somewhere up to 12 cells away, sooner again the less it walked
    if (pd.NextWalkTime >= tick || UnitCanMove(md, mob, tick) == false)
        return;
    ROWOWMapGrid const* mapGrid = GetMapGrid(md->GetMapId());
    if (mapGrid == nullptr)
        return;
    constexpr int32 RETRY_COUNT = 20;
    int32 x0 = 0, y0 = 0;
    GetUnitCell(md, x0, y0);
    int32 d = std::max(12 - pd.MoveFailCount, 5);
    for (int32 i = 0; i < RETRY_COUNT; i++)
    {
        int32 x = x0 + irand(-d, d);
        int32 y = y0 + irand(-d, d);
        if (mapGrid->IsCellWalkable(x, y) == true && UnitWalkToXY(md, x, y) == true)
        {
            pd.MoveFailCount = 0;
            break;
        }
        if (i + 1 >= RETRY_COUNT)
        {
            pd.MoveFailCount++;
            if (pd.MoveFailCount > 1000)
            {
                pd.MoveFailCount = 0;
                mob->CanMoveTime = tick + 60000;
                return;
            }
        }
    }
    pd.NextWalkTime = tick + MIN_RANDOMWALKTIME + mob->WalkPathTime + urand(0, 999);
}

void ROWOWMod::PetAISubHard(Player* sd, ROWOWPetData& pd, uint64 tick)
{
    // pet_ai_sub_hard, from its master's update.  It walks RO's paths on the GAT with the monsters' unit functions
    Creature* md = GetPetCreature(sd, pd);
    if (md == nullptr)
    {
        if (pd.Pet.Intimate > ROWOW_PET_INTIMATE_NONE)
            PetSpawn(sd, pd); // Its creature went (a grid that unloaded takes it)
        return;
    }
    ROWOWMobData* mob = GetMobData(md);
    if (mob->IsWalking == true && md->GetMotionMaster()->HasMovementGeneratorType(ESCORT_MOTION_TYPE) == false)
        mob->IsWalking = false;
    if (mob->IsWalking == true && UnitWalkPathPos(md, mob, tick) <= 2)
        return; // No thinking when you just started to walk.
    if (pd.Pet.Intimate <= ROWOW_PET_INTIMATE_NONE)
    {
        PetRandomWalk(md, mob, pd, tick); // Pet should just... well, random walk.
        return;
    }

    // unit_check_start_teleport_timer: too far from the master for 3 seconds, it's put beside it
    if (CheckDistanceBL(sd, md, ROWOW_AREA_SIZE) == false)
    {
        if (pd.TeleportTime == 0)
            pd.TeleportTime = tick + PET_TELEPORT_DELAY_IN_MS;
        else if (tick >= pd.TeleportTime)
        {
            UnitStopWalking(md);
            md->NearTeleportTo(sd->GetPositionX(), sd->GetPositionY(), sd->GetPositionZ(), sd->GetOrientation());
            pd.TeleportTime = 0;
            return;
        }
    }
    else
        pd.TeleportTime = 0;

    if (CheckDistanceBL(sd, md, pd.Range3) == false)
    {
        // Master too far, chase.
        pd.LootTargetID.Clear();
        if (mob->IsWalking == true && mob->TargetToID == sd->GetGUID())
            return; // Already walking to him
        if (mob->CanMoveTime > tick)
            return; // Can't move yet.
        ROWOWStatus const* masterStatus = GetROStatus(sd);
        PetSetSpeed(sd, pd, std::max(masterStatus != nullptr ? masterStatus->Speed / 2 : 75, 1));
        if (UnitWalkToBL(md, mob, sd, 3, false) == false)
            PetRandomWalk(md, mob, pd, tick);
        return;
    }
    mob->TargetToID.Clear();

    // Return speed to normal.
    int32 walkSpeed = PetGetWalkSpeed(sd);
    if (pd.Speed != walkSpeed)
    {
        if (mob->IsWalking == true)
            return; // Wait until the pet finishes walking back to master.
        PetSetSpeed(sd, pd, walkSpeed);
    }

    // The floor item it's after, or another
    ROWOWFloorItem const* fitem = nullptr;
    if (pd.LootTargetID.IsEmpty() == false)
    {
        fitem = MapFindFloorItem(md->GetMap(), pd.LootTargetID);
        if (fitem == nullptr)
            pd.LootTargetID.Clear();
    }
    if (fitem == nullptr && pd.LootMax > 0 && int32(pd.LootItems.size()) < pd.LootMax && tick > pd.CanActTime)
    {
        fitem = PetLootSearch(sd, md, pd, pd.Range2 / 2); // Use half the pet's range of sight.
        if (fitem != nullptr)
            pd.LootTargetID = fitem->GUID;
    }

    if (fitem == nullptr)
    {
        // Just walk around.
        if (CheckDistanceBL(sd, md, 3) == true)
            return; // Already next to master.
        int32 x = 0, y = 0;
        GetUnitCell(sd, x, y);
        if (mob->IsWalking == true && CheckDistance(pd.ToX - x, pd.ToY - y, 3) == true)
            return; // Already walking to him
        int32 x0 = 0, y0 = 0;
        GetUnitCell(md, x0, y0);
        if (UnitCalcPos(md->GetMap(), x0, y0, x, y, UnitGetDir(sd), pd.ToX, pd.ToY) == false || UnitWalkToXY(md, pd.ToX, pd.ToY) == false)
            PetRandomWalk(md, mob, pd, tick);
        return;
    }

    // Item Targeted, attempt loot
    int32 x0 = 0, y0 = 0;
    GetUnitCell(md, x0, y0);
    if (CheckDistance(fitem->X - x0, fitem->Y - y0, 1) == false)
    {
        if (mob->IsWalking == false && UnitWalkToCell(md, mob, fitem->X, fitem->Y) == false) // Out of range
            pd.LootTargetID.Clear(); // Unreachable target.
        return;
    }
    if (int32(pd.LootItems.size()) < pd.LootMax)
    {
        pd.LootItems.push_back(fitem->Item);
        ObjectGuid itemGUID = fitem->GUID;
        MapClearFloorItem(md->GetMap(), itemGUID);
    }
    pd.LootTargetID.Clear(); // Target is unlocked regardless of whether it was picked or not.
}

void ROWOWMod::UpdatePet(Player* sd, uint32 diff)
{
    // pet_hungry's timer and pet_ai_hard, in its master's update (the master's thread): only in the RO world, so a pet doesn't hunger outside it, as it doesn't while its master is offline
    ROWOWPetData* pd = GetPet(sd);
    if (pd == nullptr)
        return;
    PetHungry(sd, *pd, diff);
    pd = GetPet(sd);
    if (pd == nullptr)
        return;
    pd->ThinkTimer += diff;
    if (pd->ThinkTimer < MIN_PETTHINKTIME)
        return;
    pd->ThinkTimer = 0;
    PetAISubHard(sd, *pd, GetTick());
}

// Hides a pet from WoW's combat and answers a click on it with the pet window: the master's update walks it, so it needs no AI of its own
class ROWOW_PetCreatureScript : public CreatureScript
{
public:
    ROWOW_PetCreatureScript() : CreatureScript("ROWOW_PetCreatureScript") {}

    CreatureAI* GetAI(Creature* creature) const override
    {
        return new PassiveAI(creature);
    }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        CloseGossipMenuFor(player);
        ROWOWPetOwner const* owner = creature->CustomData.Get<ROWOWPetOwner>("ROWOWPetOwner");
        if (ROWOW->IsEnabled == true && owner != nullptr && owner->OwnerGUID == player->GetGUID())
        {
            ROWOW->ClifSendPetStatus(player);
            WorldPacket data;
            ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_ADDON, nullptr, nullptr, "ROWOWPET\tO");
            player->SendDirectMessage(&data);
        }
        return true;
    }
};

// What a taming item casts: as the use item spell, with a target.  RO's capture cursor picks one, which is WoW's item targeting here.  The pet check runs in pet_catch_process_end, and a
// failure uses the item up as RO does (pc_useitem has spent it by then)
class ROWOW_TameSpellScript : public SpellScript
{
    PrepareSpellScript(ROWOW_TameSpellScript);

    SpellCastResult CheckUse()
    {
        Player* player = GetCaster() != nullptr ? GetCaster()->ToPlayer() : nullptr;
        Item* item = GetCastItem();
        if (player == nullptr || item == nullptr || ROWOW->IsEnabled == false)
            return SPELL_FAILED_DONT_REPORT;
        if (ROWOW->IsInROForm(player) == false)
        {
            ChatHandler(player->GetSession()).SendSysMessage("Ragnarok Online items can only be used in the Ragnarok Online world.");
            return SPELL_FAILED_DONT_REPORT;
        }
        if (ROWOW->PCIsUseItem(player, item, true) == false)
            return SPELL_FAILED_DONT_REPORT;
        return SPELL_CAST_OK;
    }

    void HandleUse(SpellEffIndex /*effIndex*/)
    {
        Player* player = GetCaster() != nullptr ? GetCaster()->ToPlayer() : nullptr;
        Item* item = GetCastItem();
        if (player == nullptr || item == nullptr)
            return;
        ObjectGuid itemGUID = item->GetGUID();
        ObjectGuid targetGUID = GetHitUnit() != nullptr ? GetHitUnit()->GetGUID() : ObjectGuid::Empty;
        player->m_Events.AddEventAtOffset([player, itemGUID, targetGUID]()
        {
            ROWOW->GetIdentity(player)->PetCatchTargetGUID = targetGUID;
            ROWOW->PCUseItem(player, itemGUID);
        }, 0ms);
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(ROWOW_TameSpellScript::CheckUse);
        OnEffectHitTarget += SpellEffectFn(ROWOW_TameSpellScript::HandleUse, EFFECT_0, SPELL_EFFECT_SCRIPT_EFFECT);
    }
};

void AddROWOWPetScripts()
{
    new ROWOW_PetCreatureScript();
    RegisterSpellScript(ROWOW_TameSpellScript);
}
