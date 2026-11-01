-- The options of an RO party (mod-rowow, R11), a WoW group: rAthena's party.exp and party.item.  ExpShare 1 is Even Share; ItemShare bit 1 is the party's priority on items,
-- bit 2 the even share of what's picked up.  A group with no row has them all off, which is what a new RO party has
CREATE TABLE IF NOT EXISTS `mod_rowow_party` (
	`GroupGUID` INT(10) UNSIGNED NOT NULL DEFAULT '0',
	`ExpShare` TINYINT(3) UNSIGNED NOT NULL DEFAULT '0',
	`ItemShare` TINYINT(3) UNSIGNED NOT NULL DEFAULT '0',
	PRIMARY KEY (`GroupGUID`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
