-- The Peco mount look (mod-rowow, Nathan 2026-11-10; Options > Interface > Ragnarok, .ro pecomount).  Out of Midgard, on a ground mount, a character
-- showing its Ragnarok identity as a job that rides a Peco, whose save slot in use has learned Riding, shows the sprite on its Peco in the mount's
-- place.  1 (the default) shows the Peco; 0 shows the mount as it is, with the sprite sitting on it.  The character's choice, as the job sprite is.
-- (This file's name sorts after mod_rowow_identity_slot.sql, which makes the table, and after mod_rowow_identity_slot_look.sql.)
ALTER TABLE `mod_rowow_identity_character`
	ADD COLUMN `peco_mount_look` TINYINT(3) UNSIGNED NOT NULL DEFAULT '1' COMMENT 'Out of Midgard, a Peco rider on a ground mount: 1 shows its Peco in the place of the mount, 0 the mount';
