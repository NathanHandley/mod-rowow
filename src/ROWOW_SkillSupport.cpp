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
//  Ported from rAthena (https://github.com/rathena/rathena), src/map/pc.cpp and skill.cpp, pre-renewal paths:
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL.  For more information, see LICENCE in rAthena's main folder

#include "Chat.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TemporarySummon.h"
#include "Util.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include "ROWOW.h"

using namespace std;

// What skills do to items and players outside the skill engine: stealing (pc_steal_item), Arrow Crafting (skill_arrow_create), breaking
// equipment (skill_break_equip), the invincibility after arriving (pc_setinvincibletimer), riding a Peco (pc_setriding), a Crusader's Devotion
// links (sd->devotion and the devoted's SC_DEVOTION), the Merchant's prices (pc_modifybuyvalue, pc_modifysellvalue), the Mage line's systems
// (Abracadabra's abra_db, Hindsight's menu, the Elemental Converter list, Sense's window, Mystical Amplification's toggle, the elemental
// fields, the cells Ice Wall and Land Protector change), the Acolyte line's (a Monk's Spirit Spheres, reviving, a Priest's partners), the
// Archer line's (a Hunter's falcon, the traps given back and revealed, an ensemble's partner), the Merchant line's (an Alchemist's summoned
// plants and Marine Spheres, Ore Discovery) and the Thief line's (Plagiarism's copy, the strips, Intimidate's warp, Gangster's Paradise,
// Graffiti's message).  All of it runs in the player's own map thread, a Devotion link's or an ensemble's two players, or the Rogues sitting
// together, being on one map whenever either reads the other

namespace
{
    // EQP_WEAPON, EQP_ARMOR, EQP_SHIELD, EQP_HELM, EQP_ACC, and the strip statuses (on monsters) and protections (Chemical Protection) of each
    uint32 const BreakWhereList[5] = { ROWOW_EQUIP_POSITION_HAND_R, ROWOW_EQUIP_POSITION_ARMOR, ROWOW_EQUIP_POSITION_HAND_L,
        ROWOW_EQUIP_POSITION_HEAD_LOW | ROWOW_EQUIP_POSITION_HEAD_MID | ROWOW_EQUIP_POSITION_HEAD_TOP, ROWOW_EQUIP_POSITION_ACC };
    int32 const BreakStripStatuses[5] = { ROWOW_SC_STRIPWEAPON, ROWOW_SC_STRIPARMOR, ROWOW_SC_STRIPSHIELD, ROWOW_SC_STRIPHELM, ROWOW_SC_NONE };
    int32 const BreakProtectStatuses[5] = { ROWOW_SC_CP_WEAPON, ROWOW_SC_CP_ARMOR, ROWOW_SC_CP_SHIELD, ROWOW_SC_CP_HELM, ROWOW_SC_NONE };

    int32 GetItemIDByAegisName(std::string const& aegisName)
    {
        std::string lowerName = aegisName;
        for (char& curChar : lowerName)
            curChar = char(tolower((unsigned char)curChar));
        auto itemIDIter = ROWOW->ItemIDsByAegisName.find(lowerName);
        return itemIDIter != ROWOW->ItemIDsByAegisName.end() ? int32(itemIDIter->second) : 0;
    }

    // A deferred bonus a worn item has (bUnbreakableWeapon and the like, which take no value)
    bool HasDeferredBonus(ROWOWStatus const* status, char const* type)
    {
        if (status == nullptr)
            return false;
        return std::any_of(status->Bonus.Deferred.begin(), status->Bonus.Deferred.end(), [type](ROWOWDeferredBonus const& bonus) { return bonus.Type == type; });
    }

    // The names Sense's window shows a monster's element, race and size by (the client's)
    char const* const ElementNames[ROWOW_ELEMENT_COUNT] = { "Neutral", "Water", "Earth", "Fire", "Wind", "Poison", "Holy", "Shadow", "Ghost", "Undead" };
    char const* const RaceNames[ROWOW_RACE_DRAGON + 1] = { "Formless", "Undead", "Brute", "Plant", "Insect", "Fish", "Demon", "Demi-Human", "Angel", "Dragon" };
    char const* const SizeNames[ROWOW_SIZE_COUNT] = { "Small", "Medium", "Large" };

    // Hindsight's spells (clif_autospell's pre-renewal list): each with the Hindsight level it needs to be above
    std::pair<uint16, uint16> const AutospellSkills[] = { { ROWOW_SKILL_MG_NAPALMBEAT, 0 }, { ROWOW_SKILL_MG_COLDBOLT, 1 }, { ROWOW_SKILL_MG_FIREBOLT, 1 },
        { ROWOW_SKILL_MG_LIGHTNINGBOLT, 1 }, { ROWOW_SKILL_MG_SOULSTRIKE, 4 }, { ROWOW_SKILL_MG_FIREBALL, 7 }, { ROWOW_SKILL_MG_FROSTDIVER, 9 } };
}

bool ROWOWMod::PCStealItem(Player* player, Unit* bl, uint16 skillLevel)
{
    // pc_steal_item: one try at the monster's drops, each at its drop rate scaled by the steal's success rate ((DEX - the monster's DEX) / 2
    // + 6 a level + 4 + bAddStealRate); a monster can be stolen from once.  (Slaves, the treasure race group and nomobloot maps come with them)
    Creature* md = bl != nullptr ? bl->ToCreature() : nullptr;
    uint32 mobID = md != nullptr ? GetROMobID(md) : 0;
    if (md == nullptr || mobID == 0)
        return false;
    ROWOWStealState* stealState = md->CustomData.GetDefault<ROWOWStealState>("ROWOWStealState");
    ROWOWStatusChange const* mdsc = GetStatusChange(static_cast<Unit const*>(md));
    if (stealState->StealFlag == UCHAR_MAX || (mdsc != nullptr && mdsc->Opt1 != 0 && mdsc->Opt1 != ROWOW_OPT1_BURNING)) // already stolen from / status change check
        return false;
    ROWOWStatus const* sdStatus = GetROStatus(player);
    ROWOWStatus const* mdStatus = GetROStatus(md);
    if ((mdStatus->Mode & ROWOW_MODE_STATUS_IMMUNE) != 0 ||
        (BattleConfig.SkillStealMaxTries != 0 && stealState->StealFlag++ >= BattleConfig.SkillStealMaxTries)) // Reached limit of steal attempts. [Lupus]
    {
        // Can't steal from
        stealState->StealFlag = UCHAR_MAX;
        return false;
    }
    // base skill success chance (percentual)
    double rate = (sdStatus->Stats[ROWOW_STAT_DEX] - mdStatus->Stats[ROWOW_STAT_DEX]) / 2 + skillLevel * 6 + 4;
    rate += GetDeferredBonus(player, "bAddStealRate");
    if (rate < 1)
        return false;

    // Try dropping one item.  Droprate is affected by the skill success rate.
    ROWOWDataTable const* mobTable = GetDataTable(ROWOW_DATA_TABLE_MOB);
    int64 rowIndex = GetMobRowIndex(mobID);
    if (mobTable == nullptr || rowIndex < 0)
        return false;
    uint32 itemID = 0;
    for (auto const& drop : ROWOWDataTable::ParseMapList(mobTable->GetCell(size_t(rowIndex), mobTable->GetColumnIndex("Drops"))))
    {
        int32 dropItemID = 0;
        double dropRate = 0;
        bool isStealProtected = false;
        for (auto const& field : drop)
        {
            if (field.first == "Item")
                dropItemID = GetItemIDByAegisName(field.second);
            else if (field.first == "Rate")
                dropRate = atof(field.second.c_str());
            else if (field.first == "StealProtected")
                isStealProtected = field.second == "true";
        }
        if (isStealProtected == true || dropItemID == 0 || ItemTemplateIDsByItemID.find(uint32(dropItemID)) == ItemTemplateIDsByItemID.end())
            continue;
        if (double(urand(0, 9999)) < dropRate * rate / 100.)
        {
            itemID = uint32(dropItemID);
            break;
        }
    }
    if (itemID == 0)
        return false;
    stealState->StealFlag = UCHAR_MAX; // you can't steal from this mob any more
    // itemdb_isidentified: equipment is stolen unidentified
    auto itemTemplateIDIter = ItemTemplateIDsByItemID.find(itemID);
    auto roItemIter = ItemsByItemTemplateID.find(itemTemplateIDIter->second);
    bool isIdentified = roItemIter == ItemsByItemTemplateID.end() || (roItemIter->second.IsWeapon == false && roItemIter->second.IsArmor == false &&
        roItemIter->second.Type != ROWOW_ITEM_TYPE_PETARMOR);
    return GiveROItem(player, itemID, 1, isIdentified);
}

void ROWOWMod::ClifArrowCreateList(Player* player)
{
    // clif_arrow_create_list: the items in the bags (identified, not worn) that Arrow Crafting makes arrows of, for the RO_Identity addon to
    // list (ROWOWARROW "<item template>,..."); the one picked comes back as .ro makearrow (skill_arrow_create)
    ROWOWDataTable const* arrowTable = GetDataTable("mod_rowow_db_create_arrow");
    ROWOWIdentity* identity = GetIdentity(player);
    if (arrowTable == nullptr)
        return;
    int32 sourceColumnIndex = arrowTable->GetColumnIndex("Source");
    std::string itemTemplateIDs;
    int32 count = 0;
    for (size_t rowIndex = 0; rowIndex < arrowTable->Rows.size(); rowIndex++)
    {
        int32 itemID = GetItemIDByAegisName(arrowTable->GetCell(rowIndex, sourceColumnIndex));
        Item* item = itemID != 0 ? PCSearchInventory(player, uint32(itemID)) : nullptr;
        if (item == nullptr || IsItemIdentified(item) == false)
            continue;
        itemTemplateIDs += fmt::format("{}{}", itemTemplateIDs.empty() == true ? "" : ",", item->GetEntry());
        count++;
    }
    if (count > 0)
    {
        identity->MenuSkillID = ROWOW_SKILL_AC_MAKINGARROW;
        identity->MenuSkillValue = count;
    }
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_ADDON, nullptr, nullptr, "ROWOWARROW\t" + itemTemplateIDs);
    player->SendDirectMessage(&data);
}

bool ROWOWMod::SkillArrowCreate(Player* player, uint32 nameID)
{
    // skill_arrow_create: one of the item picked becomes its arrows (create_arrow_db's Make list)
    ROWOWDataTable const* arrowTable = GetDataTable("mod_rowow_db_create_arrow");
    ROWOWIdentity* identity = GetIdentity(player);
    if (arrowTable == nullptr || nameID == 0 || identity->MenuSkillID != ROWOW_SKILL_AC_MAKINGARROW)
        return false;
    identity->MenuSkillID = 0;
    identity->MenuSkillValue = 0;
    if (PCCantAct(player) == true) // Make it fail to avoid shop exploits where you sell something different than you see.
        return false;
    int32 sourceColumnIndex = arrowTable->GetColumnIndex("Source");
    int32 makeColumnIndex = arrowTable->GetColumnIndex("Make");
    for (size_t rowIndex = 0; rowIndex < arrowTable->Rows.size(); rowIndex++)
    {
        if (uint32(GetItemIDByAegisName(arrowTable->GetCell(rowIndex, sourceColumnIndex))) != nameID)
            continue;
        Item* item = PCSearchInventory(player, nameID);
        if (item == nullptr)
            return false;
        uint32 count = 1;
        player->DestroyItemCount(item, count, true);
        for (auto const& made : ROWOWDataTable::ParseMapList(arrowTable->GetCell(rowIndex, makeColumnIndex)))
        {
            int32 madeItemID = 0;
            uint32 amount = 0;
            for (auto const& field : made)
            {
                if (field.first == "Item")
                    madeItemID = GetItemIDByAegisName(field.second);
                else if (field.first == "Amount")
                    amount = uint32(strtoul(field.second.c_str(), nullptr, 10));
            }
            if (madeItemID != 0 && amount != 0)
                GiveROItem(player, uint32(madeItemID), amount, true);
        }
        return true;
    }
    return false;
}

int32 ROWOWMod::SkillBreakEquip(Unit* src, Unit* bl, uint32 where, int32 rate, int32 flag)
{
    // skill_break_equip: breaks the equipment in the positions asked for, each at rate in 10000 (less the target's unbreakable bonuses, and
    // equip_skill_break_rate or equip_self_break_rate); a monster takes the strip status instead (break_mob_equip).  Chemical Protection
    // keeps its position.  Axes, maces, rods and books never break.  Returns the positions broken
    ROWOWStatusChange const* srcsc = GetStatusChange(static_cast<Unit const*>(src));
    // Grant player skills/items the ability to "break" non-player equipment.  WS_MELTDOWN is exempt from this check.
    if (BattleConfig.BreakMobEquip == 0 && bl->IsPlayer() == false && (srcsc == nullptr || srcsc->HasSCE(ROWOW_SC_MELTDOWN) == false))
        return 0;
    ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(bl));
    if (sc != nullptr && sc->IsEmpty() == true)
        sc = nullptr;
    Player* sd = bl->ToPlayer();
    if (sd != nullptr)
    {
        ROWOWStatus const* status = GetROStatus(sd);
        if (HasDeferredBonus(status, "bUnbreakableWeapon") == true)
            where &= ~ROWOW_EQUIP_POSITION_HAND_R;
        if (HasDeferredBonus(status, "bUnbreakableArmor") == true)
            where &= ~ROWOW_EQUIP_POSITION_ARMOR;
        if (HasDeferredBonus(status, "bUnbreakableHelm") == true)
            where &= ~(ROWOW_EQUIP_POSITION_HEAD_LOW | ROWOW_EQUIP_POSITION_HEAD_MID | ROWOW_EQUIP_POSITION_HEAD_TOP);
        if (HasDeferredBonus(status, "bUnbreakableShield") == true)
            where &= ~ROWOW_EQUIP_POSITION_HAND_L;
        if (HasDeferredBonus(status, "bUnbreakableGarment") == true)
            where &= ~ROWOW_EQUIP_POSITION_GARMENT;
        if (HasDeferredBonus(status, "bUnbreakableShoes") == true)
            where &= ~ROWOW_EQUIP_POSITION_SHOES;
        if (int32 unbreakable = GetDeferredBonus(sd, "bUnbreakable"))
            rate -= rate * unbreakable / 100;
        if ((where & ROWOW_EQUIP_POSITION_HAND_R) != 0)
        {
            switch (status->WeaponType)
            {
                case ROWOW_WEAPON_TYPE_FIST: // Bare fists should not break :P
                case ROWOW_WEAPON_TYPE_1HAXE:
                case ROWOW_WEAPON_TYPE_2HAXE:
                case ROWOW_WEAPON_TYPE_MACE: // Axes and Maces can't be broken [DracoRPG]
                case ROWOW_WEAPON_TYPE_2HMACE:
                case ROWOW_WEAPON_TYPE_STAFF:
                case ROWOW_WEAPON_TYPE_2HSTAFF:
                case ROWOW_WEAPON_TYPE_BOOK: // Rods and Books can't be broken [Skotlex]
                case ROWOW_WEAPON_TYPE_DOUBLE_AA: // Axe usage during dual wield should also prevent breaking [Neutral]
                case ROWOW_WEAPON_TYPE_DOUBLE_DA:
                case ROWOW_WEAPON_TYPE_DOUBLE_SA:
                    where &= ~ROWOW_EQUIP_POSITION_HAND_R;
                    break;
            }
        }
    }
    if ((flag & ROWOW_BCT_ENEMY) != 0)
    {
        if (BattleConfig.EquipSkillBreakRate != 100)
            rate = rate * BattleConfig.EquipSkillBreakRate / 100;
    }
    else if ((flag & (ROWOW_BCT_PARTY | ROWOW_BCT_SELF)) != 0)
    {
        if (BattleConfig.EquipSelfBreakRate != 100)
            rate = rate * BattleConfig.EquipSelfBreakRate / 100;
    }
    for (int32 i = 0; i < 5; i++)
    {
        if ((where & BreakWhereList[i]) == 0)
            continue;
        if (sc != nullptr && BreakProtectStatuses[i] != ROWOW_SC_NONE && sc->HasSCE(BreakProtectStatuses[i]) == true)
            where &= ~BreakWhereList[i];
        else if (int32(urand(0, 9999)) >= rate)
            where &= ~BreakWhereList[i];
        else if (sd == nullptr && BreakStripStatuses[i] != ROWOW_SC_NONE) // Cause Strip effect.
            SCStart(src, bl, BreakStripStatuses[i], 100, 0, SkillGetTime(StatusDBGetSkill(BreakStripStatuses[i]), 1));
    }
    if (where == 0) // Nothing to break.
        return 0;
    if (sd != nullptr)
    {
        // Each worn piece in a broken position (weapon or shield by what's held) is broken, and comes off
        ROWOWIdentity* identity = GetIdentity(sd);
        std::vector<std::pair<uint32, ObjectGuid::LowType>> wornItems(identity->ItemGUIDsByEquipPosition.begin(), identity->ItemGUIDsByEquipPosition.end());
        for (auto const& wornItem : wornItems)
        {
            Item* item = sd->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(wornItem.second));
            ROWOWItem const* roItem = GetROItem(item);
            if (roItem == nullptr || IsItemBroken(item) == true || (wornItem.first & ROWOW_EQUIP_POSITION_COSTUME) != 0 || wornItem.first == ROWOW_EQUIP_POSITION_AMMO)
                continue;
            bool isBroken = false;
            if ((wornItem.first & (ROWOW_EQUIP_POSITION_HAND_R | ROWOW_EQUIP_POSITION_HAND_L)) != 0)
                isBroken = ((where & ROWOW_EQUIP_POSITION_HAND_R) != 0 && roItem->IsWeapon == true) || ((where & ROWOW_EQUIP_POSITION_HAND_L) != 0 && roItem->IsArmor == true);
            else
                isBroken = (wornItem.first & where) != 0;
            if (isBroken == false)
                continue;
            SetItemState(sd, item, GetItemRefine(item), true, IsItemIdentified(item));
            PCUnequipItem(sd, wornItem.first, 3);
        }
        ChatHandler(sd->GetSession()).SendNotification("Your equipment broke.");
    }
    return int32(where); // Return list of pieces broken.
}

