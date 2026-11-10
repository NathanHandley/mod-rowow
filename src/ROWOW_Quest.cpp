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
//  The quest log follows rAthena's quest.cpp (and solve_time from common/timer.cpp):
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
//  For more information, see LICENCE in the main folder

#include "Chat.h"
#include "Group.h"
#include "Log.h"
#include "Player.h"
#include "Timer.h"
#include "WorldPacket.h"

#include "ROWOW.h"

#include <ctime>

// The quest log (R8): quest_db (mod_rowow_db_quest, from RoQuests.csv) read once at startup, and each character's quests in its identity
// (mod_rowow_identity_quest), changed by the NPC scripts' quest commands (setquest, changequest, completequest, erasequest) and by kills of
// a hunting quest's monsters.  The RO_Identity addon's quest window lists the active and inactive ones (ROWOWQUEST, see SendQuestsToClient).
// A character's quest log is only touched in its own thread: its runs, its session's commands, and kills on its map

namespace
{
    // solve_time: the seconds from now to now plus "+1d2h30mn10s" (calendar math, as mktime does it)
    int64 SolveTime(std::string const& text)
    {
        time_t now = time(nullptr);
        tm thenTime = Acore::Time::TimeBreakdown(now);
        char const* modifier = text.c_str();
        while (modifier[0] != '\0')
        {
            int32 value = atoi(modifier);
            if (value == 0)
            {
                modifier++;
                continue;
            }
            if (modifier[0] == '-' || modifier[0] == '+')
                modifier++;
            while (modifier[0] >= '0' && modifier[0] <= '9')
                modifier++;
            if (modifier[0] == 's')
            {
                thenTime.tm_sec += value;
                modifier++;
            }
            else if (modifier[0] == 'n')
            {
                thenTime.tm_min += value;
                modifier++;
            }
            else if (modifier[0] == 'm' && modifier[1] == 'n')
            {
                thenTime.tm_min += value;
                modifier += 2;
            }
            else if (modifier[0] == 'h')
            {
                thenTime.tm_hour += value;
                modifier++;
            }
            else if (modifier[0] == 'd' || modifier[0] == 'j')
            {
                thenTime.tm_mday += value;
                modifier++;
            }
            else if (modifier[0] == 'm')
            {
                thenTime.tm_mon += value;
                modifier++;
            }
            else if (modifier[0] == 'y' || modifier[0] == 'a')
            {
                thenTime.tm_year += value;
                modifier++;
            }
            else if (modifier[0] != '\0')
                modifier++;
        }
        return int64(difftime(mktime(&thenTime), now));
    }

    // split_exact_quest_time: a time of day ("4h30mn", "Monday4h"), false when it has no hour
    bool SplitExactQuestTime(std::string const& text, int32& week, int32& day, int32& hour, int32& minute, int32& second)
    {
        int32 w = -1, d = -1, h = -1, mn = -1, s = -1;
        static char const* const DAY_NAMES[7] = { "SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY" };
        char const* modifier = text.c_str();
        while (modifier[0] != '\0')
        {
            int32 value = atoi(modifier);
            if (modifier[0] == '-' || modifier[0] == '+')
                modifier++;
            while (modifier[0] >= '0' && modifier[0] <= '9')
                modifier++;
            bool isDay = false;
            for (int32 dayIndex = 0; dayIndex < 7; dayIndex++)
            {
                size_t length = strlen(DAY_NAMES[dayIndex]);
                size_t matchLength = 0;
                while (matchLength < length && toupper((unsigned char)modifier[matchLength]) == DAY_NAMES[dayIndex][matchLength])
                    matchLength++;
                if (matchLength == length)
                {
                    w = dayIndex;
                    modifier += length;
                    isDay = true;
                    break;
                }
            }
            if (isDay == true)
                continue;
            if (modifier[0] == 's')
            {
                s = value;
                modifier++;
            }
            else if (modifier[0] == 'm' && modifier[1] == 'n')
            {
                mn = value;
                modifier += 2;
            }
            else if (modifier[0] == 'h')
            {
                h = value;
                modifier++;
            }
            else if (modifier[0] == 'd' || modifier[0] == 'j')
            {
                d = value;
                modifier++;
            }
            else if (modifier[0] != '\0')
                modifier++;
        }
        if (h < 0 || h > 23 || mn > 59 || s > 59) // hour is required
            return false;
        week = w;
        day = std::max(0, d);
        hour = h;
        minute = std::max(0, mn);
        second = std::max(0, s);
        return true;
    }

