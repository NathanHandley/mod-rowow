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
//  The NPC loading, events and timers follow rAthena's npc.cpp:
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
//  For more information, see LICENCE in the main folder

#include "Chat.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "QueryResult.h"
#include "ScriptMgr.h"
#include "Timer.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#include "ROWOW.h"
#include "ROWOW_ScriptConstants.h"

#include <ctime>

using namespace std;

// RO NPC scripts (R7): rAthena's NPC script files, as the converter ships them (mod_rowow_npc_script, the WorldData\NpcScripts set in
// scripts_main.conf's order), read once at startup into NPCs and functions (npc_parsesrcfile).  Warps, monster spawns and map flags in
// the files are the converter's already (RoWarps, RoMobSpawns, RoMapFlags), so they're only counted.  A player's run lives on its
// identity and happens in its thread; the runs with no player, the timers and the queued events happen in the world update, between
// the map updates

namespace
{
    // How often changed global variables ($name) are saved
    constexpr uint32 GLOBAL_VARIABLE_SAVE_INTERVAL_IN_MS = 10000;

    // The most parse errors logged one by one (the count is always logged)
    constexpr size_t MAX_LOGGED_PARSE_ERRORS = 50;

    std::string ToLower(std::string text)
    {
        for (char& curChar : text)
            curChar = char(tolower((unsigned char)curChar));
        return text;
    }

    // RO's text colors (^RRGGBB, ^000000 back to the default) as WoW's
    std::string GetChatText(std::string const& text)
    {
        std::string chatText;
        for (size_t i = 0; i < text.size(); i++)
        {
            if (text[i] == '^' && i + 6 < text.size() && isxdigit((unsigned char)text[i + 1]) && isxdigit((unsigned char)text[i + 2]) && isxdigit((unsigned char)text[i + 3])
                && isxdigit((unsigned char)text[i + 4]) && isxdigit((unsigned char)text[i + 5]) && isxdigit((unsigned char)text[i + 6]))
            {
                std::string color = text.substr(i + 1, 6);
                chatText += color == "000000" ? std::string("|r") : "|cff" + color;
                i += 6;
                continue;
            }
            chatText += text[i];
        }
        return chatText;
    }

    // A battle config value as rAthena's config reader takes it: yes/no, on/off, decimal or 0x hexadecimal
    int64 ParseBattleFlagValue(std::string const& text)
    {
        std::string lowerText = ToLower(text);
        if (lowerText == "yes" || lowerText == "on")
            return 1;
        if (lowerText == "no" || lowerText == "off")
            return 0;
        return int64(strtoll(text.c_str(), nullptr, 0));
    }

    ROWOWScriptValue GetStoredVariableValue(std::string const& name, int64 value, std::string const& text)
    {
        if (name.empty() == false && name.back() == '$')
            return ROWOWScriptValue::FromText(text);
        return ROWOWScriptValue::FromNumber(value);
    }

    // The most text one ROWOWNPC message carries; a longer mes line goes in pieces
    constexpr size_t NPC_WINDOW_TEXT_PIECE_LENGTH = 200;

    // MAX_EVENTQUEUE: the events a player's queue holds while its run goes on
    constexpr size_t MAX_QUEUED_NPC_EVENTS = 2;

    // How deep donpcevent runs events inside the run that asked (deeper ones wait for the next world update)
    constexpr uint32 MAX_NESTED_NPC_EVENTS = 8;

    // How often a player's dialog checks it's still in the NPC's range
    constexpr uint32 NPC_RANGE_CHECK_INTERVAL_IN_MS = 500;

    // The label names of the player events (script_config's *_event_name), by ROWOWNpcEventType
    char const* const PLAYER_EVENT_LABEL_NAMES[ROWOW_NPC_EVENT_COUNT] = { "OnPCLoginEvent", "OnPCLogoutEvent", "OnPCDieEvent", "OnPCKillEvent", "OnNPCKillEvent" };

    // The RO NPC window's messages (RO_Identity_NpcDialog.lua): "ROWOWNPC<tab><kind>[<tab><values>]"
    void SendNpcWindowMessage(Player* player, std::string const& message)
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_ADDON, nullptr, nullptr, "ROWOWNPC\t" + message);
        player->SendDirectMessage(&data);
    }

    // Text as a message value: no tabs or line breaks (the message's separators)
    std::string GetNpcWindowText(std::string text)
    {
        for (char& curChar : text)
            if (curChar == '\t' || curChar == '\r' || curChar == '\n')
                curChar = ' ';
        return text;
    }
}

void ROWOWWindowScriptDialog::SendMes(Player* player, ROWOWNpc const* /*npc*/, std::string const& text)
{
    // "M<tab><line>", after "P<tab><piece>" for each piece of a long line (never cut inside a UTF-8 character).  The line is RO's text,
    // ^RRGGBB colors and all, which the window turns into its own
    std::string line = GetNpcWindowText(text);
    while (line.size() > NPC_WINDOW_TEXT_PIECE_LENGTH)
    {
        size_t pieceLength = NPC_WINDOW_TEXT_PIECE_LENGTH;
        while (pieceLength > 0 && (uint8(line[pieceLength]) & 0xC0) == 0x80)
            pieceLength--;
        SendNpcWindowMessage(player, "P\t" + line.substr(0, pieceLength));
        line.erase(0, pieceLength);
    }
    SendNpcWindowMessage(player, "M\t" + line);
}

void ROWOWWindowScriptDialog::SendNext(Player* player, ROWOWNpc const* /*npc*/)
{
    SendNpcWindowMessage(player, "N");
}

void ROWOWWindowScriptDialog::SendClose(Player* player, ROWOWNpc const* /*npc*/)
{
    SendNpcWindowMessage(player, "C");
}

void ROWOWWindowScriptDialog::SendClear(Player* player, ROWOWNpc const* /*npc*/)
{
    SendNpcWindowMessage(player, "L");
}

void ROWOWWindowScriptDialog::SendMenu(Player* player, ROWOWNpc const* /*npc*/, std::vector<std::string> const& options)
{
    // "O<tab><option>" for each option in order (the empty ones too, which the window hides but counts), then "U" shows them
    for (std::string const& option : options)
    {
        std::string optionText = GetNpcWindowText(option);
        size_t optionLength = std::min(optionText.size(), NPC_WINDOW_TEXT_PIECE_LENGTH);
        while (optionLength < optionText.size() && optionLength > 0 && (uint8(optionText[optionLength]) & 0xC0) == 0x80)
            optionLength--;
        SendNpcWindowMessage(player, "O\t" + optionText.substr(0, optionLength));
    }
    SendNpcWindowMessage(player, "U");
}

void ROWOWWindowScriptDialog::SendInput(Player* player, ROWOWNpc const* /*npc*/, bool isString, int64 minimum, int64 maximum)
{
    // "I<tab><1 for text, 0 for a number><tab><min><tab><max>" (a text's bounds are its length)
    SendNpcWindowMessage(player, fmt::format("I\t{}\t{}\t{}", isString == true ? 1 : 0, minimum, maximum));
}

void ROWOWWindowScriptDialog::SendCutin(Player* player, std::string const& image, int32 position)
{
    // "X<tab><image><tab><position>": 0 bottom left, 1 bottom center, 2 bottom right, 3 and 4 the middle; 255 takes them away
    SendNpcWindowMessage(player, fmt::format("X\t{}\t{}", GetNpcWindowText(image), position));
}

void ROWOWWindowScriptDialog::SendEnd(Player* player, ROWOWNpc const* /*npc*/)
{
    SendNpcWindowMessage(player, "E");
}

void ROWOWWindowScriptDialog::SendAnswered(Player* player, ROWOWNpc const* /*npc*/)
{
    // "A": the window clears itself on its own buttons, but an answer typed (.ro npc next, quickly or by a test) would leave the old page
    // and buttons showing under the next one
    SendNpcWindowMessage(player, "A");
}

void ROWOWChatScriptDialog::SendMes(Player* player, ROWOWNpc const* npc, std::string const& text)
{
    ChatHandler(player->GetSession()).PSendSysMessage("|cffffd100{}:|r {}", npc != nullptr ? npc->Name : "NPC", GetChatText(text));
}

void ROWOWChatScriptDialog::SendNext(Player* player, ROWOWNpc const* /*npc*/)
{
    ChatHandler(player->GetSession()).SendSysMessage("|cff88ccff[Next]|r (.ro npc next)");
}

void ROWOWChatScriptDialog::SendClose(Player* player, ROWOWNpc const* /*npc*/)
{
    ChatHandler(player->GetSession()).SendSysMessage("|cff88ccff[Close]|r (.ro npc close)");
}

void ROWOWChatScriptDialog::SendClear(Player* player, ROWOWNpc const* /*npc*/)
{
    ChatHandler(player->GetSession()).SendSysMessage("|cff88ccff--|r");
}

void ROWOWChatScriptDialog::SendMenu(Player* player, ROWOWNpc const* /*npc*/, std::vector<std::string> const& options)
{
    ChatHandler handler(player->GetSession());
    for (size_t optionIndex = 0; optionIndex < options.size(); optionIndex++)
        if (options[optionIndex].empty() == false)
            handler.PSendSysMessage("|cff88ccff{}.|r {}", optionIndex + 1, GetChatText(options[optionIndex]));
    handler.SendSysMessage("|cff88ccff[Menu]|r (.ro npc select <number>, 255 cancels)");
}

void ROWOWChatScriptDialog::SendInput(Player* player, ROWOWNpc const* /*npc*/, bool isString, int64 minimum, int64 maximum)
{
    ChatHandler(player->GetSession()).PSendSysMessage("|cff88ccff[Input {} {}-{}]|r (.ro npc input <value>)", isString == true ? "text" : "number", minimum, maximum);
}

void ROWOWChatScriptDialog::SendCutin(Player* player, std::string const& image, int32 position)
{
    ChatHandler(player->GetSession()).PSendSysMessage("|cff888888(cutin {} {})|r", image, position);
}

void ROWOWChatScriptDialog::SendEnd(Player* player, ROWOWNpc const* /*npc*/)
{
    ChatHandler(player->GetSession()).SendSysMessage("|cff88ccff[End]|r");
}

