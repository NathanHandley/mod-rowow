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

#ifndef ROWOW_SCRIPT_H
#define ROWOW_SCRIPT_H

#include "Common.h"
#include "ObjectGuid.h"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

class Item;
class Player;
class Unit;
struct ROWOWIdentity;
struct ROWOWNpc;
struct ROWOWStatus;

// The RO script engine: rAthena's script language (script.cpp), for item scripts and NPC scripts.  A script is compiled once at startup
// into a small bytecode, which runs on a value stack with the state kept in ROWOWScriptContext, so a run can stop and pick up again (an
// NPC dialog waiting on the player)

// A value: rAthena's script values are numbers or strings.  A variable handed to a command that reads or writes it by name (input .@n,
// setarray .@a[0], getarraysize(.@a)) is a reference: its name in Text, its array index in Number
struct ROWOWScriptValue
{
    bool IsString = false;
    bool IsReference = false;
    int64 Number = 0;
    std::string Text;

    static ROWOWScriptValue FromNumber(int64 number) { ROWOWScriptValue value; value.Number = number; return value; }
    static ROWOWScriptValue FromText(std::string const& text) { ROWOWScriptValue value; value.IsString = true; value.Text = text; return value; }
    static ROWOWScriptValue FromReference(std::string const& name, int64 index) { ROWOWScriptValue value; value.IsReference = true; value.Text = name; value.Number = index; return value; }
    int64 GetNumber() const; // A string's leading number, as rAthena's conv_num
    std::string GetText() const;
};

// Constants rAthena's scripts name but the engine has no number for (SC_*, EF_*, IG_*, the bonus types like bStr) become symbols: numbers
// from here up, which the commands that take them turn back into their names
#define ROWOW_SCRIPT_SYMBOL_BASE                    (int64(1) << 40)

enum ROWOWScriptOp : uint8
{
    ROWOW_SCRIPT_OP_PUSH_NUMBER,        // Number
    ROWOW_SCRIPT_OP_PUSH_STRING,        // Argument: the string
    ROWOW_SCRIPT_OP_PUSH_VARIABLE,      // Argument: the variable's name
    ROWOW_SCRIPT_OP_PUSH_ARRAY_VARIABLE,// Argument: the array's name; pops the index
    ROWOW_SCRIPT_OP_STORE_VARIABLE,     // Argument: the variable's name; pops the value
    ROWOW_SCRIPT_OP_STORE_ARRAY_VARIABLE, // Argument: the array's name; pops the value, then the index
    ROWOW_SCRIPT_OP_POP,
    ROWOW_SCRIPT_OP_UNARY,              // Argument: the operator (ROWOW_SCRIPT_OPERATOR_*)
    ROWOW_SCRIPT_OP_BINARY,             // Argument: the operator
    ROWOW_SCRIPT_OP_JUMP,               // Argument: where to
    ROWOW_SCRIPT_OP_JUMP_IF_FALSE,      // Argument: where to; pops the condition
    ROWOW_SCRIPT_OP_JUMP_IF_TRUE,       // Argument: where to; pops the condition
    ROWOW_SCRIPT_OP_CALL,               // Argument: the command, or -1 - the name's string for one the engine doesn't have; Number: how many
                                        // arguments it pops.  Pushes its result
    ROWOW_SCRIPT_OP_END,
    ROWOW_SCRIPT_OP_PUSH_REFERENCE,     // Argument: the variable's name, as a command's variable argument
    ROWOW_SCRIPT_OP_PUSH_ARRAY_REFERENCE, // Argument: the array's name; pops the index
    ROWOW_SCRIPT_OP_STORE_PARAMETER,    // Argument: the parameter (its command); pops the value (set Zeny, Zeny - 100)
    ROWOW_SCRIPT_OP_STORE_DYNAMIC_VARIABLE, // Pops the value, then the variable's name with any index (set getd("$name[2]"), 5)
    ROWOW_SCRIPT_OP_CALL_SUB,           // Argument: where to; Number: how many arguments it pops.  callsub and the script's own functions
    ROWOW_SCRIPT_OP_CALL_FUNCTION,      // Number: how many arguments it pops, the first being the function's name (callfunc)
    ROWOW_SCRIPT_OP_RETURN,             // Number: 1 when it pops the value to return
};

