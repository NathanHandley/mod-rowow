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
//  The commands follow rAthena's buildin functions (src/map/script.cpp):
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
//  For more information, see LICENCE in the main folder

#include "Chat.h"
#include "GameTime.h"
#include "Group.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "TemporarySummon.h"
#include "Timer.h"

#include "ROWOW.h"

#include <ctime>

using namespace std;

// The NPC scripts' commands (R7.2): the dialog and flow, the variable and array commands, the NPC controls (enable, events, timers), and
// the cheap player ones, by rAthena's buildin names.  Registered after the item, bonus and status scripts are compiled, so those keep
// what they had.  A command used by the scripts but not here stops an NPC run when reached (logged once); see RONotes\script-commands.md.
// Each runs in the thread of its run: the player's for a player's run (its map or session), the world update for one with none

namespace
{
    std::string ToLower(std::string text)
    {
        for (char& curChar : text)
            curChar = char(tolower((unsigned char)curChar));
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

    ROWOWScriptValue Fail(ROWOWScriptContext& context, std::string const& error)
    {
        context.Error = error;
        context.IsStopped = true;
        return ROWOWScriptValue::FromNumber(0);
    }

    // script_rid2sd: the attached player, or the run stops (as rAthena's "player not attached")
    Player* GetAttachedPlayer(ROWOWScriptContext& context, char const* commandName)
    {
        if (context.AttachedPlayer == nullptr)
            Fail(context, std::string("no player attached for ") + commandName);
        return context.AttachedPlayer;
    }

    // menu_countoptions: the options in a menu string (':' between them), counting down to the one picked when max_count is given
    int32 MenuCountOptions(std::string const& text, int32 maxCount, int32* total)
    {
        int32 count = 0;
        int32 bogusTotal = 0;
        if (total == nullptr)
            total = &bogusTotal;
        ++(*total);
        size_t position = 0;
        while (position < text.size() && text[position] == ':')
        {
            ++position;
            ++(*total);
        }
        while (position < text.size())
        {
            ++count;
            --maxCount;
            if (maxCount == 0)
                break;
            while (position < text.size() && text[position] != ':')
                ++position;
            while (position < text.size() && text[position] == ':')
            {
                ++position;
                ++(*total);
            }
        }
        return count;
    }

    // The options of a menu's strings joined as rAthena sends them, split for the dialog
    std::vector<std::string> GetMenuOptions(std::string const& joined)
    {
        std::vector<std::string> options;
        size_t start = 0;
        while (true)
        {
            size_t end = joined.find(':', start);
            options.push_back(joined.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (end == std::string::npos)
                return options;
            start = end + 1;
        }
    }
}

void ROWOWMod::RegisterNpcScriptCommands()
{
    ROWOWScriptEngine* engine = ROWOWSCRIPT;
    auto getNpc = [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments, size_t index) -> ROWOWNpc const*
    {
        // An NPC named by an argument (its exname), or the run's own
        if (index < arguments.size())
            return GetNpcByName(arguments[index].GetText(), context.AttachedPlayer != nullptr ? context.AttachedPlayer->GetMapId() : 0);
        return context.Npc;
    };

    // The dialog (clif_scriptmes and the others go to the run's dialog: the RO NPC window, or the chat for testing)
    engine->RegisterCommand("mes", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // mes "<line>"{,"<line>"...}
        Player* player = GetAttachedPlayer(context, "mes");
        if (player == nullptr)
            return ROWOWScriptValue::FromNumber(0);
        context.IsMesActive = true;
        if (context.Dialog != nullptr)
            for (ROWOWScriptValue const& argument : arguments)
                context.Dialog->SendMes(player, context.Npc, argument.GetText());
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("next", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const&)
    {
        Player* player = GetAttachedPlayer(context, "next");
        if (player == nullptr)
            return ROWOWScriptValue::FromNumber(0);
        if (context.Dialog != nullptr)
            context.Dialog->SendNext(player, context.Npc);
        context.Wait = ROWOW_SCRIPT_WAIT_NEXT;
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("close", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const&)
    {
        // The Close button, and the run ends when it's pressed (with no dialog open it just ends)
        Player* player = GetAttachedPlayer(context, "close");
        if (player == nullptr)
            return ROWOWScriptValue::FromNumber(0);
        if (context.IsMesActive == false || context.Dialog == nullptr)
        {
            context.IsStopped = true;
            return ROWOWScriptValue::FromNumber(0);
        }
        context.Dialog->SendClose(player, context.Npc);
        context.Wait = ROWOW_SCRIPT_WAIT_CLOSE_END;
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("close2", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const&)
    {
        // The Close button, and the run goes on once it's pressed
        Player* player = GetAttachedPlayer(context, "close2");
        if (player == nullptr)
            return ROWOWScriptValue::FromNumber(0);
        if (context.Dialog != nullptr)
            context.Dialog->SendClose(player, context.Npc);
        context.Wait = ROWOW_SCRIPT_WAIT_CLOSE;
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("close3", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const&)
    {
        // The dialog closes without a button, and the run ends
        context.IsStopped = true;
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("clear", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const&)
    {
        Player* player = GetAttachedPlayer(context, "clear");
        if (player != nullptr && context.Dialog != nullptr)
            context.Dialog->SendClear(player, context.Npc);
        return ROWOWScriptValue::FromNumber(0);
    });
    for (char const* menuCommandName : { "menu", "select", "prompt" })
    {
        // buildin_menu, buildin_select and buildin_prompt: the options go out and the run waits; with the answer (npc_menu) the command runs
        // again.  @menu is the option picked; menu returns which of its strings it was in (the compiled jump table takes its label), select
        // and prompt the option.  Cancel (255) ends the run, but prompt's, which returns 255
        std::string commandName = menuCommandName;
        engine->RegisterCommand(commandName, [commandName](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
        {
            Player* player = GetAttachedPlayer(context, commandName.c_str());
            if (player == nullptr)
                return ROWOWScriptValue::FromNumber(0);
            if (context.IsMenuOrInput == false)
            {
                std::string joined;
                int32 optionCount = 0;
                for (ROWOWScriptValue const& argument : arguments)
                {
                    std::string text = argument.GetText();
                    if (commandName == "menu" && text.empty() == true)
                        continue;
                    if (optionCount > 0)
                        joined += ":";
                    joined += text;
                    optionCount += MenuCountOptions(text, 0, nullptr);
                }
                context.MenuOptions = GetMenuOptions(joined);
                if (context.Dialog != nullptr)
                    context.Dialog->SendMenu(player, context.Npc, context.MenuOptions);
                context.IsMenuOrInput = true;
                context.Wait = ROWOW_SCRIPT_WAIT_MENU;
                context.IsRerunLine = true;
                return ROWOWScriptValue::FromNumber(0);
            }
            context.IsMenuOrInput = false;
            if (context.NpcMenu == 0xff)
            {
                if (commandName == "prompt")
                {
                    ROWOWSCRIPT->SetVariable(context, "@menu", 0, ROWOWScriptValue::FromNumber(0xff));
                    return ROWOWScriptValue::FromNumber(0xff);
                }
                context.IsStopped = true;
                return ROWOWScriptValue::FromNumber(0);
            }
            if (context.NpcMenu <= 0)
                return Fail(context, "an unexpected menu choice");
            int32 menu = 0;
            int32 remaining = context.NpcMenu;
            size_t argumentIndex = 0;
            for (; argumentIndex < arguments.size(); argumentIndex++)
            {
                remaining -= MenuCountOptions(arguments[argumentIndex].GetText(), remaining, &menu);
                if (remaining <= 0)
                    break;
            }
            if (remaining > 0)
                return Fail(context, "a menu choice out of range");
            ROWOWSCRIPT->SetVariable(context, "@menu", 0, ROWOWScriptValue::FromNumber(menu));
            return ROWOWScriptValue::FromNumber(commandName == "menu" ? int64(argumentIndex + 1) : int64(menu));
        });
    }
    engine->RegisterCommand("input", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // input <variable>{,<min>{,<max>}}: the number or text goes in the variable (a number kept between min and max); it returns 1 above
        // max, -1 below min (a text's length), 0 otherwise
        Player* player = GetAttachedPlayer(context, "input");
        if (player == nullptr || arguments.empty() == true || arguments[0].IsReference == false)
            return ROWOWScriptValue::FromNumber(0);
        bool isString = arguments[0].Text.empty() == false && arguments[0].Text.back() == '$';
        int64 minimum = GetArgument(arguments, 1, 0);
        int64 maximum = GetArgument(arguments, 2, INT32_MAX);
        if (context.IsMenuOrInput == false)
        {
            context.IsMenuOrInput = true;
            if (context.Dialog != nullptr)
                context.Dialog->SendInput(player, context.Npc, isString);
            context.Wait = ROWOW_SCRIPT_WAIT_INPUT;
            context.IsRerunLine = true;
            return ROWOWScriptValue::FromNumber(0);
        }
        context.IsMenuOrInput = false;
        if (isString == true)
        {
            int64 length = int64(context.NpcString.size());
            ROWOWSCRIPT->SetReferenceValue(context, arguments[0], ROWOWScriptValue::FromText(context.NpcString));
            return ROWOWScriptValue::FromNumber(length > maximum ? 1 : (length < minimum ? -1 : 0));
        }
        int64 amount = context.NpcAmount;
        ROWOWSCRIPT->SetReferenceValue(context, arguments[0], ROWOWScriptValue::FromNumber(std::clamp(amount, minimum, maximum)));
        return ROWOWScriptValue::FromNumber(amount > maximum ? 1 : (amount < minimum ? -1 : 0));
    });
    engine->SetReferenceArguments("input", 1);
    engine->RegisterCommand("cutin", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // cutin "<image>",<position>: the picture beside the dialog (position 255 takes it away)
        if (context.AttachedPlayer != nullptr && context.Dialog != nullptr)
            context.Dialog->SendCutin(context.AttachedPlayer, GetTextArgument(arguments, 0), int32(GetArgument(arguments, 1, 255)));
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("sleep", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // sleep <ms> and sleep2 <ms>: the run waits that long (sleep would detach the player, whose run it stays here)
        context.SleepUntil = uint64(GameTime::GetGameTimeMS().count()) + uint64(std::max<int64>(0, GetArgument(arguments, 0, 0)));
        context.Wait = ROWOW_SCRIPT_WAIT_SLEEP;
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("sleep2", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        context.SleepUntil = uint64(GameTime::GetGameTimeMS().count()) + uint64(std::max<int64>(0, GetArgument(arguments, 0, 0)));
        context.Wait = ROWOW_SCRIPT_WAIT_SLEEP;
        return ROWOWScriptValue::FromNumber(0);
    });

    // A function's arguments (callfunc, callsub and the script's own functions)
    engine->RegisterCommand("getarg", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // getarg(<index>{,<default>}): without the argument, the default, or an error
        int64 index = GetArgument(arguments, 0, 0);
        ROWOWScriptFrame const& frame = context.Frames.back();
        if (index >= 0 && size_t(index) < frame.Arguments.size())
            return frame.Arguments[size_t(index)];
        if (arguments.size() > 1)
            return arguments[1];
        return Fail(context, "getarg " + std::to_string(index) + " of a call with " + std::to_string(frame.Arguments.size()) + " arguments");
    });
    engine->RegisterCommand("getargcount", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const&)
    {
        return ROWOWScriptValue::FromNumber(int64(context.Frames.back().Arguments.size()));
    });

    // Variables and arrays (the variable arguments come as references)
    engine->RegisterCommand("setarray", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // setarray <array>[<first>],<value>{,<value>...}
        if (arguments.empty() == true || arguments[0].IsReference == false)
            return Fail(context, "setarray needs a variable");
        for (size_t valueIndex = 1; valueIndex < arguments.size(); valueIndex++)
            ROWOWSCRIPT->SetVariable(context, arguments[0].Text, uint32(arguments[0].Number + int64(valueIndex) - 1), arguments[valueIndex]);
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->SetReferenceArguments("setarray", 1);
    engine->RegisterCommand("cleararray", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // cleararray <array>[<first>],<value>,<count>
        if (arguments.empty() == true || arguments[0].IsReference == false)
            return Fail(context, "cleararray needs a variable");
        ROWOWScriptValue value = arguments.size() > 1 ? arguments[1] : ROWOWScriptValue::FromNumber(0);
        for (int64 offset = 0; offset < GetArgument(arguments, 2, 0); offset++)
            ROWOWSCRIPT->SetVariable(context, arguments[0].Text, uint32(arguments[0].Number + offset), value);
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->SetReferenceArguments("cleararray", 1);
    engine->RegisterCommand("copyarray", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // copyarray <destination>[<first>],<source>[<first>],<count>: read whole first, so the two can overlap
        if (arguments.size() < 3 || arguments[0].IsReference == false || arguments[1].IsReference == false)
            return Fail(context, "copyarray needs two variables");
        std::vector<ROWOWScriptValue> values;
        for (int64 offset = 0; offset < GetArgument(arguments, 2, 0); offset++)
            values.push_back(ROWOWSCRIPT->GetVariable(context, arguments[1].Text, uint32(arguments[1].Number + offset)));
        for (size_t offset = 0; offset < values.size(); offset++)
            ROWOWSCRIPT->SetVariable(context, arguments[0].Text, uint32(arguments[0].Number + int64(offset)), values[offset]);
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->SetReferenceArguments("copyarray", 3);
    engine->RegisterCommand("getarraysize", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        if (arguments.empty() == true || arguments[0].IsReference == false)
            return Fail(context, "getarraysize needs a variable");
        return ROWOWScriptValue::FromNumber(int64(ROWOWSCRIPT->GetArraySize(context, arguments[0].Text)));
    });
    engine->SetReferenceArguments("getarraysize", 1);
    engine->RegisterCommand("deletearray", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // deletearray <array>[<first>]{,<count>}: those elements go and the ones after move down (no count: all from the first)
        if (arguments.empty() == true || arguments[0].IsReference == false)
            return Fail(context, "deletearray needs a variable");
        std::string const& name = arguments[0].Text;
        int64 first = arguments[0].Number;
        int64 size = int64(ROWOWSCRIPT->GetArraySize(context, name));
        int64 count = std::min(GetArgument(arguments, 1, size - first), size - first);
        if (count <= 0)
            return ROWOWScriptValue::FromNumber(0);
        for (int64 index = first; index < size; index++)
            ROWOWSCRIPT->SetVariable(context, name, uint32(index), index + count < size ? ROWOWSCRIPT->GetVariable(context, name, uint32(index + count)) : ROWOWScriptValue::FromNumber(0));
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->SetReferenceArguments("deletearray", 1);
    engine->RegisterCommand("getelementofarray", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        if (arguments.empty() == true || arguments[0].IsReference == false)
            return Fail(context, "getelementofarray needs a variable");
        return ROWOWSCRIPT->GetVariable(context, arguments[0].Text, uint32(arguments[0].Number + GetArgument(arguments, 1, 0)));
    });
    engine->SetReferenceArguments("getelementofarray", 1);
    engine->RegisterCommand("inarray", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // inarray <array>,<value>: the first index holding it, or -1
        if (arguments.size() < 2 || arguments[0].IsReference == false)
            return Fail(context, "inarray needs a variable");
        uint32 size = ROWOWSCRIPT->GetArraySize(context, arguments[0].Text);
        for (uint32 index = 0; index < size; index++)
        {
            ROWOWScriptValue element = ROWOWSCRIPT->GetVariable(context, arguments[0].Text, index);
            if (element.IsString == true ? element.Text == arguments[1].GetText() : element.Number == arguments[1].GetNumber())
                return ROWOWScriptValue::FromNumber(index);
        }
        return ROWOWScriptValue::FromNumber(-1);
    });
    engine->SetReferenceArguments("inarray", 1);
    auto splitDynamicName = [](std::string const& fullName, std::string& nameOut) -> uint32
    {
        size_t bracketPosition = fullName.find('[');
        nameOut = bracketPosition == std::string::npos ? fullName : fullName.substr(0, bracketPosition);
        return bracketPosition == std::string::npos ? 0 : uint32(strtoul(fullName.c_str() + bracketPosition + 1, nullptr, 10));
    };
    engine->RegisterCommand("getd", [splitDynamicName](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        std::string name;
        uint32 index = splitDynamicName(GetTextArgument(arguments, 0), name);
        return ROWOWSCRIPT->GetVariable(context, name, index);
    });
    engine->RegisterCommand("getdref", [splitDynamicName](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        // getd("<name>") passed as a command's variable (setarray getd(...),...): the compiler's hidden command for its reference
        std::string name;
        uint32 index = splitDynamicName(GetTextArgument(arguments, 0), name);
        return ROWOWScriptValue::FromReference(name, index);
    });
    engine->RegisterCommand("setd", [splitDynamicName](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        std::string name;
        uint32 index = splitDynamicName(GetTextArgument(arguments, 0), name);
        ROWOWSCRIPT->SetVariable(context, name, index, arguments.size() > 1 ? arguments[1] : ROWOWScriptValue::FromNumber(0));
        return ROWOWScriptValue::FromNumber(0);
    });
    auto getOtherNpcVariables = [getNpc](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments) -> ROWOWScriptVariables*
    {
        // Another NPC's .name variables (getvariableofnpc): only . variables are an NPC's
        ROWOWNpc const* npc = getNpc(context, arguments, 1);
        if (arguments.empty() == true || arguments[0].IsReference == false || arguments[0].Text.empty() == true || arguments[0].Text[0] != '.'
            || (arguments[0].Text.size() > 1 && arguments[0].Text[1] == '@') || npc == nullptr || npc->Script == nullptr)
            return nullptr;
        return &npc->Script->LocalVariables;
    };
    engine->RegisterCommand("getvariableofnpc", [getOtherNpcVariables](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        ROWOWScriptVariables* variables = getOtherNpcVariables(context, arguments);
        if (variables == nullptr)
            return Fail(context, "getvariableofnpc of an unknown NPC or a variable that isn't an NPC's");
        std::lock_guard<std::mutex> lock(ROWOWSCRIPT->VariableMutex);
        return variables->Get(ToLower(arguments[0].Text), uint32(std::max<int64>(0, arguments[0].Number)));
    });
    engine->SetReferenceArguments("getvariableofnpc", 1);
    engine->RegisterCommand("setvariableofnpc", [getOtherNpcVariables](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // set getvariableofnpc(<variable>,"<NPC>"),<value> (the compiler's hidden command)
        ROWOWScriptVariables* variables = getOtherNpcVariables(context, arguments);
        if (variables == nullptr || arguments.size() < 3)
            return Fail(context, "set getvariableofnpc of an unknown NPC or a variable that isn't an NPC's");
        std::lock_guard<std::mutex> lock(ROWOWSCRIPT->VariableMutex);
        variables->Set(ToLower(arguments[0].Text), uint32(std::max<int64>(0, arguments[0].Number)), arguments[2]);
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->SetReferenceArguments("setvariableofnpc", 1);

    // Strings
    engine->RegisterCommand("strcharinfo", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // 0 the name, 1 the party's, 2 the guild's, 3 the RO map
        Player* player = GetAttachedPlayer(context, "strcharinfo");
        if (player == nullptr)
            return ROWOWScriptValue::FromText(std::string());
        switch (GetArgument(arguments, 0, 0))
        {
            case 0: return ROWOWScriptValue::FromText(player->GetName());
            case 1: return ROWOWScriptValue::FromText(std::string()); // Parties have no names in WoW
            case 2:
            {
                Guild* guild = player->GetGuildId() != 0 ? sGuildMgr->GetGuildById(player->GetGuildId()) : nullptr;
                return ROWOWScriptValue::FromText(guild != nullptr ? guild->GetName() : std::string());
            }
            case 3:
            {
                auto mapNameIter = MapNamesByMapID.find(player->GetMapId());
                return ROWOWScriptValue::FromText(mapNameIter != MapNamesByMapID.end() ? mapNameIter->second : std::string());
            }
            default: return ROWOWScriptValue::FromText(std::string());
        }
    });
    engine->RegisterCommand("strnpcinfo", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // 0 the whole name, 1 the shown part, 2 the hidden part (after '#'), 3 the unique name, 4 the map
        ROWOWNpc const* npc = context.Npc;
        if (npc == nullptr)
            return ROWOWScriptValue::FromText(std::string());
        switch (GetArgument(arguments, 0, 0))
        {
            case 0: return ROWOWScriptValue::FromText(npc->FullName);
            case 1: return ROWOWScriptValue::FromText(npc->Name);
            case 2:
            {
                size_t hiddenPosition = npc->FullName.find('#');
                return ROWOWScriptValue::FromText(hiddenPosition != std::string::npos ? npc->FullName.substr(hiddenPosition + 1) : std::string());
            }
            case 3: return ROWOWScriptValue::FromText(npc->UniqueName);
            case 4: return ROWOWScriptValue::FromText(npc->MapName);
            default: return ROWOWScriptValue::FromText(std::string());
        }
    });
    engine->RegisterCommand("getstrlen", [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        return ROWOWScriptValue::FromNumber(int64(GetTextArgument(arguments, 0).size()));
    });
    engine->RegisterCommand("charat", [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        std::string text = GetTextArgument(arguments, 0);
        int64 index = GetArgument(arguments, 1, 0);
        return ROWOWScriptValue::FromText(index >= 0 && size_t(index) < text.size() ? text.substr(size_t(index), 1) : std::string());
    });
    engine->RegisterCommand("substr", [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        // substr("<text>",<start>,<end>): end included
        std::string text = GetTextArgument(arguments, 0);
        int64 start = std::max<int64>(0, GetArgument(arguments, 1, 0));
        int64 end = std::min<int64>(int64(text.size()) - 1, GetArgument(arguments, 2, int64(text.size()) - 1));
        return ROWOWScriptValue::FromText(start <= end ? text.substr(size_t(start), size_t(end - start + 1)) : std::string());
    });
    engine->RegisterCommand("compare", [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        // compare("<text>","<part>"): whether the text has the part, case not counting
        return ROWOWScriptValue::FromNumber(ToLower(GetTextArgument(arguments, 0)).find(ToLower(GetTextArgument(arguments, 1))) != std::string::npos ? 1 : 0);
    });
    engine->RegisterCommand("atoi", [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        return ROWOWScriptValue::FromNumber(int64(strtoll(GetTextArgument(arguments, 0).c_str(), nullptr, 10)));
    });
    engine->RegisterCommand("getitemname", [this](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        // The item's name, or "null"
        if (arguments.empty() == true)
            return ROWOWScriptValue::FromText("null");
        uint32 itemID = arguments[0].IsString == true ? 0 : uint32(arguments[0].Number);
        if (arguments[0].IsString == true)
        {
            auto itemIDIter = ItemIDsByAegisName.find(ToLower(arguments[0].Text));
            itemID = itemIDIter != ItemIDsByAegisName.end() ? itemIDIter->second : 0;
        }
        auto itemTemplateIDIter = ItemTemplateIDsByItemID.find(itemID);
        ItemTemplate const* itemTemplate = itemTemplateIDIter != ItemTemplateIDsByItemID.end() ? sObjectMgr->GetItemTemplate(itemTemplateIDIter->second) : nullptr;
        return ROWOWScriptValue::FromText(itemTemplate != nullptr ? itemTemplate->Name1 : std::string("null"));
    });

    // The player
    engine->SetParameterSetter("Zeny", [](ROWOWScriptContext& context, ROWOWScriptValue const& value)
    {
        // In the form the character's money is the zeny
        if (Player* player = GetAttachedPlayer(context, "set Zeny"))
            player->SetMoney(uint32(std::clamp<int64>(value.GetNumber(), 0, INT32_MAX)));
    });
    engine->SetParameterSetter("StatusPoint", [this](ROWOWScriptContext& context, ROWOWScriptValue const& value)
    {
        if (Player* player = GetAttachedPlayer(context, "set StatusPoint"))
        {
            context.Identity->StatusPoints = uint32(std::max<int64>(0, value.GetNumber()));
            SendROStatusToClient(player);
        }
    });
    engine->SetParameterSetter("SkillPoint", [this](ROWOWScriptContext& context, ROWOWScriptValue const& value)
    {
        if (Player* player = GetAttachedPlayer(context, "set SkillPoint"))
        {
            context.Identity->SkillPoints = uint32(std::max<int64>(0, value.GetNumber()));
            SendSkillsToClient(player, *context.Identity, true);
        }
    });
    engine->SetParameterSetter("Hp", [](ROWOWScriptContext& context, ROWOWScriptValue const& value)
    {
        if (Player* player = GetAttachedPlayer(context, "set Hp"))
            player->SetHealth(uint32(std::clamp<int64>(value.GetNumber(), 1, player->GetMaxHealth())));
    });
    engine->SetParameterSetter("Sp", [](ROWOWScriptContext& context, ROWOWScriptValue const& value)
    {
        if (Player* player = GetAttachedPlayer(context, "set Sp"))
            player->SetPower(POWER_MANA, uint32(std::clamp<int64>(value.GetNumber(), 0, player->GetMaxPower(POWER_MANA))));
    });
    // No weight rules (plan section 8): nothing weighs anything and everything fits (rAthena's base 2000.0 as the most)
    engine->RegisterParameter("Weight", [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const&) { return ROWOWScriptValue::FromNumber(0); });
    engine->RegisterParameter("MaxWeight", [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const&) { return ROWOWScriptValue::FromNumber(20000); });
    engine->RegisterCommand("checkweight", [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const&) { return ROWOWScriptValue::FromNumber(1); });
    engine->RegisterCommand("checkweight2", [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const&) { return ROWOWScriptValue::FromNumber(1); });
    engine->RegisterCommand("getskilllv", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // getskilllv(<skill id or "name">)
        if (context.AttachedPlayer == nullptr || arguments.empty() == true)
            return ROWOWScriptValue::FromNumber(0);
        uint16 skillID = uint16(arguments[0].GetNumber());
        if (arguments[0].IsString == true)
        {
            std::string upperName = arguments[0].Text;
            for (char& curChar : upperName)
                curChar = char(toupper((unsigned char)curChar));
            auto skillIDIter = SkillIDsByName.find(upperName);
            skillID = skillIDIter != SkillIDsByName.end() ? skillIDIter->second : 0;
        }
        return ROWOWScriptValue::FromNumber(PCCheckSkill(context.AttachedPlayer, skillID));
    });
    engine->RegisterCommand("skill", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // skill <skill id or "name">,<level>{,<flag>}: flag 0 (ADDSKILL_PERMANENT) learns it, as quest NPCs give skills.  The temporary
        // kinds (item-granted skills) aren't in yet
        Player* player = GetAttachedPlayer(context, "skill");
        if (player == nullptr || arguments.empty() == true)
            return ROWOWScriptValue::FromNumber(0);
        uint16 skillID = uint16(arguments[0].GetNumber());
        if (arguments[0].IsString == true)
        {
            std::string upperName = arguments[0].Text;
            for (char& curChar : upperName)
                curChar = char(toupper((unsigned char)curChar));
            auto skillIDIter = SkillIDsByName.find(upperName);
            skillID = skillIDIter != SkillIDsByName.end() ? skillIDIter->second : 0;
        }
        if (GetArgument(arguments, 2, 1) == 0)
            PCSkill(player, skillID, uint16(std::max<int64>(0, GetArgument(arguments, 1, 1))));
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("jobchange", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // jobchange <job>{,<upper>}: pc_jobchange (the job number already says normal, upper or Baby)
        if (Player* player = GetAttachedPlayer(context, "jobchange"))
            PCJobChange(player, uint16(GetArgument(arguments, 0, ROWOW_JOB_NOVICE)));
        return ROWOWScriptValue::FromNumber(0);
    });
    for (char const* saveCommandName : { "savepoint", "save" })
    {
        engine->RegisterCommand(saveCommandName, [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
        {
            // savepoint "<map>",<x>,<y>: the save point the identity respawns and Butterfly Wings to
            Player* player = GetAttachedPlayer(context, "savepoint");
            if (player == nullptr)
                return ROWOWScriptValue::FromNumber(0);
            ROWOWPosition savePoint;
            if (GetCellPosition(GetMapIDByMapName(GetTextArgument(arguments, 0)), int32(GetArgument(arguments, 1, 0)), int32(GetArgument(arguments, 2, 0)), savePoint) == false)
                return Fail(context, "savepoint on an unknown map or cell");
            context.Identity->SavePoint = savePoint;
            return ROWOWScriptValue::FromNumber(0);
        });
    }
    engine->RegisterCommand("getmapxy", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // getmapxy(<map variable>,<x variable>,<y variable>{,<type>}): the attached player's (type 0) map and cell; 0, or -1 when it can't
        if (arguments.size() < 3 || context.AttachedPlayer == nullptr || GetArgument(arguments, 3, 0) != 0)
            return ROWOWScriptValue::FromNumber(-1);
        int32 x = 0;
        int32 y = 0;
        GetUnitCell(context.AttachedPlayer, x, y);
        auto mapNameIter = MapNamesByMapID.find(context.AttachedPlayer->GetMapId());
        ROWOWSCRIPT->SetReferenceValue(context, arguments[0], ROWOWScriptValue::FromText(mapNameIter != MapNamesByMapID.end() ? mapNameIter->second : std::string()));
        ROWOWSCRIPT->SetReferenceValue(context, arguments[1], ROWOWScriptValue::FromNumber(x));
        ROWOWSCRIPT->SetReferenceValue(context, arguments[2], ROWOWScriptValue::FromNumber(y));
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->SetReferenceArguments("getmapxy", 7);
    engine->RegisterCommand("playerattached", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const&)
    {
        return ROWOWScriptValue::FromNumber(context.AttachedPlayer != nullptr ? int64(context.AttachedPlayer->GetSession()->GetAccountId()) : 0);
    });
    engine->RegisterCommand("checkre", [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const&)
    {
        // Whether a renewal feature is on: never, this is pre-renewal
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("getbattleflag", [this](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        auto flagIter = BattleFlagsByName.find(ToLower(GetTextArgument(arguments, 0)));
        return ROWOWScriptValue::FromNumber(flagIter != BattleFlagsByName.end() ? flagIter->second : 0);
    });
    engine->RegisterCommand("gettimetick", [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        // 0 the server's tick in ms, 1 the seconds into the day, 2 the Unix time
        time_t now = time(nullptr);
        switch (GetArgument(arguments, 0, 0))
        {
            case 1:
            {
                tm localTime = Acore::Time::TimeBreakdown(now);
                return ROWOWScriptValue::FromNumber(localTime.tm_hour * 3600 + localTime.tm_min * 60 + localTime.tm_sec);
            }
            case 2: return ROWOWScriptValue::FromNumber(int64(now));
            default: return ROWOWScriptValue::FromNumber(int64(GameTime::GetGameTimeMS().count()));
        }
    });
    engine->RegisterCommand("gettime", [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        // rAthena's DT_*: 1 second, 2 minute, 3 hour, 4 day of the week, 5 day of the month, 6 month, 7 year, 8 day of the year, 9 YYYYMMDD
        tm localTime = Acore::Time::TimeBreakdown(time(nullptr));
        switch (GetArgument(arguments, 0, 0))
        {
            case 1: return ROWOWScriptValue::FromNumber(localTime.tm_sec);
            case 2: return ROWOWScriptValue::FromNumber(localTime.tm_min);
            case 3: return ROWOWScriptValue::FromNumber(localTime.tm_hour);
            case 4: return ROWOWScriptValue::FromNumber(localTime.tm_wday);
            case 5: return ROWOWScriptValue::FromNumber(localTime.tm_mday);
            case 6: return ROWOWScriptValue::FromNumber(localTime.tm_mon + 1);
            case 7: return ROWOWScriptValue::FromNumber(localTime.tm_year + 1900);
            case 8: return ROWOWScriptValue::FromNumber(localTime.tm_yday + 1);
            case 9: return ROWOWScriptValue::FromNumber((localTime.tm_year + 1900) * 10000 + (localTime.tm_mon + 1) * 100 + localTime.tm_mday);
            default: return ROWOWScriptValue::FromNumber(-1);
        }
    });

    // Announcements go out from the world update (to players on any map)
    auto queueAnnouncement = [this](uint32 mapID, std::string const& text)
    {
        ROWOWNpcAnnouncement announcement;
        announcement.MapID = mapID;
        announcement.Text = text;
        std::lock_guard<std::mutex> lock(NpcStateMutex);
        PendingNpcAnnouncements.push_back(announcement);
    };
    engine->RegisterCommand("announce", [this, queueAnnouncement](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // announce "<text>",<flag>{,...}: the flag's target bits (bc_all 0, bc_map 1, bc_area 2, bc_self 3); the color isn't kept
        std::string text = GetTextArgument(arguments, 0);
        int64 target = GetArgument(arguments, 1, 0) & 0x0F;
        if (target == 3)
        {
            if (context.AttachedPlayer != nullptr)
                ChatHandler(context.AttachedPlayer->GetSession()).SendSysMessage(text);
            return ROWOWScriptValue::FromNumber(0);
        }
        uint32 mapID = 0;
        if (target == 1 || target == 2)
            mapID = context.AttachedPlayer != nullptr ? context.AttachedPlayer->GetMapId() : (context.Npc != nullptr ? context.Npc->MapID : 0);
        if (target == 0 || mapID != 0)
            queueAnnouncement(mapID, text);
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("mapannounce", [this, queueAnnouncement](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        // mapannounce "<map>","<text>",<flag>{,...}
        uint32 mapID = GetMapIDByMapName(GetTextArgument(arguments, 0));
        if (mapID != 0)
            queueAnnouncement(mapID, GetTextArgument(arguments, 1));
        return ROWOWScriptValue::FromNumber(0);
    });

    // NPCs: shown or not, events, timers
    engine->RegisterCommand("enablenpc", [this, getNpc](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        SetNpcEnabled(getNpc(context, arguments, 0), true);
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("disablenpc", [this, getNpc](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        SetNpcEnabled(getNpc(context, arguments, 0), false);
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("hideonnpc", [this, getNpc](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        SetNpcHidden(getNpc(context, arguments, 0), true);
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("hideoffnpc", [this, getNpc](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        SetNpcHidden(getNpc(context, arguments, 0), false);
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("donpcevent", [this](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        // donpcevent "<NPC>::<label>": run with no player at the next world update; 1 when there's such an event
        return ROWOWScriptValue::FromNumber(QueueNpcEvent(GetTextArgument(arguments, 0)) == true ? 1 : 0);
    });
    engine->RegisterCommand("initnpctimer", [this, getNpc](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // initnpctimer{ "<NPC>"{,<attach>}}: from 0 (attaching the player isn't in; the labels run with no player)
        NpcTimerStart(getNpc(context, arguments, arguments.empty() == false && arguments[0].IsString == true ? 0 : 1), true);
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("startnpctimer", [this, getNpc](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        NpcTimerStart(getNpc(context, arguments, arguments.empty() == false && arguments[0].IsString == true ? 0 : 1), false);
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("stopnpctimer", [this, getNpc](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        NpcTimerStop(getNpc(context, arguments, arguments.empty() == false && arguments[0].IsString == true ? 0 : 1));
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("setnpctimer", [this, getNpc](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // setnpctimer <tick>{,"<NPC>"}
        NpcTimerSet(getNpc(context, arguments, 1), GetArgument(arguments, 0, 0));
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("getnpctimer", [this, getNpc](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // getnpctimer(<type>{,"<NPC>"})
        return ROWOWScriptValue::FromNumber(NpcTimerGet(getNpc(context, arguments, 1), int32(GetArgument(arguments, 0, 0))));
    });
    engine->RegisterCommand("getnpcid", [getNpc](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // getnpcid(0{,"<NPC>"}): its ID, 0 for none
        ROWOWNpc const* npc = getNpc(context, arguments, 1);
        return ROWOWScriptValue::FromNumber(npc != nullptr ? int64(npc->ID) : 0);
    });

    // Monsters scripts spawn (monster, areamonster), count (mobcount) and take away (killmonster).  The spawn happens on the run's thread
    // when that's the map's (the player's map) or the world update's; a kill goes to the world update
    auto spawnMobs = [this](ROWOWScriptContext& context, std::string const& mapName, int32 x1, int32 y1, int32 x2, int32 y2, uint32 mobID, int64 amount,
        std::string const& event)
    {
        uint32 mapID = GetMapIDByMapName(mapName);
        if (mapID == 0 && context.AttachedPlayer != nullptr && (mapName == "this" || mapName.empty() == true))
            mapID = context.AttachedPlayer->GetMapId();
        Map* map = nullptr;
        if (context.AttachedPlayer != nullptr)
            map = context.AttachedPlayer->GetMapId() == mapID ? context.AttachedPlayer->GetMap() : nullptr;
        else
            map = sMapMgr->FindBaseNonInstanceMap(mapID);
        if (map == nullptr)
            return;
        for (int64 spawnIndex = 0; spawnIndex < std::min<int64>(amount, 1000); spawnIndex++)
        {
            int32 x = x1 == x2 ? x1 : int32(urand(uint32(std::min(x1, x2)), uint32(std::max(x1, x2))));
            int32 y = y1 == y2 ? y1 : int32(urand(uint32(std::min(y1, y2)), uint32(std::max(y1, y2))));
            SpawnScriptMob(map, mobID, x, y, event);
        }
    };
    engine->RegisterCommand("monster", [spawnMobs](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // monster "<map>",<x>,<y>,"<name>",<mob id>,<amount>{,"<event>"{,<size>{,<ai>}}}
        int32 x = int32(GetArgument(arguments, 1, 0));
        int32 y = int32(GetArgument(arguments, 2, 0));
        spawnMobs(context, GetTextArgument(arguments, 0), x, y, x, y, uint32(GetArgument(arguments, 4, 0)), GetArgument(arguments, 5, 1), GetTextArgument(arguments, 6));
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("areamonster", [spawnMobs](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // areamonster "<map>",<x1>,<y1>,<x2>,<y2>,"<name>",<mob id>,<amount>{,"<event>"{,<size>{,<ai>}}}
        spawnMobs(context, GetTextArgument(arguments, 0), int32(GetArgument(arguments, 1, 0)), int32(GetArgument(arguments, 2, 0)), int32(GetArgument(arguments, 3, 0)),
            int32(GetArgument(arguments, 4, 0)), uint32(GetArgument(arguments, 6, 0)), GetArgument(arguments, 7, 1), GetTextArgument(arguments, 8));
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("mobcount", [this](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        // mobcount("<map>","<event>"): the living script monsters of the map with that event ("all" for any)
        uint32 mapID = GetMapIDByMapName(GetTextArgument(arguments, 0));
        std::string event = GetTextArgument(arguments, 1);
        bool isAll = ToLower(event) == "all";
        int64 count = 0;
        std::lock_guard<std::mutex> lock(NpcStateMutex);
        for (auto const& scriptMobByGUID : ScriptMobsByGUID)
            if (scriptMobByGUID.second.MapID == mapID && (isAll == true || scriptMobByGUID.second.Event == event))
                count++;
        return ROWOWScriptValue::FromNumber(count);
    });
    auto queueKill = [this](uint32 mapID, std::string const& event, bool isAll)
    {
        ROWOWScriptMobKill kill;
        kill.MapID = mapID;
        kill.Event = event;
        kill.IsAll = isAll;
        std::lock_guard<std::mutex> lock(NpcStateMutex);
        PendingScriptMobKills.push_back(kill);
    };
    engine->RegisterCommand("killmonster", [this, queueKill](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        // killmonster "<map>","<event>"{,<type>}: "All" for every script monster of the map
        std::string event = GetTextArgument(arguments, 1);
        queueKill(GetMapIDByMapName(GetTextArgument(arguments, 0)), event, ToLower(event) == "all");
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("killmonsterall", [this, queueKill](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        // killmonsterall "<map>": the script monsters of the map (its normal spawns stay)
        queueKill(GetMapIDByMapName(GetTextArgument(arguments, 0)), std::string(), true);
        return ROWOWScriptValue::FromNumber(0);
    });

    // Looks and sounds the client can't show yet: they do nothing, so the dialogs go on past them
    for (char const* lookCommandName : { "emotion", "viewpoint", "soundeffect", "soundeffectall", "misceffect" })
    {
        engine->RegisterCommand(lookCommandName, [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const&) { return ROWOWScriptValue::FromNumber(0); });
        engine->SetStub(lookCommandName);
    }
    engine->SetStub("specialeffect");
    engine->SetStub("specialeffect2");
}

Creature* ROWOWMod::SpawnScriptMob(Map* map, uint32 mobID, int32 x, int32 y, std::string const& event)
{
    // mob_once_spawn: the monster on the cell (a random one of the map for 0,0), kept until killed or taken away
    uint32 entry = GetMobCreatureEntry(mobID);
    if (map == nullptr || entry == 0)
        return nullptr;
    ROWOWPosition position;
    if (GetCellPosition(map->GetId(), x, y, position) == false)
        return nullptr;
    TempSummon* creature = map->SummonCreature(entry, Position(position.X, position.Y, position.Z, float(urand(0, 7)) * float(M_PI) / 4.0f));
    if (creature == nullptr)
        return nullptr;
    ROWOWScriptMob scriptMob;
    scriptMob.MapID = map->GetId();
    scriptMob.Event = event;
    std::lock_guard<std::mutex> lock(NpcStateMutex);
    ScriptMobsByGUID[creature->GetGUID()] = scriptMob;
    return creature;
}

void ROWOWMod::OnScriptMobDeath(Creature* creature, Unit* killer)
{
    // The monster's event (OnMyMobDead and the like) runs now, on its map's thread, with the player who killed it attached (mob_dead's
    // npc_event with the killer's sd)
    std::string event;
    {
        std::lock_guard<std::mutex> lock(NpcStateMutex);
        auto scriptMobIter = ScriptMobsByGUID.find(creature->GetGUID());
        if (scriptMobIter == ScriptMobsByGUID.end())
            return;
        event = scriptMobIter->second.Event;
        ScriptMobsByGUID.erase(scriptMobIter);
    }
    size_t separatorPosition = event.find("::");
    if (separatorPosition == std::string::npos)
        return;
    ROWOWNpc const* npc = GetNpcByName(event.substr(0, separatorPosition));
    if (npc == nullptr || npc->Script == nullptr)
        return;
    int64 labelIndex = npc->Script->GetLabel(event.substr(separatorPosition + 2));
    if (labelIndex < 0)
        return;
    Player* killerPlayer = killer != nullptr ? killer->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
    if (killerPlayer != nullptr && killerPlayer->GetMap() == creature->GetMap())
        RunNpcEvent(npc, uint32(labelIndex), killerPlayer);
    else
        QueueNpcEvent(npc, event.substr(separatorPosition + 2));
}
