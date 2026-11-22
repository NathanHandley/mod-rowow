//  Author: Nathan Handley (nathanhandley@protonmail.com)
//  Copyright (c) 2026 Nathan Handley + Automation
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.

// Where the RO equipment worn is (MergedWorlds world equipment, the core's PlayerWorldEquipment.cpp; E:\MW\SHARED.md "World containers").  An item
// worn leaves the bags for world ROWOW_MAP_WORLD's equipment container, one slot per RO equipment position bit (ROWOW.h's ROWOW_EQUIP_POSITION_*:
// the slot is the bit's number).  An item worn in several positions (a two-handed weapon, a headgear covering two) is in the slot of its lowest
// bit, the others stay empty.  Each save slot's identity has its own set (its IdentityID), the one in use being the identity's.  The identity's
// ItemGUIDsByEquipPosition stays the index of what's worn where, built from the container (RebuildWornEquipment) and kept by equip and unequip.
//
// The client's drags on the slots (the RO_Identity addon's buttons pick up and drop on container 61) come here through ROWOWWorldEquipmentHandler.
// The staging container isn't used by Ragnarok: every drag on it is refused

#include "Bag.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "Log.h"
#include "Mail.h"
#include "Player.h"

#include "ROWOW.h"

namespace
{
    class ROWOWWorldEquipmentHandler : public WorldEquipmentHandler
    {
    public:
        InventoryResult OnClientWear(Player* player, uint8 /*world*/, uint8 block, uint8 slot, Item* item) override
        {
            if (block != WORLD_CONTAINER_EQUIPMENT)
                return EQUIP_ERR_ITEM_DOESNT_GO_TO_SLOT;
            return ROWOW->WearWornSlotFromClient(player, item, slot);
        }

        InventoryResult OnClientTakeOff(Player* player, uint8 /*world*/, uint8 block, uint8 slot, uint8 bag, uint8 bagSlot) override
        {
            if (block != WORLD_CONTAINER_EQUIPMENT)
                return EQUIP_ERR_ITEM_DOESNT_GO_TO_SLOT;
            return ROWOW->TakeOffWornSlotFromClient(player, slot, bag, bagSlot);
        }

        InventoryResult OnClientMove(Player* player, uint8 /*world*/, uint8 fromBlock, uint8 fromSlot, uint8 toBlock, uint8 toSlot) override
        {
            if (fromBlock != WORLD_CONTAINER_EQUIPMENT || toBlock != WORLD_CONTAINER_EQUIPMENT)
                return EQUIP_ERR_ITEM_DOESNT_GO_TO_SLOT;
            return ROWOW->MoveWornSlotFromClient(player, fromSlot, toSlot);
        }
    };

    ROWOWWorldEquipmentHandler WorldEquipmentHandlerInstance;

    // The positions an item may choose between when it's worn: an accessory's two, and a weapon either hand holds (PCEquipPoint's ARMS)
    bool IsPositionChoice(uint32 equipPoint, uint32 locations)
    {
        return equipPoint == ROWOW_EQUIP_POSITION_ACC || (equipPoint == ROWOW_EQUIP_POSITION_ARMS && locations == ROWOW_EQUIP_POSITION_HAND_R);
    }
}

void ROWOWMod::RegisterWorldEquipment()
{
    // At startup (OnAfterConfigLoad), before any session exists
    Player::RegisterWorldEquipment(ROWOW_MAP_WORLD, ROWOW_WORN_SLOT_COUNT, &WorldEquipmentHandlerInstance);
}

uint8 ROWOWMod::GetWornSlot(uint32 equipPosition)
{
    for (uint8 slot = 0; slot < ROWOW_WORN_SLOT_COUNT; slot++)
        if ((equipPosition & (uint32(1) << slot)) != 0)
            return slot;
    return ROWOW_WORN_SLOT_COUNT;
}

