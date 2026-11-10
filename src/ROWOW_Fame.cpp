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
//  Ported from rAthena (https://github.com/rathena/rathena), src/map/pc.cpp (pc_addfame, pc_famerank), src/char/char.cpp (the fame lists):
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL.  For more information, see LICENCE in rAthena's main folder

#include "Chat.h"
#include "DatabaseEnv.h"
#include "Player.h"
#include "QueryResult.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include "ROWOW.h"
#include "ROWOW_JobMapIDs.h"

using namespace std;

// Fame: the rankings of Blacksmiths, Alchemists and Taekwons.  A character's points are on its identity (status.fame, saved with it); the ten
// best of each kind are FameLists, which the char server keeps in rAthena: built at startup from the identities (chrif_buildfamelist), then
// updated by each pc_addfame, so the lists are shared by every map's thread and read and written under FameMutex.  A ranker's Forged weapons hit
// harder, its potions heal more, and a Taekwon ranker of level 90 gets its whole tree and three times its HP and SP.  /blacksmith, /alchemist
// and /taekwon (RO_Identity_Ranking.lua) ask ".ro ranking <kind>"

namespace
{
    uint64 const MapIDBlacksmith = 0x105; // MAPID_BLACKSMITH
    uint64 const MapIDAlchemist = 0x205; // MAPID_ALCHEMIST
    uint64 const MapIDTaekwon = 0x7; // MAPID_TAEKWON
    int32 const MaxFame = 1000000000; // MAX_FAME
    size_t const FameMessageChunkLength = 200;

    uint64 GetFameMapID(int32 fameType)
    {
        return fameType == ROWOW_FAME_BLACKSMITH ? MapIDBlacksmith : (fameType == ROWOW_FAME_ALCHEMIST ? MapIDAlchemist : MapIDTaekwon);
    }

    // clif_update_rankingpoint's line
    char const* GetFamePointFormat(int32 fameType)
    {
        switch (fameType)
        {
            case ROWOW_FAME_BLACKSMITH: return "[POINT] Blacksmith Rank Point has risen by {}.  The total is {} points.";
            case ROWOW_FAME_ALCHEMIST: return "[POINT] Alchemist Rank Point has risen by {}.  The total is {} points.";
            default: return "[POINT] Taekwon Mission Rank Point has risen by {}.  The total is {} points.";
        }
    }
}

int32 ROWOWMod::GetFameType(uint16 jobID) const
{
    // (class_ & MAPID_SECONDMASK): each line's jobs, its transcendent and Baby jobs too.  -1 for none
    int64 mapID = PCJobIDToMapID(jobID);
    if (mapID < 0)
        return -1;
    for (int32 fameType = 0; fameType < ROWOW_FAME_COUNT; fameType++)
        if ((uint64(mapID) & ROWOW_MAPID_SECONDMASK) == GetFameMapID(fameType))
            return fameType;
    return -1;
}

void ROWOWMod::LoadFameLists(ObjectGuid::LowType skippedGUID)
{
    // chrif_buildfamelist / char_read_fame_list: each kind's ten best from the identities, by the job each last saved (the job's real class:
    // char_read_fame_list names the Star Gladiator's and the Baby's apart, and only Taekwons rank as Taekwons).  A player that just changed job
    // is left out of the query, as its row still holds the old one
    std::array<std::vector<ROWOWFameEntry>, ROWOW_FAME_COUNT> fameLists;
    for (int32 fameType = 0; fameType < ROWOW_FAME_COUNT; fameType++)
    {
        std::string jobIDs;
        for (ROWOWJobMapID const& jobMapID : ROWOWJobMapIDs)
            if ((uint64(jobMapID.MapID) & ROWOW_MAPID_SECONDMASK) == GetFameMapID(fameType))
                jobIDs += fmt::format("{}{}", jobIDs.empty() == true ? "" : ",", jobMapID.JobID);
        // An identity in another save slot than the first is keyed by its own ID: its character is its slot's (the lists name the character, as forged items do)
        QueryResult result = CharacterDatabase.Query("SELECT c.`guid`, i.`fame`, c.`name` FROM `mod_rowow_identity` i LEFT JOIN `mod_rowow_identity_slot` s ON s.`identity` = i.`guid` "
            "JOIN `characters` c ON c.`guid` = COALESCE(s.`guid`, i.`guid`) WHERE i.`fame` > 0 AND i.`job` IN ({}) AND c.`guid` <> {} ORDER BY i.`fame` DESC LIMIT {}",
            jobIDs, skippedGUID, ROWOW_MAX_FAME_LIST);
        if (!result)
            continue;
        do
        {
            Field* fields = result->Fetch();
            ROWOWFameEntry entry;
            entry.CharacterGUID = fields[0].Get<uint32>();
            entry.Fame = fields[1].Get<int32>();
            entry.Name = fields[2].Get<std::string>();
            fameLists[fameType].push_back(entry);
        } while (result->NextRow());
    }
    std::lock_guard<std::mutex> lock(FameMutex);
    FameLists = fameLists;
}

