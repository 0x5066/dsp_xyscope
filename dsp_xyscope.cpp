#include <windows.h>
#include "wacup/dsp.h"
#include "wacup/wa_ipc.h"
#include <vector>
#include <stdio.h>
#include <string>
#include <cmath>
#include <iostream>

const UINT_PTR TIMER_ID = 1;
const UINT TIMER_INTERVAL_MS = 16;

HWND hwnd_winamp = FindWindow("Winamp v1.x", NULL);
HWND hMainWnd = NULL;
HWND parent = NULL;
bool isMono;
bool viss;

static int prevSampleRate = 0;
static int current_srate = 44100;
static int prevNch = 0;
static int current_nch = 1;

static int win_x = 0;
static int win_y = 0;
static int win_h = 550;
static int win_w = 550;

// module getter.
winampDSPModule *getModule(int which);

// Function prototypes for X-Y Oscilloscope
void config_xyoscope(struct winampDSPModule *this_mod);
int init_xyoscope(struct winampDSPModule *this_mod);
void quit_xyoscope(struct winampDSPModule *this_mod);
int modify_samples_xyoscope(struct winampDSPModule *this_mod, short int *samples, int numsamples, int bps, int nch, int srate);
static void dsp_getIniFile(wchar_t* ini_file);
void dsp_configRead();
void dsp_configWrite();

// Module header, includes version, description, and address of the module retriever function
typedef struct {
	int version;
	char *description;
	winampDSPModule* (*getModule)(int);
	int(*sf)(int);
} winampDSPHeaderEx;

int sf(int v)
{
	int res;
	res = v * (unsigned long) 1103515245;
	res += (unsigned long) 13293;
	res &= (unsigned long) 0x7FFFFFFF;
	res ^= v;
	return res;
}

winampDSPHeaderEx hdr = { DSP_HDRVER, "X-Y Oscilloscope", getModule, sf };

// X-Y Oscilloscope module
winampDSPModule mod = {
    "X-Y Oscilloscope",
    NULL, // hwndParent
    NULL, // hDllInstance
    config_xyoscope,
    init_xyoscope,
    modify_samples_xyoscope,
    quit_xyoscope
};

// this is the only exported symbol. returns our main header.
extern "C" {
	__declspec(dllexport) winampDSPHeaderEx *winampDSPGetHeader2()
	{
		return &hdr;
	}
}

// getmodule routine from the main header. Returns NULL if an invalid module was requested,
// otherwise returns either mod1 or mod2 depending on 'which'.
winampDSPModule *getModule(int which)
{
	switch (which)
	{
		case 0:
			return &mod;
		default:
			return NULL;
	}
}

void InvalidateHWND(HWND hwnd1, HWND hwnd) {
    RECT hwnd1Rect;
    GetWindowRect(hwnd1, &hwnd1Rect); // Get screen coordinates of hMainBox

    // Convert screen coordinates to client coordinates of the parent window
    POINT topLeft = { hwnd1Rect.left, hwnd1Rect.top };
    POINT bottomRight = { hwnd1Rect.right, hwnd1Rect.bottom };
    ScreenToClient(hwnd, &topLeft);
    ScreenToClient(hwnd, &bottomRight);

    // Create a RECT structure from the client coordinates
    RECT clientRect = { topLeft.x, topLeft.y, bottomRight.x, bottomRight.y };

    // Invalidate the region
    InvalidateRect(hwnd, &clientRect, FALSE);
}

int bufferSize = 2048;
static std::vector<std::pair<short, short>> xyBuffer(bufferSize);
static size_t xyWritePos = 0; // Write position in the buffer

BITMAPINFO bmi = {0};
void* bits;

