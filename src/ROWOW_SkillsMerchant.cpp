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
//  Ported from rAthena (https://github.com/rathena/rathena), src/map/skills/merchant, pre-renewal paths:
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL.  For more information, see LICENCE in rAthena's main folder

#include "Player.h"
#include "Unit.h"

#include "ROWOW.h"

using namespace std;

// The Merchant line's skills (rAthena's skills/merchant, skill_factory_merchant), one class per skill.  They run in the caster's map
// thread

namespace
{
    // SkillMammonite: a weapon attack paid in zeny (the skill's ZenyCost, used up when the cast ends)
    class ROWOWSkillMammonite : public ROWOWWeaponSkillImpl
    {
    public:
        ROWOWSkillMammonite() : ROWOWWeaponSkillImpl(ROWOW_SKILL_MC_MAMMONITE) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* /*src*/, Unit const* /*target*/, uint16 skillLevel, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 50 * skillLevel;
        }
    };

    // SkillCartRevolution: a neutral weapon attack on the target and around it, 150% plus 1% a percent of the cart's weight (monsters at
    // full), knocking west
    class ROWOWSkillCartRevolution : public ROWOWRecursiveDamageSplashSkillImpl
    {
    public:
        ROWOWSkillCartRevolution() : ROWOWRecursiveDamageSplashSkillImpl(ROWOW_SKILL_MC_CARTREVOLUTION) { }

        void CalculateSkillRatio(ROWOWDamage const* /*wd*/, Unit const* src, Unit const* /*target*/, uint16 /*skillLevel*/, int32& baseSkillRatio, int32 /*mflag*/) const override
        {
            baseSkillRatio += 50;
            ROWOWIdentity const* identity = src->IsPlayer() == true ? src->ToPlayer()->CustomData.Get<ROWOWIdentity>("ROWOWIdentity") : nullptr;
            if (identity != nullptr && identity->CartWeight != 0 && identity->CartWeightMax > 0)
                baseSkillRatio += 100 * identity->CartWeight / identity->CartWeightMax; // +1% every 1% weight
            else if (src->IsPlayer() == false)
                baseSkillRatio += 100; // Max damage for non players.
        }

        void ModifyHitRate(int16& hitRate, Unit const* src, Unit const* /*target*/, uint16 /*skillLevel*/) const override
        {
            int32 remodeling = ROWOW->PCCheckSkill(src, ROWOW_SKILL_GN_REMODELING_CART);
            if (remodeling != 0)
                hitRate += int16(remodeling * 4);
        }

        void CastendDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            flag |= ROWOW_SD_PREAMBLE; // a fake packet will be sent for the first target to be hit
            ROWOWRecursiveDamageSplashSkillImpl::CastendDamageID(src, target, skillLevel, tick, flag);
        }
    };

    // SkillChangeCart: the cart's look is picked after (clif_parse_ChangeCart, the cart window's list); the cast itself only shows.
    // SkillDecorateCart opens that list (clif_SelectCart)
    class ROWOWSkillChangeCart : public ROWOWSkillImpl
    {
    public:
        explicit ROWOWSkillChangeCart(uint16 skillID) : ROWOWSkillImpl(skillID) { }

        void CastendNoDamageID(Unit* src, Unit* /*target*/, uint16 /*skillLevel*/, uint64 /*tick*/, int32& /*flag*/) const override
        {
            Player* sd = src->ToPlayer();
            if (sd != nullptr && SkillID == ROWOW_SKILL_MC_CARTDECORATE)
                ROWOW->SendCartToClient(sd, *ROWOW->GetIdentity(sd), true);
        }
    };

    // SkillItemAppraisal: the list of the unidentified items (clif_item_identify_list); with none, nothing is used up
    class ROWOWSkillItemAppraisal : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillItemAppraisal() : ROWOWSkillImpl(ROWOW_SKILL_MC_IDENTIFY) { }

        void CastendNoDamageID(Unit* src, Unit* /*target*/, uint16 /*skillLevel*/, uint64 /*tick*/, int32& flag) const override
        {
            Player* sd = src->ToPlayer();
            if (sd == nullptr)
                return;
            ROWOW->SendItemIdentifyList(sd);
            if (ROWOW->GetIdentity(sd)->IsIdentifyMenuOpen == false)
                flag |= ROWOW_SKILL_NOCONSUME_REQ; // failed, dont consume anything
        }
    };

    // SkillVending: the shop setup of the cart's items (clif_openvendingreq, as many items as 2 + the level)
    class ROWOWSkillVending : public ROWOWSkillImpl
    {
    public:
        ROWOWSkillVending() : ROWOWSkillImpl(ROWOW_SKILL_MC_VENDING) { }

        void CastendNoDamageID(Unit* src, Unit* /*target*/, uint16 skillLevel, uint64 /*tick*/, int32& /*flag*/) const override
        {
            if (Player* sd = src->ToPlayer())
                ROWOW->VendingOpenVendingReq(sd, uint8(skillLevel));
        }
    };

    // SkillAdrenalineRush, SkillWeaponPerfection and SkillPowerThrust: the status (val2 1 on the caster itself, whose share is bigger) on
    // the caster, or in a party on each member on the map within the skill's splash (party_foreachsamemap through skill_area_sub, which comes
    // back here with flag & 1).  A player gets it only holding one of the skill's weapons
    class ROWOWSkillPartyWeaponStatus : public ROWOWSkillImpl
    {
    public:
        explicit ROWOWSkillPartyWeaponStatus(uint16 skillID) : ROWOWSkillImpl(skillID) { }

        void CastendNoDamageID(Unit* src, Unit* target, uint16 skillLevel, uint64 tick, int32& flag) const override
        {
            ROWOWSkill const* skill = ROWOW->GetSkill(SkillID);
            Player* sd = src->ToPlayer();
            if (sd == nullptr || sd->GetGroup() == nullptr || (flag & 1) != 0)
            {
                uint32 weapontype = skill->RequiredWeapons;
                if (weapontype == 0 || target->IsPlayer() == false || ROWOW->PCCheckWeaponType(target, weapontype) == true)
                    ROWOW->SCStart2(src, target, skill->StatusID, 100, skillLevel, src == target ? 1 : 0, ROWOW->SkillGetTime(SkillID, skillLevel));
                return;
            }
            int32 areaFlag = flag | ROWOW_BCT_PARTY | 1;
            ROWOW->PartyForEachSameMap(sd, skill->SplashArea[ROWOWSkill::GetLevelIndex(skillLevel)], [this, src, skillLevel, tick, areaFlag](Unit* member)
            {
                return ROWOW->SkillAreaSub(member, src, SkillID, skillLevel, tick, areaFlag, ROWOWSkillAreaFunc::CastendNoDamageID);
            });
        }
    };
}

