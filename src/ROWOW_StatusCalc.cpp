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
//  Ported from rAthena (https://github.com/rathena/rathena), src/map/status.cpp, pre-renewal paths:
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL.  For more information, see LICENCE in rAthena's main folder

#include "Creature.h"
#include "Player.h"
#include "Unit.h"

#include "ROWOW.h"

using namespace std;

// What statuses do to a unit's status: rAthena's status_calc_bl_ and status_calc_bl_main, with the status_calc_* of each value, the HP
// and SP bonuses of statuses and the regen rates.  A player's battle status is its base status (status_calc_pc_sub) with these; a
// monster's is its mob_db status with them, kept on the creature while it has statuses.  The statuses of the third jobs and later are
// left out (nothing pre-renewal starts them).  Runs in the unit's map thread

namespace
{
    int32 GetDeferredBonusSum(ROWOWBonus const& bonus, char const* type)
    {
        int32 total = 0;
        for (ROWOWDeferredBonus const& deferred : bonus.Deferred)
        {
            if (deferred.Values.empty() == true || deferred.Type.size() != strlen(type))
                continue;
            if (std::equal(deferred.Type.begin(), deferred.Type.end(), type, [](char a, char b) { return tolower((unsigned char)a) == tolower((unsigned char)b); }) == true)
                total += int32(deferred.Values.back());
        }
        return total;
    }

    int32 SCVal(ROWOWStatusChange const* sc, int32 type, int32 valIndex)
    {
        ROWOWStatusChangeEntry const* sce = sc->GetSCE(type);
        if (sce == nullptr)
            return 0;
        switch (valIndex)
        {
            case 1: return sce->Val1;
            case 2: return sce->Val2;
            case 3: return sce->Val3;
            default: return sce->Val4;
        }
    }

    // status_calc_str
    int32 StatusCalcStr(Unit const* /*bl*/, ROWOWStatusChange const* sc, int32 str)
    {
        if (sc == nullptr || sc->IsEmpty() == true)
            return std::clamp(str, 0, int32(USHRT_MAX));
        if (sc->HasSCE(ROWOW_SC_INCALLSTATUS) == true)
            str += SCVal(sc, ROWOW_SC_INCALLSTATUS, 1);
        if (sc->HasSCE(ROWOW_SC_CHASEWALK2) == true)
            str += SCVal(sc, ROWOW_SC_CHASEWALK2, 1);
        if (sc->HasSCE(ROWOW_SC_BATTLEORDERS) == true)
            str += 5;
        if (sc->HasSCE(ROWOW_SC_LEADERSHIP) == true)
            str += SCVal(sc, ROWOW_SC_LEADERSHIP, 1);
        if (sc->HasSCE(ROWOW_SC_LOUD) == true)
            str += 4;
        if (sc->HasSCE(ROWOW_SC_TRUESIGHT) == true)
            str += 5;
        if (sc->HasSCE(ROWOW_SC_SPURT) == true)
            str += 10;
        if (sc->HasSCE(ROWOW_SC_NEN) == true)
            str += SCVal(sc, ROWOW_SC_NEN, 1);
        if (sc->HasSCE(ROWOW_SC_BLESSING) == true)
        {
            if (SCVal(sc, ROWOW_SC_BLESSING, 2) != 0)
                str += SCVal(sc, ROWOW_SC_BLESSING, 2);
            else
                str -= str / 2;
        }
        if (sc->HasSCE(ROWOW_SC_MARIONETTE) == true)
            str -= (SCVal(sc, ROWOW_SC_MARIONETTE, 3) >> 16) & 0xFF;
        if (sc->HasSCE(ROWOW_SC_MARIONETTE2) == true)
            str += (SCVal(sc, ROWOW_SC_MARIONETTE2, 3) >> 16) & 0xFF;
        if (sc->HasSCE(ROWOW_SC_SPIRIT) == true && SCVal(sc, ROWOW_SC_SPIRIT, 2) == ROWOW_SKILL_SL_HIGH)
            str += (SCVal(sc, ROWOW_SC_SPIRIT, 3) >> 16) & 0xFF;
        if (sc->HasSCE(ROWOW_SC_STOMACHACHE) == true)
            str -= SCVal(sc, ROWOW_SC_STOMACHACHE, 1);
        if (sc->HasSCE(ROWOW_SC_ALL_STAT_DOWN) == true)
            str -= SCVal(sc, ROWOW_SC_ALL_STAT_DOWN, 2);
        return std::clamp(str, 0, int32(USHRT_MAX));
    }

    // status_calc_agi
    int32 StatusCalcAgi(Unit const* /*bl*/, ROWOWStatusChange const* sc, int32 agi)
    {
        if (sc == nullptr || sc->IsEmpty() == true)
            return std::clamp(agi, 0, int32(USHRT_MAX));
        if (sc->HasSCE(ROWOW_SC_CONCENTRATE) == true && sc->HasSCE(ROWOW_SC_QUAGMIRE) == false)
            agi += (agi - SCVal(sc, ROWOW_SC_CONCENTRATE, 3)) * SCVal(sc, ROWOW_SC_CONCENTRATE, 2) / 100;
        if (sc->HasSCE(ROWOW_SC_INCALLSTATUS) == true)
            agi += SCVal(sc, ROWOW_SC_INCALLSTATUS, 1);
        if (sc->HasSCE(ROWOW_SC_SOULCOLD) == true)
            agi += SCVal(sc, ROWOW_SC_SOULCOLD, 1);
        if (sc->HasSCE(ROWOW_SC_TRUESIGHT) == true)
            agi += 5;
        if (sc->HasSCE(ROWOW_SC_INCREASEAGI) == true)
            agi += SCVal(sc, ROWOW_SC_INCREASEAGI, 2);
        if (sc->HasSCE(ROWOW_SC_INCREASING) == true)
            agi += 4; // Added based on skill updates [Reddozen]
        if (sc->HasSCE(ROWOW_SC_DECREASEAGI) == true)
            agi -= SCVal(sc, ROWOW_SC_DECREASEAGI, 2);
        if (sc->HasSCE(ROWOW_SC_QUAGMIRE) == true)
            agi -= SCVal(sc, ROWOW_SC_QUAGMIRE, 2);
        if (sc->HasSCE(ROWOW_SC_SUITON) == true && SCVal(sc, ROWOW_SC_SUITON, 3) != 0)
            agi -= SCVal(sc, ROWOW_SC_SUITON, 2);
        if (sc->HasSCE(ROWOW_SC_MARIONETTE) == true)
            agi -= (SCVal(sc, ROWOW_SC_MARIONETTE, 3) >> 8) & 0xFF;
        if (sc->HasSCE(ROWOW_SC_MARIONETTE2) == true)
            agi += (SCVal(sc, ROWOW_SC_MARIONETTE2, 3) >> 8) & 0xFF;
        if (sc->HasSCE(ROWOW_SC_SPIRIT) == true && SCVal(sc, ROWOW_SC_SPIRIT, 2) == ROWOW_SKILL_SL_HIGH)
            agi += (SCVal(sc, ROWOW_SC_SPIRIT, 3) >> 8) & 0xFF;
        if (sc->HasSCE(ROWOW_SC_STOMACHACHE) == true)
            agi -= SCVal(sc, ROWOW_SC_STOMACHACHE, 1);
        if (sc->HasSCE(ROWOW_SC_ALL_STAT_DOWN) == true)
            agi -= SCVal(sc, ROWOW_SC_ALL_STAT_DOWN, 2);
        return std::clamp(agi, 0, int32(USHRT_MAX));
    }

    // status_calc_vit
    int32 StatusCalcVit(Unit const* bl, ROWOWStatusChange const* sc, int32 vit)
    {
        if (sc == nullptr || sc->IsEmpty() == true)
            return std::clamp(vit, 0, int32(USHRT_MAX));
        if (sc->HasSCE(ROWOW_SC_INCALLSTATUS) == true)
            vit += SCVal(sc, ROWOW_SC_INCALLSTATUS, 1);
        if (sc->HasSCE(ROWOW_SC_CHANGE) == true)
            vit += SCVal(sc, ROWOW_SC_CHANGE, 2);
        if (sc->HasSCE(ROWOW_SC_GLORYWOUNDS) == true)
            vit += SCVal(sc, ROWOW_SC_GLORYWOUNDS, 1);
        if (sc->HasSCE(ROWOW_SC_TRUESIGHT) == true)
            vit += 5;
        if (sc->HasSCE(ROWOW_SC_MARIONETTE) == true)
            vit -= SCVal(sc, ROWOW_SC_MARIONETTE, 3) & 0xFF;
        if (sc->HasSCE(ROWOW_SC_MARIONETTE2) == true)
            vit += SCVal(sc, ROWOW_SC_MARIONETTE2, 3) & 0xFF;
        if (sc->HasSCE(ROWOW_SC_SPIRIT) == true && SCVal(sc, ROWOW_SC_SPIRIT, 2) == ROWOW_SKILL_SL_HIGH)
            vit += SCVal(sc, ROWOW_SC_SPIRIT, 3) & 0xFF;
        if (sc->HasSCE(ROWOW_SC_STOMACHACHE) == true)
            vit -= SCVal(sc, ROWOW_SC_STOMACHACHE, 1);
        if (sc->HasSCE(ROWOW_SC_STRIPARMOR) == true && bl->IsPlayer() == false)
            vit -= vit * SCVal(sc, ROWOW_SC_STRIPARMOR, 2) / 100;
        if (bl->IsPlayer() == true && sc->HasSCE(ROWOW_SC_DEFENCE) == true)
            vit += SCVal(sc, ROWOW_SC_DEFENCE, 2);
        if (sc->HasSCE(ROWOW_SC_ALL_STAT_DOWN) == true)
            vit -= SCVal(sc, ROWOW_SC_ALL_STAT_DOWN, 2);
        return std::clamp(vit, 0, int32(USHRT_MAX));
    }