uint32 ROWOWMod::GetWornEquipPosition(uint8 slot, ROWOWItem const* roItem)
{
    // Derived, never stored: the item's own positions, but only the slot's position for one that chooses between positions (an accessory, a weapon
    // held in the left hand) or whose positions don't hold its slot
    uint32 slotPosition = uint32(1) << slot;
    uint32 locations = roItem != nullptr ? roItem->Locations : 0;
    if (locations == 0 || locations == ROWOW_EQUIP_POSITION_ACC || (locations & slotPosition) == 0)
        return slotPosition;
    // The lowest position an item covers is its slot: anything else would be a place PCEquipItem never puts it
    if (GetWornSlot(locations) != slot)
        return slotPosition;
    return locations;
}

Item* ROWOWMod::GetWornItem(Player* player, ObjectGuid::LowType itemGUID) const
{
    return player->GetWorldEquipmentItemByGuid(ROWOW_MAP_WORLD, ObjectGuid::Create<HighGuid::Item>(itemGUID));
}

Item* ROWOWMod::GetWornOrCarriedItem(Player* player, ObjectGuid::LowType itemGUID) const
{
    if (Item* item = GetWornItem(player, itemGUID))
        return item;
    return player->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(itemGUID));
}

uint32 ROWOWMod::GetFreeCarriedPlaces(Player* player) const
{
    // The free places in the backpack and the carried bags (what an item taken off can go to)
    uint32 freePlaces = 0;
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; slot++)
        if (player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot) == nullptr)
            freePlaces++;
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; bagSlot++)
        if (Bag* bag = player->GetBagByPos(bagSlot))
            freePlaces += bag->GetFreeSlots();
    return freePlaces;
}

void ROWOWMod::RebuildWornEquipment(Player* player, ROWOWIdentity& identity) const
{
    identity.ItemGUIDsByEquipPosition.clear();
    for (uint8 slot = 0; slot < ROWOW_WORN_SLOT_COUNT; slot++)
    {
        Item* item = player->GetWorldEquipmentItem(ROWOW_MAP_WORLD, slot);
        if (item == nullptr)
            continue;
        ROWOWItem const* roItem = GetROItem(item);
        uint32 equipPosition = GetWornEquipPosition(slot, roItem);
        if (roItem == nullptr || (roItem->Locations & equipPosition) == 0)
            LOG_ERROR("module.ROWOW", "RebuildWornEquipment: {} wears item {} (entry {}) in slot {}, which isn't one of its positions ({:#x})", player->GetName(),
                item->GetGUID().GetCounter(), item->GetEntry(), slot, roItem != nullptr ? roItem->Locations : 0);
        identity.ItemGUIDsByEquipPosition[equipPosition] = item->GetGUID().GetCounter();
    }
}

