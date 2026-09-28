MCU     = attiny412
PART    = t412
PORT   ?= /dev/ttyUSB0
BAUD   ?= 115200
TARGET  = lamp

CC      = avr-gcc
OBJCOPY = avr-objcopy
SIZE    = avr-size
AVRDUDE = avrdude -c serialupdi -p $(PART) -P $(PORT) -b $(BAUD)

CFLAGS  = -mmcu=$(MCU) -Os -std=gnu11 -Wall -Wextra \
          -ffunction-sections -fdata-sections
LDFLAGS = -mmcu=$(MCU) -Wl,--gc-sections

all: $(TARGET).hex

$(TARGET).elf: main.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<
	$(SIZE) $@

$(TARGET).hex: $(TARGET).elf
	$(OBJCOPY) -O ihex -R .eeprom -R .fuse $< $@

flash: $(TARGET).hex
	$(AVRDUDE) -U flash:w:$<:i

# Print BODCFG, OSCCFG and SYSCFG0. Factory values are fine for this firmware.
fuses:
	$(AVRDUDE) -U fuse1:r:-:h -U fuse2:r:-:h -U fuse5:r:-:h

# Print the supply voltage logged at the last power-up (EEPROM bytes 0-1, little endian).
vdd:
	$(AVRDUDE) -U eeprom:r:-:h 2>/dev/null | cut -d, -f1-2

clean:
	rm -f $(TARGET).elf $(TARGET).hex

.PHONY: all flash fuses vdd clean
