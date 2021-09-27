BUILD ?= build
CFLAGS += -Wall -Wextra -Werror -pedantic
CFLAGS += -std=gnu11
CFLAGS += -DSRC_VERSION=$(shell git describe --dirty --always)
CXXFLAGS += -Wall -Wextra -Werror -pedantic
CXXFLAGS += -std=gnu++17
CXXFLAGS += -DSRC_VERSION=$(shell git describe --dirty --always)

all: backlightctl test
.PHONY : all

.PHONY: backlightctl
backlightctl: $(BUILD)/backlightctl

.PHONY: test
test: $(BUILD)/test-libbacklight

$(BUILD)/backlightctl: $(addprefix $(BUILD)/, backlightctl.o log.o)
	$(CC) -o $@ $^ $(LDFLAGS)
	
$(BUILD)/test-libbacklight: $(addprefix $(BUILD)/, test-libbacklight.o)
	$(CXX) -o $@ $^ $(LDFLAGS) -lCatch2Main -lCatch2

$(BUILD)/%.o: %.cpp 
	mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.o: %.c 
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
