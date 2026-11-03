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
//  The language follows rAthena's script engine (src/map/script.cpp):
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
//  For more information, see LICENCE in the main folder

#include "Log.h"

#include "ROWOW_Script.h"

#include <cctype>

using namespace std;

// RO script engine: the language of rAthena's scripts (script.cpp's parse_* and run_script_main), compiled to bytecode once at startup
// and run on a value stack.  Covers expressions, variables and arrays, if/else, switch, the loops, and command calls with or without
// parentheses.  The NPC scripts' language (R7) adds labels, goto, menu, callsub, callfunc and the script's own functions with their
// frames, variable scopes, parameters scripts set, and runs that wait for the player and go on (npc_parsesrcfile reads the files).  It's
// switched on once the item, bonus and status scripts are compiled (IsNpcLanguage), so those keep R4's language and runnable set

namespace
{
    // The most instructions one run may take (until it waits), so a script that loops forever stops
    constexpr uint32 MAX_INSTRUCTIONS_PER_RUN = 1000000;

    // The deepest callsub and callfunc nesting (rAthena's MAX_SCRIPT_CALL_DEPTH-like limit), so a script that recurses forever stops
    constexpr size_t MAX_FRAMES = 128;

    string ToLower(string const& text)
    {
        string lowerText = text;
        for (char& character : lowerText)
            character = char(tolower(uint8(character)));
        return lowerText;
    }

    enum ROWOWScriptTokenType : uint8
    {
        TOKEN_END,
        TOKEN_NUMBER,
        TOKEN_STRING,
        TOKEN_NAME,     // A word: a command, constant, parameter or variable (with its prefixes and $ suffix)
        TOKEN_SYMBOL,   // An operator or punctuation
    };

    struct ROWOWScriptToken
    {
        ROWOWScriptTokenType Type = TOKEN_END;
        string Text;
        int64 Number = 0;
        uint32 Line = 1;
    };

    // Splits a script into tokens (rAthena's skip_space, skip_word and parse_simpleexpr reading)
    bool Tokenize(string const& source, vector<ROWOWScriptToken>& tokensOut, string& errorOut)
    {
        static char const* const multiCharacterSymbols[] = { "==", "!=", "<=", ">=", "&&", "||", "<<", ">>", "+=", "-=", "*=", "/=", "%=", "++", "--" };
        size_t position = 0;
        uint32 line = 1;
        while (position < source.size())
        {
            char character = source[position];
            if (character == '\n')
            {
                line++;
                position++;
                continue;
            }
            if (isspace(uint8(character)))
            {
                position++;
                continue;
            }

            // Comments
            if (character == '/' && position + 1 < source.size() && source[position + 1] == '/')
            {
                while (position < source.size() && source[position] != '\n')
                    position++;
                continue;
            }
            if (character == '/' && position + 1 < source.size() && source[position + 1] == '*')
            {
                position += 2;
                while (position + 1 < source.size() && (source[position] != '*' || source[position + 1] != '/'))
                {
                    if (source[position] == '\n')
                        line++;
                    position++;
                }
                position += 2;
                continue;
            }

            ROWOWScriptToken token;
            token.Line = line;

            // Numbers (decimal and 0x hexadecimal)
            if (isdigit(uint8(character)))
            {
                size_t start = position;
                int base = 10;
                if (character == '0' && position + 1 < source.size() && (source[position + 1] == 'x' || source[position + 1] == 'X'))
                {
                    base = 16;
                    position += 2;
                    start = position;
                    while (position < source.size() && isxdigit(uint8(source[position])))
                        position++;
                }
                else
                    while (position < source.size() && isdigit(uint8(source[position])))
                        position++;
                token.Type = TOKEN_NUMBER;
                token.Number = int64(strtoll(source.substr(start, position - start).c_str(), nullptr, base));
                tokensOut.push_back(token);
                continue;
            }

            // Strings, with rAthena's escapes
            if (character == '"')
            {
                position++;
                string text;
                while (position < source.size() && source[position] != '"')
                {
                    char textCharacter = source[position];
                    if (textCharacter == '\\' && position + 1 < source.size())
                    {
                        position++;
                        char escaped = source[position];
                        textCharacter = escaped == 'n' ? '\n' : (escaped == 't' ? '\t' : escaped);
                    }
                    else if (textCharacter == '\n')
                        line++;
                    text += textCharacter;
                    position++;
                }
                if (position >= source.size())
                {
                    errorOut = "unterminated string on line " + to_string(line);
                    return false;
                }
                position++;
                token.Type = TOKEN_STRING;
                token.Text = text;
                tokensOut.push_back(token);
                continue;
            }

            // Words: a variable's prefixes (. @ $ # '), the name, and a string variable's $
            if (isalpha(uint8(character)) || character == '_' || character == '.' || character == '@' || character == '$' || character == '#' || character == '\'')
            {
                size_t start = position;
                while (position < source.size() && (source[position] == '.' || source[position] == '@' || source[position] == '$' || source[position] == '#'
                    || source[position] == '\''))
                    position++;
                size_t nameStart = position;
                while (position < source.size() && (isalnum(uint8(source[position])) || source[position] == '_'))
                    position++;
                if (position == nameStart)
                {
                    errorOut = "unexpected '" + string(1, character) + "' on line " + to_string(line);
                    return false;
                }
                if (position < source.size() && source[position] == '$')
                    position++;
                token.Type = TOKEN_NAME;
                token.Text = source.substr(start, position - start);
                tokensOut.push_back(token);
                continue;
            }

            // Operators and punctuation
            token.Type = TOKEN_SYMBOL;
            for (char const* symbol : multiCharacterSymbols)
            {
                if (source.compare(position, 2, symbol) == 0)
                {
                    token.Text = symbol;
                    break;
                }
            }
            if (token.Text.empty() == true)
            {
                if (string("+-*/%<>!~&|^?:(){}[],;=").find(character) == string::npos)
                {
                    errorOut = "unexpected '" + string(1, character) + "' on line " + to_string(line);
                    return false;
                }
                token.Text = string(1, character);
            }
            position += token.Text.size();
            tokensOut.push_back(token);
        }
        ROWOWScriptToken endToken;
        endToken.Line = line;
        tokensOut.push_back(endToken);
        return true;
    }

    // A variable name: one with a prefix, or a plain word that isn't a command, parameter or constant (a character variable in rAthena)
    bool IsVariableName(string const& name)
    {
        return name.empty() == false && (name[0] == '.' || name[0] == '@' || name[0] == '$' || name[0] == '#' || name[0] == '\'');
    }

    // Compiles tokens into a script's bytecode (rAthena's parse_line, parse_syntax and parse_expr), emitting code as it goes.  Errors
    // throw, and come back as the script's Error
    class ROWOWScriptCompiler
    {
    public:
        ROWOWScriptCompiler(ROWOWScriptEngine& engine, vector<ROWOWScriptToken> const& tokens, ROWOWScript& script) : Engine(engine), Tokens(tokens), Script(script) {}

        void CompileAll()
        {
            while (Peek().Type != TOKEN_END)
                ParseStatement();
            Emit(ROWOW_SCRIPT_OP_END);
            ResolveLabelJumps();
        }

    private:
        struct LoopJumps
        {
            vector<size_t> BreakJumps;
            vector<size_t> ContinueJumps;
            bool IsSwitch = false;
        };

        // A jump or call to a label or the script's own function, set once the whole script is read (rAthena's set_label)
        struct LabelJump
        {
            size_t InstructionIndex = 0;
            string Name; // Lowercase
            bool IsFunction = false;
            uint32 Line = 0;
        };

        ROWOWScriptEngine& Engine;
        vector<ROWOWScriptToken> const& Tokens;
        ROWOWScript& Script;
        size_t TokenIndex = 0;
        vector<LoopJumps> Loops;
        uint32 HiddenVariableCount = 0;
        vector<LabelJump> LabelJumps;
        set<string> LocalFunctionNames; // Lowercase: declared (function <name>;) or defined (function <name> {...})
        unordered_map<string, uint32> LocalFunctionStarts;

        ROWOWScriptToken const& Peek(size_t ahead = 0) const { return Tokens[min(TokenIndex + ahead, Tokens.size() - 1)]; }
        ROWOWScriptToken const& Next() { ROWOWScriptToken const& token = Peek(); if (TokenIndex < Tokens.size() - 1) TokenIndex++; return token; }
        bool IsSymbol(string const& text, size_t ahead = 0) const { return Peek(ahead).Type == TOKEN_SYMBOL && Peek(ahead).Text == text; }
        bool IsWord(string const& text, size_t ahead = 0) const { return Peek(ahead).Type == TOKEN_NAME && ToLower(Peek(ahead).Text) == text; }

        [[noreturn]] void Fail(string const& message) const { throw runtime_error(message + " on line " + to_string(Peek().Line)); }

        void Expect(string const& symbol)
        {
            if (IsSymbol(symbol) == false)
                Fail("expected '" + symbol + "' but found '" + (Peek().Type == TOKEN_END ? string("the end") : Peek().Text) + "'");
            Next();
        }

        size_t Emit(ROWOWScriptOp op, int32 argument = 0, int64 number = 0)
        {
            ROWOWScriptInstruction instruction;
            instruction.Op = op;
            instruction.Argument = argument;
            instruction.Number = number;
            Script.Code.push_back(instruction);
            return Script.Code.size() - 1;
        }

        void PatchJump(size_t instructionIndex) { Script.Code[instructionIndex].Argument = int32(Script.Code.size()); }

        int32 AddString(string const& text)
        {
            for (size_t i = 0; i < Script.Strings.size(); i++)
                if (Script.Strings[i] == text)
                    return int32(i);
            Script.Strings.push_back(text);
            return int32(Script.Strings.size() - 1);
        }

        void CountUse(string const& name) { Script.CommandUses[ToLower(name)]++; }

        string MakeHiddenVariableName(string const& kind) { return ".@__" + kind + to_string(HiddenVariableCount++); }

        // Emits a jump (or a call) to a label or function, set at the end
        void EmitLabelJump(ROWOWScriptOp op, string const& name, bool isFunction, int64 number = 0)
        {
            LabelJump labelJump;
            labelJump.InstructionIndex = Emit(op, 0, number);
            labelJump.Name = ToLower(name);
            labelJump.IsFunction = isFunction;
            labelJump.Line = Peek().Line;
            LabelJumps.push_back(labelJump);
        }

        void ResolveLabelJumps()
        {
            for (LabelJump const& labelJump : LabelJumps)
            {
                uint32 target = 0;
                auto functionIter = LocalFunctionStarts.find(labelJump.Name);
                auto labelIter = Script.LabelsByName.find(labelJump.Name);
                if (labelJump.IsFunction == true && functionIter != LocalFunctionStarts.end())
                    target = functionIter->second;
                else if (labelIter != Script.LabelsByName.end())
                    target = labelIter->second;
                else if (functionIter != LocalFunctionStarts.end())
                    target = functionIter->second;
                else
                    throw runtime_error((labelJump.IsFunction == true ? "function '" : "label '") + labelJump.Name + "' not found on line " + to_string(labelJump.Line));
                Script.Code[labelJump.InstructionIndex].Argument = int32(target);
            }
        }

