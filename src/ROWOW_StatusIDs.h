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

#ifndef ROWOW_STATUS_IDS_H
#define ROWOW_STATUS_IDS_H

#include "Define.h"
#include <utility>

// rAthena's status changes (sc_type), with its values, named as status.yml names them (SC_ without the prefix, ROWOW_SC_ here)
#define ROWOW_SC_NONE                                  -1
#define ROWOW_SC_STONE                                 0
#define ROWOW_SC_COMMON_MIN                            0
#define ROWOW_SC_FREEZE                                1
#define ROWOW_SC_STUN                                  2
#define ROWOW_SC_SLEEP                                 3
#define ROWOW_SC_POISON                                4
#define ROWOW_SC_CURSE                                 5
#define ROWOW_SC_SILENCE                               6
#define ROWOW_SC_CONFUSION                             7
#define ROWOW_SC_BLIND                                 8
#define ROWOW_SC_BLEEDING                              9
#define ROWOW_SC_DPOISON                               10
#define ROWOW_SC_STONEWAIT                             11
#define ROWOW_SC_COMMON_MAX                            11
#define ROWOW_SC_PROVOKE                               20
#define ROWOW_SC_ENDURE                                21
#define ROWOW_SC_TWOHANDQUICKEN                        22
#define ROWOW_SC_CONCENTRATE                           23
#define ROWOW_SC_HIDING                                24
#define ROWOW_SC_CLOAKING                              25
#define ROWOW_SC_ENCPOISON                             26
#define ROWOW_SC_POISONREACT                           27
#define ROWOW_SC_QUAGMIRE                              28
#define ROWOW_SC_ANGELUS                               29
#define ROWOW_SC_BLESSING                              30
#define ROWOW_SC_SIGNUMCRUCIS                          31
#define ROWOW_SC_INCREASEAGI                           32
#define ROWOW_SC_DECREASEAGI                           33
#define ROWOW_SC_SLOWPOISON                            34
#define ROWOW_SC_IMPOSITIO                             35
#define ROWOW_SC_SUFFRAGIUM                            36
#define ROWOW_SC_ASPERSIO                              37
#define ROWOW_SC_BENEDICTIO                            38
#define ROWOW_SC_KYRIE                                 39
#define ROWOW_SC_MAGNIFICAT                            40
#define ROWOW_SC_GLORIA                                41
#define ROWOW_SC_AETERNA                               42
#define ROWOW_SC_ADRENALINE                            43
#define ROWOW_SC_WEAPONPERFECTION                      44
#define ROWOW_SC_OVERTHRUST                            45
#define ROWOW_SC_MAXIMIZEPOWER                         46
#define ROWOW_SC_TRICKDEAD                             47
#define ROWOW_SC_LOUD                                  48
#define ROWOW_SC_ENERGYCOAT                            49
#define ROWOW_SC_BROKENARMOR                           50
#define ROWOW_SC_BROKENWEAPON                          51
#define ROWOW_SC_HALLUCINATION                         52
#define ROWOW_SC_WEIGHT50                              53
#define ROWOW_SC_WEIGHT90                              54
#define ROWOW_SC_ASPDPOTION0                           55
#define ROWOW_SC_ASPDPOTION1                           56
#define ROWOW_SC_ASPDPOTION2                           57
#define ROWOW_SC_ASPDPOTION3                           58
#define ROWOW_SC_SPEEDUP0                              59
#define ROWOW_SC_SPEEDUP1                              60
#define ROWOW_SC_ATKPOTION                             61
#define ROWOW_SC_MATKPOTION                            62
#define ROWOW_SC_WEDDING                               63
#define ROWOW_SC_SLOWDOWN                              64
#define ROWOW_SC_ANKLE                                 65
#define ROWOW_SC_KEEPING                               66
#define ROWOW_SC_BARRIER                               67
#define ROWOW_SC_STRIPWEAPON                           68
#define ROWOW_SC_STRIPSHIELD                           69
#define ROWOW_SC_STRIPARMOR                            70
#define ROWOW_SC_STRIPHELM                             71
#define ROWOW_SC_CP_WEAPON                             72
#define ROWOW_SC_CP_SHIELD                             73
#define ROWOW_SC_CP_ARMOR                              74
#define ROWOW_SC_CP_HELM                               75
#define ROWOW_SC_AUTOGUARD                             76
#define ROWOW_SC_REFLECTSHIELD                         77
#define ROWOW_SC_SPLASHER                              78
#define ROWOW_SC_PROVIDENCE                            79
#define ROWOW_SC_DEFENDER                              80
#define ROWOW_SC_MAGICROD                              81
#define ROWOW_SC_SPELLBREAKER                          82
#define ROWOW_SC_AUTOSPELL                             83
#define ROWOW_SC_SIGHTTRASHER                          84
#define ROWOW_SC_AUTOBERSERK                           85
#define ROWOW_SC_SPEARQUICKEN                          86
#define ROWOW_SC_AUTOCOUNTER                           87
#define ROWOW_SC_SIGHT                                 88
#define ROWOW_SC_SAFETYWALL                            89
#define ROWOW_SC_RUWACH                                90
#define ROWOW_SC_EXTREMITYFIST                         91
#define ROWOW_SC_EXPLOSIONSPIRITS                      92
#define ROWOW_SC_COMBO                                 93
#define ROWOW_SC_BLADESTOP_WAIT                        94
#define ROWOW_SC_BLADESTOP                             95
#define ROWOW_SC_FIREWEAPON                            96
#define ROWOW_SC_WATERWEAPON                           97
#define ROWOW_SC_WINDWEAPON                            98
#define ROWOW_SC_EARTHWEAPON                           99
#define ROWOW_SC_VOLCANO                               100
#define ROWOW_SC_DELUGE                                101
#define ROWOW_SC_VIOLENTGALE                           102
#define ROWOW_SC_WATK_ELEMENT                          103
#define ROWOW_SC_ARMOR                                 104
#define ROWOW_SC_ARMOR_ELEMENT_WATER                   105
#define ROWOW_SC_NOCHAT                                106
#define ROWOW_SC_PROTECTEXP                            107
#define ROWOW_SC_AURABLADE                             108
#define ROWOW_SC_PARRYING                              109
#define ROWOW_SC_CONCENTRATION                         110
#define ROWOW_SC_TENSIONRELAX                          111
#define ROWOW_SC_BERSERK                               112
#define ROWOW_SC_FURY                                  113
#define ROWOW_SC_GOSPEL                                114
#define ROWOW_SC_ASSUMPTIO                             115
#define ROWOW_SC_BASILICA                              116
#define ROWOW_SC_GUILDAURA                             117
#define ROWOW_SC_MAGICPOWER                            118
#define ROWOW_SC_EDP                                   119
#define ROWOW_SC_TRUESIGHT                             120
#define ROWOW_SC_WINDWALK                              121
#define ROWOW_SC_MELTDOWN                              122
#define ROWOW_SC_CARTBOOST                             123
#define ROWOW_SC_CHASEWALK                             124
#define ROWOW_SC_REJECTSWORD                           125
#define ROWOW_SC_MARIONETTE                            126
#define ROWOW_SC_MARIONETTE2                           127
#define ROWOW_SC_CHANGEUNDEAD                          128
#define ROWOW_SC_JOINTBEAT                             129
#define ROWOW_SC_MINDBREAKER                           130
#define ROWOW_SC_MEMORIZE                              131
#define ROWOW_SC_FOGWALL                               132
#define ROWOW_SC_SPIDERWEB                             133
#define ROWOW_SC_DEVOTION                              134
#define ROWOW_SC_SACRIFICE                             135
#define ROWOW_SC_STEELBODY                             136
#define ROWOW_SC_ORCISH                                137
#define ROWOW_SC_READYSTORM                            138
#define ROWOW_SC_READYDOWN                             139
#define ROWOW_SC_READYTURN                             140
#define ROWOW_SC_READYCOUNTER                          141
#define ROWOW_SC_DODGE                                 142
#define ROWOW_SC_RUN                                   143
#define ROWOW_SC_SHADOWWEAPON                          144
#define ROWOW_SC_ADRENALINE2                           145
#define ROWOW_SC_GHOSTWEAPON                           146
#define ROWOW_SC_KAIZEL                                147
#define ROWOW_SC_KAAHI                                 148
#define ROWOW_SC_KAUPE                                 149
#define ROWOW_SC_ONEHAND                               150
#define ROWOW_SC_PRESERVE                              151
#define ROWOW_SC_BATTLEORDERS                          152
#define ROWOW_SC_REGENERATION                          153
#define ROWOW_SC_DOUBLECAST                            154
#define ROWOW_SC_GRAVITATION                           155
#define ROWOW_SC_MAXOVERTHRUST                         156
#define ROWOW_SC_LONGING                               157
#define ROWOW_SC_HERMODE                               158
#define ROWOW_SC_SHRINK                                159
#define ROWOW_SC_SIGHTBLASTER                          160
#define ROWOW_SC_WINKCHARM                             161
#define ROWOW_SC_CLOSECONFINE                          162
#define ROWOW_SC_CLOSECONFINE2                         163
#define ROWOW_SC_DANCING                               164
#define ROWOW_SC_ELEMENTALCHANGE                       165
#define ROWOW_SC_RICHMANKIM                            166
#define ROWOW_SC_ETERNALCHAOS                          167
#define ROWOW_SC_DRUMBATTLE                            168
#define ROWOW_SC_NIBELUNGEN                            169
#define ROWOW_SC_ROKISWEIL                             170
#define ROWOW_SC_INTOABYSS                             171
#define ROWOW_SC_SIEGFRIED                             172
#define ROWOW_SC_WHISTLE                               173
#define ROWOW_SC_ASSNCROS                              174
#define ROWOW_SC_POEMBRAGI                             175
#define ROWOW_SC_APPLEIDUN                             176
#define ROWOW_SC_MODECHANGE                            177
#define ROWOW_SC_HUMMING                               178
#define ROWOW_SC_DONTFORGETME                          179
#define ROWOW_SC_FORTUNE                               180
#define ROWOW_SC_SERVICE4U                             181
#define ROWOW_SC_STOP                                  182
#define ROWOW_SC_SPURT                                 183
#define ROWOW_SC_SPIRIT                                184
#define ROWOW_SC_COMA                                  185
#define ROWOW_SC_INTRAVISION                           186
#define ROWOW_SC_INCALLSTATUS                          187
#define ROWOW_SC_INCSTR                                188
#define ROWOW_SC_INCAGI                                189
#define ROWOW_SC_INCVIT                                190
#define ROWOW_SC_INCINT                                191
#define ROWOW_SC_INCDEX                                192
#define ROWOW_SC_INCLUK                                193
#define ROWOW_SC_INCHIT                                194
#define ROWOW_SC_INCHITRATE                            195
#define ROWOW_SC_INCFLEE                               196
#define ROWOW_SC_INCFLEERATE                           197
#define ROWOW_SC_INCMHPRATE                            198
#define ROWOW_SC_INCMSPRATE                            199
#define ROWOW_SC_INCATKRATE                            200
#define ROWOW_SC_INCMATKRATE                           201
#define ROWOW_SC_INCDEFRATE                            202
#define ROWOW_SC_STRFOOD                               203
#define ROWOW_SC_AGIFOOD                               204
#define ROWOW_SC_VITFOOD                               205
#define ROWOW_SC_INTFOOD                               206
#define ROWOW_SC_DEXFOOD                               207
#define ROWOW_SC_LUKFOOD                               208
#define ROWOW_SC_HITFOOD                               209
#define ROWOW_SC_FLEEFOOD                              210
#define ROWOW_SC_BATKFOOD                              211
#define ROWOW_SC_WATKFOOD                              212
#define ROWOW_SC_MATKFOOD                              213
#define ROWOW_SC_SCRESIST                              214
#define ROWOW_SC_XMAS                                  215
#define ROWOW_SC_WARM                                  216
#define ROWOW_SC_SUN_COMFORT                           217
#define ROWOW_SC_MOON_COMFORT                          218
#define ROWOW_SC_STAR_COMFORT                          219
#define ROWOW_SC_FUSION                                220
#define ROWOW_SC_SKILLRATE_UP                          221
#define ROWOW_SC_SKE                                   222
#define ROWOW_SC_KAITE                                 223
#define ROWOW_SC_SWOO                                  224
#define ROWOW_SC_SKA                                   225
#define ROWOW_SC_EARTHSCROLL                           226
#define ROWOW_SC_MIRACLE                               227
#define ROWOW_SC_MADNESSCANCEL                         228
#define ROWOW_SC_ADJUSTMENT                            229
#define ROWOW_SC_INCREASING                            230
#define ROWOW_SC_GATLINGFEVER                          231
#define ROWOW_SC_TATAMIGAESHI                          232
#define ROWOW_SC_UTSUSEMI                              233
#define ROWOW_SC_BUNSINJYUTSU                          234
#define ROWOW_SC_KAENSIN                               235
#define ROWOW_SC_SUITON                                236
#define ROWOW_SC_NEN                                   237
#define ROWOW_SC_KNOWLEDGE                             238
#define ROWOW_SC_SMA                                   239
#define ROWOW_SC_FLING                                 240
#define ROWOW_SC_AVOID                                 241
#define ROWOW_SC_CHANGE                                242
#define ROWOW_SC_BLOODLUST                             243
#define ROWOW_SC_FLEET                                 244
#define ROWOW_SC_SPEED                                 245
#define ROWOW_SC_DEFENCE                               246
#define ROWOW_SC_INCASPDRATE                           247
#define ROWOW_SC_INCFLEE2                              248
#define ROWOW_SC_JAILED                                249
#define ROWOW_SC_ENCHANTARMS                           250
#define ROWOW_SC_MAGICALATTACK                         251
#define ROWOW_SC_ARMORCHANGE                           252
#define ROWOW_SC_CRITICALWOUND                         253
#define ROWOW_SC_MAGICMIRROR                           254
#define ROWOW_SC_SLOWCAST                              255
#define ROWOW_SC_SUMMER                                256
#define ROWOW_SC_EXPBOOST                              257
#define ROWOW_SC_ITEMBOOST                             258
#define ROWOW_SC_BOSSMAPINFO                           259
#define ROWOW_SC_LIFEINSURANCE                         260
#define ROWOW_SC_INCCRI                                261
#define ROWOW_SC_MDEF_RATE                             265
#define ROWOW_SC_INCHEALRATE                           267
#define ROWOW_SC_PNEUMA                                268
#define ROWOW_SC_AUTOTRADE                             269
#define ROWOW_SC_KSPROTECTED                           270
#define ROWOW_SC_ARMOR_RESIST                          271
#define ROWOW_SC_SPCOST_RATE                           272
#define ROWOW_SC_COMMONSC_RESIST                       273
#define ROWOW_SC_SEVENWIND                             274
#define ROWOW_SC_DEF_RATE                              275
#define ROWOW_SC_WALKSPEED                             277
#define ROWOW_SC_MERC_FLEEUP                           278
#define ROWOW_SC_MERC_ATKUP                            279
#define ROWOW_SC_MERC_HPUP                             280
#define ROWOW_SC_MERC_SPUP                             281
#define ROWOW_SC_MERC_HITUP                            282
#define ROWOW_SC_MERC_QUICKEN                          283
#define ROWOW_SC_REBIRTH                               284
#define ROWOW_SC_ITEMSCRIPT                            289
#define ROWOW_SC_S_LIFEPOTION                          290
#define ROWOW_SC_L_LIFEPOTION                          291
#define ROWOW_SC_JEXPBOOST                             292
#define ROWOW_SC_HELLPOWER                             294
#define ROWOW_SC_INVINCIBLE                            295
#define ROWOW_SC_MANU_ATK                              297
#define ROWOW_SC_MANU_DEF                              298
#define ROWOW_SC_SPL_ATK                               299
#define ROWOW_SC_SPL_DEF                               300
#define ROWOW_SC_MANU_MATK                             301
#define ROWOW_SC_SPL_MATK                              302
#define ROWOW_SC_FOOD_STR_CASH                         303
#define ROWOW_SC_FOOD_AGI_CASH                         304
#define ROWOW_SC_FOOD_VIT_CASH                         305
#define ROWOW_SC_FOOD_DEX_CASH                         306
#define ROWOW_SC_FOOD_INT_CASH                         307
#define ROWOW_SC_FOOD_LUK_CASH                         308
#define ROWOW_SC_FEAR                                  309
#define ROWOW_SC_BURNING                               310
#define ROWOW_SC_FREEZING                              311
#define ROWOW_SC_ENCHANTBLADE                          312
#define ROWOW_SC_DEATHBOUND                            313
#define ROWOW_SC_MILLENNIUMSHIELD                      314
#define ROWOW_SC_CRUSHSTRIKE                           315
#define ROWOW_SC_REFRESH                               316
#define ROWOW_SC_REUSE_REFRESH                         317
#define ROWOW_SC_GIANTGROWTH                           318
#define ROWOW_SC_STONEHARDSKIN                         319
#define ROWOW_SC_VITALITYACTIVATION                    320
#define ROWOW_SC_STORMBLAST                            321
#define ROWOW_SC_FIGHTINGSPIRIT                        322
#define ROWOW_SC_ABUNDANCE                             323
#define ROWOW_SC_ADORAMUS                              324
#define ROWOW_SC_EPICLESIS                             325
#define ROWOW_SC_ORATIO                                326
#define ROWOW_SC_LAUDAAGNUS                            327
#define ROWOW_SC_LAUDARAMUS                            328
#define ROWOW_SC_RENOVATIO                             329
#define ROWOW_SC_EXPIATIO                              330
#define ROWOW_SC_DUPLELIGHT                            331
#define ROWOW_SC_SECRAMENT                             332
#define ROWOW_SC_WHITEIMPRISON                         333
#define ROWOW_SC_MARSHOFABYSS                          334
#define ROWOW_SC_RECOGNIZEDSPELL                       335
#define ROWOW_SC_STASIS                                336
#define ROWOW_SC_SPHERE_1                              337
#define ROWOW_SC_SPHERE_2                              338
#define ROWOW_SC_SPHERE_3                              339
#define ROWOW_SC_SPHERE_4                              340
#define ROWOW_SC_SPHERE_5                              341
#define ROWOW_SC_READING_SB                            342
#define ROWOW_SC_FREEZE_SP                             343
#define ROWOW_SC_FEARBREEZE                            344
#define ROWOW_SC_ELECTRICSHOCKER                       345
#define ROWOW_SC_WUGDASH                               346
#define ROWOW_SC_BITE                                  347
#define ROWOW_SC_CAMOUFLAGE                            348
#define ROWOW_SC_ACCELERATION                          349
#define ROWOW_SC_HOVERING                              350
#define ROWOW_SC_SHAPESHIFT                            351
#define ROWOW_SC_INFRAREDSCAN                          352
#define ROWOW_SC_ANALYZE                               353
#define ROWOW_SC_MAGNETICFIELD                         354
#define ROWOW_SC_NEUTRALBARRIER                        355
#define ROWOW_SC_NEUTRALBARRIER_MASTER                 356
#define ROWOW_SC_STEALTHFIELD                          357
#define ROWOW_SC_STEALTHFIELD_MASTER                   358
#define ROWOW_SC_OVERHEAT                              359
#define ROWOW_SC_OVERHEAT_LIMITPOINT                   360
#define ROWOW_SC_VENOMIMPRESS                          361
#define ROWOW_SC_POISONINGWEAPON                       362
#define ROWOW_SC_WEAPONBLOCKING                        363
#define ROWOW_SC_CLOAKINGEXCEED                        364
#define ROWOW_SC_HALLUCINATIONWALK                     365
#define ROWOW_SC_HALLUCINATIONWALK_POSTDELAY           366
#define ROWOW_SC_ROLLINGCUTTER                         367
#define ROWOW_SC_TOXIN                                 368
#define ROWOW_SC_PARALYSE                              369
#define ROWOW_SC_VENOMBLEED                            370
#define ROWOW_SC_MAGICMUSHROOM                         371
#define ROWOW_SC_DEATHHURT                             372
#define ROWOW_SC_PYREXIA                               373
#define ROWOW_SC_OBLIVIONCURSE                         374
#define ROWOW_SC_LEECHESEND                            375
#define ROWOW_SC_REFLECTDAMAGE                         376
#define ROWOW_SC_FORCEOFVANGUARD                       377
#define ROWOW_SC_SHIELDSPELL_HP                        378
#define ROWOW_SC_SHIELDSPELL_SP                        379
#define ROWOW_SC_SHIELDSPELL_ATK                       380
#define ROWOW_SC_EXEEDBREAK                            381
#define ROWOW_SC_PRESTIGE                              382
#define ROWOW_SC_BANDING                               383
#define ROWOW_SC_BANDING_DEFENCE                       384
#define ROWOW_SC_EARTHDRIVE                            385
#define ROWOW_SC_INSPIRATION                           386
#define ROWOW_SC_SPELLFIST                             387
#define ROWOW_SC_CRYSTALIZE                            388
#define ROWOW_SC_STRIKING                              389
#define ROWOW_SC_WARMER                                390
#define ROWOW_SC_VACUUM_EXTREME                        391
#define ROWOW_SC_PROPERTYWALK                          392
#define ROWOW_SC_SWINGDANCE                            393
#define ROWOW_SC_SYMPHONYOFLOVER                       394
#define ROWOW_SC_MOONLITSERENADE                       395
#define ROWOW_SC_RUSHWINDMILL                          396
#define ROWOW_SC_ECHOSONG                              397
#define ROWOW_SC_HARMONIZE                             398
#define ROWOW_SC_VOICEOFSIREN                          399
#define ROWOW_SC_DEEPSLEEP                             400
#define ROWOW_SC_SIRCLEOFNATURE                        401
#define ROWOW_SC_GLOOMYDAY                             402
#define ROWOW_SC_GLOOMYDAY_SK                          403
#define ROWOW_SC_SONGOFMANA                            404
#define ROWOW_SC_DANCEWITHWUG                          405
#define ROWOW_SC_SATURDAYNIGHTFEVER                    406
#define ROWOW_SC_LERADSDEW                             407
#define ROWOW_SC_MELODYOFSINK                          408
#define ROWOW_SC_BEYONDOFWARCRY                        409
#define ROWOW_SC_UNLIMITEDHUMMINGVOICE                 410
#define ROWOW_SC_SITDOWN_FORCE                         411
#define ROWOW_SC_NETHERWORLD                           412
#define ROWOW_SC_CRESCENTELBOW                         413
#define ROWOW_SC_CURSEDCIRCLE_ATKER                    414
#define ROWOW_SC_CURSEDCIRCLE_TARGET                   415
#define ROWOW_SC_LIGHTNINGWALK                         416
#define ROWOW_SC_RAISINGDRAGON                         417
#define ROWOW_SC_GT_ENERGYGAIN                         418
#define ROWOW_SC_GT_CHANGE                             419
#define ROWOW_SC_GT_REVITALIZE                         420
#define ROWOW_SC_GN_CARTBOOST                          421
#define ROWOW_SC_THORNSTRAP                            422
#define ROWOW_SC_BLOODSUCKER                           423
#define ROWOW_SC_SMOKEPOWDER                           424
#define ROWOW_SC_TEARGAS                               425
#define ROWOW_SC_MANDRAGORA                            426
#define ROWOW_SC_STOMACHACHE                           427
#define ROWOW_SC_MYSTERIOUS_POWDER                     428
#define ROWOW_SC_MELON_BOMB                            429
#define ROWOW_SC_BANANA_BOMB                           430
#define ROWOW_SC_BANANA_BOMB_SITDOWN                   431
#define ROWOW_SC_SAVAGE_STEAK                          432
#define ROWOW_SC_COCKTAIL_WARG_BLOOD                   433
#define ROWOW_SC_MINOR_BBQ                             434
#define ROWOW_SC_SIROMA_ICE_TEA                        435
#define ROWOW_SC_DROCERA_HERB_STEAMED                  436
#define ROWOW_SC_PUTTI_TAILS_NOODLES                   437
#define ROWOW_SC_BOOST500                              438
#define ROWOW_SC_FULL_SWING_K                          439
#define ROWOW_SC_MANA_PLUS                             440
#define ROWOW_SC_MUSTLE_M                              441
#define ROWOW_SC_LIFE_FORCE_F                          442
#define ROWOW_SC_EXTRACT_WHITE_POTION_Z                443
#define ROWOW_SC_VITATA_500                            444
#define ROWOW_SC_EXTRACT_SALAMINE_JUICE                445
#define ROWOW_SC__REPRODUCE                            446
#define ROWOW_SC__AUTOSHADOWSPELL                      447
#define ROWOW_SC__SHADOWFORM                           448
#define ROWOW_SC__BODYPAINT                            449
#define ROWOW_SC__INVISIBILITY                         450
#define ROWOW_SC__DEADLYINFECT                         451
#define ROWOW_SC__ENERVATION                           452
#define ROWOW_SC__GROOMY                               453
#define ROWOW_SC__IGNORANCE                            454
#define ROWOW_SC__LAZINESS                             455
#define ROWOW_SC__UNLUCKY                              456
#define ROWOW_SC__WEAKNESS                             457
#define ROWOW_SC__STRIPACCESSORY                       458
#define ROWOW_SC__MANHOLE                              459
#define ROWOW_SC__BLOODYLUST                           460
#define ROWOW_SC_CIRCLE_OF_FIRE                        461
#define ROWOW_SC_CIRCLE_OF_FIRE_OPTION                 462
#define ROWOW_SC_FIRE_CLOAK                            463
#define ROWOW_SC_FIRE_CLOAK_OPTION                     464
#define ROWOW_SC_WATER_SCREEN                          465
#define ROWOW_SC_WATER_SCREEN_OPTION                   466
#define ROWOW_SC_WATER_DROP                            467
#define ROWOW_SC_WATER_DROP_OPTION                     468
#define ROWOW_SC_WATER_BARRIER                         469
#define ROWOW_SC_WIND_STEP                             470
#define ROWOW_SC_WIND_STEP_OPTION                      471
#define ROWOW_SC_WIND_CURTAIN                          472
#define ROWOW_SC_WIND_CURTAIN_OPTION                   473
#define ROWOW_SC_ZEPHYR                                474
#define ROWOW_SC_SOLID_SKIN                            475
#define ROWOW_SC_SOLID_SKIN_OPTION                     476
#define ROWOW_SC_STONE_SHIELD                          477
#define ROWOW_SC_STONE_SHIELD_OPTION                   478
#define ROWOW_SC_POWER_OF_GAIA                         479
#define ROWOW_SC_PYROTECHNIC                           480
#define ROWOW_SC_PYROTECHNIC_OPTION                    481
#define ROWOW_SC_HEATER                                482
#define ROWOW_SC_HEATER_OPTION                         483
#define ROWOW_SC_TROPIC                                484
#define ROWOW_SC_TROPIC_OPTION                         485
#define ROWOW_SC_AQUAPLAY                              486
#define ROWOW_SC_AQUAPLAY_OPTION                       487
#define ROWOW_SC_COOLER                                488
#define ROWOW_SC_COOLER_OPTION                         489
#define ROWOW_SC_CHILLY_AIR                            490
#define ROWOW_SC_CHILLY_AIR_OPTION                     491
#define ROWOW_SC_GUST                                  492
#define ROWOW_SC_GUST_OPTION                           493
#define ROWOW_SC_BLAST                                 494
#define ROWOW_SC_BLAST_OPTION                          495
#define ROWOW_SC_WILD_STORM                            496
#define ROWOW_SC_WILD_STORM_OPTION                     497
#define ROWOW_SC_PETROLOGY                             498
#define ROWOW_SC_PETROLOGY_OPTION                      499
#define ROWOW_SC_CURSED_SOIL                           500
#define ROWOW_SC_CURSED_SOIL_OPTION                    501
#define ROWOW_SC_UPHEAVAL                              502
#define ROWOW_SC_UPHEAVAL_OPTION                       503
#define ROWOW_SC_TIDAL_WEAPON                          504
#define ROWOW_SC_TIDAL_WEAPON_OPTION                   505
#define ROWOW_SC_ROCK_CRUSHER                          506
#define ROWOW_SC_ROCK_CRUSHER_ATK                      507
#define ROWOW_SC_LEADERSHIP                            508
#define ROWOW_SC_GLORYWOUNDS                           509
#define ROWOW_SC_SOULCOLD                              510
#define ROWOW_SC_HAWKEYES                              511
#define ROWOW_SC_ODINS_POWER                           512
#define ROWOW_SC_RAID                                  513
#define ROWOW_SC_FIRE_INSIGNIA                         514
#define ROWOW_SC_WATER_INSIGNIA                        515
#define ROWOW_SC_WIND_INSIGNIA                         516
#define ROWOW_SC_EARTH_INSIGNIA                        517
#define ROWOW_SC_PUSH_CART                             518
#define ROWOW_SC_SPELLBOOK1                            519
#define ROWOW_SC_SPELLBOOK2                            520
#define ROWOW_SC_SPELLBOOK3                            521
#define ROWOW_SC_SPELLBOOK4                            522
#define ROWOW_SC_SPELLBOOK5                            523
#define ROWOW_SC_SPELLBOOK6                            524
#define ROWOW_SC_MAXSPELLBOOK                          525
#define ROWOW_SC_INCMHP                                526
#define ROWOW_SC_INCMSP                                527
#define ROWOW_SC_PARTYFLEE                             528
#define ROWOW_SC_MEIKYOUSISUI                          529
#define ROWOW_SC_JYUMONJIKIRI                          530
#define ROWOW_SC_KYOUGAKU                              531
#define ROWOW_SC_IZAYOI                                532
#define ROWOW_SC_ZENKAI                                533
#define ROWOW_SC_KAGEHUMI                              534
#define ROWOW_SC_KYOMU                                 535
#define ROWOW_SC_KAGEMUSYA                             536
#define ROWOW_SC_ZANGETSU                              537
#define ROWOW_SC_GENSOU                                538
#define ROWOW_SC_AKAITSUKI                             539
#define ROWOW_SC_STYLE_CHANGE                          540
#define ROWOW_SC_TINDER_BREAKER                        541
#define ROWOW_SC_TINDER_BREAKER2                       542
#define ROWOW_SC_CBC                                   543
#define ROWOW_SC_EQC                                   544
#define ROWOW_SC_GOLDENE_FERSE                         545
#define ROWOW_SC_ANGRIFFS_MODUS                        546
#define ROWOW_SC_OVERED_BOOST                          547
#define ROWOW_SC_LIGHT_OF_REGENE                       548
#define ROWOW_SC_ASH                                   549
#define ROWOW_SC_GRANITIC_ARMOR                        550
#define ROWOW_SC_MAGMA_FLOW                            551
#define ROWOW_SC_PYROCLASTIC                           552
#define ROWOW_SC_PARALYSIS                             553
#define ROWOW_SC_PAIN_KILLER                           554
#define ROWOW_SC_HANBOK                                555
#define ROWOW_SC_DEFSET                                556
#define ROWOW_SC_MDEFSET                               557
#define ROWOW_SC_DARKCROW                              558
#define ROWOW_SC_FULL_THROTTLE                         559
#define ROWOW_SC_REBOUND                               560
#define ROWOW_SC_UNLIMIT                               561
#define ROWOW_SC_KINGS_GRACE                           562
#define ROWOW_SC_TELEKINESIS_INTENSE                   563
#define ROWOW_SC_OFFERTORIUM                           564
#define ROWOW_SC_FRIGG_SONG                            565
#define ROWOW_SC_MONSTER_TRANSFORM                     566
#define ROWOW_SC_ANGEL_PROTECT                         567
#define ROWOW_SC_ILLUSIONDOPING                        568
#define ROWOW_SC_FLASHCOMBO                            569
#define ROWOW_SC_MOONSTAR                              570
#define ROWOW_SC_SUPER_STAR                            571
#define ROWOW_SC_HEAT_BARREL                           572
#define ROWOW_SC_MAGICALBULLET                         573
#define ROWOW_SC_P_ALTER                               574
#define ROWOW_SC_E_CHAIN                               575
#define ROWOW_SC_C_MARKER                              576
#define ROWOW_SC_ANTI_M_BLAST                          577
#define ROWOW_SC_B_TRAP                                578
#define ROWOW_SC_H_MINE                                579
#define ROWOW_SC_QD_SHOT_READY                         580
#define ROWOW_SC_MTF_ASPD                              581
#define ROWOW_SC_MTF_RANGEATK                          582
#define ROWOW_SC_MTF_MATK                              583
#define ROWOW_SC_MTF_MLEATKED                          584
#define ROWOW_SC_MTF_CRIDAMAGE                         585
#define ROWOW_SC_OKTOBERFEST                           586
#define ROWOW_SC_STRANGELIGHTS                         587
#define ROWOW_SC_DECORATION_OF_MUSIC                   588
#define ROWOW_SC_QUEST_BUFF1                           589
#define ROWOW_SC_QUEST_BUFF2                           590
#define ROWOW_SC_QUEST_BUFF3                           591
#define ROWOW_SC_ALL_RIDING                            592
#define ROWOW_SC_TEARGAS_SOB                           593
#define ROWOW_SC__FEINTBOMB                            594
#define ROWOW_SC__CHAOS                                595
#define ROWOW_SC_CHASEWALK2                            596
#define ROWOW_SC_VACUUM_EXTREME_POSTDELAY              597
#define ROWOW_SC_MTF_ASPD2                             598
#define ROWOW_SC_MTF_RANGEATK2                         599
#define ROWOW_SC_MTF_MATK2                             600
#define ROWOW_SC_2011RWC_SCROLL                        601
#define ROWOW_SC_JP_EVENT04                            602
#define ROWOW_SC_MTF_MHP                               603
#define ROWOW_SC_MTF_MSP                               604
#define ROWOW_SC_MTF_PUMPKIN                           605
#define ROWOW_SC_MTF_HITFLEE                           606
#define ROWOW_SC_CRIFOOD                               607
#define ROWOW_SC_ATTHASTE_CASH                         608
#define ROWOW_SC_REUSE_LIMIT_A                         609
#define ROWOW_SC_REUSE_LIMIT_B                         610
#define ROWOW_SC_REUSE_LIMIT_C                         611
#define ROWOW_SC_REUSE_LIMIT_D                         612
#define ROWOW_SC_REUSE_LIMIT_E                         613
#define ROWOW_SC_REUSE_LIMIT_F                         614
#define ROWOW_SC_REUSE_LIMIT_G                         615
#define ROWOW_SC_REUSE_LIMIT_H                         616
#define ROWOW_SC_REUSE_LIMIT_MTF                       617
#define ROWOW_SC_REUSE_LIMIT_ASPD_POTION               618
#define ROWOW_SC_REUSE_MILLENNIUMSHIELD                619
#define ROWOW_SC_REUSE_CRUSHSTRIKE                     620
#define ROWOW_SC_REUSE_STORMBLAST                      621
#define ROWOW_SC_ALL_RIDING_REUSE_LIMIT                622
#define ROWOW_SC_REUSE_LIMIT_ECL                       623
#define ROWOW_SC_REUSE_LIMIT_RECALL                    624
#define ROWOW_SC_PROMOTE_HEALTH_RESERCH                625
#define ROWOW_SC_ENERGY_DRINK_RESERCH                  626
#define ROWOW_SC_NORECOVER_STATE                       627
#define ROWOW_SC_SUHIDE                                628
#define ROWOW_SC_SU_STOOP                              629
#define ROWOW_SC_SPRITEMABLE                           630
#define ROWOW_SC_CATNIPPOWDER                          631
#define ROWOW_SC_SV_ROOTTWIST                          632
#define ROWOW_SC_BITESCAR                              633
#define ROWOW_SC_ARCLOUSEDASH                          634
#define ROWOW_SC_TUNAPARTY                             635
#define ROWOW_SC_SHRIMP                                636
#define ROWOW_SC_FRESHSHRIMP                           637
#define ROWOW_SC_ACTIVE_MONSTER_TRANSFORM              638
#define ROWOW_SC_CLOUD_KILL                            639
#define ROWOW_SC_LJOSALFAR                             640
#define ROWOW_SC_MERMAID_LONGING                       641
#define ROWOW_SC_HAT_EFFECT                            642
#define ROWOW_SC_FLOWERSMOKE                           643
#define ROWOW_SC_FSTONE                                644
#define ROWOW_SC_HAPPINESS_STAR                        645
#define ROWOW_SC_MAPLE_FALLS                           646
#define ROWOW_SC_TIME_ACCESSORY                        647
#define ROWOW_SC_MAGICAL_FEATHER                       648
#define ROWOW_SC_GVG_GIANT                             649
#define ROWOW_SC_GVG_GOLEM                             650
#define ROWOW_SC_GVG_STUN                              651
#define ROWOW_SC_GVG_STONE                             652
#define ROWOW_SC_GVG_FREEZ                             653
#define ROWOW_SC_GVG_SLEEP                             654
#define ROWOW_SC_GVG_CURSE                             655
#define ROWOW_SC_GVG_SILENCE                           656
#define ROWOW_SC_GVG_BLIND                             657
#define ROWOW_SC_CLAN_INFO                             658
#define ROWOW_SC_SWORDCLAN                             659
#define ROWOW_SC_ARCWANDCLAN                           660
#define ROWOW_SC_GOLDENMACECLAN                        661
#define ROWOW_SC_CROSSBOWCLAN                          662
#define ROWOW_SC_JUMPINGCLAN                           663
#define ROWOW_SC_TAROTCARD                             664
#define ROWOW_SC_GEFFEN_MAGIC1                         665
#define ROWOW_SC_GEFFEN_MAGIC2                         666
#define ROWOW_SC_GEFFEN_MAGIC3                         667
#define ROWOW_SC_MAXPAIN                               668
#define ROWOW_SC_ARMOR_ELEMENT_EARTH                   669
#define ROWOW_SC_ARMOR_ELEMENT_FIRE                    670
#define ROWOW_SC_ARMOR_ELEMENT_WIND                    671
#define ROWOW_SC_DAILYSENDMAILCNT                      672
#define ROWOW_SC_DORAM_BUF_01                          673
#define ROWOW_SC_DORAM_BUF_02                          674
#define ROWOW_SC_HISS                                  675
#define ROWOW_SC_NYANGGRASS                            676
#define ROWOW_SC_GROOMING                              677
#define ROWOW_SC_SHRIMPBLESSING                        678
#define ROWOW_SC_CHATTERING                            679
#define ROWOW_SC_DORAM_WALKSPEED                       680
#define ROWOW_SC_DORAM_MATK                            681
#define ROWOW_SC_DORAM_FLEE2                           682
#define ROWOW_SC_DORAM_SVSP                            683
#define ROWOW_SC_FALLEN_ANGEL                          684
#define ROWOW_SC_CHEERUP                               685
#define ROWOW_SC_DRESSUP                               686
#define ROWOW_SC_GLASTHEIM_ATK                         687
#define ROWOW_SC_GLASTHEIM_DEF                         688
#define ROWOW_SC_GLASTHEIM_HEAL                        689
#define ROWOW_SC_GLASTHEIM_HIDDEN                      690
#define ROWOW_SC_GLASTHEIM_STATE                       691
#define ROWOW_SC_GLASTHEIM_ITEMDEF                     692
#define ROWOW_SC_GLASTHEIM_HPSP                        693
#define ROWOW_SC_LHZ_DUN_N1                            694
#define ROWOW_SC_LHZ_DUN_N2                            695
#define ROWOW_SC_LHZ_DUN_N3                            696
#define ROWOW_SC_LHZ_DUN_N4                            697
#define ROWOW_SC_ANCILLA                               698
#define ROWOW_SC_EARTHSHAKER                           699
#define ROWOW_SC_WEAPONBLOCK_ON                        700
#define ROWOW_SC_SPORE_EXPLOSION                       701
#define ROWOW_SC_ADAPTATION                            702
#define ROWOW_SC_BASILICA_CELL                         703
#define ROWOW_SC_ENTRY_QUEUE_APPLY_DELAY               704
#define ROWOW_SC_ENTRY_QUEUE_NOTIFY_ADMISSION_TIME_OUT 705
#define ROWOW_SC_LIGHTOFMOON                           706
#define ROWOW_SC_LIGHTOFSUN                            707
#define ROWOW_SC_LIGHTOFSTAR                           708
#define ROWOW_SC_LUNARSTANCE                           709
#define ROWOW_SC_UNIVERSESTANCE                        710
#define ROWOW_SC_SUNSTANCE                             711
#define ROWOW_SC_FLASHKICK                             712
#define ROWOW_SC_NEWMOON                               713
#define ROWOW_SC_STARSTANCE                            714
#define ROWOW_SC_DIMENSION                             715
#define ROWOW_SC_DIMENSION1                            716
#define ROWOW_SC_DIMENSION2                            717
#define ROWOW_SC_CREATINGSTAR                          718
#define ROWOW_SC_FALLINGSTAR                           719
#define ROWOW_SC_NOVAEXPLOSING                         720
#define ROWOW_SC_GRAVITYCONTROL                        721
#define ROWOW_SC_SOULCOLLECT                           722
#define ROWOW_SC_SOULREAPER                            723
#define ROWOW_SC_SOULUNITY                             724
#define ROWOW_SC_SOULSHADOW                            725
#define ROWOW_SC_SOULFAIRY                             726
#define ROWOW_SC_SOULFALCON                            727
#define ROWOW_SC_SOULGOLEM                             728
#define ROWOW_SC_SOULDIVISION                          729
#define ROWOW_SC_SOULENERGY                            730
#define ROWOW_SC_USE_SKILL_SP_SPA                      731
#define ROWOW_SC_USE_SKILL_SP_SHA                      732
#define ROWOW_SC_SP_SHA                                733
#define ROWOW_SC_SOULCURSE                             734
#define ROWOW_SC_HELLS_PLANT                           735
#define ROWOW_SC_INCREASE_MAXHP                        736
#define ROWOW_SC_INCREASE_MAXSP                        737
#define ROWOW_SC_REF_T_POTION                          738
#define ROWOW_SC_ADD_ATK_DAMAGE                        739
#define ROWOW_SC_ADD_MATK_DAMAGE                       740
#define ROWOW_SC_HELPANGEL                             741
#define ROWOW_SC_SOUNDOFDESTRUCTION                    742
#define ROWOW_SC_LUXANIMA                              743
#define ROWOW_SC_REUSE_LIMIT_LUXANIMA                  744
#define ROWOW_SC_ENSEMBLEFATIGUE                       745
#define ROWOW_SC_MISTY_FROST                           746
#define ROWOW_SC_MAGIC_POISON                          747
#define ROWOW_SC_EP16_2_BUFF_SS                        748
#define ROWOW_SC_EP16_2_BUFF_SC                        749
#define ROWOW_SC_EP16_2_BUFF_AC                        750
#define ROWOW_SC_OVERBRANDREADY                        751
#define ROWOW_SC_POISON_MIST                           752
#define ROWOW_SC_STONE_WALL                            753
#define ROWOW_SC_CLOUD_POISON                          754
#define ROWOW_SC_HOMUN_TIME                            755
#define ROWOW_SC_EMERGENCY_MOVE                        756
#define ROWOW_SC_MADOGEAR                              757
#define ROWOW_SC_NPC_HALLUCINATIONWALK                 758
#define ROWOW_SC_PACKING_ENVELOPE1                     759
#define ROWOW_SC_PACKING_ENVELOPE2                     760
#define ROWOW_SC_PACKING_ENVELOPE3                     761
#define ROWOW_SC_PACKING_ENVELOPE4                     762
#define ROWOW_SC_PACKING_ENVELOPE5                     763
#define ROWOW_SC_PACKING_ENVELOPE6                     764
#define ROWOW_SC_PACKING_ENVELOPE7                     765
#define ROWOW_SC_PACKING_ENVELOPE8                     766
#define ROWOW_SC_PACKING_ENVELOPE9                     767
#define ROWOW_SC_PACKING_ENVELOPE10                    768
#define ROWOW_SC_SOULATTACK                            769
#define ROWOW_SC_WIDEWEB                               770
#define ROWOW_SC_BURNT                                 771
#define ROWOW_SC_CHILL                                 772
#define ROWOW_SC_HANDICAPSTATE_DEEPBLIND               773
#define ROWOW_SC_HANDICAPSTATE_DEEPSILENCE             774
#define ROWOW_SC_HANDICAPSTATE_LASSITUDE               775
#define ROWOW_SC_HANDICAPSTATE_FROSTBITE               776
#define ROWOW_SC_HANDICAPSTATE_SWOONING                777
#define ROWOW_SC_HANDICAPSTATE_LIGHTNINGSTRIKE         778
#define ROWOW_SC_HANDICAPSTATE_CRYSTALLIZATION         779
#define ROWOW_SC_HANDICAPSTATE_CONFLAGRATION           780
#define ROWOW_SC_HANDICAPSTATE_MISFORTUNE              781
#define ROWOW_SC_HANDICAPSTATE_DEADLYPOISON            782
#define ROWOW_SC_HANDICAPSTATE_DEPRESSION              783
#define ROWOW_SC_HANDICAPSTATE_HOLYFLAME               784
#define ROWOW_SC_SERVANTWEAPON                         785
#define ROWOW_SC_SERVANT_SIGN                          786
#define ROWOW_SC_CHARGINGPIERCE                        787
#define ROWOW_SC_CHARGINGPIERCE_COUNT                  788
#define ROWOW_SC_DRAGONIC_AURA                         789
#define ROWOW_SC_VIGOR                                 790
#define ROWOW_SC_DEADLY_DEFEASANCE                     791
#define ROWOW_SC_CLIMAX_DES_HU                         792
#define ROWOW_SC_CLIMAX                                793
#define ROWOW_SC_CLIMAX_EARTH                          794
#define ROWOW_SC_CLIMAX_BLOOM                          795
#define ROWOW_SC_CLIMAX_CRYIMP                         796
#define ROWOW_SC_WINDSIGN                              797
#define ROWOW_SC_CRESCIVEBOLT                          798
#define ROWOW_SC_CALAMITYGALE                          799
#define ROWOW_SC_MEDIALE                               800
#define ROWOW_SC_A_VITA                                801
#define ROWOW_SC_A_TELUM                               802
#define ROWOW_SC_PRE_ACIES                             803
#define ROWOW_SC_COMPETENTIA                           804
#define ROWOW_SC_RELIGIO                               805
#define ROWOW_SC_BENEDICTUM                            806
#define ROWOW_SC_AXE_STOMP                             807
#define ROWOW_SC_A_MACHINE                             808
#define ROWOW_SC_D_MACHINE                             809
#define ROWOW_SC_ABR_BATTLE_WARIOR                     810
#define ROWOW_SC_ABR_DUAL_CANNON                       811
#define ROWOW_SC_ABR_MOTHER_NET                        812
#define ROWOW_SC_ABR_INFINITY                          813
#define ROWOW_SC_SHADOW_EXCEED                         814
#define ROWOW_SC_DANCING_KNIFE                         815
#define ROWOW_SC_POTENT_VENOM                          816
#define ROWOW_SC_SHADOW_SCAR                           817
#define ROWOW_SC_E_SLASH_COUNT                         818
#define ROWOW_SC_SHADOW_WEAPON                         819
#define ROWOW_SC_GUARD_STANCE                          820
#define ROWOW_SC_ATTACK_STANCE                         821
#define ROWOW_SC_GUARDIAN_S                            822
#define ROWOW_SC_REBOUND_S                             823
#define ROWOW_SC_HOLY_S                                824
#define ROWOW_SC_ULTIMATE_S                            825
#define ROWOW_SC_SPEAR_SCAR                            826
#define ROWOW_SC_SHIELD_POWER                          827
#define ROWOW_SC_SPELL_ENCHANTING                      828
#define ROWOW_SC_SUMMON_ELEMENTAL_ARDOR                829
#define ROWOW_SC_SUMMON_ELEMENTAL_DILUVIO              830
#define ROWOW_SC_SUMMON_ELEMENTAL_PROCELLA             831
#define ROWOW_SC_SUMMON_ELEMENTAL_TERREMOTUS           832
#define ROWOW_SC_SUMMON_ELEMENTAL_SERPENS              833
#define ROWOW_SC_ELEMENTAL_VEIL                        834
#define ROWOW_SC_MYSTIC_SYMPHONY                       835
#define ROWOW_SC_KVASIR_SONATA                         836
#define ROWOW_SC_SOUNDBLEND                            837
#define ROWOW_SC_GEF_NOCTURN                           838
#define ROWOW_SC_AIN_RHAPSODY                          839
#define ROWOW_SC_MUSICAL_INTERLUDE                     840
#define ROWOW_SC_JAWAII_SERENADE                       841
#define ROWOW_SC_PRON_MARCH                            842
#define ROWOW_SC_ROSEBLOSSOM                           843
#define ROWOW_SC_POWERFUL_FAITH                        844
#define ROWOW_SC_SINCERE_FAITH                         845
#define ROWOW_SC_FIRM_FAITH                            846
#define ROWOW_SC_HOLY_OIL                              847
#define ROWOW_SC_FIRST_BRAND                           848
#define ROWOW_SC_SECOND_BRAND                          849
#define ROWOW_SC_SECOND_JUDGE                          850
#define ROWOW_SC_THIRD_EXOR_FLAME                      851
#define ROWOW_SC_FIRST_FAITH_POWER                     852
#define ROWOW_SC_MASSIVE_F_BLASTER                     853
#define ROWOW_SC_PROTECTSHADOWEQUIP                    854
#define ROWOW_SC_RESEARCHREPORT                        855
#define ROWOW_SC_BO_HELL_DUSTY                         856
#define ROWOW_SC_BIONIC_WOODENWARRIOR                  857
#define ROWOW_SC_BIONIC_WOODEN_FAIRY                   858
#define ROWOW_SC_BIONIC_CREEPER                        859
#define ROWOW_SC_BIONIC_HELLTREE                       860
#define ROWOW_SC_SHADOW_STRIP                          861
#define ROWOW_SC_ABYSS_DAGGER                          862
#define ROWOW_SC_ABYSSFORCEWEAPON                      863
#define ROWOW_SC_ABYSS_SLAYER                          864
#define ROWOW_SC_FLAMETECHNIC                          865
#define ROWOW_SC_FLAMETECHNIC_OPTION                   866
#define ROWOW_SC_FLAMEARMOR                            867
#define ROWOW_SC_FLAMEARMOR_OPTION                     868
#define ROWOW_SC_COLD_FORCE                            869
#define ROWOW_SC_COLD_FORCE_OPTION                     870
#define ROWOW_SC_CRYSTAL_ARMOR                         871
#define ROWOW_SC_CRYSTAL_ARMOR_OPTION                  872
#define ROWOW_SC_GRACE_BREEZE                          873
#define ROWOW_SC_GRACE_BREEZE_OPTION                   874
#define ROWOW_SC_EYES_OF_STORM                         875
#define ROWOW_SC_EYES_OF_STORM_OPTION                  876
#define ROWOW_SC_EARTH_CARE                            877
#define ROWOW_SC_EARTH_CARE_OPTION                     878
#define ROWOW_SC_STRONG_PROTECTION                     879
#define ROWOW_SC_STRONG_PROTECTION_OPTION              880
#define ROWOW_SC_DEEP_POISONING                        881
#define ROWOW_SC_DEEP_POISONING_OPTION                 882
#define ROWOW_SC_POISON_SHIELD                         883
#define ROWOW_SC_POISON_SHIELD_OPTION                  884
#define ROWOW_SC_SUB_WEAPONPROPERTY                    885
#define ROWOW_SC_M_LIFEPOTION                          886
#define ROWOW_SC_S_MANAPOTION                          887
#define ROWOW_SC_ALMIGHTY                              888
#define ROWOW_SC_ULTIMATECOOK                          889
#define ROWOW_SC_M_DEFSCROLL                           890
#define ROWOW_SC_INFINITY_DRINK                        891
#define ROWOW_SC_MENTAL_POTION                         892
#define ROWOW_SC_LIMIT_POWER_BOOSTER                   893
#define ROWOW_SC_COMBAT_PILL                           894
#define ROWOW_SC_COMBAT_PILL2                          895
#define ROWOW_SC_MYSTICPOWDER                          896
#define ROWOW_SC_SPARKCANDY                            897
#define ROWOW_SC_MAGICCANDY                            898
#define ROWOW_SC_ACARAJE                               899
#define ROWOW_SC_POPECOOKIE                            900
#define ROWOW_SC_VITALIZE_POTION                       901
#define ROWOW_SC_CUP_OF_BOZA                           902
#define ROWOW_SC_SKF_MATK                              903
#define ROWOW_SC_SKF_ATK                               904
#define ROWOW_SC_SKF_ASPD                              905
#define ROWOW_SC_SKF_CAST                              906
#define ROWOW_SC_BEEF_RIB_STEW                         907
#define ROWOW_SC_PORK_RIB_STEW                         908
#define ROWOW_SC_WEAPONBREAKER                         909
#define ROWOW_SC_TOXIN_OF_MANDARA                      910
#define ROWOW_SC_GOLDENE_TONE                          911
#define ROWOW_SC_TEMPERING                             912
#define ROWOW_SC_GRADUAL_GRAVITY                       913
#define ROWOW_SC_ALL_STAT_DOWN                         914
#define ROWOW_SC_KILLING_AURA                          915
#define ROWOW_SC_DAMAGE_HEAL                           916
#define ROWOW_SC_IMMUNE_PROPERTY_NOTHING               917
#define ROWOW_SC_IMMUNE_PROPERTY_WATER                 918
#define ROWOW_SC_IMMUNE_PROPERTY_GROUND                919
#define ROWOW_SC_IMMUNE_PROPERTY_FIRE                  920
#define ROWOW_SC_IMMUNE_PROPERTY_WIND                  921
#define ROWOW_SC_IMMUNE_PROPERTY_POISON                922
#define ROWOW_SC_IMMUNE_PROPERTY_SAINT                 923
#define ROWOW_SC_IMMUNE_PROPERTY_DARKNESS              924
#define ROWOW_SC_IMMUNE_PROPERTY_TELEKINESIS           925
#define ROWOW_SC_IMMUNE_PROPERTY_UNDEAD                926
#define ROWOW_SC_RELIEVE_ON                            927
#define ROWOW_SC_RELIEVE_OFF                           928
#define ROWOW_SC_RUSH_QUAKE1                           929
#define ROWOW_SC_RUSH_QUAKE2                           930
#define ROWOW_SC_G_LIFEPOTION                          931
#define ROWOW_SC_HNNOWEAPON                            932
#define ROWOW_SC_SHIELDCHAINRUSH                       933
#define ROWOW_SC_MISTYFROST                            934
#define ROWOW_SC_GROUNDGRAVITY                         935
#define ROWOW_SC_BREAKINGLIMIT                         936
#define ROWOW_SC_RULEBREAK                             937
#define ROWOW_SC_INTENSIVE_AIM                         938
#define ROWOW_SC_INTENSIVE_AIM_COUNT                   939
#define ROWOW_SC_GRENADE_FRAGMENT_1                    940
#define ROWOW_SC_GRENADE_FRAGMENT_2                    941
#define ROWOW_SC_GRENADE_FRAGMENT_3                    942
#define ROWOW_SC_GRENADE_FRAGMENT_4                    943
#define ROWOW_SC_GRENADE_FRAGMENT_5                    944
#define ROWOW_SC_GRENADE_FRAGMENT_6                    945
#define ROWOW_SC_AUTO_FIRING_LAUNCHER                  946
#define ROWOW_SC_HIDDEN_CARD                           947
#define ROWOW_SC_PERIOD_RECEIVEITEM_2ND                948
#define ROWOW_SC_PERIOD_PLUSEXP_2ND                    949
#define ROWOW_SC_POWERUP                               951
#define ROWOW_SC_AGIUP                                 952
#define ROWOW_SC_PROTECTION                            953
#define ROWOW_SC_BATH_FOAM_A                           954
#define ROWOW_SC_BATH_FOAM_B                           955
#define ROWOW_SC_BATH_FOAM_C                           956
#define ROWOW_SC_BUCHEDENOEL                           957
#define ROWOW_SC_EP16_DEF                              958
#define ROWOW_SC_STR_SCROLL                            959
#define ROWOW_SC_INT_SCROLL                            960
#define ROWOW_SC_CONTENTS_1                            961
#define ROWOW_SC_CONTENTS_2                            962
#define ROWOW_SC_CONTENTS_3                            963
#define ROWOW_SC_CONTENTS_4                            964
#define ROWOW_SC_CONTENTS_5                            965
#define ROWOW_SC_CONTENTS_6                            966
#define ROWOW_SC_CONTENTS_7                            967
#define ROWOW_SC_CONTENTS_8                            968
#define ROWOW_SC_CONTENTS_9                            969
#define ROWOW_SC_CONTENTS_10                           970
#define ROWOW_SC_MYSTERY_POWDER                        971
#define ROWOW_SC_CONTENTS_26                           972
#define ROWOW_SC_CONTENTS_27                           973
#define ROWOW_SC_CONTENTS_28                           974
#define ROWOW_SC_CONTENTS_29                           975
#define ROWOW_SC_CONTENTS_31                           976
#define ROWOW_SC_CONTENTS_32                           977
#define ROWOW_SC_CONTENTS_33                           978
#define ROWOW_SC_TALISMAN_OF_PROTECTION                979
#define ROWOW_SC_TALISMAN_OF_WARRIOR                   980
#define ROWOW_SC_TALISMAN_OF_MAGICIAN                  981
#define ROWOW_SC_TALISMAN_OF_FIVE_ELEMENTS             982
#define ROWOW_SC_T_FIRST_GOD                           983
#define ROWOW_SC_T_SECOND_GOD                          984
#define ROWOW_SC_T_THIRD_GOD                           985
#define ROWOW_SC_T_FOURTH_GOD                          986
#define ROWOW_SC_T_FIFTH_GOD                           987
#define ROWOW_SC_HEAVEN_AND_EARTH                      988
#define ROWOW_SC_TOTEM_OF_TUTELARY                     989
#define ROWOW_SC_RETURN_TO_ELDICASTES                  990
#define ROWOW_SC_GUARDIAN_RECALL                       991
#define ROWOW_SC_ECLAGE_RECALL                         992
#define ROWOW_SC_ALL_NIFLHEIM_RECALL                   993
#define ROWOW_SC_ALL_PRONTERA_RECALL                   994
#define ROWOW_SC_ALL_GLASTHEIM_RECALL                  995
#define ROWOW_SC_ALL_THANATOS_RECALL                   996
#define ROWOW_SC_ALL_LIGHTHALZEN_RECALL                997
#define ROWOW_SC_HOGOGONG                              998
#define ROWOW_SC_MARINE_FESTIVAL                       999
#define ROWOW_SC_SANDY_FESTIVAL                        1000
#define ROWOW_SC_KI_SUL_RAMPAGE                        1001
#define ROWOW_SC_COLORS_OF_HYUN_ROK_1                  1002
#define ROWOW_SC_COLORS_OF_HYUN_ROK_2                  1003
#define ROWOW_SC_COLORS_OF_HYUN_ROK_3                  1004
#define ROWOW_SC_COLORS_OF_HYUN_ROK_4                  1005
#define ROWOW_SC_COLORS_OF_HYUN_ROK_5                  1006
#define ROWOW_SC_COLORS_OF_HYUN_ROK_6                  1007
#define ROWOW_SC_COLORS_OF_HYUN_ROK_BUFF               1008
#define ROWOW_SC_TEMPORARY_COMMUNION                   1009
#define ROWOW_SC_BLESSING_OF_M_CREATURES               1010
#define ROWOW_SC_BLESSING_OF_M_C_DEBUFF                1011
#define ROWOW_SC_RISING_SUN                            1012
#define ROWOW_SC_NOON_SUN                              1013
#define ROWOW_SC_SUNSET_SUN                            1014
#define ROWOW_SC_RISING_MOON                           1015
#define ROWOW_SC_MIDNIGHT_MOON                         1016
#define ROWOW_SC_DAWN_MOON                             1017
#define ROWOW_SC_STAR_BURST                            1018
#define ROWOW_SC_SKY_ENCHANT                           1019
#define ROWOW_SC_WILD_WALK                             1020
#define ROWOW_SC_SHADOW_CLOCK                          1021
#define ROWOW_SC_SHINKIROU_CALL                        1022
#define ROWOW_SC_NIGHTMARE                             1023
#define ROWOW_SC_SBUNSHIN                              1024
#define ROWOW_SC_CONTENTS_34                           1025
#define ROWOW_SC_CONTENTS_35                           1026
#define ROWOW_SC_NOACTION                              1027
#define ROWOW_SC_C_BUFF_3                              1028
#define ROWOW_SC_C_BUFF_4                              1029
#define ROWOW_SC_C_BUFF_5                              1030
#define ROWOW_SC_C_BUFF_6                              1031
#define ROWOW_SC_CONTENTS_15                           1032
#define ROWOW_SC_CONTENTS_16                           1033
#define ROWOW_SC_CONTENTS_17                           1034
#define ROWOW_SC_CONTENTS_18                           1035
#define ROWOW_SC_CONTENTS_19                           1036
#define ROWOW_SC_CONTENTS_20                           1037
#define ROWOW_SC_OVERCOMING_CRISIS                     1038
#define ROWOW_SC_CHASING                               1039
#define ROWOW_SC_FIRE_CHARM_POWER                      1040
#define ROWOW_SC_WATER_CHARM_POWER                     1041
#define ROWOW_SC_WIND_CHARM_POWER                      1042
#define ROWOW_SC_GROUND_CHARM_POWER                    1043
#define ROWOW_SC_MAX                                   1044

