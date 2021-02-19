CFLAGS = -Wall -ggdb -I include/

all: ddr3

ddr3: src/ddr3.c
	gcc -O2 -o $@ $^ $(CFLAGS)

clean:
	rm -f ddr3