    // status_calc_int
    int32 StatusCalcInt(Unit const* bl, ROWOWStatusChange const* sc, int32 int_)
    {
        if (sc == nullptr || sc->IsEmpty() == true)
            return std::clamp(int_, 0, int32(USHRT_MAX));
        if (sc->HasSCE(ROWOW_SC_INCALLSTATUS) == true)
            int_ += SCVal(sc, ROWOW_SC_INCALLSTATUS, 1);
        if (sc->HasSCE(ROWOW_SC_CHANGE) == true)
            int_ += SCVal(sc, ROWOW_SC_CHANGE, 3);
        if (sc->HasSCE(ROWOW_SC_BATTLEORDERS) == true)
            int_ += 5;
        if (sc->HasSCE(ROWOW_SC_TRUESIGHT) == true)
            int_ += 5;
        if (sc->HasSCE(ROWOW_SC_BLESSING) == true)
        {
            if (SCVal(sc, ROWOW_SC_BLESSING, 2) != 0)
                int_ += SCVal(sc, ROWOW_SC_BLESSING, 2);
            else
                int_ -= int_ / 2;
        }
        if (sc->HasSCE(ROWOW_SC_NEN) == true)
            int_ += SCVal(sc, ROWOW_SC_NEN, 1);
        if (sc->HasSCE(ROWOW_SC_MARIONETTE) == true)
            int_ -= (SCVal(sc, ROWOW_SC_MARIONETTE, 4) >> 16) & 0xFF;
        if (sc->HasSCE(ROWOW_SC_MARIONETTE2) == true)
            int_ += (SCVal(sc, ROWOW_SC_MARIONETTE2, 4) >> 16) & 0xFF;
        if (sc->HasSCE(ROWOW_SC_SPIRIT) == true && SCVal(sc, ROWOW_SC_SPIRIT, 2) == ROWOW_SKILL_SL_HIGH)
            int_ += (SCVal(sc, ROWOW_SC_SPIRIT, 4) >> 16) & 0xFF;
        if (sc->HasSCE(ROWOW_SC_STOMACHACHE) == true)
            int_ -= SCVal(sc, ROWOW_SC_STOMACHACHE, 1);
        if (bl->IsPlayer() == false && sc->HasSCE(ROWOW_SC_STRIPHELM) == true)
            int_ -= int_ * SCVal(sc, ROWOW_SC_STRIPHELM, 2) / 100;
        if (sc->HasSCE(ROWOW_SC_ALL_STAT_DOWN) == true)
            int_ -= SCVal(sc, ROWOW_SC_ALL_STAT_DOWN, 2);
        return std::clamp(int_, 0, int32(USHRT_MAX));
    }

    // status_calc_dex
    int32 StatusCalcDex(Unit const* /*bl*/, ROWOWStatusChange const* sc, int32 dex)
    {
        if (sc == nullptr || sc->IsEmpty() == true)
            return std::clamp(dex, 0, int32(USHRT_MAX));
        if (sc->HasSCE(ROWOW_SC_CONCENTRATE) == true && sc->HasSCE(ROWOW_SC_QUAGMIRE) == false)
            dex += (dex - SCVal(sc, ROWOW_SC_CONCENTRATE, 4)) * SCVal(sc, ROWOW_SC_CONCENTRATE, 2) / 100;
        if (sc->HasSCE(ROWOW_SC_INCALLSTATUS) == true)
            dex += SCVal(sc, ROWOW_SC_INCALLSTATUS, 1);
        if (sc->HasSCE(ROWOW_SC_BATTLEORDERS) == true)
            dex += 5;
        if (sc->HasSCE(ROWOW_SC_HAWKEYES) == true)
            dex += SCVal(sc, ROWOW_SC_HAWKEYES, 1);
        if (sc->HasSCE(ROWOW_SC_TRUESIGHT) == true)
            dex += 5;
        if (sc->HasSCE(ROWOW_SC_QUAGMIRE) == true)
            dex -= SCVal(sc, ROWOW_SC_QUAGMIRE, 2);
        if (sc->HasSCE(ROWOW_SC_BLESSING) == true)
        {
            if (SCVal(sc, ROWOW_SC_BLESSING, 2) != 0)
                dex += SCVal(sc, ROWOW_SC_BLESSING, 2);
            else
                dex -= dex / 2;
        }
        if (sc->HasSCE(ROWOW_SC_INCREASING) == true)
            dex += 4; // Added based on skill updates [Reddozen]
        if (sc->HasSCE(ROWOW_SC_MARIONETTE) == true)
            dex -= (SCVal(sc, ROWOW_SC_MARIONETTE, 4) >> 8) & 0xFF;
        if (sc->HasSCE(ROWOW_SC_MARIONETTE2) == true)
            dex += (SCVal(sc, ROWOW_SC_MARIONETTE2, 4) >> 8) & 0xFF;
        if (sc->HasSCE(ROWOW_SC_SPIRIT) == true && SCVal(sc, ROWOW_SC_SPIRIT, 2) == ROWOW_SKILL_SL_HIGH)
            dex += (SCVal(sc, ROWOW_SC_SPIRIT, 4) >> 8) & 0xFF;
        if (sc->HasSCE(ROWOW_SC_STOMACHACHE) == true)
            dex -= SCVal(sc, ROWOW_SC_STOMACHACHE, 1);
        if (sc->HasSCE(ROWOW_SC_ALL_STAT_DOWN) == true)
            dex -= SCVal(sc, ROWOW_SC_ALL_STAT_DOWN, 2);
        return std::clamp(dex, 0, int32(USHRT_MAX));
    }

    // status_calc_luk
    int32 StatusCalcLuk(Unit const* /*bl*/, ROWOWStatusChange const* sc, int32 luk)
    {
        if (sc == nullptr || sc->IsEmpty() == true)
            return std::clamp(luk, 0, int32(USHRT_MAX));
        if (sc->HasSCE(ROWOW_SC_CURSE) == true)
            return 0;
        if (sc->HasSCE(ROWOW_SC_INCALLSTATUS) == true)
            luk += SCVal(sc, ROWOW_SC_INCALLSTATUS, 1);
        if (sc->HasSCE(ROWOW_SC_TRUESIGHT) == true)
            luk += 5;
        if (sc->HasSCE(ROWOW_SC_GLORIA) == true)
            luk += 30;
        if (sc->HasSCE(ROWOW_SC_MARIONETTE) == true)
            luk -= SCVal(sc, ROWOW_SC_MARIONETTE, 4) & 0xFF;
        if (sc->HasSCE(ROWOW_SC_MARIONETTE2) == true)
            luk += SCVal(sc, ROWOW_SC_MARIONETTE2, 4) & 0xFF;
        if (sc->HasSCE(ROWOW_SC_SPIRIT) == true && SCVal(sc, ROWOW_SC_SPIRIT, 2) == ROWOW_SKILL_SL_HIGH)
            luk += SCVal(sc, ROWOW_SC_SPIRIT, 4) & 0xFF;
        if (sc->HasSCE(ROWOW_SC_STOMACHACHE) == true)
            luk -= SCVal(sc, ROWOW_SC_STOMACHACHE, 1);
        if (sc->HasSCE(ROWOW_SC_ALL_STAT_DOWN) == true)
            luk -= SCVal(sc, ROWOW_SC_ALL_STAT_DOWN, 2);
        return std::clamp(luk, 0, int32(USHRT_MAX));
    }

    // status_calc_batk
    int32 StatusCalcBatk(Unit const* /*bl*/, ROWOWStatusChange const* sc, int32 batk)
    {
        if (sc == nullptr || sc->IsEmpty() == true)
            return batk;
        if (sc->HasSCE(ROWOW_SC_VOLCANO) == true)
            batk += SCVal(sc, ROWOW_SC_VOLCANO, 2);
        if (sc->HasSCE(ROWOW_SC_GATLINGFEVER) == true)
            batk += SCVal(sc, ROWOW_SC_GATLINGFEVER, 3);
        if (sc->HasSCE(ROWOW_SC_MADNESSCANCEL) == true)
            batk += 100;
        return batk;
    }

    // status_calc_watk
    int32 StatusCalcWatk(Unit const* bl, ROWOWStatusChange const* sc, int32 watk, bool isLeftHand)
    {
        if (sc == nullptr || sc->IsEmpty() == true)
            return std::clamp(watk, 0, int32(USHRT_MAX));
        if (sc->HasSCE(ROWOW_SC_IMPOSITIO) == true)
            watk += SCVal(sc, ROWOW_SC_IMPOSITIO, 2);
        if (sc->HasSCE(ROWOW_SC_WATKFOOD) == true)
            watk += SCVal(sc, ROWOW_SC_WATKFOOD, 1);
        if (sc->HasSCE(ROWOW_SC_VOLCANO) == true && bl->IsCreature() == true)
            watk += SCVal(sc, ROWOW_SC_VOLCANO, 2);
        if (sc->HasSCE(ROWOW_SC_MERC_ATKUP) == true)
            watk += SCVal(sc, ROWOW_SC_MERC_ATKUP, 2);
        if (sc->HasSCE(ROWOW_SC_NIBELUNGEN) == true)
        {
            if (bl->IsPlayer() == false)
                watk += SCVal(sc, ROWOW_SC_NIBELUNGEN, 2);
            else
            {
                // Only on a level 4 weapon in that hand
                ROWOWStatus const* status = ROWOW->GetROBaseStatus(bl);
                if (status != nullptr && (isLeftHand == true ? status->LeftWeaponLevel : status->WeaponLevel) == 4)
                    watk += SCVal(sc, ROWOW_SC_NIBELUNGEN, 2);
            }
        }
        return std::clamp(watk, 0, int32(USHRT_MAX));
    }

