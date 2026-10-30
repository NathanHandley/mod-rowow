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
//  Ported from rAthena (https://github.com/rathena/rathena), src/map/battle.cpp, status.cpp and mob.cpp, pre-renewal paths:
//  Copyright (c) rAthena Dev Teams - Licensed under GNU GPL.  For more information, see LICENCE in rAthena's main folder

#include "Chat.h"
#include "Creature.h"
#include "Item.h"
#include "Log.h"
#include "Player.h"
#include "SpellMgr.h"
#include "Unit.h"

#include "ROWOW.h"

using namespace std;

// RO combat between RO combatants: rAthena's pre-renewal battle code, keeping its function names (in the comments beside each and in
// ROWOW.h) and structure so the port can be checked against it.  So far it's the normal attack: skills (R6), the bonus engine and cards
// (R5), refine (R4) and status changes (R6) plug into the places marked for them.  All of it runs in the map's thread, which the
// attacker and its target share
//
// rAthena's macros, written out: ATK_RATE (damage * rate / 100, the left hand's too when attacking with it), ATK_ADDRATE (+ damage *
// rate / 100), ATK_ADD (+ value), and the *2 forms with a separate value for the left hand.  rnd() % n is urand(0, n - 1)

namespace
{
    // rAthena's names for the elements, sizes, races and monster classes, as mob_db and item scripts write them
    char const* const ElementNames[ROWOW_ELEMENT_COUNT] = { "Neutral", "Water", "Earth", "Fire", "Wind", "Poison", "Holy", "Dark", "Ghost", "Undead" };
    char const* const SizeNames[ROWOW_SIZE_COUNT] = { "Small", "Medium", "Large" };
    char const* const RaceNames[] = { "Formless", "Undead", "Brute", "Plant", "Insect", "Fish", "Demon", "Demihuman", "Angel", "Dragon" };
    char const* const ClassNames[] = { "Normal", "Boss", "Guardian", "Battlefield", "Event" };

    // A name's index in one of the tables above (any case), or -1
    int32 GetNameIndex(char const* const* names, size_t nameCount, std::string const& name)
    {
        for (size_t nameIndex = 0; nameIndex < nameCount; nameIndex++)
        {
            std::string const tableName = names[nameIndex];
            if (tableName.size() == name.size() && std::equal(tableName.begin(), tableName.end(), name.begin(),
                [](char a, char b) { return tolower((unsigned char)a) == tolower((unsigned char)b); }))
                return int32(nameIndex);
        }
        return -1;
    }

    // ATK_ADD2: the left hand's only when the attack is left-handed
    void AttackAdd(ROWOWDamage& wd, bool isLeftHanded, int64 value, int64 leftValue)
    {
        wd.Damage += value;
        if (isLeftHanded == true)
            wd.Damage2 += leftValue;
    }

    // The jobs in the Thief line (rAthena's MAPID_THIEF with the first-job mask): Thief, Assassin, Rogue and their transcendent jobs
    bool IsThiefClass(uint16 jobID)
    {
        switch (jobID)
        {
            case 6: case 12: case 17: case 4007: case 4013: case 4018:
                return true;
            default:
                return false;
        }
    }

    // battle_check_undead with undead_detect_type 0: the undead element
    bool IsUndead(ROWOWStatus const& status)
    {
        return status.DefElement == ROWOW_ELEMENT_UNDEAD;
    }

    // The weapon hit sounds' spells, as the converter numbers them from ROWOW_SPELL_WEAPON_HIT_SOUND_START (its RO_WEAPON_HIT_SOUND_NAMES)
    enum WeaponHitSound
    {
        WEAPON_HIT_SOUND_SWORD = 0,
        WEAPON_HIT_SOUND_SPEAR = 1,
        WEAPON_HIT_SOUND_AXE = 2,
        WEAPON_HIT_SOUND_MACE = 3,
        WEAPON_HIT_SOUND_ROD = 4,
        WEAPON_HIT_SOUND_ARROW = 5,
        WEAPON_HIT_SOUND_HANDGUN = 6,
        WEAPON_HIT_SOUND_RIFLE = 7,
    };

    // DAMAGE_DIV_FIX
    void DamageDivFix(int64& damage, int16& div)
    {
        if (div < 0)
        {
            div *= -1;
            damage /= div;
        }
        damage *= div;
    }
}

int32 ROWOWMod::GetElementByName(std::string const& name)
{
    return GetNameIndex(ElementNames, ROWOW_ELEMENT_COUNT, name);
}