    // quest_time: when a quest taken now runs out, 0 for never
    time_t QuestTime(ROWOWQuestData const& questData)
    {
        if (questData.Time < 0)
            return 0;
        if (questData.IsTimeAt == false && questData.Time > 0)
            return time(nullptr) + questData.Time;
        if (questData.IsTimeAt == true)
        {
            time_t now = time(nullptr);
            tm localTime = Acore::Time::TimeBreakdown(now);
            int64 timeToday = localTime.tm_hour * 3600 + localTime.tm_min * 60 + localTime.tm_sec;
            int32 dayShift = 0;
            if (timeToday >= (questData.Time % 86400)) // Carry over to the next day
                dayShift = 1;
            if (questData.TimeWeek > -1)
            {
                if (questData.TimeWeek < (localTime.tm_wday + dayShift))
                    dayShift = questData.TimeWeek + 7 - localTime.tm_wday;
                else
                    dayShift = questData.TimeWeek - localTime.tm_wday;
            }
            return time_t(now + (dayShift * 86400) + questData.Time - timeToday);
        }
        return 0;
    }

    // Cuts a text for an addon message field: no tabs, at most maxLength bytes, never inside a UTF-8 character
    std::string GetMessageField(std::string const& text, size_t maxLength)
    {
        std::string field = text;
        for (char& curChar : field)
            if (curChar == '\t' || curChar == '\n' || curChar == '\r')
                curChar = ' ';
        if (field.size() <= maxLength)
            return field;
        size_t length = maxLength;
        while (length > 0 && (uint8(field[length]) & 0xC0) == 0x80)
            length--;
        return field.substr(0, length);
    }

    void SendQuestMessage(Player* player, std::string const& message)
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_ADDON, nullptr, nullptr, "ROWOWQUEST\t" + message);
        player->SendDirectMessage(&data);
    }
}

