SCREENSIZE  := 128x64x1
FILESYSTEMS := common base_fonts 128x64x1
FONTS        = filesystem/$(FILESYSTEM)/media/12normal.fon \
               filesystem/$(FILESYSTEM)/media/04b03.fon
LANGUAGE    := devo10

include target/drivers/mcu/emu/Makefile.inc
