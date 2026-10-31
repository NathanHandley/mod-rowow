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
};

// An NPC event waiting for the world update (donpcevent from a map thread, a timer's label): run attached to no player
struct ROWOWNpcEvent
{
    uint32 NpcID = 0;
    uint32 InstructionIndex = 0;
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

// The NPC dialog going to the chat, for testing until the RO NPC window (R7.3): .ro npc next / select / input / close answer it
class ROWOWChatScriptDialog : public ROWOWScriptDialog
{
public:
    void SendMes(Player* player, ROWOWNpc const* npc, std::string const& text) override;
    void SendNext(Player* player, ROWOWNpc const* npc) override;
    void SendClose(Player* player, ROWOWNpc const* npc) override;
    void SendClear(Player* player, ROWOWNpc const* npc) override;
    void SendMenu(Player* player, ROWOWNpc const* npc, std::vector<std::string> const& options) override;
    void SendInput(Player* player, ROWOWNpc const* npc, bool isString) override;
    void SendCutin(Player* player, std::string const& image, int32 position) override;
    void SendEnd(Player* player, ROWOWNpc const* npc) override;
};

#endif