void ROWOWMod::LoadQuestData()
{
    // QuestDatabase::parseBodyNode: Title, TimeLimit ("+<time>" from when it's taken, or a time of day) and the hunting Targets (Mob, Count)
    QuestsByID.clear();
    ROWOWDataTable const* questTable = GetDataTable("mod_rowow_db_quest");
    ROWOWDataTable const* mobTable = GetDataTable(ROWOW_DATA_TABLE_MOB);
    if (questTable == nullptr || mobTable == nullptr)
    {
        LOG_ERROR("module.ROWOW", "ROWOWMod::LoadQuestData found no mod_rowow_db_quest or mob table, so the NPC scripts' quests can't be given");
        return;
    }
    std::unordered_map<std::string, uint32> mobIDsByAegisName; // mobdb_search_aegisname (not case sensitive)
    int32 mobIDColumnIndex = mobTable->GetColumnIndex("Id");
    int32 mobAegisNameColumnIndex = mobTable->GetColumnIndex("AegisName");
    for (size_t rowIndex = 0; rowIndex < mobTable->Rows.size(); rowIndex++)
    {
        std::string aegisName = mobTable->GetCell(rowIndex, mobAegisNameColumnIndex);
        for (char& curChar : aegisName)
            curChar = char(toupper((unsigned char)curChar));
        mobIDsByAegisName[aegisName] = uint32(mobTable->GetInt(rowIndex, mobIDColumnIndex, 0));
    }

    int32 idColumnIndex = questTable->GetColumnIndex("Id");
    int32 titleColumnIndex = questTable->GetColumnIndex("Title");
    int32 timeLimitColumnIndex = questTable->GetColumnIndex("TimeLimit");
    int32 targetsColumnIndex = questTable->GetColumnIndex("Targets");
    uint32 skippedCount = 0;
    uint32 huntingCount = 0;
    for (size_t rowIndex = 0; rowIndex < questTable->Rows.size(); rowIndex++)
    {
        ROWOWQuestData questData;
        questData.ID = uint32(questTable->GetInt(rowIndex, idColumnIndex, 0));
        if (questData.ID == 0)
            continue;
        questData.Title = ROWOWDataTable::UnescapeText(questTable->GetCell(rowIndex, titleColumnIndex));
        std::string const& timeLimit = questTable->GetCell(rowIndex, timeLimitColumnIndex);
        if (timeLimit.empty() == false)
        {
            if (timeLimit.find('+') != std::string::npos)
            {
                int64 timeDifference = SolveTime(timeLimit);
                if (timeDifference <= 0)
                {
                    LOG_ERROR("module.ROWOW", "ROWOWMod::LoadQuestData: quest {} has an incorrect TimeLimit '{}', skipping", questData.ID, timeLimit);
                    skippedCount++;
                    continue;
                }
                questData.Time = timeDifference;
            }
            else
            {
                // '+' not found, set to specific time
                int32 day = 0, hour = 0, minute = 0, second = 0, week = -1;
                if (SplitExactQuestTime(timeLimit, week, day, hour, minute, second) == false)
                {
                    LOG_ERROR("module.ROWOW", "ROWOWMod::LoadQuestData: quest {} has an incorrect TimeLimit '{}', skipping", questData.ID, timeLimit);
                    skippedCount++;
                    continue;
                }
                questData.Time = week > 0 ? hour * 3600 + minute * 60 + second : day * 86400 + hour * 3600 + minute * 60 + second;
                questData.IsTimeAt = true;
                questData.TimeWeek = week;
            }
        }
        for (std::vector<std::pair<std::string, std::string>> const& target : ROWOWDataTable::ParseMapList(questTable->GetCell(rowIndex, targetsColumnIndex)))
        {
            ROWOWQuestObjective objective;
            for (std::pair<std::string, std::string> const& field : target)
            {
                if (field.first == "Mob")
                {
                    std::string aegisName = field.second;
                    for (char& curChar : aegisName)
                        curChar = char(toupper((unsigned char)curChar));
                    auto mobIDIter = mobIDsByAegisName.find(aegisName);
                    objective.MobID = mobIDIter != mobIDsByAegisName.end() ? mobIDIter->second : 0;
                }
                else if (field.first == "Count")
                    objective.Count = int32(strtol(field.second.c_str(), nullptr, 10));
            }
            if (objective.MobID == 0 || questData.Objectives.size() >= ROWOW_MAX_QUEST_OBJECTIVES)
                continue;
            // A second target of the same monster adds to its count
            auto existingIter = std::find_if(questData.Objectives.begin(), questData.Objectives.end(), [&objective](ROWOWQuestObjective const& existing)
            {
                return existing.MobID == objective.MobID;
            });
            if (existingIter != questData.Objectives.end())
                existingIter->Count += objective.Count;
            else
                questData.Objectives.push_back(objective);
        }
        if (questData.Objectives.empty() == false)
            huntingCount++;
        QuestsByID[questData.ID] = std::move(questData);
    }
    LOG_INFO("module.ROWOW", "ROWOWMod::LoadQuestData loaded {} quests ({} with hunting targets), skipped {}", QuestsByID.size(), huntingCount, skippedCount);
}

ROWOWQuestData const* ROWOWMod::QuestSearch(uint32 questID) const
{
    auto questIter = QuestsByID.find(questID);
    return questIter != QuestsByID.end() ? &questIter->second : nullptr;
}

int32 ROWOWMod::QuestAdd(Player* sd, uint32 questID)
{
    // quest_add: the new quest goes in active, before the completed ones
    ROWOWQuestData const* questData = QuestSearch(questID);
    if (questData == nullptr)
    {
        LOG_ERROR("module.ROWOW", "ROWOWMod::QuestAdd: quest {} not found in DB", questID);
        return -1;
    }
    ROWOWIdentity* identity = GetIdentity(sd);
    if (QuestCheck(*identity, questID, ROWOW_QUEST_CHECK_HAVEQUEST) >= 0)
    {
        LOG_ERROR("module.ROWOW", "ROWOWMod::QuestAdd: character {} already has quest {}", sd->GetGUID().GetCounter(), questID);
        return -1;
    }
    ROWOWQuestEntry entry;
    entry.QuestID = questID;
    entry.Time = uint32(QuestTime(*questData));
    entry.State = ROWOW_QUEST_STATE_ACTIVE;
    auto firstCompleteIter = std::find_if(identity->Quests.begin(), identity->Quests.end(), [](ROWOWQuestEntry const& quest)
    {
        return quest.State == ROWOW_QUEST_STATE_COMPLETE;
    });
    identity->Quests.insert(firstCompleteIter, entry);
    identity->AreQuestsChanged = true;
    SendQuestsToClient(sd);
    return 0;
}

