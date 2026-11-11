-- An RO identity's return tether (mod-rowow, Nathan 2026-11-10; ROWOW_ReturnTether.cpp): where its last return-to-save teleport (a Butterfly Wing, Teleport's
-- save point) was used, which clicking off the tether's aura goes back to, until expire_time (unix seconds)
CREATE TABLE IF NOT EXISTS `mod_rowow_identity_tether` (
	`guid` INT(10) UNSIGNED NOT NULL DEFAULT '0',
	`map` INT(10) UNSIGNED NOT NULL DEFAULT '0',
	`x` FLOAT NOT NULL DEFAULT '0',
	`y` FLOAT NOT NULL DEFAULT '0',
	`z` FLOAT NOT NULL DEFAULT '0',
	`o` FLOAT NOT NULL DEFAULT '0',
	`expire_time` INT(10) UNSIGNED NOT NULL DEFAULT '0',
	PRIMARY KEY (`guid`) USING BTREE
);