// Function to draw the X-Y Oscilloscope with optimized alpha blending and custom color
void DrawXYSoscope(HWND hwnd, const std::vector<std::pair<short, short>>& buffer, size_t bufferSize) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rect;
    GetClientRect(hwnd, &rect);
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;

    // Determine the size of the square area for the visualization
    int squareSize = std::min(width, height);
    int offsetX = (width - squareSize) / 2;  // Horizontal offset to center
    int offsetY = (height - squareSize) / 2; // Vertical offset to center

    //OutputDebugStringA(std::to_string(squareSize).c_str());

    // Static variables for backbuffer and X-Y drawing context
    static HBITMAP memBitmap = NULL;
    static HDC memDC = NULL;
    static HBITMAP xyBitmap = NULL;
    static HDC xyDC = NULL;
    static int prevWidth = 0, prevHeight = 0;

    // Create or resize backbuffer and X-Y drawing context if dimensions change
    if (!memBitmap || width != prevWidth || height != prevHeight) {
        if (memBitmap) {
            DeleteObject(memBitmap);
            DeleteDC(memDC);
        }
        if (xyBitmap) {
            DeleteObject(xyBitmap);
            DeleteDC(xyDC);
        }

        memDC = CreateCompatibleDC(hdc);
        memBitmap = CreateCompatibleBitmap(hdc, width, height);
        SelectObject(memDC, memBitmap);

        xyDC = CreateCompatibleDC(hdc);
        xyBitmap = CreateCompatibleBitmap(hdc, width, height);
        SelectObject(xyDC, xyBitmap);

        // Initialize backbuffer and X-Y drawing context with black
        HBRUSH blackBrush = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(memDC, &rect, blackBrush);
        FillRect(xyDC, &rect, blackBrush);
        DeleteObject(blackBrush);

        prevWidth = width;
        prevHeight = height;
    }

    // Create a semi-transparent black layer
    HDC tempDC = CreateCompatibleDC(hdc);
    HBITMAP tempBitmap = CreateCompatibleBitmap(hdc, width, height);
    SelectObject(tempDC, tempBitmap);

    HBRUSH semiTransparentBlackBrush = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(tempDC, &rect, semiTransparentBlackBrush);
    DeleteObject(semiTransparentBlackBrush);

    BLENDFUNCTION blendFunc = { AC_SRC_OVER, 0, 32, 0 }; // Alpha = 128 (50% transparency)
    AlphaBlend(xyDC, 0, 0, width, height, tempDC, 0, 0, width, height, blendFunc);

    DeleteObject(tempBitmap);
    DeleteDC(tempDC);
    int x, y;
    float brightnessScale = 1.0f * ((current_srate / 44100.0f) * 4.0f);
    float brightnessScale2 = std::max(
        static_cast<float>(
            brightnessScale * (((float)squareSize / 488.0f) / 8.0f) + 
            ( 4.0f + std::max(static_cast<float>(log10(brightnessScale / ((float)squareSize / 488.0f)) * 96.0f) - 90.0f, 0.0f) )
        ), 
        2.0f // Explicitly cast to float
    );
    //OutputDebugStringA(std::to_string( brightnessScale2 ).c_str());

    for (size_t i = 0; i < bufferSize; ++i) {
        if (current_nch == 2) {
            x = offsetX + (buffer[i].first + 32768) * squareSize / 65536;
            y = offsetY + squareSize - (buffer[i].second + 32768) * squareSize / 65536;
        } else {
            x = offsetX + (buffer[i].first + 32768) * squareSize / 65536;
            y = offsetY + squareSize - (buffer[i].first + 32768) * squareSize / 65536;
        }

        // Get the existing pixel color from xyDC
        COLORREF existingColor = GetPixel(xyDC, x, y);
        float existingR = GetRValue(existingColor) / 255.0f;
        float existingG = GetGValue(existingColor) / 255.0f;
        float existingB = GetBValue(existingColor) / 255.0f;

        // Define the new pixel color as floats
        float newR = (124.0f / brightnessScale2) / 255.0f;
        float newG = (252.0f / brightnessScale2) / 255.0f;
        float newB = (3.0f / brightnessScale2) / 255.0f;

        //OutputDebugStringA(std::to_string(brightnessScale2).c_str());

        // Additive blending: Add the new color to the existing color
        float blendedR = std::min(existingR + newR, 1.0f); // Clamp to 1.0
        float blendedG = std::min(existingG + newG, 1.0f); // Clamp to 1.0
        float blendedB = std::min(existingB + newB, 1.0f); // Clamp to 1.0

        // Convert blended floats back to BYTE
        BYTE finalR = static_cast<BYTE>(blendedR * 255);
        BYTE finalG = static_cast<BYTE>(blendedG * 255);
        BYTE finalB = static_cast<BYTE>(blendedB * 255);

        //OutputDebugStringA(std::to_string(brightnessScale).c_str());

        // Set the blended pixel color on xyDC
        SetPixel(xyDC, x, y, RGB(finalR, finalG, finalB));
    }

    // Copy xyDC to memDC
    BitBlt(memDC, 0, 0, width, height, xyDC, 0, 0, SRCCOPY);

    // Copy backbuffer to screen
    BitBlt(hdc, 0, 0, width, height, memDC, 0, 0, SRCCOPY);

    // Cleanup
    EndPaint(hwnd, &ps);
}

