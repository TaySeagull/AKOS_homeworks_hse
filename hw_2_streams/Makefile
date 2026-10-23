# COMPILER
CC := gcc

# DIRECTORIES
BUILD_DIR := build
SRC_DIR   := src
INC_DIR   := incld

# CFLAGS
CFLAGS := -std=c11 -Wall -Wextra -Werror
CFLAGS += -I$(INC_DIR)

# SOURCES
C_SRCS := $(SRC_DIR)/main.c \
          $(SRC_DIR)/config.c \
          $(SRC_DIR)/event_queue.c \
          $(SRC_DIR)/utils.c \
          $(SRC_DIR)/simulation.c \
          $(SRC_DIR)/dispatch.c \
          $(SRC_DIR)/event_handlers.c \
          $(SRC_DIR)/output.c

# OBJECT FILES
C_OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(C_SRCS))

# TARGET
TARGET := emergency_dispatch

.PHONY: all clean test run dirs

all: dirs $(TARGET)

# CREATE BUILD DIRECTORY
dirs:
	@mkdir -p $(BUILD_DIR)

# LINKING
$(TARGET): $(C_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# COMPILING C FROM SRC/
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | dirs
	$(CC) $(CFLAGS) -c -o $@ $<

# TESTING THROUGH SH SCRIPT
test: $(TARGET)
	@chmod +x run_tests.sh
	@./run_tests.sh

# RUN PROGRAM
run: $(TARGET)
	./$(TARGET) -c config.txt -d 0

# CLEANING
clean:
	rm -rf $(BUILD_DIR)
	rm -f $(TARGET)
	rm -f dispatch.log test_dispatch.log