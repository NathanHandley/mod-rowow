-- Fame points (rAthena's char.fame): the Blacksmith, Alchemist and Taekwon rankings
ALTER TABLE `mod_rowow_identity` ADD COLUMN `fame` INT(10) NOT NULL DEFAULT '0' COMMENT 'status.fame';
