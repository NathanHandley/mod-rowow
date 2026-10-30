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

#include "DatabaseEnv.h"
#include "Log.h"
#include "QueryResult.h"
#include "Timer.h"

#include "ROWOW.h"

using namespace std;

// The rAthena pre-renewal data (the converter's Ragnarok WorldData tables), loaded once at startup and only read after that

int32 ROWOWDataTable::GetColumnIndex(std::string const& columnName) const
{
    auto columnIndexIter = ColumnIndexByName.find(columnName);
    if (columnIndexIter == ColumnIndexByName.end())
        return -1;
    return columnIndexIter->second;
}

std::string const& ROWOWDataTable::GetCell(size_t rowIndex, int32 columnIndex) const
{
    static std::string const emptyCell;
    if (columnIndex < 0 || rowIndex >= Rows.size() || size_t(columnIndex) >= Rows[rowIndex].size())
        return emptyCell;
    return Rows[rowIndex][columnIndex];
}

int64 ROWOWDataTable::GetInt(size_t rowIndex, int32 columnIndex, int64 defaultValue) const
{
    std::string const& cell = GetCell(rowIndex, columnIndex);
    if (cell.empty() == true)
        return defaultValue;
    char* parseEnd = nullptr;
    long long value = strtoll(cell.c_str(), &parseEnd, 10);
    if (parseEnd == nullptr || *parseEnd != '\0')
        return defaultValue;
    return int64(value);
}

// Escapes: "\\" backslash, "\p" '|', "\n" line feed, "\r" carriage return, "\c" ',', "\s" ';', "\e" '='
std::string ROWOWDataTable::UnescapeText(std::string const& text)
{
    if (text.find('\\') == std::string::npos)
        return text;
    std::string unescapedText;
    unescapedText.reserve(text.size());
    for (size_t i = 0; i < text.size(); i++)
    {
        char curChar = text[i];
        if (curChar != '\\' || i + 1 >= text.size())
        {
            unescapedText.push_back(curChar);
            continue;
        }
        i++;
        switch (text[i])
        {
            case '\\': unescapedText.push_back('\\'); break;
            case 'p': unescapedText.push_back('|'); break;
            case 'n': unescapedText.push_back('\n'); break;
            case 'r': unescapedText.push_back('\r'); break;
            case 'c': unescapedText.push_back(','); break;
            case 's': unescapedText.push_back(';'); break;
            case 'e': unescapedText.push_back('='); break;
            default:
            {
                unescapedText.push_back('\\');
                unescapedText.push_back(text[i]);
            } break;
        }
    }
    return unescapedText;
}

std::vector<std::pair<std::string, std::string>> ROWOWDataTable::ParseMap(std::string const& cell)
{
    std::vector<std::pair<std::string, std::string>> fields;
    size_t fieldStart = 0;
    while (fieldStart < cell.size())
    {
        size_t fieldEnd = cell.find(',', fieldStart);
        if (fieldEnd == std::string::npos)
            fieldEnd = cell.size();
        size_t equalsIndex = cell.find('=', fieldStart);
        if (equalsIndex != std::string::npos && equalsIndex < fieldEnd)
            fields.emplace_back(UnescapeText(cell.substr(fieldStart, equalsIndex - fieldStart)), UnescapeText(cell.substr(equalsIndex + 1, fieldEnd - equalsIndex - 1)));
        fieldStart = fieldEnd + 1;
    }
    return fields;
}

std::vector<std::vector<std::pair<std::string, std::string>>> ROWOWDataTable::ParseMapList(std::string const& cell)
{
    std::vector<std::vector<std::pair<std::string, std::string>>> entries;
    size_t entryStart = 0;
    while (entryStart < cell.size())
    {
        size_t entryEnd = cell.find(';', entryStart);
        if (entryEnd == std::string::npos)
            entryEnd = cell.size();
        if (entryEnd > entryStart)
            entries.push_back(ParseMap(cell.substr(entryStart, entryEnd - entryStart)));
        entryStart = entryEnd + 1;
    }
    return entries;
}

std::vector<std::string> ROWOWDataTable::ParseTextList(std::string const& cell)
{
    std::vector<std::string> entries;
    size_t entryStart = 0;
    while (entryStart < cell.size())
    {
        size_t entryEnd = cell.find(';', entryStart);
        if (entryEnd == std::string::npos)
            entryEnd = cell.size();
        if (entryEnd > entryStart)
            entries.push_back(UnescapeText(cell.substr(entryStart, entryEnd - entryStart)));
        entryStart = entryEnd + 1;
    }
    return entries;
}

