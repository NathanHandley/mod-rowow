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

#include "Chat.h"
#include "CommandScript.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "Map.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include "ROWOW.h"

using namespace std;
using namespace Acore::ChatCommands;

// The RO identity's hooks: the RO world is form-only (arriving on an RO map turns the character into its identity, and leaving turns
// it back), and in the form the character's own spells, items, experience and money stay out of play.  All of it runs in the
// player's own thread

namespace
{
    void SendNotInROFormMessage(Player* player)
    {
        ChatHandler(player->GetSession()).SendSysMessage("You can't do that as your Ragnarok Online self.");
    }
}

class ROWOW_IdentityPlayerScript : public PlayerScript
{
public:
    ROWOW_IdentityPlayerScript() : PlayerScript("ROWOW_IdentityPlayerScript") {}

    void OnPlayerMapChanged(Player* player) override
    {
        // Arriving in the RO world (by the world travel spell, any other teleport, or logging in there) turns the character into its
        // identity, and arriving anywhere else turns it back
        if (ROWOW->IsEnabled == false || player == nullptr)
            return;
        ROWOWIdentity* identity = ROWOW->GetIdentity(player);
        if (ROWOW->IsROMap(player->GetMapId()) == true && identity->IsInForm == true)
        {
            // From one RO map to another: the client was just sent the character's own spells and action bars again
            ROWOW->SendROFormClientSpellsAndActions(player, *identity);
            ROWOW->SendROFormStateToClient(player);
        }
        else if (ROWOW->IsROMap(player->GetMapId()) == true)
            ROWOW->EnterROForm(player);
        else if (identity->IsInForm == true)
            ROWOW->LeaveROForm(player);
        else if (identity->WasInFormAtLoad == true)
        {
            // Saved in the form but not in the RO world (the server stopped before the character left it): only the look is left over,
            // since the character row never stops having the character's own values
            ROWOW->RemoveROFormAuras(player);
        }
        identity->WasInFormAtLoad = false;
    }

    void OnPlayerLogin(Player* player) override
    {
        if (ROWOW->IsEnabled == false)
            return;

        // Every character has the world travel spell (new ones start with it)
        if (player->HasSpell(ROWOW_SPELL_WORLD_TRAVEL) == false && sSpellMgr->GetSpellInfo(ROWOW_SPELL_WORLD_TRAVEL) != nullptr)
            player->learnSpell(ROWOW_SPELL_WORLD_TRAVEL);

        // The pet loads after the character is placed on the map, so it's put away again here
        if (ROWOW->IsInROForm(player) == true)
            player->UnsummonPetTemporaryIfAny();
    }

    bool OnPlayerBeforeTeleport(Player* player, uint32 mapid, float /*x*/, float /*y*/, float /*z*/, float /*orientation*/, uint32 /*options*/, Unit* /*target*/) override
    {
        // Remember where the character came into the RO world from, and where the identity left it, for the world travel spell
        if (ROWOW->IsEnabled == false || player == nullptr || player->IsInWorld() == false)
            return true;
        bool isOnROMap = ROWOW->IsROMap(player->GetMapId());
        if (isOnROMap == ROWOW->IsROMap(mapid))
            return true;
        ROWOWIdentity* identity = ROWOW->GetIdentity(player);
        ROWOWPosition& position = isOnROMap == true ? identity->LastPosition : identity->ReturnPosition;
        position.IsSet = true;
        position.MapID = player->GetMapId();
        position.X = player->GetPositionX();
        position.Y = player->GetPositionY();
        position.Z = player->GetPositionZ();
        position.O = player->GetOrientation();
        return true;
    }

    void OnPlayerSaveCharacterValues(Player* player, CharacterDatabaseTransaction trans, PlayerSaveValues& values) override
    {
        // The character row keeps the character's own values, and the identity's go to its row in the same transaction
        ROWOWIdentity* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
        if (identity == nullptr)
            return;
        if (identity->IsInForm == true)
        {
            values.Level = identity->WoWLevel;
            values.XP = identity->WoWXP;
            values.Money = identity->WoWMoney;
            values.Health = identity->WoWHealth;
            for (uint8 powerIndex = 0; powerIndex < MAX_POWERS; powerIndex++)
                values.Powers[powerIndex] = identity->WoWPowers[powerIndex];
        }
        ROWOW->SaveIdentity(player, trans);
    }

