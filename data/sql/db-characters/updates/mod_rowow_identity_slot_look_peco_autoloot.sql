-- Autoloot (mod-rowow, Nathan 2026-11-10; Options > Interface > Ragnarok, .ro autoloot).  1: a monster's drops the character has the loot priority on go
-- straight into its bags when they fit (in a party, to the member the group's loot method picks); 0 (the default): on the ground.  The character's
-- choice, shared by its save slots as the Peco look is.  (This file's name sorts after mod_rowow_identity_slot_look_peco.sql.)
ALTER TABLE `mod_rowow_identity_character`
	ADD COLUMN `autoloot` TINYINT(3) UNSIGNED NOT NULL DEFAULT '0' COMMENT 'A monster''s drops the character has the loot priority on: 1 into the bags when they fit, 0 on the ground';
