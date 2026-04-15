TARGET   = ventway
BUILD    = build
CC       = arm-none-eabi-gcc
OBJCOPY  = arm-none-eabi-objcopy
SIZE     = arm-none-eabi-size

CFLAGS   = -mcpu=cortex-m4 -mthumb -mfloat-abi=soft \
           -std=c99 -Wall -Wextra -Os -g \
           -fno-common -ffunction-sections -fdata-sections \
           -ffreestanding -nostdlib

LDFLAGS  = -T ventway/linker.ld -nostdlib -Wl,--gc-sections

SRCS     = ventway/startup.c ventway/ventway.c ventway/main.c
OBJS     = $(addprefix $(BUILD)/,$(notdir $(SRCS:.c=.o)))

# Host compiler flags (tests, plot, sim)
HOST_CC      = cc
HOST_CFLAGS  = -std=c99 -Wall -Wextra -g -Iventway -Ilung_model

.PHONY: all clean renode test test-ventway test-lung test-integration sim plot

all: $(BUILD)/$(TARGET).bin
	$(SIZE) $(BUILD)/$(TARGET).elf

$(BUILD)/$(TARGET).elf: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ -lgcc

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/%.o: ventway/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)

test: test-ventway test-lung test-integration

test-ventway: $(BUILD)/test_ventway
	./$(BUILD)/test_ventway

$(BUILD)/test_ventway: tests/test_ventway.c ventway/ventway.c ventway/ventway.h | $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ tests/test_ventway.c ventway/ventway.c

test-lung: $(BUILD)/lung_test
	./$(BUILD)/lung_test

$(BUILD)/lung_test: tests/lung_test.c lung_model/lung_model.c lung_model/lung_model.h | $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ tests/lung_test.c lung_model/lung_model.c

test-integration: $(BUILD)/test_integration
	./$(BUILD)/test_integration

$(BUILD)/test_integration: tests/test_integration.c ventway/ventway.c ventway/ventway.h lung_model/lung_model.c lung_model/lung_model.h | $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ tests/test_integration.c ventway/ventway.c lung_model/lung_model.c

# Shared library for Renode lung model peripheral
sim: $(BUILD)/lung_model.so

$(BUILD)/lung_model.so: lung_model/lung_model.c lung_model/lung_model.h | $(BUILD)
	$(HOST_CC) -std=c99 -Wall -Wextra -O2 -shared -fPIC -Ilung_model -o $@ lung_model/lung_model.c

plot: $(BUILD)/lung_plot
	./$(BUILD)/lung_plot > $(BUILD)/lung_data.csv
	python3 plot/plot_lung.py $(BUILD)/lung_data.csv

$(BUILD)/lung_plot: plot/lung_plot.c lung_model/lung_model.c lung_model/lung_model.h ventway/ventway.c ventway/ventway.h | $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ plot/lung_plot.c lung_model/lung_model.c ventway/ventway.c

renode: $(BUILD)/$(TARGET).bin $(BUILD)/lung_model.so
	renode sim/ventway.resc
