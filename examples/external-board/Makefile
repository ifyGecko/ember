# An out-of-tree project using UAM as a library: one board file plus a main
# that registers it and reuses the stock runner. It needs only UAM's public
# headers and the output of `make -C $(UAM) lib`.
#
#   make UAM=/path/to/uam UAM_BUILD=/path/to/uam/build
#
# With an installed UAM (make install), use pkg-config instead:
#   UAM_CFLAGS := $(shell pkg-config --cflags uam)
#   UAM_LIBS   := $(shell pkg-config --libs uam)

UAM       ?= ../..
UAM_BUILD ?= $(UAM)/build
OUT       ?= build
CC        ?= cc

UAM_CFLAGS ?= -I$(UAM)/include
UAM_LIBS   ?= $(UAM_BUILD)/libuam.a

CFLAGS := -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -O2 -g $(UAM_CFLAGS)

$(OUT)/ext-run: ext_board.c main.c $(filter %.a,$(UAM_LIBS))
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ ext_board.c main.c $(UAM_LIBS)

.PHONY: clean
clean:
	rm -rf $(OUT)