    // status_calc_critical
    int32 StatusCalcCritical(Unit const* /*bl*/, ROWOWStatusChange const* sc, int32 critical)
    {
        if (sc == nullptr || sc->IsEmpty() == true)
            return std::clamp(critical, 10, int32(SHRT_MAX));
        if (sc->HasSCE(ROWOW_SC_EXPLOSIONSPIRITS) == true)
            critical += SCVal(sc, ROWOW_SC_EXPLOSIONSPIRITS, 2);
        if (sc->HasSCE(ROWOW_SC_FORTUNE) == true)
            critical += SCVal(sc, ROWOW_SC_FORTUNE, 2);
        if (sc->HasSCE(ROWOW_SC_TRUESIGHT) == true)
            critical += SCVal(sc, ROWOW_SC_TRUESIGHT, 2);
        if (sc->HasSCE(ROWOW_SC_CLOAKING) == true)
            critical += critical;
        if (sc->HasSCE(ROWOW_SC_INCCRI) == true)
            critical += SCVal(sc, ROWOW_SC_INCCRI, 2);
        return std::clamp(critical, 10, int32(SHRT_MAX));
    }

    // status_calc_hit
    int32 StatusCalcHit(Unit const* /*bl*/, ROWOWStatusChange const* sc, int32 hit)
    {
        if (sc == nullptr || sc->IsEmpty() == true)
            return std::clamp(hit, 1, int32(SHRT_MAX));
        if (sc->HasSCE(ROWOW_SC_INCHIT) == true)
            hit += SCVal(sc, ROWOW_SC_INCHIT, 1);
        if (sc->HasSCE(ROWOW_SC_TRUESIGHT) == true)
            hit += SCVal(sc, ROWOW_SC_TRUESIGHT, 3);
        if (sc->HasSCE(ROWOW_SC_HUMMING) == true)
            hit += SCVal(sc, ROWOW_SC_HUMMING, 2);
        if (sc->HasSCE(ROWOW_SC_CONCENTRATION) == true)
            hit += SCVal(sc, ROWOW_SC_CONCENTRATION, 3);
        if (sc->HasSCE(ROWOW_SC_ADJUSTMENT) == true)
            hit -= 30;
        if (sc->HasSCE(ROWOW_SC_INCREASING) == true)
            hit += 20; // RockmanEXE; changed based on updated [Reddozen]
        if (sc->HasSCE(ROWOW_SC_MERC_HITUP) == true)
            hit += SCVal(sc, ROWOW_SC_MERC_HITUP, 2);
        if (sc->HasSCE(ROWOW_SC_INCHITRATE) == true)
            hit += hit * SCVal(sc, ROWOW_SC_INCHITRATE, 1) / 100;
        if (sc->HasSCE(ROWOW_SC_POWERUP) == true)
            hit += hit * SCVal(sc, ROWOW_SC_POWERUP, 2) / 100;
        if (sc->HasSCE(ROWOW_SC_BLIND) == true)
            hit -= hit * 25 / 100;
        if (sc->HasSCE(ROWOW_SC_FEAR) == true)
            hit -= hit * 20 / 100;
        return std::clamp(hit, 1, int32(SHRT_MAX));
    }

    // status_calc_flee.  (The War of Emperium and battleground flee penalties don't apply: there are none here)
    int32 StatusCalcFlee(Unit const* /*bl*/, ROWOWStatusChange const* sc, int32 flee)
    {
        if (sc == nullptr || sc->IsEmpty() == true)
            return std::clamp(flee, 1, int32(SHRT_MAX));

        // Fixed value
        if (sc->HasSCE(ROWOW_SC_INCFLEE) == true)
            flee += SCVal(sc, ROWOW_SC_INCFLEE, 1);
        if (sc->HasSCE(ROWOW_SC_WHISTLE) == true)
            flee += SCVal(sc, ROWOW_SC_WHISTLE, 2);
        if (sc->HasSCE(ROWOW_SC_WINDWALK) == true)
            flee += SCVal(sc, ROWOW_SC_WINDWALK, 2);
        if (sc->HasSCE(ROWOW_SC_VIOLENTGALE) == true)
            flee += SCVal(sc, ROWOW_SC_VIOLENTGALE, 2);
        if (sc->HasSCE(ROWOW_SC_MOON_COMFORT) == true) // SG skill [Komurka]
            flee += SCVal(sc, ROWOW_SC_MOON_COMFORT, 2);
        if (sc->HasSCE(ROWOW_SC_CLOSECONFINE) == true)
            flee += SCVal(sc, ROWOW_SC_CLOSECONFINE, 3);
        if (sc->HasSCE(ROWOW_SC_ADJUSTMENT) == true)
            flee += 30;
        if (sc->HasSCE(ROWOW_SC_SPEED) == true)
            flee += 10 + SCVal(sc, ROWOW_SC_SPEED, 1) * 10;
        if (sc->HasSCE(ROWOW_SC_GATLINGFEVER) == true)
            flee -= SCVal(sc, ROWOW_SC_GATLINGFEVER, 4);
        if (sc->HasSCE(ROWOW_SC_PARTYFLEE) == true)
            flee += SCVal(sc, ROWOW_SC_PARTYFLEE, 1) * 10;
        if (sc->HasSCE(ROWOW_SC_MERC_FLEEUP) == true)
            flee += SCVal(sc, ROWOW_SC_MERC_FLEEUP, 2);

        // Rate value
        if (sc->HasSCE(ROWOW_SC_INCFLEERATE) == true)
            flee += flee * SCVal(sc, ROWOW_SC_INCFLEERATE, 1) / 100;
        if (sc->HasSCE(ROWOW_SC_AGIUP) == true)
            flee += flee * SCVal(sc, ROWOW_SC_AGIUP, 2) / 100;
        if (sc->HasSCE(ROWOW_SC_SPIDERWEB) == true)
            flee -= flee * 50 / 100;
        if (sc->HasSCE(ROWOW_SC_BERSERK) == true)
            flee -= flee * 50 / 100;
        if (sc->HasSCE(ROWOW_SC_BLIND) == true)
            flee -= flee * 25 / 100;
        if (sc->HasSCE(ROWOW_SC_FEAR) == true)
            flee -= flee * 20 / 100;
        if (sc->HasSCE(ROWOW_SC_PARALYSE) == true && SCVal(sc, ROWOW_SC_PARALYSE, 3) == 1)
            flee -= flee * 10 / 100;
        return std::clamp(flee, 1, int32(SHRT_MAX));
    }

    // status_calc_flee2
    int32 StatusCalcFlee2(Unit const* /*bl*/, ROWOWStatusChange const* sc, int32 flee2)
    {
        if (sc == nullptr || sc->IsEmpty() == true)
            return std::clamp(flee2, 10, int32(SHRT_MAX));
        if (sc->HasSCE(ROWOW_SC_WHISTLE) == true)
            flee2 += SCVal(sc, ROWOW_SC_WHISTLE, 3);
        return std::clamp(flee2, 10, int32(SHRT_MAX));
    }

    // status_calc_def (DEFTYPE_MIN/MAX are a signed char's in pre-renewal)
    int32 StatusCalcDef(Unit const* bl, ROWOWStatusChange const* sc, int32 def)
    {
        if (sc == nullptr || sc->IsEmpty() == true)
            return std::clamp(def, int32(CHAR_MIN), int32(CHAR_MAX));
        if (sc->HasSCE(ROWOW_SC_BERSERK) == true)
            return 0;
        if (sc->HasSCE(ROWOW_SC_KEEPING) == true)
            return 90;
        if (sc->HasSCE(ROWOW_SC_STEELBODY) == true)
            return 90;
        if (sc->HasSCE(ROWOW_SC_DEFSET) == true)
            return SCVal(sc, ROWOW_SC_DEFSET, 1);
        if (sc->HasSCE(ROWOW_SC_DRUMBATTLE) == true)
            def += SCVal(sc, ROWOW_SC_DRUMBATTLE, 3);
        if (sc->HasSCE(ROWOW_SC_INCDEFRATE) == true)
            def += def * SCVal(sc, ROWOW_SC_INCDEFRATE, 1) / 100;
        if (sc->HasSCE(ROWOW_SC_STONE) == true)
            def /= 2;
        if (sc->HasSCE(ROWOW_SC_FREEZE) == true)
            def /= 2;
        if ((sc->HasSCE(ROWOW_SC_POISON) == true || sc->HasSCE(ROWOW_SC_DPOISON) == true) && bl->IsPlayer() == false)
            def = def * 75 / 100; // Should round down
        if (sc->HasSCE(ROWOW_SC_SIGNUMCRUCIS) == true)
            def -= def * SCVal(sc, ROWOW_SC_SIGNUMCRUCIS, 2) / 100;
        if (sc->HasSCE(ROWOW_SC_CONCENTRATION) == true)
            def -= def * SCVal(sc, ROWOW_SC_CONCENTRATION, 4) / 100;
        if (sc->HasSCE(ROWOW_SC_SKE) == true)
            def /= 2;
        if (sc->HasSCE(ROWOW_SC_PROVOKE) == true && bl->IsPlayer() == false) // Provoke doesn't alter player defense->
            def -= def * SCVal(sc, ROWOW_SC_PROVOKE, 3) / 100;
        if (sc->HasSCE(ROWOW_SC_STRIPSHIELD) == true && bl->IsPlayer() == false) // Player doesn't have def reduction only equip removed
            def -= def * SCVal(sc, ROWOW_SC_STRIPSHIELD, 2) / 100;
        if (sc->HasSCE(ROWOW_SC_FLING) == true)
            def -= def * SCVal(sc, ROWOW_SC_FLING, 2) / 100;
        return std::clamp(def, int32(CHAR_MIN), int32(CHAR_MAX));
    }

