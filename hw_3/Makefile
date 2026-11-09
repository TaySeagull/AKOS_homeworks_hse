# Emergency Dispatch Simulator - homework 3 (processes), variant 35
#
#   bin/ed_fork_named     - 4-5 points: fork + named POSIX semaphores + POSIX shm
#   bin/ed_fork_unnamed   - 6 points:   fork + unnamed POSIX semaphores in POSIX shm
#   bin/ed_shm_*          - 7-8 points: independent programs + named semaphores + POSIX shm
#   bin/ed_mq_*           - 9-10 points: independent programs + named semaphores + POSIX message queues

CC := gcc

SRC_DIR   := src
INC_DIR   := incld
BUILD_DIR := build
BIN_DIR   := bin

CFLAGS   := -std=c11 -Wall -Wextra -Werror -I$(INC_DIR)
DEPFLAGS := -MMD -MP
LDLIBS   := -lrt -pthread   # shm_open / mq_open / sem_open

# simulation logic, the same for all programs
CORE := config.c dispatch.c event_queue.c generator.c output.c utils.c \
        simulation.c roles.c signals.c sem_common.c

# ---------- variants: name, defines, extra sources ----------
FORK_NAMED_SRC   := $(CORE) ipc_shm.c sem_named.c main_server.c
FORK_UNNAMED_SRC := $(CORE) ipc_shm.c sem_unnamed.c main_server.c
SHM_SRC          := $(CORE) ipc_shm.c sem_named.c
MQ_SRC           := $(CORE) ipc_mq.c sem_named.c

FORK_NAMED_DEF   := -DIPC_FORK -DSEM_NAMED
FORK_UNNAMED_DEF := -DIPC_FORK -DSEM_UNNAMED
SHM_DEF          := -DIPC_SHM_INDEPENDENT -DSEM_NAMED
MQ_DEF           := -DIPC_MQ -DSEM_NAMED

CLIENTS := dispatcher team callers
ROLE_dispatcher := ROLE_DISPATCHER
ROLE_team       := ROLE_TEAM
ROLE_callers    := ROLE_CALLERS

PROGRAMS := $(BIN_DIR)/ed_fork_named $(BIN_DIR)/ed_fork_unnamed \
            $(BIN_DIR)/ed_shm_clock $(addprefix $(BIN_DIR)/ed_shm_,$(CLIENTS)) \
            $(BIN_DIR)/ed_mq_clock $(addprefix $(BIN_DIR)/ed_mq_,$(CLIENTS))

.PHONY: all clean test run ipc-clean

all: $(PROGRAMS)

# object lists ($(1) = build subdir, $(2) = sources)
objs = $(patsubst %.c,$(BUILD_DIR)/$(1)/%.o,$(2))

$(BIN_DIR)/ed_fork_named: $(call objs,fork_named,$(FORK_NAMED_SRC))
	@mkdir -p $(BIN_DIR)
	$(CC) -o $@ $^ $(LDLIBS)

$(BIN_DIR)/ed_fork_unnamed: $(call objs,fork_unnamed,$(FORK_UNNAMED_SRC))
	@mkdir -p $(BIN_DIR)
	$(CC) -o $@ $^ $(LDLIBS)

$(BIN_DIR)/ed_shm_clock: $(call objs,shm,$(SHM_SRC) main_server.c)
	@mkdir -p $(BIN_DIR)
	$(CC) -o $@ $^ $(LDLIBS)

$(BIN_DIR)/ed_mq_clock: $(call objs,mq,$(MQ_SRC) main_server.c)
	@mkdir -p $(BIN_DIR)
	$(CC) -o $@ $^ $(LDLIBS)

# client programs: the same main_client.c compiled with another role
define CLIENT_RULES
$(BIN_DIR)/ed_shm_$(1): $(call objs,shm,$(SHM_SRC)) $(BUILD_DIR)/shm/client_$(1).o
	@mkdir -p $(BIN_DIR)
	$$(CC) -o $$@ $$^ $$(LDLIBS)

$(BIN_DIR)/ed_mq_$(1): $(call objs,mq,$(MQ_SRC)) $(BUILD_DIR)/mq/client_$(1).o
	@mkdir -p $(BIN_DIR)
	$$(CC) -o $$@ $$^ $$(LDLIBS)

$(BUILD_DIR)/shm/client_$(1).o: $(SRC_DIR)/main_client.c
	@mkdir -p $$(dir $$@)
	$$(CC) $$(CFLAGS) $$(SHM_DEF) -DCLIENT_ROLE=$(ROLE_$(1)) $$(DEPFLAGS) -c -o $$@ $$<

$(BUILD_DIR)/mq/client_$(1).o: $(SRC_DIR)/main_client.c
	@mkdir -p $$(dir $$@)
	$$(CC) $$(CFLAGS) $$(MQ_DEF) -DCLIENT_ROLE=$(ROLE_$(1)) $$(DEPFLAGS) -c -o $$@ $$<
endef
$(foreach c,$(CLIENTS),$(eval $(call CLIENT_RULES,$(c))))

# compiling: one build subdirectory per variant
$(BUILD_DIR)/fork_named/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(FORK_NAMED_DEF) $(DEPFLAGS) -c -o $@ $<

$(BUILD_DIR)/fork_unnamed/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(FORK_UNNAMED_DEF) $(DEPFLAGS) -c -o $@ $<

$(BUILD_DIR)/shm/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(SHM_DEF) $(DEPFLAGS) -c -o $@ $<

$(BUILD_DIR)/mq/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(MQ_DEF) $(DEPFLAGS) -c -o $@ $<

# TESTS (all programs)
test: all
	@bash ./run_tests.sh

# RUN the 4-5 points version with the main configuration
run: $(BIN_DIR)/ed_fork_named
	./$(BIN_DIR)/ed_fork_named -c config.txt

# removes IPC objects left by a run that was killed with SIGKILL
ipc-clean:
	@bash ./ipc_cleanup.sh

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)
	rm -f dispatch.log tests/*.log

-include $(shell find $(BUILD_DIR) -name '*.d' 2>/dev/null)