void ROWOWMod::PCSetInvincibleTimer(Player* player)
{
    // pc_setinvincibletimer: no monster or skill targets the player for player_invincible_time after it arrives on a map or revives.  RO's checks alone
    // hold that (battle_check_target, the monster AI's target checks): WoW's immunity to NPCs would have the client refuse the player's own skills on monsters
    if (BattleConfig.PCInvincibleTime <= 0 || IsInROForm(player) == false)
        return;
    ROWOWIdentity* identity = GetIdentity(player);
    identity->InvincibleEndTime = GetTick() + uint64(BattleConfig.PCInvincibleTime);
    identity->InvinciblePositionX = player->GetPositionX();
    identity->InvinciblePositionY = player->GetPositionY();
}

void ROWOWMod::PCDelInvincibleTimer(Player* player)
{
    // pc_delinvincibletimer: the player acted (walked, attacked, used a skill or an item); the units it stands in act on it now
    ROWOWIdentity* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    if (identity == nullptr || identity->InvincibleEndTime == 0)
        return;
    identity->InvincibleEndTime = 0;
    std::vector<uint16> skillUnitCell;
    int32 x = 0, y = 0;
    GetUnitCell(player, x, y);
    SkillUnitMove(player, GetTick(), 1, x, y, skillUnitCell);
}

void ROWOWMod::UpdateInvincibleTimer(Player* player)
{
    // pc_invincible_timer running out, or the player moving off its spot (clif_parse_WalkToXY) or attacking (clif_parse_ActionRequest)
    ROWOWIdentity* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    if (identity == nullptr || identity->InvincibleEndTime == 0)
        return;
    if (GetTick() >= identity->InvincibleEndTime || identity->IsInForm == false || player->GetVictim() != nullptr ||
        player->GetExactDist2d(identity->InvinciblePositionX, identity->InvinciblePositionY) > 0.5f)
        PCDelInvincibleTimer(player);
}

bool ROWOWMod::PCIsInvincible(Unit const* unit) const
{
    if (unit == nullptr || unit->IsPlayer() == false)
        return false;
    ROWOWIdentity const* identity = unit->ToPlayer()->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    return identity != nullptr && identity->InvincibleEndTime != 0;
}

bool ROWOWMod::PCIsRiding(Unit const* unit) const
{
    // pc_isriding: a player in the form on its Peco
    if (unit == nullptr || unit->IsPlayer() == false)
        return false;
    ROWOWIdentity const* identity = unit->ToPlayer()->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    return identity != nullptr && identity->IsInForm == true && (identity->Option & ROWOW_OPTION_RIDING) != 0;
}

void ROWOWMod::PCSetRiding(Player* player, bool isRiding)
{
    // pc_setriding: mounts a Peco (only with Riding learned) or dismounts it, and pc_setoption's status calculation follows (the speed, the
    // ASPD and Cavalier Mastery, the spear against medium monsters).  The look follows (pc_setoption's clif_changelook): the job's mounted
    // body (Peco Knight, Peco Crusader...) and the riding icon (R9.5)
    ROWOWIdentity* identity = GetIdentity(player);
    uint32 option = identity->Option;
    if (isRiding == true)
    {
        if (PCCheckSkill(*identity, ROWOW_SKILL_KN_RIDING) > 0) // add peco
            option |= ROWOW_OPTION_RIDING;
    }
    else
        option &= ~ROWOW_OPTION_RIDING;
    if (option == identity->Option)
        return;
    identity->Option = option;
    std::bitset<ROWOW_SCB_MAX> calcFlag;
    calcFlag.set(ROWOW_SCB_BASE);
    calcFlag.set(ROWOW_SCB_SPEED);
    StatusCalcBL(player, calcFlag);
    if (identity->IsInForm == true && player->IsInWorld() == true)
        RefreshROFormLook(player);
}

Unit* ROWOWMod::GetDevotionSource(Unit const* bl, bool isRangeChecked) const
{
    // The Crusader of a devoted unit, as rAthena checks it before each use of SC_DEVOTION: the player of val1, on the unit's map, whose slot
    // val2 still names the unit, and (check_distance_bl) within the link's reach, val3.  A mercenary devoted to its master is the other kind
    ROWOWStatusChangeEntry const* sce = GetSCE(bl, ROWOW_SC_DEVOTION);
    if (sce == nullptr || sce->Val2 < 0 || sce->Val2 >= ROWOW_MAX_DEVOTION || bl->IsInWorld() == false)
        return nullptr;
    if (Player const* master = bl->ToPlayer())
    {
        ROWOWMercData const* md = GetMercenary(master);
        if (md != nullptr && md->IsDevotion == true && md->CreatureGUID.GetCounter() == uint32(sce->Val1))
        {
            Creature* mercenary = GetFollowerCreature(const_cast<Player*>(master), *md);
            return mercenary == nullptr || (isRangeChecked == true && DistanceBL(bl, mercenary) > sce->Val3) ? nullptr : mercenary;
        }
    }
    Player* d_bl = ObjectAccessor::GetPlayer(bl->GetMap(), ObjectGuid::Create<HighGuid::Player>(uint32(sce->Val1)));
    ROWOWIdentity const* identity = d_bl != nullptr ? d_bl->CustomData.Get<ROWOWIdentity>("ROWOWIdentity") : nullptr;
    if (identity == nullptr || identity->DevotionGUIDs[sce->Val2] != bl->GetGUID())
        return nullptr;
    if (isRangeChecked == true && DistanceBL(bl, d_bl) > sce->Val3)
        return nullptr;
    return d_bl;
}

Player* ROWOWMod::GetDevotedPlayer(Player* sd, int32 slot)
{
    // sd->devotion[slot]: the player there, on the Crusader's map, whose Devotion still names the Crusader and the slot.  A slot whose player
    // left or lost the status (its status_change_end may have run where the Crusader's map isn't) is let go here
    ROWOWIdentity* identity = GetIdentity(sd);
    ObjectGuid& devotionGUID = identity->DevotionGUIDs[slot];
    if (devotionGUID.IsEmpty() == true)
        return nullptr;
    Player* tsd = ObjectAccessor::GetPlayer(sd->GetMap(), devotionGUID);
    ROWOWStatusChangeEntry const* sce = tsd != nullptr ? GetSCE(tsd, ROWOW_SC_DEVOTION) : nullptr;
    if (sce == nullptr || uint32(sce->Val1) != sd->GetGUID().GetCounter() || sce->Val2 != slot)
    {
        devotionGUID.Clear();
        return nullptr;
    }
    return tsd;
}

int32 ROWOWMod::PCDevotionSlot(Player* sd, ObjectGuid targetGUID, int32 count)
{
    // The slot Devotion uses for a target among the level's: the one devoting the Crusader to it already, else the first free one, else -1
    // (no free slots, the skill fails)
    ROWOWIdentity const* identity = GetIdentity(sd);
    for (int32 i = 0; i < count; i++)
        GetDevotedPlayer(sd, i);
    for (int32 i = 0; i < count; i++)
        if (identity->DevotionGUIDs[i] == targetGUID)
            return i;
    for (int32 i = 0; i < count; i++)
        if (identity->DevotionGUIDs[i].IsEmpty() == true)
            return i;
    return -1;
}

void ROWOWMod::PCDelDevotions(Player* sd)
{
    // pc_dead: the Crusader's devotions end with its death
    ROWOWIdentity* identity = GetIdentity(sd);
    for (int32 k = 0; k < ROWOW_MAX_DEVOTION; k++)
    {
        if (Player* devsd = GetDevotedPlayer(sd, k))
            StatusChangeEnd(devsd, ROWOW_SC_DEVOTION);
        identity->DevotionGUIDs[k].Clear();
    }
}

void ROWOWMod::StatusChangeStartDevoted(Unit* src, Player* sd, int32 type, int32 val1, int32 val2, int32 val3, int32 val4, int64 tick, uint8 flag)
{
    // status_change_start's devotion loops: a Crusader's Auto Guard, Defender, Reflect Shield and Endure go to each player it devotes itself to
    for (int32 i = 0; i < ROWOW_MAX_DEVOTION; i++)
        if (Player* tsd = GetDevotedPlayer(sd, i))
            StatusChangeStart(src, tsd, type, 10000, val1, val2, val3, val4, tick, flag);
}

Unit* ROWOWMod::BattleCheckDevotion(Unit* bl) const
{
    // battle_check_devotion: damage reflected onto a devoted attacker goes to its Crusader instead at devotion_rdamage's chance
    if (BattleConfig.DevotionRDamage != 0 && BattleConfig.DevotionRDamage > int32(urand(0, 99)))
    {
        if (ROWOWStatusChangeEntry const* sce = GetSCE(bl, ROWOW_SC_DEVOTION); sce != nullptr && bl->IsInWorld() == true)
            return ObjectAccessor::GetPlayer(bl->GetMap(), ObjectGuid::Create<HighGuid::Player>(uint32(sce->Val1)));
    }
    return nullptr;
}

void ROWOWMod::BattleFixDamage(Unit* src, Unit* target, int64 damage, uint16 skillID)
{
    // battle_fix_damage (status_fix_damage): the damage alone, with no attack of its own: shown on the target (clif_damage of the target on
    // itself), as the skill's spell, and dealt from src
    if (src == nullptr || target == nullptr || target->IsAlive() == false || damage <= 0)
        return;
    uint32 spellID = GetSkillSpellID(skillID, 1);
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellID != 0 ? spellID : GetWeaponHitSoundSpellID(ROWOW_WEAPON_TYPE_FIST));
    uint32 dealt = uint32(std::clamp<int64>(damage, 0, std::numeric_limits<uint32>::max()));
    if (spellInfo != nullptr)
    {
        SpellNonMeleeDamage log(target, target, spellInfo, SPELL_SCHOOL_MASK_NORMAL);
        log.damage = dealt;
        target->SendSpellNonMeleeDamageLog(&log);
    }
    Unit::DealDamage(src, target, dealt, nullptr, SPELL_DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, spellInfo, false);
}

int32 ROWOWMod::PCModifyBuyValue(Player const* player, int32 origValue) const
{
    // pc_modifybuyvalue: Discount (5% + 2% a level, 24% at 10) or the Rogue's Compulsion Discount (5% + 4% a level), the better
    int32 skill = 0, val = origValue, rate1 = 0, rate2 = 0;
    if ((skill = PCCheckSkill(player, ROWOW_SKILL_MC_DISCOUNT)) > 0) // merchant discount
        rate1 = 5 + skill * 2 - ((skill == 10) ? 1 : 0);
    if ((skill = PCCheckSkill(player, ROWOW_SKILL_RG_COMPULSION)) > 0) // rogue discount
        rate2 = 5 + skill * 4;
    if (rate1 < rate2)
        rate1 = rate2;
    if (rate1 != 0)
        val = int32(double(origValue) * double(100 - rate1) / 100.);
    if (val < 0)
        val = 0;
    if (origValue > 0 && val < 1)
        val = 1;
    return val;
}

int32 ROWOWMod::PCModifySellValue(Player const* player, int32 origValue) const
{
    // pc_modifysellvalue: Overcharge (5% + 2% a level, 24% at 10)
    int32 skill = 0, val = origValue, rate = 0;
    if ((skill = PCCheckSkill(player, ROWOW_SKILL_MC_OVERCHARGE)) > 0) // OverCharge
        rate = 5 + skill * 2 - ((skill == 10) ? 1 : 0);
    if (rate != 0)
        val = int32(double(origValue) * double(100 + rate) / 100.);
    if (val < 0)
        val = 0;
    if (origValue > 0 && val < 1)
        val = 1;
    return val;
}

void ROWOWMod::LoadAbraData()
{
    // abra_db (AbraDatabase::parseBodyNode): the skills Abracadabra can pick, each with its chance in 10000 at each of Abracadabra's levels
    // (one value for all of them, or 500 at every level when none is given).  An unknown or passive skill is left out
    AbraSkills.clear();
    if (IsEnabled == false)
        return;
    ROWOWDataTable const* abraTable = GetDataTable("mod_rowow_db_abra");
    if (abraTable == nullptr)
    {
        LOG_ERROR("module.ROWOW", "ROWOWMod::LoadAbraData found no mod_rowow_db_abra table, so Abracadabra picks nothing (run the converter)");
        return;
    }
    int32 skillColumnIndex = abraTable->GetColumnIndex("Skill");
    int32 probabilityColumnIndex = abraTable->GetColumnIndex("Probability");
    for (size_t rowIndex = 0; rowIndex < abraTable->Rows.size(); rowIndex++)
    {
        std::string const& skillName = abraTable->GetCell(rowIndex, skillColumnIndex);
        ROWOWSkill const* skill = GetSkill(GetSkillIDByName(skillName));
        if (skill == nullptr || skill->IsPassive == true)
        {
            LOG_ERROR("module.ROWOW", "ROWOWMod::LoadAbraData: Abracadabra can't cast skill '{}', skipping", skillName);
            continue;
        }
        ROWOWAbraSkill abraSkill;
        abraSkill.SkillID = skill->SkillID;
        std::string const& probability = abraTable->GetCell(rowIndex, probabilityColumnIndex);
        if (probability.empty() == true)
            std::fill(std::begin(abraSkill.Per), std::end(abraSkill.Per), 500);
        else if (probability.find('=') == std::string::npos)
            std::fill(std::begin(abraSkill.Per), std::end(abraSkill.Per), int32(strtol(probability.c_str(), nullptr, 10)));
        else
        {
            for (auto const& levelProbability : ROWOWDataTable::ParseMapList(probability))
            {
                int32 level = 0, chance = 0;
                for (auto const& field : levelProbability)
                {
                    if (field.first == "Level")
                        level = int32(strtol(field.second.c_str(), nullptr, 10));
                    else if (field.first == "Probability")
                        chance = int32(strtol(field.second.c_str(), nullptr, 10));
                }
                if (level >= 1 && level <= ROWOW_MAX_SKILL_LEVEL)
                    abraSkill.Per[level - 1] = chance;
            }
        }
        AbraSkills.push_back(abraSkill);
    }
    LOG_INFO("module.ROWOW", "ROWOWMod::LoadAbraData loaded {} skills Abracadabra can cast", AbraSkills.size());
}