void ROWOWMod::LoadBattleData()
{
    MobStatusesByMobID.clear();
    for (uint8 level = 0; level < ROWOW_ELEMENT_LEVEL_MAX; level++)
        for (uint8 attackElement = 0; attackElement < ROWOW_ELEMENT_COUNT; attackElement++)
            for (uint8 defenseElement = 0; defenseElement < ROWOW_ELEMENT_COUNT; defenseElement++)
                AttrFix[level][attackElement][defenseElement] = 100;
    if (IsEnabled == false)
        return;

    // attr_fix.yml: a row for each element level, holding for each attacking element its damage against each defending element
    if (ROWOWDataTable const* attrFixTable = GetDataTable(ROWOW_DATA_TABLE_ATTR_FIX))
    {
        int32 levelColumnIndex = attrFixTable->GetColumnIndex("Level");
        for (size_t rowIndex = 0; rowIndex < attrFixTable->Rows.size(); rowIndex++)
        {
            int64 level = attrFixTable->GetInt(rowIndex, levelColumnIndex, 0);
            if (level < 1 || level > ROWOW_ELEMENT_LEVEL_MAX)
                continue;
            for (uint8 attackElement = 0; attackElement < ROWOW_ELEMENT_COUNT; attackElement++)
            {
                for (auto const& ratioField : ROWOWDataTable::ParseMap(attrFixTable->GetCell(rowIndex, attrFixTable->GetColumnIndex(ElementNames[attackElement]))))
                {
                    int32 defenseElement = GetElementByName(ratioField.first);
                    if (defenseElement >= 0)
                        AttrFix[level - 1][attackElement][defenseElement] = int32(strtol(ratioField.second.c_str(), nullptr, 10));
                }
            }
        }
    }
    else
        LOG_ERROR("module.ROWOW", "ROWOWMod::LoadBattleData found no {} table, so every element deals full damage to every other", ROWOW_DATA_TABLE_ATTR_FIX);

    // Each monster's status, as MobDatabase::parseBodyNode and loadingFinished leave it (mob_db's defaults for what a row leaves out)
    ROWOWDataTable const* mobTable = GetDataTable(ROWOW_DATA_TABLE_MOB);
    if (mobTable == nullptr)
        return;
    static char const* const statColumnNames[ROWOW_STAT_COUNT] = { "Str", "Agi", "Vit", "Int", "Dex", "Luk" };
    int32 idColumnIndex = mobTable->GetColumnIndex("Id");
    int32 levelColumnIndex = mobTable->GetColumnIndex("Level");
    int32 hpColumnIndex = mobTable->GetColumnIndex("Hp");
    int32 spColumnIndex = mobTable->GetColumnIndex("Sp");
    int32 attackColumnIndex = mobTable->GetColumnIndex("Attack");
    int32 attack2ColumnIndex = mobTable->GetColumnIndex("Attack2");
    int32 defenseColumnIndex = mobTable->GetColumnIndex("Defense");
    int32 magicDefenseColumnIndex = mobTable->GetColumnIndex("MagicDefense");
    int32 attackRangeColumnIndex = mobTable->GetColumnIndex("AttackRange");
    int32 sizeColumnIndex = mobTable->GetColumnIndex("Size");
    int32 raceColumnIndex = mobTable->GetColumnIndex("Race");
    int32 elementColumnIndex = mobTable->GetColumnIndex("Element");
    int32 elementLevelColumnIndex = mobTable->GetColumnIndex("ElementLevel");
    int32 attackDelayColumnIndex = mobTable->GetColumnIndex("AttackDelay");
    int32 attackMotionColumnIndex = mobTable->GetColumnIndex("AttackMotion");
    int32 damageMotionColumnIndex = mobTable->GetColumnIndex("DamageMotion");
    int32 damageTakenColumnIndex = mobTable->GetColumnIndex("DamageTaken");
    int32 classColumnIndex = mobTable->GetColumnIndex("Class");
    int32 modesColumnIndex = mobTable->GetColumnIndex("Modes");
    for (size_t rowIndex = 0; rowIndex < mobTable->Rows.size(); rowIndex++)
    {
        uint32 mobID = uint32(mobTable->GetInt(rowIndex, idColumnIndex, 0));
        ROWOWStatus& status = MobStatusesByMobID[mobID];
        status = ROWOWStatus();
        status.IsPlayer = false;
        status.Level = uint32(std::max<int64>(1, mobTable->GetInt(rowIndex, levelColumnIndex, 1)));
        status.MaxHP = uint32(std::max<int64>(1, mobTable->GetInt(rowIndex, hpColumnIndex, 1)));
        status.MaxSP = uint32(std::max<int64>(1, mobTable->GetInt(rowIndex, spColumnIndex, 1)));
        status.WeaponAttack = int32(mobTable->GetInt(rowIndex, attackColumnIndex, 0));
        status.WeaponRefineAttack = int32(mobTable->GetInt(rowIndex, attack2ColumnIndex, 0));
        status.Def = std::clamp<int32>(int32(mobTable->GetInt(rowIndex, defenseColumnIndex, 0)), CHAR_MIN, CHAR_MAX); // DEFTYPE_MIN/MAX, pre-renewal
        status.MDef = std::clamp<int32>(int32(mobTable->GetInt(rowIndex, magicDefenseColumnIndex, 0)), CHAR_MIN, CHAR_MAX);
        for (uint8 statIndex = 0; statIndex < ROWOW_STAT_COUNT; statIndex++)
            status.Stats[statIndex] = std::max<int32>(0, int32(mobTable->GetInt(rowIndex, mobTable->GetColumnIndex(statColumnNames[statIndex]), 1)));
        status.Range = int32(mobTable->GetInt(rowIndex, attackRangeColumnIndex, 0));
        status.Size = uint8(std::max(0, GetNameIndex(SizeNames, ROWOW_SIZE_COUNT, mobTable->GetCell(rowIndex, sizeColumnIndex))));
        status.Race = uint8(std::max(0, GetNameIndex(RaceNames, std::size(RaceNames), mobTable->GetCell(rowIndex, raceColumnIndex))));
        status.DefElement = uint8(std::max(0, GetElementByName(mobTable->GetCell(rowIndex, elementColumnIndex))));
        status.ElementLevel = uint8(std::clamp<int64>(mobTable->GetInt(rowIndex, elementLevelColumnIndex, 1), 1, ROWOW_ELEMENT_LEVEL_MAX));
        status.Class = uint8(std::max(0, GetNameIndex(ClassNames, std::size(ClassNames), mobTable->GetCell(rowIndex, classColumnIndex))));
        // MAX_ASPD_NOPC .. MIN_ASPD, and amotion 1 .. MIN_ASPD / AMOTION_DIVIDER_NOPC
        status.AttackDelay = std::clamp<int32>(int32(mobTable->GetInt(rowIndex, attackDelayColumnIndex, 100)), 100, 8000);
        status.AttackMotion = std::clamp<int32>(int32(mobTable->GetInt(rowIndex, attackMotionColumnIndex, 100)), 1, 8000);
        status.DamageMotion = int32(mobTable->GetInt(rowIndex, damageMotionColumnIndex, 0));
        if (BattleConfig.MonsterDamageDelayRate != 100)
            status.DamageMotion = status.DamageMotion * BattleConfig.MonsterDamageDelayRate / 100;
        status.DamageTaken = int32(mobTable->GetInt(rowIndex, damageTakenColumnIndex, 100));
        for (auto const& modeField : ROWOWDataTable::ParseMap(mobTable->GetCell(rowIndex, modesColumnIndex)))
        {
            uint32 mode = 0;
            if (modeField.first == "IgnoreMelee")
                mode = ROWOW_MODE_IGNORE_MELEE;
            else if (modeField.first == "IgnoreMagic")
                mode = ROWOW_MODE_IGNORE_MAGIC;
            else if (modeField.first == "IgnoreRanged")
                mode = ROWOW_MODE_IGNORE_RANGED;
            else if (modeField.first == "IgnoreMisc")
                mode = ROWOW_MODE_IGNORE_MISC;
            else if (modeField.first == "Mvp")
                mode = ROWOW_MODE_MVP;
            if (modeField.second == "true")
                status.Mode |= mode;
            else
                status.Mode &= ~mode;
        }

        // loadingFinished: an attack can't come sooner than its motion, the rest of the status follows from the stats, and the HP rates
        status.AttackDelay = std::max(status.AttackDelay, status.AttackMotion);
        status.AspdRate = 1000;
        StatusCalcMisc(status, status.Level);
        double maxHP = double(status.MaxHP);
        if ((status.Mode & ROWOW_MODE_MVP) != 0)
        {
            if (BattleConfig.MvpHPRate != 100)
                maxHP = maxHP * double(BattleConfig.MvpHPRate) / 100.;
        }
        else if (BattleConfig.MonsterHPRate != 100)
            maxHP = maxHP * double(BattleConfig.MonsterHPRate) / 100.;
        status.MaxHP = std::max<uint32>(1, uint32(maxHP));
    }
    LOG_INFO("module.ROWOW", "ROWOWMod::LoadBattleData loaded {} RO monster statuses", MobStatusesByMobID.size());
}

ROWOWStatus const* ROWOWMod::GetROStatus(Unit const* unit) const
{
    if (unit == nullptr)
        return nullptr;
    if (unit->IsPlayer() == true)
    {
        ROWOWIdentity const* identity = unit->ToPlayer()->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
        if (identity == nullptr || identity->IsInForm == false)
            return nullptr;
        return &identity->Status;
    }
    if (unit->IsCreature() == true)
    {
        auto statusIter = MobStatusesByMobID.find(GetROMobID(unit->ToCreature()));
        if (statusIter == MobStatusesByMobID.end())
            return nullptr;
        return &statusIter->second;
    }
    return nullptr;
}

int32 ROWOWMod::PCCheckSkill(Unit const* /*unit*/, uint16 /*skillID*/) const
{
    // pc_checkskill.  The identity learns its skills in R6; until then it knows none
    return 0;
}

void ROWOWMod::ApplyMobStatus(Creature* creature) const
{
    // The end of Creature::SelectLevel (spawn, respawn and entry changes): the monster's exact RO HP, in place of the Warcraft health the
    // converter fitted to its level, and its attack delay
    ROWOWStatus const* status = GetROStatus(creature);
    if (status == nullptr)
        return;
    creature->SetCreateHealth(status->MaxHP);
    creature->SetMaxHealth(status->MaxHP);
    creature->SetHealth(status->MaxHP);
    creature->SetStatFlatModifier(UNIT_MOD_HEALTH, BASE_VALUE, float(status->MaxHP));
    creature->SetAttackTime(BASE_ATTACK, uint32(status->AttackDelay));
    creature->SetAttackTime(OFF_ATTACK, uint32(status->AttackDelay));
}

uint32 ROWOWMod::GetWeaponHitSoundSpellID(uint8 weaponType)
{
    // The client's hit sound of each weapon type (roBrowser's WeaponSoundTable): fists, the dual wield types and the rest sound like a mace
    uint32 hitSound = WEAPON_HIT_SOUND_MACE;
    switch (weaponType)
    {
        case ROWOW_WEAPON_TYPE_DAGGER:
        case ROWOW_WEAPON_TYPE_1HSWORD:
        case ROWOW_WEAPON_TYPE_2HSWORD:
            hitSound = WEAPON_HIT_SOUND_SWORD;
            break;
        case ROWOW_WEAPON_TYPE_1HSPEAR:
        case ROWOW_WEAPON_TYPE_2HSPEAR:
            hitSound = WEAPON_HIT_SOUND_SPEAR;
            break;
        case ROWOW_WEAPON_TYPE_1HAXE:
        case ROWOW_WEAPON_TYPE_2HAXE:
            hitSound = WEAPON_HIT_SOUND_AXE;
            break;
        case ROWOW_WEAPON_TYPE_STAFF:
        case ROWOW_WEAPON_TYPE_2HSTAFF:
            hitSound = WEAPON_HIT_SOUND_ROD;
            break;
        case ROWOW_WEAPON_TYPE_BOW:
            hitSound = WEAPON_HIT_SOUND_ARROW;
            break;
        case ROWOW_WEAPON_TYPE_REVOLVER:
            hitSound = WEAPON_HIT_SOUND_HANDGUN;
            break;
        case ROWOW_WEAPON_TYPE_RIFLE:
            hitSound = WEAPON_HIT_SOUND_RIFLE;
            break;
    }
    return ROWOW_SPELL_WEAPON_HIT_SOUND_START + hitSound;
}

float ROWOWMod::GetAutoAttackExtraRange(Player* player, Unit const* victim) const
{
    // An RO weapon reaches its range in cells (a bow 5 and more), which past melee range is further than Warcraft's
    ROWOWStatus const* status = GetROStatus(player);
    if (status == nullptr || victim == nullptr)
        return 0.0f;
    float rangeYards = float(status->Range) * CellSize + player->GetCombatReach() + victim->GetCombatReach();
    return std::max(0.0f, rangeYards - player->GetMeleeRange(victim));
}

