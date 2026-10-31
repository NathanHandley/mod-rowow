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

#ifndef ROWOW_NPC_H
#define ROWOW_NPC_H

#include "Common.h"
#include "ObjectGuid.h"

#include "ROWOW_Script.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

// The RO NPC scripts (R7): rAthena's NPC script files (the converter's mod_rowow_npc_script, from WorldData\NpcScripts), read into NPCs
// and functions once at startup.  An NPC that stands in the world is a creature row (the converter's town NPCs), tied to its NPC by
// mod_rowow_creature_npc

// The player events of rAthena's script_event (npce_event) the scripts use: every NPC's label of that name runs with the player attached
enum ROWOWNpcEventType : uint8
{
    ROWOW_NPC_EVENT_LOGIN,      // OnPCLoginEvent: entering the RO world in the form (a login, or the form starting)
    ROWOW_NPC_EVENT_LOGOUT,     // OnPCLogoutEvent
    ROWOW_NPC_EVENT_DIE,        // OnPCDieEvent
    ROWOW_NPC_EVENT_KILL_PC,    // OnPCKillEvent: the player killed a player
    ROWOW_NPC_EVENT_KILL_NPC,   // OnNPCKillEvent: the player killed a monster
    ROWOW_NPC_EVENT_COUNT
};

// An NPC's timer (rAthena's npc_data u.scr timer fields): its OnTimer<ms> labels go off as the time passes them, attached to no player
struct ROWOWNpcTimer
{
    bool IsRunning = false;
    uint64 StartTime = 0; // GameTime::GetGameTimeMS when it (last) started, less the tick it started from
    int64 StoppedTick = 0; // The tick it holds while stopped (stopnpctimer, setnpctimer)
    size_t NextEventIndex = 0; // The next of the NPC's OnTimer labels to go off
};

// An NPC (rAthena's npc_data): a script NPC placed on a map or floating ("-"), a duplicate of one (sharing its script and NPC variables),
// or a shop.  The startup data is only read after loading; the state scripts change (shown, its timer) is under ROWOWMod's NpcStateMutex
struct ROWOWNpc
{
    uint32 ID = 0; // Its place in NpcsByID plus one (rAthena's oid, getnpcid)
    std::string Name; // The name shown (before any '#')
    std::string UniqueName; // rAthena's exname: the label after "::", or the whole name; what duplicates, donpcevent and strnpcinfo(3) use
    std::string FullName; // As written, without the "::<label>"
    std::string MapName; // Lowercase, empty for a floating NPC
    uint32 MapID = 0; // The WoW map of an RO map, 0 when the map isn't converted or the NPC floats
    int32 X = 0;
    int32 Y = 0;
    int32 Direction = 0;
    std::string Sprite; // A number or a sprite constant (-1 and FAKE_NPC are invisible, 111/HIDDEN_NPC an unclickable one)
    int32 TriggerSpanX = -1; // The OnTouch area, -1 for none
    int32 TriggerSpanY = -1;
    std::shared_ptr<ROWOWScript> Script; // Shared with its duplicates; null for a shop
    std::vector<std::pair<int64, uint32>> TimerEvents; // Its OnTimer<ms> labels, by tick: the instruction of each
    bool IsShop = false;
    std::string SourceFile; // The script file (as ScriptFiles.txt lists it) and line it's defined on
    uint32 SourceLine = 0;
    std::vector<ObjectGuid::LowType> CreatureSpawnIDs; // The creature rows that stand for it

    // Changed by scripts (under NpcStateMutex)
    bool IsEnabled = true; // disablenpc
    bool IsHidden = false; // hideonnpc
    ROWOWNpcTimer Timer;
    ObjectGuid TimerPlayerGUID; // The player its timer's labels run with (attachnpctimer, initnpctimer's attach flag), empty for none
    ObjectGuid TouchingPlayerGUID; // touching_id: the one player its OnTouch_ ran for, until that player leaves the area

