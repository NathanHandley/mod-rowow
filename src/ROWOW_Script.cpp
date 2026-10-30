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
// parentheses.  Labels, goto, functions and NPC dialogs come with the NPC scripts (R7)

namespace
{
    // The most instructions one run may take, so a script that loops forever stops
    constexpr uint32 MAX_INSTRUCTIONS_PER_RUN = 1000000;

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
        }

    private:
        struct LoopJumps
        {
            vector<size_t> BreakJumps;
            vector<size_t> ContinueJumps;
            bool IsSwitch = false;
        };

        ROWOWScriptEngine& Engine;
        vector<ROWOWScriptToken> const& Tokens;
        ROWOWScript& Script;
        size_t TokenIndex = 0;
        vector<LoopJumps> Loops;
        uint32 HiddenVariableCount = 0;

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
            if (token.Type != TOKEN_NAME)
                Fail("unexpected '" + token.Text + "'");

            string word = ToLower(token.Text);
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
                ParseBreakOrContinue(word == "break");
                Expect(";");
            }
            else if (word == "end" || word == "close" || word == "return")
            {
                // An item script has no caller to return to, so return ends it like end does (functions come with R7)
                Next();
                if (word == "return" && IsSymbol(";") == false)
                {
                    ParseExpression();
                    Emit(ROWOW_SCRIPT_OP_POP);
                }
                Emit(ROWOW_SCRIPT_OP_END);
                Expect(";");
            }
            else if (word == "set")
            {
                // set <variable>, <value>;
                Next();
                bool hasParentheses = IsSymbol("(");
                if (hasParentheses == true)
                    Next();
                ParseAssignmentTarget([this]() { ParseExpression(); }, ",");
                if (hasParentheses == true)
                    Expect(")");
                Expect(";");
            }
            else if (IsSymbol(":", 1) == true)
            {
                Fail("labels come with the NPC scripts");
            }
            else if (IsVariableName(token.Text) == true || (Peek(1).Type == TOKEN_SYMBOL && (Peek(1).Text == "=" || Peek(1).Text == "+=" || Peek(1).Text == "-="
                || Peek(1).Text == "*=" || Peek(1).Text == "/=" || Peek(1).Text == "%=" || Peek(1).Text == "++" || Peek(1).Text == "--" || Peek(1).Text == "[")))
            {
                ParseAssignmentStatement();
                Expect(";");
            }
            else
            {
                ParseCommandStatement();
            }
        }

        void ParseIf()
        {
            Next();
            Expect("(");
            ParseExpression();
            Expect(")");
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
            size_t conditionStart = Script.Code.size();
            Expect("(");
            ParseExpression();
            Expect(")");
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
            size_t bodyStart = Script.Code.size();
            Loops.push_back(LoopJumps());
            ParseStatement();
            size_t conditionStart = Script.Code.size();
            if (IsWord("while") == false)
                Fail("expected 'while'");
            Next();
            Expect("(");
            ParseExpression();
            Expect(")");
            Expect(";");
            Emit(ROWOW_SCRIPT_OP_JUMP_IF_TRUE, int32(bodyStart));
            FinishLoop(conditionStart);
        }

        void ParseFor()
        {
            // for (<start>; <condition>; <step>) <statement>: the step's code is compiled where it's read, and jumped around
            Next();
            Expect("(");
            if (IsSymbol(";") == false)
                ParseAssignmentStatement();
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
                ParseAssignmentStatement();
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
            Expect("(");
            ParseExpression();
            Expect(")");
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

        // Reads a variable (and its array index), then either the separator and a value (set's ",") or an assignment operator, and stores
        void ParseAssignmentTarget(function<void()> parseValue, string const& separator)
        {
            ROWOWScriptToken const& nameToken = Next();
            if (nameToken.Type != TOKEN_NAME)
                Fail("expected a variable");
            int64 constantValue = 0;
            if (IsVariableName(nameToken.Text) == false && (Engine.GetCommandIndex(nameToken.Text) >= 0 || Engine.GetConstant(nameToken.Text, constantValue) == true))
            {
                // Setting a parameter (Zeny, BaseLevel...) comes with the NPC scripts
                Script.UnavailableCommands.push_back("set " + nameToken.Text);
            }
            int32 nameIndex = AddString(nameToken.Text);
            bool isArray = false;
            if (IsSymbol("[") == true)
            {
                Next();
                ParseExpression();
                Expect("]");
                isArray = true;
            }
            auto emitRead = [&]()
            {
                if (isArray == true)
                {
                    // The index is on the stack under the value being built, so it's read again from a hidden copy
                    Fail("compound assignment to an array element comes with the NPC scripts");
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
            int32 commandIndex = Engine.GetCommandIndex(name);
            if (commandIndex < 0)
            {
                Script.UnavailableCommands.push_back(name);
                Emit(ROWOW_SCRIPT_OP_CALL, -1, int64(argumentCount));
                return;
            }

            // A call whose arguments are all written out gets the command's check
            ROWOWScriptCommand const* command = Engine.GetCommand(commandIndex);
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
            ROWOWScriptToken const& token = Next();
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
                    Emit(ROWOW_SCRIPT_OP_CALL, commandIndex, 0);
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

void ROWOWScriptEngine::Clear()
{
    Commands.clear();
    CommandIndexesByName.clear();
    ConstantsByName.clear();
    SymbolPrefixes.clear();
    SymbolNames.clear();
    SymbolsByName.clear();
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

std::shared_ptr<ROWOWScript> ROWOWScriptEngine::Compile(std::string const& source, std::string const& sourceName)
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

bool ROWOWScriptEngine::Run(ROWOWScript const& script, ROWOWScriptContext& context, bool isLenient) const
{
    // run_script_main: one instruction at a time until the end, an error, or a command stops the run
    auto stop = [&](std::string const& error)
    {
        context.Error = error;
        context.IsStopped = true;
        LOG_ERROR("module.ROWOW", "ROWOWScriptEngine::Run: {} in the script of {}", error, script.SourceName);
    };
    auto pop = [&]() -> ROWOWScriptValue
    {
        if (context.Stack.empty() == true)
            return ROWOWScriptValue::FromNumber(0);
        ROWOWScriptValue value = context.Stack.back();
        context.Stack.pop_back();
        return value;
    };
    auto getVariable = [&](std::string const& name) -> ROWOWScriptValue
    {
        auto variableIter = context.Variables.find(name);
        if (variableIter == context.Variables.end())
            return GetUnsetVariableValue(name);
        return variableIter->second;
    };
    auto getArrayElementName = [](std::string const& name, int64 index) -> std::string
    {
        return index == 0 ? name : name + "[" + std::to_string(index) + "]";
    };

    if (script.Error.empty() == false || (isLenient == false && script.IsRunnable() == false))
    {
        stop(script.Error.empty() == false ? script.Error : "unavailable command " + script.UnavailableCommands[0]);
        return false;
    }
    while (context.IsStopped == false && context.InstructionIndex < script.Code.size())
    {
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
                context.Stack.push_back(getVariable(script.Strings[instruction.Argument]));
                break;
            case ROWOW_SCRIPT_OP_PUSH_ARRAY_VARIABLE:
            {
                int64 index = pop().GetNumber();
                context.Stack.push_back(getVariable(getArrayElementName(script.Strings[instruction.Argument], index)));
                break;
            }
            case ROWOW_SCRIPT_OP_STORE_VARIABLE:
            {
                std::string const& name = script.Strings[instruction.Argument];
                ROWOWScriptValue value = pop();
                bool isStringVariable = name.empty() == false && name.back() == '$';
                context.Variables[name] = isStringVariable == true ? ROWOWScriptValue::FromText(value.GetText()) : ROWOWScriptValue::FromNumber(value.GetNumber());
                break;
            }
            case ROWOW_SCRIPT_OP_STORE_ARRAY_VARIABLE:
            {
                std::string const& name = script.Strings[instruction.Argument];
                ROWOWScriptValue value = pop();
                int64 index = pop().GetNumber();
                bool isStringVariable = name.empty() == false && name.back() == '$';
                context.Variables[getArrayElementName(name, index)] = isStringVariable == true ? ROWOWScriptValue::FromText(value.GetText()) : ROWOWScriptValue::FromNumber(value.GetNumber());
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
                ROWOWScriptCommand const* command = GetCommand(instruction.Argument);
                size_t argumentCount = size_t(instruction.Number);
                if (command == nullptr && isLenient == true && context.Stack.size() >= argumentCount)
                {
                    context.Stack.resize(context.Stack.size() - argumentCount);
                    context.Stack.push_back(ROWOWScriptValue::FromNumber(0));
                    break;
                }
                if (command == nullptr || context.Stack.size() < argumentCount)
                {
                    stop("a call to an unavailable command");
                    break;
                }
                std::vector<ROWOWScriptValue> arguments(context.Stack.end() - argumentCount, context.Stack.end());
                context.Stack.resize(context.Stack.size() - argumentCount);
                ROWOWScriptValue result = command->Function(context, arguments);
                if (context.IsStopped == true && context.Error.empty() == false)
                    LOG_ERROR("module.ROWOW", "ROWOWScriptEngine::Run: {} (in {}) in the script of {}", context.Error, command->Name, script.SourceName);
                context.Stack.push_back(result);
                break;
            }
            case ROWOW_SCRIPT_OP_END:
                context.IsStopped = true;
                break;
        }
    }
    return context.Error.empty() == true;
}