bool ROWOWMod::SkillCastAbraSkill(Unit* src, uint16 skillID, uint16 skillLevel)
{
    // Abracadabra's pick.  A player's is its skill item (clif_item_skill), which RO's client then lets the player aim; a WoW client can't be
    // made to start aiming a spell, so it's aimed as a monster's Abracadabra aims (SkillHocusPocus's mob branch), with the player's selection
    // standing for the monster's target: a self skill at the caster, a support skill at the selected friend (a monster's at itself), an
    // offensive skill at the target, a ground skill at the target's cell (or the caster's own).  It's cast as the skill's spell, with its cast
    // time; a pick with no spell (the special picks, which have no cast time) goes off at once.  Runs in the caster's update
    ROWOWSkill const* skill = GetSkill(skillID);
    ROWOWSkillUnitData* ud = GetSkillUnitData(src);
    Player* sd = src->ToPlayer();
    if (skill == nullptr || src->IsAlive() == false || IsROCombatant(src) == false || (sd != nullptr && ud->SkillItemID != skillID))
        return false;
    Unit* selected = sd != nullptr ? sd->GetSelectedUnit() : src->GetVictim();
    if (selected != nullptr && (selected->GetMap() != src->GetMap() || IsROCombatant(selected) == false))
        selected = nullptr;
    uint8 castType = SkillGetCastType(skillID);
    Unit* target = src;
    if ((skill->Inf & ROWOW_INF_SUPPORT_SKILL) != 0)
        target = (sd != nullptr && selected != nullptr && BattleCheckTarget(src, selected, ROWOW_BCT_NOENEMY) > 0) ? selected : src;
    else if (castType == ROWOW_CAST_GROUND)
        target = selected != nullptr ? selected : src;
    else if ((skill->Inf & ROWOW_INF_SELF_SKILL) == 0)
    {
        // Assume offensive skills: with no target there's nothing to cast it on
        if (selected == nullptr || selected == src)
        {
            if (sd != nullptr)
            {
                ClifSkillFail(sd, skillID, ROWOW_USESKILL_FAIL_LEVEL);
                ud->SkillItemID = 0;
                ud->SkillItemLevel = 0;
            }
            return false;
        }
        target = selected;
    }

    uint32 spellID = GetSkillSpellID(skillID, skillLevel);
    SpellInfo const* spellInfo = spellID != 0 ? sSpellMgr->GetSpellInfo(spellID) : nullptr;
    if (spellInfo == nullptr)
    {
        WorldLocation dest(target->GetMapId(), target->GetPositionX(), target->GetPositionY(), target->GetPositionZ());
        if (SkillCastEndCheck(src, target, castType == ROWOW_CAST_GROUND ? &dest : nullptr, skillID, skillLevel) != SPELL_CAST_OK)
            return false;
        SkillCastEnd(src, target, castType == ROWOW_CAST_GROUND ? &dest : nullptr, skillID, skillLevel);
        return true;
    }
    SpellCastTargets targets;
    if (castType == ROWOW_CAST_GROUND)
        targets.SetDst(*target);
    else
        targets.SetUnitTarget(target);
    return src->CastSpell(targets, spellInfo, nullptr, TRIGGERED_NONE) == SPELL_CAST_OK;
}

void ROWOWMod::SkillToggleMagicPower(Unit* bl, uint16 skillID)
{
    // skill_toggle_magicpower: Mystical Amplification waits for a magic damage skill; that skill's cast start turns it on (val4) for the
    // skill's hits, and the next one's start ends it.  Non-offensive and non-magic skills do not affect the status
    ROWOWSkill const* skill = GetSkill(skillID);
    if (skill == nullptr || skill->NK[ROWOW_NK_NODAMAGE] == true || (skill->AttackType & ROWOW_BF_MAGIC) == 0)
        return;
    ROWOWStatusChange* sc = GetStatusChange(bl);
    ROWOWStatusChangeEntry* sce = sc != nullptr ? sc->GetSCE(ROWOW_SC_MAGICPOWER) : nullptr;
    if (sce == nullptr)
        return;
    if (sce->Val4 != 0)
        StatusChangeEnd(bl, ROWOW_SC_MAGICPOWER);
    else
    {
        sce->Val4 = 1;
        if (ROWOWStatusChangeDB const* scdb = GetStatusChangeDB(ROWOW_SC_MAGICPOWER))
            StatusCalcBL(bl, scdb->CalcFlag);
    }
}

void ROWOWMod::ClifAutospell(Player* sd, uint16 skillLevel)
{
    // clif_autospell: Hindsight's menu of the bolts the player knows that its level allows (ROWOWAUTOSPELL "<skill>:<name>,..."), whose pick
    // comes back as .ro autospell (clif_parse_AutoSpell)
    std::string choices;
    for (auto const& autospellSkill : AutospellSkills)
    {
        ROWOWSkill const* skill = GetSkill(autospellSkill.first);
        if (skill == nullptr || skillLevel <= autospellSkill.second || PCCheckSkill(sd, autospellSkill.first) <= 0)
            continue;
        choices += fmt::format("{}{}:{}", choices.empty() == true ? "" : ",", skill->SkillID, skill->Description);
    }
    ROWOWIdentity* identity = GetIdentity(sd);
    identity->MenuSkillID = ROWOW_SKILL_SA_AUTOSPELL;
    identity->MenuSkillValue = skillLevel;
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_ADDON, nullptr, nullptr, "ROWOWAUTOSPELL\t" + choices);
    sd->SendDirectMessage(&data);
}

int32 ROWOWMod::SkillAutospell(Player* sd, uint16 skillID)
{
    // skill_autospell: the bolt picked from Hindsight's menu, at most at the level Hindsight's level allows (and the player knows).  The Sage
    // spirit lets the bolts go up to 10
    ROWOWIdentity* identity = GetIdentity(sd);
    if (identity->MenuSkillID != ROWOW_SKILL_SA_AUTOSPELL || GetSkill(skillID) == nullptr)
        return 0;
    uint16 lv = uint16(PCCheckSkill(sd, skillID));
    uint16 skillLevel = uint16(identity->MenuSkillValue);
    identity->MenuSkillID = 0; // clif_menuskill_clear
    identity->MenuSkillValue = 0;
    uint16 maxlv = 1;
    if (skillLevel == 0 || lv == 0)
        return 0; // Player must learn the skill before doing auto-spell [Lance]
    if (skillID == ROWOW_SKILL_MG_NAPALMBEAT)
        maxlv = 3;
    else if (skillID == ROWOW_SKILL_MG_COLDBOLT || skillID == ROWOW_SKILL_MG_FIREBOLT || skillID == ROWOW_SKILL_MG_LIGHTNINGBOLT)
    {
        ROWOWStatusChangeEntry const* spirit = GetSCE(sd, ROWOW_SC_SPIRIT);
        if (spirit != nullptr && spirit->Val2 == ROWOW_SKILL_SL_SAGE)
            maxlv = 10; // Soul Linker bonus. [Skotlex]
        else if (skillLevel == 2)
            maxlv = 1;
        else if (skillLevel == 3)
            maxlv = 2;
        else if (skillLevel >= 4)
            maxlv = 3;
    }
    else if (skillID == ROWOW_SKILL_MG_SOULSTRIKE)
    {
        if (skillLevel == 5)
            maxlv = 1;
        else if (skillLevel == 6)
            maxlv = 2;
        else if (skillLevel >= 7)
            maxlv = 3;
    }
    else if (skillID == ROWOW_SKILL_MG_FIREBALL)
    {
        if (skillLevel == 8)
            maxlv = 1;
        else if (skillLevel >= 9)
            maxlv = 2;
    }
    else if (skillID == ROWOW_SKILL_MG_FROSTDIVER)
        maxlv = 1;
    else
        return 0;
    maxlv = std::min(lv, maxlv);
    SCStart4(sd, sd, ROWOW_SC_AUTOSPELL, 100, skillLevel, skillID, maxlv, 0, SkillGetTime(ROWOW_SKILL_SA_AUTOSPELL, skillLevel));
    return 0;
}

void ROWOWMod::ClifElementalConverterList(Player* sd)
{
    // clif_elementalconverter_list: the Elemental Converters (produce_db's item level 23) the player has the materials for, for the RO_Identity
    // addon to list (ROWOWCONVERTER "<item template>,..."); the one picked comes back as .ro makearrow, as Arrow Crafting's list does
    std::string itemTemplateIDs;
    int32 count = 0;
    for (ROWOWProduce const& produce : ProduceDB)
    {
        if (SkillCanProduceMix(sd, produce.NameID, 23, 1) == 0)
            continue;
        itemTemplateIDs += fmt::format("{}{}", itemTemplateIDs.empty() == true ? "" : ",", ItemTemplateIDsByItemID.at(produce.NameID));
        count++;
    }
    if (count > 0)
    {
        ROWOWIdentity* identity = GetIdentity(sd);
        identity->MenuSkillID = ROWOW_SKILL_SA_CREATECON;
        identity->MenuSkillValue = count;
    }
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_ADDON, nullptr, nullptr, "ROWOWCONVERTER\t" + itemTemplateIDs);
    sd->SendDirectMessage(&data);
}

void ROWOWMod::ClifSkillEstimation(Player* sd, Creature* creature)
{
    // clif_skill_estimation: Sense's window of the monster (level, size, HP, DEF and MDEF by estimation_type 3, race, element, and how much
    // each element hurts it, a negative one shown as 0), as system messages to the caster
    ROWOWStatus const* status = GetROStatus(creature);
    if (status == nullptr)
        return;
    ChatHandler handler(sd->GetSession());
    handler.PSendSysMessage("{} (Lv {}): {}, {}, {} {}", creature->GetName(), status->Level, status->Size < ROWOW_SIZE_COUNT ? SizeNames[status->Size] : "?",
        status->Race <= ROWOW_RACE_DRAGON ? RaceNames[status->Race] : "?", status->DefElement < ROWOW_ELEMENT_COUNT ? ElementNames[status->DefElement] : "?",
        status->ElementLevel);
    handler.PSendSysMessage("HP {} / {}, DEF {}, MDEF {}", creature->GetHealth(), creature->GetMaxHealth(), status->Def + status->Def2, status->MDef + status->MDef2);
    std::string elements;
    for (int32 element = ROWOW_ELEMENT_WATER; element < ROWOW_ELEMENT_COUNT; element++)
        elements += fmt::format("{}{} {}%", elements.empty() == true ? "" : ", ", ElementNames[element],
            std::max<int64>(BattleAttrFix(nullptr, nullptr, 100, element, status->DefElement, status->ElementLevel, 0), 0));
    handler.PSendSysMessage("{}", elements);
}

std::shared_ptr<ROWOWSkillUnitGroup> ROWOWMod::SkillLocateElementField(Unit* bl)
{
    // skill_locate_element_field: the unit's elemental field (Volcano, Deluge, Whirlwind), Land Protector or Suiton, of which it has one at most
    ROWOWSkillUnitData const* ud = GetSkillUnitData(bl);
    for (int32 groupID : ud->SkillUnitGroupIDs)
    {
        std::shared_ptr<ROWOWSkillUnitGroup> group = SkillID2Group(bl->GetMap(), groupID);
        if (group == nullptr)
            continue;
        switch (group->SkillID)
        {
            case ROWOW_SKILL_SA_DELUGE:
            case ROWOW_SKILL_SA_VOLCANO:
            case ROWOW_SKILL_SA_VIOLENTGALE:
            case ROWOW_SKILL_SA_LANDPROTECTOR:
            case ROWOW_SKILL_NJ_SUITON:
                return group;
        }
    }
    return nullptr;
}

int32 ROWOWMod::SkillClearGroup(Unit* bl, int32 flag)
{
    // skill_clear_group: the unit's groups of a kind go: flag&1 the elemental fields, Land Protector, Suiton and Kaensin; flag&2 its traps
    std::vector<int32> groupIDs = GetSkillUnitData(bl)->SkillUnitGroupIDs;
    int32 count = 0;
    for (int32 groupID : groupIDs)
    {
        std::shared_ptr<ROWOWSkillUnitGroup> group = SkillID2Group(bl->GetMap(), groupID);
        if (group == nullptr)
            continue;
        ROWOWSkill const* skill = GetSkill(group->SkillID);
        bool isCleared = false;
        switch (group->SkillID)
        {
            case ROWOW_SKILL_SA_DELUGE:
            case ROWOW_SKILL_SA_VOLCANO:
            case ROWOW_SKILL_SA_VIOLENTGALE:
            case ROWOW_SKILL_SA_LANDPROTECTOR:
            case ROWOW_SKILL_NJ_SUITON:
            case ROWOW_SKILL_NJ_KAENSIN:
                isCleared = (flag & 1) != 0;
                break;
            default:
                isCleared = (flag & 2) != 0 && skill != nullptr && skill->Inf2[ROWOW_INF2_ISTRAP] == true;
                break;
        }
        if (isCleared == true)
        {
            SkillDelUnitGroup(bl->GetMap(), group);
            count++;
        }
    }
    return count;
}

std::shared_ptr<ROWOWSkillUnitGroup> ROWOWMod::MapFindSkillUnitOnCell(Unit* target, int32 x, int32 y, uint16 skillID, int32 flag, int32& indexOut)
{
    // map_find_skill_unit_oncell: a live unit of the skill on the cell of the target's map, with flag&1 one whose targets the target is
    std::shared_ptr<ROWOWSkillUnitGroup> found;
    Map* map = target->GetMap();
    MapForEachSkillUnitInArea(map, x, y, x, y, [this, map, target, skillID, flag, &found, &indexOut](ROWOWSkillUnitGroup& group, int32 index)
    {
        if (found != nullptr || group.SkillID != skillID)
            return 0;
        if ((flag & 1) != 0 && BattleCheckTargetSkillUnit(group, map, target, group.TargetFlag) <= 0)
            return 0;
        found = SkillID2Group(map, group.GroupID);
        indexOut = index;
        return 1;
    });
    return found;
}

void ROWOWMod::ClifSkillPosEffect(Unit* src, uint16 skillID, uint16 skillLevel, int32 x, int32 y, float z)
{
    // clif_skill_poseffect: the skill's look on a cell (each of Meteor Storm's meteors), its spell cast there for the look alone
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(GetSkillSpellID(skillID, skillLevel));
    if (spellInfo == nullptr || src->IsInWorld() == false)
        return;
    float worldX = 0, worldY = 0;
    GetCellWorldPosition(src->GetMapId(), x, y, worldX, worldY);
    SpellCastTargets targets;
    targets.SetDst(worldX, worldY, z, 0, src->GetMapId());
    ROWOWSkillUnitData* ud = GetSkillUnitData(src);
    ud->IsSkillLookCast = true;
    src->CastSpell(targets, spellInfo, nullptr, TRIGGERED_FULL_MASK);
    ud->IsSkillLookCast = false;
}

void ROWOWMod::ClifSkillNoDamage(Unit* src, Unit* target, uint16 skillID, uint16 skillLevel)
{
    // clif_skill_nodamage: the skill's look on a unit (Magic Rod taking a spell in, Soul Drain), its spell cast on it for the look alone.  (A monster's
    // level can pass the skill's spells: the highest below it)
    uint32 spellID = 0;
    for (uint16 level = skillLevel; level > 0 && spellID == 0; level--)
        spellID = GetSkillSpellID(skillID, level);
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellID);
    if (spellInfo == nullptr || src->IsInWorld() == false || target->IsInWorld() == false)
        return;
    ROWOWSkillUnitData* ud = GetSkillUnitData(src);
    ud->IsSkillLookCast = true;
    src->CastSpell(target, spellInfo, TRIGGERED_FULL_MASK);
    ud->IsSkillLookCast = false;
}

