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
#include "Corpse.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include "ROWOW.h"

using namespace std;

// Identity per world (Nathan 2026-11-09).  A character has three forms it can show, its identities: Warcraft (its own body), EverQuest (its EverQuest
// look: GetEverQuestIdentityLookSpellID) and Ragnarok (its Ragnarok Online character's job sprite, head and headgear).  Which one shows is picked for
// each world: Azeroth (every map that is neither of the others), Norrath (mod-everquest's maps) and Midgard (the RO maps), each starting as the world's
// own.  The choices are the character's (mod_rowow_world_identity; no row is the world's own) and are set by ".identity <world> <identity>", which
// the unit frame's menu and Options > Interface > Display send.
//
// Only the body shown changes: in Midgard the character is in the RO form whatever it looks like (RO stats, skills and bars), and out of it the
// Ragnarok identity is a look alone.  What may be shown where is gated by the Midgard achievements (GetWorldIdentityLock).
//
// A look is kept on the character as a client hidden aura (the core's Aura::SetClientHidden): no buff icon for anyone, nothing to click off, and
// never saved.  RefreshWorldIdentityLook puts on what is wanted and takes off what it put on before, on every arrival and change, and once a second
// for whatever took an aura off meanwhile (a death, a resurrection, a profile swap, a dispel).  A look put on by hand (a guise spell, an RO illusion out
// of the form) is a visible aura, which this leaves alone: it shows over the identity while it lasts, as before.
//
// A character arrives wearing its look (Nathan 2026-11-10): at a login (the look isn't saved, so the character loads in its own body) and on every
// change of map, the look for the world arrived in is made right before the character is put on the map, so what its own client and every other
// are first sent is the character as it will stay (ROWOW_IdentityPlayerScript::OnPlayerSendInitialPacketsBeforeAddToMap).  A look that is already
// right is never taken off and put on again: every refresh only changes what differs.
//
// In a battleground or an arena every character shows its Warcraft identity, whatever it picked (the pick is kept and shows again on leaving), and no
// identity can be changed in combat (GetWorldIdentityChangeLock)

namespace
{
    char const* const WorldNames[ROWOW_WORLD_COUNT] = { "Azeroth", "Norrath", "Midgard" };
    char const* const WorldIdentityNames[ROWOW_WORLD_IDENTITY_COUNT] = { "Warcraft", "EverQuest", "Ragnarok" };

    // The world's own identity: what a character shows there until it picks another
    uint8 GetWorldDefaultIdentity(uint8 world)
    {
        switch (world)
        {
            case ROWOW_WORLD_NORRATH: return ROWOW_WORLD_IDENTITY_EVERQUEST;
            case ROWOW_WORLD_MIDGARD: return ROWOW_WORLD_IDENTITY_RAGNAROK;
            default: return ROWOW_WORLD_IDENTITY_WARCRAFT;
        }
    }

    bool IsNameMatch(std::string const& text, char const* name)
    {
        // The whole name or its start, any case ("mid", "RAGNAROK")
        if (text.empty() == true || text.size() > strlen(name))
            return false;
        for (size_t i = 0; i < text.size(); i++)
            if (std::tolower(static_cast<unsigned char>(text[i])) != std::tolower(static_cast<unsigned char>(name[i])))
                return false;
        return true;
    }
}

uint8 ROWOWMod::GetWorldOfMap(uint32 mapID) const
{
    if (IsROMap(mapID) == true)
        return ROWOW_WORLD_MIDGARD;
    if (IsNorrathMap(mapID) == true)
        return ROWOW_WORLD_NORRATH;
    return ROWOW_WORLD_AZEROTH;
}

ROWOWWorldIdentityState* ROWOWMod::GetWorldIdentityState(Player* player)
{
    // Its own data on the player, not the RO identity's: that one is the save slot's, and is made anew when the slot changes
    ROWOWWorldIdentityState* state = player->CustomData.GetDefault<ROWOWWorldIdentityState>("ROWOWWorldIdentity");
    if (state->IsLoaded == true)
        return state;
    state->IsLoaded = true;
    for (uint8 world = 0; world < ROWOW_WORLD_COUNT; world++)
        state->Chosen[world] = GetWorldDefaultIdentity(world);
    if (QueryResult result = CharacterDatabase.Query("SELECT `world`, `identity` FROM `mod_rowow_world_identity` WHERE `guid` = {}", player->GetGUID().GetCounter()))
    {
        do
        {
            Field* fields = result->Fetch();
            uint8 world = fields[0].Get<uint8>();
            uint8 worldIdentity = fields[1].Get<uint8>();
            if (world < ROWOW_WORLD_COUNT && worldIdentity < ROWOW_WORLD_IDENTITY_COUNT)
                state->Chosen[world] = worldIdentity;
        } while (result->NextRow());
    }
    return state;
}

