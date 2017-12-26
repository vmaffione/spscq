CC=clang
OBJS=spscq.o
PROGS=spscq
CFLAGS=-O2 -Wall -Werror

all: $(PROGS)

spscq: spscq.o

clean:
	-rm -rf $(OBJS)