void ROWOWMod::ActivateIdentityEquipment(Player* player, ROWOWIdentity& identity)
{
    // The identity's set goes in use (the set of the identity before it is put away), then what mod_rowow_identity_equipment had worn from before
    // world equipment is put in it (once: the rows go with the next save of the inventory).  The core loads the sets with the inventory; an
    // identity loaded before that waits (GetIdentity does this at its next call)
    if (player->IsWorldEquipmentLoaded() == false)
    {
        identity.IsWorldEquipmentActivationPending = true;
        return;
    }
    identity.IsWorldEquipmentActivationPending = false;
    if (identity.Exists == false)
    {
        // A save slot with no identity yet wears nothing: the slot before it keeps its set, put away
        player->DeactivateWorldEquipmentSet(ROWOW_MAP_WORLD);
        identity.ItemGUIDsByEquipPosition.clear();
        return;
    }
    InventoryResult result = player->ActivateWorldEquipmentSet(ROWOW_MAP_WORLD, identity.IdentityID);
    if (result != EQUIP_ERR_OK)
        LOG_ERROR("module.ROWOW", "ActivateIdentityEquipment: {}'s identity {} equipment couldn't be put in use ({})", player->GetName(), identity.IdentityID, uint32(result));

    // Migration: each item where it is (the bags, or the put away carried set) into its slot, unless that's taken.  Nothing is ever displaced
    for (auto const& legacyRow : identity.LegacyEquipmentRows)
    {
        uint8 slot = GetWornSlot(legacyRow.first);
        ObjectGuid itemGUID = ObjectGuid::Create<HighGuid::Item>(legacyRow.second);
        if (slot >= ROWOW_WORN_SLOT_COUNT || player->GetWorldEquipmentItem(ROWOW_MAP_WORLD, slot) != nullptr || GetWornItem(player, legacyRow.second) != nullptr)
            continue;
        InventoryResult migrateResult = EQUIP_ERR_ITEM_NOT_FOUND;
        if (Item* item = player->GetItemByGuid(itemGUID))
            migrateResult = player->WearWorldEquipmentItem(ROWOW_MAP_WORLD, slot, item);
        else
        {
            for (auto stowedIter = identity.StowedItems.begin(); stowedIter != identity.StowedItems.end(); ++stowedIter)
            {
                if (stowedIter->StowedItem == nullptr || stowedIter->StowedItem->GetGUID() != itemGUID)
                    continue;
                Item* item = stowedIter->StowedItem;
                migrateResult = player->PutWorldEquipmentItem(ROWOW_MAP_WORLD, slot, item);
                if (migrateResult == EQUIP_ERR_OK)
                {
                    // Out of the put away set's rows with the next save of the inventory, which writes its world equipment row in the same transaction
                    identity.StowedItems.erase(stowedIter);
                    identity.AreStowedItemsChanged = true;
                }
                break;
            }
        }
        LOG_INFO("module.ROWOW", "ActivateIdentityEquipment: {}'s identity {}: worn item {} (position {:#x}) {}", player->GetName(), identity.IdentityID,
            legacyRow.second, legacyRow.first, migrateResult == EQUIP_ERR_OK ? "moved to its equipment slot" : fmt::format("left where it is ({})", uint32(migrateResult)));
    }
    if (identity.LegacyEquipmentRows.empty() == false)
    {
        identity.LegacyEquipmentRows.clear();
        identity.IsLegacyEquipmentDeletePending = true;
    }
    RebuildWornEquipment(player, identity);
}

void ROWOWMod::MailWornItem(Player* player, uint8 slot)
{
    // In one transaction: out of the world equipment's and the inventory's rows, into a mail.  Whenever the server stops it's in one place
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    Item* item = player->RemoveWorldEquipmentItem(ROWOW_MAP_WORLD, slot, trans);
    if (item == nullptr)
        return;
    if (item->GetState() == ITEM_UNCHANGED)
        item->FSetState(ITEM_CHANGED); // Its item_instance row as it is now
    item->SaveToDB(trans);
    std::string itemName = item->GetTemplate()->Name1;
    LOG_INFO("module.ROWOW", "MailWornItem: {}'s worn item {} (entry {}) came off with no room in the bags, sent by mail", player->GetName(),
        item->GetGUID().GetCounter(), item->GetEntry());
    MailDraft draft("Ragnarok Online", "Your bags had no room for this when it came off, so it was sent to you here.");
    draft.AddItem(item);
    draft.SendMailTo(trans, MailReceiver(player), MailSender(MAIL_NORMAL, 0, MAIL_STATIONERY_GM));
    CharacterDatabase.CommitTransaction(trans);
    ChatHandler(player->GetSession()).SendSysMessage(fmt::format("Your {} came off and was sent to you by mail: there was no room in your bags.", itemName));
}

InventoryResult ROWOWMod::MoveWornItemOff(Player* player, uint8 slot, bool isAsked, uint8 bag, uint8 bagSlot)
{
    // The one destination rule for an item coming off: a place in the RO bags when they're on the character, else (only when it's forced) the mail
    if (player->GetWorldEquipmentItem(ROWOW_MAP_WORLD, slot) == nullptr)
        return EQUIP_ERR_OK;
    if (IsROSetCarried(player) == true)
    {
        InventoryResult result = player->TakeOffWorldEquipmentItem(ROWOW_MAP_WORLD, slot, bag, bagSlot);
        if (result == EQUIP_ERR_OK || isAsked == true)
            return result;
        if (bag != NULL_BAG && player->TakeOffWorldEquipmentItem(ROWOW_MAP_WORLD, slot) == EQUIP_ERR_OK)
            return EQUIP_ERR_OK;
    }
    else if (isAsked == true)
        return EQUIP_ERR_CANT_DO_RIGHT_NOW;
    MailWornItem(player, slot);
    return EQUIP_ERR_OK;
}

