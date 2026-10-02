CC = gcc
CFLAGS = -Wall -Wextra -O2
TARGET = system_stats
INSTALL_PATH = /usr/local/bin

all:
	$(CC) $(CFLAGS) system_stats.c -o $(TARGET)

clean:
	rm -f $(TARGET)

install: all
	install -m 755 $(TARGET) $(INSTALL_PATH)/$(TARGET)

uninstall:
	rm -f $(INSTALL_PATH)/$(TARGET)