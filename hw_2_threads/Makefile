# COMPILER
CC := gcc

# DIRECTORIES
BUILD_DIR    := build
BUILD_DIR_CV := build_cv
SRC_DIR      := src
INC_DIR      := incld

# CFLAGS
CFLAGS   := -std=c11 -Wall -Wextra -Werror -pthread
CFLAGS   += -I$(INC_DIR)
DEPFLAGS := -MMD -MP # generate header dependencies (build/*.d)

# SOURCES (common part of both programs)
C_SRCS := $(SRC_DIR)/main.c \
          $(SRC_DIR)/config.c \
          $(SRC_DIR)/event_queue.c \
          $(SRC_DIR)/utils.c \
          $(SRC_DIR)/simulation.c \
          $(SRC_DIR)/dispatch.c \
          $(SRC_DIR)/output.c \
          $(SRC_DIR)/generator.c \
          $(SRC_DIR)/threads.c \
          $(SRC_DIR)/threads/team_thread.c \
          $(SRC_DIR)/threads/dispatcher_thread.c \
          $(SRC_DIR)/threads/signal_thread.c \
          $(SRC_DIR)/threads/call_thread.c \

# main program: mutexes + semaphores
OBJS    := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(C_SRCS) $(SRC_DIR)/sync_sem.c)
# second program: mutexes + condition variables (additional requirement 8)
OBJS_CV := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR_CV)/%.o,$(C_SRCS) $(SRC_DIR)/sync_cv.c)

# TARGETS
TARGET    := emergency_dispatch
TARGET_CV := emergency_dispatch_cv

.PHONY: all clean test run

all: $(TARGET) $(TARGET_CV)

# LINKING
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

$(TARGET_CV): $(OBJS_CV)
	$(CC) $(CFLAGS) -o $@ $^

# COMPILING C FROM SRC/
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

$(BUILD_DIR_CV)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DUSE_CONDVAR $(DEPFLAGS) -c -o $@ $<

# TESTING THROUGH THE SCRIPT (both programs)
test: all
	@bash ./run_tests.sh ./$(TARGET)
	@bash ./run_tests.sh ./$(TARGET_CV)

# RUN PROGRAM
run: $(TARGET)
	./$(TARGET) -c config.txt

# CLEANING
clean:
	rm -rf $(BUILD_DIR) $(BUILD_DIR_CV)
	rm -f $(TARGET) $(TARGET_CV)
	rm -f dispatch.log
	rm -f tests/*.log

# header dependencies
-include $(OBJS:.o=.d) $(OBJS_CV:.o=.d)
