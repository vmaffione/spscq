CC=g++
#CC=clang++
PROGS=spscq fan vswitch cover
CFLAGS=-Wall -Werror -g
CXXFLAGS=$(CFLAGS) -std=c++11
CFLAGS+=-O2
LDFLAGS=-lpthread -std=c++11 -g

all: $(PROGS)

spscq: spscq.o mlib.o
mlib.o: mlib.h
spscq.o: mlib.h spscq.h
cover: cover.o

fan: fan.o mlib.o
	gcc -o fan fan.o mlib.o -lpthread -Wall -Werror -O2 -g

fan.o: fan.c mlib.h spscq.h
	gcc -c fan.c -Wall -Werror -O2 -g

vswitch: vswitch.o mlib.o
	gcc -o vswitch vswitch.o mlib.o -lpthread -Wall -Werror -O2 -g

vswitch.o: vswitch.c mlib.h spscq.h
	gcc -c vswitch.c -Wall -Werror -O2 -g

clean:
	-rm -rf *.o $(PROGS)

format:
	clang-format -i -style=file $(shell git ls-files *.c *.h *.cpp *.hpp)