char ROWOWMod::GetWorldIdentityLock(Player* player, uint8 world, uint8 worldIdentity)
{
    // Why the identity can't be shown in the world now, ROWOW_WORLD_IDENTITY_LOCK_NONE when it can:
    // - the EverQuest identity needs an EverQuest look (none on a server without mod-everquest, or for a race it has no look for)
    // - the Ragnarok identity out of Midgard needs a Ragnarok Online character (in any save slot) and Midgard Pathfinder.  In Midgard it is never
    //   locked: arriving there with no character makes one, as it always did
    // - any other identity in Midgard needs Midgard Wayfarer
    // Warcraft and EverQuest are free in Azeroth and Norrath
    if (worldIdentity == ROWOW_WORLD_IDENTITY_EVERQUEST && GetEverQuestIdentityLookSpellID(player) == 0)
        return ROWOW_WORLD_IDENTITY_LOCK_NO_EVERQUEST;
    if (worldIdentity == ROWOW_WORLD_IDENTITY_RAGNAROK)
    {
        if (world == ROWOW_WORLD_MIDGARD)
            return ROWOW_WORLD_IDENTITY_LOCK_NONE;
        if (HasAnyIdentity(*GetIdentity(player)) == false)
            return ROWOW_WORLD_IDENTITY_LOCK_NO_RO_CHARACTER;
        if (HasMidgardPathfinder(player) == false)
            return ROWOW_WORLD_IDENTITY_LOCK_PATHFINDER;
        return ROWOW_WORLD_IDENTITY_LOCK_NONE;
    }
    if (world == ROWOW_WORLD_MIDGARD && HasMidgardWayfarer(player) == false)
        return ROWOW_WORLD_IDENTITY_LOCK_WAYFARER;
    return ROWOW_WORLD_IDENTITY_LOCK_NONE;
}

bool ROWOWMod::IsWorldIdentityHeldBack(Player const* player) const
{
    // Battlegrounds and arenas: the character's own body, which the other side knows how to read
    Map const* map = player != nullptr ? player->FindMap() : nullptr;
    return map != nullptr && map->IsBattlegroundOrArena() == true;
}

char ROWOWMod::GetWorldIdentityChangeLock(Player* player, uint8 world, uint8 worldIdentity)
{
    // Why the identity can't be picked for the world now: what the client shows beside it, and what .identity refuses with.  No change in combat, none
    // for the world of a battleground or arena while in it, and otherwise what keeps it from being shown at all
    if (player->IsInCombat() == true)
        return ROWOW_WORLD_IDENTITY_LOCK_COMBAT;
    if (IsWorldIdentityHeldBack(player) == true && world == GetWorldOfMap(player->GetMapId()))
        return ROWOW_WORLD_IDENTITY_LOCK_BATTLEGROUND;
    return GetWorldIdentityLock(player, world, worldIdentity);
}

char const* ROWOWMod::GetWorldIdentityLockText(char lock) const
{
    switch (lock)
    {
        case ROWOW_WORLD_IDENTITY_LOCK_COMBAT: return "You can't change your identity while you are in combat.";
        case ROWOW_WORLD_IDENTITY_LOCK_BATTLEGROUND: return "Your Warcraft identity is shown in battlegrounds and arenas. Your choice is kept and shows again when you leave.";
        case ROWOW_WORLD_IDENTITY_LOCK_PATHFINDER: return "You need the achievement Midgard Pathfinder to show your Ragnarok identity outside of Midgard.";
        case ROWOW_WORLD_IDENTITY_LOCK_WAYFARER: return "You need the achievement Midgard Wayfarer to show another identity in Midgard. It also lets you use your non-Ragnarok companion pets there.";
        case ROWOW_WORLD_IDENTITY_LOCK_NO_RO_CHARACTER: return "You have no Ragnarok Online character yet. Make one on the Ragnarok page of the character window.";
        case ROWOW_WORLD_IDENTITY_LOCK_NO_EVERQUEST: return "Your character has no EverQuest identity.";
        default: return "";
    }
}

uint8 ROWOWMod::GetWorldIdentity(Player* player, uint8 world)
{
    // The one picked while it may be shown, else the world's own, else Warcraft (a pick stays kept while it's locked, and shows again once it isn't)
    if (world >= ROWOW_WORLD_COUNT)
        return ROWOW_WORLD_IDENTITY_WARCRAFT;
    if (IsWorldIdentityHeldBack(player) == true && world == GetWorldOfMap(player->GetMapId()))
        return ROWOW_WORLD_IDENTITY_WARCRAFT; // In a battleground or arena, whatever was picked (which is kept)
    uint8 chosen = GetWorldIdentityState(player)->Chosen[world];
    if (GetWorldIdentityLock(player, world, chosen) == ROWOW_WORLD_IDENTITY_LOCK_NONE)
        return chosen;
    uint8 defaultIdentity = GetWorldDefaultIdentity(world);
    if (GetWorldIdentityLock(player, world, defaultIdentity) == ROWOW_WORLD_IDENTITY_LOCK_NONE)
        return defaultIdentity;
    return ROWOW_WORLD_IDENTITY_WARCRAFT;
}

