# Makefile do motor de regex.
# Uso: make, make test, make clean. CC, CFLAGS e LDFLAGS podem ser trocados:
#   make test CFLAGS="-O1 -g -fsanitize=address,undefined" LDFLAGS="-fsanitize=address,undefined"

CFLAGS  ?= -O2 -g
LDFLAGS ?=
WARN    := -std=c11 -Wall -Wextra -Wpedantic
ALL_CFLAGS = $(WARN) -Iinclude -Isrc $(CFLAGS)

B := build
HEADERS := $(wildcard include/*.h src/*.h)
LIB := $(B)/librx.a
OBJS := $(B)/utf8.o $(B)/rx_parse.o $(B)/rx_compile.o $(B)/rx_vm.o

.PHONY: all test clean

all: $(LIB) $(B)/rgrep

$(B):
	mkdir -p $@

$(B)/%.o: src/%.c $(HEADERS) | $(B)
	$(CC) $(ALL_CFLAGS) -c $< -o $@

$(LIB): $(OBJS)
	$(AR) rcs $@ $^

$(B)/rgrep: $(B)/rgrep.o $(LIB)
	$(CC) $(ALL_CFLAGS) $^ $(LDFLAGS) -o $@

$(B)/test_rx: tests/test_rx.c tests/teste.h $(LIB)
	$(CC) $(ALL_CFLAGS) $< $(LIB) $(LDFLAGS) -o $@

$(B)/rx_teste: tests/rx_teste.c $(LIB)
	$(CC) $(ALL_CFLAGS) $< $(LIB) $(LDFLAGS) -o $@

test: all $(B)/test_rx $(B)/rx_teste
	$(B)/test_rx
	sh tests/rgrep.sh $(B)
	@if command -v python3 > /dev/null 2>&1; then \
		python3 tests/diferencial.py $(B)/rx_teste; \
	else \
		echo "python3 não encontrado: pulando o teste diferencial"; \
	fi

clean:
	rm -rf $(B)
