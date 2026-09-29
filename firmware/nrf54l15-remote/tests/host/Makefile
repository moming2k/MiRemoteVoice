SRC_DIR := ../../src
CFLAGS ?= -std=c11 -Wall -Wextra -Wno-unused-parameter -Werror -O1 -g -fsanitize=address,undefined
CFLAGS += -D_DEFAULT_SOURCE -I$(SRC_DIR)
LDLIBS := -lm

SRCS := test_main.c $(SRC_DIR)/adpcm.c $(SRC_DIR)/atvv_proto.c $(SRC_DIR)/remote_sm.c $(SRC_DIR)/battery_level.c

.PHONY: test clean
test: test_runner
	./test_runner

test_runner: $(SRCS) $(wildcard $(SRC_DIR)/*.h)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDLIBS)

clean:
	rm -f test_runner
