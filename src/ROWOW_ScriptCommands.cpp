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
#include "GroupMgr.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "TemporarySummon.h"
#include "Timer.h"

#include "WorldSession.h"
#include "WorldSessionMgr.h"

#include "ROWOW.h"
#include "ROWOW_JobMapIDs.h"

#include <ctime>
#include <mutex>
#include <regex>
#include <set>

using namespace std;

// The NPC scripts' commands (R7.2): the dialog and flow, the variable and array commands, the NPC controls (enable, events, timers), and
// the cheap player ones, by rAthena's buildin names.  Registered after the item, bonus and status scripts are compiled, so those keep
// what they had.  A command used by the scripts but not here stops an NPC run when reached (logged once); see RONotes\script-commands.md.
// Each runs in the thread of its run: the player's for a player's run (its map or session), the world update for one with none

namespace
{
    // e_broadcast_flags: announce's target, and its NPC source
    constexpr int64 ANNOUNCE_TARGET_MASK = 0x07;
    constexpr int64 ANNOUNCE_TARGET_MAP = 1;
    constexpr int64 ANNOUNCE_TARGET_AREA = 2;
    constexpr int64 ANNOUNCE_TARGET_SELF = 3;
    constexpr int64 ANNOUNCE_SOURCE_NPC = 0x08;

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