// Function to draw the mono oscilloscope
void DrawMonoOscilloscope(HWND hwnd, const short int* samples, int numsamples) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rect;
    GetClientRect(hwnd, &rect);
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;

    // Create backbuffer
    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBitmap = CreateCompatibleBitmap(hdc, width, height);
    HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, memBitmap);

    // Clear background
    HBRUSH blackBrush = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(memDC, &rect, blackBrush);
    DeleteObject(blackBrush);

    int midY = height / 2;
    int prevY = midY;
    int prevX = 0;

    HPEN darkgreenPen = CreatePen(PS_SOLID, 1, RGB(40, 64, 40)); // #284028
    HPEN olderPen = (HPEN)SelectObject(memDC, darkgreenPen);

    MoveToEx(memDC, 0, midY, NULL); 
    LineTo(memDC, width, midY); // Draw center line

    HPEN greenPen = CreatePen(PS_SOLID, 1, RGB(160, 255, 160)); // #a0ffa0 windows media player puke green
    HPEN oldPen = (HPEN)SelectObject(memDC, greenPen);

    // Draw the mono oscilloscope
    for (int i = 0; i < numsamples; i++) {
        int x = (i * width) / numsamples;
        int y = midY - ((samples[i] * height) / 65536);
        
        // Draw line from previous point
        if (i == 0) {
            MoveToEx(memDC, x, y, NULL);
        }
        LineTo(memDC, x, y);
    }

    // Cleanup drawing objects
    SelectObject(memDC, oldPen);
    DeleteObject(greenPen);
    SelectObject(memDC, olderPen);
    DeleteObject(darkgreenPen);

    // Copy backbuffer to screen
    BitBlt(hdc, 0, 0, width, height, memDC, 0, 0, SRCCOPY);

    // Cleanup
    SelectObject(memDC, oldBitmap);
    DeleteObject(memBitmap);
    DeleteDC(memDC);
    EndPaint(hwnd, &ps);
}

// Function to mix stereo samples down to mono
void MixStereoToMono(const short int* stereoSamples, int numsamples, int nch, short int* monoSamples) {
    for (int i = 0; i < numsamples; ++i) {
        monoSamples[i] = (stereoSamples[i * nch] + stereoSamples[i * nch + 1]) / 2;
    }
}

// Custom window procedure for the X-Y Oscilloscope window
LRESULT CALLBACK XYSoscopeWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_PAINT:
        if (viss) {
            short int* monoBuffer = (short int*)GetProp(hwnd, "MonoBuffer");
            int monoSize = (int)(intptr_t)GetProp(hwnd, "MonoBufferSize");
            if (monoBuffer && monoSize > 0) {
                DrawMonoOscilloscope(hwnd, monoBuffer, monoSize);
            }
        } else {
            DrawXYSoscope(hwnd, xyBuffer, xyBuffer.size());
        }
        break;

    case WM_LBUTTONDOWN:
        viss = !viss;
        return 0;

    case WM_CLOSE:
        //ShowWindow(hwnd, SW_HIDE); // Hide the window instead of destroying it
        return 0;

    //case WM_DESTROY: PostQuitMessage(0); return 0;

/*     case WM_GETMINMAXINFO: {
        LPMINMAXINFO lpMMI = (LPMINMAXINFO)lParam;
        lpMMI->ptMinTrackSize.x = 300; // Minimum width
        lpMMI->ptMinTrackSize.y = 300; // Minimum height
        return 0;
    } */
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

