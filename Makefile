SRC = $(wildcard *.c)
OBJ = $(SRC:.c=.o)
EXEC ?= pk

CFLAGS ?= -Wall -g

$(EXEC): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^

.PHONY: install clean

install: $(EXEC)
	sudo cp $(EXEC) /usr/local/bin/

clean:
	rm -f $(OBJ) $(EXEC)