    // status_calc_def2
    int32 StatusCalcDef2(Unit const* /*bl*/, ROWOWStatusChange const* sc, int32 def2)
    {
        if (sc == nullptr || sc->IsEmpty() == true)
            return std::clamp(def2, 1, int32(SHRT_MAX));
        if (sc->HasSCE(ROWOW_SC_BERSERK) == true)
            return 0;
        if (sc->HasSCE(ROWOW_SC_ETERNALCHAOS) == true)
            return 0;
        if (sc->HasSCE(ROWOW_SC_DEFSET) == true)
            return SCVal(sc, ROWOW_SC_DEFSET, 1);
        if (sc->HasSCE(ROWOW_SC_SUN_COMFORT) == true)
            def2 += SCVal(sc, ROWOW_SC_SUN_COMFORT, 2);
        if (sc->HasSCE(ROWOW_SC_ANGELUS) == true)
            def2 += def2 * SCVal(sc, ROWOW_SC_ANGELUS, 2) / 100;
        if (sc->HasSCE(ROWOW_SC_CONCENTRATION) == true)
            def2 -= def2 * SCVal(sc, ROWOW_SC_CONCENTRATION, 4) / 100;
        if (sc->HasSCE(ROWOW_SC_POISON) == true || sc->HasSCE(ROWOW_SC_DPOISON) == true)
            def2 = def2 * 75 / 100; // Should round down
        if (sc->HasSCE(ROWOW_SC_SKE) == true)
            def2 -= def2 * 50 / 100;
        if (sc->HasSCE(ROWOW_SC_PROVOKE) == true)
            def2 -= def2 * SCVal(sc, ROWOW_SC_PROVOKE, 3) / 100;
        if (sc->HasSCE(ROWOW_SC_JOINTBEAT) == true)
        {
            int32 jointBeat = SCVal(sc, ROWOW_SC_JOINTBEAT, 2);
            def2 -= def2 * ((jointBeat & ROWOW_BREAK_SHOULDER) != 0 ? 50 : 0) / 100 + def2 * ((jointBeat & ROWOW_BREAK_WAIST) != 0 ? 25 : 0) / 100;
        }
        if (sc->HasSCE(ROWOW_SC_FLING) == true)
            def2 -= def2 * SCVal(sc, ROWOW_SC_FLING, 3) / 100;
        return std::clamp(def2, 1, int32(SHRT_MAX));
    }

    // status_calc_mdef
    int32 StatusCalcMdef(Unit const* /*bl*/, ROWOWStatusChange const* sc, int32 mdef)
    {
        if (sc == nullptr || sc->IsEmpty() == true)
            return std::clamp(mdef, int32(CHAR_MIN), int32(CHAR_MAX));
        if (sc->HasSCE(ROWOW_SC_BERSERK) == true)
            return 0;
        if (sc->HasSCE(ROWOW_SC_STEELBODY) == true)
            return 90;
        if (sc->HasSCE(ROWOW_SC_MDEFSET) == true)
            return SCVal(sc, ROWOW_SC_MDEFSET, 1);
        if (ROWOWStatusChangeEntry const* endure = sc->GetSCE(ROWOW_SC_ENDURE); endure != nullptr && endure->Val3 == 0) // It has been confirmed that Eddga card grants 1 MDEF, not 0, not 10, but 1.
            mdef += (endure->Val4 == 0) ? endure->Val1 : 1;
        if (sc->HasSCE(ROWOW_SC_STONE) == true)
            mdef += 25 * mdef / 100;
        if (sc->HasSCE(ROWOW_SC_FREEZE) == true)
            mdef += 25 * mdef / 100;
        if (sc->HasSCE(ROWOW_SC_BURNING) == true)
            mdef -= 25 * mdef / 100;
        return std::clamp(mdef, int32(CHAR_MIN), int32(CHAR_MAX));
    }

    // status_calc_mdef2
    int32 StatusCalcMdef2(Unit const* /*bl*/, ROWOWStatusChange const* sc, int32 mdef2)
    {
        if (sc == nullptr || sc->IsEmpty() == true)
            return std::clamp(mdef2, 1, int32(SHRT_MAX));
        if (sc->HasSCE(ROWOW_SC_BERSERK) == true)
            return 0;
        if (sc->HasSCE(ROWOW_SC_SKA) == true)
            return 90;
        if (sc->HasSCE(ROWOW_SC_MDEFSET) == true)
            return SCVal(sc, ROWOW_SC_MDEFSET, 1);
        if (sc->HasSCE(ROWOW_SC_MINDBREAKER) == true)
            mdef2 -= mdef2 * SCVal(sc, ROWOW_SC_MINDBREAKER, 3) / 100;
        if (sc->HasSCE(ROWOW_SC_BURNING) == true)
            mdef2 -= mdef2 * 25 / 100;
        return std::clamp(mdef2, 1, int32(SHRT_MAX));
    }

    // status_calc_speed: the walk speed (ms per cell), from the statuses that slow and speed it, riding a Peco, and casting with Free Cast
    int32 StatusCalcSpeed(Unit const* bl, ROWOWStatusChange const* sc, ROWOWBonus const& bonus, int32 speed)
    {
        // A player riding a Peco is hastened with no statuses too: its statuses are then none rather than missing
        static ROWOWStatusChange const NoStatusChange;
        bool isPlayer = bl->IsPlayer();
        bool isRiding = isPlayer == true && ROWOW->PCIsRiding(bl) == true;
        ROWOWSkillUnitData const* ud = isPlayer == true ? bl->CustomData.Get<ROWOWSkillUnitData>("ROWOWSkillUnitData") : nullptr;
        int32 freeCast = (ud != nullptr && ud->IsSkillTimer == true) ? ROWOW->PCCheckSkill(bl, ROWOW_SKILL_SA_FREECAST) : 0;
        int32 speedRate = 100;
        if (sc == nullptr)
        {
            if (isRiding == false && freeCast == 0)
                return std::clamp(speed, ROWOW_MIN_WALK_SPEED, ROWOW_MAX_WALK_SPEED);
            sc = &NoStatusChange;
        }
        int32 itemSpeedRate = isPlayer == true ? GetDeferredBonusSum(bonus, "bSpeedRate") + GetDeferredBonusSum(bonus, "bSpeedAddRate") : 0;

        // A Sage casting with Free Cast walks at its own rate, in place of every haste and slowness
        if (freeCast > 0)
            speedRate = 175 - 5 * freeCast;
        else
        {
            // GetMoveHasteValue2()
            int32 val = 0;
            if (sc->HasSCE(ROWOW_SC_FUSION) == true)
                val = 25;
            else if (isRiding == true)
                val = 25; // (Same bonus as a dragon's, a third job's)
            speedRate -= val;

            // GetMoveSlowValue()
            if (isPlayer == true && sc->HasSCE(ROWOW_SC_HIDING) == true && ROWOW->PCCheckSkill(bl, ROWOW_SKILL_RG_TUNNELDRIVE) > 0)
                val = 120 - 6 * ROWOW->PCCheckSkill(bl, ROWOW_SKILL_RG_TUNNELDRIVE);
            else if (isPlayer == true && sc->HasSCE(ROWOW_SC_CHASEWALK) == true && SCVal(sc, ROWOW_SC_CHASEWALK, 3) < 0)
                val = SCVal(sc, ROWOW_SC_CHASEWALK, 3);
            else
            {
                val = 0;
                // Longing for Freedom/Special Singer cancels song/dance penalty
                if (sc->HasSCE(ROWOW_SC_LONGING) == true)
                    val = std::max(val, 50 - 10 * SCVal(sc, ROWOW_SC_LONGING, 1));
                else if (isPlayer == true && sc->HasSCE(ROWOW_SC_DANCING) == true)
                {
                    ROWOWStatusChangeEntry const* spirit = sc->GetSCE(ROWOW_SC_SPIRIT);
                    int32 lessonSkill = ROWOW->PCCheckSkill(bl, ROWOW_SKILL_BA_MUSICALLESSON) + ROWOW->PCCheckSkill(bl, ROWOW_SKILL_DC_DANCINGLESSON);
                    val = std::max(val, 500 - (40 + 10 * (spirit != nullptr && spirit->Val2 == ROWOW_SKILL_SL_BARDDANCER ? 1 : 0)) * lessonSkill);
                }
                if (sc->HasSCE(ROWOW_SC_DECREASEAGI) == true)
                    val = std::max(val, 25);
                if (sc->HasSCE(ROWOW_SC_QUAGMIRE) == true)
                    val = std::max(val, 50);
                if (sc->HasSCE(ROWOW_SC_DONTFORGETME) == true)
                    val = std::max(val, SCVal(sc, ROWOW_SC_DONTFORGETME, 3));
                if (sc->HasSCE(ROWOW_SC_CURSE) == true)
                    val = std::max(val, 300);
                if (sc->HasSCE(ROWOW_SC_CHASEWALK) == true)
                    val = std::max(val, SCVal(sc, ROWOW_SC_CHASEWALK, 3));
                if (sc->HasSCE(ROWOW_SC_WEDDING) == true)
                    val = std::max(val, 100);
                if (sc->HasSCE(ROWOW_SC_JOINTBEAT) == true && (SCVal(sc, ROWOW_SC_JOINTBEAT, 2) & (ROWOW_BREAK_ANKLE | ROWOW_BREAK_KNEE)) != 0)
                {
                    int32 jointBeat = SCVal(sc, ROWOW_SC_JOINTBEAT, 2);
                    val = std::max(val, ((jointBeat & ROWOW_BREAK_ANKLE) != 0 ? 50 : 0) + ((jointBeat & ROWOW_BREAK_KNEE) != 0 ? 30 : 0));
                }
                if (ROWOWStatusChangeEntry const* cloaking = sc->GetSCE(ROWOW_SC_CLOAKING); cloaking != nullptr && (cloaking->Val4 & 1) == 0)
                    val = std::max(val, cloaking->Val1 < 3 ? 300 : 30 - 3 * cloaking->Val1);
                if (sc->HasSCE(ROWOW_SC_GOSPEL) == true && SCVal(sc, ROWOW_SC_GOSPEL, 4) == ROWOW_BCT_ENEMY)
                    val = std::max(val, 75);
                if (sc->HasSCE(ROWOW_SC_SLOWDOWN) == true) // Slow Potion
                    val = std::max(val, SCVal(sc, ROWOW_SC_SLOWDOWN, 1));
                if (sc->HasSCE(ROWOW_SC_GATLINGFEVER) == true)
                    val = std::max(val, 100);
                if (sc->HasSCE(ROWOW_SC_SUITON) == true)
                    val = std::max(val, SCVal(sc, ROWOW_SC_SUITON, 3));
                if (sc->HasSCE(ROWOW_SC_SWOO) == true)
                    val = std::max(val, 300);
                if (isPlayer == true && itemSpeedRate > 0) // Permanent item-based speedup
                    val = std::max(val, itemSpeedRate);
            }
            speedRate += val;
            val = 0;

            // GetMoveHasteValue1()
            if (sc->HasSCE(ROWOW_SC_AGIUP) == true)
                val = std::max(val, SCVal(sc, ROWOW_SC_AGIUP, 1));
            if (sc->HasSCE(ROWOW_SC_INCREASEAGI) == true)
                val = std::max(val, 25);
            if (sc->HasSCE(ROWOW_SC_WINDWALK) == true)
                val = std::max(val, 2 * SCVal(sc, ROWOW_SC_WINDWALK, 1));
            if (sc->HasSCE(ROWOW_SC_CARTBOOST) == true)
                val = std::max(val, 20);
            if (isPlayer == true && ROWOW->PCCheckSkill(bl, ROWOW_SKILL_TF_MISS) > 0)
            {
                // An Assassin's Improve Dodge also hastens (MAPID_ASSASSIN)
                ROWOWIdentity const* identity = bl->ToPlayer()->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
                ROWOWJob const* job = identity != nullptr ? ROWOW->GetJob(identity->JobID) : nullptr;
                if (job != nullptr && job->IllusionJobKey.find("assassin") != std::string::npos)
                    val = std::max(val, 1 * ROWOW->PCCheckSkill(bl, ROWOW_SKILL_TF_MISS));
            }
            if (ROWOWStatusChangeEntry const* cloaking = sc->GetSCE(ROWOW_SC_CLOAKING); cloaking != nullptr && (cloaking->Val4 & 1) == 1)
                val = std::max(val, cloaking->Val1 >= 10 ? 25 : 3 * cloaking->Val1 - 3);
            if (sc->HasSCE(ROWOW_SC_BERSERK) == true)
                val = std::max(val, 25);
            if (sc->HasSCE(ROWOW_SC_RUN) == true)
                val = std::max(val, 55);
            if (sc->HasSCE(ROWOW_SC_AVOID) == true)
                val = std::max(val, SCVal(sc, ROWOW_SC_AVOID, 2));
            if (sc->HasSCE(ROWOW_SC_INVINCIBLE) == true)
                val = std::max(val, SCVal(sc, ROWOW_SC_INVINCIBLE, 3));
            if (sc->HasSCE(ROWOW_SC_PARALYSE) == true && SCVal(sc, ROWOW_SC_PARALYSE, 3) == 0)
                val = std::max(val, 50);
            if (isPlayer == true && itemSpeedRate < 0) // Permanent item-based speedup
                val = std::max(val, -itemSpeedRate);
            speedRate -= val;
            if (speedRate < 40)
                speedRate = 40;
        }

        // GetSpeed()
        if (isPlayer == true && ROWOW->PCIsCartOn(bl) == true)
            speed += speed * (50 - 5 * ROWOW->PCCheckSkill(bl, ROWOW_SKILL_MC_PUSHCART)) / 100;
        if (sc->HasSCE(ROWOW_SC_PARALYSE) == true && SCVal(sc, ROWOW_SC_PARALYSE, 3) == 1)
            speed += speed * 50 / 100;
        if (speedRate != 100)
            speed = speed * speedRate / 100;
        if (sc->HasSCE(ROWOW_SC_STEELBODY) == true)
            speed = 200;
        if (sc->HasSCE(ROWOW_SC_DEFENDER) == true)
            speed = std::max(speed, 200);
        if (sc->HasSCE(ROWOW_SC_ARMOR) == true)
            speed = std::max(speed, 200);
        if (sc->HasSCE(ROWOW_SC_WALKSPEED) == true && SCVal(sc, ROWOW_SC_WALKSPEED, 1) > 0) // ChangeSpeed
            speed = speed * 100 / SCVal(sc, ROWOW_SC_WALKSPEED, 1);
        return std::clamp(speed, ROWOW_MIN_WALK_SPEED, ROWOW_MAX_WALK_SPEED);
    }

