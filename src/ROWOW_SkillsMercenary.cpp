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
//  Ported from rAthena (https://github.com/rathena/rathena), src/map/skills/mercenary, pre-renewal paths:
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
//  For more information, see LICENCE in the main folder

#include "Creature.h"
#include "Player.h"
#include "Unit.h"

#include "ROWOW.h"
#include "ROWOW_Skill.h"

using namespace std;

// The mercenaries' skills (rAthena's skills/mercenary, skill_factory_mercenary) that aren't a job skill's class under another SkillID: those are registered with their job's
// (AddROWOWSkillsSwordman and the like).  Each runs in the map thread of the mercenary and its master

namespace
{
    // SkillMercenaryCrash: the blow, and its chance to stun
    class ROWOWSkillMercenaryCrash : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillMercenaryCrash() : ROWOWWeaponSkillImpl(ROWOW_SKILL_MER_CRASH) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 10 * skillLevel;
        }

        void ApplyAdditionalEffects(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32 /*attackType*/, uint8 /*dmgLv*/) const override
        {
            ROWOW->SCStart(src, target, ROWOW_SC_STUN, 6 * skillLevel, skillLevel, ROWOW->SkillGetTime2(SkillID, skillLevel));
        }
    };

    // SkillMercenaryRegain and its kin: ends what it cures on the target
    class ROWOWSkillMercenaryCure : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillMercenaryCure(uint16 skillID, std::vector<int32> statuses) : ROWOWSkillImpl(skillID), Statuses(std::move(statuses)) { }

        void CastendNoDamageID(Unit* /*src*/, Unit* target, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*flag*/) const override
        {
            for (int32 type : Statuses)
                ROWOW->StatusChangeEnd(target, type);
        }

    private:
        std::vector<int32> Statuses;
    };

    // SkillMercenaryMagnificat: on itself and its master's party, or on its master alone
    class ROWOWSkillMercenaryMagnificat : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillMercenaryMagnificat() : ROWOWSkillImpl(ROWOW_SKILL_MER_MAGNIFICAT) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOWSkill const* skill = ROWOW->GetSkill(SkillID);
            Player* master = ROWOW->GetFollowerMaster(src);
            int64 duration = ROWOW->SkillGetTime(SkillID, skillLevel);
            ROWOW->SCStart(src, target, skill->StatusID, 100, skillLevel, duration);
            if (master == nullptr || (flag & 1) != 0)
                return;
            if (master->GetGroup() != nullptr)
            {
                int32 areaFlag = flag | ROWOW_BCT_PARTY | 1;
                ROWOW->PartyForEachSameMap(master, ROWOW->SkillGetSplash(SkillID, skillLevel), [this, src, skillLevel, tick, areaFlag](Unit* member)
                {
                    return ROWOW->SkillAreaSub(member, src, SkillID, skillLevel, tick, areaFlag, ROWOWSkillAreaFunc::CastendNoDamageID);
                });
            }
            else
                ROWOW->SCStart(src, master, skill->StatusID, 100, skillLevel, duration);
        }
    };

    // SkillMercenaryRemoveTrap: any trap on the cell it's cast at goes (traps can't be picked as WoW targets, as the Hunter's skill), and gives nothing back
    class ROWOWSkillMercenaryRemoveTrap : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillMercenaryRemoveTrap() : ROWOWSkillImpl(ROWOW_SKILL_MA_REMOVETRAP) { }

        void CastendPos2(Unit* src, int32 x, int32 y, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*flag*/) const override
        {
            ROWOW->SkillRemoveTrapOnCell(src, x, y);
        }
    };

    // SkillMercenarySacrifice: Devotion, on its master only, within the level difference, never a Crusader
    class ROWOWSkillMercenarySacrifice : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillMercenarySacrifice() : ROWOWSkillImpl(ROWOW_SKILL_ML_DEVOTION) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 /*tick*/, int32& flag) const override
        {
            ROWOWSkill const* skill = ROWOW->GetSkill(SkillID);
            Player* dstsd = target->ToPlayer();
            ROWOWFollower* fd = ROWOW->GetFollowerType(src) == ROWOW_BL_MER ? ROWOW->GetFollower(src) : nullptr;
            if (dstsd == nullptr || fd == nullptr)
                return; // Only players can be devoted
            ROWOWMercData* mer = static_cast<ROWOWMercData*>(fd);
            ROWOWStatus const* sstatus = ROWOW->GetROStatus(src);
            ROWOWStatus const* tstatus = ROWOW->GetROStatus(target);
            ROWOWStatusChangeEntry const* sce = ROWOW->GetSCE(target, skill->StatusID);
            int32 levelDifference = std::abs(int32(sstatus->Level) - int32(tstatus->Level));
            uint16 jobID = ROWOW->GetIdentity(dstsd)->JobID;
            if (levelDifference > ROWOW->BattleConfig.DevotionLevelDifference || // Level difference requeriments
                (sce != nullptr && uint32(sce->Val1) != src->GetGUID().GetCounter()) || // Cannot Devote a player devoted from another source
                ROWOW->GetMercenary(dstsd) != mer || // Mercenary only can devote owner
                jobID == 14 || jobID == 21 || jobID == 4015 || jobID == 4022 || // Crusader Cannot be devoted
                ROWOW->GetSCE(target, ROWOW_SC_HELLPOWER) != nullptr) // Players affected by SC_HELLPOWER cannot be devoted.
            {
                flag |= ROWOW_SKILL_NOCONSUME_REQ;
                return;
            }
            mer->IsDevotion = true; // Mercenary Devoting Owner
            ROWOW->StatusChangeStart(src, target, skill->StatusID, 10000, int32(src->GetGUID().GetCounter()), 0, ROWOW->SkillGetRange2(src, SkillID, skillLevel, true), 0,
                ROWOW->SkillGetTime2(SkillID, skillLevel), ROWOW_SCSTART_NOAVOID);
        }
    };

    // SkillMercenaryScapegoat: all its HP to its master, and it's gone
    class ROWOWSkillMercenaryScapegoat : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillMercenaryScapegoat() : ROWOWSkillImpl(ROWOW_SKILL_MER_SCAPEGOAT) { }

        void CastendNoDamageID(Unit* src, Unit* /*target*/, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            if (Player* master = ROWOW->GetFollowerMaster(src))
            {
                ROWOW->StatusHeal(master, src->GetHealth(), 0, 2, src, ROWOW->GetSkillSpellID(SkillID, skillLevel));
                ROWOW->StatusKill(src);
            }
        }
    };

    // SkillMercenarySacrifice's kin on a boss: ends a target's Invincible
    class ROWOWSkillMercenaryMindBlaster : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillMercenaryMindBlaster() : ROWOWSkillImpl(ROWOW_SKILL_MER_INVINCIBLEOFF2) { }

        void CastendNoDamageID(Unit* /*src*/, Unit* target, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*flag*/) const override
        {
            ROWOW->StatusChangeEnd(target, ROWOW_SC_INVINCIBLE);
        }
    };
}