        // Statements

        void ParseStatement()
        {
            ROWOWScriptToken const& token = Peek();
            if (token.Type == TOKEN_SYMBOL && token.Text == ";")
            {
                Next();
                return;
            }
            if (token.Type == TOKEN_SYMBOL && token.Text == "{")
            {
                Next();
                while (IsSymbol("}") == false)
                {
                    if (Peek().Type == TOKEN_END)
                        Fail("missing '}'");
                    ParseStatement();
                }
                Next();
                return;
            }
            if (token.Type == TOKEN_SYMBOL && (token.Text == "++" || token.Text == "--"))
            {
                ParseExpression();
                Emit(ROWOW_SCRIPT_OP_POP);
                Expect(";");
                return;
            }
            if (token.Type != TOKEN_NAME && (Engine.IsNpcLanguage == false || (token.Type != TOKEN_NUMBER && token.Type != TOKEN_STRING)))
                Fail("unexpected '" + token.Text + "'");

            string word = token.Type == TOKEN_NAME ? ToLower(token.Text) : string();
            if (Engine.IsNpcLanguage == true && token.Type == TOKEN_NAME && IsSymbol(":", 1) == true && IsVariableName(token.Text) == false)
            {
                // A label (rAthena's labels are the script's own, the event labels like OnInit included).  A label written twice keeps
                // its first place, as rAthena warns and does
                Script.LabelsByName.emplace(word, uint32(Script.Code.size()));
                Next();
                Next();
                return;
            }
            if (word == "if")
                ParseIf();
            else if (word == "while")
                ParseWhile();
            else if (word == "do")
                ParseDoWhile();
            else if (word == "for")
                ParseFor();
            else if (word == "switch")
                ParseSwitch();
            else if (word == "break" || word == "continue")
            {
                Next();
                CountUse(word);
                ParseBreakOrContinue(word == "break");
                Expect(";");
            }
            else if (Engine.IsNpcLanguage == true && (word == "end" || word == "return"))
            {
                // end stops the run; return goes back to the caller with its value, ending the run outside a function
                Next();
                CountUse(word);
                if (word == "return" && IsSymbol(";") == false)
                {
                    ParseExpression();
                    Emit(ROWOW_SCRIPT_OP_RETURN, 0, 1);
                }
                else
                    Emit(word == "return" ? ROWOW_SCRIPT_OP_RETURN : ROWOW_SCRIPT_OP_END);
                Expect(";");
            }
            else if (word == "end" || word == "return" || (word == "close" && Engine.GetCommandIndex("close") < 0))
            {
                // An item script has no caller to return to, so return ends it like end does
                Next();
                if (word == "return" && IsSymbol(";") == false)
                {
                    ParseExpression();
                    Emit(ROWOW_SCRIPT_OP_POP);
                }
                Emit(ROWOW_SCRIPT_OP_END);
                Expect(";");
            }
            else if (Engine.IsNpcLanguage == true && word == "goto")
            {
                // goto <label>;
                Next();
                CountUse(word);
                ROWOWScriptToken const& labelToken = Next();
                if (labelToken.Type != TOKEN_NAME)
                    Fail("goto needs a label");
                EmitLabelJump(ROWOW_SCRIPT_OP_JUMP, labelToken.Text, false);
                Expect(";");
            }
            else if (Engine.IsNpcLanguage == true && word == "function")
                ParseFunction();
            else if (Engine.IsNpcLanguage == true && word == "menu")
                ParseMenu();
            else if (Engine.IsNpcLanguage == true && word == "callsub")
            {
                ParseCallSub();
                Emit(ROWOW_SCRIPT_OP_POP);
                Expect(";");
            }
            else if (word == "set")
            {
                // set <variable>, <value>;
                ParseSet();
                Expect(";");
            }
            else if (IsSymbol(":", 1) == true)
            {
                Fail("labels come with the NPC scripts");
            }
            else if (token.Type == TOKEN_NAME && (IsVariableName(token.Text) == true || (Peek(1).Type == TOKEN_SYMBOL && (Peek(1).Text == "=" || Peek(1).Text == "+=" || Peek(1).Text == "-="
                || Peek(1).Text == "*=" || Peek(1).Text == "/=" || Peek(1).Text == "%=" || Peek(1).Text == "++" || Peek(1).Text == "--" || Peek(1).Text == "["))))
            {
                ParseAssignmentStatement();
                ExpectAssignmentEnd();
            }
            else if (Engine.IsNpcLanguage == true && (token.Type == TOKEN_NUMBER || token.Type == TOKEN_STRING))
            {
                // A lone value, as what follows a stray character after an assignment can be (below)
                ParseExpression();
                Emit(ROWOW_SCRIPT_OP_POP);
                ExpectAssignmentEnd();
            }
            else
            {
                ParseCommandStatement();
            }
        }

        // The ";" after an assignment.  rAthena's parse_line skips whatever one character follows an assignment, so NPC scripts with a stray
        // ")" or a second "=" there load, and they do here too
        void ExpectAssignmentEnd()
        {
            if (Engine.IsNpcLanguage == true && IsSymbol(";") == false && Peek().Type == TOKEN_SYMBOL)
            {
                Next();
                return;
            }
            Expect(";");
        }

        void ParseSet()
        {
            Next();
            CountUse("set");
            bool hasParentheses = IsSymbol("(");
            if (hasParentheses == true)
                Next();
            ParseAssignmentTarget([this]() { ParseExpression(); }, ",");
            if (hasParentheses == true)
                Expect(")");
        }

        // A condition: an expression starting with "(", which may go on past its ")" (rAthena reads "if (a) && (b)" whole)
        void ParseCondition()
        {
            if (IsSymbol("(") == false)
                Fail("expected '(' but found '" + (Peek().Type == TOKEN_END ? string("the end") : Peek().Text) + "'");
            ParseExpression();
        }

        void ParseIf()
        {
            Next();
            CountUse("if");
            ParseCondition();
            size_t jumpToElse = Emit(ROWOW_SCRIPT_OP_JUMP_IF_FALSE);
            ParseStatement();
            if (IsWord("else") == true)
            {
                Next();
                size_t jumpToEnd = Emit(ROWOW_SCRIPT_OP_JUMP);
                PatchJump(jumpToElse);
                ParseStatement();
                PatchJump(jumpToEnd);
            }
            else
                PatchJump(jumpToElse);
        }

        void ParseWhile()
        {
            Next();
            CountUse("while");
            size_t conditionStart = Script.Code.size();
            ParseCondition();
            size_t jumpToEnd = Emit(ROWOW_SCRIPT_OP_JUMP_IF_FALSE);
            Loops.push_back(LoopJumps());
            ParseStatement();
            Emit(ROWOW_SCRIPT_OP_JUMP, int32(conditionStart));
            FinishLoop(conditionStart);
            PatchJump(jumpToEnd);
        }

        void ParseDoWhile()
        {
            Next();
            CountUse("do");
            size_t bodyStart = Script.Code.size();
            Loops.push_back(LoopJumps());
            ParseStatement();
            size_t conditionStart = Script.Code.size();
            if (IsWord("while") == false)
                Fail("expected 'while'");
            Next();
            ParseCondition();
            Expect(";");
            Emit(ROWOW_SCRIPT_OP_JUMP_IF_TRUE, int32(bodyStart));
            FinishLoop(conditionStart);
        }

        // A for loop's start or step: an assignment, or (NPC scripts) set or a command
        void ParseForPart()
        {
            if (Engine.IsNpcLanguage == true && IsWord("set") == true)
                ParseSet();
            else
                ParseAssignmentStatement();
        }

        void ParseFor()
        {
            // for (<start>; <condition>; <step>) <statement>: the step's code is compiled where it's read, and jumped around
            Next();
            CountUse("for");
            Expect("(");
            if (IsSymbol(";") == false)
                ParseForPart();
            Expect(";");
            size_t conditionStart = Script.Code.size();
            size_t jumpToEnd = SIZE_MAX;
            if (IsSymbol(";") == false)
            {
                ParseExpression();
                jumpToEnd = Emit(ROWOW_SCRIPT_OP_JUMP_IF_FALSE);
            }
            Expect(";");
            size_t jumpToBody = Emit(ROWOW_SCRIPT_OP_JUMP);
            size_t stepStart = Script.Code.size();
            if (IsSymbol(")") == false)
                ParseForPart();
            Expect(")");
            Emit(ROWOW_SCRIPT_OP_JUMP, int32(conditionStart));
            PatchJump(jumpToBody);
            Loops.push_back(LoopJumps());
            ParseStatement();
            Emit(ROWOW_SCRIPT_OP_JUMP, int32(stepStart));
            FinishLoop(stepStart);
            if (jumpToEnd != SIZE_MAX)
                PatchJump(jumpToEnd);
        }

        void ParseSwitch()
        {
            // switch (<value>) { case <constant>: ... default: ... }: the value goes in a hidden variable, the body is compiled in order,
            // and the comparisons follow it, jumping back to each case
            Next();
            CountUse("switch");
            ParseCondition();
            string hiddenVariableName = ".@__switch" + to_string(HiddenVariableCount++);
            int32 hiddenVariableIndex = AddString(hiddenVariableName);
            Emit(ROWOW_SCRIPT_OP_STORE_VARIABLE, hiddenVariableIndex);
            size_t jumpToComparisons = Emit(ROWOW_SCRIPT_OP_JUMP);
            Expect("{");
            LoopJumps switchJumps;
            switchJumps.IsSwitch = true;
            Loops.push_back(switchJumps);
            vector<pair<int64, size_t>> caseStarts;
            size_t defaultStart = SIZE_MAX;
            while (IsSymbol("}") == false)
            {
                if (Peek().Type == TOKEN_END)
                    Fail("missing '}'");
                if (IsWord("case") == true)
                {
                    Next();
                    int64 caseValue = ParseConstantValue();
                    Expect(":");
                    caseStarts.push_back(make_pair(caseValue, Script.Code.size()));
                }
                else if (IsWord("default") == true)
                {
                    Next();
                    Expect(":");
                    defaultStart = Script.Code.size();
                }
                else
                    ParseStatement();
            }
            Next();
            size_t jumpPastComparisons = Emit(ROWOW_SCRIPT_OP_JUMP);
            PatchJump(jumpToComparisons);
            for (auto const& caseStart : caseStarts)
            {
                Emit(ROWOW_SCRIPT_OP_PUSH_VARIABLE, hiddenVariableIndex);
                Emit(ROWOW_SCRIPT_OP_PUSH_NUMBER, 0, caseStart.first);
                Emit(ROWOW_SCRIPT_OP_BINARY, ROWOW_SCRIPT_OPERATOR_EQUAL);
                Emit(ROWOW_SCRIPT_OP_JUMP_IF_TRUE, int32(caseStart.second));
            }
            if (defaultStart != SIZE_MAX)
                Emit(ROWOW_SCRIPT_OP_JUMP, int32(defaultStart));
            PatchJump(jumpPastComparisons);
            for (size_t breakJump : Loops.back().BreakJumps)
                PatchJump(breakJump);
            Loops.pop_back();
        }

