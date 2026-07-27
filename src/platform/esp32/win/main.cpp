// OpenLara ESP32-S3 Windows simulator (__ESP32_WIN__)
// Byte-exact reference build for the ESP32-S3 port: MODE13 320x240, 8bpp
// paletted framebuffer, PKD levels. Modeled on the __GBA_WIN__ harness
// (platform/gba/main.cpp) with x64-safe window subclassing, guarded cheats
// and an fps counter.
#include "game.h"

int32 fps;
int32 frameIndex = 0;
int32 fpsCounter = 0;
uint32 curSoundBuffer = 0;

const void* TRACKS_AD4;
const void* TITLE_SCR;
uint8* levelData;

HWND hWnd;

#define WND_SCALE   3
#define WND_WIDTH   (FRAME_WIDTH * WND_SCALE)
#define WND_HEIGHT  (FRAME_HEIGHT * WND_SCALE)

uint16 MEM_PAL_BG[256];
uint32 SCREEN[FRAME_WIDTH * FRAME_HEIGHT];

// ---- RAM budget instrumentation -------------------------------------------
// The real ESP32-S3 has ~440KB of usable internal SRAM. Track what the level
// heap uses so overruns show up here, not after flashing.
#define ESP32_PSRAM_BUDGET (8 * 1024 * 1024)
static uint32 gHeapLevel, gHeapTracks, gHeapTitle;

static void ramReport()
{
    LOG("RAM: level %u KB + tracks %u KB + title %u KB = %u KB heap (PSRAM budget %u KB)\n",
        gHeapLevel >> 10, gHeapTracks >> 10, gHeapTitle >> 10,
        (gHeapLevel + gHeapTracks + gHeapTitle) >> 10, ESP32_PSRAM_BUDGET >> 10);
}
// ---------------------------------------------------------------------------

void osSetPalette(const uint16* palette)
{
    memcpy(MEM_PAL_BG, palette, 256 * 2);
}

int32 osGetSystemTimeMS()
{
    return GetTickCount();
}

bool osSaveSettings()
{
    FILE* f = fopen("settings.dat", "wb");
    if (!f) return false;
    fwrite(&gSettings, sizeof(gSettings), 1, f);
    fclose(f);
    return true;
}

bool osLoadSettings()
{
    FILE* f = fopen("settings.dat", "rb");
    if (!f) return false;
    uint8 version;
    fread(&version, 1, 1, f);
    if (version != gSettings.version) {
        fclose(f);
        return false;
    }
    fread((uint8*)&gSettings + 1, sizeof(gSettings) - 1, 1, f);
    fclose(f);
    return true;
}

