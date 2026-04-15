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

# Renode (override with: make RENODE=/path/to/renode stats)
RENODE       = ~/renode_portable/renode
RENODE_TEST  = ~/renode_portable/renode-test

# Absolute paths for Renode (it doesn't resolve relative paths)
LUNG_PY      = $(CURDIR)/build/lung_peripheral.py
LUNG_SO      = $(CURDIR)/build/lung_model.so

.PHONY: all clean renode test test-ventway test-lung test-integration sim stats

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

test: test-ventway test-lung

test-ventway: $(BUILD)/test_ventway
	./$(BUILD)/test_ventway

$(BUILD)/test_ventway: tests/test_ventway.c ventway/ventway.c ventway/ventway.h | $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ tests/test_ventway.c ventway/ventway.c

test-lung: $(BUILD)/lung_test
	./$(BUILD)/lung_test

$(BUILD)/lung_test: tests/lung_test.c lung_model/lung_model.c lung_model/lung_model.h | $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ tests/lung_test.c lung_model/lung_model.c

test-integration: $(BUILD)/$(TARGET).elf $(BUILD)/lung_model.so $(BUILD)/ventway.repl
	LD_LIBRARY_PATH=$(HOME)/renode_portable:$$LD_LIBRARY_PATH $(RENODE_TEST) tests/test_integration.robot

# Shared library for Renode lung model peripheral
sim: $(BUILD)/lung_model.so

$(BUILD)/lung_model.so: lung_model/lung_model.c lung_model/lung_model.h | $(BUILD)
	$(HOST_CC) -std=c99 -Wall -Wextra -O2 -shared -fPIC -Ilung_model -o $@ lung_model/lung_model.c

# Generate lung_peripheral.py with resolved .so path
$(BUILD)/lung_peripheral.py: sim/lung_peripheral.py $(BUILD)/lung_model.so | $(BUILD)
	@sed 's|__LUNG_SO__|$(LUNG_SO)|' $< > $@

# Generate .repl with resolved absolute path to lung_peripheral.py
$(BUILD)/ventway.repl: $(BUILD)/lung_peripheral.py | $(BUILD)
	@printf '// Auto-generated -- do not edit\n\npsens: Python.PythonPeripheral @ sysbus 0x50000000\n    size: 0x100\n    initable: true\n    filename: "%s"\n' "$(LUNG_PY)" > $@

stats: $(BUILD)/$(TARGET).elf $(BUILD)/lung_model.so $(BUILD)/lung_baseline $(BUILD)/ventway.repl
	@echo "=== Lung baseline (no ventilator) ==="
	./$(BUILD)/lung_baseline
	@echo ""
	@echo "=== With ventilator (Renode) ==="
	LD_LIBRARY_PATH=$(HOME)/renode_portable:$$LD_LIBRARY_PATH $(RENODE) --disable-xwt --console -e '$$uart_log = "$(CURDIR)/build/uart_log.txt"; include @sim/ventway_stats.resc'
	python3 sim/parse_stats.py $(BUILD)/uart_log.txt

$(BUILD)/lung_baseline: sim/lung_baseline.c lung_model/lung_model.c lung_model/lung_model.h | $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ sim/lung_baseline.c lung_model/lung_model.c

renode: $(BUILD)/$(TARGET).bin $(BUILD)/lung_model.so $(BUILD)/ventway.repl
	LD_LIBRARY_PATH=$(HOME)/renode_portable:$$LD_LIBRARY_PATH $(RENODE) sim/ventway.resc

