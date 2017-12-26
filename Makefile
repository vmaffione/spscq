CC=clang
OBJS=spscq.o
PROGS=spscq
CFLAGS=-Wall -Werror -g
CFLAGS+=-O2
LDFLAGS=-lpthread

all: $(PROGS)

spscq: spscq.o

clean:
	-rm -rf $(OBJS)

format:
	clang-format -i -style=file $(shell git ls-files *.c *.h)
