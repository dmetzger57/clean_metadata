# Makefile for clean_metadata

CC = gcc
CFLAGS = -O2 -Wall -Wextra
LDFLAGS = -pthread
TARGET = clean_metadata
INSTALL_DIR = $(HOME)/bin

# macOS .app bundle (double-clickable, installs into ~/Applications)
APP_NAME = Clean Metadata
APP_BUNDLE = $(APP_NAME).app
APP_CONTENTS = $(APP_BUNDLE)/Contents
APPLICATIONS_DIR = $(HOME)/Applications

.PHONY: all clean install app install-app

all: $(TARGET)

$(TARGET): clean_metadata.c
	$(CC) $(CFLAGS) $(LDFLAGS) $< -o $@

install: $(TARGET)
	@mkdir -p $(INSTALL_DIR)
	install -m 755 $(TARGET) $(INSTALL_DIR)/$(TARGET)
	@echo "Installed $(TARGET) to $(INSTALL_DIR)"

# Assemble the .app bundle: the compiled CLI binary lives in Resources,
# and Contents/MacOS holds a small launcher (see macos/launcher.sh) that
# prompts for a folder and runs the CLI inside Terminal.app.
app: $(TARGET)
	@rm -rf "$(APP_BUNDLE)"
	@mkdir -p "$(APP_CONTENTS)/MacOS" "$(APP_CONTENTS)/Resources"
	@cp "$(TARGET)" "$(APP_CONTENTS)/Resources/$(TARGET)"
	@cp macos/launcher.sh "$(APP_CONTENTS)/MacOS/$(APP_NAME)"
	@chmod +x "$(APP_CONTENTS)/MacOS/$(APP_NAME)"
	@cp macos/Info.plist "$(APP_CONTENTS)/Info.plist"
	@echo "Built \"$(APP_BUNDLE)\""

install-app: app
	@mkdir -p "$(APPLICATIONS_DIR)"
	@rm -rf "$(APPLICATIONS_DIR)/$(APP_BUNDLE)"
	@cp -R "$(APP_BUNDLE)" "$(APPLICATIONS_DIR)/"
	@echo "Installed to $(APPLICATIONS_DIR)/$(APP_BUNDLE)"

clean:
	rm -f $(TARGET)
	rm -rf "$(APP_BUNDLE)"