void ROWOWMod::LoadNpcScriptData()
{
    NpcsByID.clear();
    NpcsByUniqueName.clear();
    NpcsByName.clear();
    NpcsByCreatureSpawnID.clear();
    BattleFlagsByName.clear();
    TouchNpcIDsByMapID.clear();
    NpcIDsByMapID.clear();
    for (std::vector<ROWOWNpcEvent>& eventLabels : PlayerEventLabels)
        eventLabels.clear();
    ClockEventsByLabel.clear();
    if (IsEnabled == false)
        return;
    uint32 oldMSTime = getMSTime();

    // The commands, then the constants scripts name: rAthena's script constants, and (as rAthena makes them constants) the item and
    // monster AegisNames and the skill names.  None overrides one the module registered already
    RegisterNpcScriptCommands();
    int64 existingValue = 0;
    for (ROWOWScriptConstant const& constant : ROWOWScriptConstants)
        if (ROWOWSCRIPT->GetConstant(constant.Name, existingValue) == false)
            ROWOWSCRIPT->RegisterConstant(constant.Name, constant.Value);
    auto isFreeName = [&existingValue](std::string const& name)
    {
        return ROWOWSCRIPT->GetCommandIndex(name) < 0 && ROWOWSCRIPT->GetConstant(name, existingValue) == false;
    };
    for (auto const& itemIDByAegisName : ItemIDsByAegisName)
        if (isFreeName(itemIDByAegisName.first) == true)
            ROWOWSCRIPT->RegisterConstant(itemIDByAegisName.first, itemIDByAegisName.second);
    if (ROWOWDataTable const* mobTable = GetDataTable(ROWOW_DATA_TABLE_MOB))
    {
        int32 idColumnIndex = mobTable->GetColumnIndex("Id");
        int32 aegisNameColumnIndex = mobTable->GetColumnIndex("AegisName");
        for (size_t rowIndex = 0; rowIndex < mobTable->Rows.size(); rowIndex++)
            if (std::string const& aegisName = mobTable->GetCell(rowIndex, aegisNameColumnIndex); aegisName.empty() == false && isFreeName(aegisName) == true)
                ROWOWSCRIPT->RegisterConstant(aegisName, mobTable->GetInt(rowIndex, idColumnIndex, 0));
    }
    for (auto const& skillIDByName : SkillIDsByName)
        if (isFreeName(skillIDByName.first) == true)
            ROWOWSCRIPT->RegisterConstant(skillIDByName.first, skillIDByName.second);

    // A parameter the module doesn't have yet reads 0 (rather than being taken for a character variable)
    for (char const* parameterName : ROWOWScriptParameterNames)
    {
        if (ROWOWSCRIPT->GetCommandIndex(parameterName) >= 0)
            continue;
        ROWOWSCRIPT->RegisterParameter(parameterName, [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const&) { return ROWOWScriptValue::FromNumber(0); });
        ROWOWSCRIPT->SetStub(parameterName);
    }

    // getbattleflag's values
    if (ROWOWDataTable const* battleConfigTable = GetDataTable(ROWOW_DATA_TABLE_BATTLE_CONFIG))
    {
        int32 keyColumnIndex = battleConfigTable->GetColumnIndex("Key");
        int32 valueColumnIndex = battleConfigTable->GetColumnIndex("Value");
        for (size_t rowIndex = 0; rowIndex < battleConfigTable->Rows.size(); rowIndex++)
            BattleFlagsByName[ToLower(battleConfigTable->GetCell(rowIndex, keyColumnIndex))] = ParseBattleFlagValue(battleConfigTable->GetCell(rowIndex, valueColumnIndex));
    }
    // The refine window is a client feature this client doesn't have: rAthena turns feature.refineui off for clients before 2016-10-12
    // (battle.cpp's PACKETVER check), so the refiners talk (refinemain) instead
    BattleFlagsByName["feature.refineui"] = 0;

    // The global variables kept from before ($name)
    {
        std::lock_guard<std::mutex> lock(ROWOWSCRIPT->VariableMutex);
        ROWOWSCRIPT->GlobalVariables.Clear();
        ROWOWSCRIPT->GlobalTempVariables.Clear();
        if (QueryResult result = CharacterDatabase.Query("SELECT `name`, `index`, `value`, `text` FROM `mod_rowow_global_variable`"))
        {
            do
            {
                Field* fields = result->Fetch();
                std::string name = ToLower(fields[0].Get<std::string>());
                ROWOWSCRIPT->GlobalVariables.Set(name, fields[1].Get<uint32>(), GetStoredVariableValue(name, fields[2].Get<int64>(), fields[3].Get<std::string>()));
            } while (result->NextRow());
        }
        ROWOWSCRIPT->GlobalVariables.ChangedElements.clear();
        ROWOWSCRIPT->GlobalTempVariables.ChangedElements.clear();
    }

    // The script files, in load order
    ROWOWSCRIPT->IsNpcLanguage = true;
    QueryResult tableResult = WorldDatabase.Query("SELECT 1 FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = 'mod_rowow_npc_script'");
    if (!tableResult)
    {
        LOG_INFO("module.ROWOW", "ROWOWMod::LoadNpcScriptData found no mod_rowow_npc_script table, so there are no RO NPC scripts (run the converter)");
        return;
    }
    std::vector<std::pair<std::string, std::string>> scriptFiles; // Path, text
    if (QueryResult result = WorldDatabase.Query("SELECT `FileIndex`, `FilePath`, `Text` FROM `mod_rowow_npc_script` ORDER BY `FileIndex`"))
    {
        uint32 currentFileIndex = UINT32_MAX;
        do
        {
            Field* fields = result->Fetch();
            if (fields[0].Get<uint32>() != currentFileIndex)
            {
                currentFileIndex = fields[0].Get<uint32>();
                scriptFiles.emplace_back(fields[1].Get<std::string>(), std::string());
            }
            scriptFiles.back().second += fields[2].Get<std::string>();
        } while (result->NextRow());
    }

    // Each file's definitions: functions, NPCs and shops in their order, then the duplicates (their sources can come later in the list)
    std::vector<std::string> parseErrors;
    uint32 functionCount = 0;
    uint32 otherDefinitionCount = 0;
    uint32 duplicateNameCount = 0;
    struct PendingDuplicate
    {
        ROWOWScriptDefinition Definition;
        std::string FilePath;
    };
    std::vector<PendingDuplicate> pendingDuplicates;
    std::map<std::string, std::pair<uint32, std::set<std::string>>> commandUses; // Name: uses, files
    auto addNpc = [&](ROWOWScriptDefinition const& definition, std::string const& filePath, std::shared_ptr<ROWOWScript> script, bool isShop) -> ROWOWNpc*
    {
        std::unique_ptr<ROWOWNpc> npc = std::make_unique<ROWOWNpc>();
        npc->ID = uint32(NpcsByID.size() + 1);

        // npc_parsename: "Shown#hidden::Unique"; the unique name (exname) is the label, or the whole name
        std::string fullName = definition.Name;
        size_t labelPosition = fullName.find("::");
        npc->UniqueName = labelPosition != std::string::npos ? fullName.substr(labelPosition + 2) : fullName;
        npc->FullName = labelPosition != std::string::npos ? fullName.substr(0, labelPosition) : fullName;
        size_t hiddenPosition = npc->FullName.find('#');
        npc->Name = hiddenPosition != std::string::npos ? npc->FullName.substr(0, hiddenPosition) : npc->FullName;
        npc->MapName = definition.MapName;
        npc->MapID = definition.MapName.empty() == false ? GetMapIDByMapName(definition.MapName) : 0;
        npc->X = definition.X;
        npc->Y = definition.Y;
        npc->Direction = definition.Direction;
        npc->Sprite = definition.Sprite;
        npc->TriggerSpanX = definition.TriggerSpanX;
        npc->TriggerSpanY = definition.TriggerSpanY;
        npc->Script = script;
        npc->IsShop = isShop;
        npc->SourceFile = filePath;
        npc->SourceLine = definition.Line;

        // Its OnTimer<ms> labels in time order
        if (script != nullptr)
        {
            for (auto const& labelByName : script->LabelsByName)
                if (labelByName.first.size() > 7 && labelByName.first.compare(0, 7, "ontimer") == 0 && isdigit((unsigned char)labelByName.first[7]))
                    npc->TimerEvents.emplace_back(strtoll(labelByName.first.c_str() + 7, nullptr, 10), labelByName.second);
            std::sort(npc->TimerEvents.begin(), npc->TimerEvents.end());
        }

        // An exname given twice keeps its first NPC (rAthena renames the later one)
        if (NpcsByUniqueName.emplace(ToLower(npc->UniqueName), npc.get()).second == false)
            duplicateNameCount++;
        NpcsByName.emplace(ToLower(npc->Name), npc.get());
        if (ToLower(npc->FullName) != ToLower(npc->Name) && ToLower(npc->FullName) != ToLower(npc->UniqueName))
            NpcsByName.emplace(ToLower(npc->FullName), npc.get());
        NpcsByID.push_back(std::move(npc));
        return NpcsByID.back().get();
    };
    for (auto const& scriptFile : scriptFiles)
    {
        std::vector<ROWOWScriptDefinition> definitions = ROWOWSCRIPT->ParseNpcFile(scriptFile.second, scriptFile.first, parseErrors);
        for (ROWOWScriptDefinition& definition : definitions)
        {
            if (definition.Script != nullptr)
            {
                for (auto const& commandUse : definition.Script->CommandUses)
                {
                    commandUses[commandUse.first].first += commandUse.second;
                    commandUses[commandUse.first].second.insert(scriptFile.first);
                }
                definition.Script->CommandUses.clear();
            }
            switch (definition.Type)
            {
                case ROWOW_SCRIPT_DEFINITION_FUNCTION:
                    if (definition.Script->Error.empty() == true)
                        ROWOWSCRIPT->RegisterFunction(definition.Name, definition.Script);
                    functionCount++;
                    break;
                case ROWOW_SCRIPT_DEFINITION_SCRIPT:
                    addNpc(definition, scriptFile.first, definition.Script, false);
                    break;
                case ROWOW_SCRIPT_DEFINITION_SHOP:
                    addNpc(definition, scriptFile.first, nullptr, true);
                    break;
                case ROWOW_SCRIPT_DEFINITION_DUPLICATE:
                {
                    PendingDuplicate pendingDuplicate;
                    pendingDuplicate.Definition = definition;
                    pendingDuplicate.FilePath = scriptFile.first;
                    pendingDuplicates.push_back(std::move(pendingDuplicate));
                    break;
                }
                default:
                    otherDefinitionCount++;
                    break;
            }
        }
    }

    // Duplicates take their source's script (and so its NPC variables) or its wares.  One of a warp is the converter's (RoWarps)
    uint32 duplicateCount = 0;
    uint32 warpDuplicateCount = 0;
    for (PendingDuplicate const& pendingDuplicate : pendingDuplicates)
    {
        auto sourceIter = NpcsByUniqueName.find(ToLower(pendingDuplicate.Definition.DuplicateSource));
        if (sourceIter == NpcsByUniqueName.end())
        {
            warpDuplicateCount++;
            continue;
        }
        ROWOWNpc const* source = sourceIter->second;
        ROWOWScriptDefinition definition = pendingDuplicate.Definition;
        if (definition.TriggerSpanX < 0)
        {
            definition.TriggerSpanX = source->TriggerSpanX;
            definition.TriggerSpanY = source->TriggerSpanY;
        }
        addNpc(definition, pendingDuplicate.FilePath, source->Script, source->IsShop);
        duplicateCount++;
    }

    // The creature rows the converter placed for the NPCs, by the file and line each NPC is defined on
    uint32 mappedCreatureCount = 0;
    uint32 unmatchedCreatureCount = 0;
    if (WorldDatabase.Query("SELECT 1 FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = 'mod_rowow_creature_npc'"))
    {
        std::map<std::pair<std::string, uint32>, ROWOWNpc*> npcsBySource;
        for (std::unique_ptr<ROWOWNpc> const& npc : NpcsByID)
            npcsBySource.emplace(std::make_pair(ToLower(npc->SourceFile), npc->SourceLine), npc.get());
        if (QueryResult result = WorldDatabase.Query("SELECT `CreatureGUID`, `NpcFile`, `NpcLine` FROM `mod_rowow_creature_npc`"))
        {
            do
            {
                Field* fields = result->Fetch();
                auto npcIter = npcsBySource.find(std::make_pair(ToLower(fields[1].Get<std::string>()), fields[2].Get<uint32>()));
                if (npcIter == npcsBySource.end())
                {
                    unmatchedCreatureCount++;
                    continue;
                }
                ObjectGuid::LowType spawnID = fields[0].Get<uint32>();
                npcIter->second->CreatureSpawnIDs.push_back(spawnID);
                NpcsByCreatureSpawnID[spawnID] = npcIter->second;
                mappedCreatureCount++;
            } while (result->NextRow());
        }
    }

    // The NPCs with an OnTouch area on a converted map, each player event's labels (npc_read_event_script), and the clock labels
    // (npc_event_do_clock's OnMinute<mm>, OnClock<hhmm>, OnHour<hh>, On<Day><hhmm>, OnDay<MMdd>)
    uint32 touchNpcCount = 0;
    uint32 clockLabelCount = 0;
    for (std::unique_ptr<ROWOWNpc> const& npc : NpcsByID)
    {
        if (npc->MapID != 0)
            NpcIDsByMapID[npc->MapID].push_back(npc->ID);
        if (npc->Script == nullptr || npc->Script->Error.empty() == false)
            continue;
        if (npc->TriggerSpanX >= 0 && npc->TriggerSpanY >= 0 && npc->MapID != 0)
        {
            TouchNpcIDsByMapID[npc->MapID].push_back(npc->ID);
            touchNpcCount++;
        }
        for (uint8 eventType = 0; eventType < ROWOW_NPC_EVENT_COUNT; eventType++)
        {
            int64 labelIndex = npc->Script->GetLabel(PLAYER_EVENT_LABEL_NAMES[eventType]);
            if (labelIndex < 0)
                continue;
            ROWOWNpcEvent npcEvent;
            npcEvent.NpcID = npc->ID;
            npcEvent.InstructionIndex = uint32(labelIndex);
            PlayerEventLabels[eventType].push_back(npcEvent);
        }
        for (auto const& labelByName : npc->Script->LabelsByName)
        {
            std::string const& labelName = labelByName.first;
            static char const* const clockPrefixes[] = { "onminute", "onclock", "onhour", "onsun", "onmon", "ontue", "onwed", "onthu", "onfri", "onsat", "onday" };
            bool isClockLabel = false;
            for (char const* clockPrefix : clockPrefixes)
            {
                size_t prefixLength = strlen(clockPrefix);
                if (labelName.size() > prefixLength && labelName.compare(0, prefixLength, clockPrefix) == 0 && isdigit((unsigned char)labelName[prefixLength]))
                    isClockLabel = true;
            }
            if (isClockLabel == false)
                continue;
            ROWOWNpcEvent npcEvent;
            npcEvent.NpcID = npc->ID;
            npcEvent.InstructionIndex = labelByName.second;
            ClockEventsByLabel[labelName].push_back(npcEvent);
            clockLabelCount++;
        }
    }
    time_t nowTime = time(nullptr);
    LastClockMinute = int64(nowTime / 60);

    uint32 scriptNpcCount = 0;
    uint32 floatingNpcCount = 0;
    uint32 shopCount = 0;
    for (std::unique_ptr<ROWOWNpc> const& npc : NpcsByID)
    {
        if (npc->IsShop == true)
            shopCount++;
        else if (npc->MapName.empty() == true)
            floatingNpcCount++;
        else
            scriptNpcCount++;
    }
    for (size_t errorIndex = 0; errorIndex < parseErrors.size() && errorIndex < MAX_LOGGED_PARSE_ERRORS; errorIndex++)
        LOG_ERROR("module.ROWOW", "ROWOWMod::LoadNpcScriptData: {}", parseErrors[errorIndex]);
    LOG_INFO("module.ROWOW", "ROWOWMod::LoadNpcScriptData read {} script files: {} NPCs ({} placed, {} floating, {} of them duplicates, {} shops), {} functions, "
        "{} other definitions (warps, monsters, map flags), {} duplicates of warps or unknown NPCs, {} exnames given twice, {} parse errors; {} creatures tied to "
        "their NPCs ({} creature rows match no NPC), {} OnTouch areas, {} clock labels in {} ms", scriptFiles.size(), NpcsByID.size(), scriptNpcCount,
        floatingNpcCount, duplicateCount, shopCount, functionCount, otherDefinitionCount, warpDuplicateCount, duplicateNameCount, parseErrors.size(), mappedCreatureCount,
        unmatchedCreatureCount, touchNpcCount, clockLabelCount, GetMSTimeDiffToNow(oldMSTime));

    // The commands the scripts use, how often, in how many files, and whether the engine has them (for RONotes\script-commands.md)
    std::string commandReport;
    for (auto const& commandUse : commandUses)
    {
        int32 commandIndex = ROWOWSCRIPT->GetCommandIndex(commandUse.first);
        ROWOWScriptCommand const* command = ROWOWSCRIPT->GetCommand(commandIndex);
        static std::set<std::string> const languageWords = { "if", "switch", "for", "while", "do", "set", "end", "return", "goto", "callsub", "callfunc", "function",
            "break", "continue", "getd" };
        char const* status = command == nullptr ? (languageWords.count(commandUse.first) != 0 || ROWOWSCRIPT->GetFunction(commandUse.first) != nullptr ? "language" : "missing")
            : (command->IsStub == true ? "stub" : (command->IsParameter == true ? "parameter" : "done"));
        commandReport += fmt::format("{}{}:{}:{}:{}", commandReport.empty() == true ? "" : ";", commandUse.first, commandUse.second.first, commandUse.second.second.size(), status);
    }
    LOG_INFO("module.ROWOW", "ROWOWMod::LoadNpcScriptData command uses (name:uses:files:status): {}", commandReport);

    // OnInit of every NPC, in load order (npc_event_doall("OnInit")), attached to no player
    uint32 initCount = 0;
    uint32 initStoppedCount = 0;
    for (std::unique_ptr<ROWOWNpc> const& npc : NpcsByID)
    {
        if (npc->Script == nullptr || npc->Script->Error.empty() == false)
            continue;
        int64 initIndex = npc->Script->GetLabel("OnInit");
        if (initIndex < 0)
            continue;
        ROWOWScriptContext context;
        SetupNpcRunContext(context, nullptr, npc.get());
        context.InstructionIndex = size_t(initIndex);
        ROWOWSCRIPT->Run(*npc->Script, context);
        initCount++;
        if (context.Error.empty() == false)
            initStoppedCount++;
        else if (context.Wait == ROWOW_SCRIPT_WAIT_SLEEP)
            SleepingNpcRuns.push_back(std::make_unique<ROWOWScriptContext>(std::move(context)));
    }
    size_t pendingMobCount = 0;
    {
        std::lock_guard<std::mutex> lock(NpcStateMutex);
        pendingMobCount = PendingScriptMobs.size();
    }
    LOG_INFO("module.ROWOW", "ROWOWMod::LoadNpcScriptData ran {} OnInit labels, {} of them stopped early (a command not in yet, or an error); {} script monsters wait for "
        "their maps to load", initCount, initStoppedCount, pendingMobCount);
}