void ROWOWMod::UpdateFameList(int32 fameType, ObjectGuid::LowType characterGUID, int32 fame, std::string const& name)
{
    // The char server's fame list update (0x2b10): out of its place, then back in by its points, the ten best kept
    std::lock_guard<std::mutex> lock(FameMutex);
    std::vector<ROWOWFameEntry>& fameList = FameLists[fameType];
    fameList.erase(std::remove_if(fameList.begin(), fameList.end(), [characterGUID](ROWOWFameEntry const& entry) { return entry.CharacterGUID == characterGUID; }), fameList.end());
    if (fame <= 0)
        return;
    ROWOWFameEntry entry;
    entry.CharacterGUID = characterGUID;
    entry.Fame = fame;
    entry.Name = name;
    auto placeIter = std::find_if(fameList.begin(), fameList.end(), [fame](ROWOWFameEntry const& other) { return other.Fame < fame; });
    fameList.insert(placeIter, entry);
    if (fameList.size() > ROWOW_MAX_FAME_LIST)
        fameList.resize(ROWOW_MAX_FAME_LIST);
}

bool ROWOWMod::PCAddFame(Player* player, int32 count)
{
    // pc_addfame, clif_update_rankingpoint and chrif_updatefamelist.  False for a job with no ranking
    ROWOWIdentity* identity = GetIdentity(player);
    int32 fameType = GetFameType(identity->JobID);
    if (fameType < 0)
        return false;
    identity->Fame = std::clamp<int64>(int64(identity->Fame) + count, 0, MaxFame);
    ChatHandler(player->GetSession()).PSendSysMessage(GetFamePointFormat(fameType), count, identity->Fame);
    UpdateFameList(fameType, player->GetGUID().GetCounter(), identity->Fame, player->GetName());
    return true;
}

uint8 ROWOWMod::PCFameRank(ObjectGuid::LowType characterGUID, int32 fameType) const
{
    // pc_famerank: its place in the list, 1 the best, 0 when it isn't in it
    if (characterGUID == 0)
        return 0;
    std::lock_guard<std::mutex> lock(FameMutex);
    std::vector<ROWOWFameEntry> const& fameList = FameLists[fameType];
    for (size_t index = 0; index < fameList.size(); index++)
        if (fameList[index].CharacterGUID == characterGUID)
            return uint8(index + 1);
    return 0;
}

bool ROWOWMod::PCIsTaekwonRanker(ROWOWIdentity const& identity) const
{
    // pc_is_taekwon_ranker: a Taekwon of the Fame Top 10 at taekwon_ranker_min_lv
    return GetFameType(identity.JobID) == ROWOW_FAME_TAEKWON && int32(identity.BaseLevel) >= BattleConfig.TaekwonRankerMinLv && PCFameRank(identity.CharacterGUID, ROWOW_FAME_TAEKWON) != 0;
}

void ROWOWMod::ClifSubRanklist(Player* player, int32 fameType)
{
    // clif_sub_ranklist: a ranking for the RO_Identity window, "ROWOWRANK<tab>B<tab><kind>", "S<tab><name>:<points>,...", "E"
    auto send = [player](std::string const& message)
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_ADDON, nullptr, nullptr, "ROWOWRANK\t" + message);
        player->SendDirectMessage(&data);
    };
    std::vector<ROWOWFameEntry> fameList;
    {
        std::lock_guard<std::mutex> lock(FameMutex);
        fameList = FameLists[fameType];
    }
    send(fmt::format("B\t{}", fameType));
    std::string chunk;
    for (ROWOWFameEntry const& entry : fameList)
    {
        std::string text = fmt::format("{}:{}", entry.Name, entry.Fame);
        if (chunk.empty() == false && chunk.size() + 1 + text.size() > FameMessageChunkLength)
        {
            send("S\t" + chunk);
            chunk.clear();
        }
        chunk += (chunk.empty() == true ? "" : ",") + text;
    }
    if (chunk.empty() == false)
        send("S\t" + chunk);
    send("E");
}