enum ROWOWScriptOperator : uint8
{
    ROWOW_SCRIPT_OPERATOR_NEGATE, ROWOW_SCRIPT_OPERATOR_NOT, ROWOW_SCRIPT_OPERATOR_BITWISE_NOT,
    ROWOW_SCRIPT_OPERATOR_ADD, ROWOW_SCRIPT_OPERATOR_SUBTRACT, ROWOW_SCRIPT_OPERATOR_MULTIPLY, ROWOW_SCRIPT_OPERATOR_DIVIDE,
    ROWOW_SCRIPT_OPERATOR_MODULO, ROWOW_SCRIPT_OPERATOR_EQUAL, ROWOW_SCRIPT_OPERATOR_NOT_EQUAL, ROWOW_SCRIPT_OPERATOR_LESS,
    ROWOW_SCRIPT_OPERATOR_LESS_EQUAL, ROWOW_SCRIPT_OPERATOR_GREATER, ROWOW_SCRIPT_OPERATOR_GREATER_EQUAL, ROWOW_SCRIPT_OPERATOR_BITWISE_AND,
    ROWOW_SCRIPT_OPERATOR_BITWISE_OR, ROWOW_SCRIPT_OPERATOR_BITWISE_XOR, ROWOW_SCRIPT_OPERATOR_SHIFT_LEFT, ROWOW_SCRIPT_OPERATOR_SHIFT_RIGHT,
};

struct ROWOWScriptInstruction
{
    ROWOWScriptOp Op = ROWOW_SCRIPT_OP_END;
    int32 Argument = 0;
    int64 Number = 0;
};

// A set of script variables (one scope).  rAthena's variable names aren't case sensitive, so they're kept in lowercase; every variable is
// an array, its element 0 being the plain variable.  An element set to 0 (or "" for a string variable, a name ending in $) is removed, as
// rAthena removes it
struct ROWOWScriptVariables
{
    std::unordered_map<std::string, std::map<uint32, ROWOWScriptValue>> ElementsByName;
    std::set<std::pair<std::string, uint32>> ChangedElements; // Set or removed since the last save, for the stores that are saved

    ROWOWScriptValue Get(std::string const& name, uint32 index) const;
    void Set(std::string const& name, uint32 index, ROWOWScriptValue const& value);
    uint32 GetArraySize(std::string const& name) const; // The highest index set, plus one (getarraysize)
    void Clear(); // Removes every element (noted as changed, so a saved store's rows go too)
};

// A compiled script.  One that calls a command the engine doesn't have yet (UnavailableCommands) is never run as an item's script, so an
// item whose script needs one can't be used, rather than being used up for part of its effect.  An NPC script runs up to such a command
struct ROWOWScript
{
    std::string SourceName; // What it came from, for the logs
    std::vector<ROWOWScriptInstruction> Code;
    std::vector<std::string> Strings; // String constants and variable names
    std::vector<std::string> UnavailableCommands;
    std::string Error; // Why it didn't compile, empty when it did
    std::unordered_map<std::string, uint32> LabelsByName; // Its labels (OnInit, L_Menu...), lowercase, at their instructions
    std::map<std::string, uint32> CommandUses; // How often each command, parameter and keyword is written in it (startup's count)
    mutable ROWOWScriptVariables LocalVariables; // Its NPC variables (.name): an NPC's, which its duplicates share, or a function's.  Only
                                                 // touched under the engine's VariableMutex

    bool IsRunnable() const { return Error.empty() == true && UnavailableCommands.empty() == true; }
    int64 GetLabel(std::string const& name) const; // Its instruction, or -1
};

