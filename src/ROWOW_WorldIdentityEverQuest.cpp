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


#include "Item.h"
#include "ObjectMgr.h"
#include "Player.h"

#include "ROWOW.h"

// The one place mod-rowow reads (and calls) mod-everquest (the identities' EverQuest side).  The two modules are built into the same library, so this file
// includes mod-everquest's header when it is in the source tree, and without it the EverQuest identity and the world Norrath simply don't exist:
// mod-rowow still builds and runs alone.  Nothing in mod-everquest knows of this
#if __has_include("EverQuest.h")
#include "EverQuest.h"
#define ROWOW_HAS_MOD_EVERQUEST 1
#else
#define ROWOW_HAS_MOD_EVERQUEST 0
#endif

bool ROWOWMod::IsNorrathMap(uint32 mapID) const
{
#if ROWOW_HAS_MOD_EVERQUEST
    return EverQuest->IsEnabled == true && EverQuest->IsMapIDAnEverQuestMap(mapID) == true;
#else
    (void)mapID;
    return false;
#endif
}

uint32 ROWOWMod::GetEverQuestIdentityLookSpellID(Player* player) const
{
    // THE SEAM FOR THE EQWOW-SoL MERGE: "which EverQuest look does this character have", as the spell whose aura shows it (an EverQuest illusion form
    // spell, which mod-everquest dresses as it dresses any: its aura hooks do the rest).  0 for none.
    // The racial guise of the character's EverQuest race is the look: its item teaches a spell whose form spells (male and female) are the look.  The race
    // is the one picked on the EverQuest page of character creation (EverQuestMod::GetPlayerCreateInfoForPlayer goes by its eqRace start), and a character
    // from before that page goes by the guise its WoW race and class are given (mod_everquest_playercreateinfo.illusionitem)
#if ROWOW_HAS_MOD_EVERQUEST
    if (player == nullptr || EverQuest->IsEnabled == false)
        return 0;
    if (EverQuest->HasCreatePlayerData(player->getRace(true), player->getClass()) == false)
        return 0;
    ItemTemplate const* guiseItemTemplate = sObjectMgr->GetItemTemplate(EverQuest->GetPlayerCreateInfoForPlayer(player).IllusionItemID);
    if (guiseItemTemplate == nullptr)
        return 0;
    for (uint8 spellIndex = 0; spellIndex < MAX_ITEM_PROTO_SPELLS; spellIndex++)
    {
        int32 guiseSpellID = guiseItemTemplate->Spells[spellIndex].SpellId;
        if (guiseSpellID <= 0 || EverQuest->IsSpellAnEQSpell(uint32(guiseSpellID)) == false)
            continue;
        EverQuestSpell const& guiseSpell = EverQuest->GetSpellDataForSpellID(uint32(guiseSpellID));
        uint32 formSpellID = ROWOWGetOwnGender(player) == GENDER_MALE ? guiseSpell.MaleFormSpellID : guiseSpell.FemaleFormSpellID;
        if (formSpellID != 0)
            return formSpellID;
    }
    return 0;
#else
    (void)player;
    return 0;
#endif
}

void ROWOWMod::OnEverQuestIdentityLookHidden(Player* player)
{
    // An identity's look is only a look: mod-everquest's faction reactions (an illusion form makes Norrath's factions take the character for the
    // form's race and alignment) skip a client hidden aura.  But the aura is hidden only after it is on (AddWorldIdentityAura), and mod-everquest
    // worked the reactions out as it went on, so they are worked out again here
#if ROWOW_HAS_MOD_EVERQUEST
    if (player != nullptr && EverQuest->IsEnabled == true)
        EverQuest->RecalculateTemporaryFactionReactionsForPlayer(player);
#else
    (void)player;
#endif
}

void ROWOWMod::RefreshEverQuestIdentityLookDisplay(Player* player)
{
    // The EverQuest look's display taken back when something else has set the character's display under it.  The core does at every arrival: once
    // the character is on its map it handles its first transform aura again "for the client" (Player::SendInitialPacketsAfterAddToMap), which
    // sets the form spell's own creature display over the dressed character display mod-everquest gave the form.  mod-everquest takes its
    // display back at its next gear check (EverQuest.Illusion.GearRefreshTimeInMS, a second), by when every client has been sent the other
    // display and loads the character's model twice over.  Asked for in the arrival's own update (UpdateWorldIdentity), the display is the one
    // the clients were sent with the character before any of them is sent the other
#if ROWOW_HAS_MOD_EVERQUEST
    if (player != nullptr && EverQuest->IsEnabled == true)
        EverQuest->RefreshIllusionGearDisplayForPlayer(player);
#else
    (void)player;
#endif
}

void ROWOWMod::EndEverQuestMentorshipForROWorld(Player* player)
{
    // No mentorship in the RO world (Nathan 2026-11-14): it's ended before the trip there, while the character still stands on its Warcraft map.
    // The arrival keeps the character's level to give back on leaving (EnterROForm's WoWLevel), so a borrowed level has to be handed back first,
    // or the character would come back out at it.  The partner's half ends at their next check.  mod-everquest refuses to start one in the form
#if ROWOW_HAS_MOD_EVERQUEST
    if (player != nullptr && EverQuest->IsEnabled == true)
        EverQuest->EndMentorshipForPlayer(player, "a mentorship cannot be carried into Midgard", true);
#else
    (void)player;
#endif
}
