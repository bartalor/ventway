TARGET   = ventway
BUILD    = build
CC       = arm-none-eabi-gcc
OBJCOPY  = arm-none-eabi-objcopy
SIZE     = arm-none-eabi-size

CFLAGS   = -mcpu=cortex-m4 -mthumb -mfloat-abi=soft \
           -std=c99 -Wall -Wextra -Os -g \
           -fno-common -ffunction-sections -fdata-sections \
           -ffreestanding -nostdlib

LDFLAGS  = -T linker.ld -nostdlib -Wl,--gc-sections

SRCS     = startup.c ventway.c main.c
OBJS     = $(addprefix $(BUILD)/,$(SRCS:.c=.o))

.PHONY: all clean renode test sim

all: $(BUILD)/$(TARGET).bin
	$(SIZE) $(BUILD)/$(TARGET).elf

$(BUILD)/$(TARGET).elf: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ -lgcc

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)

test: $(BUILD)/test_ventway
	./$(BUILD)/test_ventway

$(BUILD)/test_ventway: test_ventway.c ventway.c ventway.h lung_model.c lung_model.h | $(BUILD)
	cc -std=c99 -Wall -Wextra -g -o $@ test_ventway.c ventway.c lung_model.c

# Shared library for Renode lung model peripheral
sim: $(BUILD)/lung_model.so

$(BUILD)/lung_model.so: lung_model.c lung_model.h | $(BUILD)
	cc -std=c99 -Wall -Wextra -O2 -shared -fPIC -o $@ lung_model.c

plot: $(BUILD)/test_lung_plot
	./$(BUILD)/test_lung_plot > $(BUILD)/lung_data.csv
	python3 plot_lung.py $(BUILD)/lung_data.csv

$(BUILD)/test_lung_plot: test_lung_plot.c lung_model.c lung_model.h ventway.c ventway.h | $(BUILD)
	cc -std=c99 -Wall -Wextra -g -o $@ test_lung_plot.c lung_model.c ventway.c

renode: $(BUILD)/$(TARGET).bin $(BUILD)/lung_model.so
	renode ventway.resc