    bool OnPlayerCanLoseDurabilityOnDeath(Player* player) override
    {
        // The form doesn't wear the character's items
        return ROWOW->IsInROForm(player) == false;
    }

    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* /*victim*/, uint8 /*xpSource*/) override
    {
        // The form earns RO experience, never the character's
        if (ROWOW->IsInROForm(player) == true)
            amount = 0;
    }

    void OnPlayerAfterUpdateMaxHealth(Player* player, float& value) override
    {
        ROWOWIdentity* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
        if (identity != nullptr && identity->IsInForm == true)
            value = float(ROWOW->GetIdentityMaxHP(*identity));
    }

    void OnPlayerAfterUpdateMaxPower(Player* player, Powers& power, float& value) override
    {
        // SP is the form's mana
        ROWOWIdentity* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
        if (identity != nullptr && identity->IsInForm == true && power == POWER_MANA)
            value = float(ROWOW->GetIdentityMaxSP(*identity));
    }

    bool OnPlayerCanEquipItem(Player* player, uint8 /*slot*/, uint16& /*dest*/, Item* pItem, bool /*swap*/, bool not_loading) override
    {
        // The character's equipment slots aren't the form's (only bags can still go on)
        if (not_loading == false || pItem == nullptr || ROWOW->IsInROForm(player) == false)
            return true;
        if (pItem->GetTemplate()->InventoryType == INVTYPE_BAG)
            return true;
        SendNotInROFormMessage(player);
        return false;
    }

    bool OnPlayerCanCastItemUseSpell(Player* player, Item* item, SpellCastTargets const& /*targets*/, uint8 /*cast_count*/, uint32 /*glyphIndex*/) override
    {
        // Only RO items work in the form
        if (item == nullptr || ROWOW->IsInROForm(player) == false || ROWOW->IsROItemTemplate(item->GetEntry()) == true)
            return true;
        SendNotInROFormMessage(player);
        return false;
    }
};

class ROWOW_IdentityServerScript : public ServerScript
{
public:
    ROWOW_IdentityServerScript() : ServerScript("ROWOW_IdentityServerScript", { SERVERHOOK_CAN_PACKET_RECEIVE }) {}

    bool CanPacketReceive(WorldSession* session, WorldPacket const& packet) override
    {
        // Runs in the session's update, so the player's own thread
        if (ROWOW->IsEnabled == false || session == nullptr)
            return true;
        Player* player = session->GetPlayer();
        if (player == nullptr || ROWOW->IsInROForm(player) == false)
            return true;

        switch (packet.GetOpcode())
        {
            case CMSG_SET_ACTION_BUTTON:
            {
                // The form's action bars are its own, and the character's stay as they were
                WorldPacket packetCopy(packet);
                uint8 button = 0;
                uint32 packedAction = 0;
                packetCopy >> button >> packedAction;
                ROWOW->SetIdentityActionButton(player, button, packedAction);
                return false;
            }
            case CMSG_REPOP_REQUEST:
            {
                // An RO death has no spirit or corpse: releasing is RO's "return to save point"
                if (player->IsAlive() == true || player->HasPlayerFlag(PLAYER_FLAGS_GHOST) == true)
                    return false;
                ROWOW->RespawnAtSavePoint(player);
                return false;
            }
            case CMSG_SELL_ITEM:
            {
                // RO shops only buy RO items
                WorldPacket packetCopy(packet);
                ObjectGuid vendorGUID;
                ObjectGuid itemGUID;
                packetCopy >> vendorGUID >> itemGUID;
                Item* item = player->GetItemByGuid(itemGUID);
                if (item == nullptr || ROWOW->IsROItemTemplate(item->GetEntry()) == true)
                    return true;
                player->SendSellError(SELL_ERR_CANT_SELL_ITEM, nullptr, itemGUID, 0);
                return false;
            }
            // Money that would cross between zeny and the character's money, or pay for the character's things with zeny
            case CMSG_SEND_MAIL:
            case CMSG_MAIL_TAKE_MONEY:
            case CMSG_MAIL_TAKE_ITEM:
            case CMSG_AUCTION_SELL_ITEM:
            case CMSG_AUCTION_PLACE_BID:
            case CMSG_GUILD_BANK_DEPOSIT_MONEY:
            case CMSG_GUILD_BANK_WITHDRAW_MONEY:
            case CMSG_GUILD_BANK_BUY_TAB:
            case CMSG_BUY_BANK_SLOT:
            case CMSG_REPAIR_ITEM:
            case CMSG_TRAINER_BUY_SPELL:
            case CMSG_ACTIVATETAXI:
            case CMSG_ACTIVATETAXIEXPRESS:
            case CMSG_BUY_STABLE_SLOT:
            case MSG_TALENT_WIPE_CONFIRM:
            case CMSG_PETITION_BUY:
            case CMSG_ALTER_APPEARANCE:
            {
                SendNotInROFormMessage(player);
                return false;
            }
            default:
                return true;
        }
    }
};

class ROWOW_IdentityAllSpellScript : public AllSpellScript
{
public:
    ROWOW_IdentityAllSpellScript() : AllSpellScript("ROWOW_IdentityAllSpellScript", { ALLSPELLHOOK_ON_SPELL_CHECK_CAST }) {}