        int64 ParseConstantValue()
        {
            bool isNegative = false;
            if (IsSymbol("-") == true)
            {
                Next();
                isNegative = true;
            }
            ROWOWScriptToken const& token = Next();
            int64 value = 0;
            if (token.Type == TOKEN_NUMBER)
                value = token.Number;
            else if (token.Type != TOKEN_NAME || Engine.GetConstant(token.Text, value) == false)
                Fail("a case needs a constant");
            return isNegative == true ? -value : value;
        }

        void ParseBreakOrContinue(bool isBreak)
        {
            for (size_t loopIndex = Loops.size(); loopIndex > 0; loopIndex--)
            {
                LoopJumps& loop = Loops[loopIndex - 1];
                if (isBreak == false && loop.IsSwitch == true)
                    continue;
                size_t jump = Emit(ROWOW_SCRIPT_OP_JUMP);
                if (isBreak == true)
                    loop.BreakJumps.push_back(jump);
                else
                    loop.ContinueJumps.push_back(jump);
                return;
            }
            Fail(isBreak == true ? "break outside a loop or switch" : "continue outside a loop");
        }

        void FinishLoop(size_t continueTarget)
        {
            for (size_t continueJump : Loops.back().ContinueJumps)
                Script.Code[continueJump].Argument = int32(continueTarget);
            for (size_t breakJump : Loops.back().BreakJumps)
                PatchJump(breakJump);
            Loops.pop_back();
        }

        // function <name>; declares the script's own function, and function <name> { ... } is it (rAthena's parse_syntax "function"),
        // called by name as a command or function with its arguments, like callsub
        void ParseFunction()
        {
            Next();
            CountUse("function");
            ROWOWScriptToken const& nameToken = Next();
            if (nameToken.Type != TOKEN_NAME)
                Fail("function needs a name");
            string name = ToLower(nameToken.Text);
            LocalFunctionNames.insert(name);
            if (IsSymbol(";") == true)
            {
                Next();
                return;
            }
            if (IsSymbol("{") == false)
                Fail("expected ';' or '{' after function " + nameToken.Text);
            size_t jumpPastBody = Emit(ROWOW_SCRIPT_OP_JUMP);
            LocalFunctionStarts[name] = uint32(Script.Code.size());
            ParseStatement();
            Emit(ROWOW_SCRIPT_OP_RETURN);
            PatchJump(jumpPastBody);
        }

        // menu "<options>",<label>{,"<options>",<label>...}; (buildin_menu): the command gives the chosen option's place among the strings,
        // and a jump table goes to its label ("-" goes on after the menu)
        void ParseMenu()
        {
            Next();
            vector<string> labelNames;
            vector<size_t> argumentStarts;
            while (true)
            {
                argumentStarts.push_back(Script.Code.size());
                ParseExpression();
                Expect(",");
                if (IsSymbol("-") == true)
                {
                    Next();
                    labelNames.push_back(string());
                }
                else
                {
                    ROWOWScriptToken const& labelToken = Next();
                    if (labelToken.Type != TOKEN_NAME)
                        Fail("menu needs a label after each option");
                    labelNames.push_back(labelToken.Text);
                }
                if (IsSymbol(",") == false)
                    break;
                Next();
            }
            Expect(";");
            EmitCall("menu", uint32(labelNames.size()), argumentStarts);
            int32 hiddenVariableIndex = AddString(MakeHiddenVariableName("menu"));
            Emit(ROWOW_SCRIPT_OP_STORE_VARIABLE, hiddenVariableIndex);
            for (size_t labelIndex = 0; labelIndex < labelNames.size(); labelIndex++)
            {
                if (labelNames[labelIndex].empty() == true)
                    continue;
                Emit(ROWOW_SCRIPT_OP_PUSH_VARIABLE, hiddenVariableIndex);
                Emit(ROWOW_SCRIPT_OP_PUSH_NUMBER, 0, int64(labelIndex + 1));
                Emit(ROWOW_SCRIPT_OP_BINARY, ROWOW_SCRIPT_OPERATOR_EQUAL);
                EmitLabelJump(ROWOW_SCRIPT_OP_JUMP_IF_TRUE, labelNames[labelIndex], false);
            }
        }

        // callsub <label>{,<argument>...} or callsub(<label>{,<argument>...}): leaves the result on the stack
        void ParseCallSub()
        {
            Next();
            CountUse("callsub");
            bool hasParentheses = IsSymbol("(");
            if (hasParentheses == true)
                Next();
            ROWOWScriptToken const& labelToken = Next();
            if (labelToken.Type != TOKEN_NAME)
                Fail("callsub needs a label");
            string labelName = labelToken.Text;
            uint32 argumentCount = 0;
            while (IsSymbol(",") == true)
            {
                Next();
                ParseExpression();
                argumentCount++;
            }
            if (hasParentheses == true)
                Expect(")");
            EmitLabelJump(ROWOW_SCRIPT_OP_CALL_SUB, labelName, false, int64(argumentCount));
        }

        // <variable> = <value>, the compound assignments, and ++/-- (before or after)
        void ParseAssignmentStatement()
        {
            if (IsSymbol("++") == true || IsSymbol("--") == true)
            {
                ParseExpression();
                Emit(ROWOW_SCRIPT_OP_POP);
                return;
            }
            ParseAssignmentTarget(nullptr, string());
        }

        // Reads a variable (and its array index), then either the separator and a value (set's ",") or an assignment operator, and stores.
        // The target can also be a parameter scripts set (Zeny), or (NPC scripts) getd("<name>")
        void ParseAssignmentTarget(function<void()> parseValue, string const& separator)
        {
            ROWOWScriptToken const& nameToken = Next();
            if (nameToken.Type != TOKEN_NAME)
                Fail("expected a variable");
            if (Engine.IsNpcLanguage == true && ToLower(nameToken.Text) == "getd" && IsSymbol("(") == true)
            {
                // set getd("<name>"), <value>: the name is worked out when it runs
                CountUse("getd");
                Next();
                ParseExpression();
                Expect(")");
                if (separator.empty() == false)
                {
                    Expect(separator);
                    parseValue();
                }
                else
                {
                    Expect("=");
                    ParseExpression();
                }
                Emit(ROWOW_SCRIPT_OP_STORE_DYNAMIC_VARIABLE);
                return;
            }
            if (Engine.IsNpcLanguage == true && ToLower(nameToken.Text) == "getvariableofnpc" && IsSymbol("(") == true)
            {
                // set getvariableofnpc(<variable>,"<NPC>"), <value>: the hidden command setvariableofnpc(<variable>,"<NPC>",<value>)
                Next();
                ROWOWScriptToken const& variableToken = Next();
                if (variableToken.Type != TOKEN_NAME)
                    Fail("getvariableofnpc needs a variable");
                vector<size_t> argumentStarts;
                argumentStarts.push_back(Script.Code.size());
                Emit(ROWOW_SCRIPT_OP_PUSH_REFERENCE, AddString(variableToken.Text));
                Expect(",");
                argumentStarts.push_back(Script.Code.size());
                ParseExpression();
                Expect(")");
                if (separator.empty() == false)
                    Expect(separator);
                else
                    Expect("=");
                argumentStarts.push_back(Script.Code.size());
                if (separator.empty() == false)
                    parseValue();
                else
                    ParseExpression();
                EmitCall("setvariableofnpc", 3, argumentStarts);
                Emit(ROWOW_SCRIPT_OP_POP);
                return;
            }
            int64 constantValue = 0;
            int32 parameterIndex = -1;
            if (IsVariableName(nameToken.Text) == false && (Engine.GetCommandIndex(nameToken.Text) >= 0 || Engine.GetConstant(nameToken.Text, constantValue) == true))
            {
                int32 commandIndex = Engine.GetCommandIndex(nameToken.Text);
                ROWOWScriptCommand const* command = Engine.GetCommand(commandIndex);
                if (command != nullptr && command->IsParameter == true && command->Setter != nullptr)
                    parameterIndex = commandIndex;
                else
                {
                    // Setting a parameter the engine can't set: an NPC run stops there
                    Script.UnavailableCommands.push_back("set " + nameToken.Text);
                    if (Engine.IsNpcLanguage == true)
                    {
                        Emit(ROWOW_SCRIPT_OP_CALL, -1 - AddString("set " + nameToken.Text), 0);
                        Emit(ROWOW_SCRIPT_OP_POP);
                    }
                }
            }
            if (parameterIndex >= 0)
                CountUse(nameToken.Text);
            int32 nameIndex = AddString(nameToken.Text);
            bool isArray = false;
            int32 hiddenIndexVariableIndex = -1;
            if (IsSymbol("[") == true)
            {
                Next();
                ParseExpression();
                Expect("]");
                isArray = true;
                if (Engine.IsNpcLanguage == true && IsSymbol("=") == false && separator.empty() == true)
                {
                    // A compound assignment reads the element too, so the index is kept in a hidden variable for both
                    hiddenIndexVariableIndex = AddString(MakeHiddenVariableName("index"));
                    Emit(ROWOW_SCRIPT_OP_STORE_VARIABLE, hiddenIndexVariableIndex);
                    Emit(ROWOW_SCRIPT_OP_PUSH_VARIABLE, hiddenIndexVariableIndex);
                }
            }
            auto emitRead = [&]()
            {
                if (parameterIndex >= 0)
                {
                    Emit(ROWOW_SCRIPT_OP_CALL, parameterIndex, 0);
                    return;
                }
                if (isArray == true)
                {
                    // The index is on the stack under the value being built, so it's read again from a hidden copy
                    if (hiddenIndexVariableIndex < 0)
                        Fail("compound assignment to an array element comes with the NPC scripts");
                    Emit(ROWOW_SCRIPT_OP_PUSH_VARIABLE, hiddenIndexVariableIndex);
                    Emit(ROWOW_SCRIPT_OP_PUSH_ARRAY_VARIABLE, nameIndex);
                    return;
                }
                Emit(ROWOW_SCRIPT_OP_PUSH_VARIABLE, nameIndex);
            };
            if (separator.empty() == false)
            {
                Expect(separator);
                parseValue();
            }
            else if (IsSymbol("=") == true)
            {
                Next();
                ParseExpression();
            }
            else if (IsSymbol("++") == true || IsSymbol("--") == true)
            {
                bool isIncrement = Next().Text == "++";
                emitRead();
                Emit(ROWOW_SCRIPT_OP_PUSH_NUMBER, 0, 1);
                Emit(ROWOW_SCRIPT_OP_BINARY, isIncrement == true ? ROWOW_SCRIPT_OPERATOR_ADD : ROWOW_SCRIPT_OPERATOR_SUBTRACT);
            }
            else if (Peek().Type == TOKEN_SYMBOL && Peek().Text.size() == 2 && Peek().Text[1] == '=')
            {
                char operatorCharacter = Next().Text[0];
                emitRead();
                ParseExpression();
                switch (operatorCharacter)
                {
                    case '+': Emit(ROWOW_SCRIPT_OP_BINARY, ROWOW_SCRIPT_OPERATOR_ADD); break;
                    case '-': Emit(ROWOW_SCRIPT_OP_BINARY, ROWOW_SCRIPT_OPERATOR_SUBTRACT); break;
                    case '*': Emit(ROWOW_SCRIPT_OP_BINARY, ROWOW_SCRIPT_OPERATOR_MULTIPLY); break;
                    case '/': Emit(ROWOW_SCRIPT_OP_BINARY, ROWOW_SCRIPT_OPERATOR_DIVIDE); break;
                    default: Emit(ROWOW_SCRIPT_OP_BINARY, ROWOW_SCRIPT_OPERATOR_MODULO); break;
                }
            }
            else
                Fail("expected an assignment");
            if (parameterIndex >= 0)
                Emit(ROWOW_SCRIPT_OP_STORE_PARAMETER, parameterIndex);
            else
                Emit(isArray == true ? ROWOW_SCRIPT_OP_STORE_ARRAY_VARIABLE : ROWOW_SCRIPT_OP_STORE_VARIABLE, nameIndex);
        }

