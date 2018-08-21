CC=g++
#CC=clang++
PROGS=spscq fan
CFLAGS=-Wall -Werror -g
CXXFLAGS=$(CFLAGS) -std=c++11
CFLAGS+=-O2
LDFLAGS=-lpthread -std=c++11 -g

all: $(PROGS)

spscq: spscq.o mlib.o
mlib.o: mlib.h
spscq.o: mlib.h spscq.h

fan: fan.o
	gcc -o fan fan.o -lpthread -Wall -Werror -O2 -g

fan.o: fan.c mlib.h spscq.h
	gcc -c fan.c -Wall -Werror -O2 -g

clean:
	-rm -rf *.o $(PROGS)

format:
	clang-format -i -style=file $(shell git ls-files *.c *.h *.cpp *.hpp)
