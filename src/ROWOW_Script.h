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
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class Player;
struct ROWOWIdentity;

// The RO script engine: a subset of rAthena's script language (script.cpp), for item scripts now and NPC scripts later (R7).  A script
// is compiled once at startup into a small bytecode, which runs on a value stack with the state kept in ROWOWScriptContext, so a run can
// later stop and pick up again (an NPC dialog waiting on the player)

// A value: rAthena's script values are numbers or strings
struct ROWOWScriptValue
{
    bool IsString = false;
    int64 Number = 0;
    std::string Text;

    static ROWOWScriptValue FromNumber(int64 number) { ROWOWScriptValue value; value.Number = number; return value; }
    static ROWOWScriptValue FromText(std::string const& text) { ROWOWScriptValue value; value.IsString = true; value.Text = text; return value; }
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
    ROWOW_SCRIPT_OP_CALL,               // Argument: the command; Number: how many arguments it pops.  Pushes its result
    ROWOW_SCRIPT_OP_END,
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

// A compiled script.  One that calls a command the engine doesn't have yet (UnavailableCommands) is never run, so an item whose script
// needs one can't be used, rather than being used up for part of its effect
struct ROWOWScript
{
    std::string SourceName; // What it came from, for the logs
    std::vector<ROWOWScriptInstruction> Code;
    std::vector<std::string> Strings; // String constants and variable names
    std::vector<std::string> UnavailableCommands;
    std::string Error; // Why it didn't compile, empty when it did

    bool IsRunnable() const { return Error.empty() == true && UnavailableCommands.empty() == true; }
};

// One run of a script: what it's attached to (rAthena's RID, the player), and its state
struct ROWOWScriptContext
{
    Player* AttachedPlayer = nullptr;
    ROWOWIdentity* Identity = nullptr;
    ObjectGuid UsedItemGUID; // The item being used, for an item's use script
    uint32 UsedItemTemplateID = 0;
    std::unordered_map<std::string, ROWOWScriptValue> Variables; // By full name.  Only the run's own for now: lasting variables come with R7
    std::vector<ROWOWScriptValue> Stack;
    size_t InstructionIndex = 0;
    uint32 InstructionCount = 0;
    bool IsStopped = false; // An error, or a command ended the run
    std::string Error;
};

typedef std::function<ROWOWScriptValue(ROWOWScriptContext& context, std::vector<ROWOWScriptValue> const& arguments)> ROWOWScriptCommandFunction;

// Whether a command can run with these arguments, for the calls whose arguments are all written out in the script (an itemskill of a
// skill that isn't in yet, say).  Checked once when the script is compiled
typedef std::function<bool(std::vector<ROWOWScriptValue> const& arguments)> ROWOWScriptCommandCheck;

// A command (rAthena's buildin functions), or a parameter (BaseLevel and the like, which scripts read as a word without arguments)
struct ROWOWScriptCommand
{
    std::string Name;
    ROWOWScriptCommandFunction Function;
    ROWOWScriptCommandCheck Check;
    bool IsParameter = false;
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
    void RegisterCommand(std::string const& name, ROWOWScriptCommandFunction function, ROWOWScriptCommandCheck check = nullptr);
    void RegisterParameter(std::string const& name, ROWOWScriptCommandFunction function);
    void RegisterConstant(std::string const& name, int64 value);
    void RegisterSymbolPrefix(std::string const& prefix); // Words starting with it are constants: symbols when not registered with a number

    // Compiles a script (at startup).  It always returns a script, which says why it can't run when it can't
    std::shared_ptr<ROWOWScript> Compile(std::string const& source, std::string const& sourceName);

    // Runs a compiled script to its end (or an error).  False on an error, which is logged
    bool Run(ROWOWScript const& script, ROWOWScriptContext& context) const;

    // A symbol's name (SC_POISON for the symbol of that constant), or empty when the value isn't a symbol
    std::string GetSymbolName(int64 value) const;
    bool IsSymbol(int64 value) const { return value >= ROWOW_SCRIPT_SYMBOL_BASE && value < ROWOW_SCRIPT_SYMBOL_BASE + int64(SymbolNames.size()); }

    // Whether a word is a constant (registered, or with a symbol prefix), and its value
    bool GetConstant(std::string const& name, int64& valueOut);

    int32 GetCommandIndex(std::string const& name) const;
    ROWOWScriptCommand const* GetCommand(int32 commandIndex) const;

private:
    std::vector<ROWOWScriptCommand> Commands;
    std::unordered_map<std::string, int32> CommandIndexesByName; // Lowercase, as rAthena's command names aren't case sensitive
    std::unordered_map<std::string, int64> ConstantsByName; // Lowercase, as rAthena's constants aren't case sensitive
    std::vector<std::string> SymbolPrefixes; // Lowercase
    std::vector<std::string> SymbolNames;
    std::unordered_map<std::string, int64> SymbolsByName; // Lowercase
};

#define ROWOWSCRIPT ROWOWScriptEngine::instance()

#endif
