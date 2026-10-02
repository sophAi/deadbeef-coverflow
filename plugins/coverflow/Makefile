CC ?= gcc
CFLAGS ?= -O2 -Wall -fPIC
LDFLAGS ?= -shared

GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 epoxy)
GTK_LIBS   := $(shell pkg-config --libs gtk+-3.0 epoxy) -lm -ldl

INCLUDES := -I../../include -I../.. -I.

SRCS := coverflow.c gl_coverflow.c album_manager.c
OBJS := $(SRCS:.c=.o)
TARGET := coverflow_gtk3.so

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(GTK_LIBS)

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) $(GTK_CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
