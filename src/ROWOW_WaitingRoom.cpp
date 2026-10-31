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
//  Waiting rooms follow rAthena's chat.cpp (chat_createnpcchat, chat_joinchat, chat_leavechat, chat_triggerevent...) and its script
//  commands (src/map/script.cpp: buildin_waitingroom and the others):
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
//  For more information, see LICENCE in the main folder

#include "Chat.h"
#include "Creature.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "WorldSession.h"

#include "ROWOW.h"

using namespace std;

// Waiting rooms (R8.3): RO's chat rooms over NPCs, which the job tests (Knight, Crusader, Hunter, Assassin, Wizard, Sage, Dancer...) use as
// their queue: a player joins, and when enough have joined (the trigger) and the event is enabled, the NPC's event runs (OnStartArena),
// which warps the waiting players into the test (warpwaitingpc) and disables the event until the test is over.  rAthena's player-made
// chat rooms aren't here; only NPCs own rooms.
//
// The room is server state (WaitingRoomsByNpcID, under NpcStateMutex).  The client draws each room near the player as RO's bubble over its
// NPC (even a hidden one, as RO does) through the RO_Identity addon (RO_Identity_WaitingRoom.lua, the ROWOWROOM messages below); a click
// on the bubble joins (".ro room join <id>"), and the joined room's window lists its members with a Leave button (".ro room leave").  RO
// keeps a player in a chat room from walking; here walking off the cell it joined on leaves the room instead.  Leaving the map or the game
// leaves it too.  See RONotes\R8-content.md "Waiting rooms"

namespace
{
    // How often a player's update looks at the rooms around it (its bubbles, and whether it walked out of its room)
    constexpr uint32 WAITING_ROOM_CHECK_INTERVAL_IN_MS = 500;

    // CHATROOM_TITLE_SIZE less its terminator
    constexpr size_t WAITING_ROOM_TITLE_LENGTH = 36;

    // rAthena's e_refuse_enter_room, the answers to a join (clif_joinchatfail), with RO's message for each
    enum ROWOWEnterRoomResult : int32
    {
        ROWOW_ENTERROOM_FULL            = 0,
        ROWOW_ENTERROOM_WRONG_PASSWORD  = 1,
        ROWOW_ENTERROOM_KICKED          = 2,
        ROWOW_ENTERROOM_SUCCESS         = 3,
        ROWOW_ENTERROOM_NO_ZENY         = 4,
        ROWOW_ENTERROOM_TOO_LOW_LEVEL   = 5,
        ROWOW_ENTERROOM_TOO_HIGH_LEVEL  = 6,
    };

    char const* GetEnterRoomFailText(int32 result)
    {
        switch (result)
        {
            case ROWOW_ENTERROOM_FULL: return "The room is full.";
            case ROWOW_ENTERROOM_WRONG_PASSWORD: return "Wrong password.";
            case ROWOW_ENTERROOM_KICKED: return "You have been kicked out of this room.";
            case ROWOW_ENTERROOM_NO_ZENY: return "You don't have enough zeny to enter.";
            case ROWOW_ENTERROOM_TOO_LOW_LEVEL: return "Your level is too low to enter.";
            case ROWOW_ENTERROOM_TOO_HIGH_LEVEL: return "Your level is too high to enter.";
            default: return "You can't enter this room.";
        }
    }

    void SendWaitingRoomMessage(Player* player, std::string const& message)
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_ADDON, nullptr, nullptr, "ROWOWROOM\t" + message);
        player->SendDirectMessage(&data);
    }

    // Text as a message value: no tabs, commas (the member list's separator) or line breaks
    std::string GetWaitingRoomText(std::string text)
    {
        for (char& curChar : text)
            if (curChar == '\t' || curChar == '\r' || curChar == '\n' || curChar == ',')
                curChar = ' ';
        return text;
    }

    int64 GetArgument(std::vector<ROWOWScriptValue> const& arguments, size_t index, int64 defaultValue)
    {
        return index < arguments.size() ? arguments[index].GetNumber() : defaultValue;
    }

    std::string GetTextArgument(std::vector<ROWOWScriptValue> const& arguments, size_t index)
    {
        return index < arguments.size() ? arguments[index].GetText() : std::string();
    }
}

