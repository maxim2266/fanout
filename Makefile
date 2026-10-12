BIN    := fanout
CFILES := $(BIN).c
CFLAGS := -s -Os -std=c11 -Wall -Wextra

.PHONY: all clean test static

# build
all: $(BIN)

$(BIN): $(CFILES) version
	$(CC) $(CFLAGS) -DVER=$(shell ./version) -o $@ $(CFILES)
	chmod 0711 $@

# build with static linking (useful for containers)
static: CFLAGS := -static $(CFLAGS)
static: $(BIN)

clean:
	$(RM) $(BIN)

test: $(BIN)
	./run-tests