void ROWOWMod::RunWornItemScripts(Player* player, Item* item, bool isEquip)
{
    // The item's EquipScript or UnEquipScript, then its cards'
    ROWOWItem const* roItem = GetROItem(item);
    if (roItem == nullptr)
        return;
    RunItemScript(player, isEquip == true ? roItem->EquipScript.get() : roItem->UnEquipScript.get(), item);
    for (uint8 slot = 0; slot < ROWOW_MAX_SLOTS; slot++)
    {
        auto cardTemplateIDIter = ItemTemplateIDsByItemID.find(GetItemCard(item, slot));
        auto cardIter = cardTemplateIDIter != ItemTemplateIDsByItemID.end() ? ItemsByItemTemplateID.find(cardTemplateIDIter->second) : ItemsByItemTemplateID.end();
        if (cardIter != ItemsByItemTemplateID.end())
            RunItemScript(player, isEquip == true ? cardIter->second.EquipScript.get() : cardIter->second.UnEquipScript.get(), item);
    }
}

uint32 ROWOWMod::GetWornEquipPositionOfSlot(ROWOWIdentity const& identity, uint8 slot) const
{
    for (auto const& itemGUIDByEquipPosition : identity.ItemGUIDsByEquipPosition)
        if (GetWornSlot(itemGUIDByEquipPosition.first) == slot)
            return itemGUIDByEquipPosition.first;
    return 0;
}

uint32 ROWOWMod::GetWornItemCount(Player* player, uint32 itemTemplateID) const
{
    uint32 count = 0;
    for (uint8 slot = 0; slot < ROWOW_WORN_SLOT_COUNT; slot++)
        if (Item* item = player->GetWorldEquipmentItem(ROWOW_MAP_WORLD, slot))
            if (item->GetEntry() == itemTemplateID)
                count += item->GetCount();
    return count;
}

void ROWOWMod::DestroyWornItemCount(Player* player, uint32 itemTemplateID, uint32 count)
{
    // What's worn of a kind, used up (a script's delitem past what the bags hold): each taken off in place through the unequip path, then destroyed
    ROWOWIdentity* identity = GetIdentity(player);
    std::vector<uint32> equipPositions;
    for (auto const& itemGUIDByEquipPosition : identity->ItemGUIDsByEquipPosition)
        equipPositions.push_back(itemGUIDByEquipPosition.first);
    for (uint32 equipPosition : equipPositions)
    {
        if (count == 0)
            break;
        Item* item = player->GetWorldEquipmentItem(ROWOW_MAP_WORLD, GetWornSlot(equipPosition));
        if (item == nullptr || item->GetEntry() != itemTemplateID)
            continue;
        uint32 itemCount = std::min(count, item->GetCount());
        count -= itemCount;
        if (itemCount == item->GetCount())
            PCUnequipItem(player, equipPosition, 1 | ROWOW_UNEQUIP_IN_PLACE);
        player->DestroyItemCount(item, itemCount, true);
    }
}