void AddROWOWSkillsMerchant()
{
    ROWOW->RegisterSkillImpl(new ROWOWSkillMammonite());
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_MC_LOUD)); // SkillCrazyUproar: pre-renewal's is the StatusSkillImpl alone
    ROWOW->RegisterSkillImpl(new ROWOWSkillCartRevolution());
    ROWOW->RegisterSkillImpl(new ROWOWSkillChangeCart(ROWOW_SKILL_MC_CHANGECART));
    ROWOW->RegisterSkillImpl(new ROWOWSkillChangeCart(ROWOW_SKILL_MC_CARTDECORATE));
    ROWOW->RegisterSkillImpl(new ROWOWSkillItemAppraisal());
    ROWOW->RegisterSkillImpl(new ROWOWSkillVending());
    ROWOW->RegisterSkillImpl(new ROWOWSkillPartyWeaponStatus(ROWOW_SKILL_BS_ADRENALINE));
    ROWOW->RegisterSkillImpl(new ROWOWSkillPartyWeaponStatus(ROWOW_SKILL_BS_WEAPONPERFECT));
    ROWOW->RegisterSkillImpl(new ROWOWSkillPartyWeaponStatus(ROWOW_SKILL_BS_OVERTHRUST));
    ROWOW->RegisterSkillImpl(new ROWOWStatusSkillImpl(ROWOW_SKILL_BS_MAXIMIZE, true)); // Ends when cast again
}
