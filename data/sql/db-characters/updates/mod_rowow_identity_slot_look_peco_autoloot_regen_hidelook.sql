-- The equipment window's hide buttons (mod-rowow, Nathan 2026-11-18; .ro hidelook): the visual slots whose items aren't drawn on the RO sprite,
-- ROWOW_HIDDEN_LOOK_* bits (upper, middle and lower headgear and garment, then the same four costume slots).  0 (the default) shows them all.  The
-- character's choice, shared by its save slots as autoloot is.  (This file's name sorts after mod_rowow_identity_slot_look_peco_autoloot_regen.sql.)
ALTER TABLE `mod_rowow_identity_character`
	ADD COLUMN `hidden_looks` TINYINT(3) UNSIGNED NOT NULL DEFAULT '0' COMMENT 'The RO visual slots not drawn: ROWOW_HIDDEN_LOOK_* bits';