bool ROWOWMod::CalculateMeleeDamage(Unit* attacker, Unit* victim, CalcDamageInfo* damageInfo)
{
    // A swing between RO combatants is rAthena's normal attack (battle_weapon_attack), and nothing of Warcraft's melee: no miss, dodge,
    // parry, block, glancing or crushing table, armor, resilience, absorb or level difference
    if (IsEnabled == false || IsROCombatant(attacker) == false || IsROCombatant(victim) == false)
        return false;
    for (uint8 i = 0; i < MAX_ITEM_PROTO_DAMAGES; ++i)
    {
        damageInfo->damages[i].damageSchoolMask = SPELL_SCHOOL_MASK_NORMAL;
        damageInfo->damages[i].damage = 0;
        damageInfo->damages[i].absorb = 0;
        damageInfo->damages[i].resist = 0;
    }
    damageInfo->cleanDamage = 0;
    damageInfo->blocked_amount = 0;
    if (victim->IsCreature() == true && victim->ToCreature()->IsEvadingAttacks() == true)
    {
        damageInfo->HitInfo |= HITINFO_MISS | HITINFO_SWINGNOHITSOUND;
        damageInfo->TargetState = VICTIMSTATE_EVADES;
        damageInfo->hitOutCome = MELEE_HIT_EVADE;
        return true;
    }

    // The client shows a perfect dodge as RO's "Lucky" (dodge, which RO combat otherwise never has), a miss as "Miss", and a critical
    ROWOWDamage wd;
    switch (BattleWeaponAttack(attacker, victim, wd))
    {
        case ROWOW_ATK_LUCKY:
            damageInfo->hitOutCome = MELEE_HIT_DODGE;
            damageInfo->TargetState = VICTIMSTATE_DODGE;
            break;
        case ROWOW_ATK_DEF:
        {
            damageInfo->damages[0].damage = uint32(std::clamp<int64>(wd.Damage + wd.Damage2, 0, std::numeric_limits<uint32>::max()));
            damageInfo->TargetState = VICTIMSTATE_HIT;
            damageInfo->HitInfo |= HITINFO_AFFECTS_VICTIM;
            if (IsAttackCritical(wd, attacker, victim, 0, 0, false) == true)
            {
                damageInfo->hitOutCome = MELEE_HIT_CRIT;
                damageInfo->HitInfo |= HITINFO_CRITICALHIT;
            }
            else
                damageInfo->hitOutCome = MELEE_HIT_NORMAL;
            if (damageInfo->damages[0].damage > 0)
                damageInfo->procVictim |= PROC_FLAG_TAKEN_DAMAGE;

            // A player's weapon makes its RO hit sound on the target (missing until the converter has made the spells)
            ROWOWStatus const* sstatus = GetROStatus(attacker);
            uint32 hitSoundSpellID = GetWeaponHitSoundSpellID(sstatus->WeaponType);
            if (attacker->IsPlayer() == true && damageInfo->damages[0].damage > 0 && sSpellMgr->GetSpellInfo(hitSoundSpellID) != nullptr)
                victim->CastSpell(victim, hitSoundSpellID, true);
        } break;
        default:
            damageInfo->hitOutCome = MELEE_HIT_MISS;
            damageInfo->HitInfo |= HITINFO_MISS;
            damageInfo->TargetState = VICTIMSTATE_INTACT;
            break;
    }
    return true;
}

bool ROWOWMod::HasNormalAttackAmmo(Player* player) const
{
    // battle_weapon_attack's ammo check: a bow or gun needs its ammo worn, of the right type (Ishizu-chan's check, with ammo_check_weapon)
    ROWOWStatus const* status = GetROStatus(player);
    if (status == nullptr || status->IsArrowAttack == false)
        return true;
    if (GetIdentityEquippedItem(player, ROWOW_EQUIP_POSITION_AMMO) == nullptr)
        return false;
    if (BattleConfig.AmmoCheckWeapon == 0)
        return true;
    uint8 neededAmmoType = ROWOW_AMMO_TYPE_BULLET;
    if (status->WeaponType == ROWOW_WEAPON_TYPE_BOW)
        neededAmmoType = ROWOW_AMMO_TYPE_ARROW;
    else if (status->WeaponType == ROWOW_WEAPON_TYPE_GRENADE)
        neededAmmoType = ROWOW_AMMO_TYPE_GRENADE;
    return status->AmmoType == neededAmmoType;
}

uint8 ROWOWMod::BattleWeaponAttack(Unit* src, Unit* target, ROWOWDamage& wdOut)
{
    // battle_weapon_attack, for a normal attack.  (Auto Counter, Poison React, Blade Stop, Raging Trifecta Blow, Sacrifice and the other
    // statuses and skills that turn a normal attack into something else come with them, R6)
    ROWOWStatus const* sstatus = GetROStatus(src);
    if (sstatus == nullptr || GetROStatus(target) == nullptr)
        return ROWOW_ATK_NONE;
    Player* sd = src->ToPlayer();
    Item* ammoItem = nullptr;
    if (sd != nullptr && sstatus->IsArrowAttack == true)
    {
        // A bow or gun shoots its ammo (the auto attack hook stops the attack without it)
        if (HasNormalAttackAmmo(sd) == false)
            return ROWOW_ATK_NONE;
        ammoItem = GetIdentityEquippedItem(sd, ROWOW_EQUIP_POSITION_AMMO);
    }

    wdOut = BattleCalcAttack(ROWOW_BF_WEAPON, src, target, 0, 0, 0);

    // Consume arrow (battle_consume_ammo), and take off the last one
    if (ammoItem != nullptr && BattleConfig.ArrowDecrement != 0)
    {
        bool isLastAmmo = ammoItem->GetCount() <= 1;
        uint32 count = 1;
        sd->DestroyItemCount(ammoItem, count, true);
        if (isLastAmmo == true)
        {
            ROWOWIdentity* identity = GetIdentity(sd);
            identity->ItemGUIDsByEquipPosition.erase(ROWOW_EQUIP_POSITION_AMMO);
            identity->IsEquipmentChanged = true;
            RefreshROForm(sd);
        }
    }
    return wdOut.DmgLv;
}

ROWOWDamage ROWOWMod::BattleCalcAttack(int32 attackType, Unit* src, Unit* target, uint16 skillID, uint16 skillLevel, int32 flag) const
{
    // battle_calc_attack.  (Magic and misc attacks come with the skills, R6)
    ROWOWDamage d;
    switch (attackType)
    {
        case ROWOW_BF_WEAPON: d = BattleCalcWeaponAttack(src, target, skillID, skillLevel, flag); break;
        default:
            LOG_ERROR("module.ROWOW", "ROWOWMod::BattleCalcAttack: unknown attack type {} (skill_id={}, skill_lv={})", attackType, skillID, skillLevel);
            d.DmgLv = ROWOW_ATK_NONE;
            return d;
    }
    if (d.Damage + d.Damage2 < 1)
    {
        // Miss/Absorbed
        if (d.DmgLv == ROWOW_ATK_DEF)
            d.DmgLv = ROWOW_ATK_MISS;
        d.DMotion = 0;
        if (src->IsPlayer() == true)
            d.Div = 1;
    }
    else // Some skills like Weaponry Research will cause damage even if attack is dodged
        d.DmgLv = ROWOW_ATK_DEF;
    return d;
}

ROWOWDamage ROWOWMod::InitializeWeaponData(Unit const* src, Unit const* target, uint16 skillID, uint16 /*skillLevel*/, int32 wflag) const
{
    // initialize_weapon_data
    ROWOWStatus const* sstatus = GetROStatus(src);
    ROWOWStatus const* tstatus = GetROStatus(target);
    ROWOWDamage wd;
    wd.Type = ROWOW_DMG_NORMAL; // Normal attack
    wd.Div = 1; // A skill's hit count comes with the skills (R6)
    wd.AMotion = sstatus->AttackMotion;
    wd.DMotion = tstatus->DamageMotion;
    wd.MiscFlag = wflag;
    wd.Flag = ROWOW_BF_WEAPON; // Initial Flag
    wd.Flag |= (skillID != 0 || wd.MiscFlag != 0) ? ROWOW_BF_SKILL : ROWOW_BF_NORMAL; // Baphomet card's splash damage is counted as a skill. [Inkfish]
    wd.Damage = wd.Damage2 = wd.BaseDamage = 0;
    wd.DmgLv = ROWOW_ATK_DEF; // This assumption simplifies the assignation later

    // wd.flag from basic attacks (a skill's range type comes with the skills, R6)
    if (IsSkillUsingArrow(src, skillID) == true)
        wd.Flag |= ROWOW_BF_LONG;
    else
        wd.Flag |= ROWOW_BF_SHORT;
    return wd;
}