    // status_calc_fix_aspd
    int32 StatusCalcFixAspd(Unit const* /*bl*/, ROWOWStatusChange const* sc, int32 aspd)
    {
        if (sc == nullptr || sc->IsEmpty() == true)
            return std::clamp(aspd, 1, ROWOW_MIN_ASPD);
        if (sc->HasSCE(ROWOW_SC_FIGHTINGSPIRIT) == true)
            aspd -= SCVal(sc, ROWOW_SC_FIGHTINGSPIRIT, 2);
        return std::clamp(aspd, 1, ROWOW_MIN_ASPD); // Will be recap for proper bl anyway
    }

    // status_calc_aspd_rate: the attack delay's rate, in tenths of a percent.  The fastest quickening counts, then the rest add up
    int32 StatusCalcAspdRate(Unit const* bl, ROWOWStatusChange const* sc, uint8 weaponType, int32 aspdRate)
    {
        if (sc == nullptr || sc->IsEmpty() == true)
            return std::clamp(aspdRate, 0, int32(SHRT_MAX));
        int32 max = 0;
        if (sc->HasSCE(ROWOW_SC_STAR_COMFORT) == true)
            max = SCVal(sc, ROWOW_SC_STAR_COMFORT, 2);
        if (sc->HasSCE(ROWOW_SC_TWOHANDQUICKEN) == true && max < SCVal(sc, ROWOW_SC_TWOHANDQUICKEN, 2))
            max = SCVal(sc, ROWOW_SC_TWOHANDQUICKEN, 2);
        if (sc->HasSCE(ROWOW_SC_ONEHAND) == true && max < SCVal(sc, ROWOW_SC_ONEHAND, 2))
            max = SCVal(sc, ROWOW_SC_ONEHAND, 2);
        if (sc->HasSCE(ROWOW_SC_MERC_QUICKEN) == true && max < SCVal(sc, ROWOW_SC_MERC_QUICKEN, 2))
            max = SCVal(sc, ROWOW_SC_MERC_QUICKEN, 2);
        if (sc->HasSCE(ROWOW_SC_ADRENALINE2) == true && max < SCVal(sc, ROWOW_SC_ADRENALINE2, 3))
            max = SCVal(sc, ROWOW_SC_ADRENALINE2, 3);
        if (sc->HasSCE(ROWOW_SC_ADRENALINE) == true && max < SCVal(sc, ROWOW_SC_ADRENALINE, 3))
            max = SCVal(sc, ROWOW_SC_ADRENALINE, 3);
        if (sc->HasSCE(ROWOW_SC_SPEARQUICKEN) == true && max < SCVal(sc, ROWOW_SC_SPEARQUICKEN, 2))
            max = SCVal(sc, ROWOW_SC_SPEARQUICKEN, 2);
        if (sc->HasSCE(ROWOW_SC_GATLINGFEVER) == true && max < SCVal(sc, ROWOW_SC_GATLINGFEVER, 2))
            max = SCVal(sc, ROWOW_SC_GATLINGFEVER, 2);
        if (sc->HasSCE(ROWOW_SC_FLEET) == true && max < SCVal(sc, ROWOW_SC_FLEET, 2))
            max = SCVal(sc, ROWOW_SC_FLEET, 2);
        if (sc->HasSCE(ROWOW_SC_INVINCIBLE) == true && max < SCVal(sc, ROWOW_SC_INVINCIBLE, 4))
            max = SCVal(sc, ROWOW_SC_INVINCIBLE, 4);
        if (sc->HasSCE(ROWOW_SC_ASSNCROS) == true && max < SCVal(sc, ROWOW_SC_ASSNCROS, 2))
        {
            // Not with a bow or a gun
            bool isRanged = weaponType == ROWOW_WEAPON_TYPE_BOW || (weaponType >= ROWOW_WEAPON_TYPE_REVOLVER && weaponType <= ROWOW_WEAPON_TYPE_GRENADE);
            if (bl->IsPlayer() == false || isRanged == false)
                max = SCVal(sc, ROWOW_SC_ASSNCROS, 2);
        }
        aspdRate -= max;
        if (sc->HasSCE(ROWOW_SC_BERSERK) == true)
            aspdRate -= 300;
        else if (sc->HasSCE(ROWOW_SC_MADNESSCANCEL) == true)
            aspdRate -= 200;
        for (int32 potion : { ROWOW_SC_ASPDPOTION3, ROWOW_SC_ASPDPOTION2, ROWOW_SC_ASPDPOTION1, ROWOW_SC_ASPDPOTION0 })
        {
            if (sc->HasSCE(potion) == true)
            {
                aspdRate -= SCVal(sc, potion, 2);
                break;
            }
        }
        if (bl->IsPlayer() == true && sc->HasSCE(ROWOW_SC_DONTFORGETME) == true)
            aspdRate += SCVal(sc, ROWOW_SC_DONTFORGETME, 2);
        if (sc->HasSCE(ROWOW_SC_LONGING) == true)
            aspdRate += SCVal(sc, ROWOW_SC_LONGING, 2);
        if (sc->HasSCE(ROWOW_SC_STEELBODY) == true)
            aspdRate += 250;
        if (sc->HasSCE(ROWOW_SC_DEFENDER) == true)
            aspdRate += SCVal(sc, ROWOW_SC_DEFENDER, 4);
        if (sc->HasSCE(ROWOW_SC_GOSPEL) == true && SCVal(sc, ROWOW_SC_GOSPEL, 4) == ROWOW_BCT_ENEMY)
            aspdRate += 250;
        if (sc->HasSCE(ROWOW_SC_GRAVITATION) == true)
            aspdRate += SCVal(sc, ROWOW_SC_GRAVITATION, 2);
        if (sc->HasSCE(ROWOW_SC_JOINTBEAT) == true)
        {
            if ((SCVal(sc, ROWOW_SC_JOINTBEAT, 2) & ROWOW_BREAK_WRIST) != 0)
                aspdRate += 250;
            if ((SCVal(sc, ROWOW_SC_JOINTBEAT, 2) & ROWOW_BREAK_KNEE) != 0)
                aspdRate += 100;
        }
        if (sc->HasSCE(ROWOW_SC_PARALYSE) == true && SCVal(sc, ROWOW_SC_PARALYSE, 3) == 1)
            aspdRate += 100;
        if (sc->HasSCE(ROWOW_SC_INCASPDRATE) == true)
            aspdRate -= SCVal(sc, ROWOW_SC_INCASPDRATE, 1) * 10;
        return std::clamp(aspdRate, 0, int32(SHRT_MAX));
    }

