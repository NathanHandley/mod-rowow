-- The RO equipment an RO identity wears (mod-rowow), by rAthena equipment position (its EQP_* bit)
CREATE TABLE IF NOT EXISTS `mod_rowow_identity_equipment` (
	`guid` INT(10) UNSIGNED NOT NULL DEFAULT '0',
	`equip_position` INT(10) UNSIGNED NOT NULL DEFAULT '0',
	`item_guid` INT(10) UNSIGNED NOT NULL DEFAULT '0' COMMENT 'item_instance guid',
	PRIMARY KEY (`guid`, `equip_position`) USING BTREE
);