bool ROWOWMod::SkillCheckConditionSCRequired(Player* sd, uint16 skillID, ROWOWSkillCondition const& require)
{
    // skill_check_condition_sc_required: each status the skill needs is on the player (the Pushcart has its own message)
    for (int32 type : require.Statuses)
    {
        if (type == ROWOW_SC_NONE || GetSCE(sd, type) != nullptr)
            continue;
        ClifSkillFail(sd, skillID, type == ROWOW_SC_PUSH_CART ? ROWOW_USESKILL_FAIL_CART : ROWOW_USESKILL_FAIL_LEVEL);
        return false;
    }
    return true;
}

int64 ROWOWMod::StatusPercentChange(Unit* src, Unit* target, int8 hpRate, int8 spRate, uint8 flag)
{
    // status_percent_change: HP and SP by a rate of what the target has (positive) or of its maximum (negative, -100 all of it), at least 1;
    // flag 0 heals them, 1 takes them (it can kill), 2 takes them but leaves 1 HP.  Damage with no attacker (or the target's own) is passive
    uint32 hp = 0, sp = 0;
    uint32 curSP = target->GetPower(POWER_MANA), maxSP = target->GetMaxPower(POWER_MANA);
    if (hpRate > 99)
        hp = target->GetHealth();
    else if (hpRate > 0)
        hp = uint32(uint64(target->GetHealth()) * hpRate / 100);
    else if (hpRate < -99)
        hp = target->GetMaxHealth();
    else if (hpRate < 0)
        hp = uint32(uint64(target->GetMaxHealth()) * (-hpRate) / 100);
    if (hpRate != 0 && hp == 0)
        hp = 1;
    if (flag == 2 && hp >= target->GetHealth())
        hp = target->GetHealth() - 1; // Must not kill target.
    if (spRate > 99)
        sp = curSP;
    else if (spRate > 0)
        sp = uint32(uint64(curSP) * spRate / 100);
    else if (spRate < -99)
        sp = maxSP;
    else if (spRate < 0)
        sp = uint32(uint64(maxSP) * (-spRate) / 100);
    if (spRate != 0 && sp == 0)
        sp = 1;
    if (flag == 0)
        return StatusHeal(target, hp, sp, 0, nullptr, 0);
    sp = std::min(sp, curSP);
    hp = std::min(hp, target->GetHealth());
    if (src == nullptr || src == target || hp == 0)
        StatusZap(target, hp, sp);
    else
    {
        StatusZap(target, 0, sp);
        Unit::DealDamage(src, target, hp, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
    }
    return int64(hp) + sp;
}

void ROWOWMod::StatusSetSP(Unit* bl, uint32 sp)
{
    // status_set_sp: the SP set outright, never past the maximum
    if (bl->GetMaxPower(POWER_MANA) > 0)
        bl->SetPower(POWER_MANA, std::min(sp, bl->GetMaxPower(POWER_MANA)));
}

void ROWOWMod::StatusKill(Unit* bl)
{
    // status_kill (status_percent_damage(nullptr, bl, 100, 0, true)): all its HP, with no attacker.  A monster dies with flag 4: mob_dead's no exp, no drops
    if (bl->IsAlive() == false)
        return;
    if (Creature* md = bl->ToCreature(); md != nullptr && GetROMobID(md) != 0)
        GetMobData(md)->IsNoRewards = true;
    StatusZap(bl, bl->GetHealth(), 0);
}

uint32 ROWOWMod::GetMobCreatureEntry(uint32 mobID) const
{
    // The creature template a monster spawns as (its first, when the converter made several)
    uint32 entry = 0;
    for (auto const& mobIDByCreatureEntry : MobIDsByCreatureEntry)
        if (mobIDByCreatureEntry.second == mobID && (entry == 0 || mobIDByCreatureEntry.first < entry))
            entry = mobIDByCreatureEntry.first;
    return entry;
}

uint32 ROWOWMod::MobGetRandomID(std::string const& groupName, bool isDBRate) const
{
    // mob_get_random_id: a random monster of a mob_summon group (RMF_DB_RATE: each kept at its rate in 1000000), tried three times the group's
    // size, else the group's default.  Only monsters that have a creature template here count
    ROWOWDataTable const* summonTable = GetDataTable("mod_rowow_db_mob_summon");
    ROWOWDataTable const* mobTable = GetDataTable(ROWOW_DATA_TABLE_MOB);
    if (summonTable == nullptr || mobTable == nullptr)
        return 0;
    int32 aegisColumnIndex = mobTable->GetColumnIndex("AegisName");
    int32 idColumnIndex = mobTable->GetColumnIndex("Id");
    auto getMobID = [mobTable, aegisColumnIndex, idColumnIndex](std::string const& aegisName)
    {
        for (size_t rowIndex = 0; rowIndex < mobTable->Rows.size(); rowIndex++)
            if (StringEqualI(mobTable->GetCell(rowIndex, aegisColumnIndex), aegisName) == true)
                return uint32(mobTable->GetInt(rowIndex, idColumnIndex, 0));
        return uint32(0);
    };
    int32 groupColumnIndex = summonTable->GetColumnIndex("Group");
    for (size_t rowIndex = 0; rowIndex < summonTable->Rows.size(); rowIndex++)
    {
        if (StringEqualI(summonTable->GetCell(rowIndex, groupColumnIndex), groupName) == false)
            continue;
        std::vector<std::pair<uint32, int32>> list;
        for (auto const& summon : ROWOWDataTable::ParseMapList(summonTable->GetCell(rowIndex, summonTable->GetColumnIndex("Summon"))))
        {
            uint32 mobID = 0;
            int32 rate = 0;
            for (auto const& field : summon)
            {
                if (field.first == "Mob")
                    mobID = getMobID(field.second);
                else if (field.first == "Rate")
                    rate = int32(strtol(field.second.c_str(), nullptr, 10));
            }
            list.emplace_back(mobID, rate);
        }
        for (size_t i = 0, max = list.size() * 3; i < max && list.empty() == false; i++)
        {
            std::pair<uint32, int32> const& entry = list[urand(0, uint32(list.size() - 1))];
            if (entry.first == 0 || GetMobCreatureEntry(entry.first) == 0 || (isDBRate == true && entry.second < 1000000 && entry.second <= int32(urand(0, 999999))))
                continue;
            return entry.first;
        }
        return getMobID(summonTable->GetCell(rowIndex, summonTable->GetColumnIndex("Default")));
    }
    return 0;
}

bool ROWOWMod::MobClassChange(Creature* md, uint32 mobID)
{
    // mob_class_change: the monster becomes another (its creature template, whose level, HP and status the template's selection sets),
    // keeping its HP rate, its Keeping and Barrier ended and its attack stopped
    uint32 entry = GetMobCreatureEntry(mobID);
    if (md->IsInWorld() == false || mobID == 0 || entry == 0 || GetROMobID(md) == mobID)
        return false;
    int64 mobRowIndex = GetMobRowIndex(GetROMobID(md));
    ROWOWDataTable const* mobTable = GetDataTable(ROWOW_DATA_TABLE_MOB);
    if (mobRowIndex >= 0 && mobTable != nullptr && mobTable->GetCell(size_t(mobRowIndex), mobTable->GetColumnIndex("RaceGroups")).find("Treasure") != std::string::npos)
        return false; // Treasure Boxes
    if (ROWOWMobSummon const* summon = md->CustomData.Get<ROWOWMobSummon>("ROWOWMobSummon"); summon != nullptr && summon->AI > ROWOW_AI_ATTACK)
        return false; // Marine Spheres and Floras.
    uint64 hpRate = md->GetMaxHealth() > 0 ? uint64(md->GetHealth()) * 100 / md->GetMaxHealth() : 100;
    StatusChangeEnd(md, ROWOW_SC_KEEPING); // End before calling status_calc_mob().
    StatusChangeEnd(md, ROWOW_SC_BARRIER);
    md->AttackStop();
    md->StopMoving();
    UnitSkillCastCancel(md, 0);
    MobUnlockTarget(md, GetTick());
    if (md->UpdateEntry(entry) == false)
        return false;
    md->SetRegeneratingHealth(false);
    StatusCalcBL(md, std::bitset<ROWOW_SCB_MAX>().set());

    // monster_class_change_full_recover: full HP and a clean damage log, otherwise the HP rate stays
    if (BattleConfig.MonsterClassChangeRecover != 0)
    {
        md->SetHealth(md->GetMaxHealth());
        md->CustomData.Erase("ROWOWDamageLog");
    }
    else
        md->SetHealth(uint32(std::max<uint64>(1, uint64(md->GetMaxHealth()) * hpRate / 100)));

    // Skill delays and targets are cleared on morph
    ROWOWMobData* mob = GetMobData(md);
    mob->SkillDelays.clear();
    mob->SkillIdx = -1;
    mob->TargetID.Clear();
    mob->AttackedID.Clear();
    mob->NormAttackedID.Clear();
    return true;
}

int32 ROWOWMod::PCGetSpiritBall(Unit const* unit) const
{
    // sd->spiritball: only a player has Spirit Spheres
    if (unit == nullptr || unit->IsPlayer() == false)
        return 0;
    ROWOWSkillUnitData const* ud = unit->CustomData.Get<ROWOWSkillUnitData>("ROWOWSkillUnitData");
    return ud != nullptr ? int32(ud->SpiritBallEndTimes.size()) : 0;
}

void ROWOWMod::PCAddSpiritBall(Player* sd, int64 interval, int32 max)
{
    // pc_addspiritball: a sphere for interval ms (a negative interval: one that stays, as @spiritball's), kept in the order the spheres go;
    // at the most (max, never past MAX_SPIRITBALL) the one going soonest makes room
    ROWOWSkillUnitData* ud = GetSkillUnitData(sd);
    if (max > ROWOW_MAX_SPIRITBALL)
        max = ROWOW_MAX_SPIRITBALL;
    std::vector<uint64>& endTimes = ud->SpiritBallEndTimes;
    if (endTimes.empty() == false && int32(endTimes.size()) >= max)
        endTimes.erase(endTimes.begin());
    uint64 endTime = interval < 0 ? std::numeric_limits<uint64>::max() : GetTick() + uint64(interval);
    endTimes.insert(std::upper_bound(endTimes.begin(), endTimes.end(), endTime), endTime);
    SendSpiritBallsToClient(sd);
}

void ROWOWMod::PCDelSpiritBall(Player* sd, int32 count, int32 type)
{
    // pc_delspiritball: the spheres going soonest go first
    ROWOWSkillUnitData* ud = sd->CustomData.Get<ROWOWSkillUnitData>("ROWOWSkillUnitData");
    if (ud == nullptr || ud->SpiritBallEndTimes.empty() == true || count <= 0)
        return;
    count = std::min<int32>(count, int32(ud->SpiritBallEndTimes.size()));
    ud->SpiritBallEndTimes.erase(ud->SpiritBallEndTimes.begin(), ud->SpiritBallEndTimes.begin() + count);
    if (type == 0)
        SendSpiritBallsToClient(sd);
}

void ROWOWMod::UpdateSpiritBalls(Player* sd)
{
    // pc_spiritball_timer: each sphere goes at its time
    ROWOWSkillUnitData* ud = sd->CustomData.Get<ROWOWSkillUnitData>("ROWOWSkillUnitData");
    if (ud == nullptr || ud->SpiritBallEndTimes.empty() == true)
        return;
    uint64 tick = GetTick();
    auto firstLeftIter = std::upper_bound(ud->SpiritBallEndTimes.begin(), ud->SpiritBallEndTimes.end(), tick);
    if (firstLeftIter == ud->SpiritBallEndTimes.begin())
        return;
    ud->SpiritBallEndTimes.erase(ud->SpiritBallEndTimes.begin(), firstLeftIter);
    SendSpiritBallsToClient(sd);
}

void ROWOWMod::SendSpiritBallsToClient(Player* sd)
{
    // clif_spiritball: the RO_Identity addon draws the spheres around the player's frame.  "ROWOWSPHERES<tab><count><tab><ms left of each,
    // soonest first>" (-1 for a sphere that stays)
    ROWOWSkillUnitData const* ud = sd->CustomData.Get<ROWOWSkillUnitData>("ROWOWSkillUnitData");
    std::string timesLeft;
    uint64 tick = GetTick();
    size_t count = ud != nullptr ? ud->SpiritBallEndTimes.size() : 0;
    for (size_t i = 0; i < count; i++)
    {
        uint64 endTime = ud->SpiritBallEndTimes[i];
        if (i != 0)
            timesLeft += ",";
        timesLeft += endTime == std::numeric_limits<uint64>::max() ? "-1" : std::to_string(endTime > tick ? endTime - tick : 0);
    }
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_ADDON, nullptr, nullptr, fmt::format("ROWOWSPHERES\t{}\t{}", count, timesLeft));
    sd->SendDirectMessage(&data);
}

bool ROWOWMod::StatusRevive(Unit* bl, uint8 perHP, uint8 perSP)
{
    // status_revive: a dead player stands again with a share of its max HP (at least 1 with a share) and a share of its max SP added to the SP
    // it kept, then pc_revive: the invincibility after reviving, its statuses shown again.  RO's revive is WoW's resurrection.  (A monster's
    // revive comes with the monsters' skills, R10)
    Player* sd = bl->ToPlayer();
    if (sd == nullptr || sd->IsAlive() == true || IsInROForm(sd) == false)
        return false;
    uint32 maxHP = sd->GetMaxHealth();
    uint32 maxSP = sd->GetMaxPower(POWER_MANA);
    uint32 sp = std::min(GetIdentity(sd)->SP, maxSP); // RO keeps the SP through death (WoW's death empties the power; the identity kept it)
    uint32 hp = uint32(uint64(maxHP) * perHP / 100);
    uint32 addSP = uint32(uint64(maxSP) * perSP / 100);
    if (perHP != 0 && hp == 0)
        hp = 1;
    addSP = std::min(addSP, maxSP - std::min(sp, maxSP));
    if (sd->getDeathState() == DeathState::JustDied)
        sd->KillPlayer();
    sd->ResurrectPlayer(1.0f);
    sd->SetHealth(std::max<uint32>(1, std::min(hp, maxHP)));
    sd->SetPower(POWER_MANA, sp + addSP);
    PCSetInvincibleTimer(sd);
    SendStatusChangesToClient(sd); // The statuses that stay through death show again
    return true;
}

