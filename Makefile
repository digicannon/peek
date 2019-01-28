SRC = $(wildcard *.c)
OBJ = $(SRC:.c=.o)
EXEC ?= pk

CFLAGS ?= -Wall -DDEBUG=1 -g -pipe
CFLAGS_RELEASE ?= -Wall -DDEBUG=0 -g0 -O2 -flto -march=native -fpie -Wl,-pie -pipe

$(EXEC): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^

.PHONY: clean release install

clean:
	rm -f $(OBJ) $(EXEC)

release: clean
	$(MAKE) $(EXEC) CFLAGS="$(CFLAGS_RELEASE)"

install: release
	sudo cp $(EXEC) /usr/local/bin/