    // status_calc_element
    int32 StatusCalcElement(ROWOWStatusChange const* sc, int32 element)
    {
        if (sc == nullptr || sc->IsEmpty() == true)
            return std::clamp(element, 0, int32(UCHAR_MAX));
        if (sc->HasSCE(ROWOW_SC_FREEZE) == true)
            return ROWOW_ELEMENT_WATER;
        if (sc->HasSCE(ROWOW_SC_STONE) == true)
            return ROWOW_ELEMENT_EARTH;
        if (sc->HasSCE(ROWOW_SC_BENEDICTIO) == true)
            return ROWOW_ELEMENT_HOLY;
        if (sc->HasSCE(ROWOW_SC_CHANGEUNDEAD) == true)
            return ROWOW_ELEMENT_UNDEAD;
        if (sc->HasSCE(ROWOW_SC_ELEMENTALCHANGE) == true)
            return SCVal(sc, ROWOW_SC_ELEMENTALCHANGE, 2);
        return std::clamp(element, 0, int32(UCHAR_MAX));
    }

    // status_calc_element_lv
    int32 StatusCalcElementLevel(ROWOWStatusChange const* sc, int32 level)
    {
        if (sc == nullptr || sc->IsEmpty() == true)
            return std::clamp(level, 1, 4);
        if (sc->HasSCE(ROWOW_SC_FREEZE) == true || sc->HasSCE(ROWOW_SC_STONE) == true || sc->HasSCE(ROWOW_SC_BENEDICTIO) == true ||
            sc->HasSCE(ROWOW_SC_CHANGEUNDEAD) == true)
            return 1;
        if (sc->HasSCE(ROWOW_SC_ELEMENTALCHANGE) == true)
            return SCVal(sc, ROWOW_SC_ELEMENTALCHANGE, 1);
        return std::clamp(level, 1, 4);
    }

    // status_calc_mode
    uint32 StatusCalcMode(ROWOWStatusChange const* sc, uint32 mode)
    {
        ROWOWStatusChangeEntry const* sce = sc != nullptr ? sc->GetSCE(ROWOW_SC_MODECHANGE) : nullptr;
        if (sce == nullptr)
            return mode;
        if (sce->Val2 != 0)
            mode = uint32(sce->Val2); // Set mode (the AI modes, MD_MASK, are all a mode change sets)
        if (sce->Val3 != 0)
            mode |= uint32(sce->Val3); // Add mode
        if (sce->Val4 != 0)
            mode &= ~uint32(sce->Val4); // Del mode
        return mode;
    }
}

int32 ROWOWMod::StatusCalcAttackElement(ROWOWStatusChange const* sc, int32 element)
{
    // status_calc_attack_element: the endowments override the weapon's element
    if (sc == nullptr || sc->IsEmpty() == true)
        return std::clamp(element, 0, int32(UCHAR_MAX));
    if (ROWOWStatusChangeEntry const* sce = sc->GetSCE(ROWOW_SC_ENCHANTARMS))
        return sce->Val1;
    if (sc->HasSCE(ROWOW_SC_WATERWEAPON) == true)
        return ROWOW_ELEMENT_WATER;
    if (sc->HasSCE(ROWOW_SC_EARTHWEAPON) == true)
        return ROWOW_ELEMENT_EARTH;
    if (sc->HasSCE(ROWOW_SC_FIREWEAPON) == true)
        return ROWOW_ELEMENT_FIRE;
    if (sc->HasSCE(ROWOW_SC_WINDWEAPON) == true)
        return ROWOW_ELEMENT_WIND;
    if (sc->HasSCE(ROWOW_SC_ENCPOISON) == true)
        return ROWOW_ELEMENT_POISON;
    if (sc->HasSCE(ROWOW_SC_ASPERSIO) == true)
        return ROWOW_ELEMENT_HOLY;
    if (sc->HasSCE(ROWOW_SC_SHADOWWEAPON) == true)
        return ROWOW_ELEMENT_DARK;
    if (sc->HasSCE(ROWOW_SC_GHOSTWEAPON) == true)
        return ROWOW_ELEMENT_GHOST;
    return std::clamp(element, 0, int32(UCHAR_MAX));
}

ROWOWStatus const* ROWOWMod::GetROBaseStatus(Unit const* unit) const
{
    // status_get_base_status
    if (unit == nullptr)
        return nullptr;
    if (unit->IsPlayer() == true)
    {
        ROWOWIdentity const* identity = unit->ToPlayer()->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
        return (identity != nullptr && identity->IsInForm == true) ? &identity->BaseStatus : nullptr;
    }
    if (unit->IsCreature() == true)
    {
        auto statusIter = MobStatusesByMobID.find(GetROMobID(unit->ToCreature()));
        return statusIter != MobStatusesByMobID.end() ? &statusIter->second : nullptr;
    }
    return nullptr;
}

int64 ROWOWMod::SkillGetTime(uint16 skillID, uint16 skillLevel) const
{
    ROWOWSkill const* skill = GetSkill(skillID);
    return skill != nullptr ? skill->Duration1[ROWOWSkill::GetLevelIndex(skillLevel)] : 0;
}

int64 ROWOWMod::SkillGetTime2(uint16 skillID, uint16 skillLevel) const
{
    ROWOWSkill const* skill = GetSkill(skillID);
    return skill != nullptr ? skill->Duration2[ROWOWSkill::GetLevelIndex(skillLevel)] : 0;
}

int32 ROWOWMod::SkillGetSplash(uint16 skillID, uint16 skillLevel) const
{
    // skill_get_splash: a skill level's SplashArea, a negative one being the whole area a unit sees (AREA_SIZE)
    ROWOWSkill const* skill = GetSkill(skillID);
    int32 splash = skill != nullptr ? skill->SplashArea[ROWOWSkill::GetLevelIndex(skillLevel)] : 0;
    return splash < 0 ? BattleConfig.AreaSize : splash;
}

int32 ROWOWMod::StatusGetHPBonus(ROWOWStatusChange const* sc, int32 type) const
{
    // status_get_hpbonus, the statuses' part: the HP they add (STATUS_BONUS_FIX), or their rate.  A player's gear and Faith are added by
    // status_calc_maxhp_pc (StatusCalcMaxHPPC)
    int32 bonus = 0;
    if (sc == nullptr)
        return bonus;
    if (type == ROWOW_STATUS_BONUS_FIX)
    {
        if (sc->HasSCE(ROWOW_SC_INCMHP) == true)
            bonus += SCVal(sc, ROWOW_SC_INCMHP, 1);
        if (sc->HasSCE(ROWOW_SC_PROMOTE_HEALTH_RESERCH) == true)
            bonus += SCVal(sc, ROWOW_SC_PROMOTE_HEALTH_RESERCH, 3);
        if (sc->HasSCE(ROWOW_SC_MARIONETTE) == true)
            bonus -= 1000;
    }
    else if (type == ROWOW_STATUS_BONUS_RATE)
    {
        // Increasing
        if (sc->HasSCE(ROWOW_SC_INCMHPRATE) == true)
            bonus += SCVal(sc, ROWOW_SC_INCMHPRATE, 1);
        if (sc->HasSCE(ROWOW_SC_APPLEIDUN) == true)
            bonus += SCVal(sc, ROWOW_SC_APPLEIDUN, 2);
        if (sc->HasSCE(ROWOW_SC_DELUGE) == true)
            bonus += SCVal(sc, ROWOW_SC_DELUGE, 2);
        if (sc->HasSCE(ROWOW_SC_BERSERK) == true)
            bonus += 200; // +200%
        if (sc->HasSCE(ROWOW_SC_MERC_HPUP) == true)
            bonus += SCVal(sc, ROWOW_SC_MERC_HPUP, 2);
        // Decreasing
        if (sc->HasSCE(ROWOW_SC_VENOMBLEED) == true && SCVal(sc, ROWOW_SC_VENOMBLEED, 3) == 1)
            bonus -= 15;
        // Max rate reduce is -100%
        bonus = std::max(bonus, -100);
    }
    return bonus;
}

int32 ROWOWMod::StatusGetSPBonus(ROWOWStatusChange const* sc, int32 type) const
{
    // status_get_spbonus, the statuses' part (and status_get_spbonus_item for STATUS_BONUS_ITEM).  A player's gear, Kaina, Meditatio and
    // Soul Drain are added by status_calc_maxsp_pc (StatusCalcMaxSPPC)
    int32 bonus = 0;
    if (sc == nullptr)
        return bonus;
    if (type == ROWOW_STATUS_BONUS_FIX)
    {
        if (sc->HasSCE(ROWOW_SC_INCMSP) == true)
            bonus += SCVal(sc, ROWOW_SC_INCMSP, 1);
    }
    else if (type == ROWOW_STATUS_BONUS_RATE)
    {
        if (sc->HasSCE(ROWOW_SC_INCMSPRATE) == true)
            bonus += SCVal(sc, ROWOW_SC_INCMSPRATE, 1);
        if (sc->HasSCE(ROWOW_SC_SERVICE4U) == true)
            bonus += SCVal(sc, ROWOW_SC_SERVICE4U, 2);
        if (sc->HasSCE(ROWOW_SC_MERC_SPUP) == true)
            bonus += SCVal(sc, ROWOW_SC_MERC_SPUP, 2);
        // Max rate reduce is -100%
        bonus = std::max(bonus, -100);
    }
    else if (type == ROWOW_STATUS_BONUS_ITEM && sc->HasSCE(ROWOW_SC_ENERGY_DRINK_RESERCH) == true)
        bonus += SCVal(sc, ROWOW_SC_ENERGY_DRINK_RESERCH, 3);
    return bonus;
}