bool ROWOWMod::SetWorldIdentity(Player* player, uint8 world, uint8 worldIdentity, std::string& errorOut)
{
    if (IsEnabled == false || world >= ROWOW_WORLD_COUNT || worldIdentity >= ROWOW_WORLD_IDENTITY_COUNT)
    {
        errorOut = "Use .identity <azeroth/norrath/midgard> <warcraft/everquest/ragnarok>.";
        return false;
    }
    char lock = GetWorldIdentityChangeLock(player, world, worldIdentity);
    if (lock != ROWOW_WORLD_IDENTITY_LOCK_NONE)
    {
        errorOut = GetWorldIdentityLockText(lock);
        return false;
    }
    ROWOWWorldIdentityState* state = GetWorldIdentityState(player);
    if (state->Chosen[world] != worldIdentity)
    {
        // Written at once, on its own: it is the character's whatever its next save does
        state->Chosen[world] = worldIdentity;
        if (worldIdentity == GetWorldDefaultIdentity(world))
            CharacterDatabase.Execute("DELETE FROM `mod_rowow_world_identity` WHERE `guid` = {} AND `world` = {}", player->GetGUID().GetCounter(), world);
        else
            CharacterDatabase.Execute("REPLACE INTO `mod_rowow_world_identity` (`guid`, `world`, `identity`) VALUES ({}, {}, {})", player->GetGUID().GetCounter(), world, worldIdentity);
    }
    RefreshWorldIdentityLook(player);
    SendWorldIdentityToClient(player, true);
    return true;
}

uint32 ROWOWMod::GetWorldIdentityRagnarokLookSpellID(Player* player)
{
    // The Ragnarok identity's body: the job sprite shown (GetIdentityLookIllusionSpellID: the one picked, else the job of the save slot in use), in the
    // form riding its Peco.  Out of Midgard with nothing picked and no character in the slot in use, the first slot's that has one.
    // On a WoW mount (Midgard Worldwalker's, in the form) a Peco rider shows its body on foot, which sits on the mount's back (the body on its Peco
    // has no place there); it still rides its Peco, and shows it again once off the mount (UpdateWorldIdentity).
    // Out of Midgard (Nathan 2026-11-10) the body is the one on foot, whatever the RO character's Peco option, but for one case: on a ground mount
    // a job sprite that has a Peco body, of a character that has learned Riding, is shown on its Peco, in the mount's place (IsPecoShownForMount;
    // RefreshWorldIdentityMount keeps the mount's own model from showing)
    ROWOWIdentity* identity = GetIdentity(player);
    if (identity->IsInForm == true)
        return GetIdentityLookIllusionSpellID(player, PCIsRiding(player) == true && player->IsMounted() == false);
    uint32 spellID = GetIdentityLookIllusionSpellID(player, IsPecoShownForMount(player, GetIdentityLookJob(player)));
    if (spellID != 0)
        return spellID;
    bool isMale = player->getGender() == GENDER_MALE;
    for (ROWOWIdentitySlot const& identitySlot : identity->Slots)
    {
        ROWOWJob const* job = identitySlot.Exists == true ? GetJob(identitySlot.JobID) : nullptr;
        if (job == nullptr)
            continue;
        auto illusionSpellIDIter = IllusionSpellIDsByJobKey.end();
        if (IsPecoShownForMount(player, job) == true)
            illusionSpellIDIter = IllusionSpellIDsByJobKey.find(GetIllusionJobKey(job->IllusionJobKey + "_riding", isMale));
        if (illusionSpellIDIter == IllusionSpellIDsByJobKey.end())
            illusionSpellIDIter = IllusionSpellIDsByJobKey.find(GetIllusionJobKey(job->IllusionJobKey, isMale));
        if (illusionSpellIDIter != IllusionSpellIDsByJobKey.end())
            return illusionSpellIDIter->second;
    }
    return 0;
}

bool ROWOWMod::HasLearnedPecoRiding(Player* player)
{
    // "Has learned Peco riding" (Nathan 2026-11-10, second ruling): the identity of any of the character's save slots has learned Riding (KN_RIDING,
    // the one skill a Peco is mounted by: PCSetRiding; it is on the Knight's and the Crusader's trees and their transcendent and baby jobs'),
    // whatever job sprite is shown.  The slot in use is asked for the skill itself, so learning or losing it counts at once; a slot put away by
    // what it had when it was last in use (ROWOWIdentitySlot::HasRiding: read with the slots, set when the slot is changed, cleared by a reset)
    ROWOWIdentity* identity = GetIdentity(player);
    if (identity->Exists == true && PCCheckSkill(*identity, ROWOW_SKILL_KN_RIDING) > 0)
        return true;
    for (uint8 slotIndex = 0; slotIndex < ROWOW_IDENTITY_SLOT_COUNT; slotIndex++)
        if (slotIndex + 1 != identity->Slot && identity->Slots[slotIndex].Exists == true && identity->Slots[slotIndex].HasRiding == true)
            return true;
    return false;
}

uint32 ROWOWMod::GetMountAuraSpellID(Player const* player) const
{
    Unit::AuraEffectList const& mountAuras = player->GetAuraEffectsByType(SPELL_AURA_MOUNTED);
    return mountAuras.empty() == false ? mountAuras.front()->GetId() : 0;
}

