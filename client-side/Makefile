# Compiler variables
CC = gcc
CFLAGS = -g -std=c17 -D_POSIX_C_SOURCE=200809L -I. \
		 -Wall -Wextra \
		 -Wcast-align -Wconversion -Wfloat-equal -Wformat=2 \
		 -Wnull-dereference -Wshadow -Wsign-conversion -Wswitch-enum -Wundef -Wunreachable-code -Wunused 
#CFLAGS += -pthread

# flags que adicionei para fazer o debug final
CFLAGS += -fsanitize=address -fsanitize=undefined 
CFLAGS += -Wpedantic -Wstrict-prototypes -Wmissing-prototypes -Wold-style-definition
CFLAGS += -Wpointer-arith -Wwrite-strings -Wvla
CFLAGS += -Wdouble-promotion -Wformat-truncation=2 -Wformat-overflow=2
CFLAGS += -Wduplicated-cond -Wduplicated-branches -Wlogical-op
CFLAGS += -Walloca -Walloc-zero -Wfree-nonheap-object
CFLAGS += -Wstringop-overflow=4 -Wstringop-truncation
LDFLAGS = -lncurses

# Directory variables
OBJ_DIR = obj
BIN_DIR = bin
INCLUDE_DIR = include
CLIENT_DIR = src/client

# executable 
TARGET = Pacmanist

#client
CLIENT = client


#Client objects
OBJS_CLIENT = client_main.o debug.o api.o display.o

# Dependencies
display.o = display.h
board.o = board.h
parser.o = parser.h
api.o = api.h protocol.h

# Object files path
vpath %.o $(OBJ_DIR)
vpath %.c $(CLIENT_DIR) $(INCLUDE_DIR)

# Make targets
all: client

client: $(BIN_DIR)/$(CLIENT)

$(BIN_DIR)/$(CLIENT): $(OBJS_CLIENT) | folders
	$(CC) $(CFLAGS) $(addprefix $(OBJ_DIR)/,$(OBJS_CLIENT)) -o $@ $(LDFLAGS)

# dont include LDFLAGS in the end, to allow compilation on macos
%.o: %.c $($@) | folders
	$(CC) -I $(INCLUDE_DIR) $(CFLAGS) -o $(OBJ_DIR)/$@ -c $<

# Create folders
folders:
	mkdir -p $(OBJ_DIR)
	mkdir -p $(BIN_DIR)

# Clean object files and executable
clean:
	rm -f $(OBJ_DIR)/*.o
	rm -f $(BIN_DIR)/$(TARGET)
	rm -f $(BIN_DIR)/$(CLIENT)

# indentify targets that do not create files
.PHONY: all clean run folders