int32 ROWOWMod::QuestChange(Player* sd, uint32 qid1, uint32 qid2)
{
    // quest_change: the new quest takes the old one's place, active, its counts and time new
    ROWOWQuestData const* questData = QuestSearch(qid2);
    if (questData == nullptr)
    {
        LOG_ERROR("module.ROWOW", "ROWOWMod::QuestChange: quest {} not found in DB", qid2);
        return -1;
    }
    ROWOWIdentity* identity = GetIdentity(sd);
    if (QuestCheck(*identity, qid2, ROWOW_QUEST_CHECK_HAVEQUEST) >= 0)
    {
        LOG_ERROR("module.ROWOW", "ROWOWMod::QuestChange: character {} already has quest {}", sd->GetGUID().GetCounter(), qid2);
        return -1;
    }
    if (QuestCheck(*identity, qid1, ROWOW_QUEST_CHECK_HAVEQUEST) < 0)
    {
        LOG_ERROR("module.ROWOW", "ROWOWMod::QuestChange: character {} doesn't have quest {}", sd->GetGUID().GetCounter(), qid1);
        return -1;
    }
    auto questIter = std::find_if(identity->Quests.begin(), identity->Quests.end(), [qid1](ROWOWQuestEntry const& quest)
    {
        return quest.QuestID == qid1 && quest.State != ROWOW_QUEST_STATE_COMPLETE;
    });
    if (questIter == identity->Quests.end())
    {
        LOG_ERROR("module.ROWOW", "ROWOWMod::QuestChange: character {} has completed quest {}", sd->GetGUID().GetCounter(), qid1);
        return -1;
    }
    *questIter = ROWOWQuestEntry();
    questIter->QuestID = qid2;
    questIter->Time = uint32(QuestTime(*questData));
    questIter->State = ROWOW_QUEST_STATE_ACTIVE;
    identity->AreQuestsChanged = true;
    SendQuestsToClient(sd);
    return 0;
}

int32 ROWOWMod::QuestDelete(Player* sd, uint32 questID)
{
    // quest_delete
    ROWOWIdentity* identity = GetIdentity(sd);
    auto questIter = std::find_if(identity->Quests.begin(), identity->Quests.end(), [questID](ROWOWQuestEntry const& quest) { return quest.QuestID == questID; });
    if (questIter == identity->Quests.end())
    {
        LOG_ERROR("module.ROWOW", "ROWOWMod::QuestDelete: character {} doesn't have quest {}", sd->GetGUID().GetCounter(), questID);
        return -1;
    }
    identity->Quests.erase(questIter);
    identity->AreQuestsChanged = true;
    SendQuestsToClient(sd);
    return 0;
}

int32 ROWOWMod::QuestUpdateStatus(Player* sd, uint32 questID, uint8 status)
{
    // quest_update_status: only an active or inactive quest changes; a completed one moves behind the others (and leaves the window)
    ROWOWIdentity* identity = GetIdentity(sd);
    auto questIter = std::find_if(identity->Quests.begin(), identity->Quests.end(), [questID](ROWOWQuestEntry const& quest)
    {
        return quest.QuestID == questID && quest.State != ROWOW_QUEST_STATE_COMPLETE;
    });
    if (questIter == identity->Quests.end())
    {
        LOG_ERROR("module.ROWOW", "ROWOWMod::QuestUpdateStatus: character {} doesn't have quest {}", sd->GetGUID().GetCounter(), questID);
        return -1;
    }
    questIter->State = status;
    identity->AreQuestsChanged = true;
    if (status == ROWOW_QUEST_STATE_COMPLETE)
    {
        ROWOWQuestEntry entry = *questIter;
        identity->Quests.erase(questIter);
        identity->Quests.push_back(entry);
    }
    SendQuestsToClient(sd);
    return 0;
}

