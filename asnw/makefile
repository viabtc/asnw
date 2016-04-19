SOURCE  := $(wildcard *.c)
OBJS    := $(patsubst %.c, %.o, $(SOURCE))
CC      := gcc
CFLAGS  := -Wall -Wno-strict-aliasing -Wno-uninitialized -g -rdynamic -std=gnu11

.PHONY : all clean install 

TARGET  := libasnw.a

all : $(TARGET)

clean :
	rm -rf *.d *.o $(TARGET)

$(TARGET) : $(OBJS)
	ar -rs $@ $^
.c.o :
	$(CC) $(CFLAGS) -c -o $@ $< $(INCS)

