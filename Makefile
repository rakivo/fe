CC := cc
SRC_DIR := src
ROOT_FILE := $(SRC_DIR)/main.c
SRC_FILES := $(filter-out $(ROOT_FILE), $(wildcard $(SRC_DIR)/*))
CFLAGS := -Wall -Wextra -Wpedantic -g -O2
CLIBS := -lraylib

fe: $(ROOT_FILE) $(SRC_FILES)
	$(CC) -o $@ $(CFLAGS) $(CLIBS) $<