void ROWOWMod::ChatCreateNpcChat(ROWOWNpc const* npc, std::string const& title, int32 limit, int32 trigger, std::string const& event, int64 zeny,
    int32 minLevel, int32 maxLevel)
{
    // chat_createnpcchat: one room per NPC (MAX_CHAT_USERS 20 at most)
    if (npc == nullptr)
        return;
    std::lock_guard<std::mutex> lock(NpcStateMutex);
    if (WaitingRoomsByNpcID.find(npc->ID) != WaitingRoomsByNpcID.end())
    {
        LOG_ERROR("module.ROWOW", "ROWOWMod::ChatCreateNpcChat: npc '{}' already has a chatroom, cannot create new one!", npc->UniqueName);
        return;
    }
    ROWOWWaitingRoom& room = WaitingRoomsByNpcID[npc->ID];
    room.NpcID = npc->ID;
    room.Title = title.substr(0, WAITING_ROOM_TITLE_LENGTH);
    room.Limit = std::min(limit, int32(ROWOW_MAX_WAITING_ROOM_USERS));
    room.Trigger = trigger;
    room.Event = event;
    room.Zeny = zeny;
    room.MinLevel = minLevel;
    room.MaxLevel = maxLevel;
}

void ROWOWMod::ChatDeleteNpcChat(ROWOWNpc const* npc)
{
    // chat_deletenpcchat: everyone out (chat_npckickall), then the room goes
    if (npc == nullptr)
        return;
    std::lock_guard<std::mutex> lock(NpcStateMutex);
    WaitingRoomsByNpcID.erase(npc->ID);
}

int32 ROWOWMod::ChatJoinChat(Player* player, uint32 npcID)
{
    // chat_joinchat, and clif_joinchatfail's message when it can't
    int32 result = ChatJoinChatSub(player, npcID);
    if (result != ROWOW_ENTERROOM_SUCCESS && player->GetSession() != nullptr)
        ChatHandler(player->GetSession()).SendSysMessage(GetEnterRoomFailText(result));
    return result;
}

int32 ROWOWMod::ChatJoinChatSub(Player* player, uint32 npcID)
{
    // chat_joinchat: a room on the player's map (in sight: the client only shows those), not full (the NPC counts as one), the player in none
    // already, of its level and with its zeny; then the event if enough have joined (chat_triggerevent).  The player's session thread
    ROWOWIdentity* identity = GetIdentity(player);
    ROWOWNpc const* npc = GetNpc(npcID);
    if (identity->Exists == false || npc == nullptr || npc->MapID != player->GetMapId())
        return ROWOW_ENTERROOM_FULL;
    int32 x = 0, y = 0;
    GetUnitCell(player, x, y);
    int32 range = BattleConfig.AreaSize + 1;
    if (std::abs(x - npc->X) > range || std::abs(y - npc->Y) > range)
        return ROWOW_ENTERROOM_FULL;
    std::string event;
    {
        std::lock_guard<std::mutex> lock(NpcStateMutex);
        auto roomIter = WaitingRoomsByNpcID.find(npcID);
        if (roomIter == WaitingRoomsByNpcID.end() || npc->IsEnabled == false)
            return ROWOW_ENTERROOM_FULL;
        for (auto const& otherRoom : WaitingRoomsByNpcID)
            for (ROWOWWaitingRoomUser const& user : otherRoom.second.Users)
                if (user.PlayerGUID == player->GetGUID())
                    return ROWOW_ENTERROOM_FULL;
        ROWOWWaitingRoom& room = roomIter->second;
        if (int32(room.Users.size()) + 1 >= room.Limit)
            return ROWOW_ENTERROOM_FULL;
        if (int32(identity->BaseLevel) < room.MinLevel)
            return ROWOW_ENTERROOM_TOO_LOW_LEVEL;
        if (int32(identity->BaseLevel) > room.MaxLevel)
            return ROWOW_ENTERROOM_TOO_HIGH_LEVEL;
        if (int64(player->GetMoney()) < room.Zeny)
            return ROWOW_ENTERROOM_NO_ZENY;
        ROWOWWaitingRoomUser user;
        user.PlayerGUID = player->GetGUID();
        user.AccountID = player->GetSession()->GetAccountId();
        user.Name = player->GetName();
        user.MapID = player->GetMapId();
        user.X = x;
        user.Y = y;
        room.Users.push_back(user);
        if (int32(room.Users.size()) >= room.Trigger)
            event = room.Event;
    }
    player->StopMoving(); // unit_stop_walking
    identity->SentWaitingRoomMessage.clear();
    if (event.empty() == false)
        QueueNpcEvent(event); // npc_event_do, at the next world update
    return ROWOW_ENTERROOM_SUCCESS;
}