        // <command> <argument>, ...; or <command>(<argument>, ...);
        void ParseCommandStatement()
        {
            ROWOWScriptToken const& nameToken = Next();
            uint32 argumentCount = 0;
            vector<size_t> argumentStarts;
            if (IsSymbol("(") == true && IsCallWrappedInParentheses() == true)
            {
                Next();
                argumentCount = ParseArguments(")", argumentStarts);
                Expect(")");
            }
            else if (IsSymbol(";") == false)
                argumentCount = ParseArguments(";", argumentStarts);
            EmitCall(nameToken.Text, argumentCount, argumentStarts);
            Emit(ROWOW_SCRIPT_OP_POP);
            Expect(";");
        }

        // Whether the "(" after a command closes just before the ";" (cmd(a, b);), rather than starting its first argument (cmd (a + b) * 2;)
        bool IsCallWrappedInParentheses() const
        {
            int32 depth = 0;
            for (size_t ahead = 0; Peek(ahead).Type != TOKEN_END; ahead++)
            {
                ROWOWScriptToken const& token = Peek(ahead);
                if (token.Type != TOKEN_SYMBOL)
                    continue;
                if (token.Text == "(")
                    depth++;
                else if (token.Text == ")")
                {
                    depth--;
                    if (depth == 0)
                        return IsSymbol(";", ahead + 1);
                }
            }
            return false;
        }

        uint32 ParseArguments(string const& closingSymbol, vector<size_t>& argumentStartsOut)
        {
            uint32 argumentCount = 0;
            while (IsSymbol(closingSymbol) == false)
            {
                argumentStartsOut.push_back(Script.Code.size());
                ParseExpression();
                argumentCount++;
                if (IsSymbol(",") == false)
                    break;
                Next();
            }
            return argumentCount;
        }

        void EmitCall(string const& name, uint32 argumentCount, vector<size_t> const& argumentStarts)
        {
            string lowerName = ToLower(name);
            if (Engine.IsNpcLanguage == true && LocalFunctionNames.find(lowerName) != LocalFunctionNames.end())
            {
                // The script's own function (not a command, so not counted as one)
                EmitLabelJump(ROWOW_SCRIPT_OP_CALL_SUB, lowerName, true, int64(argumentCount));
                return;
            }
            CountUse(lowerName);
            if (Engine.IsNpcLanguage == true && lowerName == "callfunc")
            {
                Emit(ROWOW_SCRIPT_OP_CALL_FUNCTION, 0, int64(argumentCount));
                return;
            }
            int32 commandIndex = Engine.GetCommandIndex(name);
            if (commandIndex < 0)
            {
                Script.UnavailableCommands.push_back(name);
                Emit(ROWOW_SCRIPT_OP_CALL, -1 - AddString(name), int64(argumentCount));
                return;
            }
            ROWOWScriptCommand const* command = Engine.GetCommand(commandIndex);

            // The arguments a command reads or writes as variables are passed as references (an argument that is just a variable)
            if (command->ReferenceArgumentMask != 0)
            {
                for (size_t argumentIndex = 0; argumentIndex < argumentStarts.size(); argumentIndex++)
                {
                    bool isReference = argumentIndex < 31 ? (command->ReferenceArgumentMask & (uint32(1) << argumentIndex)) != 0
                        : (command->ReferenceArgumentMask & ROWOW_SCRIPT_REFERENCE_ARGUMENT_REST) != 0;
                    if (isReference == false)
                        continue;
                    size_t argumentEnd = argumentIndex + 1 < argumentStarts.size() ? argumentStarts[argumentIndex + 1] : Script.Code.size();
                    ROWOWScriptInstruction& lastInstruction = Script.Code[argumentEnd - 1];
                    if (lastInstruction.Op == ROWOW_SCRIPT_OP_PUSH_VARIABLE)
                        lastInstruction.Op = ROWOW_SCRIPT_OP_PUSH_REFERENCE;
                    else if (lastInstruction.Op == ROWOW_SCRIPT_OP_PUSH_ARRAY_VARIABLE)
                        lastInstruction.Op = ROWOW_SCRIPT_OP_PUSH_ARRAY_REFERENCE;
                    else if (lastInstruction.Op == ROWOW_SCRIPT_OP_CALL && lastInstruction.Argument == Engine.GetCommandIndex("getd") && Engine.GetCommandIndex("getdref") >= 0)
                        lastInstruction.Argument = Engine.GetCommandIndex("getdref"); // getd("<name>") as the variable: its reference
                }
            }

            // A call whose arguments are all written out gets the command's check
            if (command->Check != nullptr)
            {
                vector<ROWOWScriptValue> literalArguments;
                for (size_t argumentIndex = 0; argumentIndex < argumentStarts.size(); argumentIndex++)
                {
                    size_t argumentEnd = argumentIndex + 1 < argumentStarts.size() ? argumentStarts[argumentIndex + 1] : Script.Code.size();
                    if (argumentEnd != argumentStarts[argumentIndex] + 1)
                        break;
                    ROWOWScriptInstruction const& instruction = Script.Code[argumentStarts[argumentIndex]];
                    if (instruction.Op == ROWOW_SCRIPT_OP_PUSH_NUMBER)
                        literalArguments.push_back(ROWOWScriptValue::FromNumber(instruction.Number));
                    else if (instruction.Op == ROWOW_SCRIPT_OP_PUSH_STRING)
                        literalArguments.push_back(ROWOWScriptValue::FromText(Script.Strings[instruction.Argument]));
                    else
                        break;
                }
                if (literalArguments.size() == argumentStarts.size() && command->Check(literalArguments) == false)
                    Script.UnavailableCommands.push_back(name + (literalArguments.empty() == false ? " " + literalArguments[0].GetText() : string()));
            }
            Emit(ROWOW_SCRIPT_OP_CALL, commandIndex, int64(argumentCount));
        }

        // Expressions, in rAthena's (C's) order of precedence

        void ParseExpression() { ParseTernary(); }

        void ParseTernary()
        {
            ParseLogicalOr();
            if (IsSymbol("?") == false)
                return;
            Next();
            size_t jumpToElse = Emit(ROWOW_SCRIPT_OP_JUMP_IF_FALSE);
            ParseExpression();
            size_t jumpToEnd = Emit(ROWOW_SCRIPT_OP_JUMP);
            Expect(":");
            PatchJump(jumpToElse);
            ParseExpression();
            PatchJump(jumpToEnd);
        }

        void ParseLogicalOr()
        {
            ParseLogicalAnd();
            while (IsSymbol("||") == true)
            {
                // a || b: 1 if a is true, otherwise whether b is
                Next();
                size_t jumpToTrue = Emit(ROWOW_SCRIPT_OP_JUMP_IF_TRUE);
                ParseLogicalAnd();
                Emit(ROWOW_SCRIPT_OP_UNARY, ROWOW_SCRIPT_OPERATOR_NOT);
                Emit(ROWOW_SCRIPT_OP_UNARY, ROWOW_SCRIPT_OPERATOR_NOT);
                size_t jumpToEnd = Emit(ROWOW_SCRIPT_OP_JUMP);
                PatchJump(jumpToTrue);
                Emit(ROWOW_SCRIPT_OP_PUSH_NUMBER, 0, 1);
                PatchJump(jumpToEnd);
            }
        }

        void ParseLogicalAnd()
        {
            ParseBinary(0);
            while (IsSymbol("&&") == true)
            {
                // a && b: 0 if a is false, otherwise whether b is
                Next();
                size_t jumpToFalse = Emit(ROWOW_SCRIPT_OP_JUMP_IF_FALSE);
                ParseBinary(0);
                Emit(ROWOW_SCRIPT_OP_UNARY, ROWOW_SCRIPT_OPERATOR_NOT);
                Emit(ROWOW_SCRIPT_OP_UNARY, ROWOW_SCRIPT_OPERATOR_NOT);
                size_t jumpToEnd = Emit(ROWOW_SCRIPT_OP_JUMP);
                PatchJump(jumpToFalse);
                Emit(ROWOW_SCRIPT_OP_PUSH_NUMBER, 0, 0);
                PatchJump(jumpToEnd);
            }
        }

        // The binary operators from | down to * / %, by level
        void ParseBinary(uint32 level)
        {
            static vector<vector<pair<string, ROWOWScriptOperator>>> const operatorLevels =
            {
                { { "|", ROWOW_SCRIPT_OPERATOR_BITWISE_OR } },
                { { "^", ROWOW_SCRIPT_OPERATOR_BITWISE_XOR } },
                { { "&", ROWOW_SCRIPT_OPERATOR_BITWISE_AND } },
                { { "==", ROWOW_SCRIPT_OPERATOR_EQUAL }, { "!=", ROWOW_SCRIPT_OPERATOR_NOT_EQUAL } },
                { { "<", ROWOW_SCRIPT_OPERATOR_LESS }, { "<=", ROWOW_SCRIPT_OPERATOR_LESS_EQUAL }, { ">", ROWOW_SCRIPT_OPERATOR_GREATER }, { ">=", ROWOW_SCRIPT_OPERATOR_GREATER_EQUAL } },
                { { "<<", ROWOW_SCRIPT_OPERATOR_SHIFT_LEFT }, { ">>", ROWOW_SCRIPT_OPERATOR_SHIFT_RIGHT } },
                { { "+", ROWOW_SCRIPT_OPERATOR_ADD }, { "-", ROWOW_SCRIPT_OPERATOR_SUBTRACT } },
                { { "*", ROWOW_SCRIPT_OPERATOR_MULTIPLY }, { "/", ROWOW_SCRIPT_OPERATOR_DIVIDE }, { "%", ROWOW_SCRIPT_OPERATOR_MODULO } },
            };
            if (level >= operatorLevels.size())
            {
                ParseUnary();
                return;
            }
            ParseBinary(level + 1);
            while (true)
            {
                bool isFound = false;
                for (auto const& levelOperator : operatorLevels[level])
                {
                    if (IsSymbol(levelOperator.first) == true)
                    {
                        Next();
                        ParseBinary(level + 1);
                        Emit(ROWOW_SCRIPT_OP_BINARY, levelOperator.second);
                        isFound = true;
                        break;
                    }
                }
                if (isFound == false)
                    return;
            }
        }

