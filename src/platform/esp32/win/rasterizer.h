// Forwarder: render.cpp (copy of gba/render.iwram.cpp) does #include "rasterizer.h"
// which resolves to the directory of the including file — this file forwards to the
// shared ESP32 MODE13 rasterizer used by both the Windows sim and the real target.
#include "../rasterizer.h"
