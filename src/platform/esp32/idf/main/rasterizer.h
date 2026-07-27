// Forwarder: render.cpp (configure-time copy of gba/render.iwram.cpp) does
// #include "rasterizer.h" which resolves to this directory — forward to the
// shared ESP32 MODE13 rasterizer (same one the Windows sim executes).
#include "../../rasterizer.h"