// One run of a script: what it's attached to (rAthena's RID, the player), and its state
// potion_flag 1 (Aid Potion and the like throwing a potion): what the potion's itemheal and percentheal would heal, kept instead of healing,
// and the unit its statuses go to instead (potion_target, for half their time)
struct ROWOWPotionHeal
{
    int32 HP = 0; // potion_hp
    int32 SP = 0;
    int32 PerHP = 0; // potion_per_hp
    int32 PerSP = 0;
    Unit* Target = nullptr; // potion_target, null for none
};

// A function or callsub's frame (rAthena's script_stack scopes): where it returns to, its arguments and its own .@ variables
struct ROWOWScriptFrame
{
    ROWOWScript const* Script = nullptr;
    size_t ReturnIndex = 0; // Where the caller goes on
    size_t StackBase = 0; // The caller's stack height
    std::vector<ROWOWScriptValue> Arguments; // getarg
    ROWOWScriptVariables ScopeVariables; // .@name
};

// What a run waits for (rAthena's script states STOP and RERUNLINE, with the reason): the player's answer to the dialog, or time
enum ROWOWScriptWait : uint8
{
    ROWOW_SCRIPT_WAIT_NONE,
    ROWOW_SCRIPT_WAIT_NEXT,         // next: the Next button
    ROWOW_SCRIPT_WAIT_CLOSE,        // close2: the Close button, then it goes on
    ROWOW_SCRIPT_WAIT_CLOSE_END,    // close: the Close button, then it ends
    ROWOW_SCRIPT_WAIT_MENU,         // menu, select, prompt: a choice (npc_menu, 255 for cancel)
    ROWOW_SCRIPT_WAIT_INPUT,        // input: a number or a string
    ROWOW_SCRIPT_WAIT_SLEEP,        // sleep, sleep2: until SleepUntil
};

class ROWOWScriptDialog;

struct ROWOWScriptContext
{
    Player* AttachedPlayer = nullptr;
    ROWOWIdentity* Identity = nullptr;
    ObjectGuid UsedItemGUID; // The item being used, for an item's use script
    uint32 UsedItemTemplateID = 0;
    ROWOWStatus* BonusStatus = nullptr; // The status a worn item's script gives its bonuses to (status_calc_pc_sub), null otherwise
    uint8 BonusLRFlag = 0; // ROWOW_LR_FLAG_*: whose the bonuses are
    Item const* EquipItem = nullptr; // The worn item whose script (or card's, combo's) runs (current_equip_item_index)
    uint32 EquipPositions = 0; // Where it's worn (its equip, or current_equip_combo_pos for a combo), which an autobonus keeps
    uint32 EquipCardID = 0; // The card whose script runs (current_equip_card_id), 0 for none
    ROWOWPotionHeal* Potion = nullptr; // potion_flag 1: a thrown potion's heal is only noted here, null otherwise
    std::unordered_map<std::string, ROWOWScriptValue> Variables; // An item script's variables, by full name (its run's own)
    std::vector<ROWOWScriptValue> Stack;
    size_t InstructionIndex = 0;
    uint32 InstructionCount = 0;
    bool IsStopped = false; // An error, or a command ended the run
    std::string Error;