    void OnSpellCheckCast(Spell* spell, bool /*strict*/, SpellCastResult& res) override
    {
        // The character's spells are hidden from the form, and refused if cast anyway (a macro, an old action button).  Item and
        // triggered casts go through their own checks
        if (res != SPELL_CAST_OK || spell == nullptr || spell->IsTriggered() == true || spell->m_CastItem != nullptr)
            return;
        WorldObject* caster = spell->GetCaster();
        if (caster == nullptr || caster->IsPlayer() == false || ROWOW->IsInROForm(caster->ToPlayer()) == false)
            return;
        if (ROWOW->IsSpellKeptInROForm(spell->GetSpellInfo()) == true)
            return;
        res = SPELL_FAILED_NOT_KNOWN;
    }
};

// The world travel spell: 8 seconds to the RO world (where the identity last left it, or the training ground for a new one), or back
// to where the character came from.  The form follows the arrival (ROWOW_IdentityPlayerScript::OnPlayerMapChanged)
class ROWOW_WorldTravelSpellScript : public SpellScript
{
    PrepareSpellScript(ROWOW_WorldTravelSpellScript);

    SpellCastResult CheckTravel()
    {
        Player* player = GetCaster() != nullptr ? GetCaster()->ToPlayer() : nullptr;
        if (player == nullptr || ROWOW->IsEnabled == false)
            return SPELL_FAILED_DONT_REPORT;
        Map* map = player->GetMap();
        if (map->IsDungeon() == true || map->IsBattlegroundOrArena() == true)
            return SPELL_FAILED_NOT_HERE;
        ROWOWPosition destination;
        if (ROWOW->GetWorldTravelDestination(player, destination) == false)
            return SPELL_FAILED_NOT_HERE;
        return SPELL_CAST_OK;
    }

    void HandleTravel(SpellEffIndex /*effIndex*/)
    {
        Player* player = GetCaster() != nullptr ? GetCaster()->ToPlayer() : nullptr;
        if (player == nullptr)
            return;
        ROWOWPosition destination;
        if (ROWOW->GetWorldTravelDestination(player, destination) == false)
            return;
        player->TeleportTo(destination.MapID, destination.X, destination.Y, destination.Z, destination.O);
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(ROWOW_WorldTravelSpellScript::CheckTravel);
        OnEffectHit += SpellEffectFn(ROWOW_WorldTravelSpellScript::HandleTravel, EFFECT_0, SPELL_EFFECT_SCRIPT_EFFECT);
    }
};

// GM commands for testing the RO identity, on the selected player (or yourself)
class ROWOW_CommandScript : public CommandScript
{
public:
    ROWOW_CommandScript() : CommandScript("ROWOW_CommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable identityCommandTable =
        {
            { "reset", HandleROIdentityResetCommand, SEC_GAMEMASTER, Console::No },
        };

        static ChatCommandTable roCommandTable =
        {
            { "sync",     HandleROSyncCommand,  SEC_PLAYER,     Console::No },
            { "info",     HandleROInfoCommand,  SEC_GAMEMASTER, Console::No },
            { "form",     HandleROFormCommand,  SEC_GAMEMASTER, Console::No },
            { "job",      HandleROJobCommand,   SEC_GAMEMASTER, Console::No },
            { "level",    HandleROLevelCommand, SEC_GAMEMASTER, Console::No },
            { "zeny",     HandleROZenyCommand,  SEC_GAMEMASTER, Console::No },
            { "identity", identityCommandTable },
        };

        static ChatCommandTable commandTable =
        {
            { "ro", roCommandTable },
        };
        return commandTable;
    }

    static bool HandleROSyncCommand(ChatHandler* handler)
    {
        // The RO_Identity addon asks for the form state once it's loaded (after a login or a reload of the interface)
        Player* player = handler->GetSession() != nullptr ? handler->GetSession()->GetPlayer() : nullptr;
        if (player == nullptr)
            return false;
        ROWOW->SendROFormStateToClient(player);
        return true;
    }

    static bool HandleROInfoCommand(ChatHandler* handler)
    {
        Player* player = handler->getSelectedPlayerOrSelf();
        if (player == nullptr)
            return false;
        ROWOWIdentity* identity = ROWOW->GetIdentity(player);
        if (identity->Exists == false)
        {
            handler->PSendSysMessage("{} has no Ragnarok Online identity yet.", player->GetName());
            return true;
        }
        ROWOWJob const* job = ROWOW->GetJob(identity->JobID);
        handler->PSendSysMessage("{}: {} ({}) base {} ({} exp), job {} ({} exp), {} status points, {} skill points, STR {} AGI {} VIT {} INT {} DEX {} LUK {}, {} zeny, {}",
            player->GetName(), job != nullptr ? job->Name : "?", identity->JobID, identity->BaseLevel, identity->BaseExp, identity->JobLevel, identity->JobExp,
            identity->StatusPoints, identity->SkillPoints, identity->Stats[0], identity->Stats[1], identity->Stats[2], identity->Stats[3], identity->Stats[4],
            identity->Stats[5], identity->IsInForm == true ? player->GetMoney() : identity->Zeny, identity->IsInForm == true ? "in the form" : "not in the form");
        return true;
    }