        void ParseUnary()
        {
            if (IsSymbol("-") == true || IsSymbol("!") == true || IsSymbol("~") == true)
            {
                string symbol = Next().Text;
                ParseUnary();
                Emit(ROWOW_SCRIPT_OP_UNARY, symbol == "-" ? ROWOW_SCRIPT_OPERATOR_NEGATE : (symbol == "!" ? ROWOW_SCRIPT_OPERATOR_NOT : ROWOW_SCRIPT_OPERATOR_BITWISE_NOT));
                return;
            }
            if (IsSymbol("++") == true || IsSymbol("--") == true)
            {
                // ++<variable>: the new value
                bool isIncrement = Next().Text == "++";
                ROWOWScriptToken const& nameToken = Next();
                if (nameToken.Type != TOKEN_NAME || IsSymbol("["))
                    Fail("++ and -- need a variable");
                int32 nameIndex = AddString(nameToken.Text);
                Emit(ROWOW_SCRIPT_OP_PUSH_VARIABLE, nameIndex);
                Emit(ROWOW_SCRIPT_OP_PUSH_NUMBER, 0, 1);
                Emit(ROWOW_SCRIPT_OP_BINARY, isIncrement == true ? ROWOW_SCRIPT_OPERATOR_ADD : ROWOW_SCRIPT_OPERATOR_SUBTRACT);
                Emit(ROWOW_SCRIPT_OP_STORE_VARIABLE, nameIndex);
                Emit(ROWOW_SCRIPT_OP_PUSH_VARIABLE, nameIndex);
                return;
            }
            ParsePrimary();
        }

        void ParsePrimary()
        {
            ROWOWScriptToken const& token = Peek();
            if (token.Type == TOKEN_NAME && Engine.IsNpcLanguage == true && ToLower(token.Text) == "callsub")
            {
                ParseCallSub();
                return;
            }
            Next();
            if (token.Type == TOKEN_NUMBER)
            {
                Emit(ROWOW_SCRIPT_OP_PUSH_NUMBER, 0, token.Number);
                return;
            }
            if (token.Type == TOKEN_STRING)
            {
                Emit(ROWOW_SCRIPT_OP_PUSH_STRING, AddString(token.Text));
                return;
            }
            if (token.Type == TOKEN_SYMBOL && token.Text == "(")
            {
                ParseExpression();
                Expect(")");
                return;
            }
            if (token.Type != TOKEN_NAME)
                Fail("unexpected '" + (token.Type == TOKEN_END ? string("end") : token.Text) + "'");

            // A function call
            if (IsSymbol("(") == true && IsVariableName(token.Text) == false)
            {
                Next();
                vector<size_t> argumentStarts;
                uint32 argumentCount = ParseArguments(")", argumentStarts);
                Expect(")");
                EmitCall(token.Text, argumentCount, argumentStarts);
                return;
            }

            // A constant, a parameter, or a variable (an array element with its index)
            int64 constantValue = 0;
            if (IsVariableName(token.Text) == false)
            {
                if (Engine.GetConstant(token.Text, constantValue) == true)
                {
                    Emit(ROWOW_SCRIPT_OP_PUSH_NUMBER, 0, constantValue);
                    return;
                }
                int32 commandIndex = Engine.GetCommandIndex(token.Text);
                if (commandIndex >= 0 && Engine.GetCommand(commandIndex)->IsParameter == true)
                {
                    if (Engine.IsNpcLanguage == true)
                        CountUse(token.Text);
                    Emit(ROWOW_SCRIPT_OP_CALL, commandIndex, 0);
                    return;
                }
                if (Engine.IsNpcLanguage == true && LocalFunctionNames.find(ToLower(token.Text)) != LocalFunctionNames.end())
                {
                    // The script's own function called without arguments or parentheses
                    EmitLabelJump(ROWOW_SCRIPT_OP_CALL_SUB, token.Text, true, 0);
                    return;
                }
            }
            int32 nameIndex = AddString(token.Text);
            bool isArray = IsSymbol("[");
            if (isArray == true)
            {
                Next();
                ParseExpression();
                Expect("]");
                Emit(ROWOW_SCRIPT_OP_PUSH_ARRAY_VARIABLE, nameIndex);
            }
            else
                Emit(ROWOW_SCRIPT_OP_PUSH_VARIABLE, nameIndex);

            // <variable>++: the old value
            if (IsSymbol("++") == true || IsSymbol("--") == true)
            {
                if (isArray == true)
                    Fail("++ and -- on an array element come with the NPC scripts");
                bool isIncrement = Next().Text == "++";
                Emit(ROWOW_SCRIPT_OP_PUSH_VARIABLE, nameIndex);
                Emit(ROWOW_SCRIPT_OP_PUSH_NUMBER, 0, 1);
                Emit(ROWOW_SCRIPT_OP_BINARY, isIncrement == true ? ROWOW_SCRIPT_OPERATOR_ADD : ROWOW_SCRIPT_OPERATOR_SUBTRACT);
                Emit(ROWOW_SCRIPT_OP_STORE_VARIABLE, nameIndex);
            }
        }
    };

    // A variable's value when it was never set: 0, or "" for a string variable (a name ending in $)
    ROWOWScriptValue GetUnsetVariableValue(string const& name)
    {
        if (name.empty() == false && name.back() == '$')
            return ROWOWScriptValue::FromText(string());
        return ROWOWScriptValue::FromNumber(0);
    }

    // A value stored in a variable: a string variable keeps text, any other a number
    ROWOWScriptValue GetStoredValue(string const& name, ROWOWScriptValue const& value)
    {
        bool isStringVariable = name.empty() == false && name.back() == '$';
        return isStringVariable == true ? ROWOWScriptValue::FromText(value.GetText()) : ROWOWScriptValue::FromNumber(value.GetNumber());
    }

    string Trim(string const& text)
    {
        size_t start = text.find_first_not_of(" \t\r\n");
        if (start == string::npos)
            return string();
        size_t end = text.find_last_not_of(" \t\r\n");
        return text.substr(start, end - start + 1);
    }

    vector<string> Split(string const& text, char separator)
    {
        vector<string> parts;
        size_t start = 0;
        while (true)
        {
            size_t end = text.find(separator, start);
            parts.push_back(text.substr(start, end == string::npos ? string::npos : end - start));
            if (end == string::npos)
                return parts;
            start = end + 1;
        }
    }

    // Where the block starting at the "{" ends: its matching "}", skipping strings and comments (npc_skip_script), or npos
    size_t FindBlockEnd(string const& source, size_t openPosition)
    {
        int32 depth = 0;
        for (size_t position = openPosition; position < source.size(); position++)
        {
            char character = source[position];
            if (character == '"')
            {
                for (position++; position < source.size() && source[position] != '"'; position++)
                    if (source[position] == '\\')
                        position++;
            }
            else if (character == '/' && position + 1 < source.size() && source[position + 1] == '/')
            {
                while (position < source.size() && source[position] != '\n')
                    position++;
            }
            else if (character == '/' && position + 1 < source.size() && source[position + 1] == '*')
            {
                size_t commentEnd = source.find("*/", position + 2);
                if (commentEnd == string::npos)
                    return string::npos;
                position = commentEnd + 1;
            }
            else if (character == '{')
                depth++;
            else if (character == '}')
            {
                depth--;
                if (depth == 0)
                    return position;
            }
        }
        return string::npos;
    }

    uint32 CountLines(string const& source, size_t start, size_t end)
    {
        uint32 lineCount = 0;
        for (size_t position = start; position < end && position < source.size(); position++)
            if (source[position] == '\n')
                lineCount++;
        return lineCount;
    }
}

int64 ROWOWScriptValue::GetNumber() const
{
    if (IsString == false)
        return Number;
    return int64(strtoll(Text.c_str(), nullptr, 10));
}

std::string ROWOWScriptValue::GetText() const
{
    if (IsString == true)
        return Text;
    return to_string(Number);
}

ROWOWScriptValue ROWOWScriptVariables::Get(std::string const& name, uint32 index) const
{
    auto elementsIter = ElementsByName.find(name);
    if (elementsIter != ElementsByName.end())
    {
        auto elementIter = elementsIter->second.find(index);
        if (elementIter != elementsIter->second.end())
            return elementIter->second;
    }
    return GetUnsetVariableValue(name);
}

void ROWOWScriptVariables::Set(std::string const& name, uint32 index, ROWOWScriptValue const& value)
{
    ROWOWScriptValue storedValue = GetStoredValue(name, value);
    bool isUnset = storedValue.IsString == true ? storedValue.Text.empty() : storedValue.Number == 0;
    ChangedElements.insert(std::make_pair(name, index));
    if (isUnset == false)
    {
        ElementsByName[name][index] = storedValue;
        return;
    }
    auto elementsIter = ElementsByName.find(name);
    if (elementsIter == ElementsByName.end())
        return;
    elementsIter->second.erase(index);
    if (elementsIter->second.empty() == true)
        ElementsByName.erase(elementsIter);
}

void ROWOWScriptVariables::Clear()
{
    for (auto const& elementsByName : ElementsByName)
        for (auto const& elementByIndex : elementsByName.second)
            ChangedElements.insert(std::make_pair(elementsByName.first, elementByIndex.first));
    ElementsByName.clear();
}

uint32 ROWOWScriptVariables::GetArraySize(std::string const& name) const
{
    auto elementsIter = ElementsByName.find(name);
    if (elementsIter == ElementsByName.end() || elementsIter->second.empty() == true)
        return 0;
    return elementsIter->second.rbegin()->first + 1;
}

int64 ROWOWScript::GetLabel(std::string const& name) const
{
    auto labelIter = LabelsByName.find(ToLower(name));
    return labelIter == LabelsByName.end() ? -1 : int64(labelIter->second);
}

void ROWOWScriptEngine::Clear()
{
    Commands.clear();
    CommandIndexesByName.clear();
    ConstantsByName.clear();
    SymbolPrefixes.clear();
    SymbolNames.clear();
    SymbolsByName.clear();
    FunctionsByName.clear();
    IsNpcLanguage = false;
}

void ROWOWScriptEngine::RegisterCommand(std::string const& name, ROWOWScriptCommandFunction function, ROWOWScriptCommandCheck check)
{
    ROWOWScriptCommand command;
    command.Name = name;
    command.Function = function;
    command.Check = check;
    CommandIndexesByName[ToLower(name)] = int32(Commands.size());
    Commands.push_back(command);
}