// The status changes by name (sc_type without SC_), as status.yml and the skill database name them in any case
inline constexpr std::pair<char const*, int32> ROWOWStatusNames[] =
{
    { "STONE", 0 },
    { "FREEZE", 1 },
    { "STUN", 2 },
    { "SLEEP", 3 },
    { "POISON", 4 },
    { "CURSE", 5 },
    { "SILENCE", 6 },
    { "CONFUSION", 7 },
    { "BLIND", 8 },
    { "BLEEDING", 9 },
    { "DPOISON", 10 },
    { "STONEWAIT", 11 },
    { "PROVOKE", 20 },
    { "ENDURE", 21 },
    { "TWOHANDQUICKEN", 22 },
    { "CONCENTRATE", 23 },
    { "HIDING", 24 },
    { "CLOAKING", 25 },
    { "ENCPOISON", 26 },
    { "POISONREACT", 27 },
    { "QUAGMIRE", 28 },
    { "ANGELUS", 29 },
    { "BLESSING", 30 },
    { "SIGNUMCRUCIS", 31 },
    { "INCREASEAGI", 32 },
    { "DECREASEAGI", 33 },
    { "SLOWPOISON", 34 },
    { "IMPOSITIO", 35 },
    { "SUFFRAGIUM", 36 },
    { "ASPERSIO", 37 },
    { "BENEDICTIO", 38 },
    { "KYRIE", 39 },
    { "MAGNIFICAT", 40 },
    { "GLORIA", 41 },
    { "AETERNA", 42 },
    { "ADRENALINE", 43 },
    { "WEAPONPERFECTION", 44 },
    { "OVERTHRUST", 45 },
    { "MAXIMIZEPOWER", 46 },
    { "TRICKDEAD", 47 },
    { "LOUD", 48 },
    { "ENERGYCOAT", 49 },
    { "BROKENARMOR", 50 },
    { "BROKENWEAPON", 51 },
    { "HALLUCINATION", 52 },
    { "WEIGHT50", 53 },
    { "WEIGHT90", 54 },
    { "ASPDPOTION0", 55 },
    { "ASPDPOTION1", 56 },
    { "ASPDPOTION2", 57 },
    { "ASPDPOTION3", 58 },
    { "SPEEDUP0", 59 },
    { "SPEEDUP1", 60 },
    { "ATKPOTION", 61 },
    { "MATKPOTION", 62 },
    { "WEDDING", 63 },
    { "SLOWDOWN", 64 },
    { "ANKLE", 65 },
    { "KEEPING", 66 },
    { "BARRIER", 67 },
    { "STRIPWEAPON", 68 },
    { "STRIPSHIELD", 69 },
    { "STRIPARMOR", 70 },
    { "STRIPHELM", 71 },
    { "CP_WEAPON", 72 },
    { "CP_SHIELD", 73 },
    { "CP_ARMOR", 74 },
    { "CP_HELM", 75 },
    { "AUTOGUARD", 76 },
    { "REFLECTSHIELD", 77 },
    { "SPLASHER", 78 },
    { "PROVIDENCE", 79 },
    { "DEFENDER", 80 },
    { "MAGICROD", 81 },
    { "SPELLBREAKER", 82 },
    { "AUTOSPELL", 83 },
    { "SIGHTTRASHER", 84 },
    { "AUTOBERSERK", 85 },
    { "SPEARQUICKEN", 86 },
    { "AUTOCOUNTER", 87 },
    { "SIGHT", 88 },
    { "SAFETYWALL", 89 },
    { "RUWACH", 90 },
    { "EXTREMITYFIST", 91 },
    { "EXPLOSIONSPIRITS", 92 },
    { "COMBO", 93 },
    { "BLADESTOP_WAIT", 94 },
    { "BLADESTOP", 95 },
    { "FIREWEAPON", 96 },
    { "WATERWEAPON", 97 },
    { "WINDWEAPON", 98 },
    { "EARTHWEAPON", 99 },
    { "VOLCANO", 100 },
    { "DELUGE", 101 },
    { "VIOLENTGALE", 102 },
    { "WATK_ELEMENT", 103 },
    { "ARMOR", 104 },
    { "ARMOR_ELEMENT_WATER", 105 },
    { "NOCHAT", 106 },
    { "PROTECTEXP", 107 },
    { "AURABLADE", 108 },
    { "PARRYING", 109 },
    { "CONCENTRATION", 110 },
    { "TENSIONRELAX", 111 },
    { "BERSERK", 112 },
    { "FURY", 113 },
    { "GOSPEL", 114 },
    { "ASSUMPTIO", 115 },
    { "BASILICA", 116 },
    { "GUILDAURA", 117 },
    { "MAGICPOWER", 118 },
    { "EDP", 119 },
    { "TRUESIGHT", 120 },
    { "WINDWALK", 121 },
    { "MELTDOWN", 122 },
    { "CARTBOOST", 123 },
    { "CHASEWALK", 124 },
    { "REJECTSWORD", 125 },
    { "MARIONETTE", 126 },
    { "MARIONETTE2", 127 },
    { "CHANGEUNDEAD", 128 },
    { "JOINTBEAT", 129 },
    { "MINDBREAKER", 130 },
    { "MEMORIZE", 131 },
    { "FOGWALL", 132 },
    { "SPIDERWEB", 133 },
    { "DEVOTION", 134 },
    { "SACRIFICE", 135 },
    { "STEELBODY", 136 },
    { "ORCISH", 137 },
    { "READYSTORM", 138 },
    { "READYDOWN", 139 },
    { "READYTURN", 140 },
    { "READYCOUNTER", 141 },
    { "DODGE", 142 },
    { "RUN", 143 },
    { "SHADOWWEAPON", 144 },
    { "ADRENALINE2", 145 },
    { "GHOSTWEAPON", 146 },
    { "KAIZEL", 147 },
    { "KAAHI", 148 },
    { "KAUPE", 149 },
    { "ONEHAND", 150 },
    { "PRESERVE", 151 },
    { "BATTLEORDERS", 152 },
    { "REGENERATION", 153 },
    { "DOUBLECAST", 154 },
    { "GRAVITATION", 155 },
    { "MAXOVERTHRUST", 156 },
    { "LONGING", 157 },
    { "HERMODE", 158 },
    { "SHRINK", 159 },
    { "SIGHTBLASTER", 160 },
    { "WINKCHARM", 161 },
    { "CLOSECONFINE", 162 },
    { "CLOSECONFINE2", 163 },
    { "DANCING", 164 },
    { "ELEMENTALCHANGE", 165 },
    { "RICHMANKIM", 166 },
    { "ETERNALCHAOS", 167 },
    { "DRUMBATTLE", 168 },
    { "NIBELUNGEN", 169 },
    { "ROKISWEIL", 170 },
    { "INTOABYSS", 171 },
    { "SIEGFRIED", 172 },
    { "WHISTLE", 173 },
    { "ASSNCROS", 174 },
    { "POEMBRAGI", 175 },
    { "APPLEIDUN", 176 },
    { "MODECHANGE", 177 },
    { "HUMMING", 178 },
    { "DONTFORGETME", 179 },
    { "FORTUNE", 180 },
    { "SERVICE4U", 181 },
    { "STOP", 182 },
    { "SPURT", 183 },
    { "SPIRIT", 184 },
    { "COMA", 185 },
    { "INTRAVISION", 186 },
    { "INCALLSTATUS", 187 },
    { "INCSTR", 188 },
    { "INCAGI", 189 },
    { "INCVIT", 190 },
    { "INCINT", 191 },
    { "INCDEX", 192 },
    { "INCLUK", 193 },
    { "INCHIT", 194 },
    { "INCHITRATE", 195 },
    { "INCFLEE", 196 },
    { "INCFLEERATE", 197 },
    { "INCMHPRATE", 198 },
    { "INCMSPRATE", 199 },
    { "INCATKRATE", 200 },
    { "INCMATKRATE", 201 },
    { "INCDEFRATE", 202 },
    { "STRFOOD", 203 },
    { "AGIFOOD", 204 },
    { "VITFOOD", 205 },
    { "INTFOOD", 206 },
    { "DEXFOOD", 207 },
    { "LUKFOOD", 208 },
    { "HITFOOD", 209 },
    { "FLEEFOOD", 210 },
    { "BATKFOOD", 211 },
    { "WATKFOOD", 212 },
    { "MATKFOOD", 213 },
    { "SCRESIST", 214 },
    { "XMAS", 215 },
    { "WARM", 216 },
    { "SUN_COMFORT", 217 },
    { "MOON_COMFORT", 218 },
    { "STAR_COMFORT", 219 },
    { "FUSION", 220 },
    { "SKILLRATE_UP", 221 },
    { "SKE", 222 },
    { "KAITE", 223 },
    { "SWOO", 224 },
    { "SKA", 225 },
    { "EARTHSCROLL", 226 },
    { "MIRACLE", 227 },
    { "MADNESSCANCEL", 228 },
    { "ADJUSTMENT", 229 },
    { "INCREASING", 230 },
    { "GATLINGFEVER", 231 },
    { "TATAMIGAESHI", 232 },
    { "UTSUSEMI", 233 },
    { "BUNSINJYUTSU", 234 },
    { "KAENSIN", 235 },
    { "SUITON", 236 },
    { "NEN", 237 },
    { "KNOWLEDGE", 238 },
    { "SMA", 239 },
    { "FLING", 240 },
    { "AVOID", 241 },
    { "CHANGE", 242 },
    { "BLOODLUST", 243 },
    { "FLEET", 244 },
    { "SPEED", 245 },
    { "DEFENCE", 246 },
    { "INCASPDRATE", 247 },
    { "INCFLEE2", 248 },
    { "JAILED", 249 },
    { "ENCHANTARMS", 250 },
    { "MAGICALATTACK", 251 },
    { "ARMORCHANGE", 252 },
    { "CRITICALWOUND", 253 },
    { "MAGICMIRROR", 254 },
    { "SLOWCAST", 255 },
    { "SUMMER", 256 },
    { "EXPBOOST", 257 },
    { "ITEMBOOST", 258 },
    { "BOSSMAPINFO", 259 },
    { "LIFEINSURANCE", 260 },
    { "INCCRI", 261 },
    { "MDEF_RATE", 265 },
    { "INCHEALRATE", 267 },
    { "PNEUMA", 268 },
    { "AUTOTRADE", 269 },
    { "KSPROTECTED", 270 },
    { "ARMOR_RESIST", 271 },
    { "SPCOST_RATE", 272 },
    { "COMMONSC_RESIST", 273 },
    { "SEVENWIND", 274 },
    { "DEF_RATE", 275 },
    { "WALKSPEED", 277 },
    { "MERC_FLEEUP", 278 },
    { "MERC_ATKUP", 279 },
    { "MERC_HPUP", 280 },
    { "MERC_SPUP", 281 },
    { "MERC_HITUP", 282 },
    { "MERC_QUICKEN", 283 },
    { "REBIRTH", 284 },
    { "ITEMSCRIPT", 289 },
    { "S_LIFEPOTION", 290 },
    { "L_LIFEPOTION", 291 },
    { "JEXPBOOST", 292 },
    { "HELLPOWER", 294 },
    { "INVINCIBLE", 295 },
    { "MANU_ATK", 297 },
    { "MANU_DEF", 298 },
    { "SPL_ATK", 299 },
    { "SPL_DEF", 300 },
    { "MANU_MATK", 301 },
    { "SPL_MATK", 302 },
    { "FOOD_STR_CASH", 303 },
    { "FOOD_AGI_CASH", 304 },
    { "FOOD_VIT_CASH", 305 },
    { "FOOD_DEX_CASH", 306 },
    { "FOOD_INT_CASH", 307 },
    { "FOOD_LUK_CASH", 308 },
    { "FEAR", 309 },
    { "BURNING", 310 },
    { "FREEZING", 311 },
    { "ENCHANTBLADE", 312 },
    { "DEATHBOUND", 313 },
    { "MILLENNIUMSHIELD", 314 },
    { "CRUSHSTRIKE", 315 },
    { "REFRESH", 316 },
    { "REUSE_REFRESH", 317 },
    { "GIANTGROWTH", 318 },
    { "STONEHARDSKIN", 319 },
    { "VITALITYACTIVATION", 320 },
    { "STORMBLAST", 321 },
    { "FIGHTINGSPIRIT", 322 },
    { "ABUNDANCE", 323 },
    { "ADORAMUS", 324 },
    { "EPICLESIS", 325 },
    { "ORATIO", 326 },
    { "LAUDAAGNUS", 327 },
    { "LAUDARAMUS", 328 },
    { "RENOVATIO", 329 },
    { "EXPIATIO", 330 },
    { "DUPLELIGHT", 331 },
    { "SECRAMENT", 332 },
    { "WHITEIMPRISON", 333 },
    { "MARSHOFABYSS", 334 },
    { "RECOGNIZEDSPELL", 335 },
    { "STASIS", 336 },
    { "SPHERE_1", 337 },
    { "SPHERE_2", 338 },
    { "SPHERE_3", 339 },
    { "SPHERE_4", 340 },
    { "SPHERE_5", 341 },
    { "READING_SB", 342 },
    { "FREEZE_SP", 343 },
    { "FEARBREEZE", 344 },
    { "ELECTRICSHOCKER", 345 },
    { "WUGDASH", 346 },
    { "BITE", 347 },
    { "CAMOUFLAGE", 348 },
    { "ACCELERATION", 349 },
    { "HOVERING", 350 },
    { "SHAPESHIFT", 351 },
    { "INFRAREDSCAN", 352 },
    { "ANALYZE", 353 },
    { "MAGNETICFIELD", 354 },
    { "NEUTRALBARRIER", 355 },
    { "NEUTRALBARRIER_MASTER", 356 },
    { "STEALTHFIELD", 357 },
    { "STEALTHFIELD_MASTER", 358 },
    { "OVERHEAT", 359 },
    { "OVERHEAT_LIMITPOINT", 360 },
    { "VENOMIMPRESS", 361 },
    { "POISONINGWEAPON", 362 },
    { "WEAPONBLOCKING", 363 },
    { "CLOAKINGEXCEED", 364 },
    { "HALLUCINATIONWALK", 365 },
    { "HALLUCINATIONWALK_POSTDELAY", 366 },
    { "ROLLINGCUTTER", 367 },
    { "TOXIN", 368 },
    { "PARALYSE", 369 },
    { "VENOMBLEED", 370 },
    { "MAGICMUSHROOM", 371 },
    { "DEATHHURT", 372 },
    { "PYREXIA", 373 },
    { "OBLIVIONCURSE", 374 },
    { "LEECHESEND", 375 },
    { "REFLECTDAMAGE", 376 },
    { "FORCEOFVANGUARD", 377 },
    { "SHIELDSPELL_HP", 378 },
    { "SHIELDSPELL_SP", 379 },
    { "SHIELDSPELL_ATK", 380 },
    { "EXEEDBREAK", 381 },
    { "PRESTIGE", 382 },
    { "BANDING", 383 },
    { "BANDING_DEFENCE", 384 },
    { "EARTHDRIVE", 385 },
    { "INSPIRATION", 386 },
    { "SPELLFIST", 387 },
    { "CRYSTALIZE", 388 },
    { "STRIKING", 389 },
    { "WARMER", 390 },
    { "VACUUM_EXTREME", 391 },
    { "PROPERTYWALK", 392 },
    { "SWINGDANCE", 393 },
    { "SYMPHONYOFLOVER", 394 },
    { "MOONLITSERENADE", 395 },
    { "RUSHWINDMILL", 396 },
    { "ECHOSONG", 397 },
    { "HARMONIZE", 398 },
    { "VOICEOFSIREN", 399 },
    { "DEEPSLEEP", 400 },
    { "SIRCLEOFNATURE", 401 },
    { "GLOOMYDAY", 402 },
    { "GLOOMYDAY_SK", 403 },
    { "SONGOFMANA", 404 },
    { "DANCEWITHWUG", 405 },
    { "SATURDAYNIGHTFEVER", 406 },
    { "LERADSDEW", 407 },
    { "MELODYOFSINK", 408 },
    { "BEYONDOFWARCRY", 409 },
    { "UNLIMITEDHUMMINGVOICE", 410 },
    { "SITDOWN_FORCE", 411 },
    { "NETHERWORLD", 412 },
    { "CRESCENTELBOW", 413 },
    { "CURSEDCIRCLE_ATKER", 414 },
    { "CURSEDCIRCLE_TARGET", 415 },
    { "LIGHTNINGWALK", 416 },
    { "RAISINGDRAGON", 417 },
    { "GT_ENERGYGAIN", 418 },
    { "GT_CHANGE", 419 },
    { "GT_REVITALIZE", 420 },
    { "GN_CARTBOOST", 421 },
    { "THORNSTRAP", 422 },
    { "BLOODSUCKER", 423 },
    { "SMOKEPOWDER", 424 },
    { "TEARGAS", 425 },
    { "MANDRAGORA", 426 },
    { "STOMACHACHE", 427 },
    { "MYSTERIOUS_POWDER", 428 },
    { "MELON_BOMB", 429 },
    { "BANANA_BOMB", 430 },
    { "BANANA_BOMB_SITDOWN", 431 },
    { "SAVAGE_STEAK", 432 },
    { "COCKTAIL_WARG_BLOOD", 433 },
    { "MINOR_BBQ", 434 },
    { "SIROMA_ICE_TEA", 435 },
    { "DROCERA_HERB_STEAMED", 436 },
    { "PUTTI_TAILS_NOODLES", 437 },
    { "BOOST500", 438 },
    { "FULL_SWING_K", 439 },
    { "MANA_PLUS", 440 },
    { "MUSTLE_M", 441 },
    { "LIFE_FORCE_F", 442 },
    { "EXTRACT_WHITE_POTION_Z", 443 },
    { "VITATA_500", 444 },
    { "EXTRACT_SALAMINE_JUICE", 445 },
    { "_REPRODUCE", 446 },
    { "_AUTOSHADOWSPELL", 447 },
    { "_SHADOWFORM", 448 },
    { "_BODYPAINT", 449 },
    { "_INVISIBILITY", 450 },
    { "_DEADLYINFECT", 451 },
    { "_ENERVATION", 452 },
    { "_GROOMY", 453 },
    { "_IGNORANCE", 454 },
    { "_LAZINESS", 455 },
    { "_UNLUCKY", 456 },
    { "_WEAKNESS", 457 },
    { "_STRIPACCESSORY", 458 },
    { "_MANHOLE", 459 },
    { "_BLOODYLUST", 460 },
    { "CIRCLE_OF_FIRE", 461 },
    { "CIRCLE_OF_FIRE_OPTION", 462 },
    { "FIRE_CLOAK", 463 },
    { "FIRE_CLOAK_OPTION", 464 },
    { "WATER_SCREEN", 465 },
    { "WATER_SCREEN_OPTION", 466 },
    { "WATER_DROP", 467 },
    { "WATER_DROP_OPTION", 468 },
    { "WATER_BARRIER", 469 },
    { "WIND_STEP", 470 },
    { "WIND_STEP_OPTION", 471 },
    { "WIND_CURTAIN", 472 },
    { "WIND_CURTAIN_OPTION", 473 },
    { "ZEPHYR", 474 },
    { "SOLID_SKIN", 475 },
    { "SOLID_SKIN_OPTION", 476 },
    { "STONE_SHIELD", 477 },
    { "STONE_SHIELD_OPTION", 478 },
    { "POWER_OF_GAIA", 479 },
    { "PYROTECHNIC", 480 },
    { "PYROTECHNIC_OPTION", 481 },
    { "HEATER", 482 },
    { "HEATER_OPTION", 483 },
    { "TROPIC", 484 },
    { "TROPIC_OPTION", 485 },
    { "AQUAPLAY", 486 },
    { "AQUAPLAY_OPTION", 487 },
    { "COOLER", 488 },
    { "COOLER_OPTION", 489 },
    { "CHILLY_AIR", 490 },
    { "CHILLY_AIR_OPTION", 491 },
    { "GUST", 492 },
    { "GUST_OPTION", 493 },
    { "BLAST", 494 },
    { "BLAST_OPTION", 495 },
    { "WILD_STORM", 496 },
    { "WILD_STORM_OPTION", 497 },
    { "PETROLOGY", 498 },
    { "PETROLOGY_OPTION", 499 },
    { "CURSED_SOIL", 500 },
    { "CURSED_SOIL_OPTION", 501 },
    { "UPHEAVAL", 502 },
    { "UPHEAVAL_OPTION", 503 },
    { "TIDAL_WEAPON", 504 },
    { "TIDAL_WEAPON_OPTION", 505 },
    { "ROCK_CRUSHER", 506 },
    { "ROCK_CRUSHER_ATK", 507 },
    { "LEADERSHIP", 508 },
    { "GLORYWOUNDS", 509 },
    { "SOULCOLD", 510 },
    { "HAWKEYES", 511 },
    { "ODINS_POWER", 512 },
    { "RAID", 513 },
    { "FIRE_INSIGNIA", 514 },
    { "WATER_INSIGNIA", 515 },
    { "WIND_INSIGNIA", 516 },
    { "EARTH_INSIGNIA", 517 },
    { "PUSH_CART", 518 },
    { "SPELLBOOK1", 519 },
    { "SPELLBOOK2", 520 },
    { "SPELLBOOK3", 521 },
    { "SPELLBOOK4", 522 },
    { "SPELLBOOK5", 523 },
    { "SPELLBOOK6", 524 },
    { "MAXSPELLBOOK", 525 },
    { "INCMHP", 526 },
    { "INCMSP", 527 },
    { "PARTYFLEE", 528 },
    { "MEIKYOUSISUI", 529 },
    { "JYUMONJIKIRI", 530 },
    { "KYOUGAKU", 531 },
    { "IZAYOI", 532 },
    { "ZENKAI", 533 },
    { "KAGEHUMI", 534 },
    { "KYOMU", 535 },
    { "KAGEMUSYA", 536 },
    { "ZANGETSU", 537 },
    { "GENSOU", 538 },
    { "AKAITSUKI", 539 },
    { "STYLE_CHANGE", 540 },
    { "TINDER_BREAKER", 541 },
    { "TINDER_BREAKER2", 542 },
    { "CBC", 543 },
    { "EQC", 544 },
    { "GOLDENE_FERSE", 545 },
    { "ANGRIFFS_MODUS", 546 },
    { "OVERED_BOOST", 547 },
    { "LIGHT_OF_REGENE", 548 },
    { "ASH", 549 },
    { "GRANITIC_ARMOR", 550 },
    { "MAGMA_FLOW", 551 },
    { "PYROCLASTIC", 552 },
    { "PARALYSIS", 553 },
    { "PAIN_KILLER", 554 },
    { "HANBOK", 555 },
    { "DEFSET", 556 },
    { "MDEFSET", 557 },
    { "DARKCROW", 558 },
    { "FULL_THROTTLE", 559 },
    { "REBOUND", 560 },
    { "UNLIMIT", 561 },
    { "KINGS_GRACE", 562 },
    { "TELEKINESIS_INTENSE", 563 },
    { "OFFERTORIUM", 564 },
    { "FRIGG_SONG", 565 },
    { "MONSTER_TRANSFORM", 566 },
    { "ANGEL_PROTECT", 567 },
    { "ILLUSIONDOPING", 568 },
    { "FLASHCOMBO", 569 },
    { "MOONSTAR", 570 },
    { "SUPER_STAR", 571 },
    { "HEAT_BARREL", 572 },
    { "MAGICALBULLET", 573 },
    { "P_ALTER", 574 },
    { "E_CHAIN", 575 },
    { "C_MARKER", 576 },
    { "ANTI_M_BLAST", 577 },
    { "B_TRAP", 578 },
    { "H_MINE", 579 },
    { "QD_SHOT_READY", 580 },
    { "MTF_ASPD", 581 },
    { "MTF_RANGEATK", 582 },
    { "MTF_MATK", 583 },
    { "MTF_MLEATKED", 584 },
    { "MTF_CRIDAMAGE", 585 },
    { "OKTOBERFEST", 586 },
    { "STRANGELIGHTS", 587 },
    { "DECORATION_OF_MUSIC", 588 },
    { "QUEST_BUFF1", 589 },
    { "QUEST_BUFF2", 590 },
    { "QUEST_BUFF3", 591 },
    { "ALL_RIDING", 592 },
    { "TEARGAS_SOB", 593 },
    { "_FEINTBOMB", 594 },
    { "_CHAOS", 595 },
    { "CHASEWALK2", 596 },
    { "VACUUM_EXTREME_POSTDELAY", 597 },
    { "MTF_ASPD2", 598 },
    { "MTF_RANGEATK2", 599 },
    { "MTF_MATK2", 600 },
    { "2011RWC_SCROLL", 601 },
    { "JP_EVENT04", 602 },
    { "MTF_MHP", 603 },
    { "MTF_MSP", 604 },
    { "MTF_PUMPKIN", 605 },
    { "MTF_HITFLEE", 606 },
    { "CRIFOOD", 607 },
    { "ATTHASTE_CASH", 608 },
    { "REUSE_LIMIT_A", 609 },
    { "REUSE_LIMIT_B", 610 },
    { "REUSE_LIMIT_C", 611 },
    { "REUSE_LIMIT_D", 612 },
    { "REUSE_LIMIT_E", 613 },
    { "REUSE_LIMIT_F", 614 },
    { "REUSE_LIMIT_G", 615 },
    { "REUSE_LIMIT_H", 616 },
    { "REUSE_LIMIT_MTF", 617 },
    { "REUSE_LIMIT_ASPD_POTION", 618 },
    { "REUSE_MILLENNIUMSHIELD", 619 },
    { "REUSE_CRUSHSTRIKE", 620 },
    { "REUSE_STORMBLAST", 621 },
    { "ALL_RIDING_REUSE_LIMIT", 622 },
    { "REUSE_LIMIT_ECL", 623 },
    { "REUSE_LIMIT_RECALL", 624 },
    { "PROMOTE_HEALTH_RESERCH", 625 },
    { "ENERGY_DRINK_RESERCH", 626 },
    { "NORECOVER_STATE", 627 },
    { "SUHIDE", 628 },
    { "SU_STOOP", 629 },
    { "SPRITEMABLE", 630 },
    { "CATNIPPOWDER", 631 },
    { "SV_ROOTTWIST", 632 },
    { "BITESCAR", 633 },
    { "ARCLOUSEDASH", 634 },
    { "TUNAPARTY", 635 },
    { "SHRIMP", 636 },
    { "FRESHSHRIMP", 637 },
    { "ACTIVE_MONSTER_TRANSFORM", 638 },
    { "CLOUD_KILL", 639 },
    { "LJOSALFAR", 640 },
    { "MERMAID_LONGING", 641 },
    { "HAT_EFFECT", 642 },
    { "FLOWERSMOKE", 643 },
    { "FSTONE", 644 },
    { "HAPPINESS_STAR", 645 },
    { "MAPLE_FALLS", 646 },
    { "TIME_ACCESSORY", 647 },
    { "MAGICAL_FEATHER", 648 },
    { "GVG_GIANT", 649 },
    { "GVG_GOLEM", 650 },
    { "GVG_STUN", 651 },
    { "GVG_STONE", 652 },
    { "GVG_FREEZ", 653 },
    { "GVG_SLEEP", 654 },
    { "GVG_CURSE", 655 },
    { "GVG_SILENCE", 656 },
    { "GVG_BLIND", 657 },
    { "CLAN_INFO", 658 },
    { "SWORDCLAN", 659 },
    { "ARCWANDCLAN", 660 },
    { "GOLDENMACECLAN", 661 },
    { "CROSSBOWCLAN", 662 },
    { "JUMPINGCLAN", 663 },
    { "TAROTCARD", 664 },
    { "GEFFEN_MAGIC1", 665 },
    { "GEFFEN_MAGIC2", 666 },
    { "GEFFEN_MAGIC3", 667 },
    { "MAXPAIN", 668 },
    { "ARMOR_ELEMENT_EARTH", 669 },
    { "ARMOR_ELEMENT_FIRE", 670 },
    { "ARMOR_ELEMENT_WIND", 671 },
    { "DAILYSENDMAILCNT", 672 },
    { "DORAM_BUF_01", 673 },
    { "DORAM_BUF_02", 674 },
    { "HISS", 675 },
    { "NYANGGRASS", 676 },
    { "GROOMING", 677 },
    { "SHRIMPBLESSING", 678 },
    { "CHATTERING", 679 },
    { "DORAM_WALKSPEED", 680 },
    { "DORAM_MATK", 681 },
    { "DORAM_FLEE2", 682 },
    { "DORAM_SVSP", 683 },
    { "FALLEN_ANGEL", 684 },
    { "CHEERUP", 685 },
    { "DRESSUP", 686 },
    { "GLASTHEIM_ATK", 687 },
    { "GLASTHEIM_DEF", 688 },
    { "GLASTHEIM_HEAL", 689 },
    { "GLASTHEIM_HIDDEN", 690 },
    { "GLASTHEIM_STATE", 691 },
    { "GLASTHEIM_ITEMDEF", 692 },
    { "GLASTHEIM_HPSP", 693 },
    { "LHZ_DUN_N1", 694 },
    { "LHZ_DUN_N2", 695 },
    { "LHZ_DUN_N3", 696 },
    { "LHZ_DUN_N4", 697 },
    { "ANCILLA", 698 },
    { "EARTHSHAKER", 699 },
    { "WEAPONBLOCK_ON", 700 },
    { "SPORE_EXPLOSION", 701 },
    { "ADAPTATION", 702 },
    { "BASILICA_CELL", 703 },
    { "ENTRY_QUEUE_APPLY_DELAY", 704 },
    { "ENTRY_QUEUE_NOTIFY_ADMISSION_TIME_OUT", 705 },
    { "LIGHTOFMOON", 706 },
    { "LIGHTOFSUN", 707 },
    { "LIGHTOFSTAR", 708 },
    { "LUNARSTANCE", 709 },
    { "UNIVERSESTANCE", 710 },
    { "SUNSTANCE", 711 },
    { "FLASHKICK", 712 },
    { "NEWMOON", 713 },
    { "STARSTANCE", 714 },
    { "DIMENSION", 715 },
    { "DIMENSION1", 716 },
    { "DIMENSION2", 717 },
    { "CREATINGSTAR", 718 },
    { "FALLINGSTAR", 719 },
    { "NOVAEXPLOSING", 720 },
    { "GRAVITYCONTROL", 721 },
    { "SOULCOLLECT", 722 },
    { "SOULREAPER", 723 },
    { "SOULUNITY", 724 },
    { "SOULSHADOW", 725 },
    { "SOULFAIRY", 726 },
    { "SOULFALCON", 727 },
    { "SOULGOLEM", 728 },
    { "SOULDIVISION", 729 },
    { "SOULENERGY", 730 },
    { "USE_SKILL_SP_SPA", 731 },
    { "USE_SKILL_SP_SHA", 732 },
    { "SP_SHA", 733 },
    { "SOULCURSE", 734 },
    { "HELLS_PLANT", 735 },
    { "INCREASE_MAXHP", 736 },
    { "INCREASE_MAXSP", 737 },
    { "REF_T_POTION", 738 },
    { "ADD_ATK_DAMAGE", 739 },
    { "ADD_MATK_DAMAGE", 740 },
    { "HELPANGEL", 741 },
    { "SOUNDOFDESTRUCTION", 742 },
    { "LUXANIMA", 743 },
    { "REUSE_LIMIT_LUXANIMA", 744 },
    { "ENSEMBLEFATIGUE", 745 },
    { "MISTY_FROST", 746 },
    { "MAGIC_POISON", 747 },
    { "EP16_2_BUFF_SS", 748 },
    { "EP16_2_BUFF_SC", 749 },
    { "EP16_2_BUFF_AC", 750 },
    { "OVERBRANDREADY", 751 },
    { "POISON_MIST", 752 },
    { "STONE_WALL", 753 },
    { "CLOUD_POISON", 754 },
    { "HOMUN_TIME", 755 },
    { "EMERGENCY_MOVE", 756 },
    { "MADOGEAR", 757 },
    { "NPC_HALLUCINATIONWALK", 758 },
    { "PACKING_ENVELOPE1", 759 },
    { "PACKING_ENVELOPE2", 760 },
    { "PACKING_ENVELOPE3", 761 },
    { "PACKING_ENVELOPE4", 762 },
    { "PACKING_ENVELOPE5", 763 },
    { "PACKING_ENVELOPE6", 764 },
    { "PACKING_ENVELOPE7", 765 },
    { "PACKING_ENVELOPE8", 766 },
    { "PACKING_ENVELOPE9", 767 },
    { "PACKING_ENVELOPE10", 768 },
    { "SOULATTACK", 769 },
    { "WIDEWEB", 770 },
    { "BURNT", 771 },
    { "CHILL", 772 },
    { "HANDICAPSTATE_DEEPBLIND", 773 },
    { "HANDICAPSTATE_DEEPSILENCE", 774 },
    { "HANDICAPSTATE_LASSITUDE", 775 },
    { "HANDICAPSTATE_FROSTBITE", 776 },
    { "HANDICAPSTATE_SWOONING", 777 },
    { "HANDICAPSTATE_LIGHTNINGSTRIKE", 778 },
    { "HANDICAPSTATE_CRYSTALLIZATION", 779 },
    { "HANDICAPSTATE_CONFLAGRATION", 780 },
    { "HANDICAPSTATE_MISFORTUNE", 781 },
    { "HANDICAPSTATE_DEADLYPOISON", 782 },
    { "HANDICAPSTATE_DEPRESSION", 783 },
    { "HANDICAPSTATE_HOLYFLAME", 784 },
    { "SERVANTWEAPON", 785 },
    { "SERVANT_SIGN", 786 },
    { "CHARGINGPIERCE", 787 },
    { "CHARGINGPIERCE_COUNT", 788 },
    { "DRAGONIC_AURA", 789 },
    { "VIGOR", 790 },
    { "DEADLY_DEFEASANCE", 791 },
    { "CLIMAX_DES_HU", 792 },
    { "CLIMAX", 793 },
    { "CLIMAX_EARTH", 794 },
    { "CLIMAX_BLOOM", 795 },
    { "CLIMAX_CRYIMP", 796 },
    { "WINDSIGN", 797 },
    { "CRESCIVEBOLT", 798 },
    { "CALAMITYGALE", 799 },
    { "MEDIALE", 800 },
    { "A_VITA", 801 },
    { "A_TELUM", 802 },
    { "PRE_ACIES", 803 },
    { "COMPETENTIA", 804 },
    { "RELIGIO", 805 },
    { "BENEDICTUM", 806 },
    { "AXE_STOMP", 807 },
    { "A_MACHINE", 808 },
    { "D_MACHINE", 809 },
    { "ABR_BATTLE_WARIOR", 810 },
    { "ABR_DUAL_CANNON", 811 },
    { "ABR_MOTHER_NET", 812 },
    { "ABR_INFINITY", 813 },
    { "SHADOW_EXCEED", 814 },
    { "DANCING_KNIFE", 815 },
    { "POTENT_VENOM", 816 },
    { "SHADOW_SCAR", 817 },
    { "E_SLASH_COUNT", 818 },
    { "SHADOW_WEAPON", 819 },
    { "GUARD_STANCE", 820 },
    { "ATTACK_STANCE", 821 },
    { "GUARDIAN_S", 822 },
    { "REBOUND_S", 823 },
    { "HOLY_S", 824 },
    { "ULTIMATE_S", 825 },
    { "SPEAR_SCAR", 826 },
    { "SHIELD_POWER", 827 },
    { "SPELL_ENCHANTING", 828 },
    { "SUMMON_ELEMENTAL_ARDOR", 829 },
    { "SUMMON_ELEMENTAL_DILUVIO", 830 },
    { "SUMMON_ELEMENTAL_PROCELLA", 831 },
    { "SUMMON_ELEMENTAL_TERREMOTUS", 832 },
    { "SUMMON_ELEMENTAL_SERPENS", 833 },
    { "ELEMENTAL_VEIL", 834 },
    { "MYSTIC_SYMPHONY", 835 },
    { "KVASIR_SONATA", 836 },
    { "SOUNDBLEND", 837 },
    { "GEF_NOCTURN", 838 },
    { "AIN_RHAPSODY", 839 },
    { "MUSICAL_INTERLUDE", 840 },
    { "JAWAII_SERENADE", 841 },
    { "PRON_MARCH", 842 },
    { "ROSEBLOSSOM", 843 },
    { "POWERFUL_FAITH", 844 },
    { "SINCERE_FAITH", 845 },
    { "FIRM_FAITH", 846 },
    { "HOLY_OIL", 847 },
    { "FIRST_BRAND", 848 },
    { "SECOND_BRAND", 849 },
    { "SECOND_JUDGE", 850 },
    { "THIRD_EXOR_FLAME", 851 },
    { "FIRST_FAITH_POWER", 852 },
    { "MASSIVE_F_BLASTER", 853 },
    { "PROTECTSHADOWEQUIP", 854 },
    { "RESEARCHREPORT", 855 },
    { "BO_HELL_DUSTY", 856 },
    { "BIONIC_WOODENWARRIOR", 857 },
    { "BIONIC_WOODEN_FAIRY", 858 },
    { "BIONIC_CREEPER", 859 },
    { "BIONIC_HELLTREE", 860 },
    { "SHADOW_STRIP", 861 },
    { "ABYSS_DAGGER", 862 },
    { "ABYSSFORCEWEAPON", 863 },
    { "ABYSS_SLAYER", 864 },
    { "FLAMETECHNIC", 865 },
    { "FLAMETECHNIC_OPTION", 866 },
    { "FLAMEARMOR", 867 },
    { "FLAMEARMOR_OPTION", 868 },
    { "COLD_FORCE", 869 },
    { "COLD_FORCE_OPTION", 870 },
    { "CRYSTAL_ARMOR", 871 },
    { "CRYSTAL_ARMOR_OPTION", 872 },
    { "GRACE_BREEZE", 873 },
    { "GRACE_BREEZE_OPTION", 874 },
    { "EYES_OF_STORM", 875 },
    { "EYES_OF_STORM_OPTION", 876 },
    { "EARTH_CARE", 877 },
    { "EARTH_CARE_OPTION", 878 },
    { "STRONG_PROTECTION", 879 },
    { "STRONG_PROTECTION_OPTION", 880 },
    { "DEEP_POISONING", 881 },
    { "DEEP_POISONING_OPTION", 882 },
    { "POISON_SHIELD", 883 },
    { "POISON_SHIELD_OPTION", 884 },
    { "SUB_WEAPONPROPERTY", 885 },
    { "M_LIFEPOTION", 886 },
    { "S_MANAPOTION", 887 },
    { "ALMIGHTY", 888 },
    { "ULTIMATECOOK", 889 },
    { "M_DEFSCROLL", 890 },
    { "INFINITY_DRINK", 891 },
    { "MENTAL_POTION", 892 },
    { "LIMIT_POWER_BOOSTER", 893 },
    { "COMBAT_PILL", 894 },
    { "COMBAT_PILL2", 895 },
    { "MYSTICPOWDER", 896 },
    { "SPARKCANDY", 897 },
    { "MAGICCANDY", 898 },
    { "ACARAJE", 899 },
    { "POPECOOKIE", 900 },
    { "VITALIZE_POTION", 901 },
    { "CUP_OF_BOZA", 902 },
    { "SKF_MATK", 903 },
    { "SKF_ATK", 904 },
    { "SKF_ASPD", 905 },
    { "SKF_CAST", 906 },
    { "BEEF_RIB_STEW", 907 },
    { "PORK_RIB_STEW", 908 },
    { "WEAPONBREAKER", 909 },
    { "TOXIN_OF_MANDARA", 910 },
    { "GOLDENE_TONE", 911 },
    { "TEMPERING", 912 },
    { "GRADUAL_GRAVITY", 913 },
    { "ALL_STAT_DOWN", 914 },
    { "KILLING_AURA", 915 },
    { "DAMAGE_HEAL", 916 },
    { "IMMUNE_PROPERTY_NOTHING", 917 },
    { "IMMUNE_PROPERTY_WATER", 918 },
    { "IMMUNE_PROPERTY_GROUND", 919 },
    { "IMMUNE_PROPERTY_FIRE", 920 },
    { "IMMUNE_PROPERTY_WIND", 921 },
    { "IMMUNE_PROPERTY_POISON", 922 },
    { "IMMUNE_PROPERTY_SAINT", 923 },
    { "IMMUNE_PROPERTY_DARKNESS", 924 },
    { "IMMUNE_PROPERTY_TELEKINESIS", 925 },
    { "IMMUNE_PROPERTY_UNDEAD", 926 },
    { "RELIEVE_ON", 927 },
    { "RELIEVE_OFF", 928 },
    { "RUSH_QUAKE1", 929 },
    { "RUSH_QUAKE2", 930 },
    { "G_LIFEPOTION", 931 },
    { "HNNOWEAPON", 932 },
    { "SHIELDCHAINRUSH", 933 },
    { "MISTYFROST", 934 },
    { "GROUNDGRAVITY", 935 },
    { "BREAKINGLIMIT", 936 },
    { "RULEBREAK", 937 },
    { "INTENSIVE_AIM", 938 },
    { "INTENSIVE_AIM_COUNT", 939 },
    { "GRENADE_FRAGMENT_1", 940 },
    { "GRENADE_FRAGMENT_2", 941 },
    { "GRENADE_FRAGMENT_3", 942 },
    { "GRENADE_FRAGMENT_4", 943 },
    { "GRENADE_FRAGMENT_5", 944 },
    { "GRENADE_FRAGMENT_6", 945 },
    { "AUTO_FIRING_LAUNCHER", 946 },
    { "HIDDEN_CARD", 947 },
    { "PERIOD_RECEIVEITEM_2ND", 948 },
    { "PERIOD_PLUSEXP_2ND", 949 },
    { "POWERUP", 951 },
    { "AGIUP", 952 },
    { "PROTECTION", 953 },
    { "BATH_FOAM_A", 954 },
    { "BATH_FOAM_B", 955 },
    { "BATH_FOAM_C", 956 },
    { "BUCHEDENOEL", 957 },
    { "EP16_DEF", 958 },
    { "STR_SCROLL", 959 },
    { "INT_SCROLL", 960 },
    { "CONTENTS_1", 961 },
    { "CONTENTS_2", 962 },
    { "CONTENTS_3", 963 },
    { "CONTENTS_4", 964 },
    { "CONTENTS_5", 965 },
    { "CONTENTS_6", 966 },
    { "CONTENTS_7", 967 },
    { "CONTENTS_8", 968 },
    { "CONTENTS_9", 969 },
    { "CONTENTS_10", 970 },
    { "MYSTERY_POWDER", 971 },
    { "CONTENTS_26", 972 },
    { "CONTENTS_27", 973 },
    { "CONTENTS_28", 974 },
    { "CONTENTS_29", 975 },
    { "CONTENTS_31", 976 },
    { "CONTENTS_32", 977 },
    { "CONTENTS_33", 978 },
    { "TALISMAN_OF_PROTECTION", 979 },
    { "TALISMAN_OF_WARRIOR", 980 },
    { "TALISMAN_OF_MAGICIAN", 981 },
    { "TALISMAN_OF_FIVE_ELEMENTS", 982 },
    { "T_FIRST_GOD", 983 },
    { "T_SECOND_GOD", 984 },
    { "T_THIRD_GOD", 985 },
    { "T_FOURTH_GOD", 986 },
    { "T_FIFTH_GOD", 987 },
    { "HEAVEN_AND_EARTH", 988 },
    { "TOTEM_OF_TUTELARY", 989 },
    { "RETURN_TO_ELDICASTES", 990 },
    { "GUARDIAN_RECALL", 991 },
    { "ECLAGE_RECALL", 992 },
    { "ALL_NIFLHEIM_RECALL", 993 },
    { "ALL_PRONTERA_RECALL", 994 },
    { "ALL_GLASTHEIM_RECALL", 995 },
    { "ALL_THANATOS_RECALL", 996 },
    { "ALL_LIGHTHALZEN_RECALL", 997 },
    { "HOGOGONG", 998 },
    { "MARINE_FESTIVAL", 999 },
    { "SANDY_FESTIVAL", 1000 },
    { "KI_SUL_RAMPAGE", 1001 },
    { "COLORS_OF_HYUN_ROK_1", 1002 },
    { "COLORS_OF_HYUN_ROK_2", 1003 },
    { "COLORS_OF_HYUN_ROK_3", 1004 },
    { "COLORS_OF_HYUN_ROK_4", 1005 },
    { "COLORS_OF_HYUN_ROK_5", 1006 },
    { "COLORS_OF_HYUN_ROK_6", 1007 },
    { "COLORS_OF_HYUN_ROK_BUFF", 1008 },
    { "TEMPORARY_COMMUNION", 1009 },
    { "BLESSING_OF_M_CREATURES", 1010 },
    { "BLESSING_OF_M_C_DEBUFF", 1011 },
    { "RISING_SUN", 1012 },
    { "NOON_SUN", 1013 },
    { "SUNSET_SUN", 1014 },
    { "RISING_MOON", 1015 },
    { "MIDNIGHT_MOON", 1016 },
    { "DAWN_MOON", 1017 },
    { "STAR_BURST", 1018 },
    { "SKY_ENCHANT", 1019 },
    { "WILD_WALK", 1020 },
    { "SHADOW_CLOCK", 1021 },
    { "SHINKIROU_CALL", 1022 },
    { "NIGHTMARE", 1023 },
    { "SBUNSHIN", 1024 },
    { "CONTENTS_34", 1025 },
    { "CONTENTS_35", 1026 },
    { "NOACTION", 1027 },
    { "C_BUFF_3", 1028 },
    { "C_BUFF_4", 1029 },
    { "C_BUFF_5", 1030 },
    { "C_BUFF_6", 1031 },
    { "CONTENTS_15", 1032 },
    { "CONTENTS_16", 1033 },
    { "CONTENTS_17", 1034 },
    { "CONTENTS_18", 1035 },
    { "CONTENTS_19", 1036 },
    { "CONTENTS_20", 1037 },
    { "OVERCOMING_CRISIS", 1038 },
    { "CHASING", 1039 },
    { "FIRE_CHARM_POWER", 1040 },
    { "WATER_CHARM_POWER", 1041 },
    { "WIND_CHARM_POWER", 1042 },
    { "GROUND_CHARM_POWER", 1043 },
};

#endif
