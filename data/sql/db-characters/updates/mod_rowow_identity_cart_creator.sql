-- Who made a cart item (a forged weapon, a brewed potion), which the item's ITEM_FIELD_CREATOR holds in the bags (rAthena's card[2] and card[3])
ALTER TABLE `mod_rowow_identity_cart` ADD COLUMN `creator` INT(10) UNSIGNED NOT NULL DEFAULT '0' COMMENT 'guid of its maker';