ROWOWDamage ROWOWMod::BattleCalcWeaponAttack(Unit* src, Unit* target, uint16 skillID, uint16 skillLevel, int32 wflag) const
{
    // battle_calc_weapon_attack
    ROWOWDamage wd;
    ROWOWStatus const* sstatus = GetROStatus(src);
    ROWOWStatus const* tstatus = GetROStatus(target);
    if (sstatus == nullptr || tstatus == nullptr)
    {
        wd.DmgLv = ROWOW_ATK_NONE;
        return wd;
    }
    wd = InitializeWeaponData(src, target, skillID, skillLevel, wflag);
    bool isLeftHanded = IsAttackLeftHanded(src, skillID);
    bool infdef = false;

    // Check for Lucky Dodge
    if (skillID == 0 && tstatus->Flee2 != 0 && int32(urand(0, 999)) < tstatus->Flee2)
    {
        wd.Type = ROWOW_DMG_LUCY_DODGE;
        wd.DmgLv = ROWOW_ATK_LUCKY;
        if (wd.Div < 0)
            wd.Div *= -1;
        return wd;
    }

    // on official check for multi hit first so we can override crit on double attack [helvetica]
    BattleCalcMultiAttack(wd, src, target, skillID, skillLevel);

    // crit check is next since crits always hit on official [helvetica]
    if (IsAttackCritical(wd, src, target, skillID, skillLevel, true) == true)
        wd.Type = ROWOW_DMG_CRITICAL;

    // check if we're landing a hit
    if (IsAttackHitting(wd, src, target, skillID, skillLevel, true) == false)
        wd.DmgLv = ROWOW_ATK_FLEE;
    else if ((infdef = IsInfiniteDefense(target, wd.Flag)) == false) // no need for math against plants
    {
        // First call function with skill_id 0 to get base damage of a normal attack
        BattleCalcSkillBaseDamage(wd, src, target, 0, 0); // base damage
        wd.BaseDamage = wd.Damage;
        // (A skill's own base damage, ratio and constant addition, battle_calc_attack_skill_ratio and the player's skill ATK bonuses come with
        // the skills, R6.  The skill ratio of a normal attack is 100, or more from statuses like Overthrust, R6)

        // final attack bonuses that aren't affected by cards (battle_attack_sc_bonus: the ATK percent of statuses, R6)

        if (wd.Damage + wd.Damage2 != 0)
        {
            BattleCalcDefenseReduction(wd, src, target, skillID, skillLevel);
            BattleCalcAttackPostDefense(wd, src, target, skillID, skillLevel);
        }
    }

    BattleCalcElementDamage(wd, src, target, skillID, skillLevel);

    // The weapons' elements, for the cards' element bonuses
    int32 rightElement = BattleGetWeaponElement(wd, src, target, skillID, skillLevel, true);
    int32 leftElement = BattleGetWeaponElement(wd, src, target, skillID, skillLevel, false);
    Player* sd = src->ToPlayer();
    if (sd != nullptr)
    {
        int32 skill = 0;
        if ((skill = PCCheckSkill(sd, ROWOW_SKILL_BS_WEAPONRESEARCH)) > 0)
            AttackAdd(wd, isLeftHanded, skill * 2, skill * 2);
        if (PCCheckSkill(sd, ROWOW_SKILL_BS_HILTBINDING) > 0)
            AttackAdd(wd, isLeftHanded, 4, 4);

        // Card Fix for attacker (sd), 2 is added to the "left" flag meaning "attacker cards only"
        wd.Damage += BattleCalcCardFix(src, target, rightElement, leftElement, wd.Damage, 2, wd.Flag);
        if (isLeftHanded == true)
            wd.Damage2 += BattleCalcCardFix(src, target, rightElement, leftElement, wd.Damage2, 3, wd.Flag);
    }
    if (target->IsPlayer() == true)
    {
        // Card Fix for target (tsd), 2 is not added to the "left" flag meaning "target cards only"
        wd.Damage += BattleCalcCardFix(src, target, rightElement, leftElement, wd.Damage, 0, wd.Flag);
        if (isLeftHanded == true)
            wd.Damage2 += BattleCalcCardFix(src, target, rightElement, leftElement, wd.Damage2, 1, wd.Flag);
    }

    // only do 1 dmg to plant, no need to calculate rest
    if (infdef == true)
    {
        BattleCalcAttackPlant(wd, src, target, skillID, skillLevel);
        return wd;
    }

    // Apply DAMAGE_DIV_FIX and check for min damage
    BattleApplyDivFix(wd, skillID);

    BattleCalcAttackLeftRightHands(wd, src, target, skillID, skillLevel);

    // battle_calc_attack_gvg_bg: the final damage of each hand (the reflected damage of cards and statuses comes with them, R5/R6; there
    // is no War of Emperium or battleground here)
    if (wd.Damage + wd.Damage2 != 0)
    {
        if (wd.Damage2 == 0)
            wd.Damage = BattleCalcDamage(src, target, wd, wd.Damage, skillID, skillLevel);
        else if (wd.Damage == 0)
            wd.Damage2 = BattleCalcDamage(src, target, wd, wd.Damage2, skillID, skillLevel);
        else
        {
            wd.Damage = BattleCalcDamage(src, target, wd, wd.Damage, skillID, skillLevel);
            wd.Damage2 = BattleCalcDamage(src, target, wd, wd.Damage2, skillID, skillLevel);
            if (wd.Damage > 1 && wd.Damage2 < 1)
                wd.Damage2 = 1;
        }
    }

    // (battle_calc_weapon_final_atk_modifiers, battle_absorb_damage and battle_do_reflect are statuses' and cards', R5/R6)
    return wd;
}

bool ROWOWMod::IsAttackCritical(ROWOWDamage& wd, Unit* src, Unit const* target, uint16 skillID, uint16 /*skillLevel*/, bool firstCall) const
{
    // is_attack_critical
    if (firstCall == false)
        return wd.Type == ROWOW_DMG_CRITICAL;

    // (The always-critical skills and a skill's own critical flag come with the skills, R6: until then a skill never crits)
    if (skillID != 0)
        return false;

    ROWOWStatus const* sstatus = GetROStatus(src);
    if (sstatus->Critical == 0)
        return false;
    Player* sd = src->ToPlayer();
    if (wd.Type == ROWOW_DMG_MULTI_HIT)
    {
        // Multiple Hit Attack Skills: Chain Action and Double Attack aren't critical skills
        if (PCCheckSkill(sd, ROWOW_SKILL_GS_CHAINACTION) > 0)
            return false;
        if (PCCheckSkill(sd, ROWOW_SKILL_TF_DOUBLE) > 0)
            return false;
    }

    ROWOWStatus const* tstatus = GetROStatus(target);
    int32 cri = sstatus->Critical;
    if (sd != nullptr)
    {
        cri += sstatus->Bonus.CriticalAddRace[tstatus->Race] + sstatus->Bonus.CriticalAddRace[ROWOW_RACE_ALL];
        if (skillID == 0 && IsSkillUsingArrow(src, skillID) == true)
        {
            cri += sstatus->Bonus.ArrowCri;
            cri += sstatus->Bonus.CriticalRangeAtk;
        }
    }

    // The official equation is *2, but that only applies when sd's do critical.
    // Therefore, we use the old value 3 on cases when an sd gets attacked by a mob
    cri -= tstatus->Stats[ROWOW_STAT_LUK] * ((sd == nullptr && target->IsPlayer() == true) ? 3 : 2);

    // (Sleep doubles it, and Auto Counter makes it twice as likely or certain, R6)
    if (target->IsPlayer() == true && tstatus->Bonus.CriticalDef != 0)
        cri = cri * (100 - tstatus->Bonus.CriticalDef) / 100;
    return int32(urand(0, 999)) < cri;
}

bool ROWOWMod::IsAttackHitting(ROWOWDamage& wd, Unit* src, Unit* target, uint16 skillID, uint16 skillLevel, bool firstCall) const
{
    // is_attack_hitting
    if (firstCall == false)
        return wd.DmgLv != ROWOW_ATK_FLEE;
    ROWOWStatus const* sstatus = GetROStatus(src);
    if (IsAttackCritical(wd, src, target, skillID, skillLevel, false) == true)
        return true;
    else if (src->IsPlayer() == true && sstatus->Bonus.PerfectHit > 0 && int32(urand(0, 99)) < sstatus->Bonus.PerfectHit)
        return true;
    // (Fusion, the skills that always hit and a frozen, stoned or sleeping target, R6)

    ROWOWStatus const* tstatus = GetROStatus(target);
    int32 flee = tstatus->Flee;
    int32 hitrate = 80; // Default hitrate

    // The target's FLEE drops for each attacker past agi_penalty_count (unit_counttargeted: the units attacking it)
    if (BattleConfig.AgiPenaltyType != 0 && (BattleConfig.AgiPenaltyTarget & GetBattleObjectType(*tstatus)) != 0)
    {
        int32 attackerCount = int32(std::min<size_t>(target->getAttackers().size(), 255));
        if (attackerCount >= BattleConfig.AgiPenaltyCount)
        {
            if (BattleConfig.AgiPenaltyType == 1)
                flee = (flee * (100 - (attackerCount - (BattleConfig.AgiPenaltyCount - 1)) * BattleConfig.AgiPenaltyNum)) / 100;
            else // assume type 2: absolute reduction
                flee -= (attackerCount - (BattleConfig.AgiPenaltyCount - 1)) * BattleConfig.AgiPenaltyNum;
            if (flee < 1)
                flee = 1;
        }
    }

    hitrate += sstatus->Hit - flee;

    // (Fog Wall and a skill's own hit rate, R6)
    Player* sd = src->ToPlayer();
    if (sd != nullptr && IsSkillUsingArrow(src, skillID) == true)
        hitrate += sstatus->Bonus.ArrowHit;
    if (skillID == 0 && sd != nullptr && (wd.Type & ROWOW_DMG_MULTI_HIT) != 0 && wd.Div == 2)
        hitrate += PCCheckSkill(sd, ROWOW_SKILL_TF_DOUBLE); // +1 hit per level of Double Attack on a successful double attack [helvetica]

    if (sd != nullptr)
    {
        // Weaponry Research hidden bonus
        int32 skill = 0;
        if ((skill = PCCheckSkill(sd, ROWOW_SKILL_BS_WEAPONRESEARCH)) > 0)
            hitrate += hitrate * (2 * skill) / 100;
    }

    hitrate = std::clamp(hitrate, BattleConfig.MinHitRate, BattleConfig.MaxHitRate);
    return int32(urand(0, 99)) < hitrate;
}