void ROWOWMod::LoadDataTables()
{
    DataTablesByName.clear();
    MobRowIndexesByMobID.clear();
    if (IsEnabled == false)
        return;
    uint32 oldMSTime = getMSTime();

    // Every mod_rowow_db_* table the converter wrote
    std::vector<std::string> tableNames;
    QueryResult tableResult = WorldDatabase.Query("SELECT `table_name` FROM information_schema.tables WHERE table_schema = DATABASE() AND `table_name` LIKE 'mod\\_rowow\\_db\\_%' ORDER BY `table_name`");
    if (!tableResult)
    {
        LOG_INFO("module.ROWOW", "ROWOWMod::LoadDataTables found no mod_rowow_db_* tables, so there is no RO rule data (run the converter with Ragnarok enabled)");
        return;
    }
    do
    {
        tableNames.push_back(tableResult->Fetch()[0].Get<std::string>());
    } while (tableResult->NextRow());

    size_t cellCount = 0;
    for (std::string const& tableName : tableNames)
    {
        ROWOWDataTable& dataTable = DataTablesByName[tableName];
        dataTable.TableName = tableName;
        QueryResult result = WorldDatabase.Query("SELECT * FROM `{}` ORDER BY `RowIndex`", tableName);
        if (!result)
        {
            LOG_INFO("module.ROWOW", "ROWOWMod::LoadDataTables loaded 0 rows from {}", tableName);
            continue;
        }

        // RowIndex (the first column) is only the order
        uint32 fieldCount = result->GetFieldCount();
        for (uint32 fieldIndex = 1; fieldIndex < fieldCount; fieldIndex++)
        {
            dataTable.ColumnIndexByName[result->GetFieldName(fieldIndex)] = int32(dataTable.ColumnNames.size());
            dataTable.ColumnNames.push_back(result->GetFieldName(fieldIndex));
        }
        do
        {
            Field* fields = result->Fetch();
            std::vector<std::string> row;
            row.reserve(fieldCount - 1);
            for (uint32 fieldIndex = 1; fieldIndex < fieldCount; fieldIndex++)
                row.push_back(fields[fieldIndex].IsNull() == true ? std::string() : fields[fieldIndex].Get<std::string>());
            dataTable.Rows.push_back(std::move(row));
        } while (result->NextRow());
        cellCount += dataTable.Rows.size() * dataTable.ColumnNames.size();
        LOG_INFO("module.ROWOW", "ROWOWMod::LoadDataTables loaded {} rows ({} columns) from {}", dataTable.Rows.size(), dataTable.ColumnNames.size(), tableName);
    }

    // The monsters by their RO ID
    ROWOWDataTable const* mobTable = GetDataTable(ROWOW_DATA_TABLE_MOB);
    if (mobTable != nullptr)
    {
        int32 idColumnIndex = mobTable->GetColumnIndex("Id");
        for (size_t rowIndex = 0; rowIndex < mobTable->Rows.size(); rowIndex++)
            MobRowIndexesByMobID[uint32(mobTable->GetInt(rowIndex, idColumnIndex, 0))] = rowIndex;
    }
    LOG_INFO("module.ROWOW", "ROWOWMod::LoadDataTables loaded {} RO data tables ({} cells) in {} ms", DataTablesByName.size(), cellCount, GetMSTimeDiffToNow(oldMSTime));
}

void ROWOWMod::LoadCreatureMobData()
{
    MobIDsByCreatureEntry.clear();
    if (IsEnabled == false)
        return;

    // The converter creates the table, so it may not exist yet on a server that hasn't had the newer ROWOW SQL applied
    QueryResult tableResult = WorldDatabase.Query("SELECT 1 FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = 'mod_rowow_creature_mob'");
    if (!tableResult)
    {
        LOG_INFO("module.ROWOW", "ROWOWMod::LoadCreatureMobData found no mod_rowow_creature_mob table, so no creature is an RO monster");
        return;
    }
    QueryResult result = WorldDatabase.Query("SELECT `CreatureTemplateID`, `MobID` FROM `mod_rowow_creature_mob`");
    if (!result)
        return;
    uint32 missingMobCount = 0;
    do
    {
        Field* fields = result->Fetch();
        uint32 mobID = fields[1].Get<uint32>();
        MobIDsByCreatureEntry[fields[0].Get<uint32>()] = mobID;
        if (MobRowIndexesByMobID.find(mobID) == MobRowIndexesByMobID.end())
            missingMobCount++;
    } while (result->NextRow());
    if (missingMobCount > 0)
        LOG_ERROR("module.ROWOW", "ROWOWMod::LoadCreatureMobData has {} creature templates whose RO monster isn't in {}", missingMobCount, ROWOW_DATA_TABLE_MOB);
    LOG_INFO("module.ROWOW", "ROWOWMod::LoadCreatureMobData loaded {} RO monster creature templates", MobIDsByCreatureEntry.size());
}

ROWOWDataTable const* ROWOWMod::GetDataTable(std::string const& tableName) const
{
    auto dataTableIter = DataTablesByName.find(tableName);
    if (dataTableIter == DataTablesByName.end())
        return nullptr;
    return &dataTableIter->second;
}

int64 ROWOWMod::GetMobRowIndex(uint32 mobID) const
{
    auto mobRowIndexIter = MobRowIndexesByMobID.find(mobID);
    if (mobRowIndexIter == MobRowIndexesByMobID.end())
        return -1;
    return int64(mobRowIndexIter->second);
}
