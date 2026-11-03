-- The carried set an RO identity's character has put away (mod-rowow): the backpack, bag slot and bag items of the side it isn't on (its own while
-- in the RO form, the form's while out of it), each with the place it goes back to.  bag 255 is the backpack and the bag slots, 19-22 the bag in that slot
CREATE TABLE IF NOT EXISTS `mod_rowow_identity_stowed_item` (
	`guid` INT(10) UNSIGNED NOT NULL DEFAULT '0',
	`item_guid` INT(10) UNSIGNED NOT NULL DEFAULT '0' COMMENT 'item_instance guid',
	`bag` TINYINT(3) UNSIGNED NOT NULL DEFAULT '0',
	`slot` TINYINT(3) UNSIGNED NOT NULL DEFAULT '0',
	PRIMARY KEY (`item_guid`) USING BTREE,
	KEY `idx_guid` (`guid`)
);