int32 ROWOWMod::QuestCheck(ROWOWIdentity const& identity, uint32 questID, uint8 type) const
{
    // quest_check.  HAVEQUEST: the state (an inactive one as active); PLAYTIME: 2 when its time is up (a quest with no limit counts as up,
    // as in rAthena), 1 when complete, else 0; HUNTING: for an active or inactive quest, 2 when every objective's kills are made, 1 when its
    // time is up, else 0
    auto questIter = std::find_if(identity.Quests.begin(), identity.Quests.end(), [questID](ROWOWQuestEntry const& quest) { return quest.QuestID == questID; });
    if (questIter == identity.Quests.end())
        return -1;
    uint32 now = uint32(time(nullptr));
    switch (type)
    {
        case ROWOW_QUEST_CHECK_HAVEQUEST:
            if (questIter->State == ROWOW_QUEST_STATE_INACTIVE) // Player has the quest but it's in the inactive state; send it as Q_ACTIVE.
                return 1;
            return questIter->State;
        case ROWOW_QUEST_CHECK_PLAYTIME:
            return questIter->Time < now ? 2 : (questIter->State == ROWOW_QUEST_STATE_COMPLETE ? 1 : 0);
        case ROWOW_QUEST_CHECK_HUNTING:
            if (questIter->State == ROWOW_QUEST_STATE_INACTIVE || questIter->State == ROWOW_QUEST_STATE_ACTIVE)
            {
                ROWOWQuestData const* questData = QuestSearch(questID);
                size_t objectiveIndex = 0;
                size_t objectiveCount = questData != nullptr ? questData->Objectives.size() : 0;
                while (objectiveIndex < objectiveCount && questIter->Counts[objectiveIndex] >= questData->Objectives[objectiveIndex].Count)
                    objectiveIndex++;
                if (objectiveIndex == objectiveCount)
                    return 2;
                if (questIter->Time < now)
                    return 1;
            }
            return 0;
        default:
            LOG_ERROR("module.ROWOW", "ROWOWMod::QuestCheck: unknown check type {}", type);
            return -1;
    }
}

void ROWOWMod::QuestUpdateObjective(Player* sd, Creature* md)
{
    // quest_update_objective: each active or inactive quest hunting this monster counts the kill, up to its count.  (pre-re quest_db has
    // only monster targets, no race/size/element/level/map ones, and no quest drops)
    ROWOWIdentity* identity = GetIdentity(sd);
    uint32 mobID = GetROMobID(md);
    if (mobID == 0)
        return;
    bool isChanged = false;
    for (ROWOWQuestEntry& quest : identity->Quests)
    {
        if (quest.State == ROWOW_QUEST_STATE_COMPLETE) // Skip complete quests
            continue;
        ROWOWQuestData const* questData = QuestSearch(quest.QuestID);
        if (questData == nullptr)
            continue;
        for (size_t objectiveIndex = 0; objectiveIndex < questData->Objectives.size(); objectiveIndex++)
        {
            if (questData->Objectives[objectiveIndex].MobID == mobID && quest.Counts[objectiveIndex] < questData->Objectives[objectiveIndex].Count)
            {
                quest.Counts[objectiveIndex]++;
                isChanged = true;
            }
        }
    }
    if (isChanged == true)
    {
        identity->AreQuestsChanged = true;
        SendQuestsToClient(sd);
    }
}

void ROWOWMod::QuestKillUpdate(Creature* md, Player* sd)
{
    // mob_dead: the killer's party members within AREA_SIZE of the monster (map_foreachinallrange) count it, or the killer alone.  A WoW
    // group is the RO party.  The monster's map thread: only players on its map (on a stitched map, its part) are looked at
    if (md == nullptr || sd == nullptr || sd->GetMap() != md->GetMap() || IsOnSameROMap(sd, md) == false || IsInROForm(sd) == false || GetROMobID(md) == 0)
        return;
    Group* group = sd->GetGroup();
    if (group == nullptr)
    {
        QuestUpdateObjective(sd, md);
        return;
    }
    int32 mobX = 0, mobY = 0;
    GetUnitCell(md, mobX, mobY);
    for (GroupReference* memberReference = group->GetFirstMember(); memberReference != nullptr; memberReference = memberReference->next())
    {
        Player* member = memberReference->GetSource();
        if (member == nullptr || IsOnSameROMap(member, md) == false || IsInROForm(member) == false)
            continue;
        int32 memberX = 0, memberY = 0;
        GetUnitCell(member, memberX, memberY);
        if (std::abs(memberX - mobX) > BattleConfig.AreaSize || std::abs(memberY - mobY) > BattleConfig.AreaSize)
            continue;
        QuestUpdateObjective(member, md);
    }
}