bool ROWOWMod::AttackIgnoresDef(ROWOWDamage& wd, Unit* src, Unit const* target, uint16 skillID, uint16 skillLevel, bool isRightHand) const
{
    // attack_ignores_def.  (Fusion and a skill's own flag, R6)
    if (IsAttackCritical(wd, src, target, skillID, skillLevel, false) == true)
        return true;

    // Ignore Defense?  (Each hand's own bonuses: left_cardfix_to_right is off)
    ROWOWStatus const* sstatus = GetROStatus(src);
    ROWOWStatus const* tstatus = GetROStatus(target);
    if (src->IsPlayer() == false)
        return false;
    ROWOWWeaponBonus const& weaponBonus = isRightHand == true ? sstatus->Bonus.RightWeapon : sstatus->Bonus.LeftWeapon;
    return (weaponBonus.IgnoreDefEle & (1 << tstatus->DefElement)) != 0 || (weaponBonus.IgnoreDefEle & (1 << ROWOW_ELEMENT_ALL)) != 0
        || (weaponBonus.IgnoreDefRace & (1 << tstatus->Race)) != 0 || (weaponBonus.IgnoreDefRace & (1 << ROWOW_RACE_ALL)) != 0
        || (weaponBonus.IgnoreDefClass & (1 << tstatus->Class)) != 0 || (weaponBonus.IgnoreDefClass & (1 << ROWOW_CLASS_ALL)) != 0;
}

int32 ROWOWMod::IsAttackPiercing(ROWOWDamage& wd, Unit* src, Unit const* target, uint16 skillID, uint16 skillLevel, bool isRightHand) const
{
    // is_attack_piercing: the defense-ratio bonuses of cards (Ice Pick and the like).  (Investigate, R6)
    if (src->IsPlayer() == false || IsAttackCritical(wd, src, target, skillID, skillLevel, false) == true)
        return 0;
    ROWOWStatus const* sstatus = GetROStatus(src);
    ROWOWStatus const* tstatus = GetROStatus(target);
    ROWOWWeaponBonus const& weaponBonus = isRightHand == true ? sstatus->Bonus.RightWeapon : sstatus->Bonus.LeftWeapon;
    // Elemental/Racial adjustments
    if ((weaponBonus.DefRatioAtkEle & (1 << tstatus->DefElement)) != 0 || (weaponBonus.DefRatioAtkEle & (1 << ROWOW_ELEMENT_ALL)) != 0
        || (weaponBonus.DefRatioAtkRace & (1 << tstatus->Race)) != 0 || (weaponBonus.DefRatioAtkRace & (1 << ROWOW_RACE_ALL)) != 0
        || (weaponBonus.DefRatioAtkClass & (1 << tstatus->Class)) != 0 || (weaponBonus.DefRatioAtkClass & (1 << ROWOW_CLASS_ALL)) != 0)
        return 1;
    return 0;
}

bool ROWOWMod::IsInfiniteDefense(Unit const* target, int32 flag) const
{
    // is_infinite_defense: plants and the like take 1 damage from each kind of attack they ignore.  (Invincible, R6)
    ROWOWStatus const* tstatus = GetROStatus(target);
    if ((tstatus->Mode & ROWOW_MODE_IGNORE_MELEE) != 0 && (flag & (ROWOW_BF_WEAPON | ROWOW_BF_SHORT)) == (ROWOW_BF_WEAPON | ROWOW_BF_SHORT))
        return true;
    if ((tstatus->Mode & ROWOW_MODE_IGNORE_MAGIC) != 0 && (flag & ROWOW_BF_MAGIC) != 0)
        return true;
    if ((tstatus->Mode & ROWOW_MODE_IGNORE_RANGED) != 0 && (flag & (ROWOW_BF_WEAPON | ROWOW_BF_LONG)) == (ROWOW_BF_WEAPON | ROWOW_BF_LONG))
        return true;
    if ((tstatus->Mode & ROWOW_MODE_IGNORE_MISC) != 0 && (flag & ROWOW_BF_MISC) != 0)
        return true;
    return false;
}

bool ROWOWMod::IsSkillUsingArrow(Unit const* src, uint16 /*skillID*/) const
{
    // is_skill_using_arrow: a player's bow or gun, or a monster's attack from more than 3 cells.  (A skill's ammo and the trap skills
    // come with the skills, R6)
    ROWOWStatus const* sstatus = GetROStatus(src);
    if (sstatus == nullptr)
        return false;
    if (src->IsPlayer() == true)
        return sstatus->IsArrowAttack;
    return sstatus->Range > 3;
}

bool ROWOWMod::IsAttackRightHanded(Unit const* src, uint16 skillID) const
{
    // is_attack_right_handed: skills ALWAYS use ONLY your right-hand weapon (tested on Aegis 10.2)
    ROWOWStatus const* sstatus = GetROStatus(src);
    if (skillID == 0 && src->IsPlayer() == true && sstatus->RightWeaponType == ROWOW_WEAPON_TYPE_FIST && sstatus->LeftWeaponType != ROWOW_WEAPON_TYPE_FIST)
        return false;
    return true;
}

bool ROWOWMod::IsAttackLeftHanded(Unit const* src, uint16 skillID) const
{
    // is_attack_left_handed: skills ALWAYS use ONLY your right-hand weapon (tested on Aegis 10.2)
    if (skillID != 0)
        return false;
    ROWOWStatus const* sstatus = GetROStatus(src);
    if (src->IsPlayer() == true)
    {
        if (sstatus->RightWeaponType == ROWOW_WEAPON_TYPE_FIST && sstatus->LeftWeaponType != ROWOW_WEAPON_TYPE_FIST)
            return true;
        if (sstatus->WeaponType == ROWOW_WEAPON_TYPE_KATAR)
            return true;
    }
    return sstatus->LeftWeaponAttack != 0;
}

int32 ROWOWMod::BattleGetWeaponElement(ROWOWDamage const& /*wd*/, Unit const* src, Unit const* /*target*/, uint16 skillID, uint16 /*skillLevel*/, bool isRightHand) const
{
    // battle_get_weapon_element: a normal attack takes the weapon's element, or the ammo's on a weapon that isn't a bow or gun.  (A
    // skill's own element, the endow statuses and the random element come with the skills, R6)
    ROWOWStatus const* sstatus = GetROStatus(src);
    int32 element = isRightHand == true ? sstatus->WeaponElement : sstatus->LeftWeaponElement;
    if (IsSkillUsingArrow(src, skillID) == true && src->IsPlayer() == true && sstatus->ArrowElement != ROWOW_ELEMENT_NEUTRAL && isRightHand == true)
        element = sstatus->ArrowElement;
    return element;
}

void ROWOWMod::BattleCalcMultiAttack(ROWOWDamage& wd, Unit* src, Unit* /*target*/, uint16 skillID, uint16 /*skillLevel*/) const
{
    // battle_calc_multi_attack: a player's normal attack may hit twice.  (Fear Breeze and Kagemusya come with the statuses, R6)
    Player* sd = src->ToPlayer();
    if (sd == nullptr || skillID != 0)
        return;
    ROWOWStatus const* sstatus = GetROStatus(src);
    int32 skillLevel = 0;
    if (wd.Div == 1 && (((skillLevel = PCCheckSkill(sd, ROWOW_SKILL_TF_DOUBLE)) > 0 && sstatus->RightWeaponType == ROWOW_WEAPON_TYPE_DAGGER)
        || (sstatus->Bonus.DoubleRate > 0 && sstatus->RightWeaponType != ROWOW_WEAPON_TYPE_FIST))) // Will fail bare-handed
    {
        // Success chance is not added, the higher one is used [Skotlex]
        int32 maxRate = std::max(5 * skillLevel, sstatus->Bonus.DoubleRate);
        if (int32(urand(0, 99)) < maxRate)
        {
            wd.Div = 2; // Double Attack's hits
            wd.Type = ROWOW_DMG_MULTI_HIT;
        }
    }
    if (wd.Div == 1 && sstatus->RightWeaponType == ROWOW_WEAPON_TYPE_REVOLVER && (skillLevel = PCCheckSkill(sd, ROWOW_SKILL_GS_CHAINACTION)) > 0 &&
        int32(urand(0, 99)) < 5 * skillLevel) // Normal Chain Action effect
    {
        wd.Div = 2; // Chain Action's hits
        wd.Type = ROWOW_DMG_MULTI_HIT;
    }
}

