# OpenLara ESP32-S3 Windows simulator (__ESP32_WIN__)
# MODE13 320x240, 8bpp paletted fb, PKD levels, pure-C++ rasterizer (../rasterizer.h)
#
# HARD REQUIREMENT: 32-bit (i686) g++. The fixed engine stores pointers in
# uint32 (fmt/pkd.h offset fixups, rasterizer color smuggling) — 64-bit builds
# fail to compile and would corrupt at runtime. ESP32-S3 is 32-bit, so i686 is
# also the faithful reference.  Install once:  pacman -S mingw-w64-i686-gcc
# plain assignment: make's builtin CXX default (g++) would win over ?=
CXX = C:/msys64/mingw32/bin/g++.exe

# refuse to build 64-bit — catches accidental use of a x86_64 g++
ifneq (,$(findstring x86_64,$(shell $(CXX) -dumpmachine)))
$(error $(CXX) targets x86_64; the fixed engine needs 32-bit pointers — use an i686 g++)
endif

FIXED = ../../../fixed
GBA   = ../../gba

# -U__WIN32__ is MANDATORY: MinGW g++ predefines __WIN32__, which would select
# the desktop GL1/PHD platform branch in common.h instead of __ESP32_WIN__.
DEFS = -D__ESP32_WIN__ -U__WIN32__ -D_DEBUG

CXXFLAGS = -std=gnu++11 -O2 -g -fno-exceptions -fno-rtti \
           -Wall -Wno-narrowing -Wno-unused-value -Wno-unused-variable \
           -I. -I$(FIXED) $(DEFS)

# fully static: no MSYS2 runtime DLLs (libwinpthread etc.) — exe runs anywhere
LDFLAGS = -static
LIBS    = -lgdi32 -lwinmm -luser32   # StretchDIBits / waveOut* / CreateWindow

TARGET = OpenLara_esp32sim.exe
OBJS   = main.o sound.o render.o common.o

all: $(TARGET) data

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS) $(LIBS)

main.o: main.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# reuse the GBA mixer verbatim — pure-C path when USE_ASM is not defined
sound.o: $(GBA)/sound.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# the generic renderer MUST be compiled from THIS directory so that its
# quoted #include "rasterizer.h" picks up our local forwarder to the
# shared MODE13 rasterizer (same copy trick as dos/deploy.bat and tns/Makefile)
render.cpp: $(GBA)/render.iwram.cpp
	cp $(GBA)/render.iwram.cpp render.cpp

render.o: render.cpp rasterizer.h ../rasterizer.h
	$(CXX) $(CXXFLAGS) -c render.cpp -o $@

# engine globals — always a separate TU
common.o: $(FIXED)/common.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# runtime data files next to the exe (PKD levels + music from the GBA port)
data:
	mkdir -p data
	cp $(GBA)/data/TITLE.PKD $(GBA)/data/GYM.PKD $(GBA)/data/LEVEL1.PKD $(GBA)/data/LEVEL2.PKD $(GBA)/data/TRACKS.AD4 data/

clean:
	rm -f *.o render.cpp $(TARGET)

.PHONY: all clean data