void ROWOWScriptEngine::RegisterParameter(std::string const& name, ROWOWScriptCommandFunction function)
{
    RegisterCommand(name, function);
    Commands.back().IsParameter = true;
}

void ROWOWScriptEngine::RegisterConstant(std::string const& name, int64 value)
{
    ConstantsByName[ToLower(name)] = value;
}

void ROWOWScriptEngine::RegisterSymbolPrefix(std::string const& prefix)
{
    SymbolPrefixes.push_back(ToLower(prefix));
}

void ROWOWScriptEngine::SetParameterSetter(std::string const& name, ROWOWScriptParameterSetter setter)
{
    int32 commandIndex = GetCommandIndex(name);
    if (commandIndex >= 0)
        Commands[commandIndex].Setter = setter;
}

void ROWOWScriptEngine::SetReferenceArguments(std::string const& name, uint32 argumentMask)
{
    int32 commandIndex = GetCommandIndex(name);
    if (commandIndex >= 0)
        Commands[commandIndex].ReferenceArgumentMask = argumentMask;
}

void ROWOWScriptEngine::SetStub(std::string const& name)
{
    int32 commandIndex = GetCommandIndex(name);
    if (commandIndex >= 0)
        Commands[commandIndex].IsStub = true;
}

void ROWOWScriptEngine::RegisterFunction(std::string const& name, std::shared_ptr<ROWOWScript> script)
{
    FunctionsByName[ToLower(name)] = script;
}

ROWOWScript const* ROWOWScriptEngine::GetFunction(std::string const& name) const
{
    auto functionIter = FunctionsByName.find(ToLower(name));
    return functionIter == FunctionsByName.end() ? nullptr : functionIter->second.get();
}

bool ROWOWScriptEngine::GetConstant(std::string const& name, int64& valueOut)
{
    std::string lowerName = ToLower(name);
    auto constantIter = ConstantsByName.find(lowerName);
    if (constantIter != ConstantsByName.end())
    {
        valueOut = constantIter->second;
        return true;
    }
    auto symbolIter = SymbolsByName.find(lowerName);
    if (symbolIter != SymbolsByName.end())
    {
        valueOut = symbolIter->second;
        return true;
    }

    // A word with a constant prefix becomes a symbol.  The bonus types (bStr, bAtkEle...) are a "b" before a capital letter
    bool isSymbol = name.size() > 1 && name[0] == 'b' && isupper(uint8(name[1]));
    for (std::string const& prefix : SymbolPrefixes)
        if (lowerName.compare(0, prefix.size(), prefix) == 0)
            isSymbol = true;
    if (isSymbol == false)
        return false;
    valueOut = ROWOW_SCRIPT_SYMBOL_BASE + int64(SymbolNames.size());
    SymbolNames.push_back(name);
    SymbolsByName[lowerName] = valueOut;
    return true;
}

bool ROWOWScriptEngine::GetKnownConstant(std::string const& name, int64& valueOut) const
{
    std::string lowerName = ToLower(name);
    auto constantIter = ConstantsByName.find(lowerName);
    if (constantIter != ConstantsByName.end())
    {
        valueOut = constantIter->second;
        return true;
    }
    auto symbolIter = SymbolsByName.find(lowerName);
    if (symbolIter == SymbolsByName.end())
        return false;
    valueOut = symbolIter->second;
    return true;
}

std::string ROWOWScriptEngine::GetSymbolName(int64 value) const
{
    if (IsSymbol(value) == false)
        return std::string();
    return SymbolNames[size_t(value - ROWOW_SCRIPT_SYMBOL_BASE)];
}

int32 ROWOWScriptEngine::GetCommandIndex(std::string const& name) const
{
    auto commandIndexIter = CommandIndexesByName.find(ToLower(name));
    if (commandIndexIter == CommandIndexesByName.end())
        return -1;
    return commandIndexIter->second;
}

ROWOWScriptCommand const* ROWOWScriptEngine::GetCommand(int32 commandIndex) const
{
    if (commandIndex < 0 || size_t(commandIndex) >= Commands.size())
        return nullptr;
    return &Commands[commandIndex];
}

std::shared_ptr<ROWOWScript> ROWOWScriptEngine::Compile(std::string const& source, std::string const& sourceName, uint32 firstLine)
{
    std::shared_ptr<ROWOWScript> script = std::make_shared<ROWOWScript>();
    script->SourceName = sourceName;
    std::vector<ROWOWScriptToken> tokens;
    std::string error;
    if (Tokenize(source, tokens, error) == false)
    {
        script->Error = error;
        return script;
    }
    if (firstLine > 1)
        for (ROWOWScriptToken& token : tokens)
            token.Line += firstLine - 1;
    try
    {
        ROWOWScriptCompiler compiler(*this, tokens, *script);
        compiler.CompileAll();
    }
    catch (std::runtime_error const& exception)
    {
        script->Error = exception.what();
        script->Code.clear();
    }
    return script;
}

std::vector<ROWOWScriptDefinition> ROWOWScriptEngine::ParseNpcFile(std::string const& source, std::string const& fileName, std::vector<std::string>& errorsOut)
{
    // npc_parsesrcfile: each definition is a line of four tab separated fields (rAthena's w1..w4), a script's or function's code running
    // on from its "{" to the matching "}"
    std::vector<ROWOWScriptDefinition> definitions;
    size_t position = 0;
    uint32 line = 1;
    auto addError = [&](uint32 errorLine, std::string const& message)
    {
        errorsOut.push_back(fmt::format("{}:{}: {}", fileName, errorLine, message));
    };
    while (position < source.size())
    {
        // Blank space and comments between definitions
        char character = source[position];
        if (character == '\n')
        {
            line++;
            position++;
            continue;
        }
        if (isspace(uint8(character)))
        {
            position++;
            continue;
        }
        if (source.compare(position, 2, "//") == 0)
        {
            while (position < source.size() && source[position] != '\n')
                position++;
            continue;
        }
        if (source.compare(position, 2, "/*") == 0)
        {
            size_t commentEnd = source.find("*/", position + 2);
            if (commentEnd == std::string::npos)
                commentEnd = source.size();
            line += CountLines(source, position, commentEnd);
            position = std::min(source.size(), commentEnd + 2);
            continue;
        }

        size_t lineEnd = source.find('\n', position);
        if (lineEnd == std::string::npos)
            lineEnd = source.size();
        std::string lineText = source.substr(position, lineEnd - position);
        if (lineText.empty() == false && lineText.back() == '\r')
            lineText.pop_back();
        uint32 definitionLine = line;
        std::vector<std::string> fields;
        size_t fieldStart = 0;
        for (int32 fieldIndex = 0; fieldIndex < 3; fieldIndex++)
        {
            size_t tabPosition = lineText.find('\t', fieldStart);
            if (tabPosition == std::string::npos)
                break;
            fields.push_back(lineText.substr(fieldStart, tabPosition - fieldStart));
            fieldStart = tabPosition + 1;
        }
        if (fields.size() < 3)
        {
            // rAthena reads a line with fewer than three tabs as a three-field line only for some kinds; none are in the pre-renewal set
            if (fields.size() == 2)
                fields.push_back(lineText.substr(fieldStart));
            else
            {
                addError(definitionLine, "can't read the line '" + lineText.substr(0, 60) + "'");
                position = lineEnd;
                continue;
            }
            fieldStart = lineText.size();
        }
        size_t fourthStart = position + fieldStart; // Where the fourth field starts in the source
        std::string fourthField = fieldStart < lineText.size() ? lineText.substr(fieldStart) : std::string();

        ROWOWScriptDefinition definition;
        definition.Line = definitionLine;
        definition.Kind = Trim(fields[1]);
        definition.Name = Trim(fields[2]);
        std::string kind = ToLower(definition.Kind);
        std::string first = Trim(fields[0]);
        bool isFunction = ToLower(first) == "function" && kind == "script";
        if (isFunction == true)
            definition.Type = ROWOW_SCRIPT_DEFINITION_FUNCTION;
        else if (kind == "script")
            definition.Type = ROWOW_SCRIPT_DEFINITION_SCRIPT;
        else if (kind.compare(0, 10, "duplicate(") == 0 && kind.back() == ')')
        {
            definition.Type = ROWOW_SCRIPT_DEFINITION_DUPLICATE;
            definition.DuplicateSource = Trim(definition.Kind.substr(10, definition.Kind.size() - 11));
        }
        else if (kind == "shop" || kind == "cashshop" || kind == "itemshop" || kind == "pointshop" || kind == "marketshop")
            definition.Type = ROWOW_SCRIPT_DEFINITION_SHOP;
        else if (kind == "warp" || kind == "warp2")
            definition.Type = ROWOW_SCRIPT_DEFINITION_WARP;
        else if (kind == "monster" || kind == "boss_monster" || kind == "miniboss_monster")
            definition.Type = ROWOW_SCRIPT_DEFINITION_MONSTER;
        else if (kind == "mapflag")
            definition.Type = ROWOW_SCRIPT_DEFINITION_MAPFLAG;
        else
        {
            addError(definitionLine, "unknown kind '" + definition.Kind + "'");
            definition.Type = ROWOW_SCRIPT_DEFINITION_OTHER;
        }

        // The place: <map>,<x>,<y>{,<dir>}, or "-" for a floating NPC
        if (isFunction == false && first != "-")
        {
            std::vector<std::string> placeParts = Split(first, ',');
            definition.MapName = ToLower(Trim(placeParts[0]));
            definition.X = placeParts.size() > 1 ? int32(strtol(placeParts[1].c_str(), nullptr, 10)) : 0;
            definition.Y = placeParts.size() > 2 ? int32(strtol(placeParts[2].c_str(), nullptr, 10)) : 0;
            definition.Direction = placeParts.size() > 3 ? int32(strtol(placeParts[3].c_str(), nullptr, 10)) : 0;
        }

        if (definition.Type == ROWOW_SCRIPT_DEFINITION_SCRIPT || definition.Type == ROWOW_SCRIPT_DEFINITION_FUNCTION)
        {
            // The code: from the "{" (on the line or after it) to its "}", and the rest of that line is ignored
            size_t openPosition = source.find('{', fourthStart);
            size_t closePosition = openPosition == std::string::npos ? std::string::npos : FindBlockEnd(source, openPosition);
            if (closePosition == std::string::npos)
            {
                addError(definitionLine, "no '{...}' code for " + definition.Name);
                position = lineEnd;
                continue;
            }
            std::string header = source.substr(fourthStart, openPosition - fourthStart);
            std::vector<std::string> headerParts = Split(header, ',');
            definition.Sprite = Trim(headerParts[0]);
            if (headerParts.size() >= 4)
            {
                definition.TriggerSpanX = int32(strtol(headerParts[1].c_str(), nullptr, 10));
                definition.TriggerSpanY = int32(strtol(headerParts[2].c_str(), nullptr, 10));
            }
            uint32 openLine = line + CountLines(source, position, openPosition);
            definition.Script = Compile(source.substr(openPosition, closePosition - openPosition + 1), fileName + ":" + definition.Name, openLine);
            if (definition.Script->Error.empty() == false)
                addError(definitionLine, definition.Name + ": " + definition.Script->Error);
            line = openLine + CountLines(source, openPosition, closePosition);
            position = source.find('\n', closePosition);
            if (position == std::string::npos)
                position = source.size();
        }
        else
        {
            std::vector<std::string> headerParts = Split(fourthField, ',');
            definition.Sprite = Trim(headerParts[0]);
            if (definition.Type == ROWOW_SCRIPT_DEFINITION_DUPLICATE && headerParts.size() >= 3)
            {
                definition.TriggerSpanX = int32(strtol(headerParts[1].c_str(), nullptr, 10));
                definition.TriggerSpanY = int32(strtol(headerParts[2].c_str(), nullptr, 10));
            }
            size_t commaPosition = fourthField.find(',');
            definition.Parameters = commaPosition == std::string::npos ? std::string() : fourthField.substr(commaPosition + 1);
            position = lineEnd;
        }
        definitions.push_back(std::move(definition));
    }
    return definitions;
}