void ROWOWMod::StatusCalcRegenRate(Player const* sd, ROWOWStatusChange const* sc, ROWOWStatus& status) const
{
    // status_calc_regen_rate: what regenerates and how fast, from the statuses (the regen skills when they give anything)
    status.RegenFlag = ROWOW_RGN_HP | ROWOW_RGN_SP;
    if (status.SkillRegenHP != 0 || status.SitSkillRegenHP != 0)
        status.RegenFlag |= ROWOW_RGN_SHP;
    if (status.SkillRegenSP != 0 || status.SitSkillRegenSP != 0)
        status.RegenFlag |= ROWOW_RGN_SSP;
    status.RegenRateHP = 100;
    status.RegenRateSP = 100;
    if (sc == nullptr || sc->IsEmpty() == true)
        return;

    // No HP or SP regen
    if ((sc->HasSCE(ROWOW_SC_POISON) == true && sc->HasSCE(ROWOW_SC_SLOWPOISON) == false) || (sc->HasSCE(ROWOW_SC_DPOISON) == true && sc->HasSCE(ROWOW_SC_SLOWPOISON) == false) ||
        sc->HasSCE(ROWOW_SC_BERSERK) == true || sc->HasSCE(ROWOW_SC_TRICKDEAD) == true || sc->HasSCE(ROWOW_SC_BLEEDING) == true ||
        (sc->HasSCE(ROWOW_SC_MAGICMUSHROOM) == true && SCVal(sc, ROWOW_SC_MAGICMUSHROOM, 3) == 1) || sc->HasSCE(ROWOW_SC_NORECOVER_STATE) == true)
        status.RegenFlag = 0;

    // No natural SP regen: a Monk's (or Champion's) Fury and Asura Strike stop it, but with the Monk spirit
    ROWOWStatusChangeEntry const* spirit = sc->GetSCE(ROWOW_SC_SPIRIT);
    ROWOWIdentity const* identity = sd != nullptr ? sd->CustomData.Get<ROWOWIdentity>("ROWOWIdentity") : nullptr;
    bool isMonk = identity != nullptr && (GetNonBabyJobID(identity->JobID) == ROWOW_JOB_MONK || identity->JobID == ROWOW_JOB_CHAMPION);
    if (sc->HasSCE(ROWOW_SC_DANCING) == true || sc->HasSCE(ROWOW_SC_MAXIMIZEPOWER) == true ||
        (isMonk == true && (sc->HasSCE(ROWOW_SC_EXTREMITYFIST) == true || sc->HasSCE(ROWOW_SC_EXPLOSIONSPIRITS) == true) &&
        (spirit == nullptr || spirit->Val2 != ROWOW_SKILL_SL_MONK)) ||
        (sc->HasSCE(ROWOW_SC_OBLIVIONCURSE) == true && SCVal(sc, ROWOW_SC_OBLIVIONCURSE, 3) == 1))
        status.RegenFlag &= ~ROWOW_RGN_SP;
    if (sc->HasSCE(ROWOW_SC_TENSIONRELAX) == true)
        status.RegenRateHP += 200; // (Overweight halves it, but weight isn't computed)
    if (sc->HasSCE(ROWOW_SC_MAGNIFICAT) == true)
        status.RegenRateSP += 100;
    if (ROWOWStatusChangeEntry const* sce = sc->GetSCE(ROWOW_SC_REGENERATION))
    {
        if (sce->Val4 == 0)
        {
            status.RegenRateHP += sce->Val2 * 100;
            status.RegenRateSP += sce->Val3 * 100;
        }
        else
            status.RegenFlag &= ~uint8(sce->Val4); // Remove regen as specified by val4
    }
}

