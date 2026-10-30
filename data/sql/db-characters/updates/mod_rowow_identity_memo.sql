-- An RO identity's Warp Portal memo points (mod-rowow), rAthena's memo table: the map and RO cell of each
CREATE TABLE IF NOT EXISTS `mod_rowow_identity_memo` (
	`guid` INT(10) UNSIGNED NOT NULL DEFAULT '0',
	`slot` TINYINT(3) UNSIGNED NOT NULL DEFAULT '0',
	`map` INT(10) UNSIGNED NOT NULL DEFAULT '0',
	`x` INT(10) NOT NULL DEFAULT '0',
	`y` INT(10) NOT NULL DEFAULT '0',
	PRIMARY KEY (`guid`, `slot`) USING BTREE
);