void AddROWOWSkillsMercenary()
{
    ROWOW->RegisterSkillImpl(new ROWOWSkillMercenaryCrash());
    ROWOW->RegisterSkillImpl(new ROWOWSkillMercenaryCure(ROWOW_SKILL_MER_REGAIN, { ROWOW_SC_SLEEP, ROWOW_SC_STUN }));
    ROWOW->RegisterSkillImpl(new ROWOWSkillMercenaryCure(ROWOW_SKILL_MER_TENDER, { ROWOW_SC_FREEZE, ROWOW_SC_STONE }));
    ROWOW->RegisterSkillImpl(new ROWOWSkillMercenaryCure(ROWOW_SKILL_MER_BENEDICTION, { ROWOW_SC_CURSE, ROWOW_SC_BLIND }));
    ROWOW->RegisterSkillImpl(new ROWOWSkillMercenaryCure(ROWOW_SKILL_MER_RECUPERATE, { ROWOW_SC_POISON, ROWOW_SC_DPOISON, ROWOW_SC_SILENCE }));
    ROWOW->RegisterSkillImpl(new ROWOWSkillMercenaryCure(ROWOW_SKILL_MER_MENTALCURE, { ROWOW_SC_CONFUSION }));
    ROWOW->RegisterSkillImpl(new ROWOWSkillMercenaryCure(ROWOW_SKILL_MER_COMPRESS, { ROWOW_SC_BLEEDING }));
    ROWOW->RegisterSkillImpl(new ROWOWSkillMercenaryMagnificat());
    ROWOW->RegisterSkillImpl(new ROWOWSkillMercenaryRemoveTrap());
    ROWOW->RegisterSkillImpl(new ROWOWSkillMercenarySacrifice());
    ROWOW->RegisterSkillImpl(new ROWOWSkillMercenaryScapegoat());
    ROWOW->RegisterSkillImpl(new ROWOWSkillMercenaryMindBlaster());
}