bool ROWOWMod::TopUpWornAmmo(Player* player, Item* ammo)
{
    // Ammo of the worn stack's kind joins it, up to a full stack (D6): from the item given, or from the bags.  True when any did
    Item* wornAmmo = GetIdentityEquippedItem(player, ROWOW_EQUIP_POSITION_AMMO);
    if (wornAmmo == nullptr)
        return false;
    uint32 maxCount = wornAmmo->GetMaxStackCount();
    bool isTopped = false;
    auto topUpFrom = [&](Item* source)
    {
        if (source == nullptr || source == wornAmmo || source->GetEntry() != wornAmmo->GetEntry() || wornAmmo->GetCount() >= maxCount)
            return;
        uint32 count = std::min(source->GetCount(), maxCount - wornAmmo->GetCount());
        if (count == 0)
            return;
        wornAmmo->SetCount(wornAmmo->GetCount() + count);
        wornAmmo->SetState(ITEM_CHANGED, player);
        player->DestroyItemCount(source, count, true);
        isTopped = true;
    };
    if (ammo != nullptr)
    {
        topUpFrom(ammo);
        return isTopped;
    }
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END && wornAmmo->GetCount() < maxCount; slot++)
        topUpFrom(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END && wornAmmo->GetCount() < maxCount; bagSlot++)
        if (Bag* bag = player->GetBagByPos(bagSlot))
            for (uint32 slot = 0; slot < bag->GetBagSize() && wornAmmo->GetCount() < maxCount; slot++)
                topUpFrom(bag->GetItemByPos(uint8(slot)));
    return isTopped;
}

InventoryResult ROWOWMod::WearWornSlotFromClient(Player* player, Item* item, uint8 slot)
{
    // A bag item dropped on an equipment slot: .ro wear's checks, then PCEquipItem in the slot's position.  The item may go to another slot than the
    // one it was dropped on (a two-handed weapon dropped on the shield's)
    if (IsROSetCarried(player) == false)
        return EQUIP_ERR_CANT_DO_RIGHT_NOW;
    ROWOWItem const* roItem = GetROItem(item);
    if (roItem == nullptr || roItem->Locations == 0 || slot >= ROWOW_WORN_SLOT_COUNT)
        return EQUIP_ERR_ITEM_DOESNT_GO_TO_SLOT;
    if (IsItemIdentified(item) == false)
    {
        ChatHandler(player->GetSession()).SendSysMessage("Appraise it before wearing it.");
        return EQUIP_ERR_NONE;
    }
    if (IsItemUseBlockedByNpc(player) == true)
    {
        ChatHandler(player->GetSession()).SendSysMessage("You can't put this item on.");
        return EQUIP_ERR_NONE;
    }
    uint32 slotPosition = uint32(1) << slot;
    if ((uint32(PCEquipPoint(player, *GetIdentity(player), *roItem)) & slotPosition) == 0)
        return EQUIP_ERR_ITEM_DOESNT_GO_TO_SLOT;
    InventoryResult result = EQUIP_ERR_NONE;
    if (PCEquipItem(player, item, int32(slotPosition), &result) == true)
        return EQUIP_ERR_OK;
    return result == EQUIP_ERR_OK ? EQUIP_ERR_NONE : result;
}

InventoryResult ROWOWMod::TakeOffWornSlotFromClient(Player* player, uint8 slot, uint8 bag, uint8 bagSlot)
{
    // A worn item dropped in the bags: there, or anywhere in a bag dropped on its button.  Dropped on an item, that item is worn in the slot instead
    // (WoW's exchange), or the two can't be swapped
    if (IsROSetCarried(player) == false)
        return EQUIP_ERR_CANT_DO_RIGHT_NOW;
    if (IsItemUseBlockedByNpc(player) == true)
    {
        ChatHandler(player->GetSession()).SendSysMessage("You can't take this item off.");
        return EQUIP_ERR_NONE;
    }
    if (bagSlot != NULL_SLOT)
    {
        if (Item* bagItem = player->GetItemByPos(bag, bagSlot))
        {
            ROWOWItem const* roBagItem = GetROItem(bagItem);
            if (roBagItem == nullptr || roBagItem->Locations == 0 || (uint32(PCEquipPoint(player, *GetIdentity(player), *roBagItem)) & (uint32(1) << slot)) == 0)
                return EQUIP_ERR_ITEMS_CANT_BE_SWAPPED;
            return WearWornSlotFromClient(player, bagItem, slot);
        }
    }
    uint32 equipPosition = GetWornEquipPositionOfSlot(*GetIdentity(player), slot);
    if (equipPosition == 0)
        return EQUIP_ERR_ITEM_NOT_FOUND;
    InventoryResult result = EQUIP_ERR_NONE;
    if (PCUnequipItem(player, equipPosition, 1 | ROWOW_UNEQUIP_ASKED, bag, bagSlot, &result) == true)
        return EQUIP_ERR_OK;
    return result == EQUIP_ERR_OK ? EQUIP_ERR_NONE : result;
}