bool ROWOWMod::ChatLeaveChat(Player* player, bool isKicked)
{
    // chat_leavechat: out of its room (an NPC's room stays when it empties).  False when it was in none
    bool isFound = false;
    {
        std::lock_guard<std::mutex> lock(NpcStateMutex);
        for (auto& roomByNpcID : WaitingRoomsByNpcID)
        {
            std::vector<ROWOWWaitingRoomUser>& users = roomByNpcID.second.Users;
            auto userIter = std::find_if(users.begin(), users.end(), [player](ROWOWWaitingRoomUser const& user) { return user.PlayerGUID == player->GetGUID(); });
            if (userIter == users.end())
                continue;
            users.erase(userIter);
            isFound = true;
            break;
        }
    }
    if (isFound == false)
        return false;
    GetIdentity(player)->SentWaitingRoomMessage.clear();
    if (isKicked == true && player->GetSession() != nullptr)
        ChatHandler(player->GetSession()).SendSysMessage(GetEnterRoomFailText(ROWOW_ENTERROOM_KICKED));
    return true;
}

void ROWOWMod::ChatTriggerEvent(ROWOWNpc const* npc)
{
    // chat_triggerevent: the event, when the room has enough users and the event is enabled (the trigger's 0x80 bit off)
    std::string event;
    {
        std::lock_guard<std::mutex> lock(NpcStateMutex);
        auto roomIter = npc != nullptr ? WaitingRoomsByNpcID.find(npc->ID) : WaitingRoomsByNpcID.end();
        if (roomIter == WaitingRoomsByNpcID.end())
            return;
        if (int32(roomIter->second.Users.size()) >= roomIter->second.Trigger)
            event = roomIter->second.Event;
    }
    if (event.empty() == false)
        QueueNpcEvent(event);
}

void ROWOWMod::ChatSetEventEnabled(ROWOWNpc const* npc, bool isEnabled)
{
    // chat_enableevent (trigger &= 0x7f, then the event if enough are waiting) and chat_disableevent (trigger |= 0x80)
    {
        std::lock_guard<std::mutex> lock(NpcStateMutex);
        auto roomIter = npc != nullptr ? WaitingRoomsByNpcID.find(npc->ID) : WaitingRoomsByNpcID.end();
        if (roomIter == WaitingRoomsByNpcID.end())
            return;
        if (isEnabled == true)
            roomIter->second.Trigger &= 0x7f;
        else
            roomIter->second.Trigger |= 0x80;
    }
    if (isEnabled == true)
        ChatTriggerEvent(npc);
}

void ROWOWMod::ChatNpcKickAll(ROWOWNpc const* npc)
{
    // chat_npckickall: everyone leaves (their bubbles and windows follow at their next update)
    if (npc == nullptr)
        return;
    std::lock_guard<std::mutex> lock(NpcStateMutex);
    auto roomIter = WaitingRoomsByNpcID.find(npc->ID);
    if (roomIter != WaitingRoomsByNpcID.end())
        roomIter->second.Users.clear();
}

void ROWOWMod::WarpWaitingPlayer(ObjectGuid const& playerGUID, int64 zeny, std::string const& mapName, int32 x, int32 y)
{
    // warpwaitingpc's warp of one player (pc_payzeny for the room's fee, then pc_setpos), with the maps idle
    Player* player = ObjectAccessor::FindPlayer(playerGUID);
    if (player == nullptr || player->IsInWorld() == false || IsInROForm(player) == false)
        return;
    if (zeny > 0)
    {
        if (int64(player->GetMoney()) < zeny)
            return;
        player->ModifyMoney(-int32(zeny));
    }
    GetIdentity(player)->SentWaitingRoomMessage.clear();
    WarpPlayerToCell(player, mapName, x, y);
}

