CC=clang
OBJS=spscq.o
PROGS=spscq
CFLAGS=-O2 -Wall -Werror
LDFLAGS=-lpthread

all: $(PROGS)

spscq: spscq.o

clean:
	-rm -rf $(OBJS)

format:
	clang-format -i -style=file $(shell git ls-files *.c *.h)
