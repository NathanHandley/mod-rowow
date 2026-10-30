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
#include "Item.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"

#include "ROWOW.h"

using namespace std;

// RO items' hooks: using an RO item runs its script (ROWOW_Items.cpp), RO equipment drops unidentified, worn items can't be sold or
// traded, and the worn items are kept true to the bags.  All of it runs in the player's own thread (its session's packets, its map's
// update)

namespace
{
    // How often the form's worn items are checked against the bags
    constexpr uint32 EQUIPMENT_CHECK_INTERVAL_IN_MS = 500;
}

class ROWOW_ItemPlayerScript : public PlayerScript
{
public:
    ROWOW_ItemPlayerScript() : PlayerScript("ROWOW_ItemPlayerScript", { PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_LOOT_ITEM, PLAYERHOOK_CAN_SELL_ITEM, PLAYERHOOK_CAN_SET_TRADE_ITEM }) {}

    void OnPlayerUpdate(Player* player, uint32 p_time) override
    {
        if (ROWOW->IsEnabled == false || ROWOW->IsInROForm(player) == false)
            return;
        ROWOWIdentity* identity = ROWOW->GetIdentity(player);
        identity->EquipmentCheckTimer += p_time;
        if (identity->EquipmentCheckTimer < EQUIPMENT_CHECK_INTERVAL_IN_MS)
            return;
        identity->EquipmentCheckTimer = 0;
        ROWOW->UpdateIdentityEquipment(player, *identity);
    }

    void OnPlayerLootItem(Player* player, Item* item, uint32 /*count*/, ObjectGuid lootGUID) override
    {
        // RO equipment an RO monster drops is unidentified (mob_setdropitem with itemdb_isidentified)
        if (ROWOW->IsEnabled == false || item == nullptr || lootGUID.IsCreatureOrVehicle() == false || ROWOW->IsROMap(player->GetMapId()) == false)
            return;
        ROWOWItem const* roItem = ROWOW->GetROItem(item);
        if (roItem == nullptr || (roItem->Type != ROWOW_ITEM_TYPE_WEAPON && roItem->Type != ROWOW_ITEM_TYPE_ARMOR && roItem->Type != ROWOW_ITEM_TYPE_PETARMOR))
            return;
        ROWOWMod::SetItemState(player, item, 0, false, false);
    }

    bool OnPlayerCanSellItem(Player* player, Item* item, Creature* /*creature*/) override
    {
        // RO items are sold for zeny in the RO world only, and never while worn
        if (ROWOW->IsEnabled == false || item == nullptr || ROWOW->IsROItemTemplate(item->GetEntry()) == false)
            return true;
        ROWOWIdentity* identity = ROWOW->GetIdentity(player);
        if (identity->IsInForm == false)
        {
            ChatHandler(player->GetSession()).SendSysMessage("Ragnarok Online items can only be sold in the Ragnarok Online world.");
            return false;
        }
        return ROWOW->GetIdentityEquipPosition(*identity, item->GetGUID().GetCounter()) == 0;
    }

    bool OnPlayerCanSetTradeItem(Player* player, Item* tradedItem, uint8 /*tradeSlot*/) override
    {
        if (ROWOW->IsEnabled == false || tradedItem == nullptr || ROWOW->IsROItemTemplate(tradedItem->GetEntry()) == false)
            return true;
        if (ROWOW->GetIdentityEquipPosition(*ROWOW->GetIdentity(player), tradedItem->GetGUID().GetCounter()) == 0)
            return true;
        ChatHandler(player->GetSession()).SendSysMessage("Take it off before trading it.");
        return false;
    }
};

// The spell every usable RO item casts: it checks the item can be used (pc_isUseitem), and once cast uses it (pc_useitem).  The spell
// still holds the item while its effects run, so the item is used at the player's next update
class ROWOW_UseItemSpellScript : public SpellScript
{
    PrepareSpellScript(ROWOW_UseItemSpellScript);

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
        player->m_Events.AddEventAtOffset([player, itemGUID]()
        {
            ROWOW->PCUseItem(player, itemGUID);
        }, 0ms);
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(ROWOW_UseItemSpellScript::CheckUse);
        OnEffectHit += SpellEffectFn(ROWOW_UseItemSpellScript::HandleUse, EFFECT_0, SPELL_EFFECT_SCRIPT_EFFECT);
    }
};

void AddROWOWItemScripts()
{
    new ROWOW_ItemPlayerScript();
    RegisterSpellScript(ROWOW_UseItemSpellScript);
}
