CC=clang
PROGS=spscq
CFLAGS=-Wall -Werror -g
CFLAGS+=-O2
LDFLAGS=-lpthread

all: $(PROGS)

spscq: spscq.o mlib.o

clean:
	-rm -rf *.o $(PROGS)

format:
	clang-format -i -style=file $(shell git ls-files *.c *.h)
