CC ?= gcc
CFLAGS ?= -O2 -Wall -fPIC
INCLUDES = -I../../include -I../.. -I.
DEPS_CFLAGS = $(shell pkg-config --cflags gdk-pixbuf-2.0 dbus-1 x11)
DEPS_LIBS = $(shell pkg-config --libs gdk-pixbuf-2.0 dbus-1 x11) -lpthread

TARGET = notify.so
OBJS = notify.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -shared -o $@ $(OBJS) $(DEPS_LIBS)

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) $(DEPS_CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
