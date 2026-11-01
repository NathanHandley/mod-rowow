-- RO's taming pets (mod-rowow, R11): rAthena's pet table.  A pet's egg names its row (mod_rowow_pet.PetID, on the item as an enchantment past any SpellItemEnchantment row).
-- OwnerGUID is the character that last hatched it, IsIncubate 1 while it's in its egg.  One with IsIncubate 0 is out with its owner
CREATE TABLE IF NOT EXISTS `mod_rowow_pet` (
	`PetID` INT(10) UNSIGNED NOT NULL DEFAULT '0',
	`OwnerGUID` INT(10) UNSIGNED NOT NULL DEFAULT '0',
	`MobID` INT(10) UNSIGNED NOT NULL DEFAULT '0',
	`Name` VARCHAR(48) NOT NULL DEFAULT '',
	`Level` SMALLINT(5) UNSIGNED NOT NULL DEFAULT '1',
	`EggID` INT(10) UNSIGNED NOT NULL DEFAULT '0',
	`EquipID` INT(10) UNSIGNED NOT NULL DEFAULT '0',
	`Intimate` SMALLINT(5) NOT NULL DEFAULT '0',
	`Hungry` SMALLINT(5) NOT NULL DEFAULT '0',
	`IsRenamed` TINYINT(3) UNSIGNED NOT NULL DEFAULT '0',
	`IsIncubate` TINYINT(3) UNSIGNED NOT NULL DEFAULT '1',
	PRIMARY KEY (`PetID`),
	KEY `OwnerGUID` (`OwnerGUID`, `IsIncubate`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
