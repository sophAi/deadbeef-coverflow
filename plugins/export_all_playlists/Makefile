CC ?= gcc
CFLAGS ?= -g -O2 -Wall -fPIC
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS := $(shell pkg-config --libs gtk+-3.0)

INCLUDES = -I../../include -I../.. -I.
LIBS = $(GTK_LIBS) -lm -ldl

TARGET = export_all_playlists.so
OBJS = export_all_playlists.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -shared -o $@ $^ $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) $(GTK_CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