ROWOWScriptVariables* ROWOWScriptEngine::GetVariableScope(ROWOWScriptContext& context, std::string const& lowerName, bool& isShared) const
{
    // rAthena's variable prefixes: .@ the scope's, . the NPC's (or function's), $@ and $ global, @ the player's until logout, # and ## the
    // account's, ' the instance's (no instances: the NPC's), and none the character's
    isShared = false;
    if (lowerName.compare(0, 2, ".@") == 0)
        return context.Frames.empty() == true ? nullptr : &context.Frames.back().ScopeVariables;
    if (lowerName[0] == '.' || lowerName[0] == '\'')
    {
        isShared = true;
        return context.Frames.empty() == true ? nullptr : &context.Frames.back().Script->LocalVariables;
    }
    if (lowerName[0] == '$')
    {
        isShared = true;
        return lowerName.size() > 1 && lowerName[1] == '@' ? &GlobalTempVariables : &GlobalVariables;
    }
    if (lowerName[0] == '@')
        return context.PlayerVariables;
    if (lowerName[0] == '#')
        return context.AccountVariables;
    return context.CharacterVariables;
}

ROWOWScriptValue ROWOWScriptEngine::GetVariable(ROWOWScriptContext& context, std::string const& name, uint32 index) const
{
    std::string lowerName = ToLower(name);
    if (lowerName.empty() == true)
        return ROWOWScriptValue::FromNumber(0);
    bool isShared = false;
    ROWOWScriptVariables* variables = GetVariableScope(context, lowerName, isShared);
    if (variables == nullptr)
    {
        context.Error = "no player attached to read " + name;
        context.IsStopped = true;
        LOG_ERROR("module.ROWOW", "ROWOWScriptEngine::GetVariable: {} in the script of {}", context.Error, context.GetScript() != nullptr ? context.GetScript()->SourceName : "?");
        return GetUnsetVariableValue(lowerName);
    }
    if (isShared == true)
    {
        std::lock_guard<std::mutex> lock(VariableMutex);
        return variables->Get(lowerName, index);
    }
    return variables->Get(lowerName, index);
}

void ROWOWScriptEngine::SetVariable(ROWOWScriptContext& context, std::string const& name, uint32 index, ROWOWScriptValue const& value) const
{
    std::string lowerName = ToLower(name);
    if (lowerName.empty() == true)
        return;
    bool isShared = false;
    ROWOWScriptVariables* variables = GetVariableScope(context, lowerName, isShared);
    if (variables == nullptr)
    {
        context.Error = "no player attached to set " + name;
        context.IsStopped = true;
        LOG_ERROR("module.ROWOW", "ROWOWScriptEngine::SetVariable: {} in the script of {}", context.Error, context.GetScript() != nullptr ? context.GetScript()->SourceName : "?");
        return;
    }
    if (isShared == true)
    {
        std::lock_guard<std::mutex> lock(VariableMutex);
        variables->Set(lowerName, index, value);
        return;
    }
    variables->Set(lowerName, index, value);
}

uint32 ROWOWScriptEngine::GetArraySize(ROWOWScriptContext& context, std::string const& name) const
{
    std::string lowerName = ToLower(name);
    if (lowerName.empty() == true)
        return 0;
    bool isShared = false;
    ROWOWScriptVariables* variables = GetVariableScope(context, lowerName, isShared);
    if (variables == nullptr)
        return 0;
    if (isShared == true)
    {
        std::lock_guard<std::mutex> lock(VariableMutex);
        return variables->GetArraySize(lowerName);
    }
    return variables->GetArraySize(lowerName);
}

ROWOWScriptValue ROWOWScriptEngine::GetReferenceValue(ROWOWScriptContext& context, ROWOWScriptValue const& reference) const
{
    if (reference.IsReference == false)
        return reference;
    return GetVariable(context, reference.Text, uint32(std::max<int64>(0, reference.Number)));
}

void ROWOWScriptEngine::SetReferenceValue(ROWOWScriptContext& context, ROWOWScriptValue const& reference, ROWOWScriptValue const& value) const
{
    if (reference.IsReference == true)
        SetVariable(context, reference.Text, uint32(std::max<int64>(0, reference.Number)), value);
}

bool ROWOWScriptEngine::Run(ROWOWScript const& script, ROWOWScriptContext& context, bool isLenient) const
{
    // A run starts in the script given, at the context's instruction (a label's for an NPC's event)
    if (script.Error.empty() == false || (isLenient == false && context.IsNpcRun == false && script.IsRunnable() == false))
    {
        context.Error = script.Error.empty() == false ? script.Error : "unavailable command " + script.UnavailableCommands[0];
        context.IsStopped = true;
        LOG_ERROR("module.ROWOW", "ROWOWScriptEngine::Run: {} in the script of {}", context.Error, script.SourceName);
        return false;
    }
    context.Frames.clear();
    context.Frames.emplace_back();
    context.Frames.back().Script = &script;
    context.Wait = ROWOW_SCRIPT_WAIT_NONE;
    context.IsRerunLine = false;
    return Execute(context, isLenient);
}

bool ROWOWScriptEngine::Resume(ROWOWScriptContext& context) const
{
    if (context.IsStopped == true || context.Frames.empty() == true)
        return false;
    context.Wait = ROWOW_SCRIPT_WAIT_NONE;
    context.InstructionCount = 0;
    return Execute(context, false);
}

