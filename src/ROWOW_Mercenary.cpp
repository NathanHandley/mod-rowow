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
//  Mercenaries are a port of rAthena's mercenary.cpp:
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
//  For more information, see LICENCE in the main folder

#include "Chat.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "Player.h"
#include "WorldPacket.h"

#include "ROWOW.h"
#include "ROWOW_Skill.h"

using namespace std;

// A mercenary (R11), mercenary.cpp under its names: rented from a scroll for its contract's time, on the master's identity (s_mercenary_data as ROWOWMercData) with its row
// (mod_rowow_mercenary) saved with the character.  The guilds' standing with a character (mercenary_owner) is the identity's.  Its contract is counted in the master's update, so
// only while the master is in the RO world, as rAthena's timer only runs with its master logged in.  Its creature and AI are ROWOW_Follower.cpp's

namespace
{
    enum MercenaryGuild : int32
    {
        ARCH_MERC_GUILD = 0,
        SPEAR_MERC_GUILD = 1,
        SWORD_MERC_GUILD = 2,
        NONE_MERC_GUILD = 3
    };

    void SendAddonMessage(Player* player, std::string const& message)
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_ADDON, nullptr, nullptr, message);
        player->SendDirectMessage(&data);
    }
}

ROWOWMercData* ROWOWMod::GetMercenary(Player const* player) const
{
    ROWOWIdentity const* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    return identity != nullptr ? identity->Mercenary.get() : nullptr;
}

uint16 ROWOWMod::MercenaryCheckSkill(Unit const* md, uint16 skillID) const
{
    // mercenary_checkskill
    ROWOWFollower const* fd = GetFollowerType(md) == ROWOW_BL_MER ? GetFollower(md) : nullptr;
    if (fd == nullptr)
        return 0;
    for (auto const& skill : static_cast<ROWOWMercData const*>(fd)->DB->Skills)
        if (skill.first == skillID)
            return skill.second;
    return 0;
}

int32 ROWOWMod::MercenaryGetGuild(ROWOWMercData const& md) const
{
    // mercenary_get_guild
    if (md.ClassID >= ROWOW_MERID_MER_ARCHER01 && md.ClassID <= ROWOW_MERID_MER_ARCHER10)
        return ARCH_MERC_GUILD;
    if (md.ClassID >= ROWOW_MERID_MER_LANCER01 && md.ClassID <= ROWOW_MERID_MER_LANCER10)
        return SPEAR_MERC_GUILD;
    if (md.ClassID >= ROWOW_MERID_MER_SWORDMAN01 && md.ClassID <= ROWOW_MERID_MER_SWORDMAN10)
        return SWORD_MERC_GUILD;
    return NONE_MERC_GUILD;
}

int32 ROWOWMod::MercenaryGetFaith(Player* sd) const
{
    // mercenary_get_faith
    ROWOWMercData const* md = GetMercenary(sd);
    ROWOWIdentity const* identity = sd->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    if (md == nullptr || identity == nullptr)
        return 0;
    switch (MercenaryGetGuild(*md))
    {
        case ARCH_MERC_GUILD: return identity->MercenaryOwner.ArchFaith;
        case SPEAR_MERC_GUILD: return identity->MercenaryOwner.SpearFaith;
        case SWORD_MERC_GUILD: return identity->MercenaryOwner.SwordFaith;
        default: return 0;
    }
}

void ROWOWMod::MercenarySetFaith(Player* sd, int32 value)
{
    // mercenary_set_faith
    ROWOWMercData const* md = GetMercenary(sd);
    if (md == nullptr)
        return;
    ROWOWMercenaryOwner& owner = GetIdentity(sd)->MercenaryOwner;
    int32* faith = nullptr;
    switch (MercenaryGetGuild(*md))
    {
        case ARCH_MERC_GUILD: faith = &owner.ArchFaith; break;
        case SPEAR_MERC_GUILD: faith = &owner.SpearFaith; break;
        case SWORD_MERC_GUILD: faith = &owner.SwordFaith; break;
        default: return;
    }
    *faith = std::clamp(*faith + value, 0, int32(SHRT_MAX));
}

