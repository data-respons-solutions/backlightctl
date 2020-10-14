CC ?= gcc
INSTALL_PATH ?= /usr/bin
OBJS = backlightctl.o log.o
SRC_VERSION := $(shell git describe --dirty --always)

CFLAGS += -std=gnu11 -Wall -Wextra -Werror -pedantic
CFLAGS += -DSRC_VERSION=$(SRC_VERSION)

all: backlightctl
.PHONY : all

backlightctl : $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

.c.o:
	$(CC) $(CFLAGS) -c $< -o $@

install:
	install -m 0755 -D backlightctl $(INSTALL_PATH)/

clean:
	rm -f *.o
	rm -f backlightctl