bool ROWOWScriptEngine::Execute(ROWOWScriptContext& context, bool isLenient) const
{
    // run_script_main: one instruction at a time until the end, an error, a wait, or a command stops the run
    auto stop = [&](std::string const& error)
    {
        context.Error = error;
        context.IsStopped = true;
        LOG_ERROR("module.ROWOW", "ROWOWScriptEngine::Run: {} in the script of {}", error, context.GetScript()->SourceName);
    };
    auto pop = [&]() -> ROWOWScriptValue
    {
        if (context.Stack.empty() == true)
            return ROWOWScriptValue::FromNumber(0);
        ROWOWScriptValue value = context.Stack.back();
        context.Stack.pop_back();
        return value;
    };

    // An item script's variables are its run's own, by full name (an array element's name has its index); an NPC run's are in their scopes
    auto getArrayElementName = [](std::string const& name, int64 index) -> std::string
    {
        return index == 0 ? name : name + "[" + std::to_string(index) + "]";
    };
    auto getVariable = [&](std::string const& name, int64 index) -> ROWOWScriptValue
    {
        if (context.IsNpcRun == true)
        {
            if (index < 0)
            {
                stop("a negative array index for " + name);
                return GetUnsetVariableValue(name);
            }
            return GetVariable(context, name, uint32(index));
        }
        auto variableIter = context.Variables.find(getArrayElementName(name, index));
        if (variableIter == context.Variables.end())
            return GetUnsetVariableValue(name);
        return variableIter->second;
    };
    auto setVariable = [&](std::string const& name, int64 index, ROWOWScriptValue const& value)
    {
        if (context.IsNpcRun == true)
        {
            if (index < 0)
                stop("a negative array index for " + name);
            else
                SetVariable(context, name, uint32(index), value);
            return;
        }
        context.Variables[getArrayElementName(name, index)] = GetStoredValue(name, value);
    };

    while (context.IsStopped == false && context.IsWaiting() == false)
    {
        ROWOWScriptFrame& frame = context.Frames.back();
        ROWOWScript const& script = *frame.Script;
        if (context.InstructionIndex >= script.Code.size())
        {
            context.IsStopped = true;
            break;
        }
        if (++context.InstructionCount > MAX_INSTRUCTIONS_PER_RUN)
        {
            stop("too many instructions (an endless loop?)");
            return false;
        }
        ROWOWScriptInstruction const& instruction = script.Code[context.InstructionIndex++];
        switch (instruction.Op)
        {
            case ROWOW_SCRIPT_OP_PUSH_NUMBER:
                context.Stack.push_back(ROWOWScriptValue::FromNumber(instruction.Number));
                break;
            case ROWOW_SCRIPT_OP_PUSH_STRING:
                context.Stack.push_back(ROWOWScriptValue::FromText(script.Strings[instruction.Argument]));
                break;
            case ROWOW_SCRIPT_OP_PUSH_VARIABLE:
                context.Stack.push_back(getVariable(script.Strings[instruction.Argument], 0));
                break;
            case ROWOW_SCRIPT_OP_PUSH_ARRAY_VARIABLE:
            {
                int64 index = pop().GetNumber();
                context.Stack.push_back(getVariable(script.Strings[instruction.Argument], index));
                break;
            }
            case ROWOW_SCRIPT_OP_PUSH_REFERENCE:
                context.Stack.push_back(ROWOWScriptValue::FromReference(script.Strings[instruction.Argument], 0));
                break;
            case ROWOW_SCRIPT_OP_PUSH_ARRAY_REFERENCE:
            {
                int64 index = pop().GetNumber();
                context.Stack.push_back(ROWOWScriptValue::FromReference(script.Strings[instruction.Argument], index));
                break;
            }
            case ROWOW_SCRIPT_OP_STORE_VARIABLE:
                setVariable(script.Strings[instruction.Argument], 0, pop());
                break;
            case ROWOW_SCRIPT_OP_STORE_ARRAY_VARIABLE:
            {
                ROWOWScriptValue value = pop();
                int64 index = pop().GetNumber();
                setVariable(script.Strings[instruction.Argument], index, value);
                break;
            }
            case ROWOW_SCRIPT_OP_STORE_DYNAMIC_VARIABLE:
            {
                // setd: "<name>" or "<name>[<index>]"
                ROWOWScriptValue value = pop();
                std::string name = pop().GetText();
                int64 index = 0;
                size_t bracketPosition = name.find('[');
                if (bracketPosition != std::string::npos)
                {
                    index = strtoll(name.c_str() + bracketPosition + 1, nullptr, 10);
                    name = name.substr(0, bracketPosition);
                }
                setVariable(name, index, value);
                break;
            }
            case ROWOW_SCRIPT_OP_STORE_PARAMETER:
            {
                ROWOWScriptValue value = pop();
                ROWOWScriptCommand const* parameter = GetCommand(instruction.Argument);
                if (parameter == nullptr || parameter->Setter == nullptr)
                    stop("a parameter that can't be set");
                else
                    parameter->Setter(context, value);
                break;
            }
            case ROWOW_SCRIPT_OP_POP:
                pop();
                break;
            case ROWOW_SCRIPT_OP_UNARY:
            {
                int64 value = pop().GetNumber();
                switch (instruction.Argument)
                {
                    case ROWOW_SCRIPT_OPERATOR_NEGATE: value = -value; break;
                    case ROWOW_SCRIPT_OPERATOR_NOT: value = value == 0 ? 1 : 0; break;
                    default: value = ~value; break;
                }
                context.Stack.push_back(ROWOWScriptValue::FromNumber(value));
                break;
            }
            case ROWOW_SCRIPT_OP_BINARY:
            {
                ROWOWScriptValue right = pop();
                ROWOWScriptValue left = pop();

                // Strings join with + and compare with the comparisons (op_2str); a string and a number compare as numbers
                if (left.IsString == true && right.IsString == true)
                {
                    int32 comparison = left.Text.compare(right.Text);
                    switch (instruction.Argument)
                    {
                        case ROWOW_SCRIPT_OPERATOR_ADD: context.Stack.push_back(ROWOWScriptValue::FromText(left.Text + right.Text)); break;
                        case ROWOW_SCRIPT_OPERATOR_EQUAL: context.Stack.push_back(ROWOWScriptValue::FromNumber(comparison == 0)); break;
                        case ROWOW_SCRIPT_OPERATOR_NOT_EQUAL: context.Stack.push_back(ROWOWScriptValue::FromNumber(comparison != 0)); break;
                        case ROWOW_SCRIPT_OPERATOR_LESS: context.Stack.push_back(ROWOWScriptValue::FromNumber(comparison < 0)); break;
                        case ROWOW_SCRIPT_OPERATOR_LESS_EQUAL: context.Stack.push_back(ROWOWScriptValue::FromNumber(comparison <= 0)); break;
                        case ROWOW_SCRIPT_OPERATOR_GREATER: context.Stack.push_back(ROWOWScriptValue::FromNumber(comparison > 0)); break;
                        case ROWOW_SCRIPT_OPERATOR_GREATER_EQUAL: context.Stack.push_back(ROWOWScriptValue::FromNumber(comparison >= 0)); break;
                        default: stop("a string in a number operation"); break;
                    }
                    break;
                }
                if ((left.IsString == true || right.IsString == true) && instruction.Argument == ROWOW_SCRIPT_OPERATOR_ADD)
                {
                    context.Stack.push_back(ROWOWScriptValue::FromText(left.GetText() + right.GetText()));
                    break;
                }
                int64 a = left.GetNumber();
                int64 b = right.GetNumber();
                int64 result = 0;
                switch (instruction.Argument)
                {
                    case ROWOW_SCRIPT_OPERATOR_ADD: result = a + b; break;
                    case ROWOW_SCRIPT_OPERATOR_SUBTRACT: result = a - b; break;
                    case ROWOW_SCRIPT_OPERATOR_MULTIPLY: result = a * b; break;
                    case ROWOW_SCRIPT_OPERATOR_DIVIDE:
                    case ROWOW_SCRIPT_OPERATOR_MODULO:
                        if (b == 0)
                        {
                            stop("division by zero");
                            break;
                        }
                        result = instruction.Argument == ROWOW_SCRIPT_OPERATOR_DIVIDE ? a / b : a % b;
                        break;
                    case ROWOW_SCRIPT_OPERATOR_EQUAL: result = a == b; break;
                    case ROWOW_SCRIPT_OPERATOR_NOT_EQUAL: result = a != b; break;
                    case ROWOW_SCRIPT_OPERATOR_LESS: result = a < b; break;
                    case ROWOW_SCRIPT_OPERATOR_LESS_EQUAL: result = a <= b; break;
                    case ROWOW_SCRIPT_OPERATOR_GREATER: result = a > b; break;
                    case ROWOW_SCRIPT_OPERATOR_GREATER_EQUAL: result = a >= b; break;
                    case ROWOW_SCRIPT_OPERATOR_BITWISE_AND: result = a & b; break;
                    case ROWOW_SCRIPT_OPERATOR_BITWISE_OR: result = a | b; break;
                    case ROWOW_SCRIPT_OPERATOR_BITWISE_XOR: result = a ^ b; break;
                    case ROWOW_SCRIPT_OPERATOR_SHIFT_LEFT: result = b >= 0 && b < 63 ? a << b : 0; break;
                    case ROWOW_SCRIPT_OPERATOR_SHIFT_RIGHT: result = b >= 0 && b < 63 ? a >> b : 0; break;
                }
                context.Stack.push_back(ROWOWScriptValue::FromNumber(result));
                break;
            }
            case ROWOW_SCRIPT_OP_JUMP:
                context.InstructionIndex = size_t(instruction.Argument);
                break;
            case ROWOW_SCRIPT_OP_JUMP_IF_FALSE:
                if (pop().GetNumber() == 0)
                    context.InstructionIndex = size_t(instruction.Argument);
                break;
            case ROWOW_SCRIPT_OP_JUMP_IF_TRUE:
                if (pop().GetNumber() != 0)
                    context.InstructionIndex = size_t(instruction.Argument);
                break;
            case ROWOW_SCRIPT_OP_CALL:
            {
                size_t callIndex = context.InstructionIndex - 1;
                ROWOWScriptCommand const* command = GetCommand(instruction.Argument);
                size_t argumentCount = size_t(instruction.Number);
                if (command == nullptr && isLenient == true && context.Stack.size() >= argumentCount)
                {
                    context.Stack.resize(context.Stack.size() - argumentCount);
                    context.Stack.push_back(ROWOWScriptValue::FromNumber(0));
                    break;
                }
                if (command == nullptr && context.IsNpcRun == true && instruction.Argument < 0 &&
                    GetFunction(script.Strings[size_t(-instruction.Argument - 1)]) != nullptr && context.Stack.size() >= argumentCount)
                {
                    // A global function called by its name (rAthena's C_USERFUNC: "F_GetNumSuffix(1)" is callfunc "F_GetNumSuffix",1)
                    if (context.Frames.size() >= MAX_FRAMES)
                    {
                        stop("calls nested too deep");
                        break;
                    }
                    ROWOWScriptFrame calledFrame;
                    calledFrame.Script = GetFunction(script.Strings[size_t(-instruction.Argument - 1)]);
                    calledFrame.ReturnIndex = context.InstructionIndex;
                    calledFrame.Arguments.assign(context.Stack.end() - argumentCount, context.Stack.end());
                    context.Stack.resize(context.Stack.size() - argumentCount);
                    calledFrame.StackBase = context.Stack.size();
                    context.Frames.push_back(std::move(calledFrame));
                    context.InstructionIndex = 0;
                    break;
                }
                if (command == nullptr && context.IsNpcRun == true)
                {
                    // An NPC run stops at a command that isn't in yet, which is logged once
                    std::string commandName = instruction.Argument < 0 ? script.Strings[size_t(-instruction.Argument - 1)] : std::string("?");
                    context.Error = "unavailable command " + commandName;
                    context.IsStopped = true;
                    std::lock_guard<std::mutex> lock(UnknownCommandMutex);
                    if (LoggedUnknownCommands.insert(ToLower(commandName)).second == true)
                        LOG_INFO("module.ROWOW", "ROWOWScriptEngine: the NPC scripts' command '{}' isn't in yet (first in {})", commandName, script.SourceName);
                    break;
                }
                if (command == nullptr || context.Stack.size() < argumentCount)
                {
                    stop("a call to an unavailable command");
                    break;
                }
                std::vector<ROWOWScriptValue> arguments(context.Stack.end() - argumentCount, context.Stack.end());
                context.Stack.resize(context.Stack.size() - argumentCount);
                context.IsRerunLine = false;
                ROWOWScriptValue result = command->Function(context, arguments);
                if (context.IsStopped == true && context.Error.empty() == false)
                    LOG_ERROR("module.ROWOW", "ROWOWScriptEngine::Run: {} (in {}) in the script of {}", context.Error, command->Name, script.SourceName);
                if (context.IsWaiting() == true && context.IsRerunLine == true)
                {
                    // RERUNLINE: the command runs again, with the same arguments, once the answer is in
                    context.Stack.insert(context.Stack.end(), arguments.begin(), arguments.end());
                    context.InstructionIndex = callIndex;
                    break;
                }
                context.Stack.push_back(result);
                break;
            }
            case ROWOW_SCRIPT_OP_CALL_SUB:
            case ROWOW_SCRIPT_OP_CALL_FUNCTION:
            {
                // callsub, callfunc and the script's own functions: a frame with the arguments and its own .@ variables
                size_t argumentCount = std::min(size_t(instruction.Number), context.Stack.size());
                std::vector<ROWOWScriptValue> arguments(context.Stack.end() - argumentCount, context.Stack.end());
                context.Stack.resize(context.Stack.size() - argumentCount);
                ROWOWScript const* calledScript = &script;
                size_t startIndex = size_t(instruction.Argument);
                if (instruction.Op == ROWOW_SCRIPT_OP_CALL_FUNCTION)
                {
                    std::string functionName = arguments.empty() == true ? std::string() : arguments[0].GetText();
                    calledScript = GetFunction(functionName);
                    if (calledScript == nullptr)
                    {
                        stop("callfunc of the unknown function '" + functionName + "'");
                        break;
                    }
                    if (arguments.empty() == false)
                        arguments.erase(arguments.begin());
                    startIndex = 0;
                }
                if (context.Frames.size() >= MAX_FRAMES)
                {
                    stop("calls nested too deep");
                    break;
                }
                ROWOWScriptFrame calledFrame;
                calledFrame.Script = calledScript;
                calledFrame.ReturnIndex = context.InstructionIndex;
                calledFrame.StackBase = context.Stack.size();
                calledFrame.Arguments = std::move(arguments);
                context.Frames.push_back(std::move(calledFrame));
                context.InstructionIndex = startIndex;
                break;
            }
            case ROWOW_SCRIPT_OP_RETURN:
            {
                ROWOWScriptValue value = instruction.Number != 0 ? pop() : ROWOWScriptValue::FromNumber(0);
                if (context.Frames.size() <= 1)
                {
                    // A return outside a function ends the run, as end does
                    context.IsStopped = true;
                    break;
                }
                context.Stack.resize(std::min(context.Stack.size(), context.Frames.back().StackBase));
                context.InstructionIndex = context.Frames.back().ReturnIndex;
                context.Frames.pop_back();
                context.Stack.push_back(value);
                break;
            }
            case ROWOW_SCRIPT_OP_END:
                context.IsStopped = true;
                break;
        }
    }
    return context.Error.empty() == true;
}