int32 ROWOWMod::SkillCheckPCPartner(Player* sd, uint16 skillID, uint16& skillLevel, int32 range, int32 castFlag)
{
    // skill_check_pc_partner: the partners a skill needs within range cells (skill_check_condition_char_sub), found when the cast starts and
    // kept on the caster for its end (castFlag), when they give their part.  B.S. Sacramenti needs two Acolyte line players standing to the
    // caster's left and right, who give 10 SP each.  An ensemble needs one partner beside the performer: a Bard or Dancer of the other sex in
    // its party, standing (not sitting, free to move, not dancing), holding an instrument or a whip and knowing the ensemble; its level is the
    // average of both (for the search's caller), and once the ensemble is set the partner dances it too (its SC_DANCING names the performer and
    // the same unit group).  With player_skill_partner_check off there are always enough
    if (BattleConfig.PlayerSkillPartnerCheck == 0)
        return 99; // As if there were infinite partners.
    ROWOWSkillUnitData* ud = GetSkillUnitData(sd);
    ROWOWSkill const* skill = GetSkill(skillID);
    if (castFlag != 0)
    {
        // Execute the skill on the partners.
        if (skillID == ROWOW_SKILL_PR_BENEDICTIO)
        {
            for (ObjectGuid const& partnerGUID : ud->PartnerGUIDs)
                if (Player* tsd = ObjectAccessor::GetPlayer(sd->GetMap(), partnerGUID))
                    StatusCharge(tsd, 0, 10);
        }
        else if (skill != nullptr && skill->Inf2[ROWOW_INF2_ISENSEMBLE] == true)
        {
            Player* tsd = ud->PartnerGUIDs.empty() == false ? ObjectAccessor::GetPlayer(sd->GetMap(), ud->PartnerGUIDs[0]) : nullptr;
            ROWOWStatusChange* sc = GetStatusChange(sd);
            if (ROWOWStatusChangeEntry* dancing = sc != nullptr ? sc->GetSCE(ROWOW_SC_DANCING) : nullptr; tsd != nullptr && dancing != nullptr)
            {
                dancing->Val4 = int32(tsd->GetGUID().GetCounter());
                SCStart4(sd, tsd, ROWOW_SC_DANCING, 100, skillID, dancing->Val2, skillLevel, int32(sd->GetGUID().GetCounter()), SkillGetTime(skillID, skillLevel) + 1000);
                ClifSkillNoDamage(tsd, sd, skillID, skillLevel);
                ROWOWSkillUnitData* tud = GetSkillUnitData(tsd);
                tud->SkillIDDance = skillID;
                tud->SkillLevelDance = skillLevel;
            }
        }
        return int32(ud->PartnerGUIDs.size());
    }

    // Else: new search for partners.
    ud->PartnerGUIDs.clear();
    int32 sdX = 0, sdY = 0;
    GetUnitCell(sd, sdX, sdY);
    int32 i = MapForEachInRange(sd, range, [this, sd, skillID, ud, sdX, sdY](Unit* bl)
    {
        // skill_check_condition_char_sub: one companion (B.S. Sacramenti two)
        Player* tsd = bl->ToPlayer();
        if (tsd == nullptr || tsd == sd || ud->PartnerGUIDs.size() >= (skillID == ROWOW_SKILL_PR_BENEDICTIO ? 2u : 1u) || tsd->IsAlive() == false ||
            IsInROForm(tsd) == false)
            return 0;
        ROWOWStatusChange const* tsc = GetStatusChange(static_cast<Unit const*>(tsd));
        if (tsc != nullptr && tsc->Cant.Cast == true)
            return 0;
        if (skillID == ROWOW_SKILL_PR_BENEDICTIO)
        {
            int32 tsdX = 0, tsdY = 0;
            GetUnitCell(tsd, tsdX, tsdY);
            uint8 dir = GetRODirection(float(tsdX - sdX), float(tsdY - sdY));
            dir = (UnitGetDir(sd) + dir) % ROWOW_RO_DIRECTION_COUNT; // This adjusts dir to account for the direction the sd is facing.
            ROWOWJob const* job = GetJob(GetIdentity(tsd)->JobID);
            bool isAcolyte = job != nullptr && (job->NormalJobID == ROWOW_JOB_ACOLYTE || job->JobID == ROWOW_JOB_ACOLYTE_HIGH || job->FirstJobID == ROWOW_JOB_ACOLYTE);
            if (isAcolyte == true && (dir == 2 || dir == 6) && sd->GetPower(POWER_MANA) >= 10) // Must be standing to the left/right of Priest.
                ud->PartnerGUIDs.push_back(tsd->GetGUID());
            return 1;
        }
        // Warning: Assuming Ensemble Dance/Songs for code speed. [Skotlex]
        if (tsd->IsSitState() == true || tsd->HasUnitState(UNIT_STATE_ROOT) == true || (tsc != nullptr && tsc->Cant.Move == true)) // pc_issit, unit_can_move
            return 0;
        ROWOWStatus const* tstatus = GetROStatus(tsd);
        int32 partnerSkillLevel = PCCheckSkill(tsd, skillID);
        if (sd->getGender() != tsd->getGender() && PCIsBardDancer(tsd) == true && partnerSkillLevel > 0 && tstatus != nullptr &&
            (tstatus->WeaponType == ROWOW_WEAPON_TYPE_MUSICAL || tstatus->WeaponType == ROWOW_WEAPON_TYPE_WHIP) && sd->GetGroup() != nullptr &&
            sd->GetGroup() == tsd->GetGroup() && (tsc == nullptr || tsc->HasSCE(ROWOW_SC_DANCING) == false))
        {
            ud->PartnerGUIDs.push_back(tsd->GetGUID());
            return partnerSkillLevel;
        }
        return 0;
    });
    if (skillID != ROWOW_SKILL_PR_BENEDICTIO) // Apply the average lv to encore skills.
        skillLevel = uint16((i + skillLevel) / (int32(ud->PartnerGUIDs.size()) + 1)); // I know c should be one, but this shows how it could be used for the average of n partners.
    return int32(ud->PartnerGUIDs.size());
}

bool ROWOWMod::PCIsBardDancer(Unit const* unit) const
{
    // (class_&MAPID_SECONDMASK) == MAPID_BARDDANCER: a Bard or Dancer, normal, transcendent (Clown, Gypsy) or Baby
    if (unit == nullptr || unit->IsPlayer() == false)
        return false;
    ROWOWIdentity const* identity = unit->ToPlayer()->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    if (identity == nullptr || identity->IsInForm == false)
        return false;
    switch (identity->JobID)
    {
        case ROWOW_JOB_BARD:
        case ROWOW_JOB_DANCER:
        case ROWOW_JOB_CLOWN:
        case ROWOW_JOB_GYPSY:
        case ROWOW_JOB_BABY_BARD:
        case ROWOW_JOB_BABY_DANCER:
            return true;
    }
    return false;
}

bool ROWOWMod::PCIsFalcon(Unit const* unit) const
{
    // pc_isfalcon: a player in the form with its falcon
    if (unit == nullptr || unit->IsPlayer() == false)
        return false;
    ROWOWIdentity const* identity = unit->ToPlayer()->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    return identity != nullptr && identity->IsInForm == true && (identity->Option & ROWOW_OPTION_FALCON) != 0;
}

void ROWOWMod::PCSetFalcon(Player* player, bool isFalcon)
{
    // pc_setfalcon: takes a falcon (only with Falconry Mastery learned) or lets it go.  It's an option kept with the identity, as the Peco is;
    // the falcon changes no status (pc_setoption's only part for it is the EFST_FALCON icon).  It's seen flying as Blitz Beat's and Falcon
    // Assault's projectile, the converter's FALCON effect (the 매 sprite), and on the Hunter's shoulder with its icon (R9.5,
    // RefreshIllusionOptionLooks)
    ROWOWIdentity* identity = GetIdentity(player);
    uint32 option = identity->Option;
    if (isFalcon == true)
    {
        if (PCCheckSkill(*identity, ROWOW_SKILL_HT_FALCON) > 0) // add falcon if he have the skill
            option |= ROWOW_OPTION_FALCON;
    }
    else
        option &= ~ROWOW_OPTION_FALCON; // remove falcon
    if (option == identity->Option)
        return;
    identity->Option = option;
    ChatHandler(player->GetSession()).SendSysMessage((option & ROWOW_OPTION_FALCON) != 0 ? "A falcon is now with you." : "Your falcon has flown away.");
}

void ROWOWMod::SkillRevealTrapInArea(Unit* src, int32 range, int32 x, int32 y)
{
    // skill_reveal_trap_inarea: the hidden traps around a cell show (skill_reveal_trap).  A trap is only hidden with traps_setting (pre-renewal's
    // is 0, and no pre-renewal trap has UF_HIDDENTRAP), so nothing is hidden to reveal unless the battle config says otherwise
    if (BattleConfig.TrapsSetting == 0 || src->IsInWorld() == false)
        return;
    MapForEachSkillUnitInArea(src->GetMap(), x - range, y - range, x + range, y + range, [](ROWOWSkillUnitGroup& group, int32 index)
    {
        ROWOWSkillUnit& unit = group.Units[index];
        ROWOWSkill const* skill = ROWOW->GetSkill(group.SkillID);
        if (unit.IsAlive == false || unit.IsHidden == false || skill == nullptr || skill->Inf2[ROWOW_INF2_ISTRAP] == false)
            return 0;
        unit.IsHidden = false;
        return 1;
    });
}

void ROWOWMod::SkillTrapGiveBack(Unit* src, ROWOWSkillUnitGroup const& group)
{
    // The trap Remove Trap gets back (pc_additem)
    Player* sd = src != nullptr ? src->ToPlayer() : nullptr;
    if (sd == nullptr || IsInROForm(sd) == false)
        return;
    GiveROItem(sd, group.ItemID != 0 ? group.ItemID : ROWOW_ITEMID_TRAP, 1, true);
}

void ROWOWMod::SkillTrapRevert(Map* map, Unit* src, ROWOWSkillUnitGroup const& group, int32 x, int32 y)
{
    // skill_unit_timer_sub's revert: an expired trap of a player turns back into a trap on the ground (map_addflooritem, flags 4), for anyone to take
    if (src == nullptr || src->IsPlayer() == false)
        return;
    auto itemTemplateIDIter = ItemTemplateIDsByItemID.find(group.ItemID != 0 ? group.ItemID : ROWOW_ITEMID_TRAP);
    if (itemTemplateIDIter == ItemTemplateIDsByItemID.end())
        return;
    ROWOWDropItem item;
    item.ItemTemplateID = itemTemplateIDIter->second;
    MapAddFloorItem(map, item, x, y, ObjectGuid::Empty, ObjectGuid::Empty, ObjectGuid::Empty, ROWOW_FLOORITEM_NO_DROP_CHECK, ROWOW_RO_DIRECTION_COUNT, false);
}

void ROWOWMod::SkillExtremityFistEnd(Unit* src, Unit* target, uint16 skillLevel)
{
    // What Asura Strike leaves its user with, once it hit (SkillAsuraStrike::castendDamageId) or failed (skill_castend_id's failure): no SP
    // and no SP regen for its time (SC_EXTREMITYFIST), Fury and Blade Stop over, and the Monk moved 3 cells toward the target (from its cell)
    StatusSetSP(src, 0);
    SCStart(src, src, ROWOW_SC_EXTREMITYFIST, 100, skillLevel, SkillGetTime(ROWOW_SKILL_MO_EXTREMITYFIST, skillLevel));
    StatusChangeEnd(src, ROWOW_SC_EXPLOSIONSPIRITS);
    StatusChangeEnd(src, ROWOW_SC_BLADESTOP);
    if (target == nullptr)
        return;
    int32 srcX = 0, srcY = 0, targetX = 0, targetY = 0;
    GetUnitCell(src, srcX, srcY);
    GetUnitCell(target, targetX, targetY);
    uint8 dir = GetRODirection(float(targetX - srcX), float(targetY - srcY)); // map_calc_dir
    int32 i = 3; // Move 3 cells (From caster)
    int32 x = 0, y = 0;
    if (dir > 0 && dir < 4)
        x = -i;
    else if (dir > 4)
        x = i;
    if (dir > 2 && dir < 6)
        y = -i;
    else if (dir == 7 || dir < 2)
        y = i;
    if (UnitMovePos(src, srcX + x, srcY + y) == true && src->IsPlayer() == true)
        SendSpiritBallsToClient(src->ToPlayer()); // clif_spiritball
}

uint32 const ROWOWMod::FloraSummonMobIDs[5] = { ROWOW_MOBID_G_MANDRAGORA, ROWOW_MOBID_G_HYDRA, ROWOW_MOBID_G_FLORA, ROWOW_MOBID_G_PARASITE, ROWOW_MOBID_G_GEOGRAPHER };

Creature* ROWOWMod::MobOnceSpawnSub(Unit* src, int32 x, int32 y, uint32 mobID, uint8 ai, int64 duration)
{
    // mob_once_spawn_sub and mob_spawn: the monster on the RO cell, gone after the duration (its deletetimer: a timed summon of the caster's).
    // One with an AI (AI_FLORA, AI_SPHERE) answers to the caster (master_id) and is in its summon list.  A plant takes the caster's faction,
    // so it can fight the caster's enemies; a Marine Sphere is neutral instead, as only players hitting it (alchemist_summon_setting) set it
    // off, and WoW lets no one attack a unit of their own side
    uint32 entry = GetMobCreatureEntry(mobID);
    if (entry == 0)
    {
        LOG_ERROR("module.ROWOW", "ROWOWMod::MobOnceSpawnSub: monster {} has no creature template (run the converter)", mobID);
        return nullptr;
    }
    if (src->IsInWorld() == false)
        return nullptr;
    // The ground of the cell near the caster's height (the highest ground can be terrain hidden above an RO map's floor)
    float cellX = 0, cellY = 0;
    GetCellWorldPosition(src->GetMapId(), x, y, cellX, cellY);
    float cellZ = src->GetMap()->GetHeight(src->GetPhaseMask(), cellX, cellY, src->GetPositionZ() + ROWOW_SKILL_CELL_GROUND_SEARCH_HEIGHT, true,
        ROWOW_SKILL_CELL_GROUND_SEARCH_DISTANCE);
    if (cellZ <= INVALID_HEIGHT)
        return nullptr;
    // A duration of 0 is a monster with no deletetimer: it stays until it dies, then its body goes as a spawned one's
    TempSummon* md = src->SummonCreature(entry, cellX, cellY, cellZ, src->GetOrientation(), duration > 0 ? TEMPSUMMON_TIMED_DESPAWN : TEMPSUMMON_CORPSE_TIMED_DESPAWN,
        duration > 0 ? uint32(duration) : ConfigCorpseTimeInMS);
    if (md == nullptr)
        return nullptr;
    if (ai != ROWOW_AI_NONE)
    {
        ROWOWMobSummon* summon = md->CustomData.GetDefault<ROWOWMobSummon>("ROWOWMobSummon");
        summon->MasterGUID = src->GetGUID();
        summon->AI = ai;
        md->SetFaction(ai == ROWOW_AI_SPHERE ? uint32(FACTION_CREATURE) : src->GetFaction());
        GetSkillUnitData(src)->SummonGUIDs.push_back(md->GetGUID());
    }
    return md;
}

Unit const* ROWOWMod::BattleGetMaster(Unit const* bl) const
{
    // battle_get_master: a monster a skill summoned answers to its master, while the master is on its map
    if (Player const* master = GetFollowerMaster(bl))
        return master; // A homunculus's or mercenary's
    ROWOWMobSummon const* summon = bl->IsCreature() == true ? bl->CustomData.Get<ROWOWMobSummon>("ROWOWMobSummon") : nullptr;
    if (summon == nullptr || summon->MasterGUID.IsEmpty() == true)
        return bl;
    Unit const* master = ObjectAccessor::GetUnit(*bl, summon->MasterGUID);
    return master != nullptr && master->IsInWorld() == true ? master : bl;
}

int32 ROWOWMod::SkillCheckConditionMobMaster(Player* sd, uint16 skillID, uint32 mobClass, int32& countOut)
{
    // skill_check_condition_mob_master_sub over the map: the player's summons of the skill's AI (AI_SPHERE for Summon Marine Sphere, AI_FLORA
    // for Summon Flora) on its map, and in countOut those of the monster asked for.  Summons gone or elsewhere leave the list
    uint8 ai = skillID == ROWOW_SKILL_AM_SPHEREMINE ? ROWOW_AI_SPHERE : ROWOW_AI_FLORA;
    std::vector<ObjectGuid>& summonGUIDs = GetSkillUnitData(sd)->SummonGUIDs;
    int32 i = 0;
    countOut = 0;
    for (auto summonGUIDIter = summonGUIDs.begin(); summonGUIDIter != summonGUIDs.end();)
    {
        Creature* md = ObjectAccessor::GetCreature(*sd, *summonGUIDIter);
        ROWOWMobSummon const* summon = md != nullptr ? md->CustomData.Get<ROWOWMobSummon>("ROWOWMobSummon") : nullptr;
        if (md == nullptr || md->IsAlive() == false || summon == nullptr)
        {
            summonGUIDIter = summonGUIDs.erase(summonGUIDIter);
            continue;
        }
        ++summonGUIDIter;
        if (summon->AI != ai)
            continue; // Non alchemist summoned mobs have nothing to do here.
        if (GetROMobID(md) == mobClass)
            countOut++;
        i++;
    }
    return i;
}