// Configuration function for X-Y Oscilloscope
void config_xyoscope(struct winampDSPModule *this_mod) {
    ShowWindow(hMainWnd, SW_SHOW); // Show the window if it exists
    SetForegroundWindow(hMainWnd); // Bring it to the foreground
}

embedWindowState myWindowState;

char szAppName[] = "XYSoscopeWindowClass"; // Our window class, etc

// Initialization function for X-Y Oscilloscope
int init_xyoscope(struct winampDSPModule *this_mod) {
    xyBuffer.assign(bufferSize, {0, 0}); // Clear the buffer
    xyWritePos = 0;
    int styles;
    HWND (*e)(embedWindowState *v);

    dsp_configRead();

    myWindowState.flags = EMBED_FLAGS_NOWINDOWMENU | EMBED_FLAGS_SCALEABLE_WND;
    myWindowState.r.left   = win_x;
    myWindowState.r.top    = win_y;
    myWindowState.r.right  = win_x + win_w;   // right = left + width
    myWindowState.r.bottom = win_y + win_h;   // bottom = top + height

    *(void**)&e = (void *)SendMessage(this_mod->hwndParent,WM_WA_IPC,(LPARAM)0,IPC_GET_EMBEDIF);

    if (!e)
    {
		MessageBox(this_mod->hwndParent,"This plugin requires Winamp 5.0+","blah",MB_OK);
		return 1;
    }

    parent = e(&myWindowState);

    SetWindowText(myWindowState.me, this_mod->description); // set our window title

	{	// Register our window class
		WNDCLASS wc;
		memset(&wc,0,sizeof(wc));
		wc.lpfnWndProc = XYSoscopeWndProc;				// our window procedure
		wc.hInstance = this_mod->hDllInstance;	// hInstance of DLL
		wc.lpszClassName = szAppName;			// our window class name
	
		if (!RegisterClass(&wc)) 
		{
			MessageBox(this_mod->hwndParent,"Error registering window class","blah",MB_OK);
			return 1;
		}
	}

    styles = WS_VISIBLE|WS_CHILDWINDOW|WS_OVERLAPPED|WS_CLIPCHILDREN|WS_CLIPSIBLINGS;
        // Create the window
        hMainWnd = CreateWindowEx(
            0,	// these exstyles put a nice small frame, 
            // but also a button in the taskbar
            szAppName,		    // our window class name
            NULL,				// no title, we're a child
            styles,	            // styles, we are a child	
            0, 0, myWindowState.r.right - myWindowState.r.left, myWindowState.r.bottom - myWindowState.r.top, // Position and size
            parent,             // Parent window
            NULL,               // Menu
            this_mod->hDllInstance, // Instance handle
            0 // Additional application data
        );
#ifdef _WIN64
        SetWindowLongPtrW(hMainWnd, GWLP_USERDATA, (LONG_PTR)this_mod); // set our user data to a "this" pointer
#else
        SetWindowLong(hMainWnd, GWL_USERDATA, (LONG)this_mod); // set our user data to a "this" pointer
#endif
        //SendMessage(this_mod->hwndParent, WM_WA_IPC, (int)hMainWnd, IPC_SETVISWND);

        if (hMainWnd) {
            ShowWindow(parent,SW_SHOWNORMAL);
        }
    return 0; // Success
}

// Cleanup function for X-Y Oscilloscope
void quit_xyoscope(struct winampDSPModule *this_mod) {
    //SendMessage(this_mod->hwndParent, WM_WA_IPC, 0, IPC_SETVISWND);
    dsp_configWrite();
    if (myWindowState.me) 
    {
        SetForegroundWindow(this_mod->hwndParent);
        DestroyWindow(myWindowState.me);
    }
    UnregisterClass(szAppName,this_mod->hDllInstance); // unregister window class
}