int32 ROWOWMod::MercenaryGetCalls(Player* sd) const
{
    // mercenary_get_calls
    ROWOWMercData const* md = GetMercenary(sd);
    ROWOWIdentity const* identity = sd->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    if (md == nullptr || identity == nullptr)
        return 0;
    switch (MercenaryGetGuild(*md))
    {
        case ARCH_MERC_GUILD: return identity->MercenaryOwner.ArchCalls;
        case SPEAR_MERC_GUILD: return identity->MercenaryOwner.SpearCalls;
        case SWORD_MERC_GUILD: return identity->MercenaryOwner.SwordCalls;
        default: return 0;
    }
}

void ROWOWMod::MercenarySetCalls(Player* sd, int32 value)
{
    // mercenary_set_calls
    ROWOWMercData const* md = GetMercenary(sd);
    if (md == nullptr)
        return;
    ROWOWMercenaryOwner& owner = GetIdentity(sd)->MercenaryOwner;
    int32* calls = nullptr;
    switch (MercenaryGetGuild(*md))
    {
        case ARCH_MERC_GUILD: calls = &owner.ArchCalls; break;
        case SPEAR_MERC_GUILD: calls = &owner.SpearCalls; break;
        case SWORD_MERC_GUILD: calls = &owner.SwordCalls; break;
        default: return;
    }
    *calls = std::clamp<int64>(int64(*calls) + value, 0, INT_MAX);
}

void ROWOWMod::StatusCalcMercenary(Player* sd, ROWOWMercData& md, bool isFirst)
{
    // status_calc_mercenary_: mercenary_db's status, at its master's speed, and status_calc_misc
    ROWOWStatus& status = md.BaseStatus;
    if (isFirst == true)
    {
        status = md.DB->Status;
        status.Class = ROWOW_CLASS_NORMAL;
        status.Mode = ROWOW_MODE_CANMOVE | ROWOW_MODE_CANATTACK;
        if (ROWOWStatus const* masterStatus = GetROStatus(sd))
            status.Speed = masterStatus->Speed;
    }
    status.BaseAttack = status.Hit = status.Flee = status.Def2 = status.MDef2 = status.Critical = status.Flee2 = 0;
    StatusCalcMisc(status, md.DB->Level);
}

void ROWOWMod::MercSave(ROWOWMercData const& md, ObjectGuid::LowType charGUID, CharacterDatabaseTransaction trans) const
{
    // mercenary_save
    trans->Append("REPLACE INTO `mod_rowow_mercenary` (`CharacterGUID`, `Class`, `HP`, `SP`, `KillCount`, `LifeTime`) VALUES ({}, {}, {}, {}, {}, {})", charGUID, md.ClassID, md.HP, md.SP, md.KillCount, md.LifeTime);
}

bool ROWOWMod::MercenaryCreate(Player* sd, uint32 classID, uint32 lifeTime)
{
    // mercenary_create and mercenary_recv_data: a new contract, at full HP and SP, one more call of its guild
    auto dbIter = MercenariesByClass.find(classID);
    ROWOWIdentity* identity = GetIdentity(sd);
    if (dbIter == MercenariesByClass.end() || identity->Mercenary != nullptr)
        return false;
    identity->Mercenary = std::make_unique<ROWOWMercData>();
    ROWOWMercData& md = *identity->Mercenary;
    md.FollowerType = ROWOW_BL_MER;
    md.ClassID = classID;
    md.DB = &dbIter->second;
    md.HP = md.DB->Status.MaxHP;
    md.SP = md.DB->Status.MaxSP;
    md.LifeTime = int64(lifeTime);
    StatusCalcMercenary(sd, md, true);
    MercenarySetCalls(sd, 1);
    FollowerSpawn(sd, md);
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    MercSave(md, identity->IdentityID, trans);
    CharacterDatabase.CommitTransaction(trans);
    ClifMercenaryInfo(sd);
    return true;
}