uint32 ROWOWMod::ItemGroupRandomDropItem(std::string const& groupName) const
{
    // itemdb_group.get_random_entry(group, 1, GROUP_ALGORITHM_DROP), the group by its name (with or without IG_, in any case).  We pick a random item from the
    // group and then do a drop check based on the rate (item_group_rate's).  On fail, do not return any item
    std::string name = groupName;
    std::transform(name.begin(), name.end(), name.begin(), ::toupper);
    if (name.rfind("IG_", 0) == 0)
        name = name.substr(3);
    auto groupIter = ItemGroupsByName.find(name);
    if (groupIter == ItemGroupsByName.end())
        return 0;
    auto subGroupIter = groupIter->second.SubGroupsByID.find(1);
    if (subGroupIter == groupIter->second.SubGroupsByID.end() || subGroupIter->second.Entries.empty() == true)
        return 0;
    ROWOWItemGroupEntry const& entry = subGroupIter->second.Entries[urand(0, uint32(subGroupIter->second.Entries.size() - 1))];
    int32 rate = std::clamp<int32>(int32(entry.Rate) * BattleConfig.ItemGroupRate / 100, BattleConfig.ItemGroupDropMin, BattleConfig.ItemGroupDropMax);
    return int32(urand(0, 9999)) < rate ? entry.ItemID : 0;
}

int8 ROWOWMod::SkillIsCopyable(Player* sd, uint16 skillID) const
{
    // skill_isCopyable: a skill the Rogue doesn't have on its tree (its own copy aside, which is never on it), copyable by Plagiarism, while it
    // knows Plagiarism and Preserve isn't on.  (Reproduce is a third job's)
    ROWOWIdentity const* identity = sd->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    ROWOWSkill const* skill = GetSkill(skillID);
    if (identity == nullptr || skill == nullptr)
        return 0;
    // Only copy skill that player doesn't have or the skill is old clone
    if (identity->TreeSkillIDs.find(skillID) != identity->TreeSkillIDs.end())
        return 0;
    // Plagiarism only able to copy skill while SC_PRESERVE is not active and skill is copyable by Plagiarism
    if ((skill->CopyOption & ROWOW_SKILL_COPY_PLAGIARISM) != 0 && PCCheckSkill(sd, ROWOW_SKILL_RG_PLAGIARISM) > 0 && GetSCE(sd, ROWOW_SC_PRESERVE) == nullptr)
        return 1;
    return 0;
}

void ROWOWMod::SkillDoCopy(Unit* /*src*/, Unit* bl, uint16 skillID, uint16 skillLevel)
{
    // skill_do_copy: a Rogue hit by a skill it can copy keeps it in place of its last copy, at the hit's level up to its Plagiarism level, in
    // the CLONE_SKILL variables (saved with the identity); the skill's spells and the skill window follow (clif_addskill).  (Reproduce and
    // the third jobs' dummy skills, skill_dummy2skill_id, are left out)
    Player* tsd = bl->ToPlayer();
    if (tsd == nullptr || PCCheckSkill(tsd, ROWOW_SKILL_RG_PLAGIARISM) == 0)
        return;
    // If SC_PRESERVE is active, nothing to do
    if (GetSCE(tsd, ROWOW_SC_PRESERVE) != nullptr)
        return;
    if (SkillIsCopyable(tsd, skillID) != 1)
        return;
    ROWOWIdentity* identity = GetIdentity(tsd);
    PCSkillPlagiarismReset(tsd);
    // Cap level to RG_PLAGIARISM level
    int32 level = std::min<int32>(skillLevel, PCCheckSkill(tsd, ROWOW_SKILL_RG_PLAGIARISM));
    SetIdentityVariable(*identity, ROWOW_SKILL_VAR_PLAGIARISM, skillID);
    SetIdentityVariable(*identity, ROWOW_SKILL_VAR_PLAGIARISM_LV, level);
    RefreshIdentitySkills(tsd, *identity);
    SendSkillsToClient(tsd, *identity, false);
}

void ROWOWMod::PCSkillPlagiarismReset(Player* sd)
{
    // pc_skill_plagiarism_reset (Plagiarism's): the copied skill and its spells go (clif_deleteskill), and its variables
    ROWOWIdentity* identity = GetIdentity(sd);
    if (identity->CloneSkillID == 0)
        return;
    identity->CloneSkillID = 0;
    identity->CloneSkillLevel = 0;
    SetIdentityVariable(*identity, ROWOW_SKILL_VAR_PLAGIARISM, 0);
    SetIdentityVariable(*identity, ROWOW_SKILL_VAR_PLAGIARISM_LV, 0);
    GrantIdentitySkillSpells(sd, *identity);
}

bool ROWOWMod::SkillStripEquip(Unit* src, Unit* target, uint16 skillID, uint16 skillLevel)
{
    // skill_strip_equip: the chance (in 1000, by the DEX difference), the time (a monster's 15 seconds longer), then the strip status of each
    // place Chemical Protection doesn't cover; true when one landed (the status takes the player's item off, or fails when nothing is worn
    // there).  (Mado Gear, the accessories and the shadow equipment are the third jobs')
    ROWOWStatus const* sstatus = GetROStatus(src);
    ROWOWStatus const* tstatus = GetROStatus(target);
    if (sstatus == nullptr || tstatus == nullptr)
        return false;
    int32 const scAtk[4] = { ROWOW_SC_STRIPWEAPON, ROWOW_SC_STRIPSHIELD, ROWOW_SC_STRIPARMOR, ROWOW_SC_STRIPHELM };
    int32 const scDef[4] = { ROWOW_SC_CP_WEAPON, ROWOW_SC_CP_SHIELD, ROWOW_SC_CP_ARMOR, ROWOW_SC_CP_HELM };
    int32 dexDifference = sstatus->Stats[ROWOW_STAT_DEX] - tstatus->Stats[ROWOW_STAT_DEX];
    int32 rate = 0;
    int32 mod = 100;
    switch (skillID) // Rate
    {
        case ROWOW_SKILL_RG_STRIPWEAPON:
        case ROWOW_SKILL_RG_STRIPARMOR:
        case ROWOW_SKILL_RG_STRIPSHIELD:
        case ROWOW_SKILL_RG_STRIPHELM:
            rate = 50 * (skillLevel + 1) + 2 * dexDifference;
            mod = 1000;
            break;
        case ROWOW_SKILL_ST_FULLSTRIP:
        {
            int32 minRate = 50 + 20 * skillLevel;
            rate = std::max(minRate, minRate + 2 * dexDifference);
            mod = 1000;
            break;
        }
        default:
            return false;
    }
    if (int32(urand(0, uint32(mod - 1))) >= rate)
        return false;

    // Duration
    int64 time = SkillGetTime(skillID, skillLevel);
    if (target->IsPlayer() == true)
        time += std::max(1, skillLevel + 500 * dexDifference);
    else
    {
        time += 15000;
        time += std::max(1, skillLevel + 500 * dexDifference);
    }

    // Location: EQP_WEAPON, EQP_SHIELD, EQP_ARMOR, EQP_HELM as the bits of scAtk's order
    int32 location = 0;
    switch (skillID)
    {
        case ROWOW_SKILL_RG_STRIPWEAPON: location = 0x1; break;
        case ROWOW_SKILL_RG_STRIPSHIELD: location = 0x2; break;
        case ROWOW_SKILL_RG_STRIPARMOR: location = 0x4; break;
        case ROWOW_SKILL_RG_STRIPHELM: location = 0x8; break;
        case ROWOW_SKILL_ST_FULLSTRIP: location = 0xF; break;
    }
    for (int32 i = 0; i < 4; i++)
        if ((location & (1 << i)) != 0 && GetSCE(target, scDef[i]) != nullptr)
            location &= ~(1 << i);
    if (location == 0)
        return false;
    for (int32 i = 0; i < 4; i++)
        if ((location & (1 << i)) != 0 && SCStart(src, target, scAtk[i], 100, skillLevel, time) == false)
            location &= ~(1 << i);
    return location != 0;
}

bool ROWOWMod::IsUnitWarpForbidden(Unit const* bl) const
{
    // unit_warp's map flags: noteleport for a player, monster_noteleport for a monster no one summoned
    auto isMapFlagged = [this, bl](char const* flag)
    {
        return HasMapFlag(bl, flag);
    };
    if (bl->IsPlayer() == true)
        return isMapFlagged("noteleport");
    ROWOWMobSummon const* summon = bl->CustomData.Get<ROWOWMobSummon>("ROWOWMobSummon");
    return isMapFlagged("monster_noteleport") == true && (summon == nullptr || summon->MasterGUID.IsEmpty() == true);
}

void ROWOWMod::UnitRemoveMap(Unit* bl)
{
    // unit_remove_map's monster part: its walk stopped, its target dropped (a slave keeps its own: mob_slave_keep_target), idle
    Creature* md = bl->ToCreature();
    if (md == nullptr || GetROMobID(md) == 0)
        return;
    ROWOWMobData* mob = GetMobData(md);
    UnitStopWalking(md);
    if (BattleConfig.MobSlaveKeepTarget == 0)
        mob->TargetID.Clear();
    mob->SpottedLog.clear();
    mob->AttackedID.Clear();
    MobSetState(md, mob, ROWOW_MSS_IDLE);
}

bool ROWOWMod::UnitWarpRandom(Unit* bl, Position& destOut)
{
    // unit_warp(bl, -1, -1, -1, CLR_TELEPORT): to a random spot of its map (a stitched map's part it's in), unless the map forbids it.  A player's
    // teleport lands once its client answers, so where it goes is given back for those warped next to it
    uint32 mapKey = GetUnitMapKey(bl);
    if (IsUnitWarpForbidden(bl) == true || MapSearchRandomSpot(bl->GetMap(), mapKey, destOut) == false)
        return false;
    if (mapKey != bl->GetMapId() && bl->IsPlayer() == true && IsInROForm(bl->ToPlayer()) == true)
        GetIdentity(bl->ToPlayer())->IsArrivalFloorCheckPending = true; // A part's spot is a cell's ground (GetCellPosition), as a warp's
    UnitRemoveMap(bl);
    bl->NearTeleportTo(destOut.GetPositionX(), destOut.GetPositionY(), destOut.GetPositionZ(), bl->GetOrientation());
    return true;
}

bool ROWOWMod::UnitWarpNear(Unit* bl, Position const& center)
{
    // unit_warp to map_search_freecell(center, 0, x, y, 1, 1, 0): a random cell within one of the spot's cell with ground near its height
    // (the spot itself when none is found), unless the map forbids it
    if (IsUnitWarpForbidden(bl) == true)
        return false;
    Map* map = bl->GetMap();
    int32 centerX = 0, centerY = 0;
    GetWorldCell(bl->GetMapId(), center.GetPositionX(), center.GetPositionY(), centerX, centerY);
    float x = center.GetPositionX();
    float y = center.GetPositionY();
    float z = center.GetPositionZ();
    for (int32 tries = 0; tries < 9; tries++)
    {
        float cellX = 0, cellY = 0;
        GetCellWorldPosition(bl->GetMapId(), centerX + int32(urand(0, 2)) - 1, centerY + int32(urand(0, 2)) - 1, cellX, cellY);
        float cellZ = map->GetHeight(bl->GetPhaseMask(), cellX, cellY, center.GetPositionZ() + ROWOW_SKILL_CELL_GROUND_SEARCH_HEIGHT, true, ROWOW_SKILL_CELL_GROUND_SEARCH_DISTANCE);
        if (cellZ <= INVALID_HEIGHT || std::fabs(cellZ - center.GetPositionZ()) > ROWOW_SKILL_CELL_GROUND_SEARCH_HEIGHT)
            continue;
        x = cellX;
        y = cellY;
        z = cellZ + 0.5f;
        break;
    }
    UnitRemoveMap(bl);
    bl->NearTeleportTo(x, y, z, bl->GetOrientation());
    return true;
}

void ROWOWMod::SkillSit(Player* sd, bool sitting)
{
    // skill_sit: a Rogue (Gangster's Paradise) sitting down beside another sitting one (its splash around it) puts every such Rogue there in
    // Gangster's Paradise; standing up takes it off those who no longer have two sitting together around them.  (The Taekwon's rests, flag
    // 2, are SkillTaekwonRest's)
    int32 level = PCCheckSkill(sd, ROWOW_SKILL_RG_GANGSTER);
    if (level <= 0)
        return; // No need to count area if no skills are learned.
    int32 range = SkillGetSplash(ROWOW_SKILL_RG_GANGSTER, uint16(level));
    // skill_sit_count: the sitting players around who know Gangster's Paradise
    auto sitCount = [this, range](Player* center)
    {
        return MapForEachInRange(center, range, [this](Unit* bl)
        {
            Player* tsd = bl->ToPlayer();
            return (tsd != nullptr && tsd->IsSitState() == true && PCCheckSkill(tsd, ROWOW_SKILL_RG_GANGSTER) > 0) ? 1 : 0;
        });
    };
    if (sitting == true)
    {
        if (sitCount(sd) <= 1)
            return;
        // skill_sit_in
        MapForEachInRange(sd, range, [this](Unit* bl)
        {
            Player* tsd = bl->ToPlayer();
            if (tsd != nullptr && tsd->IsSitState() == true && PCCheckSkill(tsd, ROWOW_SKILL_RG_GANGSTER) > 0)
                GetIdentity(tsd)->IsGangsterParadise = true;
            return 0;
        });
        return;
    }
    // skill_sit_out
    MapForEachInRange(sd, range, [this, &sitCount](Unit* bl)
    {
        Player* tsd = bl->ToPlayer();
        if (tsd == nullptr || sitCount(tsd) > 1)
            return 0;
        GetIdentity(tsd)->IsGangsterParadise = false;
        return 0;
    });
}

void ROWOWMod::UpdateSkillSit(Player* sd)
{
    // clif_parse_ActionRequest's sit and stand (and the other ways a player sits or stands: a vendor, a hit, death), from the player's update
    ROWOWIdentity* identity = GetIdentity(sd);
    bool sitting = sd->IsSitState();
    if (sitting == identity->IsSitting)
        return;
    identity->IsSitting = sitting;
    SkillSit(sd, sitting);
    SkillTaekwonRest(sd, sitting);
}

bool ROWOWMod::MobCanTarget(Creature const* md, Unit const* target) const
{
    // mob_ai_sub_hard_activesearch and mob_ai_sub_hard's target checks: a Rogue in Gangster's Paradise isn't a target for a monster that isn't
    // status immune
    ROWOWStatus const* status = GetROStatus(md);
    if (status == nullptr || target == nullptr || target->IsPlayer() == false)
        return true;
    ROWOWIdentity const* identity = target->ToPlayer()->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    if (identity != nullptr && identity->IsInForm == true && identity->IsGangsterParadise == true && (status->Mode & ROWOW_MODE_STATUS_IMMUNE) == 0)
        return false; // Gangster paradise protection.
    return true;
}

namespace
{
    void SendGraffitiMessage(Player* player, std::string const& addonMessage)
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_ADDON, nullptr, nullptr, addonMessage);
        player->SendDirectMessage(&data);
    }
}