void ROWOWMod::WarpPendingWaitingPlayers()
{
    // The world update, with the maps idle: the warps of a warpwaitingpc that ran in a map's thread
    std::vector<ROWOWWaitingRoomWarp> warps;
    {
        std::lock_guard<std::mutex> lock(NpcStateMutex);
        warps.swap(PendingWaitingRoomWarps);
    }
    for (ROWOWWaitingRoomWarp const& warp : warps)
        WarpWaitingPlayer(warp.PlayerGUID, warp.Zeny, warp.MapName, warp.X, warp.Y);
}

void ROWOWMod::UpdatePlayerWaitingRooms(Player* player, uint32 diff)
{
    // The player's update (its map's thread), every half second: walking off the cell it joined on leaves its room, and the rooms in sight
    // on its map (AREA_SIZE, as clif_dispchat's area) go to its client when they changed
    ROWOWIdentity* identity = GetIdentity(player);
    identity->WaitingRoomCheckTimer += diff;
    if (identity->WaitingRoomCheckTimer < WAITING_ROOM_CHECK_INTERVAL_IN_MS)
        return;
    identity->WaitingRoomCheckTimer = 0;
    uint32 mapID = player->GetMapId();
    int32 x = 0, y = 0;
    GetUnitCell(player, x, y);

    struct ShownRoom
    {
        uint32 NpcID = 0;
        bool HasPosition = false;
        ROWOWPosition Position;
        std::string Line;
    };
    std::vector<ShownRoom> shownRooms;
    std::string joinedLine;
    bool isWalkedOut = false;
    {
        std::lock_guard<std::mutex> lock(NpcStateMutex);
        for (auto& roomByNpcID : WaitingRoomsByNpcID)
        {
            ROWOWWaitingRoom& room = roomByNpcID.second;
            ROWOWNpc const* npc = GetNpc(room.NpcID);
            for (ROWOWWaitingRoomUser const& user : room.Users)
            {
                if (user.PlayerGUID != player->GetGUID())
                    continue;
                if (user.MapID != mapID || user.X != x || user.Y != y)
                    isWalkedOut = true;
                else
                {
                    // The joined room's window: the NPC (its owner) first, then the users in the order they came
                    joinedLine = fmt::format("J\t{}\t{}\t{}\t{}\t{}", room.NpcID, room.Users.size() + 1, room.Limit, GetWaitingRoomText(room.Title),
                        npc != nullptr ? GetWaitingRoomText(npc->Name) : std::string());
                    for (ROWOWWaitingRoomUser const& member : room.Users)
                        joinedLine += "," + GetWaitingRoomText(member.Name);
                }
            }
            if (npc == nullptr || npc->MapID != mapID || npc->IsEnabled == false || std::abs(npc->X - x) > BattleConfig.AreaSize ||
                std::abs(npc->Y - y) > BattleConfig.AreaSize)
                continue;
            ShownRoom shownRoom;
            shownRoom.NpcID = room.NpcID;
            shownRoom.HasPosition = room.HasPosition;
            shownRoom.Position.X = room.PositionX;
            shownRoom.Position.Y = room.PositionY;
            shownRoom.Position.Z = room.PositionZ;
            shownRoom.Line = fmt::format("{}\t{}\t{}", room.Users.size() + 1, room.Limit, GetWaitingRoomText(room.Title));
            shownRooms.push_back(shownRoom);
        }
    }
    if (isWalkedOut == true)
    {
        ChatLeaveChat(player, false);
        joinedLine.clear();
    }

    // The bubble stands over the NPC: its creature (shown or hidden) where it has one, else its cell's floor.  Found once, on this map's
    // thread
    std::string message;
    for (ShownRoom& shownRoom : shownRooms)
    {
        if (shownRoom.HasPosition == false)
        {
            ROWOWNpc const* npc = GetNpc(shownRoom.NpcID);
            if (npc == nullptr)
                continue;
            Creature* npcCreature = nullptr;
            for (ObjectGuid::LowType spawnID : npc->CreatureSpawnIDs)
            {
                auto creatureRange = player->GetMap()->GetCreatureBySpawnIdStore().equal_range(spawnID);
                if (creatureRange.first != creatureRange.second)
                    npcCreature = creatureRange.first->second;
            }
            if (npcCreature != nullptr)
            {
                shownRoom.Position.X = npcCreature->GetPositionX();
                shownRoom.Position.Y = npcCreature->GetPositionY();
                shownRoom.Position.Z = npcCreature->GetPositionZ();
            }
            else if (GetCellPosition(npc->MapID, npc->X, npc->Y, shownRoom.Position) == false)
                continue;
            std::lock_guard<std::mutex> lock(NpcStateMutex);
            auto roomIter = WaitingRoomsByNpcID.find(shownRoom.NpcID);
            if (roomIter != WaitingRoomsByNpcID.end())
            {
                roomIter->second.HasPosition = true;
                roomIter->second.PositionX = shownRoom.Position.X;
                roomIter->second.PositionY = shownRoom.Position.Y;
                roomIter->second.PositionZ = shownRoom.Position.Z;
            }
        }
        message += fmt::format("R\t{}\t{:.2f}\t{:.2f}\t{:.2f}\t{}\n", shownRoom.NpcID, shownRoom.Position.X, shownRoom.Position.Y, shownRoom.Position.Z, shownRoom.Line);
    }
    if (joinedLine.empty() == false)
        message += joinedLine + "\n";
    // (marked, so an empty list (no rooms any more) differs from the cleared "send again")
    if ("S" + message == identity->SentWaitingRoomMessage)
        return;
    identity->SentWaitingRoomMessage = "S" + message;

    // "B", then a line per room in sight ("R<tab><id><tab><x><tab><y><tab><z><tab><users><tab><limit><tab><title>", users counting the NPC
    // as clif_chat_usercount does), and the joined room ("J<tab><id><tab><users><tab><limit><tab><title><tab><NPC>,<member>,..."), then "E"
    SendWaitingRoomMessage(player, "B");
    size_t start = 0;
    while (start < message.size())
    {
        size_t end = message.find('\n', start);
        SendWaitingRoomMessage(player, message.substr(start, end - start));
        start = end + 1;
    }
    SendWaitingRoomMessage(player, "E");
}