void ROWOWMod::BattleCalcSkillBaseDamage(ROWOWDamage& wd, Unit* src, Unit* target, uint16 skillID, uint16 skillLevel) const
{
    // battle_calc_skill_base_damage, the base damage of a normal attack (the default case).  (The skills with their own base damage come
    // with them, R6)
    ROWOWStatus const* sstatus = GetROStatus(src);
    ROWOWStatus const* tstatus = GetROStatus(target);
    Player* sd = src->ToPlayer();

    // Flags that apply to both pre-renewal and renewal.  (Mental Charge, Magic Crasher and Weapon Perfection come with the skills, R6)
    int32 bflag = ROWOW_BDMG_NONE;
    if (IsAttackCritical(wd, src, target, skillID, skillLevel, false) == true)
        bflag |= ROWOW_BDMG_CRIT;
    if (IsSkillUsingArrow(src, skillID) == true && sd != nullptr)
    {
        switch (sstatus->WeaponType)
        {
            case ROWOW_WEAPON_TYPE_BOW:
                bflag |= ROWOW_BDMG_ARROW;
                break;
            case ROWOW_WEAPON_TYPE_REVOLVER:
            case ROWOW_WEAPON_TYPE_RIFLE:
            case ROWOW_WEAPON_TYPE_GATLING:
            case ROWOW_WEAPON_TYPE_SHOTGUN:
            case ROWOW_WEAPON_TYPE_GRENADE:
                // Criticals with any guns are calculated like melee criticals
                if ((bflag & ROWOW_BDMG_CRIT) == 0)
                    bflag |= ROWOW_BDMG_ARROW;
                break;
            default:
                // Attacks that use ammo are calculated like melee attacks as long as no ranged weapon is equipped
                break;
        }
    }
    wd.Damage = BattleCalcBaseDamage(src, *sstatus, false, tstatus->Size, bflag);
    if (IsAttackLeftHanded(src, skillID) == true)
        wd.Damage2 = BattleCalcBaseDamage(src, *sstatus, true, tstatus->Size, bflag);

    // Add any bonuses that modify the base atk (pre-skills): +crit damage bonuses in pre-renewal mode.  (The party bonus of Taekwon's
    // Running, R6)
    if (sd != nullptr && (bflag & ROWOW_BDMG_CRIT) != 0 && sstatus->Bonus.CritAtkRate > 0)
    {
        wd.Damage += wd.Damage * sstatus->Bonus.CritAtkRate / 100;
        wd.Damage2 += wd.Damage2 * sstatus->Bonus.CritAtkRate / 100;
    }
}

int64 ROWOWMod::BattleCalcBaseDamage(Unit const* src, ROWOWStatus const& status, bool isLeftHand, uint8 targetSize, int32 flag) const
{
    // battle_calc_base_damage: the standard damage of a normal attack assuming it hits
    uint32 atkmin = 0;
    uint32 atkmax = 0;
    int64 damage = 0;
    bool isPlayer = src->IsPlayer();
    int32 weaponAttack = isLeftHand == true ? status.LeftWeaponAttack : status.WeaponAttack;
    int32 weaponAttack2 = isLeftHand == true ? status.LeftWeaponRefineAttack : status.WeaponRefineAttack;
    if (isPlayer == false)
    {
        // Mobs/Pets.  (Mental Charge, R6)
        if ((flag & ROWOW_BDMG_MAGIC) != 0)
        {
            atkmin = uint32(std::max(0, status.MagicAttackMin));
            atkmax = uint32(std::max(0, status.MagicAttackMax));
        }
        else
        {
            atkmin = uint32(std::max(0, weaponAttack));
            atkmax = uint32(std::max(0, weaponAttack2));
        }
        if (atkmin > atkmax)
            atkmin = atkmax;
    }
    else
    {
        // PCs
        if ((flag & ROWOW_BDMG_MAGIC) != 0)
        {
            atkmin = uint32(std::max(0, status.MagicAttackMin));
            atkmax = uint32(std::max(0, status.MagicAttackMax));
        }
        else
        {
            atkmax = uint32(std::max(0, weaponAttack));
            if ((flag & ROWOW_BDMG_CRIT) == 0 || (flag & ROWOW_BDMG_ARROW) != 0)
            {
                // Normal attacks: DEX sets the low end, more of it with a higher level weapon in the hand
                atkmin = uint32(std::max(0, status.Stats[ROWOW_STAT_DEX]));
                int32 weaponLevel = isLeftHand == true ? status.LeftWeaponLevel : status.WeaponLevel;
                if (weaponLevel > 0)
                    atkmin = atkmin * uint32(80 + weaponLevel * 20) / 100;
                if (atkmin > atkmax)
                    atkmin = atkmax;
                if ((flag & ROWOW_BDMG_ARROW) != 0)
                {
                    // Bows
                    atkmin = atkmin * atkmax / 100;
                    if (atkmin > atkmax)
                        atkmax = atkmin;
                }
            }
        }
    }

    // (Maximize Power, R6)

    // Weapon Damage calculation
    if ((flag & ROWOW_BDMG_CRIT) == 0)
        damage = (atkmax > atkmin ? urand(0, atkmax - atkmin - 1) : 0) + atkmin;
    else
        damage = atkmax;

    if (isPlayer == true)
    {
        // rodatazone says the range is 0~arrow_atk-1 for non crit
        if ((flag & ROWOW_BDMG_ARROW) != 0 && status.ArrowAttack > 0)
            damage += ((flag & ROWOW_BDMG_CRIT) != 0) ? status.ArrowAttack : urand(0, status.ArrowAttack - 1);

        // Size fix only for players.  (Weapon Perfection, R6)
        if (status.Bonus.NoSizeFix == false && (flag & ROWOW_BDMG_NOSIZE) == 0)
            damage = damage * (isLeftHand == true ? status.LeftWeaponSizeMods[targetSize] : status.WeaponSizeMods[targetSize]) / 100;
    }

    // Finally, add baseatk
    if ((flag & ROWOW_BDMG_MAGIC) == 0)
        damage += status.BaseAttack;

    // battle_add_weapon_damage: the overrefine bonus and the weapon damage rate of cards
    if (isPlayer == true)
    {
        int32 overrefine = isLeftHand == true ? status.Bonus.LeftWeapon.Overrefine : status.Bonus.RightWeapon.Overrefine;
        uint8 weaponType = isLeftHand == true ? status.LeftWeaponType : status.RightWeaponType;
        if (overrefine > 0)
            damage = damage + urand(0, overrefine - 1) + 1;
        if (weaponType < ROWOW_WEAPON_TYPE_MAX && status.Bonus.WeaponDamageRate[weaponType] != 0)
            damage += damage * status.Bonus.WeaponDamageRate[weaponType] / 100;
    }
    return damage;
}

