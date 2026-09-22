CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Wpedantic -Wshadow
LDLIBS  += -lpthread

SRC = src/ring.c src/chan.c src/ha.c

all: mqha-demo

mqha-demo: $(SRC) src/demo.c src/*.h
	$(CC) $(CFLAGS) -o $@ $(SRC) src/demo.c $(LDLIBS)

test_ring: src/ring.c tests/test_ring.c src/ring.h
	$(CC) $(CFLAGS) -o $@ src/ring.c tests/test_ring.c $(LDLIBS)

# ring unit/stress tests, then the failover scenario 5 times
test: test_ring mqha-demo
	./test_ring
	timeout 60 ./mqha-demo 20000 --no-kill
	for i in 1 2 3 4 5; do timeout 60 ./mqha-demo 50000 || exit 1; done

asan: CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
asan: clean test

tsan: CFLAGS += -fsanitize=thread
tsan: clean test_ring
	./test_ring

clean:
	rm -f mqha-demo test_ring

.PHONY: all test asan tsan clean