    // NPC scripts (R7): the variables live in their scopes (below and in the engine), the run keeps its frames, and it can wait
    bool IsNpcRun = false;
    ROWOWNpc const* Npc = nullptr; // The NPC it runs for (rAthena's oid), null for a function's run
    std::vector<ROWOWScriptFrame> Frames; // The first is the script the run started in
    ROWOWScriptVariables* CharacterVariables = nullptr; // The attached player's: name (saved), @name (until logout), #name and ##name (saved)
    ROWOWScriptVariables* PlayerVariables = nullptr;
    ROWOWScriptVariables* AccountVariables = nullptr;
    ROWOWScriptWait Wait = ROWOW_SCRIPT_WAIT_NONE;
    bool IsRerunLine = false; // RERUNLINE: the waiting command runs again (with the same arguments) when the answer comes
    uint64 SleepUntil = 0; // GameTime::GetGameTimeMS of the wake up
    ROWOWScriptDialog* Dialog = nullptr; // Where the dialog goes, null when the run has none (an event)
    bool IsNearNpcNeeded = false; // npc_checknear: the player's run ends when it goes out of the NPC's range (not for the GM's talk)
    bool IsMesActive = false; // mes_active: a dialog window is open
    bool IsMenuOrInput = false; // state.menu_or_input: the answer to the menu or input asked is in
    int32 NpcMenu = 0; // npc_menu: the choice made (1 up, 255 cancel)
    int64 NpcAmount = 0; // npc_amount: the number input
    std::string NpcString; // npc_str: the string input
    std::vector<std::string> MenuOptions; // The options last shown, for the transport and the GM test commands
    bool IsItemUseDisabled = false; // npc_item_flag 0 (disable_items): the player can't use or put on items while the run lasts (pre-re's
                                    // item_enabled_npc lets it by default)

    bool IsWaiting() const { return Wait != ROWOW_SCRIPT_WAIT_NONE; }
    ROWOWScript const* GetScript() const { return Frames.empty() == true ? nullptr : Frames.back().Script; }
};

// Where an NPC dialog goes: the RO NPC window (ROWOWWindowScriptDialog) or, for testing, the chat.  The engine's dialog commands call it;
// the answers come back through ROWOWMod's NpcDialog* functions (next, select, input, close), which resume the run
class ROWOWScriptDialog
{
public:
    virtual ~ROWOWScriptDialog() {}
    virtual void SendMes(Player* player, ROWOWNpc const* npc, std::string const& text) = 0; // clif_scriptmes
    virtual void SendNext(Player* player, ROWOWNpc const* npc) = 0; // clif_scriptnext: the Next button
    virtual void SendClose(Player* player, ROWOWNpc const* npc) = 0; // clif_scriptclose: the Close button
    virtual void SendClear(Player* player, ROWOWNpc const* npc) = 0; // clif_scriptclear (clear): the text so far goes
    virtual void SendMenu(Player* player, ROWOWNpc const* npc, std::vector<std::string> const& options) = 0; // clif_scriptmenu
    virtual void SendInput(Player* player, ROWOWNpc const* npc, bool isString, int64 minimum, int64 maximum) = 0; // clif_scriptinput(str), with its bounds
    virtual void SendCutin(Player* player, std::string const& image, int32 position) = 0; // clif_cutin (position 255 takes it off)
    virtual void SendEnd(Player* player, ROWOWNpc const* npc) = 0; // The run is over: the window goes
};

typedef std::function<ROWOWScriptValue(ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)> ROWOWScriptCommandFunction;
typedef std::function<void(ROWOWScriptContext& context, ROWOWScriptValue const& value)> ROWOWScriptParameterSetter;

// Whether a command can run with these arguments, for the calls whose arguments are all written out in the script (an itemskill of a
// skill that isn't in yet, say).  Checked once when the script is compiled
typedef std::function<bool(std::vector<ROWOWScriptValue> const& arguments)> ROWOWScriptCommandCheck;

// A command (rAthena's buildin functions), or a parameter (BaseLevel and the like, which scripts read as a word without arguments)
struct ROWOWScriptCommand
{
    std::string Name;
    ROWOWScriptCommandFunction Function;
    ROWOWScriptCommandCheck Check;
    ROWOWScriptParameterSetter Setter; // A parameter scripts can set (Zeny), null otherwise
    bool IsParameter = false;
    uint32 ReferenceArgumentMask = 0; // The arguments (bit per argument, the last bit for it and all after) passed as variable references
    bool IsStub = false; // In the table so scripts compile, but it does nothing yet (the commands count)
};