void ROWOWMod::SendQuestsToClient(Player* player)
{
    // clif_quest_send_list for the RO_Identity addon's quest window: "B", then for each active or inactive quest
    // "Q<tab><id><tab><1 active, 0 inactive><tab><seconds left, -1 for no limit><tab><title>{<tab><monster name><tab><kills><tab><count>}",
    // then "E".  A completed quest leaves the window (clif_quest_delete), as in RO
    if (player == nullptr || player->GetSession() == nullptr || IsInROForm(player) == false)
        return;
    ROWOWIdentity* identity = GetIdentity(player);
    ROWOWDataTable const* mobTable = GetDataTable(ROWOW_DATA_TABLE_MOB);
    int32 mobNameColumnIndex = mobTable != nullptr ? mobTable->GetColumnIndex("Name") : -1;
    uint32 now = uint32(time(nullptr));
    SendQuestMessage(player, "B");
    for (ROWOWQuestEntry const& quest : identity->Quests)
    {
        if (quest.State == ROWOW_QUEST_STATE_COMPLETE)
            continue;
        ROWOWQuestData const* questData = QuestSearch(quest.QuestID);
        int64 secondsLeft = quest.Time == 0 ? -1 : (quest.Time > now ? int64(quest.Time - now) : 0);
        std::string message = fmt::format("Q\t{}\t{}\t{}\t{}", quest.QuestID, quest.State == ROWOW_QUEST_STATE_ACTIVE ? 1 : 0, secondsLeft,
            GetMessageField(questData != nullptr ? questData->Title : std::string(), 100));
        if (questData != nullptr)
        {
            for (size_t objectiveIndex = 0; objectiveIndex < questData->Objectives.size(); objectiveIndex++)
            {
                ROWOWQuestObjective const& objective = questData->Objectives[objectiveIndex];
                int64 mobRowIndex = GetMobRowIndex(objective.MobID);
                std::string mobName = mobRowIndex >= 0 && mobTable != nullptr ? mobTable->GetCell(size_t(mobRowIndex), mobNameColumnIndex) : std::to_string(objective.MobID);
                message += fmt::format("\t{}\t{}\t{}", GetMessageField(mobName, 30), quest.Counts[objectiveIndex], objective.Count);
            }
        }
        SendQuestMessage(player, message);
    }
    SendQuestMessage(player, "E");
}

void ROWOWMod::LoadIdentityQuests(Player* /*player*/, ROWOWIdentity& identity)
{
    // The active and inactive quests first, then the completed ones (as rAthena keeps quest_log)
    identity.Quests.clear();
    std::vector<ROWOWQuestEntry> completedQuests;
    if (QueryResult result = CharacterDatabase.Query("SELECT `quest_id`, `state`, `time`, `count1`, `count2`, `count3` FROM `mod_rowow_identity_quest` WHERE `guid` = {} "
        "ORDER BY `slot`", identity.IdentityID))
    {
        do
        {
            Field* fields = result->Fetch();
            ROWOWQuestEntry entry;
            entry.QuestID = fields[0].Get<uint32>();
            entry.State = std::min<uint8>(fields[1].Get<uint8>(), ROWOW_QUEST_STATE_COMPLETE);
            entry.Time = fields[2].Get<uint32>();
            entry.Counts[0] = fields[3].Get<int32>();
            entry.Counts[1] = fields[4].Get<int32>();
            entry.Counts[2] = fields[5].Get<int32>();
            if (entry.State == ROWOW_QUEST_STATE_COMPLETE)
                completedQuests.push_back(entry);
            else
                identity.Quests.push_back(entry);
        } while (result->NextRow());
    }
    identity.Quests.insert(identity.Quests.end(), completedQuests.begin(), completedQuests.end());
    identity.AreQuestsChanged = false;
}

void ROWOWMod::SaveIdentityQuests(Player* /*player*/, ROWOWIdentity& identity, CharacterDatabaseTransaction trans)
{
    if (identity.AreQuestsChanged == false)
        return;
    uint32 guid = identity.IdentityID;
    trans->Append("DELETE FROM `mod_rowow_identity_quest` WHERE `guid` = {}", guid);
    for (size_t slot = 0; slot < identity.Quests.size(); slot++)
    {
        ROWOWQuestEntry const& quest = identity.Quests[slot];
        trans->Append("INSERT INTO `mod_rowow_identity_quest` (`guid`, `slot`, `quest_id`, `state`, `time`, `count1`, `count2`, `count3`) VALUES ({}, {}, {}, {}, {}, {}, {}, {})",
            guid, slot, quest.QuestID, quest.State, quest.Time, quest.Counts[0], quest.Counts[1], quest.Counts[2]);
    }
    identity.AreQuestsChanged = false;
}
