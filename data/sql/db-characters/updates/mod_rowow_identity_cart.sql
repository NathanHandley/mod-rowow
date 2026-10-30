-- An RO identity's cart (mod-rowow), rAthena's cart_inventory: each slot's item as its template, count and the enchantments that make it
-- (the refine, broken and unidentified state, and the cards), space separated by enchantment slot
CREATE TABLE IF NOT EXISTS `mod_rowow_identity_cart` (
	`guid` INT(10) UNSIGNED NOT NULL DEFAULT '0',
	`slot` SMALLINT(5) UNSIGNED NOT NULL DEFAULT '0',
	`item_template` INT(10) UNSIGNED NOT NULL DEFAULT '0',
	`count` INT(10) UNSIGNED NOT NULL DEFAULT '0',
	`enchantments` VARCHAR(255) NOT NULL DEFAULT '',
	PRIMARY KEY (`guid`, `slot`) USING BTREE
);