ROWOWNpc const* ROWOWMod::GetNpc(uint32 npcID) const
{
    if (npcID == 0 || npcID > NpcsByID.size())
        return nullptr;
    return NpcsByID[npcID - 1].get();
}

ROWOWNpc const* ROWOWMod::GetNpcByName(std::string const& name, uint32 preferredMapID) const
{
    std::string lowerName = ToLower(name);
    auto uniqueIter = NpcsByUniqueName.find(lowerName);
    if (uniqueIter != NpcsByUniqueName.end())
        return uniqueIter->second;
    ROWOWNpc const* found = nullptr;
    auto range = NpcsByName.equal_range(lowerName);
    for (auto npcIter = range.first; npcIter != range.second; ++npcIter)
    {
        if (found == nullptr || (npcIter->second->MapID == preferredMapID && found->MapID != preferredMapID))
            found = npcIter->second;
    }
    return found;
}

ROWOWNpc const* ROWOWMod::GetCreatureNpc(Creature const* creature) const
{
    if (creature == nullptr)
        return nullptr;
    auto npcIter = NpcsByCreatureSpawnID.find(creature->GetSpawnId());
    return npcIter == NpcsByCreatureSpawnID.end() ? nullptr : npcIter->second;
}

void ROWOWMod::SetupNpcRunContext(ROWOWScriptContext& context, Player* player, ROWOWNpc const* npc)
{
    context.IsNpcRun = true;
    context.Npc = npc;
    context.AttachedPlayer = player;
    if (player != nullptr)
    {
        ROWOWIdentity* identity = GetIdentity(player);
        context.Identity = identity;
        context.CharacterVariables = &identity->Variables;
        context.PlayerVariables = &identity->PlayerVariables;
        context.AccountVariables = &identity->AccountVariables;
    }
}

bool ROWOWMod::IsItemUseBlockedByNpc(Player* player)
{
    ROWOWIdentity* identity = GetIdentity(player);
    return identity->NpcRun != nullptr && identity->NpcRun->IsItemUseDisabled == true;
}

