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

#include "Configuration/Config.h"
#include "Log.h"
#include "ScriptMgr.h"

#include "ROWOW.h"

using namespace std;

class ROWOW_WorldScript : public WorldScript
{
public:
    ROWOW_WorldScript() : WorldScript("ROWOW_WorldScript") {}

    void OnAfterConfigLoad(bool reload) override
    {
        // The config values are read lock-free by the map update threads, so they are only loaded at startup
        if (reload == true)
        {
            LOG_INFO("module.ROWOW", "ROWOWMod skipped reloading its config (loaded at startup only). Restart the server to apply config changes.");
            return;
        }
        ROWOW->LoadConfigurationFile();
    }

    void OnStartup() override
    {
        // Before the maps start updating, so the creature AIs only ever read it
        ROWOW->LoadDataTables();
        ROWOW->LoadCreatureMobData();
        ROWOW->LoadCreatureTemplateData();
        ROWOW->LoadIllusionData();
        ROWOW->LoadMapGridData();
        ROWOW->LoadIdentityData();
        ROWOW->LoadStatusData();
        ROWOW->LoadBattleData();
        ROWOW->LoadItemScriptData();
        ROWOW->LoadBonusData();
        ROWOW->LoadProduceData(); // After the items, which its recipes make and use
        ROWOW->LoadSkillData(); // After the items, whose names the skills' item costs use
        ROWOW->LoadAbraData();
        ROWOW->LoadSkillUnitData();
        ROWOW->LoadStatusChangeData(); // After the skills, whose durations the statuses take, and the script commands their scripts use
        ROWOW->LoadQuestData(); // After the monsters, whose AegisNames its hunting targets name
        ROWOW->LoadNpcScriptData(); // Last: the NPC scripts' commands come after every other script is compiled, and OnInit runs
    }
};

void AddROWOWWorldScripts()
{
    new ROWOW_WorldScript();
}