void ROWOWMod::BattleCalcDefenseReduction(ROWOWDamage& wd, Unit* src, Unit* target, uint16 skillID, uint16 skillLevel) const
{
    // battle_calc_defense_reduction
    ROWOWStatus const* sstatus = GetROStatus(src);
    ROWOWStatus const* tstatus = GetROStatus(target);
    Player* tsd = target->ToPlayer();

    // Defense reduction.  (The statuses that lower or raise DEF, R6)
    int32 def1 = tstatus->Def;
    int32 def2 = tstatus->Def2;
    int32 vitDef = 0;
    if (src->IsPlayer() == true)
    {
        int32 ignoreRate = sstatus->Bonus.IgnoreDefByRace[tstatus->Race] + sstatus->Bonus.IgnoreDefByRace[ROWOW_RACE_ALL];
        ignoreRate += sstatus->Bonus.IgnoreDefByClass[tstatus->Class] + sstatus->Bonus.IgnoreDefByClass[ROWOW_CLASS_ALL];
        if (ignoreRate != 0)
        {
            ignoreRate = std::min(ignoreRate, 100); // cap it to 100 for 0 def min
            def1 -= def1 * ignoreRate / 100;
            def2 -= def2 * ignoreRate / 100;
        }
    }

    // A player's DEF drops for each attacker past vit_penalty_count (official servers limit the count to 22 targets)
    if (BattleConfig.VitPenaltyType != 0 && (BattleConfig.VitPenaltyTarget & GetBattleObjectType(*tstatus)) != 0)
    {
        int32 targetCount = std::min<int32>(int32(std::min<size_t>(target->getAttackers().size(), 255)), (100 / BattleConfig.VitPenaltyNum) + (BattleConfig.VitPenaltyCount - 1));
        if (targetCount >= BattleConfig.VitPenaltyCount)
        {
            int32 penalty = (targetCount - (BattleConfig.VitPenaltyCount - 1)) * BattleConfig.VitPenaltyNum;
            if (BattleConfig.VitPenaltyType == 1)
            {
                def1 = (def1 * (100 - penalty)) / 100; // (Not with Steel Body, R6)
                def2 = (def2 * (100 - penalty)) / 100;
            }
            else
            {
                // Assume type 2
                def1 -= penalty;
                def2 -= penalty;
            }
        }
        if (def2 < 1)
            def2 = 1;
    }

    // Damage reduction based on vitality
    if (tsd != nullptr)
    {
        // Sd vit-eq.  Damage reduction: [VIT*0.3] + RND(0, [VIT^2/150] - [VIT*0.3] - 1) + [VIT*0.5]
        vitDef = ((3 * def2) / 10);
        vitDef += int32(urand(0, uint32(std::max(0, (def2 * def2) / 150 - ((3 * def2) / 10) - 1))));
        vitDef += (def2 / 2);
        int32 skill = 0;
        if (src->IsCreature() == true && (IsUndead(*sstatus) == true || sstatus->Race == ROWOW_RACE_DEMON) && // This bonus already doesn't work vs players
            (skill = PCCheckSkill(tsd, ROWOW_SKILL_AL_DP)) > 0)
            vitDef += int32(((float)tsd->GetLevel() / 25.0 + 3.0) * skill + 0.5);
    }
    else
    {
        // Mob-Pet vit-eq: VIT + rnd(0,[VIT/20]^2-1).  (Eska, R6)
        vitDef = (def2 / 20) * (def2 / 20);
        vitDef = def2 + (vitDef > 0 ? int32(urand(0, uint32(vitDef - 1))) : 0);
    }

    if (BattleConfig.WeaponDefenseType != 0)
    {
        vitDef += def1 * BattleConfig.WeaponDefenseType;
        def1 = 0;
    }

    if (def1 > 100)
        def1 = 100;
    bool isLeftHanded = IsAttackLeftHanded(src, skillID);
    bool ignoresRight = AttackIgnoresDef(wd, src, target, skillID, skillLevel, true);
    bool ignoresLeft = AttackIgnoresDef(wd, src, target, skillID, skillLevel, false);
    int32 piercingRight = IsAttackPiercing(wd, src, target, skillID, skillLevel, true);
    int32 piercingLeft = IsAttackPiercing(wd, src, target, skillID, skillLevel, false);
    int64 rateRight = ignoresRight == true ? 100 : (piercingRight != 0 ? int64(piercingRight) * (def1 + vitDef) : (100 - def1));
    int64 rateLeft = ignoresLeft == true ? 100 : (piercingLeft != 0 ? int64(piercingLeft) * (def1 + vitDef) : (100 - def1));
    wd.Damage = wd.Damage * rateRight / 100;
    if (isLeftHanded == true)
        wd.Damage2 = wd.Damage2 * rateLeft / 100;
    wd.BaseDamage = wd.BaseDamage * (100 - def1) / 100;
    AttackAdd(wd, isLeftHanded, (ignoresRight == true || piercingRight != 0) ? 0 : -vitDef, (ignoresLeft == true || piercingLeft != 0) ? 0 : -vitDef);
    wd.BaseDamage -= vitDef;
}

void ROWOWMod::BattleCalcAttackPostDefense(ROWOWDamage& wd, Unit* src, Unit* target, uint16 skillID, uint16 skillLevel) const
{
    // battle_calc_attack_post_defense: post skill/vit reduction damage increases
    ROWOWStatus const* sstatus = GetROStatus(src);
    if (src->IsPlayer() == true)
    {
        // Refine bonus
        AttackAdd(wd, IsAttackLeftHanded(src, skillID), sstatus->WeaponRefineAttack, sstatus->LeftWeaponRefineAttack);
        wd.BaseDamage += sstatus->WeaponRefineAttack;
    }

    // After DEF reduction, damage can be negative, refine bonus works against that value
    // After refinement bonus was applied, damage is capped to 1, then masteries are applied
    BattleMinDamage(wd, src, skillID, 1);

    BattleCalcAttackMasteries(wd, src, target, skillID, skillLevel);
    // (Aura Blade, R6)

    // Set to min of 1
    BattleMinDamage(wd, src, skillID, 1);
}

void ROWOWMod::BattleCalcAttackMasteries(ROWOWDamage& wd, Unit* src, Unit* target, uint16 skillID, uint16 /*skillLevel*/) const
{
    // battle_calc_attack_masteries
    if (src->IsPlayer() == false)
        return;
    wd.BaseDamage = BattleAddMastery(src, target, wd.BaseDamage, false);
    wd.Damage = BattleAddMastery(src, target, wd.Damage, false);
    if (IsAttackLeftHanded(src, skillID) == true)
        wd.Damage2 = BattleAddMastery(src, target, wd.Damage2, true);
    // (The statuses considered as masteries, R6)
}

int64 ROWOWMod::BattleAddMastery(Unit const* src, Unit const* target, int64 dmg, bool isLeftHand) const
{
    // battle_addmastery.  (The third jobs' masteries are left out: no pre-renewal job has them)
    ROWOWStatus const* sstatus = GetROStatus(src);
    ROWOWStatus const* status = GetROStatus(target);
    int64 damage = dmg;
    int32 skill = 0;

    if ((skill = PCCheckSkill(src, ROWOW_SKILL_AL_DEMONBANE)) > 0 && target->IsCreature() == true && // This bonus doesn't work against players.
        (IsUndead(*status) == true || status->Race == ROWOW_RACE_DEMON))
        damage += int64(skill * (src->GetLevel() / 20.0 + 3.0));
    if ((skill = PCCheckSkill(src, ROWOW_SKILL_HT_BEASTBANE)) > 0 && (status->Race == ROWOW_RACE_INSECT || status->Race == ROWOW_RACE_BRUTE || status->Race == ROWOW_RACE_PLAYER_DORAM))
        damage += (skill * 4); // (Plus STR with the Hunter spirit, R6)

    uint8 weapon = isLeftHand == true ? sstatus->LeftWeaponType : sstatus->RightWeaponType;
    switch (weapon)
    {
        case ROWOW_WEAPON_TYPE_1HSWORD:
        case ROWOW_WEAPON_TYPE_DAGGER:
            if ((skill = PCCheckSkill(src, ROWOW_SKILL_SM_SWORD)) > 0)
                damage += (skill * 4);
            break;
        case ROWOW_WEAPON_TYPE_2HSWORD:
            if ((skill = PCCheckSkill(src, ROWOW_SKILL_SM_TWOHAND)) > 0)
                damage += (skill * 4);
            break;
        case ROWOW_WEAPON_TYPE_1HSPEAR:
        case ROWOW_WEAPON_TYPE_2HSPEAR:
            if ((skill = PCCheckSkill(src, ROWOW_SKILL_KN_SPEARMASTERY)) > 0)
                damage += (skill * 4); // (5 riding a Peco Peco, R11)
            break;
        case ROWOW_WEAPON_TYPE_1HAXE:
        case ROWOW_WEAPON_TYPE_2HAXE:
            if ((skill = PCCheckSkill(src, ROWOW_SKILL_AM_AXEMASTERY)) > 0)
                damage += (skill * 3);
            break;
        case ROWOW_WEAPON_TYPE_MACE:
        case ROWOW_WEAPON_TYPE_2HMACE:
            if ((skill = PCCheckSkill(src, ROWOW_SKILL_PR_MACEMASTERY)) > 0)
                damage += (skill * 3);
            break;
        case ROWOW_WEAPON_TYPE_FIST:
            if ((skill = PCCheckSkill(src, ROWOW_SKILL_TK_RUN)) > 0)
                damage += (skill * 10);
            [[fallthrough]];
        case ROWOW_WEAPON_TYPE_KNUCKLE:
            if ((skill = PCCheckSkill(src, ROWOW_SKILL_MO_IRONHAND)) > 0)
                damage += (skill * 3);
            break;
        case ROWOW_WEAPON_TYPE_MUSICAL:
            if ((skill = PCCheckSkill(src, ROWOW_SKILL_BA_MUSICALLESSON)) > 0)
                damage += (skill * 3);
            break;
        case ROWOW_WEAPON_TYPE_WHIP:
            if ((skill = PCCheckSkill(src, ROWOW_SKILL_DC_DANCINGLESSON)) > 0)
                damage += (skill * 3);
            break;
        case ROWOW_WEAPON_TYPE_BOOK:
            if ((skill = PCCheckSkill(src, ROWOW_SKILL_SA_ADVANCEDBOOK)) > 0)
                damage += (skill * 3);
            break;
        case ROWOW_WEAPON_TYPE_KATAR:
            if ((skill = PCCheckSkill(src, ROWOW_SKILL_AS_KATAR)) > 0)
                damage += (skill * 3);
            break;
    }
    return damage;
}