bool ROWOWMod::StartNpcRun(Player* player, ROWOWNpc const* npc, std::string const& label, ROWOWScriptDialog* dialog)
{
    // npc_click / npc_event: one run per player, so a new one ends the one before.  A click's dialog goes to the RO NPC window and ends
    // when the player goes out of range (npc_checknear); the GM's talk gives its dialog (the window or the chat) and talks from anywhere
    if (player == nullptr || npc == nullptr || npc->Script == nullptr || npc->Script->Error.empty() == false)
        return false;
    {
        std::lock_guard<std::mutex> lock(NpcStateMutex);
        if (npc->IsEnabled == false)
            return false;
    }
    size_t startIndex = 0;
    if (label.empty() == false)
    {
        int64 labelIndex = npc->Script->GetLabel(label);
        if (labelIndex < 0)
            return false;
        startIndex = size_t(labelIndex);
    }
    ROWOWIdentity* identity = GetIdentity(player);
    if (identity->NpcRun != nullptr)
        EndNpcRun(player);
    identity->NpcRun = std::make_unique<ROWOWScriptContext>();
    ROWOWScriptContext& context = *identity->NpcRun;
    SetupNpcRunContext(context, player, npc);
    context.Dialog = dialog != nullptr ? dialog : &WindowScriptDialog;
    context.IsNearNpcNeeded = dialog == nullptr;
    context.InstructionIndex = startIndex;
    ROWOWSCRIPT->Run(*npc->Script, context);
    if (identity->NpcRun != nullptr && identity->NpcRun->IsStopped == true)
        EndNpcRun(player);
    return true;
}

void ROWOWMod::EndNpcRun(Player* player)
{
    // The run is over (its end, an error, a cancel or the player leaving): the dialog goes, as npc_scriptcont's end and clif_scriptclose
    ROWOWIdentity* identity = GetIdentity(player);
    if (identity->NpcRun == nullptr)
        return;
    std::unique_ptr<ROWOWScriptContext> run = std::move(identity->NpcRun);
    if (run->Error.empty() == false && run->Dialog != nullptr && player->GetSession() != nullptr)
        ChatHandler(player->GetSession()).PSendSysMessage("|cffff8080(The NPC's script stopped: {})|r", run->Error);
    if (run->Dialog != nullptr && player->GetSession() != nullptr)
        run->Dialog->SendEnd(player, run->Npc);
}

void ROWOWMod::NpcDialogNext(Player* player)
{
    ROWOWIdentity* identity = GetIdentity(player);
    if (identity->NpcRun == nullptr || identity->NpcRun->Wait != ROWOW_SCRIPT_WAIT_NEXT)
        return;
    if (identity->NpcRun->Dialog != nullptr)
        identity->NpcRun->Dialog->SendAnswered(player, identity->NpcRun->Npc);
    ROWOWSCRIPT->Resume(*identity->NpcRun);
    if (identity->NpcRun != nullptr && identity->NpcRun->IsStopped == true)
        EndNpcRun(player);
}

void ROWOWMod::NpcDialogSelect(Player* player, int32 choice)
{
    // clif_parse_NpcSelectMenu: the choice goes in npc_menu, and the menu command runs again to take it (255 is cancel)
    ROWOWIdentity* identity = GetIdentity(player);
    if (identity->NpcRun == nullptr || identity->NpcRun->Wait != ROWOW_SCRIPT_WAIT_MENU)
        return;
    ROWOWScriptContext& context = *identity->NpcRun;
    if (context.Dialog != nullptr)
        context.Dialog->SendAnswered(player, context.Npc);
    context.NpcMenu = choice;
    ROWOWSCRIPT->Resume(context);
    if (identity->NpcRun != nullptr && identity->NpcRun->IsStopped == true)
        EndNpcRun(player);
}

void ROWOWMod::NpcDialogInput(Player* player, std::string const& value)
{
    // clif_parse_NpcAmountInput and clif_parse_NpcStringInput
    ROWOWIdentity* identity = GetIdentity(player);
    if (identity->NpcRun == nullptr || identity->NpcRun->Wait != ROWOW_SCRIPT_WAIT_INPUT)
        return;
    ROWOWScriptContext& context = *identity->NpcRun;
    if (context.Dialog != nullptr)
        context.Dialog->SendAnswered(player, context.Npc);
    context.NpcString = value;
    context.NpcAmount = int64(strtoll(value.c_str(), nullptr, 10));
    ROWOWSCRIPT->Resume(context);
    if (identity->NpcRun != nullptr && identity->NpcRun->IsStopped == true)
        EndNpcRun(player);
}

void ROWOWMod::NpcDialogClose(Player* player)
{
    // clif_parse_NpcCloseClicked: close2 goes on, close ends; closing the window at any other wait ends the run
    ROWOWIdentity* identity = GetIdentity(player);
    if (identity->NpcRun == nullptr)
        return;
    ROWOWScriptContext& context = *identity->NpcRun;
    context.IsMesActive = false;
    if (context.Wait == ROWOW_SCRIPT_WAIT_CLOSE)
    {
        ROWOWSCRIPT->Resume(context);
        if (identity->NpcRun != nullptr && identity->NpcRun->IsStopped == true)
            EndNpcRun(player);
        return;
    }
    if (context.Wait == ROWOW_SCRIPT_WAIT_SLEEP)
        return;
    EndNpcRun(player);
}

void ROWOWMod::RunNpcEvent(ROWOWNpc const* npc, uint32 instructionIndex, Player* player)
{
    // npc_event_do: a label of the NPC's script, with the player attached or none.  A run with no player that sleeps waits in the world
    // update; a player's that waits for the dialog becomes its run when it has none
    if (npc == nullptr || npc->Script == nullptr || npc->Script->Error.empty() == false)
        return;
    std::unique_ptr<ROWOWScriptContext> context = std::make_unique<ROWOWScriptContext>();
    SetupNpcRunContext(*context, player, npc);
    context->InstructionIndex = instructionIndex;
    if (player != nullptr)
    {
        context->Dialog = &WindowScriptDialog;
        context->IsNearNpcNeeded = true;
    }
    ROWOWSCRIPT->Run(*npc->Script, *context);
    if (context->IsStopped == true || context->IsWaiting() == false)
    {
        // A dialog the run opened goes with it
        if (player != nullptr && context->IsMesActive == true && player->GetSession() != nullptr)
            context->Dialog->SendEnd(player, npc);
        return;
    }
    if (player == nullptr)
    {
        if (context->Wait == ROWOW_SCRIPT_WAIT_SLEEP)
            SleepingNpcRuns.push_back(std::move(context));
        return;
    }
    ROWOWIdentity* identity = GetIdentity(player);
    if (identity->NpcRun == nullptr)
        identity->NpcRun = std::move(context);
}

void ROWOWMod::RunPlayerNpcEvent(Player* player, ROWOWNpc const* npc, uint32 instructionIndex, bool isTouch)
{
    // npc_event_sub: a player in a run gets the event once the run ends (npc_event_enqueue, at most MAX_EVENTQUEUE).  In its thread
    if (player == nullptr || npc == nullptr)
        return;
    ROWOWIdentity* identity = GetIdentity(player);
    if (identity->NpcRun != nullptr)
    {
        if (identity->QueuedNpcEvents.size() >= MAX_QUEUED_NPC_EVENTS)
            return;
        ROWOWNpcEvent npcEvent;
        npcEvent.NpcID = npc->ID;
        npcEvent.InstructionIndex = instructionIndex;
        npcEvent.IsTouch = isTouch;
        identity->QueuedNpcEvents.push_back(npcEvent);
        return;
    }
    RunNpcEvent(npc, instructionIndex, player);
}

void ROWOWMod::RunPlayerEvent(Player* player, uint8 eventType)
{
    // npc_script_event: OnPCDieEvent and the others, every NPC's label of the name, with the player attached
    if (player == nullptr || eventType >= ROWOW_NPC_EVENT_COUNT || IsInROForm(player) == false)
        return;
    for (ROWOWNpcEvent const& npcEvent : PlayerEventLabels[eventType])
        RunPlayerNpcEvent(player, GetNpc(npcEvent.NpcID), npcEvent.InstructionIndex);
}

void ROWOWMod::UpdatePlayerNpcScripts(Player* player, uint32 diff)
{
    // The player's update (its map's thread): its sleeping run wakes, its queued events run once it has no run, its OnTouch areas are
    // checked when it's on another cell, and its dialog ends when it's out of the NPC's range
    ROWOWIdentity* identity = GetIdentity(player);
    if (identity->NpcRun != nullptr && identity->NpcRun->Wait == ROWOW_SCRIPT_WAIT_SLEEP && uint64(GameTime::GetGameTimeMS().count()) >= identity->NpcRun->SleepUntil)
    {
        ROWOWSCRIPT->Resume(*identity->NpcRun);
        if (identity->NpcRun != nullptr && identity->NpcRun->IsStopped == true)
            EndNpcRun(player);
    }

    // npc_event_dequeue: an OnTouch event only while the player is still on the area
    while (identity->NpcRun == nullptr && identity->QueuedNpcEvents.empty() == false)
    {
        ROWOWNpcEvent npcEvent = identity->QueuedNpcEvents.front();
        identity->QueuedNpcEvents.pop_front();
        ROWOWNpc const* npc = GetNpc(npcEvent.NpcID);
        if (npc == nullptr)
            continue;
        if (npcEvent.IsTouch == true)
        {
            int32 x = 0, y = 0;
            GetUnitCell(player, x, y);
            if (npc->IsInArea(player->GetMapId(), x, y) == false)
                continue;
        }
        RunNpcEvent(npc, npcEvent.InstructionIndex, player);
    }

    CheckNpcTouchAreas(player);

    // The dialog of an NPC with a place ends when the player is off its map or more than AREA_SIZE + 1 cells away (npc_checknear)
    identity->NpcRangeCheckTimer += diff;
    if (identity->NpcRangeCheckTimer < NPC_RANGE_CHECK_INTERVAL_IN_MS)
        return;
    identity->NpcRangeCheckTimer = 0;
    ROWOWScriptContext const* run = identity->NpcRun.get();
    if (run == nullptr || run->IsNearNpcNeeded == false || run->Npc == nullptr || run->Npc->MapID == 0 || run->Npc->IsClassless() == true ||
        run->Wait == ROWOW_SCRIPT_WAIT_SLEEP)
        return;
    int32 x = 0, y = 0;
    GetUnitCell(player, x, y);
    int32 range = BattleConfig.AreaSize + 1;
    if (player->GetMapId() != run->Npc->MapID || std::abs(x - run->Npc->X) > range || std::abs(y - run->Npc->Y) > range)
        EndNpcRun(player);
}

