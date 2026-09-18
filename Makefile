# Makefile for clean_metadata

CC = gcc
CFLAGS = -O2 -Wall -Wextra
LDFLAGS = -pthread
TARGET = clean_metadata
INSTALL_DIR = $(HOME)/bin

# macOS GUI app bundle (double-clickable, installs into ~/Applications).
# The bundle carries its own copy of the CLI binary in Contents/Resources
# and runs it via `--scan` to drive the approval list; see gui/README or
# the main README's GUI section for details.
GUI_DIR = gui
GUI_BUILD_DIR = $(GUI_DIR)/.build/release
GUI_EXECUTABLE = CleanMetadataGUI
APP_NAME = Clean Metadata
APP_BUNDLE = $(APP_NAME).app
APP_CONTENTS = $(APP_BUNDLE)/Contents
APPLICATIONS_DIR = $(HOME)/Applications

.PHONY: all clean install gui gui-app install-gui-app clean-gui

all: $(TARGET)

$(TARGET): clean_metadata.c
	$(CC) $(CFLAGS) $(LDFLAGS) $< -o $@

install: $(TARGET)
	@mkdir -p $(INSTALL_DIR)
	install -m 755 $(TARGET) $(INSTALL_DIR)/$(TARGET)
	@echo "Installed $(TARGET) to $(INSTALL_DIR)"

# Build the SwiftUI GUI executable via Swift Package Manager.
gui:
	cd $(GUI_DIR) && swift build -c release

# Assemble the .app bundle: the GUI executable goes in Contents/MacOS, the
# compiled clean_metadata CLI (which the GUI shells out to for scanning)
# goes in Contents/Resources alongside it.
gui-app: $(TARGET) gui
	@rm -rf "$(APP_BUNDLE)"
	@mkdir -p "$(APP_CONTENTS)/MacOS" "$(APP_CONTENTS)/Resources"
	@cp "$(GUI_BUILD_DIR)/$(GUI_EXECUTABLE)" "$(APP_CONTENTS)/MacOS/$(GUI_EXECUTABLE)"
	@cp "$(TARGET)" "$(APP_CONTENTS)/Resources/$(TARGET)"
	@cp "$(GUI_DIR)/Info.plist" "$(APP_CONTENTS)/Info.plist"
	@echo "Built \"$(APP_BUNDLE)\""

install-gui-app: gui-app
	@mkdir -p "$(APPLICATIONS_DIR)"
	@rm -rf "$(APPLICATIONS_DIR)/$(APP_BUNDLE)"
	@cp -R "$(APP_BUNDLE)" "$(APPLICATIONS_DIR)/"
	@echo "Installed to $(APPLICATIONS_DIR)/$(APP_BUNDLE)"

clean-gui:
	cd $(GUI_DIR) && swift package clean
	rm -rf "$(APP_BUNDLE)"

clean:
	rm -f $(TARGET)