void ROWOWMod::MercenaryDelete(Player* sd, int32 reply)
{
    // mercenary_delete: reply 0 its time was up (+1 loyalty), 1 killed (-1 loyalty), others no change (2 dismissed, 3 its master died).  Its row goes
    ROWOWIdentity* identity = GetIdentity(sd);
    ROWOWMercData* md = identity->Mercenary.get();
    if (md == nullptr)
        return;
    if (identity->IsFollowerHeld == true)
    {
        identity->MercenaryDeleteReply = reply; // Its update's end (FollowerDeletePending)
        return;
    }
    if (md->IsDevotion == true)
    {
        md->IsDevotion = false;
        StatusChangeEnd(sd, ROWOW_SC_DEVOTION);
    }
    switch (reply)
    {
        case 0:
            MercenarySetFaith(sd, 1); // +1 Loyalty on Contract ends.
            ChatHandler(sd->GetSession()).SendSysMessage("Mercenary soldier's duty hour is over.");
            break;
        case 1:
            MercenarySetFaith(sd, -1); // -1 Loyalty on Mercenary killed
            ChatHandler(sd->GetSession()).SendSysMessage("Your mercenary soldier has been killed.");
            break;
        default:
            break;
    }
    FollowerUnspawn(sd, *md);
    CharacterDatabase.Execute("DELETE FROM `mod_rowow_mercenary` WHERE `CharacterGUID` = {}", identity->IdentityID);
    identity->Mercenary.reset();
    ClifMercenaryInfo(sd);
}

void ROWOWMod::MercenaryKills(Player* sd)
{
    // mercenary_kills: each 50th kill gives a point of loyalty and a bonus for 5 minutes (mercenary_killbonus)
    ROWOWMercData* md = GetMercenary(sd);
    if (md == nullptr)
        return;
    if (md->KillCount <= uint32(INT_MAX - 1))
        md->KillCount++; // safe cap to INT_MAX
    if ((md->KillCount % 50) == 0)
    {
        MercenarySetFaith(sd, 1);
        static int32 const statuses[] = { ROWOW_SC_MERC_FLEEUP, ROWOW_SC_MERC_ATKUP, ROWOW_SC_MERC_HPUP, ROWOW_SC_MERC_SPUP, ROWOW_SC_MERC_HITUP };
        if (Creature* creature = GetFollowerCreature(sd, *md))
            SCStart(creature, creature, statuses[urand(0, 4)], 100, irand(1, 5), 300000); // Bonus lasts for 5 minutes
    }
    ClifMercenaryInfo(sd);
}

void ROWOWMod::MercenaryUpdate(Player* sd, ROWOWMercData& md, uint32 diff)
{
    // merc_contract_end: its timer
    md.LifeTime -= int64(diff);
    if (md.LifeTime <= 0)
        MercenaryDelete(sd, 0); // Mercenary soldier's duty hour is over.
}

void ROWOWMod::ClifMercenaryInfo(Player* sd)
{
    // clif_mercenary_info and clif_mercenary_skillblock for the RO_Identity addon's mercenary window: "ROWOWFOLLOWER\tM\t<fields>\t<skills>" or "ROWOWFOLLOWER\tM" when there's none.
    // A skill is <id>:<level>
    ROWOWMercData* md = GetMercenary(sd);
    if (md == nullptr)
    {
        SendAddonMessage(sd, "ROWOWFOLLOWER\tM");
        return;
    }
    Creature* creature = GetFollowerCreature(sd, *md);
    ROWOWStatus const* status = creature != nullptr ? GetROStatus(creature) : &md->BaseStatus;
    if (status == nullptr)
        status = &md->BaseStatus;
    std::string skills;
    for (auto const& skill : md->DB->Skills)
        skills += fmt::format("{}{}:{}", skills.empty() == true ? "" : ",", skill.first, skill.second);
    uint32 hp = creature != nullptr ? creature->GetHealth() : md->HP;
    uint32 sp = creature != nullptr ? creature->GetPower(POWER_MANA) : md->SP;
    SendAddonMessage(sd, fmt::format("ROWOWFOLLOWER\tM\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}", md->DB->Name, md->ClassID, md->DB->Level, hp, status->MaxHP, sp, status->MaxSP,
        status->WeaponAttack + status->BaseAttack, status->WeaponRefineAttack + status->BaseAttack, status->MagicAttackMax, status->Hit, status->Critical / 10, status->Def, status->MDef, status->Flee, status->AttackMotion,
        md->LifeTime / 1000, MercenaryGetFaith(sd), MercenaryGetCalls(sd), md->KillCount, skills));
}