bool ROWOWMod::IsInNpcClickRange(Player const* player, Creature const* creature) const
{
    // npc_checknear: on the same map and at most AREA_SIZE + 1 cells away on each axis, whatever the height between (RO's cells have none) and
    // whatever stands in the way.  A class-less NPC is clicked from anywhere
    if (player->GetMapId() != creature->GetMapId())
        return false;
    ROWOWNpc const* npc = GetCreatureNpc(creature);
    if (npc != nullptr && npc->IsClassless() == true)
        return true;
    return DistanceBL(player, creature) <= BattleConfig.AreaSize + 1;
}

void ROWOWMod::CheckNpcTouchAreas(Player* player)
{
    // unit_walktoxy_timer's npc_touchnext_areanpc and npc_touch_area_allnpc, when the player is on another cell (its map's thread).  Not
    // during a teleport: the cell it lands on is checked once it's there (clif_parse_LoadEndAck), else an arrival's cell was taken as
    // checked while the teleport was still pending and its areas never went off
    if (player->IsBeingTeleported() == true)
        return;
    ROWOWIdentity* identity = GetIdentity(player);
    uint32 mapID = player->GetMapId();
    int32 x = 0, y = 0;
    GetUnitCell(player, x, y);
    if (mapID == identity->NpcTouchMapID && x == identity->NpcTouchX && y == identity->NpcTouchY)
        return;
    bool isSameMap = mapID == identity->NpcTouchMapID;
    identity->NpcTouchMapID = mapID;
    identity->NpcTouchX = x;
    identity->NpcTouchY = y;

    // npc_touchnext_areanpc: off an OnTouch_ area the NPC is free again, and its OnTouch_ goes to another player on the area (on this map
    // only: another map is another thread's)
    for (size_t npcIndex = 0; npcIndex < identity->OnTouchNpcIDs.size();)
    {
        ROWOWNpc const* npc = GetNpc(identity->OnTouchNpcIDs[npcIndex]);
        if (npc != nullptr && npc->IsInArea(mapID, x, y) == true)
        {
            npcIndex++;
            continue;
        }
        identity->OnTouchNpcIDs.erase(identity->OnTouchNpcIDs.begin() + npcIndex);
        if (npc == nullptr)
            continue;
        bool wasTouching = false;
        {
            std::lock_guard<std::mutex> lock(NpcStateMutex);
            ROWOWNpc* touchedNpc = NpcsByID[npc->ID - 1].get();
            if (touchedNpc->TouchingPlayerGUID == player->GetGUID())
            {
                touchedNpc->TouchingPlayerGUID.Clear();
                wasTouching = true;
            }
        }
        if (wasTouching == false || isSameMap == false || player->GetMap() == nullptr)
            continue;
        Player* nextPlayer = nullptr;
        player->GetMap()->DoForAllPlayers([&](Player* otherPlayer)
        {
            // npc_touch_areanpc_sub: not one warping, hiding or dead
            if (nextPlayer != nullptr || otherPlayer == player || otherPlayer->IsBeingTeleported() == true || otherPlayer->IsAlive() == false ||
                IsInROForm(otherPlayer) == false || IsHidingFromNpcs(otherPlayer) == true)
                return;
            int32 otherX = 0, otherY = 0;
            GetUnitCell(otherPlayer, otherX, otherY);
            if (npc->IsInArea(otherPlayer->GetMapId(), otherX, otherY) == true)
                nextPlayer = otherPlayer;
        });
        int64 labelIndex = npc->Script->GetLabel("OnTouch_");
        if (nextPlayer == nullptr || labelIndex < 0)
            continue;
        {
            std::lock_guard<std::mutex> lock(NpcStateMutex);
            NpcsByID[npc->ID - 1]->TouchingPlayerGUID = nextPlayer->GetGUID();
        }
        ROWOWIdentity* nextIdentity = GetIdentity(nextPlayer);
        if (std::find(nextIdentity->OnTouchNpcIDs.begin(), nextIdentity->OnTouchNpcIDs.end(), npc->ID) == nextIdentity->OnTouchNpcIDs.end())
            nextIdentity->OnTouchNpcIDs.push_back(npc->ID);
        RunPlayerNpcEvent(nextPlayer, npc, uint32(labelIndex), true);
    }

    // npc_touch_area_allnpc: the areas it's left are forgotten, then each it's on is touched (warps are the converter's zone lines)
    identity->AreaNpcIDs.erase(std::remove_if(identity->AreaNpcIDs.begin(), identity->AreaNpcIDs.end(), [&](uint32 npcID)
    {
        ROWOWNpc const* npc = GetNpc(npcID);
        return npc == nullptr || npc->IsInArea(mapID, x, y) == false;
    }), identity->AreaNpcIDs.end());
    auto npcIDsIter = TouchNpcIDsByMapID.find(mapID);
    if (npcIDsIter == TouchNpcIDsByMapID.end() || IsInROForm(player) == false)
        return;
    // A trade holds its items by GUID until both accept, so no script starts on a player in one: its delitem would empty the trade's side
    // while the other side's zeny still moved (rAthena's trade_check fails the commit instead).  The cell is checked again until the trade
    // ends, so an area under the player still goes off then
    if (player->GetTradeData() != nullptr)
    {
        identity->NpcTouchX = -1;
        return;
    }
    for (uint32 npcID : npcIDsIter->second)
    {
        ROWOWNpc const* npc = GetNpc(npcID);
        if (npc != nullptr && npc->IsInArea(mapID, x, y) == true)
            TouchNpcArea(player, npc);
    }
}

bool ROWOWMod::IsHidingFromNpcs(Player* player) const
{
    // pc_ishiding
    ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(player));
    return sc != nullptr && (sc->Option & (ROWOW_OPTION_HIDE | ROWOW_OPTION_CLOAK | ROWOW_OPTION_CHASEWALK)) != 0;
}

void ROWOWMod::TouchNpcArea(Player* player, ROWOWNpc const* npc)
{
    // npc_touch_areanpc for a script NPC: a disabled one does nothing.  OnTouch_ (npc_ontouch_event: one player at a time, not one dead
    // or hiding), else OnTouch (npc_ontouch2_event: each player once per entry), else the NPC is clicked
    if (player->IsAlive() == false)
        return;
    ROWOWIdentity* identity = GetIdentity(player);
    int64 touchLabelIndex = npc->Script->GetLabel("OnTouch_");
    {
        std::lock_guard<std::mutex> lock(NpcStateMutex);
        ROWOWNpc* touchedNpc = NpcsByID[npc->ID - 1].get();
        if (touchedNpc->IsEnabled == false || touchedNpc->TouchingPlayerGUID.IsEmpty() == false)
            return;
        if (std::find(identity->OnTouchNpcIDs.begin(), identity->OnTouchNpcIDs.end(), npc->ID) != identity->OnTouchNpcIDs.end())
            return;
        if (touchLabelIndex >= 0)
        {
            if (IsHidingFromNpcs(player) == true)
                return;
            touchedNpc->TouchingPlayerGUID = player->GetGUID();
        }
    }
    if (touchLabelIndex >= 0)
    {
        identity->OnTouchNpcIDs.push_back(npc->ID);
        RunPlayerNpcEvent(player, npc, uint32(touchLabelIndex), true);
        return;
    }
    if (std::find(identity->AreaNpcIDs.begin(), identity->AreaNpcIDs.end(), npc->ID) != identity->AreaNpcIDs.end())
        return;
    identity->AreaNpcIDs.push_back(npc->ID);
    int64 touch2LabelIndex = npc->Script->GetLabel("OnTouch");
    if (touch2LabelIndex >= 0)
        RunPlayerNpcEvent(player, npc, uint32(touch2LabelIndex), true);
    else if (identity->NpcRun == nullptr)
        StartNpcRun(player, npc);
}

void ROWOWMod::ForgetNpcTouches(Player* player)
{
    // Leaving the world (npc_touchnext_areanpc's leavemap): the OnTouch_ NPCs it held are free again, with no hand-off (the next player to
    // step on the area takes it)
    ROWOWIdentity* identity = GetIdentity(player);
    {
        std::lock_guard<std::mutex> lock(NpcStateMutex);
        for (uint32 npcID : identity->OnTouchNpcIDs)
            if (npcID != 0 && npcID <= NpcsByID.size() && NpcsByID[npcID - 1]->TouchingPlayerGUID == player->GetGUID())
                NpcsByID[npcID - 1]->TouchingPlayerGUID.Clear();
    }
    identity->OnTouchNpcIDs.clear();
    identity->AreaNpcIDs.clear();
    identity->NpcTouchMapID = 0;
    identity->NpcTouchX = -1;
    identity->NpcTouchY = -1;
}

void ROWOWMod::SetNpcTimerPlayer(ROWOWNpc const* npc, ObjectGuid playerGUID)
{
    if (npc == nullptr)
        return;
    std::lock_guard<std::mutex> lock(NpcStateMutex);
    NpcsByID[npc->ID - 1]->TimerPlayerGUID = playerGUID;
}

void ROWOWMod::QueueNpcEvent(ROWOWNpc const* npc, std::string const& label)
{
    if (npc == nullptr || npc->Script == nullptr)
        return;
    int64 labelIndex = npc->Script->GetLabel(label);
    if (labelIndex < 0)
        return;
    ROWOWNpcEvent npcEvent;
    npcEvent.NpcID = npc->ID;
    npcEvent.InstructionIndex = uint32(labelIndex);
    std::lock_guard<std::mutex> lock(NpcStateMutex);
    PendingNpcEvents.push_back(npcEvent);
}

bool ROWOWMod::DoNpcEvent(std::string const& eventName)
{
    // donpcevent (npc_event_do): rAthena runs the event at once, inside the run that asked, and the scripts count on that order (the
    // Wizard test's door stage disables the room before it, whose killmonsterall must go before the door's own monsters spawn).  In the
    // world update (the maps idle) it runs now too, nested at most a few deep; from a map's thread it can't (the event may touch other
    // maps), so it waits for the next world update, in order with the others
    static thread_local uint32 nestedEventDepth = 0;
    if (IsInNpcWorldUpdate == false || nestedEventDepth >= MAX_NESTED_NPC_EVENTS)
        return QueueNpcEvent(eventName);
    size_t separatorPosition = eventName.find("::");
    if (separatorPosition == std::string::npos)
        return false;
    std::string npcName = eventName.substr(0, separatorPosition);
    std::string label = eventName.substr(separatorPosition + 2);
    std::vector<std::pair<ROWOWNpc const*, uint32>> events;
    if (npcName.empty() == true)
    {
        for (std::unique_ptr<ROWOWNpc> const& npc : NpcsByID)
            if (npc->Script != nullptr && npc->Script->GetLabel(label) >= 0)
                events.emplace_back(npc.get(), uint32(npc->Script->GetLabel(label)));
    }
    else
    {
        auto npcIter = NpcsByUniqueName.find(ToLower(npcName));
        if (npcIter != NpcsByUniqueName.end() && npcIter->second->Script != nullptr && npcIter->second->Script->GetLabel(label) >= 0)
            events.emplace_back(npcIter->second, uint32(npcIter->second->Script->GetLabel(label)));
    }
    nestedEventDepth++;
    for (auto const& npcEvent : events)
        RunNpcEvent(npcEvent.first, npcEvent.second);
    nestedEventDepth--;
    return events.empty() == false;
}

