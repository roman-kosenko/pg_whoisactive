CC = gcc

CFLAGS = -std=c17 -O2 -Wall -Wextra -pedantic

# Use ncursesw instead of ncurses to enable wide-character (UTF-8) support
PKG_CFLAGS := $(shell pkg-config --cflags libpq ncursesw)
PKG_LIBS   := $(shell pkg-config --libs libpq ncursesw)

INCLUDES = -Iinclude $(PKG_CFLAGS)

SRC = \
	src/main.c \
	src/plain.c \
	src/ui.c \
	src/procfs.c \
	src/postgres.c \
	src/metrics.c

OBJ = $(SRC:.c=.o)

TARGET = pg_whoisactive

# Package metadata variables for Debian package building
PKG_NAME = pg-whoisactive
PKG_VER  = 1.2.1
PKG_ARCH = amd64
PKG_DIR  = $(PKG_NAME)_$(PKG_VER)_$(PKG_ARCH)

# Default target: build the main binary
all: $(TARGET)

# Link object files into the final executable binary
$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $@ $(PKG_LIBS)

# Compile C source files into object files
%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Build a native Debian (.deb) package for easy apt installation
deb: $(TARGET)
	@echo "Creating Debian package structure..."
	rm -rf $(PKG_DIR)
	mkdir -p $(PKG_DIR)/DEBIAN
	mkdir -p $(PKG_DIR)/usr/bin
	
	@echo "Copying binary executable..."
	cp $(TARGET) $(PKG_DIR)/usr/bin/
	
	@echo "Generating control file..."
	@echo "Package: $(PKG_NAME)" > $(PKG_DIR)/DEBIAN/control
	@echo "Version: $(PKG_VER)" >> $(PKG_DIR)/DEBIAN/control
	@echo "Section: utils" >> $(PKG_DIR)/DEBIAN/control
	@echo "Priority: optional" >> $(PKG_DIR)/DEBIAN/control
	@echo "Architecture: $(PKG_ARCH)" >> $(PKG_DIR)/DEBIAN/control
	@echo "Depends: libpq5, libncursesw6, libtinfo6" >> $(PKG_DIR)/DEBIAN/control
	@echo "Maintainer: Development Team <info@pg_whoisactive.org>" >> $(PKG_DIR)/DEBIAN/control
	@echo "Description: Advanced PostgreSQL session monitor with real-time process metrics." >> $(PKG_DIR)/DEBIAN/control
	
	@echo "Building the package using dpkg-deb..."
	dpkg-deb --build $(PKG_DIR)
	
	@echo "Cleaning up temporary directories..."
	rm -rf $(PKG_DIR)
	@echo "Debian package successfully created: $(PKG_DIR).deb"

# Clean up built artifacts, object files, and packages
clean:
	rm -f $(OBJ) $(TARGET) *.deb
	rm -rf $(PKG_NAME)_*

.PHONY: all clean deb
