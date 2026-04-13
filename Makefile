TARGET  = ventway
CC      = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
SIZE    = arm-none-eabi-size

CFLAGS  = -mcpu=cortex-m4 -mthumb -mfloat-abi=soft \
          -std=c99 -Wall -Wextra -Os -g \
          -fno-common -ffunction-sections -fdata-sections \
          -ffreestanding -nostdlib

LDFLAGS = -T linker.ld -nostdlib -Wl,--gc-sections

SRCS    = startup.c main.c
OBJS    = $(SRCS:.c=.o)

.PHONY: all clean flash renode

all: $(TARGET).bin
	$(SIZE) $(TARGET).elf

$(TARGET).elf: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET).elf $(TARGET).bin

renode:
	renode ventway.resc