bool ROWOWMod::QueueNpcEvent(std::string const& eventName)
{
    // "<NPC>::<label>"; "::<label>" alone is that label of every NPC that has it (npc_event_doall)
    size_t separatorPosition = eventName.find("::");
    if (separatorPosition == std::string::npos)
        return false;
    std::string npcName = eventName.substr(0, separatorPosition);
    std::string label = eventName.substr(separatorPosition + 2);
    if (npcName.empty() == true)
    {
        bool isFound = false;
        for (std::unique_ptr<ROWOWNpc> const& npc : NpcsByID)
        {
            if (npc->Script != nullptr && npc->Script->GetLabel(label) >= 0)
            {
                QueueNpcEvent(npc.get(), label);
                isFound = true;
            }
        }
        return isFound;
    }
    auto npcIter = NpcsByUniqueName.find(ToLower(npcName));
    if (npcIter == NpcsByUniqueName.end() || npcIter->second->Script == nullptr || npcIter->second->Script->GetLabel(label) < 0)
        return false;
    QueueNpcEvent(npcIter->second, label);
    return true;
}

void ROWOWMod::SetNpcEnabled(ROWOWNpc const* npc, bool isEnabled)
{
    // npc_enable: the NPC's creatures show or go at the next world update (they can be on another map's thread).  Its timer goes on: the job
    // tests disable their stage's NPC from its own OnTimer label, and its later labels (the warp out, the room reopening) must still run
    if (npc == nullptr)
        return;
    std::lock_guard<std::mutex> lock(NpcStateMutex);
    NpcsByID[npc->ID - 1]->IsEnabled = isEnabled;
    NpcVisibilityChangedIDs.insert(npc->ID);
}

void ROWOWMod::SetNpcHidden(ROWOWNpc const* npc, bool isHidden)
{
    if (npc == nullptr)
        return;
    std::lock_guard<std::mutex> lock(NpcStateMutex);
    NpcsByID[npc->ID - 1]->IsHidden = isHidden;
    NpcVisibilityChangedIDs.insert(npc->ID);
}

void ROWOWMod::NpcTimerStart(ROWOWNpc const* npc, bool isFromZero)
{
    // npc_timerevent_start: from the tick it holds (or 0), the next label being the first past that tick
    if (npc == nullptr)
        return;
    std::lock_guard<std::mutex> lock(NpcStateMutex);
    ROWOWNpc* timerNpc = NpcsByID[npc->ID - 1].get();
    ROWOWNpcTimer& timer = timerNpc->Timer;
    uint64 now = GameTime::GetGameTimeMS().count();
    int64 tick = isFromZero == true ? 0 : (timer.IsRunning == true ? int64(now - timer.StartTime) : timer.StoppedTick);
    timer.StartTime = now - uint64(std::max<int64>(0, tick));
    timer.IsRunning = true;
    timer.NextEventIndex = 0;
    while (timer.NextEventIndex < timerNpc->TimerEvents.size() && timerNpc->TimerEvents[timer.NextEventIndex].first < tick)
        timer.NextEventIndex++;
    RunningNpcTimerIDs.insert(npc->ID);
}

void ROWOWMod::NpcTimerStop(ROWOWNpc const* npc)
{
    if (npc == nullptr)
        return;
    std::lock_guard<std::mutex> lock(NpcStateMutex);
    ROWOWNpcTimer& timer = NpcsByID[npc->ID - 1]->Timer;
    if (timer.IsRunning == true)
        timer.StoppedTick = int64(GameTime::GetGameTimeMS().count() - timer.StartTime);
    timer.IsRunning = false;
    RunningNpcTimerIDs.erase(npc->ID);
}

void ROWOWMod::NpcTimerSet(ROWOWNpc const* npc, int64 tick)
{
    // npc_settimerevent_tick: a running timer goes on from the new tick
    if (npc == nullptr)
        return;
    bool isRunning = false;
    {
        std::lock_guard<std::mutex> lock(NpcStateMutex);
        ROWOWNpcTimer& timer = NpcsByID[npc->ID - 1]->Timer;
        timer.StoppedTick = tick;
        isRunning = timer.IsRunning;
        timer.IsRunning = false;
    }
    if (isRunning == true)
        NpcTimerStart(npc, false);
}

int64 ROWOWMod::NpcTimerGet(ROWOWNpc const* npc, int32 type)
{
    if (npc == nullptr)
        return 0;
    std::lock_guard<std::mutex> lock(NpcStateMutex);
    ROWOWNpcTimer const& timer = npc->Timer;
    switch (type)
    {
        case 0: return timer.IsRunning == true ? int64(GameTime::GetGameTimeMS().count() - timer.StartTime) : timer.StoppedTick;
        case 1: return timer.IsRunning == true ? 1 : 0;
        case 2: return int64(npc->TimerEvents.size());
        default: return 0;
    }
}

thread_local bool ROWOWMod::IsInNpcWorldUpdate = false;