    // A stub command (one of a system that isn't here, or isn't in yet) says so in the log the first time a run uses it
    std::mutex LoggedStubCommandMutex;
    std::set<std::string> LoggedStubCommands;
    void LogStubCommandOnce(std::string const& commandName, char const* reason)
    {
        std::lock_guard<std::mutex> lock(LoggedStubCommandMutex);
        if (LoggedStubCommands.insert(commandName).second == true)
            LOG_INFO("module.ROWOW", "ROWOWScriptEngine: the NPC scripts' command '{}' does nothing here: {}", commandName, reason);
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
            return GetNpcByName(arguments[index].GetText(), context.AttachedPlayer != nullptr ? GetUnitMapKey(context.AttachedPlayer) : 0); // The player's part's first
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
    // close and close3: the Close button, and the run ends when it's pressed (with no dialog open it just ends).  close3 also takes the
    // cutin away after, which the RO NPC window does at every run's end
    auto closeCommand = [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const&)
    {
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
    };
    engine->RegisterCommand("close", closeCommand);
    engine->RegisterCommand("close3", closeCommand);
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
                context.Dialog->SendInput(player, context.Npc, isString, minimum, maximum);
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
    engine->RegisterCommand("implode", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // implode(<string array>{,"<glue>"}): its elements from 0 to the highest set, joined ("NULL" for an empty one).  The Kafras'
        // menus are built with it
        if (arguments.empty() == true || arguments[0].IsReference == false || arguments[0].Text.empty() == true || arguments[0].Text.back() != '$')
            return Fail(context, "implode needs a string array");
        std::string const& name = arguments[0].Text;
        uint32 size = ROWOWSCRIPT->GetArraySize(context, name);
        if (size == 0)
            return ROWOWScriptValue::FromText("NULL");
        std::string glue = GetTextArgument(arguments, 1);
        std::string joined;
        for (uint32 index = 0; index < size; index++)
        {
            if (index > 0)
                joined += glue;
            joined += ROWOWSCRIPT->GetVariable(context, name, index).GetText();
        }
        return ROWOWScriptValue::FromText(joined);
    });
    engine->SetReferenceArguments("implode", 1);
    engine->RegisterCommand("explode", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // explode(<string array>,"<text>","<delimiter>"): the text split at the delimiter's first character, from the array's index on
        if (arguments.empty() == true || arguments[0].IsReference == false || arguments[0].Text.empty() == true || arguments[0].Text.back() != '$')
            return Fail(context, "explode needs a string array");
        std::string const& name = arguments[0].Text;
        std::string text = GetTextArgument(arguments, 1);
        std::string delimiter = GetTextArgument(arguments, 2);
        int64 index = arguments[0].Number;
        std::string piece;
        for (char curChar : text)
        {
            if (delimiter.empty() == false && curChar == delimiter[0])
            {
                ROWOWSCRIPT->SetVariable(context, name, uint32(index++), ROWOWScriptValue::FromText(piece));
                piece.clear();
            }
            else
                piece += curChar;
        }
        ROWOWSCRIPT->SetVariable(context, name, uint32(index), ROWOWScriptValue::FromText(piece));
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->SetReferenceArguments("explode", 1);
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
        // get_val: a constant's name is the constant (the Mercenary Managers read getd("SPEAR_MERC_GUILD"))
        int64 constant = 0;
        if (ROWOWSCRIPT->GetKnownConstant(name, constant) == true)
            return ROWOWScriptValue::FromNumber(constant);
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
                // The RO map's name; on a stitched map the part's the player is in
                int32 x = 0, y = 0;
                GetUnitCell(player, x, y);
                return ROWOWScriptValue::FromText(GetMapNameAt(player->GetMapId(), x, y));
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
        // skill <skill id or "name">,<level>{,<flag>}: flag 0 (ADDSKILL_PERMANENT) learns it, as quest NPCs give skills, and 3
        // (ADDSKILL_PERMANENT_GRANTED) learns it off the job's tree too.  The temporary kinds last until the next status calculation, as in
        // rAthena; an item's script has its own copy of the command (ROWOW_Bonus.cpp)
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
        int64 flag = GetArgument(arguments, 2, ROWOW_ADDSKILL_TEMP);
        if (flag == ROWOW_ADDSKILL_PERMANENT || flag == ROWOW_ADDSKILL_PERMANENT_GRANTED)
        {
            // The skill's flag: SKILL_FLAG_PERM_GRANTED, or SKILL_FLAG_PERMANENT again (learned as any skill, its points counted and reset)
            SetIdentityVariable(*GetIdentity(player), fmt::format(GrantedSkillVariableFormat, skillID), flag == ROWOW_ADDSKILL_PERMANENT_GRANTED ? 1 : 0);
            PCSkill(player, skillID, uint16(std::max<int64>(0, GetArgument(arguments, 1, 1))), true); // Told in chat when it's new (First Aid, Play Dead)
        }
        else if (flag == ROWOW_ADDSKILL_TEMP || flag == ROWOW_ADDSKILL_TEMP_ADDLEVEL)
        {
            ROWOWIdentity* identity = GetIdentity(player);
            PCAddTempSkill(*identity, skillID, int32(GetArgument(arguments, 1, 1)), flag == ROWOW_ADDSKILL_TEMP_ADDLEVEL);
            PCRefreshItemSkills(player, *identity);
        }
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("jobchange", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // jobchange <job>{,<upper>{,<char id>}}: pc_jobchange.  The job's class, made transcendent (upper 1) or Baby (2) when asked (-1, the
        // default, takes the job as it is), and the job of that class for the player's sex (a Dancer change makes a male a Bard)
        Player* player = GetAttachedPlayer(context, "jobchange");
        int64 jobID = GetArgument(arguments, 0, ROWOW_JOB_NOVICE);
        if (player == nullptr || GetJob(uint16(jobID)) == nullptr)
            return ROWOWScriptValue::FromNumber(0);
        int64 jobClass = PCJobIDToMapID(int32(jobID));
        if (jobClass == -1)
            return ROWOWScriptValue::FromNumber(0);
        switch (GetArgument(arguments, 1, -1))
        {
            case 1: jobClass |= ROWOW_JOBL_UPPER; break;
            case 2: jobClass |= ROWOW_JOBL_BABY; break;
            default: break;
        }
        int32 newJobID = PCMapIDToJobID(uint64(jobClass), player->getGender() == GENDER_MALE);
        if (newJobID >= 0)
            PCJobChange(player, uint16(newJobID));
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
            int32 x = int32(GetArgument(arguments, 1, 0));
            int32 y = int32(GetArgument(arguments, 2, 0));
            uint32 mapID = ResolveMapCell(GetTextArgument(arguments, 0), x, y);
            if (GetCellPosition(mapID, x, y, savePoint) == false)
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
        std::string mapName = GetMapNameAt(context.AttachedPlayer->GetMapId(), x, y); // On a stitched map, the part's name and its own cell
        ROWOWSCRIPT->SetReferenceValue(context, arguments[0], ROWOWScriptValue::FromText(mapName));
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
    auto queueAnnouncement = [this](uint32 mapID, std::string const& text, int32 cellX = -1, int32 cellY = -1)
    {
        ROWOWNpcAnnouncement announcement;
        announcement.MapID = mapID;
        announcement.CellX = cellX;
        announcement.CellY = cellY;
        announcement.Text = text;
        std::lock_guard<std::mutex> lock(NpcStateMutex);
        PendingNpcAnnouncements.push_back(announcement);
    };
    engine->RegisterCommand("announce", [this, queueAnnouncement](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // announce "<text>",<flag>{,...}: a flag of bc_all alone (bc_blue and bc_woe are looks) is a broadcast.  Any other names a source, bc_npc the NPC
        // and otherwise the attached player, and with no source nothing goes out: to everyone, its map (bc_map), around it (bc_area), or itself alone
        // (bc_self, which an NPC doesn't hear).  The color isn't kept
        std::string text = GetTextArgument(arguments, 0);
        int64 flag = GetArgument(arguments, 1, 0);
        int64 target = flag & ANNOUNCE_TARGET_MASK;
        bool isNpcSource = (flag & ANNOUNCE_SOURCE_NPC) != 0;
        if (target == 0 && isNpcSource == false)
        {
            queueAnnouncement(0, text);
            return ROWOWScriptValue::FromNumber(0);
        }
        Player* sourcePlayer = isNpcSource == false ? context.AttachedPlayer : nullptr;
        ROWOWNpc const* sourceNpc = isNpcSource == true ? context.Npc : nullptr;
        if (sourcePlayer == nullptr && sourceNpc == nullptr)
            return ROWOWScriptValue::FromNumber(0);
        uint32 mapID = sourceNpc != nullptr ? sourceNpc->MapID : sourcePlayer->GetMapId();
        int32 x = sourceNpc != nullptr ? sourceNpc->X : 0;
        int32 y = sourceNpc != nullptr ? sourceNpc->Y : 0;
        if (sourcePlayer != nullptr)
            GetUnitCell(sourcePlayer, x, y);
        switch (target)
        {
            case ANNOUNCE_TARGET_MAP:
                if (mapID != 0)
                    queueAnnouncement(GetMapKey(mapID, x, y), text); // A part of a stitched map is a map of its own
                break;
            case ANNOUNCE_TARGET_AREA:
                if (mapID != 0)
                    queueAnnouncement(mapID, text, x, y);
                break;
            case ANNOUNCE_TARGET_SELF:
                if (sourcePlayer != nullptr)
                    ChatHandler(sourcePlayer->GetSession()).SendSysMessage(text);
                break;
            default:
                queueAnnouncement(0, text);
                break;
        }
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("mapannounce", [this, queueAnnouncement](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        // mapannounce "<map>","<text>",<flag>{,...}: a part of a stitched map is its own players only
        uint32 mapKey = GetMapKeyByMapName(GetTextArgument(arguments, 0));
        if (mapKey != 0)
            queueAnnouncement(mapKey, GetTextArgument(arguments, 1));
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
        return ROWOWScriptValue::FromNumber(DoNpcEvent(GetTextArgument(arguments, 0)) == true ? 1 : 0);
    });
    auto startNpcTimer = [this, getNpc](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments, bool isFromZero)
    {
        // initnpctimer and startnpctimer{ "<NPC>"{,<attach>}} or { <attach>}: the attach flag gives the timer the run's player, whose
        // labels then run with it attached (the timer stops if it leaves)
        bool isNamed = arguments.empty() == false && arguments[0].IsString == true;
        ROWOWNpc const* npc = getNpc(context, arguments, isNamed == true ? 0 : 1);
        int64 attachFlag = GetArgument(arguments, isNamed == true ? 1 : 0, 0);
        if (attachFlag != 0 && context.AttachedPlayer != nullptr)
            SetNpcTimerPlayer(npc, context.AttachedPlayer->GetGUID());
        NpcTimerStart(npc, isFromZero);
    };
    engine->RegisterCommand("initnpctimer", [startNpcTimer](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        startNpcTimer(context, arguments, true);
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("startnpctimer", [startNpcTimer](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        startNpcTimer(context, arguments, false);
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("attachnpctimer", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // attachnpctimer{ "<character name>"}: the run's NPC's timer runs its labels with that player (the run's own by default)
        Player* player = arguments.empty() == false ? ObjectAccessor::FindPlayerByName(GetTextArgument(arguments, 0)) : context.AttachedPlayer;
        if (player == nullptr || context.Npc == nullptr)
            return ROWOWScriptValue::FromNumber(0);
        SetNpcTimerPlayer(context.Npc, player->GetGUID());
        return ROWOWScriptValue::FromNumber(1);
    });
    engine->RegisterCommand("detachnpctimer", [this, getNpc](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // detachnpctimer{ "<NPC>"}
        SetNpcTimerPlayer(getNpc(context, arguments, 0), ObjectGuid::Empty);
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
    auto spawnMobs = [this](ROWOWScriptContext& context, std::string const& mapName, int32 x1, int32 y1, int32 x2, int32 y2, std::string const& name,
        uint32 mobID, int64 amount, std::string const& event)
    {
        // The map, and the part of a stitched map its cells are of: by the part's name, or "this" on one (a script's cells are its own map's, the
        // part's).  "this" is the attached player's map
        Player* player = context.AttachedPlayer;
        ROWOWMapPart const* part = GetMapPart(mapName);
        uint32 mapID = part != nullptr ? part->MapID : GetMapIDByMapName(mapName);
        int32 playerX = 0, playerY = 0;
        if (player != nullptr)
            GetUnitCell(player, playerX, playerY);
        if (mapID == 0 && player != nullptr && (mapName == "this" || mapName.empty() == true))
        {
            mapID = player->GetMapId();
            part = GetMapPartAt(mapID, playerX, playerY);
        }
        Map* map = nullptr;
        if (player != nullptr)
            map = player->GetMapId() == mapID ? player->GetMap() : nullptr;
        else
            map = sMapMgr->FindBaseNonInstanceMap(mapID);
        if (map == nullptr && (mapID == 0 || player != nullptr || IsROMap(mapID) == false))
            return;

        // mob_once_spawn_sub's cell: one below 0 is next to the attached player (map_search_freecell(bl, 1, 1), the player's own when none is free),
        // and 0 or one off the map a random free cell of the map (SpawnScriptMob's -1; a part's: of the part).  The others are moved onto the map
        ROWOWMapGrid const* mapGrid = GetMapGrid(mapID);
        bool hasPartArea = part != nullptr && part->HasArea() == true;
        int32 width = hasPartArea == true ? part->CellWidth : (mapGrid != nullptr ? mapGrid->Width : 0);
        int32 height = hasPartArea == true ? part->CellHeight : (mapGrid != nullptr ? mapGrid->Height : 0);
        auto getSpawnCell = [&](int32& x, int32& y)
        {
            x = x1 == x2 ? x1 : (std::min(x1, x2) < 0 ? -1 : int32(urand(uint32(std::min(x1, x2)), uint32(std::max(x1, x2)))));
            y = y1 == y2 ? y1 : (std::min(y1, y2) < 0 ? -1 : int32(urand(uint32(std::min(y1, y2)), uint32(std::max(y1, y2)))));
            if ((x < 0 || y < 0) && map != nullptr && player != nullptr && mapGrid != nullptr)
            {
                if (MapSearchFreeCellOfSpawn(*mapGrid, map, playerX, playerY, 1, 1, 9, x, y, GetMapPartAt(mapID, playerX, playerY)) == false)
                {
                    x = playerX;
                    y = playerY;
                }
                return;
            }
            if (x <= 0 || y <= 0 || x >= width || y >= height)
            {
                x = -1;
                y = -1;
                return;
            }
            if (part != nullptr)
            {
                x += part->CellOffsetX;
                y += part->CellOffsetY;
            }
        };
        if (map == nullptr)
        {
            // An RO map that isn't loaded (an OnInit's monster at startup) gets them once it is (the world update spawns them)
            std::lock_guard<std::mutex> lock(NpcStateMutex);
            for (int64 spawnIndex = 0; spawnIndex < std::min<int64>(amount, 1000); spawnIndex++)
            {
                ROWOWPendingScriptMob mob;
                mob.MapID = mapID;
                getSpawnCell(mob.X, mob.Y);
                mob.Part = part;
                mob.MobID = mobID;
                mob.Name = name;
                mob.Event = event;
                PendingScriptMobs.push_back(mob);
            }
            return;
        }
        for (int64 spawnIndex = 0; spawnIndex < std::min<int64>(amount, 1000); spawnIndex++)
        {
            int32 x = 0, y = 0;
            getSpawnCell(x, y);
            SpawnScriptMob(map, mobID, x, y, name, event, part);
        }
    };
    engine->RegisterCommand("monster", [spawnMobs](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // monster "<map>",<x>,<y>,"<name>",<mob id>,<amount>{,"<event>"{,<size>{,<ai>}}}
        int32 x = int32(GetArgument(arguments, 1, 0));
        int32 y = int32(GetArgument(arguments, 2, 0));
        spawnMobs(context, GetTextArgument(arguments, 0), x, y, x, y, GetTextArgument(arguments, 3), uint32(GetArgument(arguments, 4, 0)), GetArgument(arguments, 5, 1), GetTextArgument(arguments, 6));
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("areamonster", [spawnMobs](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // areamonster "<map>",<x1>,<y1>,<x2>,<y2>,"<name>",<mob id>,<amount>{,"<event>"{,<size>{,<ai>}}}
        spawnMobs(context, GetTextArgument(arguments, 0), int32(GetArgument(arguments, 1, 0)), int32(GetArgument(arguments, 2, 0)), int32(GetArgument(arguments, 3, 0)),
            int32(GetArgument(arguments, 4, 0)), GetTextArgument(arguments, 5), uint32(GetArgument(arguments, 6, 0)), GetArgument(arguments, 7, 1), GetTextArgument(arguments, 8));
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("mobcount", [this](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        // mobcount("<map>","<event>"): the living script monsters of the map with that event ("all" for any); a part of a stitched map its own
        uint32 mapID = GetMapKeyByMapName(GetTextArgument(arguments, 0));
        std::string event = GetTextArgument(arguments, 1);
        bool isAll = ToLower(event) == "all";
        int64 count = 0;
        std::lock_guard<std::mutex> lock(NpcStateMutex);
        for (auto const& scriptMobByGUID : ScriptMobsByGUID)
            if (scriptMobByGUID.second.MapID == mapID && (isAll == true || scriptMobByGUID.second.Event == event))
                count++;
        return ROWOWScriptValue::FromNumber(count);
    });
    engine->RegisterCommand("getmonsterinfo", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // getmonsterinfo(<mob id or "AegisName">,<type>): mob_db's values by rAthena's MOB_* types (MOB_NAME 1 .. MOB_ID 29); "null" or -1
        // for a monster that isn't there
        enum { MOB_NAME = 1, MOB_LV, MOB_MAXHP, MOB_MAXSP, MOB_BASEEXP, MOB_JOBEXP, MOB_ATKMIN, MOB_ATKMAX, MOB_DEF, MOB_MDEF, MOB_RES, MOB_MRES, MOB_STR,
            MOB_AGI, MOB_VIT, MOB_INT, MOB_DEX, MOB_LUK, MOB_SPEED, MOB_ATKRANGE, MOB_SKILLRANGE, MOB_CHASERANGE, MOB_SIZE, MOB_RACE, MOB_ELEMENT,
            MOB_ELEMENTLV, MOB_MODE, MOB_MVPEXP, MOB_ID };
        ROWOWDataTable const* mobTable = GetDataTable(ROWOW_DATA_TABLE_MOB);
        int64 type = GetArgument(arguments, 1, 0);
        int64 mobRowIndex = -1;
        if (mobTable != nullptr && arguments.empty() == false && arguments[0].IsString == true)
        {
            int32 aegisNameColumnIndex = mobTable->GetColumnIndex("AegisName");
            for (size_t rowIndex = 0; rowIndex < mobTable->Rows.size() && mobRowIndex < 0; rowIndex++)
                if (mobTable->GetCell(rowIndex, aegisNameColumnIndex) == arguments[0].Text)
                    mobRowIndex = int64(rowIndex);
        }
        else if (mobTable != nullptr)
            mobRowIndex = GetMobRowIndex(uint32(GetArgument(arguments, 0, 0)));
        uint32 mobID = mobRowIndex >= 0 ? uint32(mobTable->GetInt(size_t(mobRowIndex), mobTable->GetColumnIndex("Id"), 0)) : 0;
        auto statusIter = MobStatusesByMobID.find(mobID);
        if (mobRowIndex < 0 || statusIter == MobStatusesByMobID.end())
            return type == MOB_NAME ? ROWOWScriptValue::FromText("null") : ROWOWScriptValue::FromNumber(-1);
        ROWOWStatus const& status = statusIter->second;
        auto getInt = [mobTable, mobRowIndex](char const* columnName) { return mobTable->GetInt(size_t(mobRowIndex), mobTable->GetColumnIndex(columnName), 0); };
        switch (type)
        {
            case MOB_NAME: return ROWOWScriptValue::FromText(mobTable->GetCell(size_t(mobRowIndex), mobTable->GetColumnIndex("JapaneseName")));
            case MOB_LV: return ROWOWScriptValue::FromNumber(status.Level);
            case MOB_MAXHP: return ROWOWScriptValue::FromNumber(status.MaxHP);
            case MOB_MAXSP: return ROWOWScriptValue::FromNumber(status.MaxSP);
            case MOB_BASEEXP: return ROWOWScriptValue::FromNumber(getInt("BaseExp") * BattleConfig.BaseExpRate / 100);
            case MOB_JOBEXP: return ROWOWScriptValue::FromNumber(getInt("JobExp") * BattleConfig.JobExpRate / 100);
            case MOB_ATKMIN: return ROWOWScriptValue::FromNumber(status.WeaponAttack);
            case MOB_ATKMAX: return ROWOWScriptValue::FromNumber(status.WeaponRefineAttack);
            case MOB_DEF: return ROWOWScriptValue::FromNumber(status.Def);
            case MOB_MDEF: return ROWOWScriptValue::FromNumber(status.MDef);
            case MOB_RES: case MOB_MRES: return ROWOWScriptValue::FromNumber(0); // Renewal's
            case MOB_STR: case MOB_AGI: case MOB_VIT: case MOB_INT: case MOB_DEX: case MOB_LUK:
                return ROWOWScriptValue::FromNumber(status.Stats[type - MOB_STR]);
            case MOB_SPEED: return ROWOWScriptValue::FromNumber(status.Speed);
            case MOB_ATKRANGE: return ROWOWScriptValue::FromNumber(status.Range);
            case MOB_SKILLRANGE: return ROWOWScriptValue::FromNumber(getInt("SkillRange"));
            case MOB_CHASERANGE: return ROWOWScriptValue::FromNumber(getInt("ChaseRange"));
            case MOB_SIZE: return ROWOWScriptValue::FromNumber(status.Size);
            case MOB_RACE: return ROWOWScriptValue::FromNumber(status.Race);
            case MOB_ELEMENT: return ROWOWScriptValue::FromNumber(status.DefElement);
            case MOB_ELEMENTLV: return ROWOWScriptValue::FromNumber(status.ElementLevel);
            case MOB_MODE: return ROWOWScriptValue::FromNumber(status.Mode);
            case MOB_MVPEXP: return ROWOWScriptValue::FromNumber(getInt("MvpExp"));
            case MOB_ID: return ROWOWScriptValue::FromNumber(mobID);
            default: return Fail(context, fmt::format("invalid getmonsterinfo type {}", type));
        }
    });
    auto queueKill =[this](uint32 mapID, std::string const& event, bool isAll)
    {
        // At once in the world update (the maps idle), as rAthena's: a script that kills a stage's monsters and spawns the next stage's
        // right after must not lose the new ones.  From a map's thread the kill waits for the world update (the monsters can be on another map).
        // The map is a map key (a part of a stitched map: its own monsters)
        if (IsInNpcWorldUpdate == true)
        {
            std::vector<ObjectGuid> killedGUIDs;
            {
                std::lock_guard<std::mutex> lock(NpcStateMutex);
                for (auto scriptMobIter = ScriptMobsByGUID.begin(); scriptMobIter != ScriptMobsByGUID.end();)
                {
                    if (scriptMobIter->second.MapID == mapID && (isAll == true || scriptMobIter->second.Event == event))
                    {
                        killedGUIDs.push_back(scriptMobIter->first);
                        scriptMobIter = ScriptMobsByGUID.erase(scriptMobIter);
                    }
                    else
                        ++scriptMobIter;
                }
            }
            Map* map = sMapMgr->FindBaseNonInstanceMap(GetMapIDOfMapKey(mapID));
            for (ObjectGuid const& killedGUID : killedGUIDs)
                if (Creature* creature = map != nullptr ? map->GetCreature(killedGUID) : nullptr)
                    creature->DespawnOrUnsummon();
            return;
        }
        ROWOWScriptMobKill kill;
        kill.MapID = mapID;
        kill.Event = event;
        kill.IsAll = isAll;
        std::lock_guard<std::mutex> lock(NpcStateMutex);
        PendingScriptMobKills.push_back(kill);
    };
    engine->RegisterCommand("makeitem", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // makeitem <item>,<amount>,"<map>",<x>,<y>: an item on the ground around the cell, for anyone (map_addflooritem, flags 4).  A negative ID comes as a drop
        // would, unidentified if it's equipment.  Only the run's own map can be reached: the player's, or any map from the world update
        if (arguments.empty() == true)
            return ROWOWScriptValue::FromNumber(0);
        bool isNegative = false;
        uint32 itemID = 0;
        if (arguments[0].IsString == true)
        {
            auto itemIDIter = ItemIDsByAegisName.find(ToLower(arguments[0].Text));
            itemID = itemIDIter != ItemIDsByAegisName.end() ? itemIDIter->second : 0;
        }
        else
        {
            int64 value = arguments[0].GetNumber();
            isNegative = value < 0;
            itemID = uint32(std::abs(value));
        }
        std::string mapName = GetTextArgument(arguments, 2);
        int32 x = int32(GetArgument(arguments, 3, 0));
        int32 y = int32(GetArgument(arguments, 4, 0));
        uint32 mapID = 0;
        if (mapName == "this" && context.AttachedPlayer != nullptr)
        {
            // The player's map; on a stitched map the cells are its part's (a script's cells are its own map's)
            mapID = context.AttachedPlayer->GetMapId();
            int32 playerX = 0, playerY = 0;
            GetUnitCell(context.AttachedPlayer, playerX, playerY);
            if (ROWOWMapPart const* part = GetMapPartAt(mapID, playerX, playerY))
            {
                x += part->CellOffsetX;
                y += part->CellOffsetY;
            }
        }
        else
            mapID = ResolveMapCell(mapName, x, y);
        Map* map = nullptr;
        if (context.AttachedPlayer != nullptr)
            map = context.AttachedPlayer->GetMapId() == mapID ? context.AttachedPlayer->GetMap() : nullptr;
        else
            map = sMapMgr->FindBaseNonInstanceMap(mapID);
        auto itemTemplateIDIter = ItemTemplateIDsByItemID.find(itemID);
        if (map == nullptr || itemTemplateIDIter == ItemTemplateIDsByItemID.end())
            return ROWOWScriptValue::FromNumber(0);
        ROWOWDropItem item;
        item.ItemTemplateID = itemTemplateIDIter->second;
        item.Amount = uint32(std::max<int64>(1, GetArgument(arguments, 1, 1)));
        item.IsIdentified = isNegative == false || ItemDBIsIdentified(itemID);
        MapAddFloorItem(map, item, x, y, ObjectGuid::Empty, ObjectGuid::Empty, ObjectGuid::Empty, ROWOW_FLOORITEM_NO_DROP_CHECK, ROWOW_RO_DIRECTION_COUNT, false);
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("killmonster", [this, queueKill](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        // killmonster "<map>","<event>"{,<type>}: "All" for every script monster of the map
        std::string event = GetTextArgument(arguments, 1);
        queueKill(GetMapKeyByMapName(GetTextArgument(arguments, 0)), event, ToLower(event) == "all");
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("killmonsterall", [this, queueKill](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        // killmonsterall "<map>": the script monsters of the map (its normal spawns stay)
        queueKill(GetMapKeyByMapName(GetTextArgument(arguments, 0)), std::string(), true);
        return ROWOWScriptValue::FromNumber(0);
    });

    // The quest log (R8, ROWOW_Quest.cpp: quest.cpp).  The <char_id> argument (script_charid2sd) may only name the attached player: a run
    // touches no other player's quests
    auto getQuestPlayer = [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments, size_t charIDIndex, char const* commandName) -> Player*
    {
        Player* player = GetAttachedPlayer(context, commandName);
        if (player != nullptr && charIDIndex < arguments.size() && arguments[charIDIndex].GetNumber() != int64(player->GetGUID().GetCounter()))
        {
            Fail(context, std::string(commandName) + " of a character other than the attached one");
            return nullptr;
        }
        return player;
    };
    auto reportQuestError = [](ROWOWScriptContext& context, char const* commandName)
    {
        // script_reportsrc and script_reportfunc
        LOG_ERROR("module.ROWOW", "{} failed in the script of {}", commandName, context.Npc != nullptr ? context.Npc->UniqueName : std::string("a function"));
    };
    engine->RegisterCommand("setquest", [this, getQuestPlayer, reportQuestError](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // setquest <ID>{,<char_id>}
        if (Player* player = getQuestPlayer(context, arguments, 1, "setquest"))
            if (QuestAdd(player, uint32(GetArgument(arguments, 0, 0))) == -1)
                reportQuestError(context, "setquest");
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("erasequest", [this, getQuestPlayer, reportQuestError](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // erasequest <ID>{,<char_id>}
        if (Player* player = getQuestPlayer(context, arguments, 1, "erasequest"))
            if (QuestDelete(player, uint32(GetArgument(arguments, 0, 0))) == -1)
                reportQuestError(context, "erasequest");
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("completequest", [this, getQuestPlayer](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // completequest <ID>{,<char_id>}
        if (Player* player = getQuestPlayer(context, arguments, 1, "completequest"))
            QuestUpdateStatus(player, uint32(GetArgument(arguments, 0, 0)), ROWOW_QUEST_STATE_COMPLETE);
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("changequest", [this, getQuestPlayer, reportQuestError](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // changequest <ID>,<ID2>{,<char_id>}
        if (Player* player = getQuestPlayer(context, arguments, 2, "changequest"))
            if (QuestChange(player, uint32(GetArgument(arguments, 0, 0)), uint32(GetArgument(arguments, 1, 0))) == -1)
                reportQuestError(context, "changequest");
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("checkquest", [this, getQuestPlayer](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // checkquest(<ID>{,PLAYTIME|HUNTING{,<char_id>}}): -1 when the character doesn't have it
        Player* player = getQuestPlayer(context, arguments, 2, "checkquest");
        if (player == nullptr)
            return ROWOWScriptValue::FromNumber(-1);
        return ROWOWScriptValue::FromNumber(QuestCheck(*context.Identity, uint32(GetArgument(arguments, 0, 0)), uint8(GetArgument(arguments, 1, ROWOW_QUEST_CHECK_HAVEQUEST))));
    });
    engine->RegisterCommand("isbegin_quest", [this, getQuestPlayer](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // isbegin_quest(<ID>{,<char_id>}): 0 not taken, 1 taken, 2 complete
        Player* player = getQuestPlayer(context, arguments, 1, "isbegin_quest");
        if (player == nullptr)
            return ROWOWScriptValue::FromNumber(0);
        int32 state = QuestCheck(*context.Identity, uint32(GetArgument(arguments, 0, 0)), ROWOW_QUEST_CHECK_HAVEQUEST);
        return ROWOWScriptValue::FromNumber(state + (state < 1 ? 1 : 0));
    });

    // The die and kill events' ids (set by OnUnitDeath, pc_dead and mob_dead), which scripts may also set
    struct KillParameter
    {
        char const* Name;
        int64 ROWOWIdentity::* Value;
    };
    for (KillParameter const& killParameter : { KillParameter{ "KillerRID", &ROWOWIdentity::KillerRID }, KillParameter{ "KilledRID", &ROWOWIdentity::KilledRID },
        KillParameter{ "KilledGID", &ROWOWIdentity::KilledGID } })
    {
        int64 ROWOWIdentity::* value = killParameter.Value;
        engine->RegisterParameter(killParameter.Name, [value](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const&)
        {
            return ROWOWScriptValue::FromNumber(context.Identity != nullptr ? context.Identity->*value : 0);
        });
        engine->SetParameterSetter(killParameter.Name, [value](ROWOWScriptContext& context, ROWOWScriptValue const& newValue)
        {
            if (context.Identity != nullptr)
                context.Identity->*value = newValue.GetNumber();
        });
    }

    // Items while talking: pre-renewal lets the player use and put on items during a dialog (item_enabled_npc), until disable_items
    engine->RegisterCommand("enable_items", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const&)
    {
        context.IsItemUseDisabled = false;
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("disable_items", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const&)
    {
        context.IsItemUseDisabled = true;
        return ROWOWScriptValue::FromNumber(0);
    });

    // Warps of other players (the world update does them: the players may be on other maps) and map counts
    auto queueMapWarp = [this](ROWOWNpcMapWarp const& mapWarp)
    {
        std::lock_guard<std::mutex> lock(NpcStateMutex);
        PendingNpcMapWarps.push_back(mapWarp);
    };
    engine->RegisterCommand("mapwarp", [this, queueMapWarp](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        // mapwarp "<from map>","<to map>",<x>,<y>{,<type>,<ID>}: everyone on the map (the guild and party filters aren't in); a part of a stitched
        // map is its own cells of it
        ROWOWNpcMapWarp mapWarp;
        mapWarp.MapID = ResolveMapArea(GetTextArgument(arguments, 0), mapWarp.X1, mapWarp.Y1, mapWarp.X2, mapWarp.Y2);
        mapWarp.DestMapName = GetTextArgument(arguments, 1);
        mapWarp.DestX = int32(GetArgument(arguments, 2, 0));
        mapWarp.DestY = int32(GetArgument(arguments, 3, 0));
        if (mapWarp.MapID != 0)
            queueMapWarp(mapWarp);
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("areawarp", [this, queueMapWarp](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        // areawarp "<from map>",<x1>,<y1>,<x2>,<y2>,"<to map>",<x3>,<y3>{,<x4>,<y4>}: everyone in the box, to the cell (or a random one of x3..x4,
        // y3..y4)
        ROWOWNpcMapWarp mapWarp;
        mapWarp.X1 = int32(GetArgument(arguments, 1, 0));
        mapWarp.Y1 = int32(GetArgument(arguments, 2, 0));
        mapWarp.X2 = int32(GetArgument(arguments, 3, 0));
        mapWarp.Y2 = int32(GetArgument(arguments, 4, 0));
        mapWarp.MapID = ResolveMapArea(GetTextArgument(arguments, 0), mapWarp.X1, mapWarp.Y1, mapWarp.X2, mapWarp.Y2);
        mapWarp.DestMapName = GetTextArgument(arguments, 5);
        mapWarp.DestX = int32(GetArgument(arguments, 6, 0));
        mapWarp.DestY = int32(GetArgument(arguments, 7, 0));
        if (arguments.size() > 9)
        {
            mapWarp.DestX2 = int32(GetArgument(arguments, 8, 0));
            mapWarp.DestY2 = int32(GetArgument(arguments, 9, 0));
        }
        if (mapWarp.MapID != 0)
            queueMapWarp(mapWarp);
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("warpchar", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // warpchar "<map>",<x>,<y>,<char_id>: only the attached player can be warped from a run
        Player* player = GetAttachedPlayer(context, "warpchar");
        if (player != nullptr && GetArgument(arguments, 3, 0) == int64(player->GetGUID().GetCounter()))
            WarpPlayerToCell(player, GetTextArgument(arguments, 0), int32(GetArgument(arguments, 1, 0)), int32(GetArgument(arguments, 2, 0)));
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("getmapusers", [this](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        // getmapusers("<map>"): the players on it as of the last world update, -1 for a map that isn't converted (a part of a stitched map: its own)
        uint32 mapID = GetMapKeyByMapName(GetTextArgument(arguments, 0));
        if (mapID == 0)
            return ROWOWScriptValue::FromNumber(-1);
        std::lock_guard<std::mutex> lock(NpcStateMutex);
        auto countIter = MapUserCounts.find(mapID);
        return ROWOWScriptValue::FromNumber(countIter != MapUserCounts.end() ? int64(countIter->second) : 0);
    });
    engine->RegisterCommand("getareausers", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // getareausers("<map>",<x1>,<y1>,<x2>,<y2>): the players in the box; only the run's own map can be looked at (another map's players
        // belong to its thread), so 0 for another
        int32 x1 = int32(GetArgument(arguments, 1, 0)), y1 = int32(GetArgument(arguments, 2, 0));
        int32 x2 = int32(GetArgument(arguments, 3, 0)), y2 = int32(GetArgument(arguments, 4, 0));
        uint32 mapID = ResolveMapArea(GetTextArgument(arguments, 0), x1, y1, x2, y2);
        Map* map = context.AttachedPlayer != nullptr ? context.AttachedPlayer->GetMap() : nullptr;
        if (map == nullptr || map->GetId() != mapID)
            return ROWOWScriptValue::FromNumber(0);
        int64 count = 0;
        map->DoForAllPlayers([this, x1, y1, x2, y2, &count](Player* player)
        {
            int32 x = 0, y = 0;
            GetUnitCell(player, x, y);
            if (x >= std::min(x1, x2) && x <= std::max(x1, x2) && y >= std::min(y1, y2) && y <= std::max(y1, y2))
                count++;
        });
        return ROWOWScriptValue::FromNumber(count);
    });

    // Parties (a WoW group is the RO party), guilds and partners
    engine->RegisterCommand("is_party_leader", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const&)
    {
        // is_party_leader({<party id>}): whether the attached player leads its group
        Player* player = context.AttachedPlayer;
        Group* group = player != nullptr ? player->GetGroup() : nullptr;
        return ROWOWScriptValue::FromNumber(group != nullptr && group->GetLeaderGUID() == player->GetGUID() ? 1 : 0);
    });
    engine->RegisterCommand("getpartyname", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const&)
    {
        // WoW groups have no names: the leader's name stands in for a party that exists, "null" for none (rAthena's answer)
        Player* player = context.AttachedPlayer;
        Group* group = player != nullptr ? player->GetGroup() : nullptr;
        if (group == nullptr)
            return ROWOWScriptValue::FromText("null");
        std::string leaderName;
        for (Group::MemberSlot const& member : group->GetMemberSlots())
            if (member.guid == group->GetLeaderGUID())
                leaderName = member.name;
        return ROWOWScriptValue::FromText(leaderName + "'s party");
    });
    engine->RegisterCommand("getpartymember", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // getpartymember <party id>{,<type>{,<array>}}: $@partymembercount, and the array named, or else $@partymembername$[] (0), $@partymembercid[] (1)
        // or $@partymemberaid[] (2), of that group: the attached player's, or another one's from the world update, as the group store is the world thread's
        uint32 partyID = uint32(GetArgument(arguments, 0, 0));
        Group* group = context.AttachedPlayer != nullptr ? context.AttachedPlayer->GetGroup() : nullptr;
        if (group == nullptr || group->GetGUID().GetCounter() != partyID)
            group = IsInNpcWorldUpdate == true ? sGroupMgr->GetGroupByGUID(partyID) : nullptr;
        int64 type = GetArgument(arguments, 1, 0);
        std::vector<ROWOWScriptValue> values;
        if (group != nullptr)
        {
            for (Group::MemberSlot const& member : group->GetMemberSlots())
            {
                if (type == 0)
                    values.push_back(ROWOWScriptValue::FromText(member.name));
                else if (type == 1)
                    values.push_back(ROWOWScriptValue::FromNumber(int64(member.guid.GetCounter())));
                else
                {
                    Player* memberPlayer = ObjectAccessor::FindConnectedPlayer(member.guid);
                    values.push_back(ROWOWScriptValue::FromNumber(memberPlayer != nullptr && memberPlayer->GetSession() != nullptr ? int64(memberPlayer->GetSession()->GetAccountId()) : 0));
                }
            }
        }
        if (arguments.size() > 2 && arguments[2].IsReference == true)
            for (size_t index = 0; index < values.size(); index++)
                ROWOWSCRIPT->SetVariable(context, arguments[2].Text, uint32(arguments[2].Number + int64(index)), values[index]);
        std::string arrayName = type == 0 ? "$@partymembername$" : (type == 1 ? "$@partymembercid" : "$@partymemberaid");
        std::lock_guard<std::mutex> lock(ROWOWSCRIPT->VariableMutex);
        ROWOWSCRIPT->GlobalTempVariables.Set("$@partymembercount", 0, ROWOWScriptValue::FromNumber(int64(values.size())));
        if (arguments.size() <= 2)
            for (size_t index = 0; index < values.size(); index++)
                ROWOWSCRIPT->GlobalTempVariables.Set(arrayName, uint32(index), values[index]);
        return ROWOWScriptValue::FromNumber(int64(values.size()));
    });
    engine->SetReferenceArguments("getpartymember", 4);
    engine->RegisterCommand("getguildname", [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        // getguildname(<guild id>): "null" for none
        Guild* guild = sGuildMgr->GetGuildById(uint32(GetArgument(arguments, 0, 0)));
        return ROWOWScriptValue::FromText(guild != nullptr ? guild->GetName() : std::string("null"));
    });
    engine->RegisterCommand("getguildmaster", [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        // getguildmaster(<guild id>): its leader's name, "null" for none
        Guild* guild = sGuildMgr->GetGuildById(uint32(GetArgument(arguments, 0, 0)));
        if (guild == nullptr)
            return ROWOWScriptValue::FromText("null");
        Guild::Member const* leader = guild->GetMember(guild->GetLeaderGUID());
        return ROWOWScriptValue::FromText(leader != nullptr ? leader->GetName() : std::string("null"));
    });
    engine->RegisterCommand("is_guild_leader", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const&)
    {
        Player* player = context.AttachedPlayer;
        Guild* guild = player != nullptr && player->GetGuildId() != 0 ? sGuildMgr->GetGuildById(player->GetGuildId()) : nullptr;
        return ROWOWScriptValue::FromNumber(guild != nullptr && guild->GetLeaderGUID() == player->GetGUID() ? 1 : 0);
    });
    // Marriage and adoption (ROWOW_Family.cpp): the family of the attached player's RO identity, by the other identities' IDs (rAthena's char_id is an
    // RO identity, a save slot: getcharid(0) is the WoW character), 0 for none
    auto registerFamilyIDCommand = [this, engine](char const* commandName, ObjectGuid::LowType ROWOWFamily::* member)
    {
        engine->RegisterCommand(commandName, [this, member](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const&)
        {
            if (context.AttachedPlayer == nullptr)
                return ROWOWScriptValue::FromNumber(0);
            return ROWOWScriptValue::FromNumber(int64(GetFamily(context.AttachedPlayer).*member));
        });
    };
    registerFamilyIDCommand("getpartnerid", &ROWOWFamily::PartnerID);
    registerFamilyIDCommand("getchildid", &ROWOWFamily::ChildID);
    registerFamilyIDCommand("getmotherid", &ROWOWFamily::MotherID);
    registerFamilyIDCommand("getfatherid", &ROWOWFamily::FatherID);
    engine->RegisterCommand("ispartneron", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const&)
    {
        // ispartneron: whether the partner is online (its character, with that save slot in use)
        Player* player = GetAttachedPlayer(context, "ispartneron");
        if (player == nullptr)
            return ROWOWScriptValue::FromNumber(0);
        ROWOWFamily family = GetFamily(player);
        return ROWOWScriptValue::FromNumber(family.PartnerID != 0 && FindIdentityPlayer(family.PartnerID, false) != nullptr ? 1 : 0);
    });
    engine->RegisterCommand("marriage", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // marriage "<name>": pc_marriage of the attached player and the one of that name (online, in the RO world): 1 when they're married
        Player* player = GetAttachedPlayer(context, "marriage");
        if (player == nullptr)
            return ROWOWScriptValue::FromNumber(0);
        std::string name = GetTextArgument(arguments, 0);
        Player* partner = normalizePlayerName(name) == true ? ObjectAccessor::FindPlayerByName(name) : nullptr;
        return ROWOWScriptValue::FromNumber(PCMarriage(player, partner) == true ? 1 : 0);
    });
    engine->RegisterCommand("wedding", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const&)
    {
        // wedding: clif_wedding_effect on the attached player.  The converter made no wedding effect: the Blessing light stands in
        if (Player* player = GetAttachedPlayer(context, "wedding"))
            PlayScriptLook(player, ROWOW_SCRIPT_LOOK_EFFECT, "BLESSING");
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("divorce", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const&)
    {
        // divorce: pc_divorce of the attached player: 1 when it was married
        Player* player = GetAttachedPlayer(context, "divorce");
        return ROWOWScriptValue::FromNumber(player != nullptr && PCDivorce(player) == true ? 1 : 0);
    });
    engine->RegisterCommand("adopt", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // adopt "<parent>","<baby>": the baby is asked, as a parent's right-click asks it (the ADOPT_* answer: 0 when it's asked).  No official script uses it
        std::string parentName = GetTextArgument(arguments, 0);
        std::string babyName = GetTextArgument(arguments, 1);
        Player* parent = normalizePlayerName(parentName) == true ? ObjectAccessor::FindPlayerByName(parentName) : nullptr;
        Player* baby = normalizePlayerName(babyName) == true ? ObjectAccessor::FindPlayerByName(babyName) : nullptr;
        if (IsInNpcWorldUpdate == false && (context.AttachedPlayer == nullptr || (parent != nullptr && parent->GetMap() != context.AttachedPlayer->GetMap()) ||
            (baby != nullptr && baby->GetMap() != context.AttachedPlayer->GetMap())))
            return ROWOWScriptValue::FromNumber(ROWOW_ADOPT_CHARACTER_NOT_FOUND); // Other players only on the run's map, or from the world update
        if (parent == nullptr || baby == nullptr)
            return ROWOWScriptValue::FromNumber(ROWOW_ADOPT_CHARACTER_NOT_FOUND);
        ROWOWFamily family = GetFamily(parent);
        Player* partner = family.PartnerID != 0 ? FindIdentityPlayer(family.PartnerID, true) : nullptr;
        int32 response = PCTryAdopt(parent, partner, baby, false);
        if (response == ROWOW_ADOPT_ALLOWED)
            PCAdoptRequest(parent, baby);
        return ROWOWScriptValue::FromNumber(response);
    });

    // Strings
    engine->RegisterCommand("strtolower", [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        return ROWOWScriptValue::FromText(ToLower(GetTextArgument(arguments, 0)));
    });
    engine->RegisterCommand("strtoupper", [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        std::string text = GetTextArgument(arguments, 0);
        for (char& curChar : text)
            curChar = char(toupper((unsigned char)curChar));
        return ROWOWScriptValue::FromText(text);
    });
    engine->RegisterCommand("charisalpha", [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        std::string text = GetTextArgument(arguments, 0);
        int64 index = GetArgument(arguments, 1, 0);
        return ROWOWScriptValue::FromNumber(index >= 0 && size_t(index) < text.size() && isalpha((unsigned char)text[size_t(index)]) != 0 ? 1 : 0);
    });
    engine->RegisterCommand("strpos", [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        // strpos("<haystack>","<needle>"{,<offset>}): -1 when it isn't there
        std::string haystack = GetTextArgument(arguments, 0);
        size_t offset = size_t(std::max<int64>(0, GetArgument(arguments, 2, 0)));
        size_t position = offset <= haystack.size() ? haystack.find(GetTextArgument(arguments, 1), offset) : std::string::npos;
        return ROWOWScriptValue::FromNumber(position == std::string::npos ? -1 : int64(position));
    });
    engine->RegisterCommand("countstr", [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        // countstr("<haystack>","<needle>"{,<case sensitive>})
        std::string haystack = GetTextArgument(arguments, 0);
        std::string needle = GetTextArgument(arguments, 1);
        if (GetArgument(arguments, 2, 1) == 0)
        {
            haystack = ToLower(haystack);
            needle = ToLower(needle);
        }
        int64 count = 0;
        for (size_t position = needle.empty() == true ? std::string::npos : haystack.find(needle); position != std::string::npos; position = haystack.find(needle, position + needle.size()))
            count++;
        return ROWOWScriptValue::FromNumber(count);
    });
    engine->RegisterCommand("replacestr", [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        // replacestr("<input>","<search>","<replace>"{,<case sensitive>{,<count>}})
        std::string input = GetTextArgument(arguments, 0);
        std::string search = GetTextArgument(arguments, 1);
        std::string replace = GetTextArgument(arguments, 2);
        bool isCaseSensitive = GetArgument(arguments, 3, 1) != 0;
        int64 count = GetArgument(arguments, 4, 0);
        if (search.empty() == true)
            return ROWOWScriptValue::FromText(input);
        std::string searchedInput = isCaseSensitive == true ? input : ToLower(input);
        std::string searchedFor = isCaseSensitive == true ? search : ToLower(search);
        std::string output;
        size_t start = 0;
        int64 replaced = 0;
        for (size_t position = searchedInput.find(searchedFor); position != std::string::npos && (count <= 0 || replaced < count); position = searchedInput.find(searchedFor, start))
        {
            output += input.substr(start, position - start) + replace;
            start = position + search.size();
            replaced++;
        }
        return ROWOWScriptValue::FromText(output + input.substr(start));
    });
    engine->RegisterCommand("insertchar", [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        // insertchar("<text>","<char>",<index>)
        std::string text = GetTextArgument(arguments, 0);
        size_t index = size_t(std::clamp<int64>(GetArgument(arguments, 2, 0), 0, int64(text.size())));
        return ROWOWScriptValue::FromText(text.insert(index, GetTextArgument(arguments, 1).substr(0, 1)));
    });
    engine->RegisterCommand("delchar", [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        // delchar("<text>",<index>)
        std::string text = GetTextArgument(arguments, 0);
        int64 index = GetArgument(arguments, 1, 0);
        if (index >= 0 && size_t(index) < text.size())
            text.erase(size_t(index), 1);
        return ROWOWScriptValue::FromText(text);
    });
    engine->RegisterCommand("gettimestr", [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        // gettimestr("<strftime format>",<max length>{,<Unix time>})
        time_t when = arguments.size() > 2 ? time_t(arguments[2].GetNumber()) : time(nullptr);
        tm localTime = Acore::Time::TimeBreakdown(when);
        size_t maxLength = size_t(std::clamp<int64>(GetArgument(arguments, 1, 0), 1, 256));
        std::vector<char> buffer(maxLength + 1, '\0');
        strftime(buffer.data(), maxLength, GetTextArgument(arguments, 0).c_str(), &localTime);
        return ROWOWScriptValue::FromText(std::string(buffer.data()));
    });
    engine->RegisterCommand("sprintf", [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        // sprintf("<format>",...): C's format with %d/%i/%u/%x/%X/%o/%c (a number) and %s (a text), one value each, in order (no %n$ positions)
        std::string format = GetTextArgument(arguments, 0);
        std::string output;
        size_t argumentIndex = 1;
        for (size_t position = 0; position < format.size(); position++)
        {
            if (format[position] != '%')
            {
                output += format[position];
                continue;
            }
            if (position + 1 < format.size() && format[position + 1] == '%')
            {
                output += '%';
                position++;
                continue;
            }
            size_t specEnd = position + 1;
            while (specEnd < format.size() && strchr("diuxXocsfeEgG", format[specEnd]) == nullptr)
                specEnd++;
            if (specEnd >= format.size())
            {
                output += format.substr(position);
                break;
            }
            std::string spec = format.substr(position, specEnd - position + 1);
            char conversion = format[specEnd];
            char buffer[512];
            if (conversion == 's')
                snprintf(buffer, sizeof(buffer), spec.c_str(), (argumentIndex < arguments.size() ? arguments[argumentIndex].GetText() : std::string()).c_str());
            else if (strchr("feEgG", conversion) != nullptr)
                snprintf(buffer, sizeof(buffer), spec.c_str(), double(argumentIndex < arguments.size() ? arguments[argumentIndex].GetNumber() : 0));
            else
            {
                spec.insert(spec.size() - 1, "ll");
                snprintf(buffer, sizeof(buffer), spec.c_str(), (long long)(argumentIndex < arguments.size() ? arguments[argumentIndex].GetNumber() : 0));
            }
            argumentIndex++;
            output += buffer;
            position = specEnd;
        }
        return ROWOWScriptValue::FromText(output);
    });

    // The player
    engine->RegisterCommand("getsavepoint", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // getsavepoint(<0 map, 1 x, 2 y>)
        if (context.Identity == nullptr || context.Identity->SavePoint.IsSet == false)
            return GetArgument(arguments, 0, 0) == 0 ? ROWOWScriptValue::FromText(std::string()) : ROWOWScriptValue::FromNumber(0);
        ROWOWPosition const& savePoint = context.Identity->SavePoint;
        int32 x = 0, y = 0;
        GetWorldCell(savePoint.MapID, savePoint.X, savePoint.Y, x, y);
        std::string mapName = GetMapNameAt(savePoint.MapID, x, y); // On a stitched map, its part's name and cell
        switch (GetArgument(arguments, 0, 0))
        {
            case 0: return ROWOWScriptValue::FromText(mapName);
            case 1: return ROWOWScriptValue::FromNumber(x);
            case 2: return ROWOWScriptValue::FromNumber(y);
            default: return ROWOWScriptValue::FromNumber(0);
        }
    });
    engine->RegisterCommand("getgmlevel", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const&)
    {
        // The account's security level stands for the group level
        return ROWOWScriptValue::FromNumber(context.AttachedPlayer != nullptr ? int64(context.AttachedPlayer->GetSession()->GetSecurity()) : 0);
    });
    engine->RegisterCommand("vip_status", [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const&)
    {
        // No VIP system (rAthena's VIP_SCRIPT off): never VIP
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("basicskillcheck", [this](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const&)
    {
        auto flagIter = BattleFlagsByName.find("basic_skill_check");
        return ROWOWScriptValue::FromNumber(flagIter != BattleFlagsByName.end() ? flagIter->second : 1);
    });
    engine->RegisterCommand("dispbottom", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // dispbottom "<message>"{,<color>}: a line in the player's chat
        if (Player* player = GetAttachedPlayer(context, "dispbottom"))
            ChatHandler(player->GetSession()).SendSysMessage(GetTextArgument(arguments, 0));
        return ROWOWScriptValue::FromNumber(0);
    });
    for (char const* logCommandName : { "logmes", "debugmes" })
    {
        engine->RegisterCommand(logCommandName, [logCommandName](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
        {
            LOG_INFO("module.ROWOW", "{} ({}): {}", logCommandName, context.Npc != nullptr ? context.Npc->UniqueName : std::string("a function"), GetTextArgument(arguments, 0));
            return ROWOWScriptValue::FromNumber(0);
        });
    }
    engine->RegisterCommand("progressbar", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // progressbar "<color>",<seconds>: the player waits that long (no bar drawn yet; moving doesn't cancel it), as sleep2 does
        Player* player = GetAttachedPlayer(context, "progressbar");
        if (player == nullptr)
            return ROWOWScriptValue::FromNumber(0);
        context.Wait = ROWOW_SCRIPT_WAIT_SLEEP;
        context.SleepUntil = uint64(GameTime::GetGameTimeMS().count()) + uint64(std::max<int64>(0, GetArgument(arguments, 1, 0))) * 1000;
        return ROWOWScriptValue::FromNumber(1);
    });
    engine->RegisterCommand("unloadnpc", [this](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        // unloadnpc "<NPC>": gone for good (NPCs are startup data here, so it stays disabled until the server restarts)
        SetNpcEnabled(GetNpcByName(GetTextArgument(arguments, 0)), false);
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("nude", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const&)
    {
        // nude: everything worn comes off (pc_unequipitem flag 2, then one status calculation)
        Player* player = GetAttachedPlayer(context, "nude");
        if (player == nullptr)
            return ROWOWScriptValue::FromNumber(0);
        std::vector<uint32> equipPositions;
        for (auto const& itemGUIDByEquipPosition : context.Identity->ItemGUIDsByEquipPosition)
            equipPositions.push_back(itemGUIDByEquipPosition.first);
        for (uint32 equipPosition : equipPositions)
            PCUnequipItem(player, equipPosition, 2);
        RefreshROForm(player);
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("getlook", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // getlook(<LOOK_*>): the hair style (1) and color (6); the clothes color (7) isn't kept (no palettes yet), so 0
        if (context.Identity == nullptr)
            return ROWOWScriptValue::FromNumber(-1);
        switch (GetArgument(arguments, 0, 0))
        {
            case 1: return ROWOWScriptValue::FromNumber(context.Identity->HairStyle);
            case 6: return ROWOWScriptValue::FromNumber(context.Identity->HairColor);
            case 7: return ROWOWScriptValue::FromNumber(0);
            default: return ROWOWScriptValue::FromNumber(-1);
        }
    });
    engine->RegisterCommand("setlook", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // setlook <LOOK_*>,<value>: the stylists' hair style (1) and color (6), kept with the identity and shown at once on the sprite (its
        // head, R9.1) and in the addon's hair options; the clothes color (7) isn't kept yet
        if (context.Identity == nullptr)
            return ROWOWScriptValue::FromNumber(0);
        int64 value = GetArgument(arguments, 1, 0);
        int64 look = GetArgument(arguments, 0, 0);
        switch (look)
        {
            case 1: context.Identity->HairStyle = uint8(std::clamp<int64>(value, 0, 255)); break;
            case 6: context.Identity->HairColor = uint8(std::clamp<int64>(value, 0, 255)); break;
            default: break;
        }
        if ((look == 1 || look == 6) && context.AttachedPlayer != nullptr)
        {
            if (GetIdentity(context.AttachedPlayer) == context.Identity)
                SaveSharedHair(context.AttachedPlayer); // The head of every save slot (Nathan 2026-11-09)
            RefreshIllusionDisplay(context.AttachedPlayer);
            SendHairToClient(context.AttachedPlayer);
        }
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("checkcart", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const&)
    {
        return ROWOWScriptValue::FromNumber(context.AttachedPlayer != nullptr && PCIsCartOn(context.AttachedPlayer) == true ? 1 : 0);
    });
    engine->RegisterCommand("setcart", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // setcart {<type>}: a Kafra's cart rental (pc_setcart: it needs Pushcart), 0 takes it off
        if (Player* player = GetAttachedPlayer(context, "setcart"))
            PCSetCart(player, int32(GetArgument(arguments, 0, 1)));
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("checkriding", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const&)
    {
        return ROWOWScriptValue::FromNumber(context.AttachedPlayer != nullptr && PCIsRiding(context.AttachedPlayer) == true ? 1 : 0);
    });
    engine->RegisterCommand("setriding", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // setriding {<flag>}: a Peco Peco breeder's (it needs Riding)
        if (Player* player = GetAttachedPlayer(context, "setriding"))
            PCSetRiding(player, GetArgument(arguments, 0, 1) != 0);
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("checkfalcon", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const&)
    {
        return ROWOWScriptValue::FromNumber(context.AttachedPlayer != nullptr && PCIsFalcon(context.AttachedPlayer) == true ? 1 : 0);
    });
    engine->RegisterCommand("setfalcon", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // setfalcon {<flag>}: the Hunter guild's falcon breeder (it needs Falconry Mastery)
        if (Player* player = GetAttachedPlayer(context, "setfalcon"))
            PCSetFalcon(player, GetArgument(arguments, 0, 1) != 0);
        return ROWOWScriptValue::FromNumber(0);
    });
    for (char const* noMountCommandName : { "ismounting", "checkwug", "checkdragon", "checkmadogear" })
    {
        // The renewal and third job mounts (and the cash mount) don't exist in pre-renewal
        engine->RegisterCommand(noMountCommandName, [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const&) { return ROWOWScriptValue::FromNumber(0); });
    }
    engine->RegisterCommand("getiteminfo", [this](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        // getiteminfo(<item id or "AegisName">,<ITEMINFO_*>): -1 for an unknown item or a field not kept
        uint32 itemID = arguments.empty() == false && arguments[0].IsString == true ? 0 : uint32(GetArgument(arguments, 0, 0));
        if (arguments.empty() == false && arguments[0].IsString == true)
        {
            auto itemIDIter = ItemIDsByAegisName.find(ToLower(arguments[0].Text));
            itemID = itemIDIter != ItemIDsByAegisName.end() ? itemIDIter->second : 0;
        }
        auto itemTemplateIDIter = ItemTemplateIDsByItemID.find(itemID);
        auto itemIter = itemTemplateIDIter != ItemTemplateIDsByItemID.end() ? ItemsByItemTemplateID.find(itemTemplateIDIter->second) : ItemsByItemTemplateID.end();
        if (itemIter == ItemsByItemTemplateID.end())
            return ROWOWScriptValue::FromNumber(-1);
        ROWOWItem const& item = itemIter->second;
        switch (GetArgument(arguments, 1, -1))
        {
            case 0: return ROWOWScriptValue::FromNumber(item.Buy); // ITEMINFO_BUY
            case 1: return ROWOWScriptValue::FromNumber(item.Buy / 2); // ITEMINFO_SELL (item_db's default)
            case 2: return ROWOWScriptValue::FromNumber(item.Type); // ITEMINFO_TYPE
            case 4: return ROWOWScriptValue::FromNumber(item.Gender); // ITEMINFO_GENDER
            case 5: return ROWOWScriptValue::FromNumber(int64(item.Locations)); // ITEMINFO_LOCATIONS
            case 6: return ROWOWScriptValue::FromNumber(item.Weight); // ITEMINFO_WEIGHT
            case 7: return ROWOWScriptValue::FromNumber(item.Attack); // ITEMINFO_ATTACK
            case 8: return ROWOWScriptValue::FromNumber(item.Defense); // ITEMINFO_DEFENSE
            case 9: return ROWOWScriptValue::FromNumber(item.Range); // ITEMINFO_RANGE
            case 10: return ROWOWScriptValue::FromNumber(item.Slots); // ITEMINFO_SLOT
            case 12: return ROWOWScriptValue::FromNumber(int64(item.EquipLevelMin)); // ITEMINFO_EQUIPLEVELMIN
            case 13: return ROWOWScriptValue::FromNumber(item.WeaponLevel); // ITEMINFO_WEAPONLEVEL
            case 15: return ROWOWScriptValue::FromNumber(int64(item.EquipLevelMax)); // ITEMINFO_EQUIPLEVELMAX
            case 17: return ROWOWScriptValue::FromNumber(int64(item.ItemID)); // ITEMINFO_ID
            case 19: return ROWOWScriptValue::FromNumber(item.IsArmor == true ? 1 : 0); // ITEMINFO_ARMORLEVEL (pre-renewal armor is level 1)
            default: return ROWOWScriptValue::FromNumber(-1);
        }
    });
    engine->RegisterCommand("readbook", [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const&)
    {
        // The client's book reader isn't in: the book doesn't open
        return ROWOWScriptValue::FromNumber(0);
    });

    // Kafra storage (R8.2, ROWOW_Storage.cpp): the character's WoW bank, and its guild's bank when the guild has one (plan section 8 item 7)
    engine->RegisterCommand("openstorage", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const&)
    {
        if (Player* player = GetAttachedPlayer(context, "openstorage"))
            StorageStorageOpen(player);
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("guildopenstorage", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const&)
    {
        // guildopenstorage(): GSTORAGE_OPEN (0) when the guild bank opens, else why not
        Player* player = GetAttachedPlayer(context, "guildopenstorage");
        return ROWOWScriptValue::FromNumber(player != nullptr ? StorageGuildStorageOpen(player) : int64(ROWOW_GSTORAGE_NO_GUILD));
    });

    // The NPCs' services (R8.4): an NPC's skill on the player (inns, the job quests' blessings and heals), and the repairmen
    engine->RegisterCommand("npcskill", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // npcskill <skill id or "name">,<skill level>,<stat point>,<NPC level>: the NPC talking with the player uses the skill on it, with
        // every stat at stat point and the level given (status_calc_npc), at once (unit_skilluse_id2; an NPC's skills have no cast time here)
        Player* player = GetAttachedPlayer(context, "npcskill");
        if (player == nullptr || arguments.size() < 4)
            return ROWOWScriptValue::FromNumber(0);
        uint16 skillID = arguments[0].IsString == true ? GetSkillIDByName(arguments[0].Text) : uint16(arguments[0].GetNumber());
        ROWOWSkill const* skill = GetSkill(skillID);
        if (skill == nullptr)
            return Fail(context, "npcskill: invalid skill " + arguments[0].GetText());
        uint16 skillLevel = uint16(std::clamp<int64>(arguments[1].GetNumber(), 1, UINT16_MAX));
        int64 statPoint = arguments[2].GetNumber();
        int64 npcLevel = arguments[3].GetNumber();
        if (statPoint < 0 || statPoint > 99 || npcLevel < 0 || npcLevel > 99)
            return Fail(context, "npcskill: stat point or level over the maximum");
        // The NPC is its creature on the player's map (rAthena's npc_id)
        Creature* npcCreature = nullptr;
        if (context.Npc != nullptr && context.Npc->MapID == player->GetMapId())
        {
            for (ObjectGuid::LowType spawnID : context.Npc->CreatureSpawnIDs)
            {
                auto creatureRange = player->GetMap()->GetCreatureBySpawnIdStore().equal_range(spawnID);
                if (creatureRange.first != creatureRange.second)
                {
                    npcCreature = creatureRange.first->second;
                    break;
                }
            }
        }
        if (npcCreature == nullptr || npcCreature->IsInWorld() == false)
            return Fail(context, "npcskill: the NPC isn't on the player's map");
        // status_calc_npc: SCO_FIRST's fixed values the first time, then the stats and what the level makes of them
        ROWOWStatus& status = npcCreature->CustomData.GetDefault<ROWOWMobStatus>("ROWOWMobStatus")->Status;
        if (status.IsPlayer == true)
        {
            status.IsPlayer = false;
            status.MaxHP = 1;
            status.MaxSP = 1;
            status.DefElement = ROWOW_ELEMENT_NEUTRAL;
            status.ElementLevel = 1;
            status.Race = ROWOW_RACE_DEMIHUMAN;
            status.Class = ROWOW_CLASS_NORMAL;
            status.Size = ROWOW_SIZE_SMALL;
            status.Range = 1 + status.Size;
            status.Mode = ROWOW_MODE_CANMOVE | ROWOW_MODE_CANATTACK;
        }
        status.Level = uint32(npcLevel);
        for (int32& stat : status.Stats)
            stat = int32(statPoint);
        StatusCalcMisc(status, status.Level);
        if ((skill->Inf & ROWOW_INF_GROUND_SKILL) != 0)
        {
            int32 x = 0, y = 0;
            GetUnitCell(player, x, y);
            GetSkillUnitData(npcCreature)->SkillZ = player->GetPositionZ();
            SkillCastendPos2(npcCreature, x, y, skillID, skillLevel, GetTick(), 0);
        }
        else
            SkillCastendNoDamageID(npcCreature, player, skillID, skillLevel, GetTick(), 0);
        return ROWOWScriptValue::FromNumber(0);
    });
    // The broken items (attribute 1) in the bags in their order, as rAthena counts its inventory
    auto getBrokenItem = [this](Player* player, int64 number) -> Item*
    {
        int64 brokenCounter = 0;
        for (uint8 bag = 0; bag <= 4; bag++)
        {
            for (uint32 slot = 1; slot <= MAX_BAG_SIZE; slot++)
            {
                Item* item = GetItemByClientPosition(player, bag, slot);
                if (item == nullptr || GetROItem(item) == nullptr || IsItemBroken(item) == false)
                    continue;
                if (++brokenCounter == number)
                    return item;
            }
        }
        return nullptr;
    };
    // A command's <char id> may only be the attached player (as the quest commands')
    auto getCharIDPlayer = [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments, size_t index, char const* commandName) -> Player*
    {
        Player* player = GetAttachedPlayer(context, commandName);
        if (player != nullptr && index < arguments.size() && arguments[index].GetNumber() != int64(player->GetGUID().GetCounter()))
        {
            Fail(context, std::string(commandName) + ": only the attached player's character");
            return nullptr;
        }
        return player;
    };
    engine->RegisterCommand("getbrokenid", [this, getBrokenItem, getCharIDPlayer](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // getbrokenid(<number>{,<char id>}): the item_db Id of the number-th broken item, 0 when there are fewer
        Player* player = getCharIDPlayer(context, arguments, 1, "getbrokenid");
        if (player == nullptr)
            return ROWOWScriptValue::FromNumber(0);
        ROWOWItem const* roItem = GetROItem(getBrokenItem(player, GetArgument(arguments, 0, 0)));
        return ROWOWScriptValue::FromNumber(roItem != nullptr ? int64(roItem->ItemID) : 0);
    });
    engine->RegisterCommand("repair", [this, getBrokenItem, getCharIDPlayer](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // repair <number>{,<char id>}: the number-th broken item mended (clif_produceeffect's repair message)
        Player* player = getCharIDPlayer(context, arguments, 1, "repair");
        Item* item = player != nullptr ? getBrokenItem(player, GetArgument(arguments, 0, 0)) : nullptr;
        if (item == nullptr)
            return ROWOWScriptValue::FromNumber(0);
        SetItemState(player, item, GetItemRefine(item), false, IsItemIdentified(item));
        ChatHandler(player->GetSession()).PSendSysMessage("{} has been repaired.", item->GetTemplate()->Name1);
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("repairall", [this, getBrokenItem, getCharIDPlayer](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // repairall {<char id>}: every broken item mended
        Player* player = getCharIDPlayer(context, arguments, 0, "repairall");
        if (player == nullptr)
            return ROWOWScriptValue::FromNumber(0);
        while (Item* item = getBrokenItem(player, 1))
        {
            SetItemState(player, item, GetItemRefine(item), false, IsItemIdentified(item));
            ChatHandler(player->GetSession()).PSendSysMessage("{} has been repaired.", item->GetTemplate()->Name1);
        }
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("preg_match", [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        // preg_match(<pattern>,<string>{,<offset>}): 0 for no match, else the captures + 1 as pcre_exec counts them (an ECMAScript regex here,
        // which has the lookaheads and \b the scripts use)
        std::string subject = GetTextArgument(arguments, 1);
        size_t offset = size_t(std::clamp<int64>(GetArgument(arguments, 2, 0), 0, int64(subject.size())));
        try
        {
            std::regex pattern(GetTextArgument(arguments, 0));
            std::smatch match;
            std::string const searched = subject.substr(offset);
            if (std::regex_search(searched, match, pattern) == false)
                return ROWOWScriptValue::FromNumber(0);
            int64 count = int64(match.size());
            while (count > 1 && match[size_t(count - 1)].matched == false)
                count--;
            return ROWOWScriptValue::FromNumber(count);
        }
        catch (std::regex_error const&)
        {
            return ROWOWScriptValue::FromNumber(0);
        }
    });

    // Systems out of scope (plan section 8 item 10: War of Emperium, battlegrounds) or not here (instances): the commands answer "none" so
    // the scripts' OnInit and dialogs go on, and do nothing
    struct StubCommand
    {
        char const* Name;
        char const* Reason;
    };
    char const* const WoEReason = "War of Emperium is out of scope (plan section 8 item 10)";
    char const* const BattlegroundReason = "battlegrounds are out of scope (plan section 8 item 10)";
    char const* const InstanceReason = "there are no RO instances";
    char const* const MapCellReason = "map cells and flags can't be changed at run time yet";
    for (StubCommand const& stubCommand : { StubCommand{ "getcastledata", WoEReason }, StubCommand{ "agitcheck", WoEReason }, StubCommand{ "agitcheck2", WoEReason },
        StubCommand{ "guardian", WoEReason }, StubCommand{ "bg_get_data", BattlegroundReason }, StubCommand{ "instance_id", InstanceReason },
        StubCommand{ "instance_check_party", InstanceReason } })
    {
        std::string name = stubCommand.Name;
        char const* reason = stubCommand.Reason;
        engine->RegisterCommand(name, [name, reason](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const&)
        {
            LogStubCommandOnce(name, reason);
            return ROWOWScriptValue::FromNumber(0);
        });
        engine->SetStub(name);
    }
    for (StubCommand const& stubCommand : { StubCommand{ "setcastledata", WoEReason }, StubCommand{ "flagemblem", WoEReason }, StubCommand{ "gvgon", WoEReason },
        StubCommand{ "gvgoff", WoEReason }, StubCommand{ "maprespawnguildid", WoEReason }, StubCommand{ "agitstart", WoEReason }, StubCommand{ "agitend", WoEReason },
        StubCommand{ "agitstart2", WoEReason }, StubCommand{ "agitend2", WoEReason }, StubCommand{ "bg_warp", BattlegroundReason },
        StubCommand{ "bg_monster", BattlegroundReason }, StubCommand{ "bg_reserve", BattlegroundReason }, StubCommand{ "bg_unbook", BattlegroundReason },
        StubCommand{ "bg_leave", BattlegroundReason }, StubCommand{ "bg_desert", BattlegroundReason }, StubCommand{ "bg_destroy", BattlegroundReason },
        StubCommand{ "bg_updatescore", BattlegroundReason }, StubCommand{ "bg_team_setxy", BattlegroundReason }, StubCommand{ "instance_warpall", InstanceReason },
        StubCommand{ "setmapflag", MapCellReason }, StubCommand{ "removemapflag", MapCellReason }, StubCommand{ "setcell", MapCellReason },
        StubCommand{ "setwall", MapCellReason }, StubCommand{ "delwall", MapCellReason }, StubCommand{ "setnpcdisplay", "NPC looks can't change yet (R9)" },
        StubCommand{ "movenpc", "NPCs can't move yet" }, StubCommand{ "refineui", "the refine window is a renewal feature (feature.refineui is off)" },
        StubCommand{ "waitingroom2bg", BattlegroundReason } })
    {
        std::string name = stubCommand.Name;
        char const* reason = stubCommand.Reason;
        engine->RegisterCommand(name, [name, reason](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const&)
        {
            LogStubCommandOnce(name, reason);
            return ROWOWScriptValue::FromNumber(0);
        });
        engine->SetStub(name);
    }
    engine->RegisterCommand("getcastlename", [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const&) { return ROWOWScriptValue::FromText(std::string()); });
    engine->SetStub("getcastlename");
    engine->RegisterCommand("guardianinfo", [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const&) { return ROWOWScriptValue::FromNumber(-1); });
    engine->SetStub("guardianinfo");
    engine->RegisterCommand("checkwall", [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const&) { return ROWOWScriptValue::FromNumber(0); });
    engine->SetStub("checkwall");
    engine->RegisterCommand("instance_create", [InstanceReason](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const&)
    {
        // No instances: creating one fails as an unknown instance does (-1)
        LogStubCommandOnce("instance_create", InstanceReason);
        return ROWOWScriptValue::FromNumber(-1);
    });
    engine->SetStub("instance_create");
    engine->RegisterCommand("instance_enter", [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const&)
    {
        return ROWOWScriptValue::FromNumber(1); // IE_NOINSTANCE
    });
    engine->SetStub("instance_enter");
    for (char const* instanceNameCommandName : { "instance_npcname", "instance_mapname" })
    {
        // Outside an instance rAthena reports an invalid instance and gives ""
        engine->RegisterCommand(instanceNameCommandName, [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const&) { return ROWOWScriptValue::FromText(std::string()); });
        engine->SetStub(instanceNameCommandName);
    }

    // Waiting rooms (the job tests' queues, ROWOW_WaitingRoom.cpp)
    RegisterWaitingRoomCommands();

    // Jobs (R8.3): a job's class (rAthena's MAPID, the EAJ_ and EAJL_ constants) for eaclass and roclass
    for (auto const& jobClassConstant : ROWOWJobClassConstants)
        engine->RegisterConstant(jobClassConstant.first, int64(jobClassConstant.second));
    engine->RegisterCommand("eaclass", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // eaclass({<job>{,<char id>}}): pc_jobid2mapid of the job, or of the attached player's (-1 with none)
        int64 jobID = 0;
        if (arguments.empty() == false)
            jobID = arguments[0].GetNumber();
        else if (context.AttachedPlayer != nullptr && context.Identity != nullptr)
            jobID = context.Identity->JobID;
        else
            return ROWOWScriptValue::FromNumber(-1);
        return ROWOWScriptValue::FromNumber(PCJobIDToMapID(int32(jobID)));
    });
    engine->RegisterCommand("roclass", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // roclass(<class>{,<sex>}): pc_mapid2jobid, for the attached player's sex (SEX_MALE 1) or male with none
        bool isMale = true;
        if (arguments.size() > 1)
            isMale = arguments[1].GetNumber() == 1;
        else if (context.AttachedPlayer != nullptr)
            isMale = context.AttachedPlayer->getGender() == GENDER_MALE;
        return ROWOWScriptValue::FromNumber(PCMapIDToJobID(uint64(GetArgument(arguments, 0, 0)), isMale));
    });
    engine->RegisterCommand("resetskill", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const&)
    {
        // resetskill{ <char id>}: pc_resetskill with flag 1, every skill point back
        if (Player* player = GetAttachedPlayer(context, "resetskill"))
            PCResetSkill(player);
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("resetlvl", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // resetlvl <action type>{,<char id>}: pc_resetlvl (the Valkyrie's rebirth is type 1, after the change to High Novice)
        if (Player* player = GetAttachedPlayer(context, "resetlvl"))
            PCResetLvl(player, int32(GetArgument(arguments, 0, 0)));
        return ROWOWScriptValue::FromNumber(0);
    });
    auto getOption =[this](Player* player) -> int64
    {
        // sc.option: the statuses' options (hiding, cloaking...) and the identity's own (a Peco, a falcon).  The cart isn't an option
        // (NEW_CARTS: the client the constants are for has SC_PUSH_CART)
        ROWOWStatusChange const* sc = GetStatusChange(static_cast<Unit const*>(player));
        return int64((sc != nullptr ? sc->Option : 0) | GetIdentity(player)->Option);
    };
    engine->RegisterCommand("checkoption", [getOption](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // checkoption(<option>{,<char id>})
        Player* player = GetAttachedPlayer(context, "checkoption");
        if (player == nullptr)
            return ROWOWScriptValue::FromNumber(0);
        return ROWOWScriptValue::FromNumber((getOption(player) & GetArgument(arguments, 0, 0)) != 0 ? 1 : 0);
    });
    engine->RegisterCommand("setoption", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // setoption <option>{,<flag>{,<char id>}}: 1 adds, 0 takes away; no option and no flag takes away the falcon and the Peco.  Only
        // those two are kept by the identity (pc_setoption's riding and falcon); a status's option comes and goes with its status
        Player* player = GetAttachedPlayer(context, "setoption");
        if (player == nullptr)
            return ROWOWScriptValue::FromNumber(0);
        int64 option = GetArgument(arguments, 0, 0);
        bool isAdded = GetArgument(arguments, 1, 1) != 0;
        if (arguments.size() < 2 && option == 0)
        {
            isAdded = false;
            option = ROWOW_OPTION_FALCON | ROWOW_OPTION_RIDING;
        }
        if ((option & ROWOW_OPTION_RIDING) != 0)
            PCSetRiding(player, isAdded);
        if ((option & ROWOW_OPTION_FALCON) != 0)
            PCSetFalcon(player, isAdded);
        if ((option & ~int64(ROWOW_OPTION_RIDING | ROWOW_OPTION_FALCON)) != 0)
            LogStubCommandOnce("setoption", "only the Peco and the falcon options can be set");
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("getinventorylist", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const&)
    {
        // getinventorylist{ <char id>}: the RO items in the bags, in bag order, as @inventorylist_* (id, idx, amount, equip, refine, identify,
        // attribute, card1-4, expire, bound, tradable) and @inventorylist_count.  idx is the item's place in that order
        Player* player = GetAttachedPlayer(context, "getinventorylist");
        if (player == nullptr)
            return ROWOWScriptValue::FromNumber(0);
        ROWOWIdentity* identity = context.Identity;
        uint32 count = 0;
        auto setListValue = [&context, &count](std::string const& name, int64 value)
        {
            ROWOWSCRIPT->SetVariable(context, "@inventorylist_" + name, count, ROWOWScriptValue::FromNumber(value));
        };
        for (uint8 bag = 0; bag <= 4; bag++)
        {
            for (uint32 slot = 1; slot <= MAX_BAG_SIZE; slot++)
            {
                Item* item = GetItemByClientPosition(player, bag, slot);
                ROWOWItem const* roItem = GetROItem(item);
                if (roItem == nullptr)
                    continue;
                int64 equip = 0;
                for (auto const& itemGUIDByPosition : identity->ItemGUIDsByEquipPosition)
                    if (itemGUIDByPosition.second == item->GetGUID().GetCounter())
                        equip |= int64(itemGUIDByPosition.first);
                setListValue("id", roItem->ItemID);
                setListValue("idx", count);
                setListValue("amount", item->GetCount());
                setListValue("equip", equip);
                setListValue("refine", GetItemRefine(item));
                setListValue("identify", IsItemIdentified(item) == true ? 1 : 0);
                setListValue("attribute", IsItemBroken(item) == true ? 1 : 0);
                for (uint8 cardSlot = 0; cardSlot < 4; cardSlot++)
                    setListValue(fmt::format("card{}", cardSlot + 1), GetItemCard(item, cardSlot));
                setListValue("expire", 0);
                setListValue("bound", 0);
                setListValue("tradable", 1);
                count++;
            }
        }
        ROWOWSCRIPT->SetVariable(context, "@inventorylist_count", 0, ROWOWScriptValue::FromNumber(count));
        return ROWOWScriptValue::FromNumber(0);
    });

    // The attached player (st->rid): a run can take another player (attachrid, by its account id, rAthena's block id) or let go of it.
    // Another player is only safe to touch where its thread isn't running: from the world update (maps idle), or on the run's own map
    engine->RegisterCommand("attachrid", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // attachrid(<account id>{,<force>}): 1 when attached.  Without force a player in a dialog isn't taken
        WorldSession* session = sWorldSessionMgr->FindSession(uint32(GetArgument(arguments, 0, 0)));
        Player* player = session != nullptr ? session->GetPlayer() : nullptr;
        if (player == nullptr || player->IsInWorld() == false || IsInROForm(player) == false)
            return ROWOWScriptValue::FromNumber(0);
        bool isForced = GetArgument(arguments, 1, 1) != 0;
        if (isForced == false && GetIdentity(player)->NpcRun != nullptr)
            return ROWOWScriptValue::FromNumber(0);
        // The run's map: the attached player's, or the last one's after a detachrid (marriage.txt detaches the groom, then attaches the bride again)
        Map const* runMap = context.AttachedPlayer != nullptr ? context.AttachedPlayer->GetMap() : context.RunMap;
        if (IsInNpcWorldUpdate == false && (runMap == nullptr || runMap != player->GetMap()))
        {
            LogStubCommandOnce("attachrid", "a player on another map can only be attached from the world update");
            return ROWOWScriptValue::FromNumber(0);
        }
        SetupNpcRunContext(context, player, context.Npc);
        if (context.Dialog == nullptr)
            context.Dialog = &WindowScriptDialog;
        return ROWOWScriptValue::FromNumber(1);
    });
    engine->RegisterCommand("detachrid", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const&)
    {
        // detachrid: the run goes on with no player
        context.AttachedPlayer = nullptr;
        context.Identity = nullptr;
        context.CharacterVariables = nullptr;
        context.PlayerVariables = nullptr;
        context.AccountVariables = nullptr;
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("getattachedrid", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const&)
    {
        return ROWOWScriptValue::FromNumber(context.AttachedPlayer != nullptr ? int64(context.AttachedPlayer->GetSession()->GetAccountId()) : 0);
    });
    engine->RegisterCommand("isloggedin", [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        // isloggedin(<account id>{,<char id>})
        WorldSession* session = sWorldSessionMgr->FindSession(uint32(GetArgument(arguments, 0, 0)));
        Player* player = session != nullptr ? session->GetPlayer() : nullptr;
        if (player != nullptr && arguments.size() > 1 && int64(player->GetGUID().GetCounter()) != arguments[1].GetNumber())
            player = nullptr;
        return ROWOWScriptValue::FromNumber(player != nullptr ? 1 : 0);
    });
    engine->RegisterCommand("rid2name", [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        // rid2name(<id>): a player's name by its account id ("(null)" for none; monsters and NPCs have no such ids here)
        WorldSession* session = sWorldSessionMgr->FindSession(uint32(GetArgument(arguments, 0, 0)));
        Player* player = session != nullptr ? session->GetPlayer() : nullptr;
        return ROWOWScriptValue::FromText(player != nullptr ? player->GetName() : std::string("(null)"));
    });

    // The players in a box of a map: only the run's own map can be touched from a map's thread, any loaded map from the world update
    auto forEachPlayerInArea = [this](ROWOWScriptContext& context, std::string const& mapName, int32 x0, int32 y0, int32 x1, int32 y1,
        std::function<void(Player*)> const& action)
    {
        uint32 mapID = ResolveMapArea(mapName, x0, y0, x1, y1);
        Map* map = nullptr;
        if (IsInNpcWorldUpdate == true)
            map = sMapMgr->FindBaseNonInstanceMap(mapID);
        else if (context.AttachedPlayer != nullptr && context.AttachedPlayer->GetMapId() == mapID)
            map = context.AttachedPlayer->GetMap();
        if (map == nullptr)
            return;
        std::vector<Player*> players;
        // A box still -1 is the whole map (a map that isn't a part: a part's whole map is its own box, from ResolveMapArea)
        bool isWholeMap = x0 < 0;
        map->DoForAllPlayers([this, x0, y0, x1, y1, isWholeMap, &players](Player* player)
        {
            int32 x = 0, y = 0;
            GetUnitCell(player, x, y);
            if (IsInROForm(player) == true && (isWholeMap == true || (x >= std::min(x0, x1) && x <= std::max(x0, x1) && y >= std::min(y0, y1) && y <= std::max(y0, y1))))
                players.push_back(player);
        });
        for (Player* player : players)
            action(player);
    };
    engine->RegisterCommand("areapercentheal", [this, forEachPlayerInArea](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // areapercentheal "<map>",<x1>,<y1>,<x2>,<y2>,<hp %>,<sp %>
        int32 hp = int32(GetArgument(arguments, 5, 0));
        int32 sp = int32(GetArgument(arguments, 6, 0));
        forEachPlayerInArea(context, GetTextArgument(arguments, 0), int32(GetArgument(arguments, 1, 0)), int32(GetArgument(arguments, 2, 0)),
            int32(GetArgument(arguments, 3, 0)), int32(GetArgument(arguments, 4, 0)), [this, hp, sp](Player* player) { PCPercentHeal(player, hp, sp); });
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("areaannounce", [forEachPlayerInArea](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // areaannounce "<map>",<x1>,<y1>,<x2>,<y2>,"<text>",<flag>{,...}: the color and font aren't kept
        std::string text = GetTextArgument(arguments, 5);
        forEachPlayerInArea(context, GetTextArgument(arguments, 0), int32(GetArgument(arguments, 1, 0)), int32(GetArgument(arguments, 2, 0)),
            int32(GetArgument(arguments, 3, 0)), int32(GetArgument(arguments, 4, 0)), [&text](Player* player) { ChatHandler(player->GetSession()).SendSysMessage(text); });
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("pushpc", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // pushpc <direction>,<cells>: unit_blown of the attached player that many cells in the direction (backwards for negative cells)
        Player* player = context.AttachedPlayer;
        int32 dir = int32(GetArgument(arguments, 0, 0)) % 8;
        int32 cells = int32(GetArgument(arguments, 1, 0));
        if (player == nullptr || cells == 0)
            return ROWOWScriptValue::FromNumber(0);
        if (cells < 0)
        {
            dir = (dir + 4) % 8;
            cells = -cells;
        }
        SkillBlown(player, player, cells, int8((dir + 4) % 8), 0); // skill_blown pushes away from the direction it's given
        return ROWOWScriptValue::FromNumber(0);
    });

    // Looks and sounds (R9.5): emotions over a unit, RO's effects and sounds, each the converter's hidden spell the unit casts on itself
    // (PlayScriptLook).  The unit is found on the run's map: the attached player's (the run is in its map's thread), else, with no player,
    // the NPC's (the run is in the world update).  A look the converter didn't make (an effect RoEffects.csv hasn't got) shows nothing
    auto getRunMap = [](ROWOWScriptContext& context, uint32 mapID) -> Map*
    {
        if (context.AttachedPlayer != nullptr)
            return context.AttachedPlayer->IsInWorld() == true && context.AttachedPlayer->GetMapId() == mapID ? context.AttachedPlayer->GetMap() : nullptr;
        return mapID != 0 ? sMapMgr->FindBaseNonInstanceMap(mapID) : nullptr;
    };
    auto getNpcUnit = [getRunMap](ROWOWScriptContext& context, ROWOWNpc const* npc) -> Unit*
    {
        // The NPC's creature (rAthena's npc_data), on the run's map
        if (npc == nullptr)
            return nullptr;
        Map* map = getRunMap(context, npc->MapID);
        if (map == nullptr)
            return nullptr;
        for (ObjectGuid::LowType spawnID : npc->CreatureSpawnIDs)
        {
            auto creatureRange = map->GetCreatureBySpawnIdStore().equal_range(spawnID);
            if (creatureRange.first != creatureRange.second && creatureRange.first->second->IsInWorld() == true)
                return creatureRange.first->second;
        }
        return nullptr;
    };
    auto getEffectName = [engine](ROWOWScriptValue const& argument) -> std::string
    {
        // EF_<name> is a symbol (the engine has no numbers for them): the look goes by the name less EF_
        std::string symbolName = argument.IsString == true ? argument.Text : engine->GetSymbolName(argument.GetNumber());
        if (symbolName.size() > 3 && (symbolName.compare(0, 3, "EF_") == 0 || symbolName.compare(0, 3, "ef_") == 0))
            symbolName = symbolName.substr(3);
        std::transform(symbolName.begin(), symbolName.end(), symbolName.begin(), [](unsigned char c) { return char(std::toupper(c)); });
        return symbolName;
    };
    engine->RegisterCommand("emotion", [this, getNpcUnit](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // emotion <emotion>{,<game ID>}: over the NPC, or the unit of the game ID: the attached player (playerattached(), getcharid(3)) or
        // another NPC (getnpcid)
        int32 type = int32(GetArgument(arguments, 0, -1));
        Unit* unit = nullptr;
        if (arguments.size() > 1)
        {
            int64 gameID = arguments[1].GetNumber();
            if (context.AttachedPlayer != nullptr && gameID == int64(context.AttachedPlayer->GetSession()->GetAccountId()))
                unit = context.AttachedPlayer;
            else
                unit = getNpcUnit(context, GetNpc(uint32(std::max<int64>(0, gameID))));
        }
        else
            unit = getNpcUnit(context, context.Npc);
        ClifEmotion(unit, type);
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("specialeffect", [this, getNpcUnit, getEffectName, getNpc](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // specialeffect <effect>{,<send target>{,"<NPC>"}}: on the NPC (or the one named).  Every send target shows it to the area, as the
        // client's spell does (SELF's view only by the attached player isn't kept).  An item's script (no NPC) shows it on its user, as
        // ROWOW_Items.cpp's registration (which this one replaces, both being the one engine's) did
        if (arguments.empty() == true)
            return ROWOWScriptValue::FromNumber(0);
        Unit* unit = context.Npc == nullptr && arguments.size() <= 2 ? static_cast<Unit*>(context.AttachedPlayer) : getNpcUnit(context, getNpc(context, arguments, 2));
        PlayScriptLook(unit, ROWOW_SCRIPT_LOOK_EFFECT, getEffectName(arguments[0]));
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("specialeffect2", [this, getEffectName](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // specialeffect2 <effect>{,<send target>{,"<player>"}}: on the attached player
        if (arguments.empty() == false && context.AttachedPlayer != nullptr)
            PlayScriptLook(context.AttachedPlayer, ROWOW_SCRIPT_LOOK_EFFECT, getEffectName(arguments[0]));
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("soundeffect", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // soundeffect "<file>",<type>: the sound at the attached player (heard around it too: the client plays the spell's sound)
        if (context.AttachedPlayer != nullptr)
            PlayScriptLook(context.AttachedPlayer, ROWOW_SCRIPT_LOOK_SOUND, ToLower(GetTextArgument(arguments, 0)));
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("soundeffectall", [this, getNpcUnit, forEachPlayerInArea](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // soundeffectall "<file>",<type>{,"<map>"{,<x0>,<y0>,<x1>,<y1>}}: around the attached player, else the NPC; with a map, every player
        // on it (or in the area of it) hears it where it is (rAthena's map_foreachinmap/map_foreachinallarea of soundeffect_sub).  A map other
        // than the run's is only reached from the world update (forEachPlayerInArea)
        std::string fileName = ToLower(GetTextArgument(arguments, 0));
        if (arguments.size() >= 3)
        {
            bool isArea = arguments.size() >= 7;
            // No box: -1, the whole map (a stitched map's part: its own cells)
            int32 x0 = isArea == true ? int32(GetArgument(arguments, 3, 0)) : -1;
            int32 y0 = isArea == true ? int32(GetArgument(arguments, 4, 0)) : -1;
            int32 x1 = isArea == true ? int32(GetArgument(arguments, 5, 0)) : -1;
            int32 y1 = isArea == true ? int32(GetArgument(arguments, 6, 0)) : -1;
            forEachPlayerInArea(context, GetTextArgument(arguments, 2), x0, y0, x1, y1, [this, &fileName](Player* player) { PlayScriptSoundToPlayer(player, fileName); });
            return ROWOWScriptValue::FromNumber(0);
        }
        Unit* unit = context.AttachedPlayer != nullptr ? static_cast<Unit*>(context.AttachedPlayer) : getNpcUnit(context, context.Npc);
        PlayScriptLook(unit, ROWOW_SCRIPT_LOOK_SOUND, fileName);
        return ROWOWScriptValue::FromNumber(0);
    });

    engine->RegisterCommand("viewpoint", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // viewpoint <action>,<x>,<y>,<point number>,<color>: a mark on the minimap (action 2 takes it off).  The client has one point of interest
        // (SMSG_GOSSIP_POI), so the last one shows, not one per point number or in its color
        Player* player = GetAttachedPlayer(context, "viewpoint");
        if (player == nullptr)
            return ROWOWScriptValue::FromNumber(0);
        float x = 0, y = 0;
        bool isOff = GetArgument(arguments, 0, 0) == 2;
        if (isOff == false)
        {
            // The script's cells are its map's: on a stitched map, the part of the NPC talked to (the player may stand just over the gap from
            // it), else the part the player is in
            int32 cellX = int32(GetArgument(arguments, 1, 0));
            int32 cellY = int32(GetArgument(arguments, 2, 0));
            ROWOWMapPart const* part = context.Npc != nullptr && context.Npc->MapID == player->GetMapId() ? GetMapPart(context.Npc->MapName) : nullptr;
            if (part == nullptr)
            {
                int32 playerX = 0, playerY = 0;
                GetUnitCell(player, playerX, playerY);
                part = GetMapPartAt(player->GetMapId(), playerX, playerY);
            }
            if (part != nullptr && part->HasArea() == true)
            {
                cellX += part->CellOffsetX;
                cellY += part->CellOffsetY;
            }
            GetCellWorldPosition(player->GetMapId(), cellX, cellY, x, y);
        }
        WorldPacket data(SMSG_GOSSIP_POI, 4 + 4 + 4 + 4 + 4 + 2);
        data << uint32(isOff == true ? 0 : 6);
        data << float(x);
        data << float(y);
        data << uint32(7); // The cross on the map
        data << uint32(0);
        data << std::string(context.Npc != nullptr ? context.Npc->Name : "");
        player->SendDirectMessage(&data);
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("misceffect", [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const&) { return ROWOWScriptValue::FromNumber(0); });
    engine->SetStub("misceffect");

    // Commands of systems that aren't here: the runs go on past them.  setunitdata and areamobuseskill are only in battlegrounds and instances,
    // openmail and openauction ask for windows an identity has none of, and freeloop only lifts the loop limit of guild castle scripts
    for (char const* unusedCommandName : { "setunitdata", "areamobuseskill", "openmail", "openauction", "freeloop" })
    {
        engine->RegisterCommand(unusedCommandName, [](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const&) { return ROWOWScriptValue::FromNumber(0); });
        engine->SetStub(unusedCommandName);
    }
    engine->RegisterCommand("getnameditem", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // getnameditem <item>,"<character name>" or <char id>: an item with the name of its maker (card[0] 254), which must be online; 1 when given
        Player* player = GetAttachedPlayer(context, "getnameditem");
        if (player == nullptr || arguments.size() < 2)
            return ROWOWScriptValue::FromNumber(0);
        auto itemTemplateIDIter = ItemTemplateIDsByItemID.find(GetItemIDOfScriptValue(arguments[0]));
        Player* named = arguments[1].IsString == true ? ObjectAccessor::FindPlayerByName(arguments[1].Text, false) :
            ObjectAccessor::FindPlayerByLowGUID(ObjectGuid::LowType(arguments[1].GetNumber()));
        if (itemTemplateIDIter == ItemTemplateIDsByItemID.end() || named == nullptr)
            return ROWOWScriptValue::FromNumber(0);
        return ROWOWScriptValue::FromNumber(StoreROItem(player, itemTemplateIDIter->second, 1, nullptr, named->GetGUID().GetCounter()) != nullptr ? 1 : 0);
    });
    engine->RegisterCommand("getitem2", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // getitem2 <item>,<amount>,<identify>,<refine>,<attribute>,<card1>,<card2>,<card3>,<card4>: an item with its refine, broken state and cards
        Player* player = GetAttachedPlayer(context, "getitem2");
        if (player == nullptr || arguments.size() < 9)
            return ROWOWScriptValue::FromNumber(0);
        uint32 itemID = GetItemIDOfScriptValue(arguments[0]);
        Item* item = nullptr;
        if (GiveROItem(player, itemID, uint32(std::max<int64>(1, GetArgument(arguments, 1, 1))), GetArgument(arguments, 2, 1) != 0, &item) == false || item == nullptr)
            return ROWOWScriptValue::FromNumber(0);
        SetItemState(player, item, uint32(std::clamp<int64>(GetArgument(arguments, 3, 0), 0, ROWOW_MAX_REFINE)), GetArgument(arguments, 4, 0) != 0, GetArgument(arguments, 2, 1) != 0);
        for (uint8 slot = 0; slot < ROWOW_MAX_SLOTS; slot++)
            if (uint32 cardItemID = uint32(GetArgument(arguments, 5 + slot, 0)))
                SetItemCard(player, item, slot, cardItemID);
        SendItemCardsToClient(player, *GetIdentity(player), true);
        return ROWOWScriptValue::FromNumber(0);
    });
    auto findExactItems = [this](Player* player, std::vector<ROWOWScriptValue> const& arguments, size_t firstField)
    {
        // countitem2's and delitem2's match: <item>,<identify>,<refine>,<attribute>,<card1..4>, after their amount for delitem2.  card1 254
        // (CARD0_CREATE) with card3 and card4 the maker's id is the maker's name; other cards aren't kept by these scripts
        std::vector<Item*> items;
        auto itemTemplateIDIter = ItemTemplateIDsByItemID.find(GetItemIDOfScriptValue(arguments[0]));
        if (itemTemplateIDIter == ItemTemplateIDsByItemID.end() || player == nullptr)
            return items;
        ObjectGuid::LowType creatorGUID = 0;
        if (GetArgument(arguments, firstField + 3, 0) == 254)
            creatorGUID = ObjectGuid::LowType(uint32(GetArgument(arguments, firstField + 5, 0)) | (uint32(GetArgument(arguments, firstField + 6, 0)) << 16));
        for (uint8 bag = 0; bag <= 4; bag++)
        {
            for (uint32 slot = 1; slot <= MAX_BAG_SIZE; slot++)
            {
                Item* item = GetItemByClientPosition(player, bag, slot);
                if (item != nullptr && item->GetEntry() == itemTemplateIDIter->second && IsItemIdentified(item) == (GetArgument(arguments, firstField, 1) != 0) &&
                    int64(GetItemRefine(item)) == GetArgument(arguments, firstField + 1, 0) && IsItemBroken(item) == (GetArgument(arguments, firstField + 2, 0) != 0) &&
                    item->GetGuidValue(ITEM_FIELD_CREATOR).GetCounter() == creatorGUID)
                    items.push_back(item);
            }
        }
        return items;
    };
    engine->RegisterCommand("countitem2", [this, findExactItems](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        int64 count = 0;
        if (arguments.size() >= 8)
            for (Item* item : findExactItems(GetAttachedPlayer(context, "countitem2"), arguments, 1))
                count += item->GetCount();
        return ROWOWScriptValue::FromNumber(count);
    });
    engine->RegisterCommand("delitem2", [this, findExactItems](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // Stops the run when there aren't enough, as delitem
        Player* player = GetAttachedPlayer(context, "delitem2");
        if (player == nullptr || arguments.size() < 9)
            return ROWOWScriptValue::FromNumber(0);
        uint32 amount = uint32(std::max<int64>(1, GetArgument(arguments, 1, 1)));
        std::vector<Item*> items = findExactItems(player, arguments, 2);
        uint32 held = 0;
        for (Item* item : items)
            held += item->GetCount();
        if (held < amount)
        {
            context.Error = "not enough items to delete";
            context.IsStopped = true;
            return ROWOWScriptValue::FromNumber(0);
        }
        for (Item* item : items)
        {
            uint32 count = std::min(amount, item->GetCount());
            amount -= count;
            player->DestroyItemCount(item, count, true);
            if (amount == 0)
                break;
        }
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("consumeitem", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // consumeitem <item>: the item's script, without having one
        Player* player = GetAttachedPlayer(context, "consumeitem");
        if (player == nullptr || arguments.empty() == true)
            return ROWOWScriptValue::FromNumber(0);
        auto itemTemplateIDIter = ItemTemplateIDsByItemID.find(GetItemIDOfScriptValue(arguments[0]));
        ROWOWItem const* roItem = itemTemplateIDIter != ItemTemplateIDsByItemID.end() ? GetROItemByItemTemplateID(itemTemplateIDIter->second) : nullptr;
        if (roItem == nullptr || roItem->UseScript == nullptr || roItem->UseScript->IsRunnable() == false)
            return ROWOWScriptValue::FromNumber(0);
        ROWOWScriptContext itemContext;
        itemContext.AttachedPlayer = player;
        itemContext.Identity = GetIdentity(player);
        itemContext.UsedItemTemplateID = itemTemplateIDIter->second;
        ROWOWSCRIPT->Run(*roItem->UseScript, itemContext);
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("convertpcinfo", [this](ROWOWScriptContext&, std::vector<ROWOWScriptValue> const& arguments)
    {
        // convertpcinfo(<char id or account id or "name">,<CPC_*>): an online character's name (0), char id (1) or account id (2).  0 (or "") offline
        if (arguments.size() < 2)
            return ROWOWScriptValue::FromNumber(0);
        Player* player = arguments[0].IsString == true ? ObjectAccessor::FindPlayerByName(arguments[0].Text, false) :
            ObjectAccessor::FindPlayerByLowGUID(ObjectGuid::LowType(arguments[0].GetNumber()));
        if (player == nullptr && arguments[0].IsString == false)
            if (WorldSession* session = sWorldSessionMgr->FindSession(uint32(arguments[0].GetNumber())))
                player = session->GetPlayer();
        int64 type = arguments[1].GetNumber();
        if (player == nullptr)
            return type == 0 ? ROWOWScriptValue::FromText(std::string()) : ROWOWScriptValue::FromNumber(0);
        switch (type)
        {
            case 0: return ROWOWScriptValue::FromText(player->GetName());
            case 1: return ROWOWScriptValue::FromNumber(int64(player->GetGUID().GetCounter()));
            default: return ROWOWScriptValue::FromNumber(int64(player->GetSession()->GetAccountId()));
        }
    });
    auto getStatIndex = [](std::vector<ROWOWScriptValue> const& arguments) -> int64
    {
        // A base stat parameter (bStr..bLuk), a symbol to the engine
        static char const* const StatParameterNames[ROWOW_STAT_COUNT] = { "bStr", "bAgi", "bVit", "bInt", "bDex", "bLuk" };
        std::string name = arguments.empty() == false ? ROWOWSCRIPT->GetSymbolName(arguments[0].Number) : std::string();
        for (int64 statIndex = 0; statIndex < ROWOW_STAT_COUNT; statIndex++)
            if (name == StatParameterNames[statIndex])
                return statIndex;
        return -1;
    };
    engine->RegisterCommand("needed_status_point", [this, getStatIndex](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // needed_status_point(<type>,<val>): the status points it takes to raise (or lower) a stat
        Player* player = GetAttachedPlayer(context, "needed_status_point");
        int64 statIndex = getStatIndex(arguments);
        if (player == nullptr || statIndex < 0)
            return ROWOWScriptValue::FromNumber(0);
        return ROWOWScriptValue::FromNumber(PCNeedStatusPoint(*GetIdentity(player), uint8(statIndex), int32(GetArgument(arguments, 1, 0))));
    });
    engine->RegisterCommand("statusup2", [this, getStatIndex](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // statusup2 <type>,<amount>: a stat raised (or lowered) with no status points paid (pc_statusup2)
        Player* player = GetAttachedPlayer(context, "statusup2");
        int64 statIndex = getStatIndex(arguments);
        if (player == nullptr || statIndex < 0)
            return ROWOWScriptValue::FromNumber(0);
        ROWOWIdentity* identity = GetIdentity(player);
        identity->Stats[statIndex] = uint32(std::clamp<int64>(int64(identity->Stats[statIndex]) + GetArgument(arguments, 1, 0), 1, 32767));
        RefreshROForm(player);
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("unitskilluseid", [this](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // unitskilluseid <game ID>,<skill>,<level>{,<target>...}: the attached player uses the skill on itself (the one use of it: a cash item's Endure)
        Player* player = GetAttachedPlayer(context, "unitskilluseid");
        if (player == nullptr || arguments.size() < 3)
            return ROWOWScriptValue::FromNumber(0);
        uint16 skillID = uint16(arguments[1].IsString == true ? GetSkillIDByName(arguments[1].Text) : arguments[1].GetNumber());
        SkillCastEnd(player, player, nullptr, skillID, uint16(GetArgument(arguments, 2, 1)));
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("warpparty", [this, queueMapWarp](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // warpparty "<to map>",<x>,<y>,<party id>{,"<from map>",<x2>,<y2>}: the attached player's party on the map named, or on its own map, at the world
        // update.  "SavePointAll" and "SavePoint" aren't told from a real map's warp: each goes to its own save point through warp's names
        Player* player = GetAttachedPlayer(context, "warpparty");
        Group* group = player != nullptr ? player->GetGroup() : nullptr;
        if (group == nullptr)
            return ROWOWScriptValue::FromNumber(0);
        ROWOWNpcMapWarp mapWarp;
        std::string fromMap = GetTextArgument(arguments, 4);
        mapWarp.MapID = fromMap.empty() == false ? ResolveMapArea(fromMap, mapWarp.X1, mapWarp.Y1, mapWarp.X2, mapWarp.Y2) : player->GetMapId(); // A part: its own cells
        mapWarp.DestMapName = GetTextArgument(arguments, 0);
        mapWarp.DestX = int32(GetArgument(arguments, 1, 0));
        mapWarp.DestY = int32(GetArgument(arguments, 2, 0));
        mapWarp.GroupGUID = group->GetGUID();
        mapWarp.RequesterGUID = player->GetGUID();
        if (mapWarp.MapID != 0)
            queueMapWarp(mapWarp);
        return ROWOWScriptValue::FromNumber(0);
    });
    engine->RegisterCommand("sscanf", [](ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)
    {
        // sscanf(<string>,<format>{,<variable>...}): the values of %d and %s, each into its variable; how many were read
        if (arguments.size() < 2)
            return ROWOWScriptValue::FromNumber(0);
        std::string text = arguments[0].GetText();
        std::string format = arguments[1].GetText();
        size_t textIndex = 0, argument = 2;
        int64 readCount = 0;
        for (size_t formatIndex = 0; formatIndex < format.size(); formatIndex++)
        {
            if (format[formatIndex] != '%' || formatIndex + 1 >= format.size())
            {
                if (isspace((unsigned char)format[formatIndex]) != 0)
                {
                    while (textIndex < text.size() && isspace((unsigned char)text[textIndex]) != 0)
                        textIndex++;
                    continue;
                }
                if (textIndex >= text.size() || text[textIndex] != format[formatIndex])
                    break;
                textIndex++;
                continue;
            }
            char specifier = format[++formatIndex];
            while (textIndex < text.size() && isspace((unsigned char)text[textIndex]) != 0)
                textIndex++;
            size_t start = textIndex;
            if (specifier == 'd')
            {
                if (textIndex < text.size() && (text[textIndex] == '-' || text[textIndex] == '+'))
                    textIndex++;
                while (textIndex < text.size() && isdigit((unsigned char)text[textIndex]) != 0)
                    textIndex++;
            }
            else
            {
                while (textIndex < text.size() && isspace((unsigned char)text[textIndex]) == 0)
                    textIndex++;
            }
            if (start == textIndex || argument >= arguments.size())
                break;
            std::string value = text.substr(start, textIndex - start);
            ROWOWSCRIPT->SetReferenceValue(context, arguments[argument++], specifier == 'd' ? ROWOWScriptValue::FromNumber(atoll(value.c_str())) : ROWOWScriptValue::FromText(value));
            readCount++;
        }
        return ROWOWScriptValue::FromNumber(readCount);
    });
    engine->SetReferenceArguments("sscanf", 0xFFFFFFFC);
}

Creature* ROWOWMod::SpawnScriptMob(Map* map, uint32 mobID, int32 x, int32 y, std::string const& name, std::string const& event, ROWOWMapPart const* part)
{
    // mob_once_spawn: the monster on the cell (a random one of the map for -1: map_search_freecell(-1, -1), xs * ys tries, 500 at most; of a stitched
    // map's part, of the part), kept until killed or taken away, with the script's name
    uint32 entry = GetMobCreatureEntry(mobID);
    if (map == nullptr || entry == 0)
        return nullptr;
    if (x < 0 || y < 0)
    {
        ROWOWMapGrid const* mapGrid = GetMapGrid(map->GetId());
        if (mapGrid == nullptr || mapGrid->HasCells() == false)
            return nullptr;
        if (part != nullptr && part->HasArea() == false)
            part = nullptr;
        int32 tries = std::min(part != nullptr ? part->CellWidth * part->CellHeight : mapGrid->Width * mapGrid->Height, 500);
        if (MapSearchFreeCellOfSpawn(*mapGrid, map, -1, -1, -1, -1, tries, x, y, part) == false)
            return nullptr;
    }
    ROWOWPosition position;
    if (GetCellPosition(map->GetId(), x, y, position) == false)
        return nullptr;
    TempSummon* creature = map->SummonCreature(entry, Position(position.X, position.Y, position.Z, float(urand(0, 7)) * float(M_PI) / 4.0f));
    if (creature == nullptr)
        return nullptr;

    // mob_parse_dataset's name: "--en--" is mob_db's name (the template's), "--ja--" its Japanese name, anything else the script's own
    // (override_mob_names is off).  WoW names a creature by its template, so a name of its own goes by the pet name query: the creature
    // carries a pet number, the client asks for that number's name (CMSG_PET_NAME_QUERY) and the server answers with the creature's own
    // name (SendPetNameQuery: GetName).  The number is the GUID's counter with the top bit (players' pet numbers count up from 1), the
    // timestamp tells the client a cached name for a reused number is stale
    std::string shownName = name;
    if (name == "--en--")
        shownName.clear();
    else if (name == "--ja--")
    {
        ROWOWDataTable const* mobTable = GetDataTable(ROWOW_DATA_TABLE_MOB);
        int64 mobRowIndex = GetMobRowIndex(mobID);
        shownName = mobTable != nullptr && mobRowIndex >= 0 ? mobTable->GetCell(size_t(mobRowIndex), mobTable->GetColumnIndex("JapaneseName")) : std::string();
    }
    if (shownName.empty() == false && shownName != creature->GetName())
    {
        creature->SetName(shownName);
        creature->SetUInt32Value(UNIT_FIELD_PETNUMBER, 0x80000000 | (creature->GetGUID().GetCounter() & 0x7FFFFFFF));
        creature->SetUInt32Value(UNIT_FIELD_PET_NAME_TIMESTAMP, uint32(GameTime::GetGameTime().count()));
    }
    ROWOWScriptMob scriptMob;
    scriptMob.MapID = GetMapKey(map->GetId(), x, y); // A part of a stitched map: mobcount and killmonster by its name are its own
    scriptMob.Event = event;
    std::lock_guard<std::mutex> lock(NpcStateMutex);
    ScriptMobsByGUID[creature->GetGUID()] = scriptMob;
    return creature;
}

bool ROWOWMod::OnScriptMobDeath(Creature* creature, Unit* killer)
{
    // The monster's event (OnMyMobDead and the like) runs now, on its map's thread, with the player who killed it attached (mob_dead's
    // npc_event with the killer's sd, after its run if it's in one).  True when it had an event
    std::string event;
    {
        std::lock_guard<std::mutex> lock(NpcStateMutex);
        auto scriptMobIter = ScriptMobsByGUID.find(creature->GetGUID());
        if (scriptMobIter == ScriptMobsByGUID.end())
            return false;
        event = scriptMobIter->second.Event;
        ScriptMobsByGUID.erase(scriptMobIter);
    }
    size_t separatorPosition = event.find("::");
    if (separatorPosition == std::string::npos)
        return false;
    ROWOWNpc const* npc = GetNpcByName(event.substr(0, separatorPosition));
    if (npc == nullptr || npc->Script == nullptr)
        return true;
    int64 labelIndex = npc->Script->GetLabel(event.substr(separatorPosition + 2));
    if (labelIndex < 0)
        return true;
    Player* killerPlayer = killer != nullptr ? killer->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
    if (killerPlayer != nullptr && killerPlayer->GetMap() == creature->GetMap() && IsInROForm(killerPlayer) == true)
        RunPlayerNpcEvent(killerPlayer, npc, uint32(labelIndex));
    else
        QueueNpcEvent(npc, event.substr(separatorPosition + 2));
    return true;
}
