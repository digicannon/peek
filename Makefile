#   Copyright (C) 2019  Noah Greenberg
#
#   This file is part of Peek.
#
#   Peek is free software: you can redistribute it and/or modify
#   it under the terms of the GNU General Public License as published by
#   the Free Software Foundation, either version 3 of the License, or
#   (at your option) any later version.
#
#   Peek is distributed in the hope that it will be useful,
#   but WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#   GNU General Public License for more details.
#
#   You should have received a copy of the GNU General Public License
#   along with this program.  If not, see <https://www.gnu.org/licenses/>.

SRC = peek.c wcwidth.c
OBJ = $(SRC:.c=.o)
EXEC ?= pk

CFLAGS ?= -Wall -DDEBUG=1 -g -pipe
CFLAGS_RELEASE ?= -Wall -DDEBUG=0 -g0 -O2 -march=native -flto -pipe

$(EXEC): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

.PHONY: clean release install

clean:
	rm -f $(OBJ) $(EXEC)

release: clean
	$(MAKE) $(EXEC) CFLAGS="$(CFLAGS_RELEASE)"

install: release
	sudo cp $(EXEC) /usr/local/bin/