void ROWOWMod::UpdateNpcScripts(uint32 diff)
{
    // The world update, between the map updates: queued events, timers, sleeping runs, the NPC creatures' shown state, and the globals.
    // Runs here know the maps are idle (IsInNpcWorldUpdate: warpwaitingpc warps at once)
    if (IsEnabled == false || NpcsByID.empty() == true)
        return;
    struct WorldUpdateFlag
    {
        WorldUpdateFlag() { IsInNpcWorldUpdate = true; }
        ~WorldUpdateFlag() { IsInNpcWorldUpdate = false; }
    } worldUpdateFlag;
    uint64 now = GameTime::GetGameTimeMS().count();

    std::vector<ROWOWNpcEvent> dueEvents;
    std::set<uint32> visibilityChangedIDs;
    std::vector<ROWOWNpcAnnouncement> announcements;
    std::vector<ROWOWScriptMobKill> kills;
    std::vector<std::pair<uint32, ObjectGuid>> killedMobs;
    std::vector<ROWOWPendingScriptMob> loadedMapMobs;
    std::vector<ROWOWNpcMapWarp> mapWarps;
    {
        std::lock_guard<std::mutex> lock(NpcStateMutex);
        dueEvents.swap(PendingNpcEvents);
        mapWarps.swap(PendingNpcMapWarps);
        visibilityChangedIDs.swap(NpcVisibilityChangedIDs);
        announcements.swap(PendingNpcAnnouncements);
        kills.swap(PendingScriptMobKills);

        // Monsters scripts spawned before their map was loaded (an OnInit's) come once it is
        for (size_t mobIndex = 0; mobIndex < PendingScriptMobs.size();)
        {
            if (sMapMgr->FindBaseNonInstanceMap(PendingScriptMobs[mobIndex].MapID) == nullptr)
            {
                mobIndex++;
                continue;
            }
            loadedMapMobs.push_back(PendingScriptMobs[mobIndex]);
            PendingScriptMobs.erase(PendingScriptMobs.begin() + mobIndex);
        }
        for (ROWOWScriptMobKill const& kill : kills)
            for (auto const& scriptMobByGUID : ScriptMobsByGUID)
                if (scriptMobByGUID.second.MapID == kill.MapID && (kill.IsAll == true || scriptMobByGUID.second.Event == kill.Event))
                    killedMobs.emplace_back(kill.MapID, scriptMobByGUID.first);

        // npc_timerevent: each label the time has passed; after the last the timer stops there
        for (auto timerIter = RunningNpcTimerIDs.begin(); timerIter != RunningNpcTimerIDs.end();)
        {
            ROWOWNpc* npc = NpcsByID[*timerIter - 1].get();
            ROWOWNpcTimer& timer = npc->Timer;
            int64 tick = int64(now - timer.StartTime);
            while (timer.NextEventIndex < npc->TimerEvents.size() && npc->TimerEvents[timer.NextEventIndex].first <= tick)
            {
                ROWOWNpcEvent npcEvent;
                npcEvent.NpcID = npc->ID;
                npcEvent.InstructionIndex = npc->TimerEvents[timer.NextEventIndex].second;
                npcEvent.PlayerGUID = npc->TimerPlayerGUID;
                dueEvents.push_back(npcEvent);
                timer.NextEventIndex++;
            }
            if (timer.NextEventIndex >= npc->TimerEvents.size())
            {
                timer.StoppedTick = npc->TimerEvents.empty() == true ? tick : npc->TimerEvents.back().first;
                timer.IsRunning = false;
                timerIter = RunningNpcTimerIDs.erase(timerIter);
            }
            else
                ++timerIter;
        }
    }
    for (ROWOWPendingScriptMob const& mob : loadedMapMobs)
        SpawnScriptMob(sMapMgr->FindBaseNonInstanceMap(mob.MapID), mob.MobID, mob.X, mob.Y, mob.Name, mob.Event);
    if (loadedMapMobs.empty() == false)
        LOG_INFO("module.ROWOW", "ROWOWMod::UpdateNpcScripts spawned {} script monsters that waited for their map (first map {})", loadedMapMobs.size(),
            loadedMapMobs.front().MapID);

    // npc_event_do_clock: each minute's OnMinute<mm>, OnClock<hhmm> and On<Day><hhmm>, each hour's OnHour<hh>, each day's OnDay<MMdd>
    // (local time), run with no player
    int64 nowMinute = int64(time(nullptr) / 60);
    if (nowMinute != LastClockMinute)
    {
        tm lastTime = Acore::Time::TimeBreakdown(time_t(LastClockMinute * 60));
        tm nowTime = Acore::Time::TimeBreakdown(time_t(nowMinute * 60));
        LastClockMinute = nowMinute;
        static char const* const dayNames[] = { "sun", "mon", "tue", "wed", "thu", "fri", "sat" };
        std::vector<std::string> labelNames;
        labelNames.push_back(fmt::format("onminute{:02}", nowTime.tm_min));
        labelNames.push_back(fmt::format("onclock{:02}{:02}", nowTime.tm_hour, nowTime.tm_min));
        labelNames.push_back(fmt::format("on{}{:02}{:02}", dayNames[nowTime.tm_wday % 7], nowTime.tm_hour, nowTime.tm_min));
        if (nowTime.tm_hour != lastTime.tm_hour)
            labelNames.push_back(fmt::format("onhour{:02}", nowTime.tm_hour));
        if (nowTime.tm_mday != lastTime.tm_mday)
            labelNames.push_back(fmt::format("onday{:02}{:02}", nowTime.tm_mon + 1, nowTime.tm_mday));
        for (std::string const& labelName : labelNames)
        {
            auto clockEventsIter = ClockEventsByLabel.find(labelName);
            if (clockEventsIter != ClockEventsByLabel.end())
                dueEvents.insert(dueEvents.end(), clockEventsIter->second.begin(), clockEventsIter->second.end());
        }
    }

    // The events: an attached timer's with its player (the maps aren't updating, so any player's run can start here), whose leaving
    // stops the timer (npc_timerevent)
    for (ROWOWNpcEvent const& npcEvent : dueEvents)
    {
        if (npcEvent.PlayerGUID.IsEmpty() == true)
        {
            RunNpcEvent(GetNpc(npcEvent.NpcID), npcEvent.InstructionIndex);
            continue;
        }
        Player* player = ObjectAccessor::FindPlayer(npcEvent.PlayerGUID);
        if (player == nullptr || IsInROForm(player) == false)
        {
            NpcTimerStop(GetNpc(npcEvent.NpcID));
            SetNpcTimerPlayer(GetNpc(npcEvent.NpcID), ObjectGuid::Empty);
            continue;
        }
        RunPlayerNpcEvent(player, GetNpc(npcEvent.NpcID), npcEvent.InstructionIndex);
    }

    // Announcements, to the players in the RO form: no WoW or EverQuest character hears RO's scripts.  The world thread: it owns the sessions, and
    // the maps aren't updating
    for (ROWOWNpcAnnouncement const& announcement : announcements)
    {
        if (announcement.MapID == 0)
        {
            for (auto const& sessionByID : sWorldSessionMgr->GetAllSessions())
            {
                Player* player = sessionByID.second->GetPlayer();
                if (player != nullptr && player->IsInWorld() == true && IsInROForm(player) == true)
                    ChatHandler(sessionByID.second).SendSysMessage(announcement.Text);
            }
            continue;
        }
        if (Map* map = sMapMgr->FindBaseNonInstanceMap(announcement.MapID))
        {
            map->DoForAllPlayers([this, &announcement](Player* player)
            {
                int32 x = 0, y = 0;
                GetUnitCell(player, x, y);
                if (IsInROForm(player) == true && (announcement.CellX < 0 ||
                    (std::abs(x - announcement.CellX) <= BattleConfig.AreaSize && std::abs(y - announcement.CellY) <= BattleConfig.AreaSize)))
                    ChatHandler(player->GetSession()).SendSysMessage(announcement.Text);
            });
        }
    }

    // killmonster: the script monsters go without a death (and without their events, as rAthena's killmonster)
    for (auto const& killedMob : killedMobs)
    {
        {
            std::lock_guard<std::mutex> lock(NpcStateMutex);
            ScriptMobsByGUID.erase(killedMob.second);
        }
        Map* map = sMapMgr->FindBaseNonInstanceMap(killedMob.first);
        if (Creature* creature = map != nullptr ? map->GetCreature(killedMob.second) : nullptr)
            creature->DespawnOrUnsummon();
    }

    // mapwarp and areawarp: the players in the RO form on the map (or in the box) go
    for (ROWOWNpcMapWarp const& mapWarp : mapWarps)
    {
        Map* map = sMapMgr->FindBaseNonInstanceMap(mapWarp.MapID);
        if (map == nullptr)
            continue;
        std::vector<Player*> warpedPlayers;
        map->DoForAllPlayers([this, &mapWarp, &warpedPlayers](Player* player)
        {
            if (IsInROForm(player) == false || player->IsBeingTeleported() == true)
                return;
            int32 x = 0, y = 0;
            GetUnitCell(player, x, y);
            if (mapWarp.X1 >= 0 && (x < std::min(mapWarp.X1, mapWarp.X2) || x > std::max(mapWarp.X1, mapWarp.X2) || y < std::min(mapWarp.Y1, mapWarp.Y2) ||
                y > std::max(mapWarp.Y1, mapWarp.Y2)))
                return;
            if (mapWarp.GroupGUID.IsEmpty() == false)
            {
                if (player->GetGroup() == nullptr || player->GetGroup()->GetGUID() != mapWarp.GroupGUID)
                    return;
                // buildin_warpparty's map flags: nowarp holds back a random warp, noreturn a warp to the save point, and either one a warp to a real
                // map, but for the player who asked
                bool isHeldBack = false;
                if (mapWarp.DestMapName == "Random")
                    isHeldBack = HasMapFlag(mapWarp.MapID, "nowarp");
                else if (mapWarp.DestMapName == "SavePoint" || mapWarp.DestMapName == "SavePointAll")
                    isHeldBack = HasMapFlag(mapWarp.MapID, "noreturn");
                else
                    isHeldBack = player->GetGUID() != mapWarp.RequesterGUID && (HasMapFlag(mapWarp.MapID, "nowarp") == true || HasMapFlag(mapWarp.MapID, "noreturn") == true);
                if (isHeldBack == true)
                    return;
            }
            warpedPlayers.push_back(player);
        });
        for (Player* player : warpedPlayers)
        {
            int32 destX = mapWarp.DestX;
            int32 destY = mapWarp.DestY;
            if (mapWarp.DestX2 >= 0 && mapWarp.DestY2 >= 0)
            {
                destX = int32(urand(uint32(std::min(mapWarp.DestX, mapWarp.DestX2)), uint32(std::max(mapWarp.DestX, mapWarp.DestX2))));
                destY = int32(urand(uint32(std::min(mapWarp.DestY, mapWarp.DestY2)), uint32(std::max(mapWarp.DestY, mapWarp.DestY2))));
            }
            WarpPlayerToCell(player, mapWarp.DestMapName, destX, destY);
        }
    }

    // map_getmapdata(m)->users for getmapusers, as of now (the map threads read it until the next update)
    {
        std::unordered_map<uint32, uint32> mapUserCounts;
        for (auto const& mapGridByMapID : MapGridsByMapID)
            if (Map* map = sMapMgr->FindBaseNonInstanceMap(mapGridByMapID.first))
                mapUserCounts[mapGridByMapID.first] = uint32(map->GetPlayers().getSize());
        std::lock_guard<std::mutex> lock(NpcStateMutex);
        MapUserCounts.swap(mapUserCounts);
    }

    // Runs with no player waking from sleep
    for (size_t runIndex = 0; runIndex < SleepingNpcRuns.size();)
    {
        ROWOWScriptContext& context = *SleepingNpcRuns[runIndex];
        if (context.Wait == ROWOW_SCRIPT_WAIT_SLEEP && now < context.SleepUntil)
        {
            runIndex++;
            continue;
        }
        ROWOWSCRIPT->Resume(context);
        if (context.IsStopped == false && context.Wait == ROWOW_SCRIPT_WAIT_SLEEP)
            runIndex++;
        else
            SleepingNpcRuns.erase(SleepingNpcRuns.begin() + runIndex);
    }

    // The creatures of NPCs scripts showed or hid (the maps aren't updating now).  An NPC enabled or shown with an OnTouch area touches the
    // players already standing on it (npc_enable's npc_enable_sub): the Knight's patience test, the Dancer's steps
    for (uint32 npcID : visibilityChangedIDs)
    {
        ROWOWNpc const* npc = GetNpc(npcID);
        if (npc == nullptr || npc->MapID == 0)
            continue;
        Map* map = sMapMgr->FindBaseNonInstanceMap(npc->MapID);
        if (map == nullptr)
            continue;
        bool isVisible = false;
        bool isEnabled = false;
        {
            std::lock_guard<std::mutex> lock(NpcStateMutex);
            isVisible = npc->IsEnabled == true && npc->IsHidden == false;
            isEnabled = npc->IsEnabled;
        }
        if (isEnabled == true && npc->TriggerSpanX >= 0 && npc->TriggerSpanY >= 0 && npc->Script != nullptr)
        {
            std::vector<Player*> touchingPlayers;
            map->DoForAllPlayers([this, npc, &touchingPlayers](Player* player)
            {
                int32 x = 0, y = 0;
                GetUnitCell(player, x, y);
                if (IsInROForm(player) == true && player->IsBeingTeleported() == false && npc->IsInArea(player->GetMapId(), x, y) == true)
                    touchingPlayers.push_back(player);
            });
            for (Player* player : touchingPlayers)
                TouchNpcArea(player, npc);
        }
        if (npc->CreatureSpawnIDs.empty() == true)
            continue;
        for (ObjectGuid::LowType spawnID : npc->CreatureSpawnIDs)
        {
            auto creatureRange = map->GetCreatureBySpawnIdStore().equal_range(spawnID);
            for (auto creatureIter = creatureRange.first; creatureIter != creatureRange.second; ++creatureIter)
                creatureIter->second->SetVisible(isVisible);
        }
    }

    // The changed global variables, every few seconds
    GlobalVariableSaveTimer += diff;
    if (GlobalVariableSaveTimer < GLOBAL_VARIABLE_SAVE_INTERVAL_IN_MS)
        return;
    GlobalVariableSaveTimer = 0;
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    uint32 statementCount = 0;
    {
        std::lock_guard<std::mutex> lock(ROWOWSCRIPT->VariableMutex);
        ROWOWScriptVariables& globals = ROWOWSCRIPT->GlobalVariables;
        for (auto const& changedElement : globals.ChangedElements)
        {
            std::string name = changedElement.first;
            CharacterDatabase.EscapeString(name);
            ROWOWScriptValue value = globals.Get(changedElement.first, changedElement.second);
            bool isUnset = value.IsString == true ? value.Text.empty() : value.Number == 0;
            if (isUnset == true)
                trans->Append("DELETE FROM `mod_rowow_global_variable` WHERE `name` = '{}' AND `index` = {}", name, changedElement.second);
            else
            {
                std::string text = value.IsString == true ? value.Text : std::string();
                CharacterDatabase.EscapeString(text);
                trans->Append("REPLACE INTO `mod_rowow_global_variable` (`name`, `index`, `value`, `text`) VALUES ('{}', {}, {}, '{}')", name, changedElement.second,
                    value.IsString == true ? 0 : value.Number, text);
            }
            statementCount++;
        }
        globals.ChangedElements.clear();
    }
    if (statementCount > 0)
        CharacterDatabase.CommitTransaction(trans);
}

