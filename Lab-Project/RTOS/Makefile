# AVIMS freeRTOS + core-logic build (POSIX simulator port)
#
#   make test    -> build & run the native core-logic tests (no FreeRTOS needed)
#   make sim     -> build the FreeRTOS POSIX simulation  (needs FreeRTOS-Kernel)
#   make run     -> build & run the simulation for a few seconds
#   make mm1 / mm2 -> run the simulation as M/M/1 or M/M/2
#
# Override the kernel location if it lives elsewhere:
#   make sim KERNEL=/path/to/FreeRTOS-Kernel

KERNEL   ?= ../FreeRTOS-Kernel
POSIX     = $(KERNEL)/portable/ThirdParty/GCC/Posix
CC        = gcc
CFLAGS    = -Wall -O2 -g
CHANNELS ?= 2

# ---- native core tests -----------------------------------------------------
.PHONY: test
test: avims_core.c avims_core_test.c conflict_table.h
	$(CC) $(CFLAGS) avims_core.c avims_core_test.c -o coretest
	./coretest

# ---- FreeRTOS simulation ---------------------------------------------------
KERNEL_SRC = \
	$(KERNEL)/tasks.c $(KERNEL)/queue.c $(KERNEL)/list.c \
	$(KERNEL)/timers.c $(KERNEL)/event_groups.c \
	$(KERNEL)/portable/MemMang/heap_4.c \
	$(POSIX)/port.c $(POSIX)/utils/wait_for_event.c

INCLUDES = -I. -I$(KERNEL)/include -I$(POSIX) -I$(POSIX)/utils

.PHONY: sim
sim: avims_freertos.c avims_core.c conflict_table.h FreeRTOSConfig.h
	$(CC) $(CFLAGS) -DAVIMS_CHANNELS=$(CHANNELS) $(INCLUDES) \
		avims_freertos.c avims_core.c $(KERNEL_SRC) \
		-o avims_sim -lpthread
	@echo "built ./avims_sim  (M/M/$(CHANNELS))"

.PHONY: run
run: sim
	timeout 3 ./avims_sim || true

.PHONY: mm1
mm1:
	$(MAKE) run CHANNELS=1

.PHONY: mm2
mm2:
	$(MAKE) run CHANNELS=2

.PHONY: clean
clean:
	rm -f coretest avims_sim
