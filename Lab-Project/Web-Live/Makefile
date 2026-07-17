# Build the interactive AVIMS FreeRTOS engine (POSIX simulator port).
#   make            -> build ./avims_live
#   make KERNEL=... -> point at a FreeRTOS-Kernel elsewhere
#
# Reuses the tested core logic from ../rtos and the FreeRTOS kernel at ../FreeRTOS-Kernel.

KERNEL ?= ../FreeRTOS-Kernel
POSIX   = $(KERNEL)/portable/ThirdParty/GCC/Posix
CC      = gcc
CFLAGS  = -Wall -O2 -g

INCLUDES = -I. -I../rtos -I$(KERNEL)/include -I$(POSIX) -I$(POSIX)/utils

KERNEL_SRC = \
	$(KERNEL)/tasks.c $(KERNEL)/queue.c $(KERNEL)/list.c \
	$(KERNEL)/timers.c $(KERNEL)/event_groups.c \
	$(KERNEL)/portable/MemMang/heap_4.c \
	$(POSIX)/port.c $(POSIX)/utils/wait_for_event.c

avims_live: avims_live.c ../rtos/avims_core.c ../rtos/conflict_table.h FreeRTOSConfig.h
	$(CC) $(CFLAGS) $(INCLUDES) avims_live.c ../rtos/avims_core.c $(KERNEL_SRC) \
		-o avims_live -lpthread
	@echo "built ./avims_live"

.PHONY: clean
clean:
	rm -f avims_live
