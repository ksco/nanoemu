CFLAGS=-O2 -Wall -Werror -pedantic-errors
SRCS=$(wildcard src/*.c)
OBJS=$(SRCS:.c=.o)

nanoemu: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJS): src/nanoemu.h

run: nanoemu
	./nanoemu xv6/xv6-kernel.bin xv6/xv6-fs.img

clean:
	rm -f nanoemu src/*.o

.PHONY: clean