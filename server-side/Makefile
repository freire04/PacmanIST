# Compiler variables
CC = gcc
CFLAGS = -g -std=c17 -D_POSIX_C_SOURCE=200809L -I. \
		 -Wall -Wextra \
		 -Wcast-align -Wfloat-equal -Wformat=2 \
		 -Wnull-dereference -Wshadow -Wswitch-enum -Wundef -Wunreachable-code -Wunused 
#CFLAGS += -pthread
CFLAGS += -fsanitize=address -fsanitize=undefined 

# flags que adicionei para fazer o debug final
CFLAGS += -Wpedantic -Wstrict-prototypes -Wmissing-prototypes -Wold-style-definition
CFLAGS += -Wpointer-arith -Wwrite-strings -Wvla
CFLAGS += -Wdouble-promotion -Wformat-truncation=2 -Wformat-overflow=2
CFLAGS += -Wduplicated-cond -Wduplicated-branches -Wlogical-op
CFLAGS += -Walloca -Walloc-zero -Wfree-nonheap-object
CFLAGS += -Wstringop-overflow=4 -Wstringop-truncation
LDFLAGS = -lncurses

# Directory variables
SRC_DIR = src
OBJ_DIR = obj
BIN_DIR = bin
INCLUDE_DIR = include

# executable 
TARGET = Pacmanist

# Objects variables
OBJS = game.o display.o board.o parser.o

# Dependencies
display.o = display.h
board.o = board.h
parser.o = parser.h

# Object files path
vpath %.o $(OBJ_DIR)
vpath %.c $(SRC_DIR)

# Make targets
all: pacmanist

pacmanist: $(BIN_DIR)/$(TARGET)

$(BIN_DIR)/$(TARGET): $(OBJS) | folders
	$(CC) $(CFLAGS) $(SLEEP) $(addprefix $(OBJ_DIR)/,$(OBJS)) -o $@ $(LDFLAGS)

# dont include LDFLAGS in the end, to allow compilation on macos
%.o: %.c $($@) | folders
	$(CC) -I $(INCLUDE_DIR) $(CFLAGS) -o $(OBJ_DIR)/$@ -c $<

# run the program
run: pacmanist
	@./$(BIN_DIR)/$(TARGET) $(ARGS)  # to run use: make run ARGS="<folder>"

# Create folders
folders:
	mkdir -p $(OBJ_DIR)
	mkdir -p $(BIN_DIR)

# Clean object files and executable
clean:
	rm -f $(OBJ_DIR)/*.o
	rm -f $(BIN_DIR)/$(TARGET)

# indentify targets that do not create files
.PHONY: all clean run folders
