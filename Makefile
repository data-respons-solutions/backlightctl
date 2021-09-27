BUILD ?= build
CFLAGS += -Wall
CFLAGS += -Wextra
CFLAGS += -Werror
CFLAGS += -std=gnu11
CFLAGS += -pedantic
CFLAGS += -DSRC_VERSION=$(shell git describe --dirty --always)

all: backlightctl
.PHONY : all

.PHONY: backlightctl
backlightctl: $(BUILD)/backlightctl

$(BUILD)/backlightctl: $(addprefix $(BUILD)/, backlightctl.o log.o)
	$(CC) -o $@ $^ $(LDFLAGS)

$(BUILD)/%.o: %.c 
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