    static bool HandleROFormCommand(ChatHandler* handler, std::string mode)
    {
        // For testing only: the RO world turns the form on and off by itself
        Player* player = handler->getSelectedPlayerOrSelf();
        if (player == nullptr)
            return false;
        if (mode == "on")
            ROWOW->EnterROForm(player);
        else if (mode == "off")
            ROWOW->LeaveROForm(player);
        else
            return false;
        return true;
    }

    static bool HandleROIdentityResetCommand(ChatHandler* handler)
    {
        // Deletes the identity, so the next entry into the RO world makes a new one (at once, when in the RO world now)
        Player* player = handler->getSelectedPlayerOrSelf();
        if (player == nullptr)
            return false;
        bool wasInForm = ROWOW->IsInROForm(player);
        ROWOW->LeaveROForm(player);
        uint32 guid = player->GetGUID().GetCounter();
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        trans->Append("DELETE FROM `mod_rowow_identity` WHERE `guid` = {}", guid);
        trans->Append("DELETE FROM `mod_rowow_identity_action` WHERE `guid` = {}", guid);
        trans->Append("DELETE FROM `mod_rowow_identity_equipment` WHERE `guid` = {}", guid);
        CharacterDatabase.DirectCommitTransaction(trans); // Before the new identity's save, which is queued
        player->CustomData.Erase("ROWOWIdentity");
        if (wasInForm == true)
            ROWOW->EnterROForm(player);
        handler->PSendSysMessage("Reset {}'s Ragnarok Online identity.", player->GetName());
        return true;
    }

    static bool HandleROJobCommand(ChatHandler* handler, uint16 jobID)
    {
        Player* player = handler->getSelectedPlayerOrSelf();
        if (player == nullptr)
            return false;
        ROWOWJob const* job = ROWOW->GetJob(jobID);
        ROWOWIdentity* identity = ROWOW->GetIdentity(player);
        if (job == nullptr || identity->Exists == false)
        {
            handler->SendSysMessage("No such job, or no identity yet.");
            return false;
        }
        identity->JobID = jobID;
        identity->JobLevel = std::min(identity->JobLevel, job->MaxJobLevel);
        ROWOW->RefreshROForm(player);
        handler->PSendSysMessage("{} is now a {}.", player->GetName(), job->Name);
        return true;
    }

    static bool HandleROLevelCommand(ChatHandler* handler, uint32 baseLevel, Optional<uint32> jobLevel)
    {
        Player* player = handler->getSelectedPlayerOrSelf();
        if (player == nullptr)
            return false;
        ROWOWIdentity* identity = ROWOW->GetIdentity(player);
        ROWOWJob const* job = ROWOW->GetJob(identity->JobID);
        if (job == nullptr || identity->Exists == false)
        {
            handler->SendSysMessage("No identity yet.");
            return false;
        }
        identity->BaseLevel = std::max<uint32>(1, std::min(baseLevel, job->MaxBaseLevel));
        identity->BaseExp = 0;
        if (jobLevel.has_value() == true)
        {
            identity->JobLevel = std::max<uint32>(1, std::min(*jobLevel, job->MaxJobLevel));
            identity->JobExp = 0;
        }
        ROWOW->RefreshROForm(player);
        handler->PSendSysMessage("{} is now base level {} and job level {}.", player->GetName(), identity->BaseLevel, identity->JobLevel);
        return true;
    }

    static bool HandleROZenyCommand(ChatHandler* handler, uint32 zeny)
    {
        Player* player = handler->getSelectedPlayerOrSelf();
        if (player == nullptr)
            return false;
        ROWOWIdentity* identity = ROWOW->GetIdentity(player);
        if (identity->Exists == false)
        {
            handler->SendSysMessage("No identity yet.");
            return false;
        }
        if (identity->IsInForm == true)
            player->SetMoney(zeny);
        else
            identity->Zeny = zeny;
        handler->PSendSysMessage("{} now has {} zeny.", player->GetName(), zeny);
        return true;
    }
};

void AddROWOWIdentityScripts()
{
    new ROWOW_IdentityPlayerScript();
    new ROWOW_IdentityServerScript();
    new ROWOW_IdentityAllSpellScript();
    RegisterSpellScript(ROWOW_WorldTravelSpellScript);
    new ROWOW_CommandScript();
}
