-- The layout of the stitched RO maps the worldserver last started with (mod-rowow, 2026-11-10): where each part (a former map, by its map key) sat in
-- its stitched map's grid.  At startup a part found somewhere else (RoMapMerges moved it, or the grid's size changed, which moves its middle) takes
-- the characters, home binds, corpses, entry points, RO save, last and return points and memo points saved on it along to the same cell
-- (ROWOWMod::MoveSavedSpotsOnMovedParts); then the rows are written again for the layout running.  A map with no rows yet only gets them.
CREATE TABLE IF NOT EXISTS `mod_rowow_map_layout` (
	`map` INT(10) UNSIGNED NOT NULL DEFAULT '0' COMMENT 'The stitched map''s WoW map ID',
	`part` INT(10) UNSIGNED NOT NULL DEFAULT '0' COMMENT 'The part''s map key (its former map ID)',
	`origin_x` FLOAT NOT NULL DEFAULT '0' COMMENT 'WoW position of the corner of the grid''s cell 0,0',
	`origin_y` FLOAT NOT NULL DEFAULT '0',
	`cell_size` FLOAT NOT NULL DEFAULT '0' COMMENT 'Yards',
	`cell_offset_x` INT(10) NOT NULL DEFAULT '0' COMMENT 'The part''s first cell in the grid',
	`cell_offset_y` INT(10) NOT NULL DEFAULT '0',
	`cell_width` INT(10) NOT NULL DEFAULT '0',
	`cell_height` INT(10) NOT NULL DEFAULT '0',
	PRIMARY KEY (`map`, `part`) USING BTREE
);