bool ROWOWMod::IsPecoShownForMount(Player* player, ROWOWJob const* shownJob)
{
    // Out of Midgard, the Ragnarok identity on a WoW ground mount: a job sprite with a Peco body ("<job key>_riding": the Knight, Crusader, Lord
    // Knight and Paladin, in both genders; a baby job shows its normal job's sprite), of a character one of whose save slots has learned Riding
    // (HasLearnedPecoRiding), is shown riding its Peco, and the mount's own model isn't shown (RefreshWorldIdentityMount).  The character is on
    // its mount all the same: its speed, its aura, every rule of being mounted.
    // - Ground or flying is the mount's spell, not where the character is: a mount whose spell gives a flight speed is a flying mount on the
    //   ground too, and the sprite sits on its back as on any mount.  A mount that is either (the ones that scale with the riding skill) is cast
    //   as one of several spells, picked by where it is cast (spell_gen_mount): its aura is its ground form where flying isn't allowed and its
    //   flying form where it is, and that form decides
    // - Only a mount of the character's own: its aura is a spell the character knows, or lasts until cancelled (a scaling mount's forms aren't
    //   known themselves).  A mount lent for a time (a Brewfest ram, which its scripts tell by its display) is shown as it is
    // - A mount with seats (a passenger sits on the mount's model) and a taxi are shown as they are
    // - The character may have it off (Options > Interface > Ragnarok, .ro pecomount): the mount is then shown as it is, with the sprite on its back
    if (shownJob == nullptr || IsInROForm(player) == true || player->IsMounted() == false || player->IsInFlight() == true || player->GetVehicleKit() != nullptr)
        return false;
    if (GetIdentity(player)->IsPecoMountLookEnabled == false)
        return false;
    if (IllusionSpellIDsByJobKey.find(GetIllusionJobKey(shownJob->IllusionJobKey + "_riding", player->getGender() == GENDER_MALE)) == IllusionSpellIDsByJobKey.end())
        return false;
    Unit::AuraEffectList const& mountAuras = player->GetAuraEffectsByType(SPELL_AURA_MOUNTED);
    if (mountAuras.empty() == true)
        return false;
    SpellInfo const* mountSpellInfo = mountAuras.front()->GetSpellInfo();
    if (mountSpellInfo->HasAura(SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED) == true || mountSpellInfo->HasAura(SPELL_AURA_MOD_INCREASE_FLIGHT_SPEED) == true
        || mountSpellInfo->HasAura(SPELL_AURA_FLY) == true)
        return false;
    if (player->HasSpell(mountSpellInfo->Id) == false && mountAuras.front()->GetBase()->IsPermanent() == false)
        return false;
    return HasLearnedPecoRiding(player);
}

void ROWOWMod::RefreshWorldIdentityMount(Player* player, bool isPecoShown)
{
    // The Peco in the mount's place (IsPecoShownForMount): the character's mount display is one that doesn't exist.  To the client a unit is
    // mounted while that field isn't 0 (its Dismount, the dismount before a cast, IsMounted and the macro conditions all read it), and it makes
    // the mount's model, seats the rider on it and has the rider play its Mount animation only when the display has a record
    // (CGUnit_C::CreateUnitMount returns before any of that with none).  So the Peco sprite stands, walks and runs as itself, at the mount's
    // speed, and the player's own client and every other see no mount.  The server reads the field only for a mounted character's collision
    // size (with no record it uses the character's own) and takes "mounted" from the unit flag, which stays.  The mount's own display is kept,
    // and put back when the Peco stops being shown on the same mount (the identity or the job sprite changed, a look was put on by hand).
    // Unit::Mount writes the field each time a mount is cast, and this runs at the end of that same player update (UpdateWorldIdentity), before
    // the field is sent to anyone
    ROWOWWorldIdentityState* state = GetWorldIdentityState(player);
    uint32 mountDisplayID = player->GetUInt32Value(UNIT_FIELD_MOUNTDISPLAYID);
    if (player->IsMounted() == false || mountDisplayID == 0)
    {
        state->HiddenMountDisplayID = 0;
        return;
    }
    if (isPecoShown == true)
    {
        if (mountDisplayID == ROWOW_MOUNT_DISPLAY_ID_NONE)
            return;
        state->HiddenMountDisplayID = mountDisplayID;
        player->SetUInt32Value(UNIT_FIELD_MOUNTDISPLAYID, ROWOW_MOUNT_DISPLAY_ID_NONE);
        return;
    }
    if (mountDisplayID != ROWOW_MOUNT_DISPLAY_ID_NONE)
        return;
    uint32 shownMountDisplayID = state->HiddenMountDisplayID;
    state->HiddenMountDisplayID = 0;
    if (shownMountDisplayID == 0)
    {
        // Not kept (it always is): the display of the mount aura's creature, as the aura's own handler picks it
        Unit::AuraEffectList const& mountAuras = player->GetAuraEffectsByType(SPELL_AURA_MOUNTED);
        if (mountAuras.empty() == false)
            if (CreatureTemplate const* creatureTemplate = sObjectMgr->GetCreatureTemplate(mountAuras.front()->GetMiscValue()))
                shownMountDisplayID = ObjectMgr::ChooseDisplayId(creatureTemplate)->CreatureDisplayID;
    }
    if (shownMountDisplayID != 0)
        player->SetUInt32Value(UNIT_FIELD_MOUNTDISPLAYID, shownMountDisplayID);
}