void ROWOWMod::StatusCalcBLMain(Unit const* unit, ROWOWStatus const& baseStatus, ROWOWStatusChange const* sc, ROWOWStatus& status) const
{
    // status_calc_bl_main with every flag (SCB_ALL): the battle status from the base status and the statuses.  Every value is worked out
    // again from the base, so the result is the same whatever changed
    status = baseStatus;
    if (sc != nullptr && sc->IsEmpty() == true)
        sc = nullptr;
    Player const* sd = unit->ToPlayer();
    ROWOWStatus const& b = baseStatus;
    int32 const* bStats = b.Stats;

    status.Stats[ROWOW_STAT_STR] = StatusCalcStr(unit, sc, bStats[ROWOW_STAT_STR]);
    status.Stats[ROWOW_STAT_AGI] = StatusCalcAgi(unit, sc, bStats[ROWOW_STAT_AGI]);
    status.Stats[ROWOW_STAT_VIT] = StatusCalcVit(unit, sc, bStats[ROWOW_STAT_VIT]);
    status.Stats[ROWOW_STAT_INT] = StatusCalcInt(unit, sc, bStats[ROWOW_STAT_INT]);
    status.Stats[ROWOW_STAT_DEX] = StatusCalcDex(unit, sc, bStats[ROWOW_STAT_DEX]);
    status.Stats[ROWOW_STAT_LUK] = StatusCalcLuk(unit, sc, bStats[ROWOW_STAT_LUK]);
    int32 const* stats = status.Stats;

    if (b.BaseAttack != 0)
    {
        // Calculate the difference of old and new base attack in the base status, then add the same difference to the base attack of
        // battle status
        status.BaseAttack = StatusBaseAtk(status) + (b.BaseAttack - StatusBaseAtk(b));
        status.BaseAttack = StatusCalcBatk(unit, sc, status.BaseAttack);
    }

    status.WeaponAttack = StatusCalcWatk(unit, sc, b.WeaponAttack, false);
    if (sd == nullptr) // Should not affect weapon refine bonus
        status.WeaponRefineAttack = StatusCalcWatk(unit, sc, b.WeaponRefineAttack, false);
    if (sd != nullptr)
    {
        int32 weaponAtkRate = GetDeferredBonusSum(b.Bonus, "bWeaponAtkRate");
        if (weaponAtkRate != 0)
            status.WeaponAttack += status.WeaponAttack * weaponAtkRate / 100;
    }
    if (b.LeftWeaponAttack != 0)
    {
        status.LeftWeaponAttack = StatusCalcWatk(unit, sc, b.LeftWeaponAttack, true);
        if (sd == nullptr)
            status.LeftWeaponRefineAttack = StatusCalcWatk(unit, sc, b.LeftWeaponRefineAttack, true);
    }

    status.Hit = StatusCalcHit(unit, sc, b.Hit + (stats[ROWOW_STAT_DEX] - bStats[ROWOW_STAT_DEX]));
    status.Flee = StatusCalcFlee(unit, sc, b.Flee + (stats[ROWOW_STAT_AGI] - bStats[ROWOW_STAT_AGI]));
    status.Def = StatusCalcDef(unit, sc, b.Def);
    status.Def2 = StatusCalcDef2(unit, sc, b.Def2 + (stats[ROWOW_STAT_VIT] - bStats[ROWOW_STAT_VIT]));
    status.MDef = StatusCalcMdef(unit, sc, b.MDef);
    status.MDef2 = StatusCalcMdef2(unit, sc, b.MDef2 + (stats[ROWOW_STAT_INT] - bStats[ROWOW_STAT_INT]) + ((stats[ROWOW_STAT_VIT] - bStats[ROWOW_STAT_VIT]) / 2));

    status.Speed = StatusCalcSpeed(unit, sc, b.Bonus, b.Speed);
    if (sd != nullptr && status.Speed < BattleConfig.MaxWalkSpeed)
    {
        // max_walk_speed is kept as the fastest walk speed (battle.cpp turns the percent into DEFAULT_WALK_SPEED * 100 / max_walk_speed)
        int32 fastest = ROWOW_DEFAULT_WALK_SPEED * 100 / std::max(1, BattleConfig.MaxWalkSpeed);
        status.Speed = std::max(status.Speed, fastest);
    }

    if (b.Critical != 0)
    {
        status.Critical = StatusCalcCritical(unit, sc, b.Critical + (stats[ROWOW_STAT_LUK] - bStats[ROWOW_STAT_LUK]) * 10 / 3);
        // After status_calc_critical so the bonus is applied despite if you have or not a sc bugreport:5240
        if (sd != nullptr && status.WeaponType == ROWOW_WEAPON_TYPE_KATAR)
            status.Critical *= 2;
    }
    if (b.Flee2 != 0)
        status.Flee2 = StatusCalcFlee2(unit, sc, b.Flee2 + (stats[ROWOW_STAT_LUK] - bStats[ROWOW_STAT_LUK]));

    status.WeaponElement = uint8(StatusCalcAttackElement(sc, b.WeaponElement));
    status.LeftWeaponElement = uint8(StatusCalcAttackElement(sc, b.LeftWeaponElement));
    status.DefElement = uint8(StatusCalcElement(sc, b.DefElement));
    status.ElementLevel = uint8(StatusCalcElementLevel(sc, b.ElementLevel));
    status.Mode = StatusCalcMode(sc, b.Mode);
    if (sc != nullptr && sc->HasSCE(ROWOW_SC_MODECHANGE) == true)
    {
        if ((status.Mode & (ROWOW_MODE_STATUS_IMMUNE | ROWOW_MODE_SKILL_IMMUNE)) == (ROWOW_MODE_STATUS_IMMUNE | ROWOW_MODE_SKILL_IMMUNE))
            status.Class = ROWOW_CLASS_BATTLEFIELD;
        else if ((status.Mode & (ROWOW_MODE_STATUS_IMMUNE | ROWOW_MODE_KNOCKBACK_IMMUNE | ROWOW_MODE_DETECTOR)) == (ROWOW_MODE_STATUS_IMMUNE | ROWOW_MODE_KNOCKBACK_IMMUNE | ROWOW_MODE_DETECTOR))
            status.Class = ROWOW_CLASS_BOSS;
        else if ((status.Mode & ROWOW_MODE_STATUS_IMMUNE) != 0)
            status.Class = ROWOW_CLASS_GUARDIAN;
        else
            status.Class = ROWOW_CLASS_NORMAL;
    }

    // Max HP and SP: a player's again from the battle VIT and INT (status_calc_maxhp_pc), a monster's with the statuses' bonuses
    if (sd != nullptr)
    {
        ROWOWIdentity const* identity = sd->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
        if (identity != nullptr)
        {
            status.MaxHP = StatusCalcMaxHPPC(*identity, status, sc);
            if (BattleConfig.HPRate != 100)
                status.MaxHP = uint32(BattleConfig.HPRate * (status.MaxHP / 100.));
            if (identity->BaseLevel < 100)
                status.MaxHP = std::min<uint32>(status.MaxHP, uint32(BattleConfig.MaxHPLevel99));
            else if (identity->BaseLevel < 151)
                status.MaxHP = std::min<uint32>(status.MaxHP, uint32(BattleConfig.MaxHPLevel150));
            else
                status.MaxHP = std::min<uint32>(status.MaxHP, uint32(BattleConfig.MaxHP));
            status.MaxSP = StatusCalcMaxSPPC(*identity, status, sc);
            if (BattleConfig.SPRate != 100)
                status.MaxSP = uint32(BattleConfig.SPRate * (status.MaxSP / 100.));
            status.MaxSP = std::min<uint32>(status.MaxSP, uint32(BattleConfig.MaxSP));
        }
    }
    else
    {
        // status_calc_maxhp and status_calc_maxsp
        int64 maxHP = int64(b.MaxHP) + StatusGetHPBonus(sc, ROWOW_STATUS_BONUS_FIX);
        int32 rate = 100 + StatusGetHPBonus(sc, ROWOW_STATUS_BONUS_RATE);
        if (rate != 100)
            maxHP = maxHP * rate / 100;
        status.MaxHP = uint32(std::clamp<int64>(maxHP, 1, UINT32_MAX));
        int64 maxSP = int64(b.MaxSP) + StatusGetSPBonus(sc, ROWOW_STATUS_BONUS_FIX);
        rate = 100 + StatusGetSPBonus(sc, ROWOW_STATUS_BONUS_RATE);
        if (rate != 100)
            maxSP = maxSP * rate / 100;
        status.MaxSP = uint32(std::clamp<int64>(maxSP, 1, UINT32_MAX));
    }

    // Matk.  (The ematk of bMatk, then the matk rate; Magic Power's boost while it's running)
    {
        int32 int_ = stats[ROWOW_STAT_INT];
        int32 matkMin = int_ + (int_ / 7) * (int_ / 7); // status_base_matk_min
        int32 matkMax = int_ + (int_ / 5) * (int_ / 5); // status_base_matk_max
        if (sd != nullptr)
        {
            int32 ematk = GetDeferredBonusSum(b.Bonus, "bMatk");
            matkMin += ematk;
            matkMax += ematk;
            if (b.Bonus.MatkRate != 100)
            {
                matkMin = matkMin * b.Bonus.MatkRate / 100;
                matkMax = matkMax * b.Bonus.MatkRate / 100;
            }
        }
        if (sc != nullptr)
        {
            if (ROWOWStatusChangeEntry const* magicPower = sc->GetSCE(ROWOW_SC_MAGICPOWER); magicPower != nullptr && magicPower->Val4 != 0)
            {
                matkMin += matkMin * magicPower->Val3 / 100;
                matkMax += matkMax * magicPower->Val3 / 100;
            }
            // (status_calc_pseudobuff_matk's statuses are all the later jobs')
        }
        status.MagicAttackMin = std::clamp(matkMin, 0, int32(USHRT_MAX));
        status.MagicAttackMax = std::clamp(matkMax, 0, int32(USHRT_MAX));
    }

    // Aspd
    if (sd != nullptr)
    {
        ROWOWIdentity const* identity = sd->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
        int32 amotion = identity != nullptr ? StatusBaseAmotionPC(*identity, status) : b.AttackMotion;
        status.AspdRate = StatusCalcAspdRate(unit, sc, status.WeaponType, b.AspdRate);
        // Absolute ASPD % modifiers
        amotion = amotion * status.AspdRate / 1000;
        // Free Cast while casting: increases amotion to reduce ASPD to the corresponding absolute percentage for each level (overriding other
        // adjustments)
        ROWOWSkillUnitData const* ud = unit->CustomData.Get<ROWOWSkillUnitData>("ROWOWSkillUnitData");
        if (int32 freeCast = (ud != nullptr && ud->IsSkillTimer == true) ? PCCheckSkill(unit, ROWOW_SKILL_SA_FREECAST) : 0; freeCast > 0)
            amotion += (2000 - amotion) * (55 - 5 * (freeCast + 1)) / 100;
        amotion = StatusCalcFixAspd(unit, sc, amotion);
        status.AttackMotion = std::clamp(amotion, BattleConfig.MaxAspdAmotion / 2, ROWOW_MIN_ASPD / 2); // pc_maxaspd / AMOTION_DIVIDER_PC, MIN_ASPD / AMOTION_DIVIDER_PC
        status.AttackDelay = 2 * status.AttackMotion;

        // DSPD
        if (b.Stats[ROWOW_STAT_AGI] != stats[ROWOW_STAT_AGI])
        {
            status.DamageMotion = std::clamp(800 - stats[ROWOW_STAT_AGI] * 4, 400, 800);
            if (BattleConfig.PCDamageDelayRate != 100)
                status.DamageMotion = status.DamageMotion * BattleConfig.PCDamageDelayRate / 100;
        }
    }
    else
    {
        // Mercenary and mobs
        int32 amotion = b.AttackMotion;
        status.AspdRate = StatusCalcAspdRate(unit, sc, status.WeaponType, b.AspdRate);
        amotion = amotion * status.AspdRate / 1000;
        amotion = StatusCalcFixAspd(unit, sc, amotion);
        status.AttackMotion = std::clamp(amotion, ROWOW_MAX_ASPD_NOPC, ROWOW_MIN_ASPD); // MAX_ASPD_NOPC / AMOTION_DIVIDER_NOPC, MIN_ASPD / AMOTION_DIVIDER_NOPC
        // FIXME: Officially, adelay only considers a few buffs and is not affected by ASPD debuffs at all.  The only way to make monsters
        // slower is to increase their amotion above their adelay.  For now we solve it by making sure we never increase adelay through
        // aspd_rate and then cap it to amotion
        int32 adelay = b.AttackDelay * std::min(status.AspdRate, 1000) / 1000;
        status.AttackDelay = std::clamp(adelay, status.AttackMotion, ROWOW_MIN_ASPD);
    }

    // Regen (a player's)
    if (sd != nullptr)
    {
        StatusCalcRegen(unit, status);
        StatusCalcRegenRate(sd, sc, status);
    }
}

void ROWOWMod::StatusCalcBL(Unit* unit, std::bitset<ROWOW_SCB_MAX> flag)
{
    // status_calc_bl_: the unit's status again after a status started or ended, and what follows from it on the unit (its maximums, attack
    // delay and speed, and for a player the client's status window).  SCB_BASE works the player's base status out again too (a status whose
    // script gives bonuses)
    if (Player* player = unit->ToPlayer())
    {
        ROWOWIdentity* identity = player->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
        if (identity == nullptr || identity->IsInForm == false)
            return;
        if (flag[ROWOW_SCB_BASE] == true)
            CalculateIdentityStatus(player, *identity); // status_calc_pc_ then status_calc_bl_main
        else
            StatusCalcBLMain(player, identity->BaseStatus, GetStatusChange(unit), identity->Status);
        player->UpdateMaxHealth();
        player->UpdateMaxPower(POWER_MANA);
        player->SetAttackTime(BASE_ATTACK, uint32(identity->Status.AttackDelay));
        SendROStatusToClient(player);
    }
    else if (Creature* creature = unit->ToCreature())
    {
        ROWOWStatus const* baseStatus = GetROBaseStatus(creature);
        if (baseStatus == nullptr)
            return;
        ROWOWMobStatus* mobStatus = creature->CustomData.GetDefault<ROWOWMobStatus>("ROWOWMobStatus");
        uint32 oldMaxHP = mobStatus->Status.MaxHP;
        StatusCalcBLMain(creature, *baseStatus, GetStatusChange(unit), mobStatus->Status);
        if (mobStatus->Status.MaxHP != oldMaxHP && oldMaxHP != 0)
        {
            creature->SetMaxHealth(mobStatus->Status.MaxHP);
            if (creature->GetHealth() > mobStatus->Status.MaxHP) // !FIXME: Should perhaps a status_zap should be issued?
                creature->SetHealth(mobStatus->Status.MaxHP);
        }
        creature->SetAttackTime(BASE_ATTACK, uint32(mobStatus->Status.AttackDelay));
    }
    if (flag[ROWOW_SCB_SPEED] == true)
    {
        unit->UpdateSpeed(MOVE_RUN, true);
        if (Creature* creature = unit->ToCreature())
        {
            // A monster walks (its random walk) at the same RO speed it chases at
            float walkSpeed = creature->GetCreatureTemplate()->speed_walk;
            ApplyROSpeed(creature, MOVE_WALK, walkSpeed);
            creature->SetSpeedRate(MOVE_WALK, walkSpeed);
        }
    }
}

void ROWOWMod::ApplyROSpeed(Unit* unit, uint8 moveType, float& speed) const
{
    // The RO walk speed on WoW's: the unit's speed rate scales by its base walk speed over its walk speed now.  The converter made a
    // monster's template speeds its mob_db walk speed, and a player's run speed is RO's default walk speed (150 ms per cell)
    if (moveType != MOVE_RUN && moveType != MOVE_WALK)
        return;
    ROWOWStatus const* status = GetROStatus(unit);
    ROWOWStatus const* baseStatus = GetROBaseStatus(unit);
    if (status == nullptr || baseStatus == nullptr || status->Speed <= 0 || status->Speed == baseStatus->Speed)
        return;
    if (unit->IsPlayer() == true && moveType != MOVE_RUN)
        return;
    speed *= float(baseStatus->Speed) / float(status->Speed);
}