#define ROWOW_SCRIPT_REFERENCE_ARGUMENT_REST        (uint32(1) << 31)

// One definition of an NPC script file (a line of rAthena's npc_parsesrcfile): an NPC, a function, a duplicate, a shop, or the kinds the
// converter already takes elsewhere (warps, monster spawns, map flags)
enum ROWOWScriptDefinitionType : uint8
{
    ROWOW_SCRIPT_DEFINITION_SCRIPT,     // <map>,<x>,<y>,<dir>	script	<name>	<sprite>{,<x span>,<y span>},{<code>} (map "-" floating)
    ROWOW_SCRIPT_DEFINITION_FUNCTION,   // function	script	<name>	{<code>}
    ROWOW_SCRIPT_DEFINITION_DUPLICATE,  // <map>,<x>,<y>,<dir>	duplicate(<label>)	<name>	<sprite>{,<x span>,<y span>}
    ROWOW_SCRIPT_DEFINITION_SHOP,       // <map>,<x>,<y>,<dir>	shop	<name>	<sprite>,<item>:<price>,... (cashshop, itemshop, pointshop, marketshop)
    ROWOW_SCRIPT_DEFINITION_WARP,       // warp, warp2: RoWarps.csv
    ROWOW_SCRIPT_DEFINITION_MONSTER,    // monster, boss_monster, miniboss_monster: RoMobSpawns.csv
    ROWOW_SCRIPT_DEFINITION_MAPFLAG,    // mapflag: RoMapFlags.csv
    ROWOW_SCRIPT_DEFINITION_OTHER,      // setcell and the like
};

struct ROWOWScriptDefinition
{
    ROWOWScriptDefinitionType Type = ROWOW_SCRIPT_DEFINITION_OTHER;
    std::string Kind; // The second field as written (script, shop, duplicate(...), warp...)
    std::string MapName; // Lowercase; empty for a floating NPC ("-") and a function
    int32 X = 0;
    int32 Y = 0;
    int32 Direction = 0;
    std::string Name; // The third field: "Shown#hidden::Unique", or the function's name
    std::string Sprite; // The fourth field's first part (a number or a sprite constant)
    int32 TriggerSpanX = -1; // The OnTouch area's spans, -1 for none
    int32 TriggerSpanY = -1;
    std::string DuplicateSource; // A duplicate's source label
    std::string Parameters; // The rest of the fourth field (a shop's items, a warp's destination, a monster's spawn)
    std::shared_ptr<ROWOWScript> Script; // A script's or function's code
    uint32 Line = 0;
};

class ROWOWScriptEngine
{
private:
    ROWOWScriptEngine() {}

public:
    static ROWOWScriptEngine* instance()
    {
        static ROWOWScriptEngine instance;
        return &instance;
    }

    // Set up once at startup, before any script is compiled, and only read after that
    void Clear();
    bool IsNpcLanguage = false; // The NPC scripts' language is on (set before compiling them; item scripts compiled before keep R4's)
    void RegisterCommand(std::string const& name, ROWOWScriptCommandFunction function, ROWOWScriptCommandCheck check = nullptr);
    void RegisterParameter(std::string const& name, ROWOWScriptCommandFunction function);
    void RegisterConstant(std::string const& name, int64 value);
    void RegisterSymbolPrefix(std::string const& prefix); // Words starting with it are constants: symbols when not registered with a number
    void SetParameterSetter(std::string const& name, ROWOWScriptParameterSetter setter);
    void SetReferenceArguments(std::string const& name, uint32 argumentMask);
    void SetStub(std::string const& name);
    void RegisterFunction(std::string const& name, std::shared_ptr<ROWOWScript> script); // A global function (callfunc), last one wins

    // Compiles a script (at startup).  It always returns a script, which says why it can't run when it can't.  The line is where the source
    // starts in its file, for the errors
    std::shared_ptr<ROWOWScript> Compile(std::string const& source, std::string const& sourceName, uint32 firstLine = 1);