bool osCheckSave()
{
    FILE* f = fopen("savegame.dat", "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

bool osSaveGame()
{
    FILE* f = fopen("savegame.dat", "wb");
    if (!f) return false;
    fwrite(&gSaveGame, sizeof(gSaveGame), 1, f);
    fwrite(&gSaveData, gSaveGame.dataSize, 1, f);
    fclose(f);
    return true;
}

bool osLoadGame()
{
    FILE* f = fopen("savegame.dat", "rb");
    if (!f) return false;

    uint32 version;
    fread(&version, sizeof(version), 1, f);

    if (SAVEGAME_VER != version)
    {
        fclose(f);
        return false;
    }

    fread(&gSaveGame.dataSize, sizeof(gSaveGame) - sizeof(version), 1, f);
    fread(&gSaveData, gSaveGame.dataSize, 1, f);
    fclose(f);
    return true;
}

void osJoyVibrate(int32 index, int32 L, int32 R) {}

// ---- sound: gba/sound.cpp mixer + waveOut double buffer -------------------
extern int8 soundBuffer[2 * SND_SAMPLES + 32]; // defined in gba/sound.cpp

HWAVEOUT waveOut;
WAVEFORMATEX waveFmt = { WAVE_FORMAT_PCM, 1, SND_OUTPUT_FREQ, SND_OUTPUT_FREQ, 1, 8, sizeof(waveFmt) };
WAVEHDR waveBuf[2];

void soundInit()
{
    sndInit();

    if (waveOutOpen(&waveOut, WAVE_MAPPER, &waveFmt, (INT_PTR)hWnd, 0, CALLBACK_WINDOW) != MMSYSERR_NOERROR)
        return;

    memset(&waveBuf, 0, sizeof(waveBuf));
    for (int i = 0; i < 2; i++)
    {
        WAVEHDR *waveHdr = waveBuf + i;
        waveHdr->dwBufferLength = SND_SAMPLES;
        waveHdr->lpData = (LPSTR)(soundBuffer + i * SND_SAMPLES);
        waveOutPrepareHeader(waveOut, waveHdr, sizeof(WAVEHDR));
        waveOutWrite(waveOut, waveHdr, sizeof(WAVEHDR));
    }
}

void soundFill()
{
    WAVEHDR *waveHdr = waveBuf + curSoundBuffer;
    waveOutUnprepareHeader(waveOut, waveHdr, sizeof(WAVEHDR));
    sndFill((int8*)waveHdr->lpData);
    waveOutPrepareHeader(waveOut, waveHdr, sizeof(WAVEHDR));
    waveOutWrite(waveOut, waveHdr, sizeof(WAVEHDR));
    curSoundBuffer ^= 1;
}
// ---------------------------------------------------------------------------

HDC hDC;

void blit()
{
    // fb is 8bpp indexed (first W*H bytes of the uint16 array) — expand
    // through the BGR555 palette. This models exactly what the ESP32-S3
    // will do per DMA chunk (palette -> RGB565 instead of RGB888).
    for (int i = 0; i < FRAME_WIDTH * FRAME_HEIGHT; i++)
    {
        uint16 c = MEM_PAL_BG[((uint8*)fb)[i]];
        SCREEN[i] = (((c << 3) & 0xFF) << 16) | ((((c >> 5) << 3) & 0xFF) << 8) | ((c >> 10 << 3) & 0xFF) | 0xFF000000;
    }
    const BITMAPINFO bmi = { { sizeof(BITMAPINFOHEADER), FRAME_WIDTH, -FRAME_HEIGHT, 1, 32, BI_RGB, 0, 0, 0, 0, 0 } };
    StretchDIBits(hDC, 0, 0, WND_WIDTH, WND_HEIGHT, 0, 0, FRAME_WIDTH, FRAME_HEIGHT, SCREEN, &bmi, DIB_RGB_COLORS, SRCCOPY);
}

LRESULT CALLBACK wndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        case WM_ACTIVATE:
        {
            keys = 0;
            break;
        }

        case WM_DESTROY :
        {
            PostQuitMessage(0);
            break;
        }

        case WM_KEYDOWN    :
        case WM_KEYUP      :
        case WM_SYSKEYUP   :
        case WM_SYSKEYDOWN :
        {
            InputKey key = IK_NONE;
            switch (wParam) {
                case VK_UP     : key = IK_UP;     break;
                case VK_RIGHT  : key = IK_RIGHT;  break;
                case VK_DOWN   : key = IK_DOWN;   break;
                case VK_LEFT   : key = IK_LEFT;   break;
                case 'A'       : key = IK_B;      break;
                case 'S'       : key = IK_A;      break;
                case 'Q'       : key = IK_L;      break;
                case 'W'       : key = IK_R;      break;
                case VK_RETURN : key = IK_START;  break;
                case VK_SPACE  : key = IK_SELECT; break;
            }

            if (players[0] && players[0]->extraL) // NULL on title screen
            {
                if (wParam == '1') players[0]->extraL->goalWeapon = WEAPON_PISTOLS;
                if (wParam == '2') players[0]->extraL->goalWeapon = WEAPON_MAGNUMS;
                if (wParam == '3') players[0]->extraL->goalWeapon = WEAPON_UZIS;
                if (wParam == '4') players[0]->extraL->goalWeapon = WEAPON_SHOTGUN;
            }

            if (msg != WM_KEYUP && msg != WM_SYSKEYUP) {
                keys |= key;
            } else {
                keys &= ~key;
            }
            break;
        }

        case MM_WOM_DONE :
        {
            soundFill();
            break;
        }

        default :
            return DefWindowProc(hWnd, msg, wParam, lParam);
    }
    return 0;
}

