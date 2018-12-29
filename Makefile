SRC = $(wildcard *.c)
OBJ = $(SRC:.c=.o)
EXEC ?= pk

$(EXEC): $(OBJ)
	$(CC) -o $@ $^

.PHONY: clean
clean:
	rm -f $(OBJ) $(EXEC)
