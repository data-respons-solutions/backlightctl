BUILD ?= build
CFLAGS += -Wall -Wextra -Werror -pedantic
CFLAGS += -std=gnu11 -O2
CFLAGS += -DSRC_VERSION=$(shell git describe --dirty --always)
CXXFLAGS += -Wall -Wextra -Werror -pedantic
CXXFLAGS += -std=gnu++17 -O2
CXXFLAGS += -DSRC_VERSION=$(shell git describe --dirty --always)

all: backlightctl
.PHONY : all

.PHONY: backlightctl
backlightctl: $(BUILD)/backlightctl

.PHONY: test
test: $(BUILD)/test-libbacklight $(BUILD)/test-ringbuf
	for test in $^; do \
		echo "Running: $${test}"; \
		if ! ./$${test}; then \
			exit 1; \
		fi \
	done

$(BUILD)/libbacklight.a: $(addprefix $(BUILD)/, ringbuf.o libbacklight.o)
	$(AR) rcs $@ $^

$(BUILD)/backlightctl: $(addprefix $(BUILD)/, backlightctl.o log.o) $(BUILD)/libbacklight.a
	$(CC) -o $@ $^ $(LDFLAGS) -liio
	
$(BUILD)/test-libbacklight: $(addprefix $(BUILD)/, test-libbacklight.o) $(BUILD)/libbacklight.a
	$(CXX) -o $@ $^ $(LDFLAGS) -lCatch2Main -lCatch2
	
$(BUILD)/test-ringbuf: $(addprefix $(BUILD)/, test-ringbuf.o ringbuf.o)
	$(CXX) -o $@ $^ $(LDFLAGS) -lCatch2Main -lCatch2

$(BUILD)/%.o: %.cpp 
	mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.o: %.c 
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