void ROWOWMod::LoadIdentityScriptVariables(Player* player, ROWOWIdentity& identity)
{
    // The character's variables (char_reg_num and char_reg_str, with the C++ rules' own like jobchange_level) and its account's
    identity.Variables.Clear();
    identity.AccountVariables.Clear();
    if (QueryResult result = CharacterDatabase.Query("SELECT `name`, `index`, `value`, `text` FROM `mod_rowow_identity_variable` WHERE `guid` = {}", player->GetGUID().GetCounter()))
    {
        do
        {
            Field* fields = result->Fetch();
            std::string name = ToLower(fields[0].Get<std::string>());
            identity.Variables.Set(name, fields[1].Get<uint32>(), GetStoredVariableValue(name, fields[2].Get<int64>(), fields[3].Get<std::string>()));
        } while (result->NextRow());
    }
    if (QueryResult result = CharacterDatabase.Query("SELECT `name`, `index`, `value`, `text` FROM `mod_rowow_account_variable` WHERE `account` = {}", player->GetSession()->GetAccountId()))
    {
        do
        {
            Field* fields = result->Fetch();
            std::string name = ToLower(fields[0].Get<std::string>());
            identity.AccountVariables.Set(name, fields[1].Get<uint32>(), GetStoredVariableValue(name, fields[2].Get<int64>(), fields[3].Get<std::string>()));
        } while (result->NextRow());
    }
    identity.Variables.ChangedElements.clear();
    identity.AccountVariables.ChangedElements.clear();
}

void ROWOWMod::SaveIdentityScriptVariables(Player* player, ROWOWIdentity& identity, CharacterDatabaseTransaction trans)
{
    // Written whole when any changed, in the identity's save transaction
    auto saveVariables = [&trans](ROWOWScriptVariables& variables, char const* tableName, char const* ownerColumnName, uint32 owner)
    {
        if (variables.ChangedElements.empty() == true)
            return;
        trans->Append("DELETE FROM `{}` WHERE `{}` = {}", tableName, ownerColumnName, owner);
        for (auto const& elementsByName : variables.ElementsByName)
        {
            std::string name = elementsByName.first;
            CharacterDatabase.EscapeString(name);
            for (auto const& elementByIndex : elementsByName.second)
            {
                std::string text = elementByIndex.second.IsString == true ? elementByIndex.second.Text : std::string();
                CharacterDatabase.EscapeString(text);
                trans->Append("INSERT INTO `{}` (`{}`, `name`, `index`, `value`, `text`) VALUES ({}, '{}', {}, {}, '{}')", tableName, ownerColumnName, owner, name,
                    elementByIndex.first, elementByIndex.second.IsString == true ? 0 : elementByIndex.second.Number, text);
            }
        }
        variables.ChangedElements.clear();
    };
    saveVariables(identity.Variables, "mod_rowow_identity_variable", "guid", player->GetGUID().GetCounter());
    if (player->GetSession() != nullptr)
        saveVariables(identity.AccountVariables, "mod_rowow_account_variable", "account", player->GetSession()->GetAccountId());
}

// The NPC scripts' hooks: the world update, a player's sleeping run and logout, and an NPC creature that comes into the world hidden
class ROWOW_NpcScriptWorldScript : public WorldScript
{
public:
    ROWOW_NpcScriptWorldScript() : WorldScript("ROWOW_NpcScriptWorldScript") {}

    void OnUpdate(uint32 diff) override
    {
        ROWOW->UpdateNpcScripts(diff);
    }
};

class ROWOW_NpcScriptPlayerScript : public PlayerScript
{
public:
    ROWOW_NpcScriptPlayerScript() : PlayerScript("ROWOW_NpcScriptPlayerScript", { PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LOGOUT,
        PLAYERHOOK_ON_CREATURE_INTERACTION_REACH }) {}

    void OnPlayerCreatureInteractionReach(Player* player, Creature const* creature, bool& isInReach) override
    {
        // On an RO map an NPC answers its click, and every packet after it (the gossip's choices, a shop's list, purchases and sales), from
        // RO's click range in cells, not WoW's yards with the height between (npc_checknear).  The RO NPC window's own steps are chat
        // commands, which UpdatePlayerNpcScripts holds to the same range
        if (ROWOW->IsEnabled == false || ROWOW->IsROMap(player->GetMapId()) == false)
            return;
        isInReach = ROWOW->IsInNpcClickRange(player, creature);
    }

    void OnPlayerUpdate(Player* player, uint32 p_time) override
    {
        // A run sleeping (sleep2) wakes, queued events, OnTouch areas and the dialog's range, in the player's own update
        if (ROWOW->IsEnabled == false || ROWOW->IsInROForm(player) == false || player->IsInWorld() == false)
            return;
        ROWOW->UpdatePlayerNpcScripts(player, p_time);
        ROWOW->UpdatePlayerWaitingRooms(player, p_time);
    }

    void OnPlayerLogin(Player* player) override
    {
        // OnPCLoginEvent (in the form)
        if (ROWOW->IsEnabled == false)
            return;
        ROWOW->RunPlayerEvent(player, ROWOW_NPC_EVENT_LOGIN);
    }

    void OnPlayerLogout(Player* player) override
    {
        // OnPCLogoutEvent, with the player still attached; then its run and queue go, and the OnTouch_ NPCs it held are free
        if (ROWOW->IsEnabled == false)
            return;
        ROWOWIdentity* identity = ROWOW->GetIdentity(player);
        identity->NpcRun.reset();
        identity->QueuedNpcEvents.clear();
        ROWOW->RunPlayerEvent(player, ROWOW_NPC_EVENT_LOGOUT);
        identity->NpcRun.reset();
        identity->QueuedNpcEvents.clear();
        ROWOW->ForgetNpcTouches(player);
    }
};

class ROWOW_NpcScriptUnitScript : public UnitScript
{
public:
    ROWOW_NpcScriptUnitScript() : UnitScript("ROWOW_NpcScriptUnitScript") {}

    void OnUnitDeath(Unit* unit, Unit* killer) override
    {
        // A script monster's death runs its event; a player's runs OnPCDieEvent, and its killer's OnPCKillEvent; a monster's killer
        // gets OnNPCKillEvent when the monster had no event of its own (pc_dead, mob_dead).  In the unit's map thread
        if (ROWOW->IsEnabled == false || unit == nullptr)
            return;
        Player* killerPlayer = killer != nullptr ? killer->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
        if (killerPlayer != nullptr && killerPlayer->GetMap() != unit->GetMap())
            killerPlayer = nullptr;
        // The ids the events read (pc_setparam's SP_KILLERRID/SP_KILLEDRID/SP_KILLEDGID): a player's is its account (rAthena's bl id), a
        // monster's killedrid its mob_db Id and killedgid the creature
        auto getRID = [](Unit* ridUnit) -> int64
        {
            if (Player* ridPlayer = ridUnit != nullptr ? ridUnit->ToPlayer() : nullptr)
                return ridPlayer->GetSession() != nullptr ? int64(ridPlayer->GetSession()->GetAccountId()) : 0;
            return ridUnit != nullptr ? int64(ridUnit->GetGUID().GetCounter()) : 0;
        };
        if (unit->IsCreature() == true)
        {
            if (killerPlayer != nullptr && ROWOW->IsInROForm(killerPlayer) == true)
            {
                // mob_dead: the killer's (and its party's) hunting quests, then the kill's ids for the monster's event or OnNPCKillEvent
                ROWOW->QuestKillUpdate(unit->ToCreature(), killerPlayer);
                ROWOWIdentity* killerIdentity = ROWOW->GetIdentity(killerPlayer);
                killerIdentity->KilledGID = int64(unit->GetGUID().GetCounter());
                killerIdentity->KilledRID = int64(ROWOW->GetROMobID(unit->ToCreature()));
                killerIdentity->KillerRID = getRID(killerPlayer);
            }
            if (ROWOW->OnScriptMobDeath(unit->ToCreature(), killer) == false && killerPlayer != nullptr)
                ROWOW->RunPlayerEvent(killerPlayer, ROWOW_NPC_EVENT_KILL_NPC);
            return;
        }
        if (unit->IsPlayer() == false)
            return;
        // pc_dead: the dead player's killerrid, then the killer's killedrid
        if (ROWOW->IsInROForm(unit->ToPlayer()) == true)
            ROWOW->GetIdentity(unit->ToPlayer())->KillerRID = killer != nullptr && killer->IsInMap(unit) == true ? getRID(killer) : 0;
        if (killerPlayer != nullptr && killerPlayer != unit && ROWOW->IsInROForm(killerPlayer) == true)
            ROWOW->GetIdentity(killerPlayer)->KilledRID = getRID(unit);
        ROWOW->RunPlayerEvent(unit->ToPlayer(), ROWOW_NPC_EVENT_DIE);
        if (killerPlayer != nullptr && killerPlayer != unit)
            ROWOW->RunPlayerEvent(killerPlayer, ROWOW_NPC_EVENT_KILL_PC);
    }
};

class ROWOW_NpcScriptAllCreatureScript : public AllCreatureScript
{
public:
    ROWOW_NpcScriptAllCreatureScript() : AllCreatureScript("ROWOW_NpcScriptAllCreatureScript") {}

    void OnCreatureRemoveWorld(Creature* creature) override
    {
        // A script monster gone some other way (despawned) no longer counts
        if (ROWOW->IsEnabled == false || creature == nullptr)
            return;
        std::lock_guard<std::mutex> lock(ROWOW->NpcStateMutex);
        ROWOW->ScriptMobsByGUID.erase(creature->GetGUID());
    }

    void OnCreatureAddWorld(Creature* creature) override
    {
        // An NPC's creature can be talked to (its script starts from the gossip hello), and one that a script disabled or hid stays out of
        // sight when it (re)spawns or its map loads.  Runs in the creature's map thread; the NPC's state is read under its lock
        if (ROWOW->IsEnabled == false || ROWOW->NpcsByCreatureSpawnID.empty() == true)
            return;
        ROWOWNpc const* npc = ROWOW->GetCreatureNpc(creature);
        if (npc == nullptr)
            return;
        if (npc->Script != nullptr && npc->IsShop == false)
            creature->SetNpcFlag(UNIT_NPC_FLAG_GOSSIP);
        std::lock_guard<std::mutex> lock(ROWOW->NpcStateMutex);
        if (npc->IsEnabled == false || npc->IsHidden == true)
            creature->SetVisible(false);
    }
};

void AddROWOWNpcScripts()
{
    new ROWOW_NpcScriptWorldScript();
    new ROWOW_NpcScriptPlayerScript();
    new ROWOW_NpcScriptUnitScript();
    new ROWOW_NpcScriptAllCreatureScript();
}