bool ROWOWMod::IsWorldIdentityLookAura(Player const* player, uint32 spellID) const
{
    // An aura an identity put on (see the top of this file)
    if (player == nullptr)
        return false;
    Aura const* aura = player->GetAura(spellID);
    return aura != nullptr && aura->IsClientHidden() == true;
}

bool ROWOWMod::HasByHandLook(Player const* player) const
{
    // A transform that is no identity's (a visible one): it is the newer one, so it is what shows
    for (AuraEffect const* auraEffect : player->GetAuraEffectsByType(SPELL_AURA_TRANSFORM))
        if (auraEffect->GetBase()->IsClientHidden() == false)
            return true;
    return false;
}

void ROWOWMod::RefreshWorldIdentityLook(Player* player, bool isArriving /*= false*/)
{
    // Makes the character look as its identity for the world it is in.  The player's own thread, in the world, and not in the middle of a form
    // change (CompleteROFormChange ends one, and comes here).
    // isArriving: the character is on its way onto a map (a login, a change of map) and no client has been sent it yet: its map is the one it
    // arrives on and its form is already the one it will have there (EnterROForm and LeaveROForm have run, the rest of the form change follows once
    // it's in the world), so the look is made right now and the character is first seen wearing it
    if (IsEnabled == false || player == nullptr)
        return;
    if (isArriving == false && (player->IsInWorld() == false || player->IsBeingTeleported() == true))
        return;
    ROWOWIdentity* identity = GetIdentity(player);
    if (isArriving == false && (identity->IsFormArrivalPending == true || identity->IsFormDeparturePending == true))
        return;
    bool isInForm = identity->IsInForm;
    uint8 world = GetWorldOfMap(player->GetMapId());
    uint8 worldIdentity = GetWorldIdentity(player, world);

    // On an RO map out of the form (an identity reset is leaving it) there is no Ragnarok look to show
    uint32 wantedIllusionSpellID = 0;
    if (worldIdentity == ROWOW_WORLD_IDENTITY_RAGNAROK && (isInForm == true || world != ROWOW_WORLD_MIDGARD))
        wantedIllusionSpellID = GetWorldIdentityRagnarokLookSpellID(player);
    uint32 wantedEverQuestSpellID = worldIdentity == ROWOW_WORLD_IDENTITY_EVERQUEST ? GetEverQuestIdentityLookSpellID(player) : 0;

    // What is on now.  The RO illusions: in the form every one is the form's (the wanted one is made the identity's, any other goes); out of it only
    // the ones an identity put on are touched.  Any other hidden transform is an EverQuest look an identity put on, and any other visible one a look
    // put on by hand
    bool hasWantedIllusion = false;
    bool hasWantedEverQuestLook = false;
    bool hasByHandLook = false;
    std::vector<uint32> spellIDsToRemove;
    std::vector<Aura*> aurasToHide;
    for (AuraEffect const* auraEffect : player->GetAuraEffectsByType(SPELL_AURA_TRANSFORM))
    {
        uint32 spellID = auraEffect->GetId();
        Aura* aura = auraEffect->GetBase();
        bool isHidden = aura->IsClientHidden();
        if (IllusionsBySpellID.find(spellID) != IllusionsBySpellID.end())
        {
            if (spellID == wantedIllusionSpellID)
            {
                hasWantedIllusion = true;
                if (isInForm == true && isHidden == false)
                    aurasToHide.push_back(aura);
            }
            else if (isInForm == true || isHidden == true)
                spellIDsToRemove.push_back(spellID);
            else
                hasByHandLook = true;
        }
        else if (spellID == wantedEverQuestSpellID)
            hasWantedEverQuestLook = true; // By hand or the identity's: the look is there either way
        else if (isHidden == true)
            spellIDsToRemove.push_back(spellID);
        else
            hasByHandLook = true;
    }
    for (Aura* aura : aurasToHide)
        aura->SetClientHidden();
    for (uint32 spellID : spellIDsToRemove)
        player->RemoveAurasDueToSpell(spellID);

    // The newest transform is the one the core shows, so an identity's look that went missing under a look put on by hand (a profile swap and a dispel
    // take it, and leave the other) isn't put back over it: it comes back the moment the look put on by hand ends (UpdateWorldIdentity)
    if (wantedEverQuestSpellID != 0 && hasWantedEverQuestLook == false && hasByHandLook == false)
        if (AddWorldIdentityAura(player, wantedEverQuestSpellID) != nullptr)
            OnEverQuestIdentityLookHidden(player);
    if (wantedIllusionSpellID != 0 && hasWantedIllusion == false && hasByHandLook == false)
        AddWorldIdentityAura(player, wantedIllusionSpellID);

    // What UpdateWorldIdentity watches between two checks
    ROWOWWorldIdentityState* state = GetWorldIdentityState(player);
    state->LookIllusionSpellID = (wantedIllusionSpellID != 0 && player->HasAura(wantedIllusionSpellID) == true) ? wantedIllusionSpellID : 0;
    state->LookEverQuestSpellID = (wantedEverQuestSpellID != 0 && player->HasAura(wantedEverQuestSpellID) == true) ? wantedEverQuestSpellID : 0;
    state->HasByHandLook = hasByHandLook;
    // Arriving: once the character is on its map the core handles its transform aura again, which sets the aura's own display over the look's
    // (the EverQuest form's creature display over its dressed character, the job sprite's bare display over the one holding its weapon).  The
    // character's first update there takes the look's display back, before any client is sent the other (UpdateWorldIdentity)
    if (isArriving == true)
        state->IsArrivalCheckPending = true;

    // The EverQuest look's own display (mod-everquest's, dressed in the character's gear), should anything have set another under it
    if (state->LookEverQuestSpellID != 0 && hasByHandLook == false)
        RefreshEverQuestIdentityLookDisplay(player);

    // A look the character still has that the core has stopped showing: when a transform put over it ends (a guise spell cast by hand, cancelled),
    // the core shows a remaining harmless transform again only on a character in no stance or form (Unit::RestoreDisplayId), so a Warrior in its
    // stance stayed in its own body.  The EverQuest look needs none of this: mod-everquest takes its display back itself
    if (wantedIllusionSpellID != 0 && player->getTransForm() == 0)
        if (AuraEffect* illusionEffect = player->GetAuraEffect(wantedIllusionSpellID, EFFECT_0))
            illusionEffect->HandleEffect(player, AURA_EFFECT_HANDLE_SEND_FOR_CLIENT, true);

    // The sprite's weapon and shield, its head and what it wears on it.  A shapeshift form with its own model shows over an illusion out of the RO
    // form (the core's rule for a harmless transform), so the sprite's display isn't forced on it, and the head has no sprite to sit on
    bool isShapeshiftShown = false;
    if (isInForm == false)
    {
        Unit::AuraEffectList const& shapeshiftAuras = player->GetAuraEffectsByType(SPELL_AURA_MOD_SHAPESHIFT);
        if (shapeshiftAuras.empty() == false && player->GetModelForForm(player->GetShapeshiftForm(), shapeshiftAuras.front()->GetId()) != 0)
            isShapeshiftShown = true;
    }
    if (isShapeshiftShown == true)
    {
        RefreshIllusionHead(player, nullptr);
        RefreshIllusionHeadgear(player, nullptr);
    }
    else
        RefreshIllusionDisplay(player);

    // On a ground mount out of Midgard the body wanted may be the job's on its Peco (GetWorldIdentityRagnarokLookSpellID).  While that body is the
    // one showing, the mount's own model isn't shown; under a look put on by hand or a shapeshift form, and for any other look, it is
    bool isPecoShown = false;
    if (wantedIllusionSpellID != 0 && isInForm == false && isShapeshiftShown == false && player->getTransForm() == wantedIllusionSpellID)
    {
        auto illusionIter = IllusionsBySpellID.find(wantedIllusionSpellID);
        isPecoShown = illusionIter != IllusionsBySpellID.end() && illusionIter->second.IsRiding == true;
    }
    RefreshWorldIdentityMount(player, isPecoShown);
}