static uint8* loadFile(const char* path, uint32* outSize)
{
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    int32 size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8* data = new uint8[size];
    fread(data, 1, size, f);
    fclose(f);
    if (outSize) *outSize = size;
    return data;
}

const void* osLoadScreen(LevelID id)
{
    return TITLE_SCR;
}

const void* osLoadLevel(LevelID id)
{
    char buf[32];

    delete[] levelData;

    sprintf(buf, "data/%s.PKD", (const char*)gLevelInfo[id].data);
    levelData = loadFile(buf, &gHeapLevel);
    if (!levelData) {
        LOG("FATAL: missing %s\n", buf);
        return NULL;
    }

    if (!TRACKS_AD4)
    {
        TRACKS_AD4 = loadFile("data/TRACKS.AD4", &gHeapTracks);
        if (!TRACKS_AD4) {
            // fallback: zeroed track table -> every track has size 0 and is skipped
            LOG("WARN: data/TRACKS.AD4 missing, music disabled\n");
            TRACKS_AD4 = new uint8[512]();
            gHeapTracks = 512;
        }
    }

    if (!TITLE_SCR)
    {
        uint32 size = 0;
        uint8* scr = loadFile("data/TITLE.SCR", &size);
        // renderBackground copies exactly W*H bytes — reject wrong-resolution assets
        // (the GBA one is 240x160 = 38400 bytes)
        if (scr && size != FRAME_WIDTH * FRAME_HEIGHT) {
            LOG("WARN: data/TITLE.SCR is %u bytes, need %u — using black background\n",
                size, (uint32)(FRAME_WIDTH * FRAME_HEIGHT));
            delete[] scr;
            scr = NULL;
        }
        if (!scr) {
            scr = new uint8[FRAME_WIDTH * FRAME_HEIGHT]();
        }
        TITLE_SCR = scr;
        gHeapTitle = FRAME_WIDTH * FRAME_HEIGHT;
    }

    ramReport();

    return (void*)levelData;
}

int main(void)
{
    RECT r = { 0, 0, WND_WIDTH, WND_HEIGHT };

    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, false);
    int wx = (GetSystemMetrics(SM_CXSCREEN) - (r.right - r.left)) / 2;
    int wy = (GetSystemMetrics(SM_CYSCREEN) - (r.bottom - r.top)) / 2;

    hWnd = CreateWindow("static", "OpenLara ESP32-S3 sim", WS_OVERLAPPEDWINDOW, wx + r.left, wy + r.top, r.right - r.left, r.bottom - r.top, 0, 0, 0, 0);
    hDC = GetDC(hWnd);

    SetWindowLongPtr(hWnd, GWLP_WNDPROC, (LONG_PTR)&wndProc);
    ShowWindow(hWnd, SW_SHOWDEFAULT);

    soundInit();

    gameInit();

    MSG msg;

    int32 startTime = GetTickCount() - 33;
    int32 lastFrame = 0;
    int32 fpsTime = GetTickCount();

    do {
        if (PeekMessage(&msg, 0, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        } else {
        #ifdef _DEBUG
            Sleep(4);
        #else
            Sleep(1); // don't busy-spin a full core
        #endif
            int32 frame = (GetTickCount() - startTime) / 33;
            if (GetAsyncKeyState('R')) frame /= 10;   // slow-mo

            int32 count = frame - lastFrame;
            if (GetAsyncKeyState('T')) count *= 10;   // fast-forward
            gameUpdate(count);
            lastFrame = frame;

            gameRender();

            blit();

            fpsCounter++;
            int32 now = GetTickCount();
            if (now - fpsTime >= 1000) {
                fps = fpsCounter;
                fpsCounter = 0;
                fpsTime += 1000;
            }
        }
    } while (msg.message != WM_QUIT);

    return 0;
}