void ROWOWMod::BattleCalcElementDamage(ROWOWDamage& wd, Unit* src, Unit* target, uint16 skillID, uint16 skillLevel) const
{
    // battle_calc_element_damage.  (The skills with their own element handling come with them, R6)
    ROWOWStatus const* sstatus = GetROStatus(src);
    ROWOWStatus const* tstatus = GetROStatus(target);
    int32 rightElement = BattleGetWeaponElement(wd, src, target, skillID, skillLevel, true);

    // Elemental attribute fix
    if (wd.Damage > 0 || wd.Damage2 > 0)
    {
        int32 leftElement = BattleGetWeaponElement(wd, src, target, skillID, skillLevel, false);
        if (skillID == 0 && (BattleConfig.AttackAttrNone & GetBattleObjectType(*sstatus)) != 0)
            return; // Non-player basic attacks are non-elemental, they deal 100% against all defense elements
        wd.Damage = BattleAttrFix(src, target, wd.Damage, rightElement, tstatus->DefElement, tstatus->ElementLevel, 1);
        if (IsAttackLeftHanded(src, skillID) == true)
            wd.Damage2 = BattleAttrFix(src, target, wd.Damage2, leftElement, tstatus->DefElement, tstatus->ElementLevel, 1);
    }

    // (The Star Crumb bonus of forged weapons, R4, the Spirit Sphere bonus, EDP and the elemental weapon statuses, R6)

    // Cap damage to 0
    if (BattleConfig.AttrRecover == 0)
        BattleMinDamage(wd, src, skillID, 0);
}

int64 ROWOWMod::BattleAttrFix(Unit const* /*src*/, Unit const* /*target*/, int64 damage, int32 atkElement, int32 defElement, int32 defElementLevel, int32 flag) const
{
    // battle_attr_fix.  (The statuses that raise an element's damage, R6)
    if (atkElement < 0 || atkElement >= ROWOW_ELEMENT_COUNT)
        atkElement = int32(urand(0, ROWOW_ELEMENT_COUNT - 1));
    if (defElement < 0 || defElement >= ROWOW_ELEMENT_COUNT || defElementLevel < 1 || defElementLevel > ROWOW_ELEMENT_LEVEL_MAX)
    {
        LOG_ERROR("module.ROWOW", "ROWOWMod::BattleAttrFix: unknown attribute type: atk={} def_type={} def_lv={}", atkElement, defElement, defElementLevel);
        return damage;
    }
    int32 ratio = AttrFix[defElementLevel - 1][atkElement][defElement];
    if (BattleConfig.AttrRecover == 0 && (flag & 1) == 0 && ratio < 0)
        ratio = 0;
    damage = (damage * ratio) / 100;

    // Damage can be negative, see battle_config.attr_recover
    return damage;
}

void ROWOWMod::BattleCalcAttackPlant(ROWOWDamage& wd, Unit* src, Unit* target, uint16 skillID, uint16 skillLevel) const
{
    // battle_calc_attack_plant: "plant"-type monsters only take 1 damage from all sources.  (The statuses that block even that, R6;
    // there is no Emperium here)
    bool attackHits = IsAttackHitting(wd, src, target, skillID, skillLevel, false);

    // Plants receive 1 damage when hit
    if (attackHits == true || wd.Damage > 0)
        wd.Damage = 1; // In some cases, right hand no need to have a weapon to deal a damage
    if (IsAttackLeftHanded(src, skillID) == true && (attackHits == true || wd.Damage2 > 0))
    {
        ROWOWStatus const* sstatus = GetROStatus(src);
        if (src->IsPlayer() == true && sstatus->WeaponType == ROWOW_WEAPON_TYPE_KATAR)
            wd.Damage2 = 0; // No backhand damage against plants
        else
            wd.Damage2 = 1; // Deal 1 HP damage as long as there is a weapon in the left hand
    }

    // For plants we don't continue with the weapon attack code, so we have to apply DAMAGE_DIV_FIX here
    BattleApplyDivFix(wd, skillID);

    // If there is left hand damage, total damage can never exceed 2, even on multiple hits
    if (wd.Damage > 1 && wd.Damage2 > 0)
    {
        wd.Damage = 1;
        wd.Damage2 = 1;
    }
}

void ROWOWMod::BattleApplyDivFix(ROWOWDamage& wd, uint16 /*skillID*/) const
{
    // battle_apply_div_fix
    if (wd.Damage != 0)
    {
        DamageDivFix(wd.Damage, wd.Div);
        // Min damage
        if (wd.Damage < wd.Div && (BattleConfig.SkillMinDamage & wd.Flag) != 0)
            wd.Damage = wd.Div;
    }
    else if (wd.Div < 0)
        wd.Div *= -1;
}

void ROWOWMod::BattleCalcAttackLeftRightHands(ROWOWDamage& wd, Unit* src, Unit* /*target*/, uint16 skillID, uint16 /*skillLevel*/) const
{
    // battle_calc_attack_left_right_hands
    Player* sd = src->ToPlayer();
    bool isLeftHanded = IsAttackLeftHanded(src, skillID);
    if (sd != nullptr)
    {
        ROWOWStatus const* sstatus = GetROStatus(src);
        ROWOWIdentity const* identity = sd->CustomData.Get<ROWOWIdentity>("ROWOWIdentity");
        bool isThief = identity != nullptr && IsThiefClass(identity->JobID);
        int32 skill = 0;
        if (sstatus->WeaponType == ROWOW_WEAPON_TYPE_KATAR && skillID == 0)
        {
            // Katars (offhand damage only applies to normal attacks, tested on Aegis 10.2)
            skill = PCCheckSkill(sd, ROWOW_SKILL_TF_DOUBLE);
            wd.Damage2 = wd.Damage * (1 + (skill * 2)) / 100;
        }
        else if (isLeftHanded == true)
        {
            // Dual-wield.  If you only have a weapon in the left hand, then your main hand damage will be identical to an unarmed attack
            if (IsAttackRightHanded(src, skillID) == true && wd.Damage != 0)
            {
                if (isThief == true)
                {
                    skill = PCCheckSkill(sd, ROWOW_SKILL_AS_RIGHT);
                    wd.Damage = wd.Damage * (50 + (skill * 10)) / 100;
                }
                if (wd.Damage < 1)
                    wd.Damage = 1;
            }
            // Left hand damage will always be adjusted, even if you don't have a weapon in the main hand
            if (wd.Damage2 != 0)
            {
                if (isThief == true)
                {
                    skill = PCCheckSkill(sd, ROWOW_SKILL_AS_LEFT);
                    wd.Damage2 = wd.Damage2 * (30 + (skill * 10)) / 100;
                }
                if (wd.Damage2 < 1)
                    wd.Damage2 = 1;
            }
        }
    }

    if (IsAttackRightHanded(src, skillID) == false && isLeftHanded == false && wd.Damage != 0)
        wd.Damage = 0;
    if (isLeftHanded == false && wd.Damage2 != 0)
        wd.Damage2 = 0;
}

int64 ROWOWMod::BattleCalcDamage(Unit* /*src*/, Unit* target, ROWOWDamage const& d, int64 damage, uint16 /*skillID*/, uint16 /*skillLevel*/) const
{
    // battle_calc_damage, the final damage.  (The no-damage states of cards, R5; the statuses that raise, lower or block damage, R6;
    // kill steal protection and the PK mode are off)
    if (damage == 0)
        return 0;
    if (BattleConfig.SkillMinDamage != 0 && damage > 0 && damage < d.Div)
    {
        if (((d.Flag & ROWOW_BF_WEAPON) != 0 && (BattleConfig.SkillMinDamage & 1) != 0) || ((d.Flag & ROWOW_BF_MAGIC) != 0 && (BattleConfig.SkillMinDamage & 2) != 0) ||
            ((d.Flag & ROWOW_BF_MISC) != 0 && (BattleConfig.SkillMinDamage & 4) != 0))
            damage = d.Div;
    }

    // Reduces damage received for Green Aura MVP
    ROWOWStatus const* tstatus = GetROStatus(target);
    if (target->IsCreature() == true && tstatus->DamageTaken != 100)
        damage = std::max<int64>(damage * tstatus->DamageTaken / 100, 1);
    return damage;
}

void ROWOWMod::BattleMinDamage(ROWOWDamage& wd, Unit const* src, uint16 skillID, int64 minimum) const
{
    // battle_min_damage: cap both damage and basedamage of damage struct to a minimum value
    if (IsAttackRightHanded(src, skillID) == true)
    {
        wd.Damage = std::max(wd.Damage, minimum);
        wd.BaseDamage = std::max(wd.BaseDamage, minimum);
    }
    // Left-hand damage is always capped to 0
    if (IsAttackLeftHanded(src, skillID) == true)
        wd.Damage2 = std::max<int64>(wd.Damage2, 0);
}
