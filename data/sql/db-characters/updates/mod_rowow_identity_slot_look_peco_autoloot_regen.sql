-- The regen options (mod-rowow, Nathan 2026-11-17; Options > Interface > Ragnarok, .ro hpregen and .ro spregen).  hp_regen_type 0 (the default,
-- Warcraft): 1% of the max HP every 2 seconds out of combat, 2% sitting, in place of RO's natural HP regen; 1 (Ragnarok): RO's natural HP regen.
-- sp_regen_rule 1 (the default): on top of RO's natural SP regen, 0.5% of the max SP every second once 5 seconds have passed since a skill spent
-- SP; 0: RO's alone.  The character's choices, shared by its save slots as autoloot is.  (This file's name sorts after
-- mod_rowow_identity_slot_look_peco_autoloot.sql.)
ALTER TABLE `mod_rowow_identity_character`
	ADD COLUMN `hp_regen_type` TINYINT(3) UNSIGNED NOT NULL DEFAULT '0' COMMENT 'The RO form''s HP regen: 0 Warcraft''s out of combat regen, 1 RO''s natural regen',
	ADD COLUMN `sp_regen_rule` TINYINT(3) UNSIGNED NOT NULL DEFAULT '1' COMMENT 'The RO form''s five second rule SP regen on top of RO''s: 1 on, 0 off';