Aura* ROWOWMod::AddWorldIdentityAura(Player* player, uint32 spellID)
{
    // Unit::AddAura by its spell: the overload that also takes a dead target (a look is put back on a character that logs in dead)
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellID);
    if (spellInfo == nullptr)
        return nullptr;
    // The core's apply runs ROWOW_UnitScript::OnAuraApply, which refreshes the sprite's display before the aura can be hidden below: the
    // headgear is the identity's all the same (RefreshIllusionHeadgear).  Taken for a look put on by hand, the Ragnarok Online character's
    // headgear aura came off and went on again at every change of body, and a client still loading the new body lost it
    ROWOWWorldIdentityState* state = GetWorldIdentityState(player);
    state->LookSpellIDBeingAdded = spellID;
    Aura* aura = player->AddAura(spellInfo, MAX_EFFECT_MASK, player);
    state->LookSpellIDBeingAdded = 0;
    if (aura != nullptr)
        aura->SetClientHidden();
    return aura;
}

void ROWOWMod::UpdateWorldIdentity(Player* player, uint32 diffInMS)
{
    // Once a second: the look kept as the identity wants it, Worldwalker opened for a character that just earned it in Midgard, and the client told
    // when anything it shows changed (the world, a choice, a lock)
    if (IsEnabled == false || player == nullptr || player->IsInWorld() == false || player->IsBeingTeleported() == true)
        return;
    ROWOWWorldIdentityState* state = GetWorldIdentityState(player);
    state->CheckTimer += diffInMS;
    // Getting on or off a WoW mount, or from one mount onto another, changes the look at once (a Peco rider's body, the cart, the Peco in a
    // ground mount's place)
    uint32 mountSpellID = GetMountAuraSpellID(player);
    if (state->IsOnMount != player->IsMounted() || state->MountSpellID != mountSpellID)
    {
        state->IsOnMount = player->IsMounted();
        state->MountSpellID = mountSpellID;
        state->CheckTimer = ROWOW_WORLD_IDENTITY_CHECK_TIME_IN_MS;
    }
    // The first update on a map arrived on: the look's display taken back from the core's arrival handling (RefreshWorldIdentityLook), in the
    // same server update, so the clients are never sent the other display
    if (state->IsArrivalCheckPending == true)
    {
        state->IsArrivalCheckPending = false;
        state->CheckTimer = ROWOW_WORLD_IDENTITY_CHECK_TIME_IN_MS;
    }
    // A look the identity had on is gone (the secondary class change's profile swap takes every aura that isn't saved, as a dispel may), or a look
    // put on by hand over it has ended: made right now.  This runs at the end of the player's update, so in the one that took the look (the client
    // never sees the character without it), or at the latest in the next
    if ((state->LookIllusionSpellID != 0 && player->HasAura(state->LookIllusionSpellID) == false)
        || (state->LookEverQuestSpellID != 0 && player->HasAura(state->LookEverQuestSpellID) == false)
        || (state->HasByHandLook == true && HasByHandLook(player) == false))
    {
        state->LookIllusionSpellID = 0;
        state->LookEverQuestSpellID = 0;
        state->HasByHandLook = false;
        state->CheckTimer = ROWOW_WORLD_IDENTITY_CHECK_TIME_IN_MS;
    }
    if (state->CheckTimer < ROWOW_WORLD_IDENTITY_CHECK_TIME_IN_MS)
        return;
    state->CheckTimer = 0;
    if (state->IsWorldwalkerOpen == false && IsInROForm(player) == true && HasMidgardWorldwalker(player) == true)
        OpenWorldwalker(player);
    if (IsInROForm(player) == true && HasMidgardWayfarer(player) == true)
        OpenWayfarerCompanions(player);
    RefreshWorldIdentityLook(player);
    SendWorldIdentityToClient(player, false);
}