void ROWOWMod::RegisterWaitingRoomCommands()
{
    ROWOWScriptEngine* engine = ROWOWSCRIPT;

    // npc_name2id for the commands that name an NPC, else the run's own (st->oid)
    auto getNpc = [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments, size_t index) -> ROWOWNpc const*
    {
        if (index < arguments.size())
            return GetNpcByName(arguments[index].GetText());
        return context.Npc;
    };

    engine->RegisterCommand("waitingroom", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // waitingroom "<title>",<limit>{,"<event>"{,<trigger>{,<zeny>{,<min lvl>{,<max lvl>}}}}}: the run's NPC's room (the trigger is the
        // limit by default)
        int32 limit = int32(GetArgument(arguments, 1, 0));
        ChatCreateNpcChat(context.Npc, GetTextArgument(arguments, 0), limit, int32(GetArgument(arguments, 3, limit)), GetTextArgument(arguments, 2),
            GetArgument(arguments, 4, 0), int32(GetArgument(arguments, 5, 1)), int32(GetArgument(arguments, 6, std::numeric_limits<int32>::max())));
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("delwaitingroom", [this, getNpc](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // delwaitingroom{ "<NPC>"}
        ChatDeleteNpcChat(getNpc(context, arguments, 0));
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("waitingroomkick", [this, getNpc](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // waitingroomkick "<NPC>","<character name>" (chat_npckickchat); the player's own update sees it gone
        ROWOWNpc const* npc = getNpc(context, arguments, 0);
        std::string name = GetTextArgument(arguments, 1);
        if (npc == nullptr)
            return ROWOWScriptValue::FromNumber(0);
        std::lock_guard<std::mutex> lock(NpcStateMutex);
        auto roomIter = WaitingRoomsByNpcID.find(npc->ID);
        if (roomIter != WaitingRoomsByNpcID.end())
        {
            std::vector<ROWOWWaitingRoomUser>& users = roomIter->second.Users;
            users.erase(std::remove_if(users.begin(), users.end(), [&name](ROWOWWaitingRoomUser const& user) { return user.Name == name; }), users.end());
        }
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("kickwaitingroomall", [this, getNpc](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // kickwaitingroomall{ "<NPC>"}
        ChatNpcKickAll(getNpc(context, arguments, 0));
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("enablewaitingroomevent", [this, getNpc](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // enablewaitingroomevent{ "<NPC>"}: the event goes now if enough are waiting
        ChatSetEventEnabled(getNpc(context, arguments, 0), true);
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("disablewaitingroomevent", [this, getNpc](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // disablewaitingroomevent{ "<NPC>"}
        ChatSetEventEnabled(getNpc(context, arguments, 0), false);
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("getwaitingroomstate", [this, getNpc](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // getwaitingroomstate(<type>{,"<NPC>"}): 0 users, 1 limit, 2 trigger, 3 event disabled, 4 title, 5 password, 16 event, 32 full,
        // 33 enough to trigger; -1 for no room or another type
        ROWOWNpc const* npc = getNpc(context, arguments, 1);
        std::lock_guard<std::mutex> lock(NpcStateMutex);
        auto roomIter = npc != nullptr ? WaitingRoomsByNpcID.find(npc->ID) : WaitingRoomsByNpcID.end();
        if (roomIter == WaitingRoomsByNpcID.end())
            return ROWOWScriptValue::FromNumber(-1);
        ROWOWWaitingRoom const& room = roomIter->second;
        int32 users = int32(room.Users.size());
        switch (GetArgument(arguments, 0, -1))
        {
            case 0: return ROWOWScriptValue::FromNumber(users);
            case 1: return ROWOWScriptValue::FromNumber(room.Limit);
            case 2: return ROWOWScriptValue::FromNumber(room.Trigger & 0x7f);
            case 3: return ROWOWScriptValue::FromNumber((room.Trigger & 0x80) != 0 ? 1 : 0);
            case 4: return ROWOWScriptValue::FromText(room.Title);
            case 5: return ROWOWScriptValue::FromText(std::string());
            case 16: return ROWOWScriptValue::FromText(room.Event);
            case 32: return ROWOWScriptValue::FromNumber(users >= room.Limit ? 1 : 0);
            case 33: return ROWOWScriptValue::FromNumber(users >= room.Trigger ? 1 : 0);
            default: return ROWOWScriptValue::FromNumber(-1);
        }
    });
    engine->RegisterCommand("getwaitingroomusers", [this, getNpc](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // getwaitingroomusers{ "<NPC>"}: .@waitingroom_users[] (account ids) and .@waitingroom_usercount
        ROWOWNpc const* npc = getNpc(context, arguments, 0);
        std::vector<uint32> accountIDs;
        {
            std::lock_guard<std::mutex> lock(NpcStateMutex);
            auto roomIter = npc != nullptr ? WaitingRoomsByNpcID.find(npc->ID) : WaitingRoomsByNpcID.end();
            if (roomIter == WaitingRoomsByNpcID.end())
                return ROWOWScriptValue::FromNumber(0);
            for (ROWOWWaitingRoomUser const& user : roomIter->second.Users)
                accountIDs.push_back(user.AccountID);
        }
        for (size_t userIndex = 0; userIndex < accountIDs.size(); ++userIndex)
            ROWOWSCRIPT->SetVariable(context, ".@waitingroom_users", uint32(userIndex), ROWOWScriptValue::FromNumber(accountIDs[userIndex]));
        ROWOWSCRIPT->SetVariable(context, ".@waitingroom_usercount", 0, ROWOWScriptValue::FromNumber(int64(accountIDs.size())));
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("warpwaitingpc", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // warpwaitingpc "<map>",<x>,<y>{,<number>}: the trigger's number (or the number given) of the longest waiting go, out of the room,
        // paying its fee; their account ids in $@warpwaitingpc[] and the count in $@warpwaitingpcnum.  "Random" and "SavePoint" as warp's.
        // Run in the world update (the rooms' events), the warps happen now, so a script can warp one again after (attachrid, warpchar);
        // from a map's thread they wait for the world update (the players can be in another's)
        std::string mapName = GetTextArgument(arguments, 0);
        int32 x = int32(GetArgument(arguments, 1, 0));
        int32 y = int32(GetArgument(arguments, 2, 0));
        ROWOWNpc const* npc = context.Npc;
        if (npc == nullptr)
            return ROWOWScriptValue::FromNumber(0);
        int32 count = 0;
        bool isNow = IsInNpcWorldUpdate == true;
        while (true)
        {
            ROWOWWaitingRoomUser user;
            int64 zeny = 0;
            {
                std::lock_guard<std::mutex> lock(NpcStateMutex);
                auto roomIter = WaitingRoomsByNpcID.find(npc->ID);
                if (roomIter == WaitingRoomsByNpcID.end())
                    return ROWOWScriptValue::FromNumber(0);
                ROWOWWaitingRoom& room = roomIter->second;
                int32 number = arguments.size() > 3 ? int32(GetArgument(arguments, 3, 0)) : (room.Trigger & 0x7f);
                if (count >= number || room.Users.empty() == true)
                    break;
                user = room.Users.front();
                zeny = room.Zeny;
                if (isNow == true && zeny > 0)
                {
                    // No zeny to cover the fee: it stays, and so does everyone after it
                    Player* player = ObjectAccessor::FindPlayer(user.PlayerGUID);
                    if (player == nullptr || int64(player->GetMoney()) < zeny)
                        break;
                }
                room.Users.erase(room.Users.begin());
                if (isNow == false)
                {
                    ROWOWWaitingRoomWarp warp;
                    warp.PlayerGUID = user.PlayerGUID;
                    warp.Zeny = zeny;
                    warp.MapName = mapName;
                    warp.X = x;
                    warp.Y = y;
                    PendingWaitingRoomWarps.push_back(warp);
                }
            }
            ROWOWSCRIPT->SetVariable(context, "$@warpwaitingpc", uint32(count), ROWOWScriptValue::FromNumber(user.AccountID));
            if (isNow == true)
                WarpWaitingPlayer(user.PlayerGUID, zeny, mapName, x, y);
            count++;
        }
        ROWOWSCRIPT->SetVariable(context, "$@warpwaitingpcnum", 0, ROWOWScriptValue::FromNumber(count));
        return ROWOWScriptValue::FromNumber(0);
    });
}

// The waiting rooms' hooks: the warps waiting for the world update, and leaving the room on leaving the map or the game
class ROWOW_WaitingRoomWorldScript : public WorldScript
{
public:
    ROWOW_WaitingRoomWorldScript() : WorldScript("ROWOW_WaitingRoomWorldScript") {}

    void OnUpdate(uint32 /*diff*/) override
    {
        if (ROWOW->IsEnabled == false)
            return;
        ROWOW->WarpPendingWaitingPlayers();
    }
};

class ROWOW_WaitingRoomPlayerScript : public PlayerScript
{
public:
    ROWOW_WaitingRoomPlayerScript() : PlayerScript("ROWOW_WaitingRoomPlayerScript", { PLAYERHOOK_ON_MAP_CHANGED, PLAYERHOOK_ON_LOGOUT }) {}

    void OnPlayerMapChanged(Player* player) override
    {
        // pc_setpos leaves the chat room
        if (ROWOW->IsEnabled == false)
            return;
        ROWOW->ChatLeaveChat(player, false);
        ROWOW->GetIdentity(player)->SentWaitingRoomMessage.clear();
    }

    void OnPlayerLogout(Player* player) override
    {
        if (ROWOW->IsEnabled == false)
            return;
        ROWOW->ChatLeaveChat(player, false);
    }
};

void AddROWOWWaitingRoomScripts()
{
    new ROWOW_WaitingRoomWorldScript();
    new ROWOW_WaitingRoomPlayerScript();
}
