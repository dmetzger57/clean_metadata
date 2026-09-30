# Makefile for clean_metadata

CC = gcc
CFLAGS = -O2 -Wall -Wextra
LDFLAGS = -pthread
TARGET = clean_metadata
INSTALL_DIR = $(HOME)/bin

# Interactive terminal front-end (the CLI equivalent of the GUI). Like the
# GUI, it runs the clean_metadata scanner via `--scan`, so install-cli
# installs both binaries side by side.
CLI_TARGET = clean_metadata_cli
CLI_SRC = cli/clean_metadata_cli.c

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
APPLICATIONS_DIR = /Applications
ICON_SRC = $(GUI_DIR)/AppIcon.png
ICONSET = $(GUI_DIR)/.build/AppIcon.iconset
ICNS = $(GUI_DIR)/.build/AppIcon.icns

.PHONY: all clean install cli install-cli gui gui-app install-gui clean-gui

all: $(TARGET)

$(TARGET): clean_metadata.c
	$(CC) $(CFLAGS) $(LDFLAGS) $< -o $@

install: $(TARGET)
	@mkdir -p $(INSTALL_DIR)
	install -m 755 $(TARGET) $(INSTALL_DIR)/$(TARGET)
	@echo "Installed $(TARGET) to $(INSTALL_DIR)"

cli: $(CLI_TARGET)

$(CLI_TARGET): $(CLI_SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) $< -o $@

install-cli: $(TARGET) $(CLI_TARGET)
	@mkdir -p $(INSTALL_DIR)
	install -m 755 $(TARGET) $(INSTALL_DIR)/$(TARGET)
	install -m 755 $(CLI_TARGET) $(INSTALL_DIR)/$(CLI_TARGET)
	@echo "Installed $(TARGET) and $(CLI_TARGET) to $(INSTALL_DIR)"

# Build the SwiftUI GUI executable via Swift Package Manager.
# With only the Command Line Tools installed (no Xcode), SwiftPM always adds
# XCTest search paths under CommandLineTools/Developer, which doesn't exist,
# so ld warns "search path ... not found". Those warnings are harmless;
# filter just them out while preserving swift build's exit status.
gui:
	@set -o pipefail; cd $(GUI_DIR) && swift build -c release 2>&1 \
		| grep -v "ld: warning: search path '.*/CommandLineTools/Developer/.*' not found"

# Convert the square PNG app icon into a multi-resolution .icns.
$(ICNS): $(ICON_SRC)
	@rm -rf "$(ICONSET)"
	@mkdir -p "$(ICONSET)"
	@for size in 16 32 128 256 512; do \
		sips -z $$size $$size "$<" --out "$(ICONSET)/icon_$${size}x$${size}.png" >/dev/null; \
		double=$$((size * 2)); \
		sips -z $$double $$double "$<" --out "$(ICONSET)/icon_$${size}x$${size}@2x.png" >/dev/null; \
	done
	@iconutil -c icns "$(ICONSET)" -o "$@"

# Assemble the .app bundle: the GUI executable goes in Contents/MacOS, the
# compiled clean_metadata CLI (which the GUI shells out to for scanning)
# goes in Contents/Resources alongside it, as does the app icon.
gui-app: $(TARGET) gui $(ICNS)
	@rm -rf "$(APP_BUNDLE)"
	@mkdir -p "$(APP_CONTENTS)/MacOS" "$(APP_CONTENTS)/Resources"
	@cp "$(GUI_BUILD_DIR)/$(GUI_EXECUTABLE)" "$(APP_CONTENTS)/MacOS/$(GUI_EXECUTABLE)"
	@cp "$(TARGET)" "$(APP_CONTENTS)/Resources/$(TARGET)"
	@cp "$(ICNS)" "$(APP_CONTENTS)/Resources/AppIcon.icns"
	@cp "$(GUI_DIR)/Info.plist" "$(APP_CONTENTS)/Info.plist"
	@echo "Built \"$(APP_BUNDLE)\""

install-gui: gui-app
	@rm -rf "$(APPLICATIONS_DIR)/$(APP_BUNDLE)"
	@cp -R "$(APP_BUNDLE)" "$(APPLICATIONS_DIR)/"
	@echo "Installed to $(APPLICATIONS_DIR)/$(APP_BUNDLE)"

clean-gui:
	cd $(GUI_DIR) && swift package clean
	rm -rf "$(APP_BUNDLE)"

clean:
	rm -f $(TARGET) $(CLI_TARGET)
