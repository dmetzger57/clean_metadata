# Makefile for clean_metadata

CC = gcc
CFLAGS = -O2 -Wall -Wextra
LDFLAGS = -pthread
TARGET = clean_metadata
INSTALL_DIR = $(HOME)/bin

.PHONY: all clean install

all: $(TARGET)

$(TARGET): clean_metadata.c
	$(CC) $(CFLAGS) $(LDFLAGS) $< -o $@

install: $(TARGET)
	@mkdir -p $(INSTALL_DIR)
	install -m 755 $(TARGET) $(INSTALL_DIR)/$(TARGET)
	@echo "Installed $(TARGET) to $(INSTALL_DIR)"

clean:
	rm -f $(TARGET)
