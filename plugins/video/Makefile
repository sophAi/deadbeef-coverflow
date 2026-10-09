CC ?= gcc
CFLAGS ?= -O2 -Wall -fPIC
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS := $(shell pkg-config --libs gtk+-3.0)

INCLUDES = -I../../include -I../.. -I. -I./include
LIBS = $(GTK_LIBS) -l:libmpv.so.2 -lm -ldl -lX11

TARGET = video_gtk3.so
OBJS = video.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -shared -o $@ $^ $(LIBS)

video.o: video.c
	$(CC) $(CFLAGS) $(INCLUDES) $(GTK_CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