void ROWOWMod::ClifGraffiti(Map* map, ROWOWSkillUnitGroup& group)
{
    // clif_graffiti: RO writes the message on the ground, which the RO_Identity addon does (R9.5): a player gets it once the Graffiti comes
    // into its view (area_size cells around it), as "ROWOWGRAF\tA\t<group>\t<x>\t<y>\t<z>\t<ms left>\t<message>", and "ROWOWGRAF\tD\t<group>"
    // when it goes out of view (or the Graffiti goes, ClifGraffitiClear)
    if (group.Units.empty() == true || group.Units[0].IsAlive == false)
        return;
    ROWOWSkillUnit const& unit = group.Units[0];
    float x = 0;
    float y = 0;
    if (GetCellWorldPosition(map->GetId(), unit.X, unit.Y, x, y) == false)
        return;
    uint64 tick = GetTick();
    uint64 endTick = group.Tick + uint64(std::max<int64>(0, group.Limit));
    std::string addMessage = fmt::format("ROWOWGRAF\tA\t{}\t{:.2f}\t{:.2f}\t{:.2f}\t{}\t{}", group.GroupID, x, y, unit.Z, endTick > tick ? endTick - tick : 0,
        group.ValStr);
    std::vector<ObjectGuid> viewerGUIDs;
    MapForEachInCellArea(map, PHASEMASK_NORMAL, unit.X - BattleConfig.AreaSize, unit.Y - BattleConfig.AreaSize, unit.X + BattleConfig.AreaSize,
        unit.Y + BattleConfig.AreaSize, [&group, &viewerGUIDs, &addMessage](Unit* bl)
    {
        Player* sd = bl->ToPlayer();
        if (sd == nullptr)
            return 0;
        viewerGUIDs.push_back(sd->GetGUID());
        if (std::find(group.ViewerGUIDs.begin(), group.ViewerGUIDs.end(), sd->GetGUID()) == group.ViewerGUIDs.end())
            SendGraffitiMessage(sd, addMessage);
        return 0;
    });
    // (A viewer gone to another map only gets a packet, which any thread may send)
    std::string clearMessage = fmt::format("ROWOWGRAF\tD\t{}", group.GroupID);
    for (ObjectGuid const& viewerGUID : group.ViewerGUIDs)
        if (std::find(viewerGUIDs.begin(), viewerGUIDs.end(), viewerGUID) == viewerGUIDs.end())
            if (Player* viewer = ObjectAccessor::FindPlayer(viewerGUID))
                SendGraffitiMessage(viewer, clearMessage);
    group.ViewerGUIDs = viewerGUIDs;
}

void ROWOWMod::ClifGraffitiClear(ROWOWSkillUnitGroup& group)
{
    // The Graffiti's message off the ground of everyone who saw it (only a packet each, which any thread may send)
    std::string clearMessage = fmt::format("ROWOWGRAF\tD\t{}", group.GroupID);
    for (ObjectGuid const& viewerGUID : group.ViewerGUIDs)
        if (Player* viewer = ObjectAccessor::FindPlayer(viewerGUID))
            SendGraffitiMessage(viewer, clearMessage);
    group.ViewerGUIDs.clear();
}

void ROWOWMod::ClifGospelInfo(Unit* bl, int32 type)
{
    // clif_gospel_info: ZC_SKILLMSG, which the client shows as its message string 868 + type (msgstringtable.txt, the iRO client's English)
    Player* sd = bl != nullptr ? bl->ToPlayer() : nullptr;
    if (sd == nullptr)
        return;
    char const* message = nullptr;
    switch (type)
    {
        case 0x15: message = "All abnormal status effects have been removed."; break;
        case 0x16: message = "You will be immune to abnormal status effects for the next minute."; break;
        case 0x17: message = "Your Max HP will stay increased for the next minute."; break;
        case 0x18: message = "Your Max SP will stay increased for the next minute."; break;
        case 0x19: message = "All of your Stats will stay increased for the next minute."; break;
        case 0x1c: message = "Your weapon will remain blessed with Holy power for the next minute."; break;
        case 0x1d: message = "Your armor will remain blessed with Holy power for the next minute."; break;
        case 0x1e: message = "Your Defense will stay increased for the next 10 seconds."; break;
        case 0x1f: message = "Your Attack strength will be increased for the next minute."; break;
        case 0x20: message = "Your Accuracy and Flee Rate will be increased for the next minute."; break;
        default: return;
    }
    ChatHandler(sd->GetSession()).SendSysMessage(message);
}

int32 ROWOWMod::MapCountOnCell(Unit* origin, int32 x, int32 y) const
{
    // map_count_oncell(m, x, y, BL_PC | BL_NPC | BL_MOB, 0): the living RO combatants standing on a cell of the origin's map
    return MapForEachInCellArea(origin->GetMap(), origin->GetPhaseMask(), x, y, x, y, [](Unit* bl) { return bl->IsAlive() == true ? 1 : 0; });
}

bool ROWOWMod::MapSearchFreeCell(Unit* center, int32 range, int32& xOut, int32& yOut) const
{
    // map_search_freecell(center, 0, &x, &y, range, range, 0): a random walkable cell within range of the unit's cell, tried a number of times
    ROWOWMapGrid const* mapGrid = GetMapGrid(center->GetMapId());
    if (mapGrid == nullptr)
        return false;
    int32 centerX = 0, centerY = 0;
    GetUnitCell(center, centerX, centerY);
    int32 tries = (range * 2 + 1) * (range * 2 + 1);
    if (tries > 100)
        tries = 100;
    while (tries-- > 0)
    {
        int32 x = centerX + int32(urand(0, uint32(range * 2))) - range;
        int32 y = centerY + int32(urand(0, uint32(range * 2))) - range;
        if (IsCellNoPass(center, *mapGrid, x, y) == false)
        {
            xOut = x;
            yOut = y;
            return true;
        }
    }
    return false;
}

bool ROWOWMod::UnitRun(Unit* bl)
{
    // unit_run: while it has SC_RUN the unit moves on the way the status keeps (val2), as far as the walkable cells with no one on them go
    // (AREA_SIZE at most); when that's no more than a cell it ran into something (unit_run_hit).  WoW moves it there as one quick move at its
    // walk speed (SC_RUN's), and UpdateUnitRun runs it on from there
    ROWOWStatusChangeEntry const* sce = GetSCE(bl, ROWOW_SC_RUN);
    ROWOWMapGrid const* mapGrid = GetMapGrid(bl->GetMapId());
    if (sce == nullptr || mapGrid == nullptr)
        return false;
    if (GetStatusChange(static_cast<Unit const*>(bl))->Cant.Move == true || bl->HasUnitState(UNIT_STATE_ROOT) == true)
    {
        StatusChangeEnd(bl, ROWOW_SC_RUN);
        return false;
    }
    static int32 const dirx[] = { 0, -1, -1, -1, 0, 1, 1, 1 };
    static int32 const diry[] = { 1, 1, 0, -1, -1, -1, 0, 1 };
    int32 dir = sce->Val2 & 7;
    int32 dirX = dirx[dir];
    int32 dirY = diry[dir];

    // Determine destination cell
    int32 blX = 0, blY = 0;
    GetUnitCell(bl, blX, blY);
    int32 toX = blX, toY = blY;
    // Search for available path
    for (int32 i = 0; i < ROWOW_AREA_SIZE; i++)
    {
        if (IsCellNoPass(bl, *mapGrid, toX + dirX, toY + dirY) == true)
            break;
        // If sprinting and there's a PC/Mob/NPC, block the path [Kevin]
        if (MapCountOnCell(bl, toX + dirX, toY + dirY) != 0)
            break;
        toX += dirX;
        toY += dirY;
    }

    // Can't run forward.
    if ((toX == blX && toY == blY) || (toX == (blX + 1) || toY == (blY + 1)) || (toX == (blX - 1) || toY == (blY - 1)))
    {
        UnitRunHit(bl);
        return false;
    }
    float x = 0, y = 0;
    GetCellWorldPosition(bl->GetMapId(), toX, toY, x, y);
    float z = bl->GetMap()->GetHeight(bl->GetPhaseMask(), x, y, bl->GetPositionZ() + ROWOW_SKILL_CELL_GROUND_SEARCH_HEIGHT, true, ROWOW_SKILL_CELL_GROUND_SEARCH_DISTANCE);
    if (z <= INVALID_HEIGHT)
    {
        UnitRunHit(bl);
        return false;
    }
    bl->GetMotionMaster()->MoveCharge(x, y, z, bl->GetSpeed(MOVE_RUN));
    return true;
}

void ROWOWMod::UnitRunHit(Unit* bl)
{
    // unit_run_hit: if you can't run forward, you must be next to a wall, so bounce back. [Skotlex]  Set running to 0 beforehand so
    // status_change_end knows not to enable spurt [Kevin]
    ROWOWStatusChangeEntry const* sce = GetSCE(bl, ROWOW_SC_RUN);
    if (sce == nullptr)
        return;
    int32 lv = sce->Val1;
    int8 dir = int8(sce->Val2 & 7);
    GetSkillUnitData(bl)->IsRunning = false;
    StatusChangeEnd(bl, ROWOW_SC_RUN);
    ROWOWSkill const* skill = GetSkill(ROWOW_SKILL_TK_RUN);
    SkillBlown(bl, bl, skill != nullptr ? ROWOWSkill::GetLevelValue(skill->Knockback, uint16(lv)) : 0, dir, ROWOW_BLOWN_NONE);
}

void ROWOWMod::UpdateUnitRun(Unit* bl)
{
    // unit_walktoxy_timer: once the run's move ended, keep trying to run
    ROWOWSkillUnitData* ud = GetSkillUnitData(bl);
    if (ud->IsRunning == false || bl->movespline->Finalized() == false)
        return;
    if (GetSCE(bl, ROWOW_SC_RUN) == nullptr || UnitRun(bl) == false)
        ud->IsRunning = false;
}

void ROWOWMod::StatusSetHP(Unit* bl, uint32 hp)
{
    // status_set_hp: the HP set outright (at least 1, never past the maximum)
    if (bl->IsAlive() == true)
        bl->SetHealth(std::clamp<uint32>(hp, 1, bl->GetMaxHealth()));
}

uint32 ROWOWMod::PCGetFeelMap(ROWOWIdentity const& identity, int32 pos) const
{
    static char const* const feelVars[ROWOW_MAX_PC_FEELHATE] = ROWOW_SG_FEEL_VARS;
    return pos >= 0 && pos < ROWOW_MAX_PC_FEELHATE ? uint32(GetIdentityVariable(identity, feelVars[pos])) : 0;
}

int32 ROWOWMod::PCGetHateMob(ROWOWIdentity const& identity, int32 pos) const
{
    // The variable holds the class + 1 (0 for none, rAthena's -1)
    static char const* const hateVars[ROWOW_MAX_PC_FEELHATE] = ROWOW_SG_HATE_VARS;
    return pos >= 0 && pos < ROWOW_MAX_PC_FEELHATE ? int32(GetIdentityVariable(identity, hateVars[pos])) - 1 : -1;
}

bool ROWOWMod::PCSetHateMob(Player* sd, int32 pos, Unit* bl)
{
    // pc_set_hate_mob: a hatred can't be changed (it's only shown).  A monster must be of the size of the place (small sun, medium moon, large
    // star) and, for the moon and star, have 6000 or 20000 HP; a player (its job) needs neither
    static char const* const hateVars[ROWOW_MAX_PC_FEELHATE] = ROWOW_SG_HATE_VARS;
    if (bl == nullptr || pos < 0 || pos >= ROWOW_MAX_PC_FEELHATE)
        return false;
    ROWOWIdentity& identity = *GetIdentity(sd);
    if (PCGetHateMob(identity, pos) != -1)
    {
        // Can't change hate targets.
        ClifHateInfo(sd, pos, PCGetHateMob(identity, pos), 0); // Display current
        return false;
    }
    int32 hateClass = 0;
    if (Creature* md = bl->ToCreature())
    {
        ROWOWStatus const* status = GetROStatus(bl);
        hateClass = int32(GetROMobID(md));
        if (hateClass == 0 || status == nullptr)
            return false;
        if ((pos == 1 && status->MaxHP < 6000) || (pos == 2 && status->MaxHP < 20000))
            return false;
        if (pos != status->Size)
            return false; // Wrong size
    }
    else if (Player* tsd = bl->ToPlayer())
        hateClass = GetIdentity(tsd)->JobID;
    else
        return false;
    SetIdentityVariable(identity, hateVars[pos], hateClass + 1);
    ClifHateInfo(sd, pos, hateClass, 1);
    return true;
}

void ROWOWMod::ClifFeelReq(Player* sd, uint16 skillLevel)
{
    // clif_feel_req (ZC_STARPLACE): RO asks the Star Gladiator to confirm the place, a choice it can't undo.  The RO_Identity addon's menu
    // (ROWOWFEEL "<level>:<map name>") answers with .ro feel
    ROWOWIdentity* identity = GetIdentity(sd);
    identity->MenuSkillID = ROWOW_SKILL_SG_FEEL;
    identity->MenuSkillValue = skillLevel;
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_ADDON, nullptr, nullptr,
        fmt::format("ROWOWFEEL\t{}:{}", skillLevel, GetMapNameOfMapKey(GetUnitMapKey(sd))));
    sd->SendDirectMessage(&data);
}

void ROWOWMod::ClifParseFeelSaveOk(Player* sd)
{
    // clif_parse_FeelSaveOk: the map the Star Gladiator stands on becomes the place the menu asked about
    static char const* const feelVars[ROWOW_MAX_PC_FEELHATE] = ROWOW_SG_FEEL_VARS;
    ROWOWIdentity& identity = *GetIdentity(sd);
    if (identity.MenuSkillID != ROWOW_SKILL_SG_FEEL)
        return;
    int32 i = identity.MenuSkillValue - 1;
    identity.MenuSkillID = 0; // clif_menuskill_clear
    identity.MenuSkillValue = 0;
    if (i < 0 || i >= ROWOW_MAX_PC_FEELHATE || IsROMap(sd->GetMapId()) == false)
        return; // Bug?
    SetIdentityVariable(identity, feelVars[i], GetUnitMapKey(sd)); // A map key: a part of a stitched map is a place of its own
    ClifFeelInfo(sd, i, 0);
}

void ROWOWMod::ClifFeelInfo(Player* sd, int32 pos, int32 type) const
{
    // clif_feel_info (clif_starskill): the place of the sun, moon or star, as just registered (type 0, after the confirmation) or shown (1).
    // The WoW client has no such window, so it's a system message
    static char const* const placeNames[ROWOW_MAX_PC_FEELHATE] = { "Sun", "Moon", "Star" };
    ROWOWIdentity const* identity = sd->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
    if (identity == nullptr || pos < 0 || pos >= ROWOW_MAX_PC_FEELHATE)
        return;
    std::string mapName = GetMapNameOfMapKey(PCGetFeelMap(*identity, pos));
    if (mapName.empty() == true)
        mapName = "?";
    if (type == 0)
        ChatHandler(sd->GetSession()).PSendSysMessage("{} is now your Place of the {}.", mapName, placeNames[pos]);
    else
        ChatHandler(sd->GetSession()).PSendSysMessage("Your Place of the {} is {}.", placeNames[pos], mapName);
}

void ROWOWMod::ClifHateInfo(Player* sd, int32 pos, int32 hateClass, int32 type) const
{
    // clif_hate_info (clif_starskill): the monster (or job) hated by the sun, moon or star, registered now (type 1) or shown (0)
    static char const* const placeNames[ROWOW_MAX_PC_FEELHATE] = { "Sun", "Moon", "Star" };
    if (pos < 0 || pos >= ROWOW_MAX_PC_FEELHATE)
        return;
    std::string name;
    if (ROWOWJob const* job = (hateClass < 1000 || (hateClass >= 4000 && hateClass < 5000)) ? GetJob(uint16(hateClass)) : nullptr)
        name = job->Name;
    else if (CreatureTemplate const* creatureTemplate = sObjectMgr->GetCreatureTemplate(GetMobCreatureEntry(uint32(hateClass))))
        name = creatureTemplate->Name;
    else
        name = std::to_string(hateClass);
    if (type == 1)
        ChatHandler(sd->GetSession()).PSendSysMessage("{} is now your Target of the {}.", name, placeNames[pos]);
    else
        ChatHandler(sd->GetSession()).PSendSysMessage("Your Target of the {} is {}.", placeNames[pos], name);
}

