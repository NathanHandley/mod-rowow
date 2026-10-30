-- The RO form's own action bars (mod-rowow), packed as SMSG_ACTION_BUTTONS sends them (action | type << 24)
CREATE TABLE IF NOT EXISTS `mod_rowow_identity_action` (
	`guid` INT(10) UNSIGNED NOT NULL DEFAULT '0',
	`button` TINYINT(3) UNSIGNED NOT NULL DEFAULT '0',
	`action` INT(10) UNSIGNED NOT NULL DEFAULT '0',
	PRIMARY KEY (`guid`, `button`) USING BTREE
);