// Modify samples for X-Y Oscilloscope
int modify_samples_xyoscope(struct winampDSPModule *this_mod, short int *samples, int numsamples, int bps, int nch, int srate) {
    // Dynamically adjust bufferSize based on sample rate

    if (srate != prevSampleRate) {
        prevSampleRate = srate;
        current_srate = srate;
        int durationMs = 31; // Desired buffer duration in milliseconds
        bufferSize = (srate * durationMs) / 1000; // Calculate buffer size
        xyBuffer.resize(bufferSize); // Resize the buffer
        xyWritePos = 0; // Reset the write position
    }

    if (nch != prevNch) {
        prevNch = nch;
        current_nch = nch;
    }

    if (viss) {
        // Ensure monoBuffer is resized properly
        static std::vector<short int> monoBuffer;
        if (monoBuffer.size() != (size_t)numsamples) {
            monoBuffer.resize(numsamples);
        }

        // Mix stereo to mono if necessary
        if (nch == 2) {
            MixStereoToMono(samples, numsamples, nch, monoBuffer.data());
        } else {
            std::copy(samples, samples + numsamples, monoBuffer.begin());
        }

        // Store mono samples and update window properties
        if (hMainWnd && IsWindow(hMainWnd)) {
            SetProp(hMainWnd, "MonoBuffer", (HANDLE)monoBuffer.data());
            SetProp(hMainWnd, "MonoBufferSize", (HANDLE)numsamples);
        }
    } else {
        // Handle stereo as before
        if (hMainWnd && IsWindow(hMainWnd)) {
            SetProp(hMainWnd, "IsMono", (HANDLE)false);
            for (int i = 0; i < numsamples; ++i) {
                if (xyWritePos >= xyBuffer.size()) {
                    // Prevent out-of-bounds access
                    xyWritePos = 0;
                }
                xyBuffer[xyWritePos] = {samples[i * nch], samples[i * nch + 1]};
                xyWritePos = (xyWritePos + 1) % xyBuffer.size();
            }
        }
    }

    if (hMainWnd && IsWindow(hMainWnd)) {
        InvalidateHWND(hMainWnd, hMainWnd);
    }

    // Debug logging
    //printf("modify_samples_xyoscope: numsamples=%d, bps=%d, nch=%d, srate=%d, isMono=%d\n", numsamples, bps, nch, srate, isMono);

    return numsamples;
}

static void dsp_getIniFile(wchar_t* ini_file) {
    // Get the Winamp plugin directory
    wchar_t *plugdir=(wchar_t*)SendMessage(hwnd_winamp,WM_WA_IPC,0,IPC_GETINIDIRECTORYW);

    // Concatenate the plugin directory with the desired INI file name
    wcscpy(ini_file, plugdir);
    wcscat(ini_file, L"\\Plugins\\dsp_xyscope.ini");
}

void dsp_configRead() {
    wchar_t ini_file[MAX_PATH];
    dsp_getIniFile(ini_file);

    win_x = GetPrivateProfileIntW(L"xyscope", L"Screen_x", win_x, ini_file);
    win_y = GetPrivateProfileIntW(L"xyscope", L"Screen_y", win_y, ini_file);
    win_w = GetPrivateProfileIntW(L"xyscope", L"Screen_w", win_x, ini_file);
    win_h = GetPrivateProfileIntW(L"xyscope", L"Screen_h", win_y, ini_file);
}

void dsp_configWrite() {
    if (!parent) return;

    RECT r;
    GetWindowRect(parent, &r);

    // convert absolute screen coords → relative to parent’s client area
    HWND parentParent = GetParent(parent);

    POINT pt = { r.left, r.top };
    ScreenToClient(parentParent, &pt);
    win_x = pt.x;
    win_y = pt.y;

    // update size
    win_w = r.right - r.left;
    win_h = r.bottom - r.top;

    wchar_t string[32];
    wchar_t ini_file[MAX_PATH];
    dsp_getIniFile(ini_file);

    wsprintfW(string, L"%d", win_x);
    WritePrivateProfileStringW(L"xyscope", L"Screen_x", string, ini_file);
    wsprintfW(string, L"%d", win_y);
    WritePrivateProfileStringW(L"xyscope", L"Screen_y", string, ini_file);
    wsprintfW(string, L"%d", win_w);
    WritePrivateProfileStringW(L"xyscope", L"Screen_w", string, ini_file);
    wsprintfW(string, L"%d", win_h);
    WritePrivateProfileStringW(L"xyscope", L"Screen_h", string, ini_file);
}
