BINARY = usbmidi
#CSTD = -std=gnu99

OPENCM3_DIR = ../libopencm3
LDSCRIPT = $(OPENCM3_DIR)/lib/stm32/f1/stm32f103x8.ld
#LDSCRIPT = ./stm32f103x8_DFU.ld


all:
	@killall st-util || true
	@rm -f usbmidi.elf
	@make -f buildcode.mk
	@st-util &
	@arm-none-eabi-gdb -x gdb_flash_script.gdbinit usbmidi.elf
	@killall st-util || true
