STANDALONE_TOOLCHAIN=~/ndk/bin/
target_host=$(STANDALONE_TOOLCHAIN)aarch64-linux-android
AR=$(target_host)-ar
AS=$(target_host)-clang
CC=$(target_host)-clang
CXX=$(target_host)-clang++
LD=$(target_host)-ld
STRIP=$target_host-strip

# Tell configure what flags Android requires.
CFLAGS=-pie -fPIC
LDFLAGS=-pie
SOURCES=poc98.c
OBJECTS=$(SOURCES:.cpp=.o)
EXECUTABLE=poc98

all: $(SOURCES) $(EXECUTABLE)
$(EXECUTABLE): $(OBJECTS) 
	$(CC) $(LDFLAGS) $(OBJECTS) -o $@
.c.o:
	$(CXX) $(CFLAGS) $< -o $@
.cpp.o:
	$(CXX) $(CFLAGS) $< -o $@

clean: 
	rm *.o poc98;