void ROWOWMod::SendWorldIdentityToClient(Player* player, bool isForced)
{
    // ROWOWIDENT: "<world the character is in: 0 Azeroth, 1 Norrath, 2 Midgard><tab><the identity shown in Azeroth, in Norrath and in Midgard, a digit
    // each: 0 Warcraft, 1 EverQuest, 2 Ragnarok><tab><nine lock letters, the three identities of Azeroth, then Norrath's, then Midgard's: 0 may be
    // picked, P needs Midgard Pathfinder, W needs Midgard Wayfarer, C needs a Ragnarok Online character, E the character has no EverQuest identity,
    // B not in a battleground or arena, F not in combat><tab><1 while Midgard Worldwalker has the character's own abilities open in the RO form,
    // else 0>".  Sent when asked for (.identity), each time the client's interface has loaded (ROWOW_IdentityServerScript), on every arrival and
    // whenever it changes
    if (player == nullptr || player->GetSession() == nullptr)
        return;
    ROWOWWorldIdentityState* state = GetWorldIdentityState(player);
    std::string shown;
    std::string locks;
    for (uint8 world = 0; world < ROWOW_WORLD_COUNT; world++)
    {
        shown += char('0' + GetWorldIdentity(player, world));
        for (uint8 worldIdentity = 0; worldIdentity < ROWOW_WORLD_IDENTITY_COUNT; worldIdentity++)
            locks += GetWorldIdentityChangeLock(player, world, worldIdentity);
    }
    std::string addonMessage = fmt::format("ROWOWIDENT\t{}\t{}\t{}\t{}", GetWorldOfMap(player->GetMapId()), shown, locks, IsWorldwalkerOpen(player) == true ? 1 : 0);
    if (isForced == false && addonMessage == state->SentMessage)
        return;
    state->SentMessage = addonMessage;
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_ADDON, nullptr, nullptr, addonMessage);
    player->SendDirectMessage(&data);
}

bool ROWOWMod::IsWorldwalkerOpen(Player const* player) const
{
    // In the RO form with Midgard Worldwalker: the character's own spells stay known and may be cast (EnterROForm, OpenWorldwalker)
    if (player == nullptr || IsInROForm(player) == false)
        return false;
    ROWOWWorldIdentityState const* state = player->CustomData.Get<ROWOWWorldIdentityState>("ROWOWWorldIdentity");
    return state != nullptr && state->IsWorldwalkerOpen == true;
}

void ROWOWMod::DeleteWorldIdentityRows(ObjectGuid::LowType guid, CharacterDatabaseTransaction trans)
{
    trans->Append("DELETE FROM `mod_rowow_world_identity` WHERE `guid` = {}", guid);
}

