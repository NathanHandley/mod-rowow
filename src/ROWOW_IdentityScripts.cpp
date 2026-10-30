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
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "Tokenize.h"
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
        ROWOW->UpdateStatusChanges(player);
        if (ROWOW->IsEnabled == false || ROWOW->IsInROForm(player) == false)
            return;
        ROWOW->UpdateSkillUnitMove(player);
        ROWOW->UpdateInvincibleTimer(player);
        ROWOW->UpdateAutoBonuses(player);

        // A vendor stays sitting by its shop: standing up or walking away closes it
        ROWOWIdentity* identity = ROWOW->GetIdentity(player);
        if (identity->IsVending == true && (player->IsSitState() == false || player->isMoving() == true))
            ROWOW->VendingCloseVending(player);
    }

    void OnPlayerLogout(Player* player) override
    {
        // unit_remove_map's skill_clear_unitgroup and vending_closevending (no autotrade)
        if (ROWOW->IsEnabled == false || ROWOW->IsInROForm(player) == false)
            return;
        ROWOW->VendingCloseVending(player);
        ROWOW->SkillClearUnitGroup(player);
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

    bool OnPlayerBeforeTeleport(Player* player, uint32 mapid, float x, float y, float /*z*/, float /*orientation*/, uint32 /*options*/, Unit* /*target*/) override
    {
        // Remember where the character came into the RO world from, and where the identity left it, for the world travel spell
        if (ROWOW->IsEnabled == false || player == nullptr || player->IsInWorld() == false)
            return true;
        if (ROWOW->IsInROForm(player) == true)
        {
            // A warp off the map takes the player's ground skills and its shop with it (clear_skills_on_warp); a warp on the map ends in the
            // same invincibility as an arrival (clif_parse_LoadEndAck), from the spot it lands on
            ROWOW->VendingCloseVending(player);
            if (mapid != player->GetMapId())
                ROWOW->SkillClearUnitGroup(player);
            else
            {
                ROWOW->PCSetInvincibleTimer(player);
                ROWOWIdentity* identity = ROWOW->GetIdentity(player);
                identity->InvinciblePositionX = x;
                identity->InvinciblePositionY = y;
            }
        }
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

    void OnPlayerGetReputationPriceDiscount(Player const* player, Creature const* /*creature*/, float& discount) override
    {
        // Discount lowers an RO shop's prices (pc_modifybuyvalue), on the price list the client is sent and on the sale
        if (ROWOW->IsEnabled == true && ROWOW->IsInROForm(player) == true)
            discount = discount * float(ROWOW->PCModifyBuyValue(player, 10000)) / 10000.0f;
    }

    void OnPlayerMoneyChanged(Player* player, int32& amount) override
    {
        // Overcharge raises what an RO shop pays for an item (pc_modifysellvalue), the money of the sale the sell packet started
        ROWOWIdentity* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
        if (identity == nullptr || identity->IsSellingToShop == false)
            return;
        identity->IsSellingToShop = false;
        if (amount > 0)
            amount = ROWOW->PCModifySellValue(player, amount);
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
            case CMSG_DESTROYITEM:
            {
                // Worn RO equipment comes off before it can be thrown away
                WorldPacket packetCopy(packet);
                uint8 bag = 0;
                uint8 slot = 0;
                packetCopy >> bag >> slot;
                Item* item = player->GetItemByPos(bag, slot);
                if (item == nullptr || ROWOW->GetIdentityEquipPosition(*ROWOW->GetIdentity(player), item->GetGUID().GetCounter()) == 0)
                    return true;
                ChatHandler(session).SendSysMessage("Take it off before destroying it.");
                player->SendEquipError(EQUIP_ERR_NONE, item, nullptr);
                return false;
            }
            case CMSG_SELL_ITEM:
            {
                // RO shops only buy RO items, and Overcharge raises what they pay (pc_modifysellvalue, on the money the sale gives)
                WorldPacket packetCopy(packet);
                ObjectGuid vendorGUID;
                ObjectGuid itemGUID;
                packetCopy >> vendorGUID >> itemGUID;
                Item* item = player->GetItemByGuid(itemGUID);
                if (item == nullptr || ROWOW->IsROItemTemplate(item->GetEntry()) == true)
                {
                    ROWOW->GetIdentity(player)->IsSellingToShop = item != nullptr;
                    return true;
                }
                player->SendSellError(SELL_ERR_CANT_SELL_ITEM, nullptr, itemGUID, 0);
                return false;
            }
            case CMSG_INITIATE_TRADE:
            case CMSG_BANKER_ACTIVATE:
            case CMSG_GROUP_ACCEPT:
            case CMSG_GROUP_INVITE:
            case CMSG_TEXT_EMOTE:
            case CMSG_EMOTE:
            {
                // basic_skill_check: Basic Skill gates trading (1), emotions (2), the Kafra's storage (6), joining a party (5) and making one
                // (7: inviting when in no group)
                int32 basicSkill = ROWOW->PCCheckSkill(player, ROWOW_SKILL_NV_BASIC);
                int32 required = 0;
                char const* action = "";
                switch (packet.GetOpcode())
                {
                    case CMSG_INITIATE_TRADE: required = 1; action = "trade"; break;
                    case CMSG_TEXT_EMOTE:
                    case CMSG_EMOTE: required = 2; action = "use emotions"; break;
                    case CMSG_BANKER_ACTIVATE: required = 6; action = "use the storage"; break;
                    case CMSG_GROUP_ACCEPT: required = 5; action = "join a party"; break;
                    case CMSG_GROUP_INVITE: required = player->GetGroup() == nullptr ? 7 : 0; action = "make a party"; break;
                }
                if (ROWOW->BattleConfig.BasicSkillCheck == 0 || basicSkill >= required)
                    return true;
                ChatHandler(session).SendNotification("You need Basic Skill level {} to {}.", required, action);
                return false;
            }
            case CMSG_STANDSTATECHANGE:
            {
                // Sitting needs Basic Skill 3
                WorldPacket packetCopy(packet);
                uint32 standState = 0;
                packetCopy >> standState;
                if (standState != UNIT_STAND_STATE_SIT || ROWOW->BattleConfig.BasicSkillCheck == 0 || ROWOW->PCCheckSkill(player, ROWOW_SKILL_NV_BASIC) >= 3)
                    return true;
                ChatHandler(session).SendNotification("You need Basic Skill level 3 to sit.");
                return false;
            }
            case CMSG_BUYBACK_ITEM: // RO shops have no buyback
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
        if (ROWOW->IsEnabled == false || victim == nullptr)
            return;

        // status_damage: being hit ends the statuses that end when hit and stops an RO skill's cast that damage can stop
        // (unit_skillcastcancel), unless it's passive damage (a status's own, status_zap)
        ROWOWStatusChange const* sc = ROWOW->GetStatusChange(static_cast<Unit const*>(victim));
        if (ROWOW->IsROCombatant(victim) == true && (sc == nullptr || sc->IsPassiveDamage == false))
        {
            ROWOW->StatusDamaged(attacker, victim, damage);
            if (damage > 0)
                ROWOW->UnitSkillCastCancel(victim, 2);
        }
        if (victim->IsCreature() == true)
            ROWOW->MobLogDamage(victim->ToCreature(), attacker, damage);
    }

    void OnUnitDeath(Unit* unit, Unit* /*killer*/) override
    {
        if (ROWOW->IsEnabled == false || unit == nullptr)
            return;

        // status_damage's death: the statuses end, but those that stay through death (NoRemoveOnDead), and the ground units it stands in
        // act as if it left them (skill_unit_move with flag 4)
        ROWOW->StatusChangeClear(unit, 0);
        if (ROWOW->IsROCombatant(unit) == true)
        {
            std::vector<uint16> skillUnitCell;
            int32 x = 0, y = 0;
            ROWOW->GetUnitCell(unit, x, y);
            ROWOW->SkillUnitMove(unit, ROWOWMod::GetTick(), 4, x, y, skillUnitCell);
        }
        if (Player* player = unit->ToPlayer())
        {
            ROWOW->VendingCloseVending(player);
            if (ROWOW->IsInROForm(player) == true)
                ROWOW->PCDelDevotions(player); // pc_dead
        }
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
        if (ROWOW->IsSpellKeptInROForm(spell->GetSpellInfo()) == true || ROWOW->IsIdentitySkillSpell(caster->ToPlayer(), spell->GetSpellInfo()->Id) == true)
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
            { "wear",     HandleROWearCommand,   SEC_PLAYER,     Console::No },
            { "takeoff",  HandleROTakeOffCommand, SEC_PLAYER,    Console::No },
            { "identify", HandleROIdentifyCommand, SEC_PLAYER,   Console::No },
            { "cardlist", HandleROCardListCommand, SEC_PLAYER,   Console::No },
            { "compound", HandleROCompoundCommand, SEC_PLAYER,   Console::No },
            { "skillup",  HandleROSkillUpCommand, SEC_PLAYER,    Console::No },
            { "skills",   HandleROSkillsCommand, SEC_PLAYER,     Console::No },
            { "cart",     HandleROCartCommand,   SEC_PLAYER,     Console::No },
            { "cartadd",  HandleROCartAddCommand, SEC_PLAYER,    Console::No },
            { "cartget",  HandleROCartGetCommand, SEC_PLAYER,    Console::No },
            { "changecart", HandleROChangeCartCommand, SEC_PLAYER, Console::No },
            { "cartoff",  HandleROCartOffCommand, SEC_PLAYER,    Console::No },
            { "vend",     HandleROVendCommand,   SEC_PLAYER,     Console::No },
            { "vendclose", HandleROVendCloseCommand, SEC_PLAYER, Console::No },
            { "vendlist", HandleROVendListCommand, SEC_PLAYER,   Console::No },
            { "vendbuy",  HandleROVendBuyCommand, SEC_PLAYER,    Console::No },
            { "memo",     HandleROMemoCommand,   SEC_PLAYER,     Console::No },
            { "warp",     HandleROWarpCommand,   SEC_PLAYER,     Console::No },
            { "makearrow", HandleROMakeArrowCommand, SEC_PLAYER, Console::No },
            { "setcart",  HandleROSetCartCommand, SEC_GAMEMASTER, Console::No },
            { "setriding", HandleROSetRidingCommand, SEC_GAMEMASTER, Console::No },
            { "units",    HandleROUnitsCommand,  SEC_GAMEMASTER, Console::No },
            { "info",     HandleROInfoCommand,   SEC_GAMEMASTER, Console::No },
            { "form",     HandleROFormCommand,   SEC_GAMEMASTER, Console::No },
            { "job",      HandleROJobCommand,    SEC_GAMEMASTER, Console::No },
            { "level",    HandleROLevelCommand,  SEC_GAMEMASTER, Console::No },
            { "exp",      HandleROExpCommand,    SEC_GAMEMASTER, Console::No },
            { "stats",    HandleROStatsCommand,  SEC_GAMEMASTER, Console::No },
            { "zeny",     HandleROZenyCommand,   SEC_GAMEMASTER, Console::No },
            { "equip",    HandleROEquipCommand,  SEC_GAMEMASTER, Console::No },
            { "give",     HandleROGiveCommand,   SEC_GAMEMASTER, Console::No },
            { "refine",   HandleRORefineCommand, SEC_GAMEMASTER, Console::No },
            { "skill",    HandleROSkillCommand,  SEC_GAMEMASTER, Console::No },
            { "battle",   HandleROBattleCommand, SEC_GAMEMASTER, Console::No },
            { "sc",       HandleROStatusChangeCommand, SEC_GAMEMASTER, Console::No },
            { "scend",    HandleROStatusChangeEndCommand, SEC_GAMEMASTER, Console::No },
            { "scrate",   HandleROStatusChangeRateCommand, SEC_GAMEMASTER, Console::No },
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
        if (ROWOW->IsInROForm(player) == true)
        {
            ROWOW->SendEquipmentToClient(player, *ROWOW->GetIdentity(player), true);
            ROWOW->SendItemCardsToClient(player, *ROWOW->GetIdentity(player), true);
            ROWOW->SendSkillsToClient(player, *ROWOW->GetIdentity(player), true);
        }
        return true;
    }

    static bool HandleROSkillUpCommand(ChatHandler* handler, std::string skillName)
    {
        // The Skill window's + buttons: one more level of a skill (by skill_db Name or ID), for a skill point (rAthena's clif_parse_SkillUp)
        Player* player = handler->GetSession() != nullptr ? handler->GetSession()->GetPlayer() : nullptr;
        if (player == nullptr || ROWOW->IsInROForm(player) == false)
            return false;
        uint16 skillID = ROWOW->GetSkillIDByName(skillName);
        if (skillID == 0)
            skillID = uint16(strtoul(skillName.c_str(), nullptr, 10));
        if (ROWOW->PCSkillUp(player, skillID) == false)
            ROWOW->SendSkillsToClient(player, *ROWOW->GetIdentity(player), true); // Refused: the window shows the tree again
        return true;
    }

    static Player* GetFormPlayer(ChatHandler* handler)
    {
        Player* player = handler->GetSession() != nullptr ? handler->GetSession()->GetPlayer() : nullptr;
        return (player != nullptr && ROWOW->IsInROForm(player) == true) ? player : nullptr;
    }

    static bool HandleROCartCommand(ChatHandler* handler)
    {
        // The cart window asks for the cart when it opens (clif_cartlist)
        Player* player = GetFormPlayer(handler);
        if (player == nullptr)
            return false;
        ROWOW->SendCartToClient(player, *ROWOW->GetIdentity(player), true);
        return true;
    }

    static bool HandleROCartAddCommand(ChatHandler* handler, uint32 bag, uint32 slot, Optional<uint32> amount)
    {
        // An item dragged from the bags onto the cart window (clif_parse_PutItemToCart), all of its stack unless an amount is given
        Player* player = GetFormPlayer(handler);
        if (player == nullptr)
            return false;
        Item* item = ROWOWMod::GetItemByClientPosition(player, bag, slot);
        if (item != nullptr)
            ROWOW->PCPutItemToCart(player, item, int32(amount.value_or(item->GetCount())));
        return true;
    }

    static bool HandleROCartGetCommand(ChatHandler* handler, uint32 cartSlot, Optional<uint32> amount)
    {
        // A cart item taken back into the bags (clif_parse_GetItemFromCart)
        Player* player = GetFormPlayer(handler);
        if (player == nullptr || ROWOW->PCIsCartOn(player) == false || cartSlot >= ROWOW_MAX_CART)
            return false;
        ROWOW->PCGetItemFromCart(player, int32(cartSlot), int32(amount.value_or(ROWOW->GetIdentity(player)->CartItems[cartSlot].Count)));
        return true;
    }

    static bool HandleROChangeCartCommand(ChatHandler* handler, uint32 type)
    {
        // clif_parse_ChangeCart: Change Cart picks a look its base level allows (2 above 40, 3 above 65, 4 above 80, 5 above 90)
        Player* player = GetFormPlayer(handler);
        if (player == nullptr || ROWOW->PCIsCartOn(player) == false)
            return false;
        if (ROWOW->PCCheckSkill(player, ROWOW_SKILL_MC_CHANGECART) <= 0 && ROWOW->PCCheckSkill(player, ROWOW_SKILL_MC_CARTDECORATE) <= 0)
            return false;
        uint32 baseLevel = ROWOW->GetIdentity(player)->BaseLevel;
        if ((type == 5 && baseLevel > 90) || (type == 4 && baseLevel > 80) || (type == 3 && baseLevel > 65) || (type == 2 && baseLevel > 40) || type == 1)
            ROWOW->PCSetCart(player, int32(type));
        return true;
    }

    static bool HandleROCartOffCommand(ChatHandler* handler)
    {
        // clif_parse_RemoveOption: the Peco is let go of first, then the cart (its items stay in it)
        Player* player = GetFormPlayer(handler);
        if (player == nullptr)
            return false;
        if (ROWOW->PCIsRiding(player) == true)
            ROWOW->PCSetRiding(player, false);
        else
            ROWOW->PCSetCart(player, 0);
        return true;
    }

    static bool HandleROSetRidingCommand(ChatHandler* handler, Optional<uint32> flag)
    {
        // A Peco breeder's (the script command setriding, R8; rAthena's @mount toggles): mounts a Peco (it needs Riding) or, with 0,
        // dismounts it
        Player* player = handler->getSelectedPlayerOrSelf();
        if (player == nullptr || ROWOW->IsInROForm(player) == false)
            return false;
        ROWOW->PCSetRiding(player, flag.value_or(1) != 0);
        handler->PSendSysMessage("{} {}.", player->GetName(), ROWOW->PCIsRiding(player) == true ? "rides a Peco Peco" :
            (flag.value_or(1) != 0 ? "can't mount a Peco Peco (no Riding)" : "has released the Peco Peco"));
        return true;
    }

    static bool HandleROSetCartCommand(ChatHandler* handler, uint32 type, Optional<uint32> weight)
    {
        // A Kafra's cart rental (the script command setcart, R8): a cart of a look (1-5), or none (0).  A weight (in tenths) is what
        // the cart weighs until its items next change, as the oracle's @oraclecart sets it, for Cart Revolution's damage checks
        Player* player = handler->getSelectedPlayerOrSelf();
        if (player == nullptr || ROWOW->IsInROForm(player) == false)
            return false;
        if (ROWOW->PCSetCart(player, int32(type)) == false)
        {
            handler->SendSysMessage("No cart: it needs Pushcart (and a cart look of 1-5).");
            return true;
        }
        if (weight.has_value() == true)
        {
            ROWOWIdentity* identity = ROWOW->GetIdentity(player);
            if (identity != nullptr)
            {
                identity->CartWeight = int32(*weight);
                handler->PSendSysMessage("Cart weight {} of {}.", identity->CartWeight, identity->CartWeightMax);
            }
        }
        return true;
    }

    static bool HandleROVendCommand(ChatHandler* handler, std::string items, Tail title)
    {
        // The shop setup's "Open shop" (clif_parse_OpenVending): "<cart slot>:<amount>:<price>,..." and the shop's title
        Player* player = GetFormPlayer(handler);
        if (player == nullptr)
            return false;
        std::vector<ROWOWVendingItem> vendingItems;
        for (std::string_view itemText : Acore::Tokenize(items, ',', false))
        {
            std::vector<std::string_view> fields = Acore::Tokenize(itemText, ':', false);
            if (fields.size() != 3)
                return false;
            ROWOWVendingItem vendingItem;
            vendingItem.CartSlot = uint16(strtoul(std::string(fields[0]).c_str(), nullptr, 10));
            vendingItem.Amount = uint32(strtoul(std::string(fields[1]).c_str(), nullptr, 10));
            vendingItem.Value = uint32(strtoul(std::string(fields[2]).c_str(), nullptr, 10));
            vendingItems.push_back(vendingItem);
        }
        std::string shopTitle(title);
        if (shopTitle.empty() == true)
            shopTitle = "Shop";
        ROWOW->VendingOpenVending(player, shopTitle, vendingItems);
        return true;
    }

    static bool HandleROVendCloseCommand(ChatHandler* handler)
    {
        Player* player = GetFormPlayer(handler);
        if (player == nullptr)
            return false;
        ROWOW->VendingCloseVending(player);
        return true;
    }

    static bool HandleROVendListCommand(ChatHandler* handler, Optional<uint32> vendorGUID)
    {
        // A shop's items for the shop window (clif_parse_VendingListReq): the target's, or a vendor by GUID
        Player* player = GetFormPlayer(handler);
        if (player == nullptr)
            return false;
        Player* vendor = vendorGUID.has_value() == true ? ObjectAccessor::GetPlayer(player->GetMap(), ObjectGuid::Create<HighGuid::Player>(*vendorGUID)) :
            handler->getSelectedPlayer();
        ROWOW->VendingVendingListReq(player, vendor);
        return true;
    }

    static bool HandleROVendBuyCommand(ChatHandler* handler, uint32 vendorGUID, std::string purchases)
    {
        // The shop window's "Buy" (clif_parse_PurchaseReq2): "<index>:<amount>,..."
        Player* player = GetFormPlayer(handler);
        if (player == nullptr)
            return false;
        Player* vendor = ObjectAccessor::GetPlayer(player->GetMap(), ObjectGuid::Create<HighGuid::Player>(vendorGUID));
        std::vector<std::pair<uint16, uint32>> purchaseList;
        for (std::string_view purchaseText : Acore::Tokenize(purchases, ',', false))
        {
            std::vector<std::string_view> fields = Acore::Tokenize(purchaseText, ':', false);
            if (fields.size() != 2)
                return false;
            purchaseList.emplace_back(uint16(strtoul(std::string(fields[0]).c_str(), nullptr, 10)), uint32(strtoul(std::string(fields[1]).c_str(), nullptr, 10)));
        }
        ROWOW->VendingPurchaseReq(player, vendor, purchaseList);
        return true;
    }

    static bool HandleROMemoCommand(ChatHandler* handler, Optional<int32> pos)
    {
        // /memo (clif_parse_memo): the spot as a Warp Portal memo point, the newest unless a slot (0-2) is given
        Player* player = GetFormPlayer(handler);
        if (player == nullptr)
            return false;
        ROWOW->PCMemo(player, pos.value_or(-1));
        return true;
    }

    static bool HandleROWarpCommand(ChatHandler* handler, uint32 skillID, std::string mapName)
    {
        // The warp point menu's pick (clif_parse_UseSkillMap): "Random", "cancel" or a map ID it listed
        Player* player = GetFormPlayer(handler);
        if (player == nullptr)
            return false;
        ROWOW->SkillCastendMap(player, uint16(skillID), mapName);
        return true;
    }

    static bool HandleROMakeArrowCommand(ChatHandler* handler, uint32 itemTemplateID)
    {
        // The Arrow Crafting list's pick (clif_parse_SelectArrow), by the item's template
        Player* player = GetFormPlayer(handler);
        if (player == nullptr)
            return false;
        auto itemIter = ROWOW->ItemsByItemTemplateID.find(itemTemplateID);
        if (itemIter == ROWOW->ItemsByItemTemplateID.end() || ROWOW->SkillArrowCreate(player, itemIter->second.ItemID) == false)
            ROWOW->ClifSkillFail(player, ROWOW_SKILL_AC_MAKINGARROW, ROWOW_USESKILL_FAIL_LEVEL);
        return true;
    }

    static bool HandleROUnitsCommand(ChatHandler* handler)
    {
        // The ground skill unit groups on the player's map: skill, level, unit, live cells and time left
        Player* player = handler->GetSession() != nullptr ? handler->GetSession()->GetPlayer() : nullptr;
        ROWOWMapSkillUnits* mapUnits = player != nullptr ? ROWOW->GetMapSkillUnits(player->GetMap(), false) : nullptr;
        if (mapUnits == nullptr || mapUnits->GroupsByID.empty() == true)
        {
            handler->SendSysMessage("No ground skill units on this map.");
            return true;
        }
        uint64 tick = ROWOWMod::GetTick();
        for (auto const& groupByID : mapUnits->GroupsByID)
        {
            ROWOWSkillUnitGroup const& group = *groupByID.second;
            ROWOWSkill const* skill = ROWOW->GetSkill(group.SkillID);
            handler->PSendSysMessage("Group {}: {} {} unit {:#x}, {} of {} cells alive, {} ms left, val1 {} val2 {} val3 {}", group.GroupID, skill != nullptr ? skill->Name : "?",
                group.SkillLevel, group.UnitID, group.AliveCount, group.Units.size(), group.Limit - (int64(tick) - int64(group.Tick)), group.Val1, group.Val2, group.Val3);
        }
        return true;
    }

    static bool HandleROSkillsCommand(ChatHandler* handler)
    {
        // The Skill window asks for the tree when it opens
        Player* player = handler->GetSession() != nullptr ? handler->GetSession()->GetPlayer() : nullptr;
        if (player == nullptr || ROWOW->IsInROForm(player) == false)
            return false;
        ROWOW->SendSkillsToClient(player, *ROWOW->GetIdentity(player), true);
        return true;
    }

    static bool HandleROSkillCommand(ChatHandler* handler, std::string skillName, uint32 level)
    {
        // Sets a skill's level directly, without skill points (rAthena's @skillid / a quest NPC's skill), 0 taking it away
        Player* player = handler->getSelectedPlayerOrSelf();
        if (player == nullptr || ROWOW->GetIdentity(player)->Exists == false)
        {
            handler->SendSysMessage("No identity yet.");
            return false;
        }
        uint16 skillID = ROWOW->GetSkillIDByName(skillName);
        if (skillID == 0)
            skillID = uint16(strtoul(skillName.c_str(), nullptr, 10));
        ROWOWSkill const* skill = ROWOW->GetSkill(skillID);
        if (skill == nullptr)
        {
            handler->PSendSysMessage("There is no RO skill '{}'.", skillName);
            return false;
        }
        ROWOW->PCSkill(player, skillID, uint16(std::min<uint32>(level, skill->MaxLevel)));
        handler->PSendSysMessage("{} now has {} ({}) at level {}.", player->GetName(), skill->Description, skill->Name, std::min<uint32>(level, skill->MaxLevel));
        return true;
    }

    static bool HandleROWearCommand(ChatHandler* handler, uint32 bag, uint32 slot, Optional<uint32> equipPosition)
    {
        // The RO Equipment window and the bags: wears the RO item at a bag position (the client's bag 0-4 and slot from 1), in the
        // equipment positions asked for or wherever it goes (rAthena's clif_parse_EquipItem)
        Player* player = handler->GetSession() != nullptr ? handler->GetSession()->GetPlayer() : nullptr;
        if (player == nullptr || ROWOW->IsInROForm(player) == false)
            return false;
        Item* item = ROWOWMod::GetItemByClientPosition(player, bag, slot);
        ROWOWItem const* roItem = ROWOW->GetROItem(item);
        if (roItem == nullptr || roItem->Locations == 0)
            return true;
        if (ROWOWMod::IsItemIdentified(item) == false)
        {
            handler->SendSysMessage("Appraise it before wearing it.");
            return true;
        }
        ROWOW->PCEquipItem(player, item, int32(equipPosition.value_or(uint32(ROWOW->PCEquipPoint(player, *ROWOW->GetIdentity(player), *roItem)))));
        return true;
    }

    static bool HandleROTakeOffCommand(ChatHandler* handler, uint32 equipPosition)
    {
        // Takes off what's worn in an RO equipment position (rAthena's clif_parse_UnequipItem)
        Player* player = handler->GetSession() != nullptr ? handler->GetSession()->GetPlayer() : nullptr;
        if (player == nullptr || ROWOW->IsInROForm(player) == false)
            return false;
        ROWOW->PCUnequipItem(player, equipPosition, 1);
        return true;
    }

    static bool HandleROCardListCommand(ChatHandler* handler, uint32 bag, uint32 slot)
    {
        // A card used from the bags: the items it can go into (rAthena's clif_parse_UseCard)
        Player* player = handler->GetSession() != nullptr ? handler->GetSession()->GetPlayer() : nullptr;
        if (player == nullptr || ROWOW->IsInROForm(player) == false)
            return false;
        ROWOW->SendCardTargetsToClient(player, ROWOWMod::GetItemByClientPosition(player, bag, slot));
        return true;
    }

    static bool HandleROCompoundCommand(ChatHandler* handler, uint32 cardBag, uint32 cardSlot, uint32 bag, uint32 slot)
    {
        // The item picked from a card's list (rAthena's clif_parse_InsertCard)
        Player* player = handler->GetSession() != nullptr ? handler->GetSession()->GetPlayer() : nullptr;
        if (player == nullptr || ROWOW->IsInROForm(player) == false)
            return false;
        Item* card = ROWOWMod::GetItemByClientPosition(player, cardBag, cardSlot);
        Item* equipment = ROWOWMod::GetItemByClientPosition(player, bag, slot);
        if (card == nullptr || equipment == nullptr || ROWOW->PCInsertCard(player, card, equipment) == false)
            handler->SendSysMessage("The card can't go into that item.");
        return true;
    }

    static bool HandleROIdentifyCommand(ChatHandler* handler, uint32 bag, uint32 slot)
    {
        // The item picked from a Magnifier's list (rAthena's clif_parse_ItemIdentify)
        Player* player = handler->GetSession() != nullptr ? handler->GetSession()->GetPlayer() : nullptr;
        if (player == nullptr || ROWOW->IsInROForm(player) == false)
            return false;
        ROWOW->SkillIdentify(player, ROWOWMod::GetItemByClientPosition(player, bag, slot));
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
        trans->Append("DELETE FROM `mod_rowow_identity_skill` WHERE `guid` = {}", guid);
        trans->Append("DELETE FROM `mod_rowow_identity_variable` WHERE `guid` = {}", guid);
        trans->Append("DELETE FROM `mod_rowow_identity_status` WHERE `guid` = {}", guid);
        trans->Append("DELETE FROM `mod_rowow_identity_cart` WHERE `guid` = {}", guid);
        trans->Append("DELETE FROM `mod_rowow_identity_memo` WHERE `guid` = {}", guid);
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
        // A job change, as rAthena's @job makes it (job level 1, the skills and skill points kept)
        ROWOW->PCJobChange(player, jobID);
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

    static bool HandleRORefineCommand(ChatHandler* handler, uint32 equipPosition, uint32 refine)
    {
        // For testing until the refiner NPCs (R8): sets the refine level of what's worn in an RO equipment position
        Player* player = handler->getSelectedPlayerOrSelf();
        Item* item = player != nullptr ? ROWOW->GetIdentityEquippedItem(player, equipPosition) : nullptr;
        if (item == nullptr || refine > ROWOW_MAX_REFINE)
        {
            handler->SendSysMessage("Nothing worn there, or a refine level past 10.");
            return false;
        }
        ROWOWMod::SetItemState(player, item, refine, ROWOWMod::IsItemBroken(item), ROWOWMod::IsItemIdentified(item));
        ROWOW->RefreshROForm(player);
        handler->PSendSysMessage("{} is now +{}.", item->GetTemplate()->Name1, refine);
        return true;
    }

    static bool HandleROGiveCommand(ChatHandler* handler, uint32 itemID, Optional<uint32> amount)
    {
        // Gives an RO item (item_db Id) as a monster drop or a box would: equipment comes unidentified
        Player* player = handler->getSelectedPlayerOrSelf();
        auto itemTemplateIDIter = ROWOW->ItemTemplateIDsByItemID.find(itemID);
        auto itemIter = itemTemplateIDIter != ROWOW->ItemTemplateIDsByItemID.end() ? ROWOW->ItemsByItemTemplateID.find(itemTemplateIDIter->second) : ROWOW->ItemsByItemTemplateID.end();
        if (player == nullptr || itemIter == ROWOW->ItemsByItemTemplateID.end())
        {
            handler->PSendSysMessage("There is no converted RO item {}.", itemID);
            return false;
        }
        uint8 type = itemIter->second.Type;
        bool isIdentified = type != ROWOW_ITEM_TYPE_WEAPON && type != ROWOW_ITEM_TYPE_ARMOR && type != ROWOW_ITEM_TYPE_PETARMOR;
        ROWOW->GiveROItem(player, itemID, amount.value_or(1), isIdentified);
        return true;
    }

    static bool HandleROBattleCommand(ChatHandler* handler, Optional<uint32> swings, Optional<std::string> skillName, Optional<uint32> skillLevel)
    {
        // Works out RO attacks between the player and the selected RO monster without dealing them, and prints the totals the rAthena
        // oracle's @oracledmg prints (for comparing with it): normal attacks both ways, and with a skill (its skill_db name or id and level)
        // the skill's cast time, after-cast delay, SP and heal, and its hits on the monster.  The lines also go to the server log
        Player* player = handler->GetSession() != nullptr ? handler->GetSession()->GetPlayer() : nullptr;
        Creature* creature = handler->getSelectedCreature();
        if (player == nullptr || ROWOW->IsInROForm(player) == false || creature == nullptr || ROWOW->GetROStatus(creature) == nullptr)
        {
            handler->SendSysMessage("Select an RO monster, in the Ragnarok Online world.");
            return false;
        }
        auto print = [handler](std::string_view format, auto&&... args)
        {
            std::string line = fmt::format(fmt::runtime(format), std::forward<decltype(args)>(args)...);
            handler->SendSysMessage(line);
            LOG_INFO("module.ROWOW", ".ro battle: {}", line);
        };
        uint16 skillID = 0;
        if (skillName.has_value() == true)
        {
            skillID = ROWOW->GetSkillIDByName(*skillName);
            if (skillID == 0 && std::all_of(skillName->begin(), skillName->end(), ::isdigit) == true)
                skillID = uint16(strtoul(skillName->c_str(), nullptr, 10));
            if (ROWOW->GetSkill(skillID) == nullptr)
            {
                print("No RO skill '{}'.", *skillName);
                return false;
            }
        }
        uint32 swingCount = std::clamp<uint32>(swings.value_or(1000), 1, 1000000);
        ROWOWStatus const& pcStatus = *ROWOW->GetROStatus(player);
        print("pc_status str={} agi={} vit={} int={} dex={} luk={} maxhp={} maxsp={}", pcStatus.Stats[0], pcStatus.Stats[1], pcStatus.Stats[2], pcStatus.Stats[3],
            pcStatus.Stats[4], pcStatus.Stats[5], pcStatus.MaxHP, pcStatus.MaxSP);
        print("pc_status_atk batk={} watk={} watk2={} lwatk={} lwatk2={} matk_min={} matk_max={} weapon_ele={} lweapon_ele={}", pcStatus.BaseAttack,
            pcStatus.WeaponAttack, pcStatus.WeaponRefineAttack, pcStatus.LeftWeaponAttack, pcStatus.LeftWeaponRefineAttack, pcStatus.MagicAttackMin,
            pcStatus.MagicAttackMax, pcStatus.WeaponElement, pcStatus.LeftWeaponElement);
        print("pc_status_def def={} def2={} mdef={} mdef2={} hit={} flee={} flee2={} cri={} def_ele={} ele_lv={}", pcStatus.Def, pcStatus.Def2, pcStatus.MDef,
            pcStatus.MDef2, pcStatus.Hit, pcStatus.Flee, pcStatus.Flee2, pcStatus.Critical, pcStatus.DefElement, pcStatus.ElementLevel);
        print("pc_status_speed amotion={} adelay={} dmotion={} aspd_rate={} speed={} aspd_display={}", pcStatus.AttackMotion, pcStatus.AttackDelay,
            pcStatus.DamageMotion, pcStatus.AspdRate, pcStatus.Speed, (2000 - pcStatus.AttackMotion) / 10);
        ROWOWStatus const& mobStatus = *ROWOW->GetROStatus(creature);
        print("mob {} lv={} str={} agi={} vit={} int={} dex={} luk={} maxhp={} watk={} watk2={} def={} def2={} mdef={} mdef2={} hit={} flee={} flee2={} "
            "cri={} def_ele={} ele_lv={} race={} size={} class={} amotion={} adelay={} dmotion={}", ROWOW->GetROMobID(creature), mobStatus.Level, mobStatus.Stats[0],
            mobStatus.Stats[1], mobStatus.Stats[2], mobStatus.Stats[3], mobStatus.Stats[4], mobStatus.Stats[5], mobStatus.MaxHP, mobStatus.WeaponAttack,
            mobStatus.WeaponRefineAttack, mobStatus.Def, mobStatus.Def2, mobStatus.MDef, mobStatus.MDef2, mobStatus.Hit, mobStatus.Flee, mobStatus.Flee2,
            mobStatus.Critical, mobStatus.DefElement, mobStatus.ElementLevel, mobStatus.Race, mobStatus.Size, mobStatus.Class, mobStatus.AttackMotion,
            mobStatus.AttackDelay, mobStatus.DamageMotion);
        print("mob_speed speed={} batk={} mode={}", mobStatus.Speed, mobStatus.BaseAttack, mobStatus.Mode);
        auto runSwings = [print, swingCount](char const* label, Unit* src, Unit* target, int32 attackType, uint16 swingSkillID, uint16 swingSkillLevel)
        {
            uint32 landed = 0, flee = 0, lucky = 0, otherMiss = 0, crit = 0, multi = 0;
            uint64 total = 0, normalTotal = 0, critTotal = 0;
            int64 normalMin = std::numeric_limits<int64>::max(), normalMax = 0, critMin = std::numeric_limits<int64>::max(), critMax = 0;
            int32 divMin = std::numeric_limits<int32>::max(), divMax = 0;
            for (uint32 swing = 0; swing < swingCount; swing++)
            {
                ROWOWDamage d = ROWOW->BattleCalcAttack(attackType, src, target, swingSkillID, swingSkillLevel, 0);
                int64 damage = d.Damage + d.Damage2;
                if (d.DmgLv == ROWOW_ATK_LUCKY || d.Type == ROWOW_DMG_LUCY_DODGE)
                {
                    lucky++;
                    continue;
                }
                if (d.DmgLv == ROWOW_ATK_FLEE)
                {
                    flee++;
                    continue;
                }
                if (damage < 1)
                {
                    otherMiss++;
                    continue;
                }
                landed++;
                total += uint64(damage);
                divMin = std::min<int32>(divMin, std::abs(d.Div));
                divMax = std::max<int32>(divMax, std::abs(d.Div));
                if (d.Type == ROWOW_DMG_MULTI_HIT)
                    multi++;
                if (d.Type == ROWOW_DMG_CRITICAL)
                {
                    crit++;
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
            uint32 normalCount = landed - std::min(landed, crit);
            print("{} n={} landed={} flee={} lucky={} othermiss={} crit={} multi={} hit_pct={:.2f} crit_pct={:.2f} lucky_pct={:.2f} avg_landed={:.3f} div={}-{}",
                label, swingCount, landed, flee, lucky, otherMiss, crit, multi, 100.0 * landed / swingCount, 100.0 * crit / swingCount, 100.0 * lucky / swingCount,
                landed != 0 ? double(total) / landed : 0.0, landed != 0 ? divMin : 0, divMax);
            print("{}_normal count={} min={} max={} avg={:.3f} crit_count={} crit_min={} crit_max={} crit_avg={:.3f}", label, normalCount,
                normalCount != 0 ? normalMin : 0, normalMax, normalCount != 0 ? double(normalTotal) / normalCount : 0.0, crit, crit != 0 ? critMin : 0, critMax,
                crit != 0 ? double(critTotal) / crit : 0.0);
        };
        runSwings("pc2mob", player, creature, ROWOW_BF_WEAPON, 0, 0);
        runSwings("mob2pc", creature, player, ROWOW_BF_WEAPON, 0, 0);
        if (skillID != 0)
        {
            ROWOWSkill const* skill = ROWOW->GetSkill(skillID);
            uint16 level = uint16(std::clamp<uint32>(skillLevel.value_or(1), 1, skill->MaxLevel));
            int32 levelIndex = ROWOWSkill::GetLevelIndex(level);
            print("skill id={} lv={} type={} ele={} cast_ms={} delay_ms={} sp={} heal={}", skillID, level, skill->AttackType, skill->Element[levelIndex],
                ROWOW->SkillCastFix(player, skillID, level), ROWOW->SkillDelayFix(player, skillID, level), skill->SPCost[levelIndex],
                ROWOW->SkillCalcHeal(player, player, skillID, level, true));
            if (skill->AttackType == ROWOW_BF_WEAPON || skill->AttackType == ROWOW_BF_MAGIC || skill->AttackType == ROWOW_BF_MISC)
            {
                runSwings("pcskill", player, creature, skill->AttackType, skillID, level);
                // A skill that hits its caster too (Grand Cross) on the player itself
                if (skill->Inf2[ROWOW_INF2_TARGETSELF] == true)
                    runSwings("pcself", player, player, skill->AttackType, skillID, level);
            }
        }
        return true;
    }

    // A status by its status.yml name (with or without SC_, any case) or number, or -1
    static int32 GetStatusTypeByName(std::string const& name)
    {
        std::string upperName = name;
        std::transform(upperName.begin(), upperName.end(), upperName.begin(), [](unsigned char c) { return char(toupper(c)); });
        if (upperName.rfind("SC_", 0) == 0)
            upperName = upperName.substr(3);
        for (auto const& statusName : ROWOWStatusNames)
            if (upperName == statusName.first)
                return statusName.second;
        if (upperName.empty() == false && std::all_of(upperName.begin(), upperName.end(), ::isdigit) == true)
            return ROWOW->GetStatusChangeDB(int32(strtol(upperName.c_str(), nullptr, 10))) != nullptr ? int32(strtol(upperName.c_str(), nullptr, 10)) : -1;
        return -1;
    }

    // The selected RO combatant (an RO monster or a player in the form), or the player
    static Unit* GetSelectedROCombatant(ChatHandler* handler)
    {
        Unit* unit = handler->getSelectedUnit();
        if (unit == nullptr && handler->GetSession() != nullptr)
            unit = handler->GetSession()->GetPlayer();
        if (unit == nullptr || ROWOW->IsROCombatant(unit) == false)
            return nullptr;
        return unit;
    }

    static bool HandleROStatusChangeCommand(ChatHandler* handler, std::string statusName, int32 val1, Optional<int32> duration, Optional<int32> val2, Optional<int32> val3,
        Optional<int32> val4)
    {
        // rAthena's @sc_start: starts a status on the selected RO combatant (or the player) at 100%, past its resistance and with the
        // duration as given (0 or none: the status's skill's Duration1 at val1, else a minute), the status working out its values from val1
        Unit* unit = GetSelectedROCombatant(handler);
        int32 type = GetStatusTypeByName(statusName);
        if (unit == nullptr || type < 0)
        {
            handler->SendSysMessage(unit == nullptr ? "Select an RO combatant, in the Ragnarok Online world." : "No such RO status.");
            return false;
        }
        int64 tick = duration.value_or(0);
        if (tick == 0)
        {
            uint16 skillID = ROWOW->StatusDBGetSkill(type);
            tick = skillID != 0 ? ROWOW->SkillGetTime(skillID, uint16(std::max(1, val1))) : 0;
            if (tick == 0)
                tick = 60000;
        }
        Player* player = handler->GetSession() != nullptr ? handler->GetSession()->GetPlayer() : nullptr;
        bool started = ROWOW->StatusChangeStart(player != nullptr ? static_cast<Unit*>(player) : unit, unit, type, 10000, val1, val2.value_or(0), val3.value_or(0), val4.value_or(0),
            tick, ROWOW_SCSTART_NOAVOID | ROWOW_SCSTART_NOTICKDEF | ROWOW_SCSTART_NORATEDEF);
        handler->PSendSysMessage("{} on {}: {} ({} ms).", statusName, unit->GetName(), started == true ? "started" : "failed", tick);
        return true;
    }

    static bool HandleROStatusChangeEndCommand(ChatHandler* handler, std::string statusName)
    {
        // rAthena's @sc_end: ends a status on the selected RO combatant (or the player)
        Unit* unit = GetSelectedROCombatant(handler);
        int32 type = GetStatusTypeByName(statusName);
        if (unit == nullptr || type < 0)
        {
            handler->SendSysMessage(unit == nullptr ? "Select an RO combatant, in the Ragnarok Online world." : "No such RO status.");
            return false;
        }
        bool ended = ROWOW->StatusChangeEnd(unit, type);
        handler->PSendSysMessage("{} on {}: {}.", statusName, unit->GetName(), ended == true ? "ended" : "not there");
        return true;
    }

    static bool HandleROStatusChangeRateCommand(ChatHandler* handler, std::string statusName, int32 rate, int32 duration, Optional<uint32> runs)
    {
        // How often a status lands on the selected RO monster from the player (for comparing with the oracle's @oraclescrate): starts it runs
        // times at the rate (1/10000) and duration, through its resistances, ending it after each that landed, and prints the count and the
        // durations it got
        Player* player = handler->GetSession() != nullptr ? handler->GetSession()->GetPlayer() : nullptr;
        Unit* unit = handler->getSelectedUnit();
        int32 type = GetStatusTypeByName(statusName);
        if (player == nullptr || unit == nullptr || unit == player || ROWOW->IsROCombatant(unit) == false || ROWOW->IsInROForm(player) == false || type < 0)
        {
            handler->SendSysMessage(type < 0 ? "No such RO status." : "Select an RO monster, in the Ragnarok Online world.");
            return false;
        }
        uint32 runCount = std::clamp<uint32>(runs.value_or(1000), 1, 100000);
        uint32 landed = 0;
        int64 tickMin = std::numeric_limits<int64>::max(), tickMax = 0, tickTotal = 0;
        uint64 now = ROWOW->GetTick();
        for (uint32 run = 0; run < runCount; run++)
        {
            if (ROWOW->StatusChangeStart(player, unit, type, rate, 1, 0, 0, 0, duration, ROWOW_SCSTART_NONE) == false)
                continue;
            landed++;
            if (ROWOWStatusChangeEntry const* sce = ROWOW->GetSCE(unit, type))
            {
                int64 tick = sce->EndTime > now ? int64(sce->EndTime - now) : 0;
                tickMin = std::min(tickMin, tick);
                tickMax = std::max(tickMax, tick);
                tickTotal += tick;
            }
            ROWOW->StatusChangeEnd(unit, type);
        }
        std::string line = fmt::format("scrate type={} rate={} tick={} runs={} ok={} pct={:.2f} tick_min={} tick_max={} tick_avg={:.1f}", type, rate, duration, runCount, landed,
            100.0 * landed / runCount, landed != 0 ? tickMin : 0, tickMax, landed != 0 ? double(tickTotal) / landed : 0.0);
        handler->SendSysMessage(line);
        LOG_INFO("module.ROWOW", ".ro scrate: {}", line);
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
