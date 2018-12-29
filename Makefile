SRC = $(wildcard *.c)
OBJ = $(SRC:.c=.o)
EXEC ?= pk

$(EXEC): $(OBJ)
	$(CC) -o $@ $^

.PHONY: install clean

install: $(EXEC)
	sudo cp $(EXEC) /usr/local/bin/

clean:
	rm -f $(OBJ) $(EXEC)