    bool IsClassless() const { return Sprite == "-1" || Sprite == "FAKE_NPC"; } // class_ < 0: clicked from anywhere (npc_checknear)
    bool IsInArea(uint32 mapID, int32 x, int32 y) const // On its OnTouch area
    {
        return TriggerSpanX >= 0 && TriggerSpanY >= 0 && mapID == MapID && x >= X - TriggerSpanX && x <= X + TriggerSpanX && y >= Y - TriggerSpanY && y <= Y + TriggerSpanY;
    }
};

// An NPC event waiting for the world update (donpcevent from a map thread, a timer's label): run attached to no player, or to the player
// of TimerPlayerGUID (an attached timer).  For a player's queue (npc_event_enqueue), one that waits for its current run to end
struct ROWOWNpcEvent
{
    uint32 NpcID = 0;
    uint32 InstructionIndex = 0;
    ObjectGuid PlayerGUID;
    bool IsTouch = false; // An OnTouch label: it runs only if the player is still on the NPC's area (npc_event_dequeue)
};

// A monster a script spawned on a map that isn't loaded yet (an OnInit's monster at startup): it spawns once the map is (world update)
struct ROWOWPendingScriptMob
{
    uint32 MapID = 0;
    int32 X = 0;
    int32 Y = 0;
    uint32 MobID = 0;
    std::string Event;
};

// An announcement a script made (announce, mapannounce), sent by the world update: to everyone, or to the players of a map
struct ROWOWNpcAnnouncement
{
    uint32 MapID = 0; // 0 for everyone
    std::string Text;
};

// A monster a script spawned (monster, areamonster): its map and the event its death runs ("<NPC>::<label>", empty for none)
struct ROWOWScriptMob
{
    uint32 MapID = 0;
    std::string Event;
};

// A killmonster for the world update: the script monsters of a map with that event, or all of them
struct ROWOWScriptMobKill
{
    uint32 MapID = 0;
    std::string Event;
    bool IsAll = false;
};

// The NPC dialog going to the chat, for GM testing (.ro npc talk -chat): .ro npc next / select / input / close answer it
class ROWOWChatScriptDialog : public ROWOWScriptDialog
{
public:
    void SendMes(Player* player, ROWOWNpc const* npc, std::string const& text) override;
    void SendNext(Player* player, ROWOWNpc const* npc) override;
    void SendClose(Player* player, ROWOWNpc const* npc) override;
    void SendClear(Player* player, ROWOWNpc const* npc) override;
    void SendMenu(Player* player, ROWOWNpc const* npc, std::vector<std::string> const& options) override;
    void SendInput(Player* player, ROWOWNpc const* npc, bool isString, int64 minimum, int64 maximum) override;
    void SendCutin(Player* player, std::string const& image, int32 position) override;
    void SendEnd(Player* player, ROWOWNpc const* npc) override;
};

// The NPC dialog going to the RO NPC window (RO_Identity_NpcDialog.lua) as ROWOWNPC addon messages; its buttons answer with the same
// .ro npc next / select / input / close commands.  See RONotes\R7-script.md "The RO NPC window"
class ROWOWWindowScriptDialog : public ROWOWScriptDialog
{
public:
    void SendMes(Player* player, ROWOWNpc const* npc, std::string const& text) override;
    void SendNext(Player* player, ROWOWNpc const* npc) override;
    void SendClose(Player* player, ROWOWNpc const* npc) override;
    void SendClear(Player* player, ROWOWNpc const* npc) override;
    void SendMenu(Player* player, ROWOWNpc const* npc, std::vector<std::string> const& options) override;
    void SendInput(Player* player, ROWOWNpc const* npc, bool isString, int64 minimum, int64 maximum) override;
    void SendCutin(Player* player, std::string const& image, int32 position) override;
    void SendEnd(Player* player, ROWOWNpc const* npc) override;
};

#endif
