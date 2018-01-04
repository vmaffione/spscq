CC=clang
CXX=clang++
PROGS=spscq
CFLAGS=-Wall -Werror -g
CXXFLAGS=$(CFLAGS) -std=c++11
CFLAGS+=-O2
LDFLAGS=-lpthread -std=c++11 #-lstdc++

all: $(PROGS)

spscq: spscq.o mlib.o

clean:
	-rm -rf *.o $(PROGS)

format:
	clang-format -i -style=file $(shell git ls-files *.c *.h)