void ROWOWMod::ClifMissionInfo(Player* sd, uint32 mobID, int32 progress) const
{
    // clif_mission_info (clif_starskill): the Taekwon mission's monster and the kills so far
    CreatureTemplate const* creatureTemplate = sObjectMgr->GetCreatureTemplate(GetMobCreatureEntry(mobID));
    ChatHandler(sd->GetSession()).PSendSysMessage("Taekwon Mission: {} ({}/100)", creatureTemplate != nullptr ? creatureTemplate->Name : std::to_string(mobID), progress);
}

void ROWOWMod::PCResetFeel(Player* sd)
{
    // pc_resetfeel
    static char const* const feelVars[ROWOW_MAX_PC_FEELHATE] = ROWOW_SG_FEEL_VARS;
    ROWOWIdentity& identity = *GetIdentity(sd);
    for (char const* feelVar : feelVars)
        SetIdentityVariable(identity, feelVar, 0);
}

void ROWOWMod::PCResetHate(Player* sd)
{
    // pc_resethate
    static char const* const hateVars[ROWOW_MAX_PC_FEELHATE] = ROWOW_SG_HATE_VARS;
    ROWOWIdentity& identity = *GetIdentity(sd);
    for (char const* hateVar : hateVars)
        SetIdentityVariable(identity, hateVar, 0);
}

bool ROWOWMod::SGIsDay(int32 pos) const
{
    // sg_info's day_func: is_day_of_sun, is_day_of_moon and is_day_of_star by the server's day of the year (date_get_dayofyear, 0 the first)
    if (BattleConfig.AllowSkillWithoutDay != 0)
        return true;
    time_t now = time(nullptr);
    tm localTime = Acore::Time::TimeBreakdown(now);
    int32 day = localTime.tm_yday + 1;
    switch (pos)
    {
        case 0: return day % 2 == 0;
        case 1: return day % 2 == 1;
        case 2: return day % 5 == 0;
    }
    return false;
}

void ROWOWMod::PCSetDieCounter(Player* sd, int64 value)
{
    // pc_setparam(SP_PCDIECOUNTER): a Super Novice's first death loses its never-died bonus (status_calc_pc)
    ROWOWIdentity& identity = *GetIdentity(sd);
    if (GetIdentityVariable(identity, ROWOW_PCDIECOUNTER_VAR) == value)
        return;
    SetIdentityVariable(identity, ROWOW_PCDIECOUNTER_VAR, value);
    if (GetNonBabyJobID(identity.JobID) == ROWOW_JOB_SUPER_NOVICE && identity.IsInForm == true)
        RefreshROForm(sd);
}

void ROWOWMod::MobDeadSGTaekwon(Creature* md, Player* sd, int32& bonus)
{
    // mob_dead for the player who killed the monster: the Star Gladiator's Blessing of the Sun, Moon or Star (10% a level more exp, 20% for the
    // star; Miracle makes every monster a star's) on its hated monster on its day, and the Taekwon mission's kill count (a new mission after
    // 100: a Branch of Dead Tree monster, and a fame point)
    static uint16 const blessSkillIDs[ROWOW_MAX_PC_FEELHATE] = { ROWOW_SKILL_SG_SUN_BLESS, ROWOW_SKILL_SG_MOON_BLESS, ROWOW_SKILL_SG_STAR_BLESS };
    ROWOWIdentity& identity = *GetIdentity(sd);
    int32 mobID = int32(GetROMobID(md));
    int32 i = 0;
    if (GetSCE(sd, ROWOW_SC_MIRACLE) != nullptr)
        i = 2; // All mobs are Star Targets
    else
        for (; i < ROWOW_MAX_PC_FEELHATE; i++)
            if (mobID == PCGetHateMob(identity, i) && SGIsDay(i) == true)
                break;
    int32 temp = 0;
    if (i < ROWOW_MAX_PC_FEELHATE && (temp = PCCheckSkill(sd, blessSkillIDs[i])) > 0)
        bonus += (i == 2 ? 20 : 10) * temp;

    // TK_MISSION [Skotlex]  (taekwon_mission_mobname's race and name matches need race2 and the names, not kept: only the monster itself)
    if (mobID != 0 && GetIdentityVariable(identity, ROWOW_TKMISSIONID_VAR) == mobID)
    {
        int64 missionCount = GetIdentityVariable(identity, ROWOW_TKMISSIONCOUNT_VAR) + 1;
        if (missionCount >= 100 && (temp = int32(MobGetRandomID("Branch_Of_Dead_Tree", false))) != 0)
        {
            PCAddFame(sd, BattleConfig.FameTaekwonMission);
            SetIdentityVariable(identity, ROWOW_TKMISSIONID_VAR, temp);
            missionCount = 0;
            ClifMissionInfo(sd, uint32(temp), 0);
        }
        SetIdentityVariable(identity, ROWOW_TKMISSIONCOUNT_VAR, missionCount);
    }
}

void ROWOWMod::PartySkillCheck(Player* sd, uint16 skillID, uint16 skillLevel)
{
    // party_skill_check: a Star Gladiator's Counter Kick raises the Triple Attack rate of the party's Monks (Friend's level), and a Monk's
    // Combo Finish the Counter Kick rate of its Star Gladiators in the counter stance with Friend.  The party here is the WoW group on the
    // sd's map
    if (sd->GetGroup() == nullptr)
        return;
    PartyForEachSameMap(sd, 0, [this, skillID, skillLevel](Unit* member)
    {
        Player* pSD = member->ToPlayer();
        if (pSD == nullptr || IsInROForm(pSD) == false)
            return 0;
        ROWOWJob const* job = GetJob(GetIdentity(pSD)->JobID);
        uint16 jobID = job != nullptr ? job->JobID : 0;
        switch (skillID)
        {
            case ROWOW_SKILL_TK_COUNTER: // Increase Triple Attack rate of Monks.
                if ((jobID == ROWOW_JOB_MONK || jobID == ROWOW_JOB_CHAMPION || jobID == ROWOW_JOB_BABY_MONK) && PCCheckSkill(pSD, ROWOW_SKILL_MO_TRIPLEATTACK) > 0)
                    SCStart4(pSD, pSD, ROWOW_SC_SKILLRATE_UP, 100, ROWOW_SKILL_MO_TRIPLEATTACK, 50 + 50 * skillLevel, 0, 0, SkillGetTime(ROWOW_SKILL_SG_FRIEND, 1)); // +100/150/200% rate
                break;
            case ROWOW_SKILL_MO_COMBOFINISH: // Increase Counter rate of Star Gladiators
                if (jobID == ROWOW_JOB_STAR_GLADIATOR && GetSCE(pSD, ROWOW_SC_READYCOUNTER) != nullptr && PCCheckSkill(pSD, ROWOW_SKILL_SG_FRIEND) > 0)
                    SCStart4(pSD, pSD, ROWOW_SC_SKILLRATE_UP, 100, ROWOW_SKILL_TK_COUNTER, 50 + 50 * PCCheckSkill(pSD, ROWOW_SKILL_SG_FRIEND), 0, 0,
                        SkillGetTime(ROWOW_SKILL_SG_FRIEND, 1)); // +100/150/200% rate
                break;
        }
        return 0;
    });
}

bool ROWOWMod::StatusDamageRevive(Unit* bl)
{
    // status_damage's Kaizel: a unit about to die is revived at val2% HP, its statuses cleared, and gets Kyrie Eleison 10 for Kaizel's
    // Duration2.  pc_dead's Super Novice: one that dies at 99% of its base level's exp is saved by its angel (full HP and SP, a moment of
    // invincibility and Mental Strength 5), once a base level.  WoW's death already happened, so the player is resurrected just after it
    // (players only: a monster's revive is R10's)
    Player* sd = bl->ToPlayer();
    if (sd == nullptr || IsInROForm(sd) == false)
        return false;
    ROWOWIdentity* identity = GetIdentity(sd);
    if (ROWOWStatusChangeEntry const* kaizel = GetSCE(bl, ROWOW_SC_KAIZEL))
    {
        int32 kaizelLevel = kaizel->Val1;
        int32 perHP = kaizel->Val2;
        sd->m_Events.AddEventAtOffset([kaizelLevel, perHP, sd]()
        {
            if (sd->IsAlive() == true || ROWOW->IsInROForm(sd) == false)
                return;
            ROWOW->StatusRevive(sd, uint8(perHP), 0);
            ROWOW->StatusChangeClear(sd, 0);
            ROWOW->ClifSkillNoDamage(sd, sd, ROWOW_SKILL_ALL_RESURRECTION, 1);
            ROWOW->SCStart(sd, sd, ROWOW_SC_KYRIE, 100, 10, ROWOW->SkillGetTime2(ROWOW_SKILL_SL_KAIZEL, uint16(kaizelLevel)));
        }, 1ms);
        return true;
    }
    // Activate Steel body if a super novice dies at 99+% exp [celest]
    if (identity->IsSuperNoviceDeadFlag == false && GetNonBabyJobID(identity->JobID) == ROWOW_JOB_SUPER_NOVICE)
    {
        uint64 exp = PCNextBaseExp(*identity);
        if (exp != 0 && identity->BaseExp * 100 / exp >= 99)
        {
            identity->IsSuperNoviceDeadFlag = true;
            sd->m_Events.AddEventAtOffset([sd]()
            {
                if (sd->IsAlive() == true || ROWOW->IsInROForm(sd) == false)
                    return;
                ROWOW->StatusRevive(sd, 100, 100);
                ROWOW->PCSetInvincibleTimer(sd);
                ROWOW->SCStart(sd, sd, ROWOW_SC_STEELBODY, 100, 5, ROWOW->SkillGetTime(ROWOW_SKILL_MO_STEELBODY, 5));
            }, 1ms);
            return true;
        }
    }
    return false;
}

void ROWOWMod::PCSuperNoviceCall(Player* sd, std::string const& message)
{
    // The Guardian Angel prayer: a Super Novice says "Dear angel, can you hear my voice?", "I am <name> Super Novice~" and "Help me out~
    // Please~ T_T" in turn (the client's own msgstringtable lines, which it turns into CZ_CHOPOKGI); at a round 10% of its base exp the angel
    // answers with Fury (clif_parse_NoviceExplosionSpirits: SC_EXPLOSIONSPIRITS at 17, +50 critical)
    ROWOWIdentity* identity = GetIdentity(sd);
    if (identity->IsInForm == false || GetNonBabyJobID(identity->JobID) != ROWOW_JOB_SUPER_NOVICE)
        return;
    switch (identity->SuperNoviceCallFlag)
    {
        case 0:
            if (message.find("Dear angel, can you hear my voice?") != std::string::npos)
                identity->SuperNoviceCallFlag = 1;
            return;
        case 1:
            identity->SuperNoviceCallFlag = message.find(fmt::format("I am {} Super Novice~", sd->GetName())) != std::string::npos ? 2 : 0;
            return;
        default:
            identity->SuperNoviceCallFlag = 0;
            if (message.find("Help me out~ Please~ T_T") == std::string::npos)
                return;
            break;
    }
    uint64 next = PCNextBaseExp(*identity);
    if (next == 0)
        return;
    int32 percent = int32((double(identity->BaseExp) / double(next)) * 1000.);
    if (percent != 0 && (percent % 100) == 0) // 10.0%, 20.0%, ..., 90.0%
    {
        SCStart(sd, sd, ROWOW_SC_EXPLOSIONSPIRITS, 100, 17, SkillGetTime(ROWOW_SKILL_MO_EXPLOSIONSPIRITS, 5)); // Lv17-> +50 critical (noted by Poki) [Skotlex]
        ClifSkillNoDamage(sd, sd, ROWOW_SKILL_MO_EXPLOSIONSPIRITS, 5); // prayer always shows successful Lv5 cast and disregards noskill restrictions
    }
}

void ROWOWMod::PCLevelUpAngel(Player* sd)
{
    // pc_checkbaselevelup: a Super Novice gets every status with the SuperNoviceAngel flag (Kyrie Eleison, Magnificat, Gloria, Impositio Manus,
    // Suffragium) at level 1 for 2 minutes and can be saved by its angel again; a Taekwon those with TaekwonAngel (Increase AGI, Blessing) at
    // level 10 for 10 minutes
    ROWOWIdentity* identity = GetIdentity(sd);
    ROWOWJob const* job = GetJob(identity->JobID);
    bool isSuperNovice = GetNonBabyJobID(identity->JobID) == ROWOW_JOB_SUPER_NOVICE;
    bool isTaekwon = job != nullptr && (job->JobID == ROWOW_JOB_TAEKWON || job->FirstJobID == ROWOW_JOB_TAEKWON);
    if (isSuperNovice == false && isTaekwon == false)
        return;
    for (auto const& statusByType : StatusChangeDBsByType)
    {
        if (isSuperNovice == true && statusByType.second.Flag[ROWOW_SCF_SUPERNOVICEANGEL] == true)
            SCStart(sd, sd, statusByType.first, 100, 1, 120000); // All buffs with super novice flag for 2 minutes
        else if (isTaekwon == true && statusByType.second.Flag[ROWOW_SCF_TAEKWONANGEL] == true)
            SCStart(sd, sd, statusByType.first, 100, 10, 600000);
    }
    if (isSuperNovice == true)
        identity->IsSuperNoviceDeadFlag = false; // Reenable steelbody resurrection on dead.
}

void ROWOWMod::SkillTaekwonRest(Player* sd, bool sitting)
{
    // skill_sit's flag 2: a Taekwon with Peaceful Break or Happy Break sitting down beside another sitting one (their splash around it) puts
    // every such Taekwon there at rest (state.rest, which their regen reads); standing up ends it for those who no longer have two sitting
    // together around them
    int32 level = 0;
    int32 range = 0;
    if ((level = PCCheckSkill(sd, ROWOW_SKILL_TK_HPTIME)) > 0)
        range = SkillGetSplash(ROWOW_SKILL_TK_HPTIME, uint16(level));
    else if ((level = PCCheckSkill(sd, ROWOW_SKILL_TK_SPTIME)) > 0)
        range = SkillGetSplash(ROWOW_SKILL_TK_SPTIME, uint16(level));
    else
        return; // No need to count area if no skills are learned.
    auto isResting = [this](Player* tsd)
    {
        return tsd->IsSitState() == true && (PCCheckSkill(tsd, ROWOW_SKILL_TK_HPTIME) > 0 || PCCheckSkill(tsd, ROWOW_SKILL_TK_SPTIME) > 0);
    };
    // skill_sit_count
    auto sitCount = [this, range, &isResting](Player* center)
    {
        return MapForEachInRange(center, range, [&isResting](Unit* bl)
        {
            Player* tsd = bl->ToPlayer();
            return (tsd != nullptr && isResting(tsd) == true) ? 1 : 0;
        });
    };
    auto setRest = [this](Player* tsd, bool isRest)
    {
        ROWOWIdentity* identity = GetIdentity(tsd);
        if (identity->IsTaekwonRest == isRest)
            return;
        identity->IsTaekwonRest = isRest;
        StatusCalcRegen(tsd, identity->Status);
    };
    if (sitting == true)
    {
        if (sitCount(sd) <= 1)
            return;
        // skill_sit_in
        MapForEachInRange(sd, range, [&isResting, &setRest](Unit* bl)
        {
            Player* tsd = bl->ToPlayer();
            if (tsd != nullptr && isResting(tsd) == true)
                setRest(tsd, true);
            return 0;
        });
        return;
    }
    // skill_sit_out
    MapForEachInRange(sd, range, [&sitCount, &setRest](Unit* bl)
    {
        Player* tsd = bl->ToPlayer();
        if (tsd == nullptr || sitCount(tsd) > 1)
            return 0;
        setRest(tsd, false);
        return 0;
    });
}
