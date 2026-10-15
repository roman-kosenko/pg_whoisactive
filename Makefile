CC = gcc

# Единый источник версии приложения
VERSION = 1.3.7

CFLAGS = -std=c17 -O2 -Wall -Wextra -pedantic -DVERSION=\"$(VERSION)\"

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
PKG_VER  = $(VERSION)
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
	@echo "Creating Debian package structure in /tmp..."
	rm -rf /tmp/$(PKG_DIR)
	mkdir -p /tmp/$(PKG_DIR)/DEBIAN
	mkdir -p /tmp/$(PKG_DIR)/usr/bin
	
	@echo "Fixing control directory permissions..."
	chmod 755 /tmp/$(PKG_DIR)/DEBIAN
	
	@echo "Copying binary executable..."
	cp $(TARGET) /tmp/$(PKG_DIR)/usr/bin/
	chmod 755 /tmp/$(PKG_DIR)/usr/bin/$(TARGET)
	ln -s $(TARGET) /tmp/$(PKG_DIR)/usr/bin/pgwa
	
	@echo "Generating control file..."
	@echo "Package: $(PKG_NAME)" > /tmp/$(PKG_DIR)/DEBIAN/control
	@echo "Version: $(PKG_VER)" >> /tmp/$(PKG_DIR)/DEBIAN/control
	@echo "Section: utils" >> /tmp/$(PKG_DIR)/DEBIAN/control
	@echo "Priority: optional" >> /tmp/$(PKG_DIR)/DEBIAN/control
	@echo "Architecture: $(PKG_ARCH)" >> /tmp/$(PKG_DIR)/DEBIAN/control
	@echo "Depends: libpq5, libncursesw6, libtinfo6" >> /tmp/$(PKG_DIR)/DEBIAN/control
	@echo "Maintainer: Development Team <info@pg_whoisactive.org>" >> /tmp/$(PKG_DIR)/DEBIAN/control
	@echo "Description: Advanced PostgreSQL session monitor with real-time process metrics." >> /tmp/$(PKG_DIR)/DEBIAN/control
	
	@echo "Building the package using dpkg-deb..."
	dpkg-deb --build /tmp/$(PKG_DIR)
	
	@echo "Moving the final package to the current directory..."
	mv /tmp/$(PKG_DIR).deb ./
	
	@echo "Cleaning up temporary directories..."
	rm -rf /tmp/$(PKG_DIR)
	@echo "Debian package successfully created: $(PKG_DIR).deb"

# Clean up built artifacts, object files, and packages
clean:
	rm -f $(OBJ) $(TARGET) *.deb
	rm -rf $(PKG_NAME)_*

.PHONY: all clean deb
