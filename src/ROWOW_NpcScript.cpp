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
#include "Player.h"
#include "QueryResult.h"
#include "ScriptMgr.h"
#include "Timer.h"
#include "WorldSession.h"

#include "ROWOW.h"
#include "ROWOW_ScriptConstants.h"

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

void ROWOWChatScriptDialog::SendInput(Player* player, ROWOWNpc const* /*npc*/, bool isString)
{
    ChatHandler(player->GetSession()).PSendSysMessage("|cff88ccff[Input {}]|r (.ro npc input <value>)", isString == true ? "text" : "number");
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
        "their NPCs ({} creature rows match no NPC) in {} ms", scriptFiles.size(), NpcsByID.size(), scriptNpcCount, floatingNpcCount, duplicateCount, shopCount,
        functionCount, otherDefinitionCount, warpDuplicateCount, duplicateNameCount, parseErrors.size(), mappedCreatureCount, unmatchedCreatureCount,
        GetMSTimeDiffToNow(oldMSTime));

    // The commands the scripts use, how often, in how many files, and whether the engine has them (for RONotes\script-commands.md)
    std::string commandReport;
    for (auto const& commandUse : commandUses)
    {
        int32 commandIndex = ROWOWSCRIPT->GetCommandIndex(commandUse.first);
        ROWOWScriptCommand const* command = ROWOWSCRIPT->GetCommand(commandIndex);
        static std::set<std::string> const languageWords = { "if", "switch", "for", "while", "do", "set", "end", "return", "goto", "callsub", "callfunc", "function",
            "break", "continue", "getd" };
        char const* status = command == nullptr ? (languageWords.count(commandUse.first) != 0 ? "language" : "missing")
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
    LOG_INFO("module.ROWOW", "ROWOWMod::LoadNpcScriptData ran {} OnInit labels, {} of them stopped early (a command not in yet, or an error)", initCount, initStoppedCount);
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

bool ROWOWMod::StartNpcRun(Player* player, ROWOWNpc const* npc, std::string const& label)
{
    // npc_click / npc_event: one run per player, so a new one ends the one before
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
    context.Dialog = &ChatScriptDialog;
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
        context->Dialog = &ChatScriptDialog;
    ROWOWSCRIPT->Run(*npc->Script, *context);
    if (context->IsStopped == true || context->IsWaiting() == false)
        return;
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
    // npc_enable: the NPC's creatures show or go at the next world update (they can be on another map's thread)
    if (npc == nullptr)
        return;
    std::lock_guard<std::mutex> lock(NpcStateMutex);
    ROWOWNpc* changedNpc = NpcsByID[npc->ID - 1].get();
    changedNpc->IsEnabled = isEnabled;
    NpcVisibilityChangedIDs.insert(npc->ID);

    // A disabled NPC's timer stops (npc_enable)
    if (isEnabled == false && changedNpc->Timer.IsRunning == true)
    {
        changedNpc->Timer.StoppedTick = int64(GameTime::GetGameTimeMS().count() - changedNpc->Timer.StartTime);
        changedNpc->Timer.IsRunning = false;
        RunningNpcTimerIDs.erase(npc->ID);
    }
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

void ROWOWMod::UpdateNpcScripts(uint32 diff)
{
    // The world update, between the map updates: queued events, timers, sleeping runs, the NPC creatures' shown state, and the globals
    if (IsEnabled == false || NpcsByID.empty() == true)
        return;
    uint64 now = GameTime::GetGameTimeMS().count();

    std::vector<ROWOWNpcEvent> dueEvents;
    std::set<uint32> visibilityChangedIDs;
    std::vector<ROWOWNpcAnnouncement> announcements;
    std::vector<ROWOWScriptMobKill> kills;
    std::vector<std::pair<uint32, ObjectGuid>> killedMobs;
    {
        std::lock_guard<std::mutex> lock(NpcStateMutex);
        dueEvents.swap(PendingNpcEvents);
        visibilityChangedIDs.swap(NpcVisibilityChangedIDs);
        announcements.swap(PendingNpcAnnouncements);
        kills.swap(PendingScriptMobKills);
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
    for (ROWOWNpcEvent const& npcEvent : dueEvents)
        RunNpcEvent(GetNpc(npcEvent.NpcID), npcEvent.InstructionIndex);

    // Announcements, to everyone or a map's players
    for (ROWOWNpcAnnouncement const& announcement : announcements)
    {
        if (announcement.MapID == 0)
        {
            ChatHandler(nullptr).SendWorldText(announcement.Text);
            continue;
        }
        if (Map* map = sMapMgr->FindBaseNonInstanceMap(announcement.MapID))
            map->DoForAllPlayers([&announcement](Player* player) { ChatHandler(player->GetSession()).SendSysMessage(announcement.Text); });
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

    // The creatures of NPCs scripts showed or hid (the maps aren't updating now)
    for (uint32 npcID : visibilityChangedIDs)
    {
        ROWOWNpc const* npc = GetNpc(npcID);
        if (npc == nullptr || npc->MapID == 0 || npc->CreatureSpawnIDs.empty() == true)
            continue;
        Map* map = sMapMgr->FindBaseNonInstanceMap(npc->MapID);
        if (map == nullptr)
            continue;
        bool isVisible = false;
        {
            std::lock_guard<std::mutex> lock(NpcStateMutex);
            isVisible = npc->IsEnabled == true && npc->IsHidden == false;
        }
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
    ROWOW_NpcScriptPlayerScript() : PlayerScript("ROWOW_NpcScriptPlayerScript", { PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_LOGOUT }) {}

    void OnPlayerUpdate(Player* player, uint32 /*p_time*/) override
    {
        // A run sleeping (sleep2) wakes in the player's own update
        if (ROWOW->IsEnabled == false || ROWOW->IsInROForm(player) == false)
            return;
        ROWOWIdentity* identity = ROWOW->GetIdentity(player);
        if (identity->NpcRun == nullptr || identity->NpcRun->Wait != ROWOW_SCRIPT_WAIT_SLEEP || uint64(GameTime::GetGameTimeMS().count()) < identity->NpcRun->SleepUntil)
            return;
        ROWOWSCRIPT->Resume(*identity->NpcRun);
        if (identity->NpcRun != nullptr && identity->NpcRun->IsStopped == true)
            ROWOW->EndNpcRun(player);
    }

    void OnPlayerLogout(Player* player) override
    {
        if (ROWOW->IsEnabled == false)
            return;
        ROWOW->GetIdentity(player)->NpcRun.reset();
    }
};

class ROWOW_NpcScriptUnitScript : public UnitScript
{
public:
    ROWOW_NpcScriptUnitScript() : UnitScript("ROWOW_NpcScriptUnitScript") {}

    void OnUnitDeath(Unit* unit, Unit* killer) override
    {
        // A script monster's death runs its event (in its map's thread)
        if (ROWOW->IsEnabled == false || unit == nullptr || unit->IsCreature() == false)
            return;
        ROWOW->OnScriptMobDeath(unit->ToCreature(), killer);
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
        // An NPC that a script disabled or hid stays out of sight when its creature (re)spawns or its map loads.  Runs in the creature's map
        // thread; the NPC's state is read under its lock
        if (ROWOW->IsEnabled == false || ROWOW->NpcsByCreatureSpawnID.empty() == true)
            return;
        ROWOWNpc const* npc = ROWOW->GetCreatureNpc(creature);
        if (npc == nullptr)
            return;
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