void ROWOWMod::DressSpriteCorpse(Corpse* corpse)
{
    // The corpse of a character that died showing an RO job sprite (the Ragnarok identity out of Midgard: an RO death leaves no corpse) is that
    // sprite lying dead: mod-everquest gives a corpse the display its character showed.  A corpse can have no auras, and the sprite's head and
    // headgear are auras' looks, so it lay there headless.  The sprite has no use for the corpse's item fields (the character's own gear, which
    // dresses a character model), so they name the head's and headgear's spells instead, and the client plays those looks on the corpse
    // (CGCorpse_C::PlayItemFieldSpellLooks).  This runs as the corpse is put on its map, after Player::CreateCorpse has filled its fields and before
    // it is saved or sent to anyone, on the owner's own map thread
    if (corpse == nullptr || corpse->GetType() == CORPSE_BONES)
        return;
    Player* owner = ObjectAccessor::FindPlayer(corpse->GetOwnerGUID());
    if (owner == nullptr || owner->IsAlive() == true || owner->GetMap() != corpse->GetMap())
        return;
    if (GetActiveIllusion(owner) == nullptr)
        return;
    uint32 corpseDisplayID = corpse->GetUInt32Value(CORPSE_FIELD_DISPLAY_ID);
    if (corpseDisplayID != owner->GetDisplayId())
    {
        // A character that died on a ground mount shown as its Peco: the display its corpse was given is the one it showed as it died (the body on
        // its Peco), and the body has been the one on foot since the death took it off the mount.  The corpse lies as the body does: on foot
        bool isIllusionDisplay = false;
        for (auto const& illusionBySpellID : IllusionsBySpellID)
            for (auto const& displayIDByKey : illusionBySpellID.second.DisplayIDsByKey)
                if (displayIDByKey.second == corpseDisplayID)
                    isIllusionDisplay = true;
        if (isIllusionDisplay == false)
            return;
        corpse->SetUInt32Value(CORPSE_FIELD_DISPLAY_ID, owner->GetDisplayId());
    }
    for (uint8 slot = 0; slot < EQUIPMENT_SLOT_END; slot++)
        corpse->SetUInt32Value(CORPSE_FIELD_ITEM + slot, 0);
    uint8 nextSlot = 0;
    for (AuraEffect const* auraEffect : owner->GetAuraEffectsByType(SPELL_AURA_DUMMY))
    {
        uint32 spellID = auraEffect->GetId();
        if (HeadSpellIDs.find(spellID) == HeadSpellIDs.end() && HeadgearSpellIDs.find(spellID) == HeadgearSpellIDs.end())
            continue;
        if (nextSlot < EQUIPMENT_SLOT_END)
            corpse->SetUInt32Value(CORPSE_FIELD_ITEM + nextSlot++, spellID);
    }
}

class ROWOW_WorldIdentityWorldObjectScript : public WorldObjectScript
{
public:
    ROWOW_WorldIdentityWorldObjectScript() : WorldObjectScript("ROWOW_WorldIdentityWorldObjectScript", { WORLDOBJECTHOOK_ON_WORLD_OBJECT_SET_MAP }) {}

    void OnWorldObjectSetMap(WorldObject* object, Map* /*map*/) override
    {
        if (ROWOW->IsEnabled == false || object == nullptr || object->GetTypeId() != TYPEID_CORPSE)
            return;
        ROWOW->DressSpriteCorpse(object->ToCorpse());
    }
};

// .identity: the client's way to ask for and set the identities (the unit frame's menu, Options > Interface > Display).  Any player's
class ROWOW_WorldIdentityCommandScript : public CommandScript
{
public:
    ROWOW_WorldIdentityCommandScript() : CommandScript("ROWOW_WorldIdentityCommandScript") { }

    Acore::ChatCommands::ChatCommandTable GetCommands() const override
    {
        static Acore::ChatCommands::ChatCommandTable commandTable =
        {
            { "identity", HandleIdentityCommand, SEC_PLAYER, Acore::ChatCommands::Console::No },
        };
        return commandTable;
    }

    static bool HandleIdentityCommand(ChatHandler* handler, Optional<std::string> worldText, Optional<std::string> identityText)
    {
        // ".identity": the ROWOWIDENT message again.  ".identity <world> <identity>": by name (or its start) or number
        Player* player = handler->GetSession() != nullptr ? handler->GetSession()->GetPlayer() : nullptr;
        if (player == nullptr || ROWOW->IsEnabled == false)
            return false;
        if (worldText.has_value() == false)
        {
            ROWOW->SendWorldIdentityToClient(player, true);
            return true;
        }
        uint8 world = ROWOW_WORLD_COUNT;
        uint8 worldIdentity = ROWOW_WORLD_IDENTITY_COUNT;
        for (uint8 i = 0; i < ROWOW_WORLD_COUNT; i++)
            if (IsNameMatch(*worldText, WorldNames[i]) == true || *worldText == std::to_string(i))
                world = i;
        if (identityText.has_value() == true)
            for (uint8 i = 0; i < ROWOW_WORLD_IDENTITY_COUNT; i++)
                if (IsNameMatch(*identityText, WorldIdentityNames[i]) == true || *identityText == std::to_string(i))
                    worldIdentity = i;
        std::string error;
        if (ROWOW->SetWorldIdentity(player, world, worldIdentity, error) == false)
        {
            handler->SendSysMessage(error);
            ROWOW->SendWorldIdentityToClient(player, true); // The menu and the options show what's kept again
        }
        return true; // Answered above (false would add the command's usage lines)
    }
};

void AddROWOWWorldIdentityScripts()
{
    new ROWOW_WorldIdentityCommandScript();
    new ROWOW_WorldIdentityWorldObjectScript();
}