    // Reads an NPC script file into its definitions (rAthena's npc_parsesrcfile), compiling the scripts and functions.  A definition that
    // can't be read is left out with its error
    std::vector<ROWOWScriptDefinition> ParseNpcFile(std::string const& source, std::string const& fileName, std::vector<std::string>& errorsOut);

    // Runs a compiled script from the context's instruction (0 for the start) to its end, an error or a wait.  False on an error, which is
    // logged.  A lenient run (a worn item's script) runs a script with commands the engine doesn't have yet, which do nothing, so the rest
    // of its bonuses still count.  An NPC run (IsNpcRun) stops at such a command, logged once for each command
    bool Run(ROWOWScript const& script, ROWOWScriptContext& context, bool isLenient = false) const;

    // Goes on with a run that waits (once the caller has put the answer in the context and cleared its Wait)
    bool Resume(ROWOWScriptContext& context) const;

    // A symbol's name (SC_POISON for the symbol of that constant), or empty when the value isn't a symbol
    std::string GetSymbolName(int64 value) const;
    bool IsSymbol(int64 value) const { return value >= ROWOW_SCRIPT_SYMBOL_BASE && value < ROWOW_SCRIPT_SYMBOL_BASE + int64(SymbolNames.size()); }

    // Whether a word is a constant (registered, or with a symbol prefix), and its value
    bool GetConstant(std::string const& name, int64& valueOut);

    int32 GetCommandIndex(std::string const& name) const;
    ROWOWScriptCommand const* GetCommand(int32 commandIndex) const;
    size_t GetCommandCount() const { return Commands.size(); }
    ROWOWScript const* GetFunction(std::string const& name) const;

    // A variable of a run, by its full name (prefixes and $ suffix) and index, in its scope (rAthena's get_val and set_reg).  Without the
    // scope's owner (no attached player for a character variable) it's an error that stops the run
    ROWOWScriptValue GetVariable(ROWOWScriptContext& context, std::string const& name, uint32 index) const;
    void SetVariable(ROWOWScriptContext& context, std::string const& name, uint32 index, ROWOWScriptValue const& value) const;
    uint32 GetArraySize(ROWOWScriptContext& context, std::string const& name) const;
    ROWOWScriptValue GetReferenceValue(ROWOWScriptContext& context, ROWOWScriptValue const& reference) const;
    void SetReferenceValue(ROWOWScriptContext& context, ROWOWScriptValue const& reference, ROWOWScriptValue const& value) const;

    // The global variables ($name saved, $@name until a restart), and the lock for them and every script's NPC variables, which runs on
    // any map's thread share
    mutable ROWOWScriptVariables GlobalVariables;
    mutable ROWOWScriptVariables GlobalTempVariables;
    mutable std::mutex VariableMutex;

private:
    bool Execute(ROWOWScriptContext& context, bool isLenient) const;
    ROWOWScriptVariables* GetVariableScope(ROWOWScriptContext& context, std::string const& lowerName, bool& isLockedOut) const;

    std::vector<ROWOWScriptCommand> Commands;
    std::unordered_map<std::string, int32> CommandIndexesByName; // Lowercase, as rAthena's command names aren't case sensitive
    std::unordered_map<std::string, int64> ConstantsByName; // Lowercase, as rAthena's constants aren't case sensitive
    std::vector<std::string> SymbolPrefixes; // Lowercase
    std::vector<std::string> SymbolNames;
    std::unordered_map<std::string, int64> SymbolsByName; // Lowercase
    std::unordered_map<std::string, std::shared_ptr<ROWOWScript>> FunctionsByName; // Lowercase
    mutable std::mutex UnknownCommandMutex;
    mutable std::set<std::string> LoggedUnknownCommands; // The commands NPC runs stopped at, logged once each
};

#define ROWOWSCRIPT ROWOWScriptEngine::instance()

#endif
