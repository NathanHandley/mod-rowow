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
#include "Creature.h"
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

    void OnPlayerSendInitialPacketsBeforeAddToMap(Player* player, WorldPacket& /*data*/) override
    {
        // Arriving on a map (by the world travel spell, any other teleport, or logging in), just after the client was sent the
        // character's spells and action bars and before it's sent the character itself: arriving in the RO world turns the character
        // into its identity, and arriving anywhere else turns it back.  The rest follows in OnPlayerMapChanged, once it's in the world
        if (ROWOW->IsEnabled == false || player == nullptr)
            return;
        ROWOWIdentity* identity = ROWOW->GetIdentity(player);
        bool isROMap = ROWOW->IsROMap(player->GetMapId());
        if (isROMap == true && identity->IsInForm == true)
            ROWOW->SendROFormClientSpellsAndActions(player, *identity); // From one RO map to another: the form's again
        else if (isROMap == true)
            ROWOW->EnterROForm(player);
        else if (identity->IsInForm == true)
            ROWOW->LeaveROForm(player);
    }

    void OnPlayerMapChanged(Player* player) override
    {
        if (ROWOW->IsEnabled == false || player == nullptr)
            return;
        ROWOW->CompleteROFormChange(player);
    }

    void OnPlayerUpdate(Player* player, uint32 p_time) override
    {
        ROWOW->UpdateNaturalRegen(player, p_time);
    }

    bool OnPlayerCanRegenerate(Player* player) override
    {
        // The form has RO's natural regen instead (UpdateNaturalRegen)
        return ROWOW->IsInROForm(player) == false;
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

    bool OnPlayerCanLoseDurability(Player* player) override
    {
        // The form doesn't wear the character's items
        return ROWOW->IsInROForm(player) == false;
    }

    bool OnPlayerCanUpdateSkill(Player* player, uint32 /*skillId*/) override
    {
        // The form's fighting and crafting raise none of the character's skills (weapon and defense skills included)
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
            value = float(identity->Status.MaxHP);
    }

    void OnPlayerAfterUpdateMaxPower(Player* player, Powers& power, float& value) override
    {
        // SP is the form's mana
        ROWOWIdentity* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
        if (identity != nullptr && identity->IsInForm == true && power == POWER_MANA)
            value = float(identity->Status.MaxSP);
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

// RO experience: an RO monster remembers who hurt it how much, and shares its exp out to them when it dies.  A death in the form costs
// exp.  These run in the map's thread, which the monster and the players near it share
class ROWOW_IdentityUnitScript : public UnitScript
{
public:
    ROWOW_IdentityUnitScript() : UnitScript("ROWOW_IdentityUnitScript", true, { UNITHOOK_ON_DAMAGE, UNITHOOK_ON_UNIT_DEATH, UNITHOOK_ON_UNIT_ENTER_EVADE_MODE }) {}

    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
    {
        if (ROWOW->IsEnabled == false || victim == nullptr || victim->IsCreature() == false)
            return;
        ROWOW->MobLogDamage(victim->ToCreature(), attacker, damage);
    }

    void OnUnitDeath(Unit* unit, Unit* /*killer*/) override
    {
        if (ROWOW->IsEnabled == false || unit == nullptr)
            return;
        if (unit->IsCreature() == true)
            ROWOW->MobDeadExp(unit->ToCreature());
        else if (unit->IsPlayer() == true)
            ROWOW->PCDeadPenalty(unit->ToPlayer());
    }

    void OnUnitEnterEvadeMode(Unit* unit, uint8 /*evadeReason*/) override
    {
        // Back home at full health, so what it was dealt before no longer counts
        if (unit != nullptr && unit->IsCreature() == true)
            if (ROWOWDamageLog* damageLog = unit->CustomData.Get<ROWOWDamageLog>("ROWOWDamageLog"))
                damageLog->Entries.clear();
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
            { "sync",     HandleROSyncCommand,   SEC_PLAYER,     Console::No },
            { "statup",   HandleROStatUpCommand, SEC_PLAYER,     Console::No },
            { "info",     HandleROInfoCommand,   SEC_GAMEMASTER, Console::No },
            { "form",     HandleROFormCommand,   SEC_GAMEMASTER, Console::No },
            { "job",      HandleROJobCommand,    SEC_GAMEMASTER, Console::No },
            { "level",    HandleROLevelCommand,  SEC_GAMEMASTER, Console::No },
            { "exp",      HandleROExpCommand,    SEC_GAMEMASTER, Console::No },
            { "stats",    HandleROStatsCommand,  SEC_GAMEMASTER, Console::No },
            { "zeny",     HandleROZenyCommand,   SEC_GAMEMASTER, Console::No },
            { "equip",    HandleROEquipCommand,  SEC_GAMEMASTER, Console::No },
            { "battle",   HandleROBattleCommand, SEC_GAMEMASTER, Console::No },
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
        ROWOW->SendROStatusToClient(player);
        return true;
    }

    static bool HandleROStatUpCommand(ChatHandler* handler, std::string statName, Optional<uint32> amount)
    {
        // The Status window's + buttons: raises a stat of the player's own identity, paying its status points (rAthena's pc_statusup)
        static char const* const statNames[ROWOW_STAT_COUNT] = { "str", "agi", "vit", "int", "dex", "luk" };
        Player* player = handler->GetSession() != nullptr ? handler->GetSession()->GetPlayer() : nullptr;
        if (player == nullptr || ROWOW->IsInROForm(player) == false)
            return false;
        for (char& curChar : statName)
            curChar = char(tolower((unsigned char)curChar));
        for (uint8 statIndex = 0; statIndex < ROWOW_STAT_COUNT; statIndex++)
        {
            if (statName != statNames[statIndex])
                continue;
            if (ROWOW->PCStatusUp(player, statIndex, int32(std::min<uint32>(amount.value_or(1), ROWOW_STAT_MAX_RAISE))) == false)
                ROWOW->SendROStatusToClient(player); // Refused: the window shows the values again
            return true;
        }
        handler->SendSysMessage("Raise which stat: str, agi, vit, int, dex or luk?");
        return false;
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
        // The status points go with the base level (statpoint.yml's totals), and a skill point with each job level, as rAthena's level
        // commands give and take them
        uint32 newBaseLevel = std::max<uint32>(1, std::min(baseLevel, job->MaxBaseLevel));
        auto oldPointsIter = ROWOW->StatusPointsByBaseLevel.find(identity->BaseLevel);
        auto newPointsIter = ROWOW->StatusPointsByBaseLevel.find(newBaseLevel);
        int64 statusPointChange = int64(newPointsIter != ROWOW->StatusPointsByBaseLevel.end() ? newPointsIter->second : 0) -
            int64(oldPointsIter != ROWOW->StatusPointsByBaseLevel.end() ? oldPointsIter->second : 0);
        identity->StatusPoints = uint32(std::max<int64>(0, int64(identity->StatusPoints) + statusPointChange));
        identity->BaseLevel = newBaseLevel;
        identity->BaseExp = 0;
        if (jobLevel.has_value() == true)
        {
            uint32 newJobLevel = std::max<uint32>(1, std::min(*jobLevel, job->MaxJobLevel));
            identity->SkillPoints = uint32(std::max<int64>(0, int64(identity->SkillPoints) + int64(newJobLevel) - int64(identity->JobLevel)));
            identity->JobLevel = newJobLevel;
            identity->JobExp = 0;
        }
        ROWOW->RefreshROForm(player);
        handler->PSendSysMessage("{} is now base level {} and job level {}.", player->GetName(), identity->BaseLevel, identity->JobLevel);
        return true;
    }

    static bool HandleROExpCommand(ChatHandler* handler, uint32 baseExp, Optional<uint32> jobExp)
    {
        // Gives exp as a kill would (pc_gainexp), levelling up as it goes
        Player* player = handler->getSelectedPlayerOrSelf();
        if (player == nullptr || ROWOW->IsInROForm(player) == false)
        {
            handler->SendSysMessage("Only in the Ragnarok Online world.");
            return false;
        }
        ROWOW->PCGainExp(player, baseExp, jobExp.value_or(0));
        return true;
    }

    static bool HandleROStatsCommand(ChatHandler* handler, uint32 str, uint32 agi, uint32 vit, uint32 int_, uint32 dex, uint32 luk)
    {
        // Sets the six base stats directly, without paying status points (for comparing with the rAthena oracle's @oraclechar)
        Player* player = handler->getSelectedPlayerOrSelf();
        if (player == nullptr || ROWOW->GetIdentity(player)->Exists == false)
        {
            handler->SendSysMessage("No identity yet.");
            return false;
        }
        ROWOWIdentity* identity = ROWOW->GetIdentity(player);
        uint32 const stats[ROWOW_STAT_COUNT] = { str, agi, vit, int_, dex, luk };
        for (uint8 statIndex = 0; statIndex < ROWOW_STAT_COUNT; statIndex++)
            identity->Stats[statIndex] = std::max<uint32>(1, std::min<uint32>(stats[statIndex], uint32(ROWOW->PCMaxParameter(*identity))));
        if (identity->IsInForm == true)
            ROWOW->RefreshROForm(player);
        else
            ROWOW->CalculateIdentityStatus(player, *identity);
        ROWOWStatus const& status = identity->Status;
        handler->PSendSysMessage("{}: STR {} AGI {} VIT {} INT {} DEX {} LUK {}, HP {} SP {}, ATK {} + {}, MATK {} ~ {}, DEF {} + {}, MDEF {} + {}, HIT {}, FLEE {} + {}, "
            "CRIT {}, ASPD {} (amotion {})", player->GetName(), status.Stats[0], status.Stats[1], status.Stats[2], status.Stats[3], status.Stats[4], status.Stats[5],
            status.MaxHP, status.MaxSP, status.BaseAttack + status.WeaponAttack + status.LeftWeaponAttack, status.WeaponRefineAttack + status.LeftWeaponRefineAttack,
            status.MagicAttackMin, status.MagicAttackMax, status.Def, status.Def2, status.MDef, status.MDef2 - status.Stats[ROWOW_STAT_VIT] / 2, status.Hit, status.Flee,
            status.Flee2 / 10, status.Critical / 10, (2000 - status.AttackMotion) / 10, status.AttackMotion);
        return true;
    }

    static bool HandleROEquipCommand(ChatHandler* handler, uint32 itemID, Optional<uint32> amount)
    {
        // For testing until the RO equipment window (R4): gives an RO item (item_db Id) and wears it in its RO equipment positions, taking
        // off whatever is worn there.  Ammo comes as a stack (100 unless told)
        Player* player = handler->getSelectedPlayerOrSelf();
        if (player == nullptr || ROWOW->GetIdentity(player)->Exists == false)
        {
            handler->SendSysMessage("No identity yet.");
            return false;
        }
        auto itemTemplateIDIter = ROWOW->ItemTemplateIDsByItemID.find(itemID);
        auto itemIter = itemTemplateIDIter != ROWOW->ItemTemplateIDsByItemID.end() ? ROWOW->ItemsByItemTemplateID.find(itemTemplateIDIter->second) : ROWOW->ItemsByItemTemplateID.end();
        if (itemIter == ROWOW->ItemsByItemTemplateID.end() || itemIter->second.Locations == 0)
        {
            handler->PSendSysMessage("There is no converted RO item {} that can be worn.", itemID);
            return false;
        }
        ROWOWItem const& roItem = itemIter->second;
        uint32 count = amount.value_or(roItem.IsAmmo == true ? 100 : 1);
        ItemPosCountVec itemDestination;
        if (count == 0 || player->CanStoreNewItem(NULL_BAG, NULL_SLOT, itemDestination, roItem.ItemTemplateID, count) != EQUIP_ERR_OK)
        {
            handler->SendSysMessage("No room in the bags for it.");
            return false;
        }
        Item* item = player->StoreNewItem(itemDestination, roItem.ItemTemplateID, true);
        if (item == nullptr)
            return false;
        player->SendNewItem(item, count, true, false);

        // Both accessory positions take the free one, or the left
        ROWOWIdentity* identity = ROWOW->GetIdentity(player);
        uint32 equipPosition = roItem.Locations;
        if (equipPosition == (ROWOW_EQUIP_POSITION_ACC_L | ROWOW_EQUIP_POSITION_ACC_R))
            equipPosition = ROWOW->GetIdentityEquippedItem(player, ROWOW_EQUIP_POSITION_ACC_L) == nullptr ? ROWOW_EQUIP_POSITION_ACC_L : ROWOW_EQUIP_POSITION_ACC_R;
        for (auto itemGUIDIter = identity->ItemGUIDsByEquipPosition.begin(); itemGUIDIter != identity->ItemGUIDsByEquipPosition.end();)
        {
            if ((itemGUIDIter->first & equipPosition) != 0)
                itemGUIDIter = identity->ItemGUIDsByEquipPosition.erase(itemGUIDIter);
            else
                ++itemGUIDIter;
        }
        identity->ItemGUIDsByEquipPosition[equipPosition] = item->GetGUID().GetCounter();
        identity->IsEquipmentChanged = true;
        if (identity->IsInForm == true)
            ROWOW->RefreshROForm(player);
        else
            ROWOW->CalculateIdentityStatus(player, *identity);
        handler->PSendSysMessage("{} now wears {} x{} (position {:#x}).", player->GetName(), item->GetTemplate()->Name1, count, equipPosition);
        return true;
    }

    static bool HandleROBattleCommand(ChatHandler* handler, Optional<uint32> swings)
    {
        // Works out RO normal attacks between the player and the selected RO monster, both ways, without dealing them, and prints the
        // totals the rAthena oracle's @oracledmg prints (for comparing with it)
        Player* player = handler->GetSession() != nullptr ? handler->GetSession()->GetPlayer() : nullptr;
        Creature* creature = handler->getSelectedCreature();
        if (player == nullptr || ROWOW->IsInROForm(player) == false || creature == nullptr || ROWOW->GetROStatus(creature) == nullptr)
        {
            handler->SendSysMessage("Select an RO monster, in the Ragnarok Online world.");
            return false;
        }
        uint32 swingCount = std::clamp<uint32>(swings.value_or(1000), 1, 1000000);
        ROWOWStatus const& mobStatus = *ROWOW->GetROStatus(creature);
        handler->PSendSysMessage("mob {} lv={} str={} agi={} vit={} int={} dex={} luk={} maxhp={} watk={} watk2={} def={} def2={} mdef={} mdef2={} hit={} flee={} flee2={} "
            "cri={} def_ele={} ele_lv={} race={} size={} class={} amotion={} adelay={} dmotion={}", ROWOW->GetROMobID(creature), mobStatus.Level, mobStatus.Stats[0],
            mobStatus.Stats[1], mobStatus.Stats[2], mobStatus.Stats[3], mobStatus.Stats[4], mobStatus.Stats[5], mobStatus.MaxHP, mobStatus.WeaponAttack,
            mobStatus.WeaponRefineAttack, mobStatus.Def, mobStatus.Def2, mobStatus.MDef, mobStatus.MDef2, mobStatus.Hit, mobStatus.Flee, mobStatus.Flee2,
            mobStatus.Critical, mobStatus.DefElement, mobStatus.ElementLevel, mobStatus.Race, mobStatus.Size, mobStatus.Class, mobStatus.AttackMotion,
            mobStatus.AttackDelay, mobStatus.DamageMotion);
        auto runSwings = [handler, swingCount](char const* label, Unit* src, Unit* target)
        {
            uint32 landed = 0, flee = 0, lucky = 0, otherMiss = 0, crit = 0, multi = 0;
            uint64 total = 0, normalTotal = 0, critTotal = 0;
            int64 normalMin = std::numeric_limits<int64>::max(), normalMax = 0, critMin = std::numeric_limits<int64>::max(), critMax = 0;
            for (uint32 swing = 0; swing < swingCount; swing++)
            {
                ROWOWDamage d = ROWOW->BattleCalcAttack(ROWOW_BF_WEAPON, src, target, 0, 0, 0);
                int64 damage = d.Damage + d.Damage2;
                if (d.Type == ROWOW_DMG_CRITICAL)
                    crit++;
                if (d.Type == ROWOW_DMG_MULTI_HIT)
                    multi++;
                if (damage > 0)
                {
                    landed++;
                    total += uint64(damage);
                    if (d.Type == ROWOW_DMG_CRITICAL)
                    {
                        critTotal += uint64(damage);
                        critMin = std::min(critMin, damage);
                        critMax = std::max(critMax, damage);
                    }
                    else
                    {
                        normalTotal += uint64(damage);
                        normalMin = std::min(normalMin, damage);
                        normalMax = std::max(normalMax, damage);
                    }
                }
                else if (d.DmgLv == ROWOW_ATK_FLEE)
                    flee++;
                else if (d.DmgLv == ROWOW_ATK_LUCKY)
                    lucky++;
                else
                    otherMiss++;
            }
            uint32 normalCount = landed - std::min(landed, crit);
            handler->PSendSysMessage("{} n={} landed={} flee={} lucky={} othermiss={} crit={} multi={} hit_pct={:.2f} crit_pct={:.2f} lucky_pct={:.2f} avg_landed={:.3f}",
                label, swingCount, landed, flee, lucky, otherMiss, crit, multi, 100.0 * landed / swingCount, 100.0 * crit / swingCount, 100.0 * lucky / swingCount,
                landed != 0 ? double(total) / landed : 0.0);
            handler->PSendSysMessage("{}_normal count={} min={} max={} avg={:.3f} crit_count={} crit_min={} crit_max={} crit_avg={:.3f}", label, normalCount,
                normalCount != 0 ? normalMin : 0, normalMax, normalCount != 0 ? double(normalTotal) / normalCount : 0.0, crit, crit != 0 ? critMin : 0, critMax,
                crit != 0 ? double(critTotal) / crit : 0.0);
        };
        runSwings("pc2mob", player, creature);
        runSwings("mob2pc", creature, player);
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
    new ROWOW_IdentityUnitScript();
    new ROWOW_IdentityAllSpellScript();
    RegisterSpellScript(ROWOW_WorldTravelSpellScript);
    new ROWOW_CommandScript();
}