InventoryResult ROWOWMod::MoveWornSlotFromClient(Player* player, uint8 fromSlot, uint8 toSlot)
{
    // A worn item dropped on another slot: only where RO lets it choose (the accessories' two places; either hand, for a weapon its job holds in
    // either).  An item in the other place changes places with it, under the same rule
    ROWOWIdentity* identity = GetIdentity(player);
    if (IsROSetCarried(player) == false)
        return EQUIP_ERR_CANT_DO_RIGHT_NOW;
    if (IsItemUseBlockedByNpc(player) == true || (identity->IsInForm == true && PCCantAct(player) == true))
        return EQUIP_ERR_CANT_DO_RIGHT_NOW;
    Item* item = player->GetWorldEquipmentItem(ROWOW_MAP_WORLD, fromSlot);
    ROWOWItem const* roItem = GetROItem(item);
    if (roItem == nullptr || toSlot >= ROWOW_WORN_SLOT_COUNT)
        return EQUIP_ERR_ITEM_NOT_FOUND;
    uint32 fromPosition = uint32(1) << fromSlot;
    uint32 toPosition = uint32(1) << toSlot;
    uint32 equipPoint = uint32(PCEquipPoint(player, *identity, *roItem));
    if (IsPositionChoice(equipPoint, roItem->Locations) == false || (equipPoint & toPosition) == 0)
        return EQUIP_ERR_ITEM_DOESNT_GO_TO_SLOT;
    if (GetWornEquipPositionOfSlot(*identity, fromSlot) != fromPosition)
        return EQUIP_ERR_ITEM_DOESNT_GO_TO_SLOT;
    // The place it goes to is either empty and covered by nothing, or holds an item that may take the other place
    uint32 coveringPosition = 0;
    for (auto const& itemGUIDByEquipPosition : identity->ItemGUIDsByEquipPosition)
        if ((itemGUIDByEquipPosition.first & toPosition) != 0)
            coveringPosition = itemGUIDByEquipPosition.first;
    Item* other = player->GetWorldEquipmentItem(ROWOW_MAP_WORLD, toSlot);
    if (coveringPosition != 0 && (other == nullptr || coveringPosition != toPosition))
        return EQUIP_ERR_ITEMS_CANT_BE_SWAPPED;
    if (other != nullptr)
    {
        ROWOWItem const* roOther = GetROItem(other);
        uint32 otherEquipPoint = roOther != nullptr ? uint32(PCEquipPoint(player, *identity, *roOther)) : 0;
        if (roOther == nullptr || IsPositionChoice(otherEquipPoint, roOther->Locations) == false || (otherEquipPoint & fromPosition) == 0)
            return EQUIP_ERR_ITEMS_CANT_BE_SWAPPED;
    }
    InventoryResult result = player->MoveWorldEquipmentItem(ROWOW_MAP_WORLD, fromSlot, toSlot);
    if (result != EQUIP_ERR_OK)
        return result;
    // The same items stay worn, in other positions: the index changes, the scripts don't run again (rAthena's own change of hand doesn't either)
    identity->ItemGUIDsByEquipPosition.erase(fromPosition);
    identity->ItemGUIDsByEquipPosition.erase(toPosition);
    identity->ItemGUIDsByEquipPosition[toPosition] = item->GetGUID().GetCounter();
    if (other != nullptr)
        identity->ItemGUIDsByEquipPosition[fromPosition] = other->GetGUID().GetCounter();
    identity->IsEquipmentChanged = true;
    if (identity->IsInForm == true)
        RefreshROForm(player);
    else
        SendEquipmentToClient(player, *identity, false);
    return EQUIP_ERR_OK;
}
