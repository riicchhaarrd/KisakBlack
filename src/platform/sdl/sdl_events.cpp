// sdl_events.cpp — SDL → engine input translation.
#include "sdl_events.h"

#include <SDL2/SDL.h>
#include <ui/keycodes.h>      // keyNum_t (the engine's key codes)
#include <win32/win_local.h>  // sysEventType_t (SE_KEY / SE_CHAR / ...)

#if defined(__EMSCRIPTEN_PTHREADS__)
// The MT web build never creates an SDL window (the GL context is made directly via
// emscripten_webgl on the render worker — see glcontext_sdl.cpp), so SDL registers no
// DOM event handlers and SDL_PollEvent/SDL_GetMouseState stay empty. We also must NOT
// register DOM listeners from this (game-loop) worker — that proxies addEventListener
// synchronously to the browser thread, which is busy proxying the render worker's GL
// calls, and deadlocks. Instead JS listeners run on the DOM thread and write input into
// a shared int32 block in WASM memory; this worker only reads it. See the block below.
#include <emscripten/em_asm.h>   // MAIN_THREAD_ASYNC_EM_ASM (wire up the DOM input bridge)
#include <cstdint>
#endif

// Provided by the engine (src/win32/win_main.cpp). On x86-32 __cdecl is the default
// convention, so this declaration resolves to the same symbol.
void Sys_QueEvent(unsigned int time, sysEventType_t type, int value, int value2,
                  int ptrLength, void *ptr);

int Sys_SDLKeyToKeyNum(int sym) {
    // Printable ASCII maps straight through (Quake convention: letter/number/symbol
    // keys use their ASCII value). SDL reports unshifted lowercase here.
    if (sym >= 0x20 && sym < 0x7F) return sym;

    switch (sym) {
        case SDLK_RETURN: case SDLK_KP_ENTER: return K_ENTER;
        case SDLK_ESCAPE:    return K_ESCAPE;
        case SDLK_BACKSPACE: return K_BACKSPACE;
        case SDLK_TAB:       return K_TAB;
        case SDLK_UP:        return K_UPARROW;
        case SDLK_DOWN:      return K_DOWNARROW;
        case SDLK_LEFT:      return K_LEFTARROW;
        case SDLK_RIGHT:     return K_RIGHTARROW;
        case SDLK_LALT:   case SDLK_RALT:   return K_ALT;
        case SDLK_LCTRL:  case SDLK_RCTRL:  return K_CTRL;
        case SDLK_LSHIFT: case SDLK_RSHIFT: return K_SHIFT;
        case SDLK_INSERT:    return K_INS;
        case SDLK_DELETE:    return K_DEL;
        case SDLK_PAGEDOWN:  return K_PGDN;
        case SDLK_PAGEUP:    return K_PGUP;
        case SDLK_HOME:      return K_HOME;
        case SDLK_END:       return K_END;
        case SDLK_CAPSLOCK:  return K_CAPSLOCK;
        default: break;
    }
    if (sym >= SDLK_F1 && sym <= SDLK_F12) return K_F1 + (sym - SDLK_F1);
    return K_NONE;
}

#if defined(__EMSCRIPTEN_PTHREADS__)
// --- Web input via shared memory (MT build) --------------------------------
// JS listeners (KBInputSetup, src/platform/web/index.html) run on the DOM thread and
// write into this int32 block in WASM linear memory — a SharedArrayBuffer in the
// pthreads build. The game worker only READS it (drain the event ring + sample the
// mouse) each frame, so there is no synchronous proxying and no deadlock.
namespace {
enum {                       // word indices into g_kbi
    KBI_MX = 0, KBI_MY,      // mouse position, engine backbuffer px (JS writes)
    KBI_DX, KBI_DY,          // accumulated raw movement for pointer-lock look (JS adds)
    KBI_HEAD, KBI_TAIL,      // event ring: head = JS producer, tail = C consumer
    KBI_RING = 8,            // ring base (8-word header)
    KBI_RINGSZ = 256,        // power of two
    KBI_WORDS = KBI_RING + KBI_RINGSZ
};
enum { KIND_KEY = 0, KIND_CHAR = 1, KIND_MBTN = 2, KIND_WHEEL = 3 };  // ring entry kinds

// Fixed low static data — never relocated, so a JS Int32Array view over it stays valid
// across memory growth (the shared buffer is not detached on grow in pthreads builds).
volatile int32_t g_kbi[KBI_WORDS];
int  s_fbW = 1920, s_fbH = 1080;
bool s_inputInited = false;

// Legacy DOM KeyboardEvent.keyCode -> engine keyNum_t. The actual typed character for
// text fields arrives separately as KIND_CHAR (real char, shift applied), so this only
// needs to cover keys the bind/menu navigation reads.
int WebDomKeyToKeyNum(int kc) {
    if (kc >= 'A' && kc <= 'Z')  return kc - 'A' + 'a';     // letters -> lowercase ascii
    if (kc >= '0' && kc <= '9')  return kc;                 // top-row digits
    if (kc >= 96  && kc <= 105)  return kc - 96 + '0';      // numpad digits
    if (kc >= 112 && kc <= 123)  return K_F1 + (kc - 112);  // F1..F12
    switch (kc) {
        case 8:  return K_BACKSPACE;  case 9:  return K_TAB;     case 13: return K_ENTER;
        case 16: return K_SHIFT;      case 17: return K_CTRL;    case 18: return K_ALT;
        case 20: return K_CAPSLOCK;   case 27: return K_ESCAPE;  case 32: return K_SPACE;
        case 33: return K_PGUP;       case 34: return K_PGDN;    case 35: return K_END;
        case 36: return K_HOME;       case 37: return K_LEFTARROW; case 38: return K_UPARROW;
        case 39: return K_RIGHTARROW; case 40: return K_DOWNARROW; case 45: return K_INS;
        case 46: return K_DEL;
        case 186: return ';';  case 187: return '=';  case 188: return ',';  case 189: return '-';
        case 190: return '.';  case 191: return '/';  case 192: return '`';  case 219: return '[';
        case 220: return '\\'; case 221: return ']';  case 222: return '\'';
        default: return K_NONE;
    }
}
} // namespace

// Set the engine backbuffer size so JS scales mouse coords into that space (called by
// the GL backend at device creation).
extern "C" void WebInput_SetResolution(int w, int h) {
    if (w > 0) s_fbW = w;
    if (h > 0) s_fbH = h;
}

// Drain the JS-filled event ring into the engine's event queue (game worker).
static void WebInput_PumpKeys() {
    int head = __atomic_load_n(&g_kbi[KBI_HEAD], __ATOMIC_ACQUIRE);
    int tail = g_kbi[KBI_TAIL];
    for (; tail != head; ++tail) {
        int32_t v = g_kbi[KBI_RING + (tail & (KBI_RINGSZ - 1))];
        int kind = (v >> 24) & 0xFF, down = (v >> 16) & 0x1, val = v & 0xFFFF;
        switch (kind) {
            case KIND_KEY: { int k = WebDomKeyToKeyNum(val);
                             if (k != K_NONE) Sys_QueEvent(0, SE_KEY, k, down, 0, nullptr); break; }
            case KIND_CHAR:  Sys_QueEvent(0, SE_CHAR, val, 0, 0, nullptr); break;
            case KIND_MBTN: {
                // DOM MouseEvent.button: 0=left 1=MIDDLE 2=RIGHT; engine order is
                // MOUSE1=left MOUSE2=right MOUSE3=middle. Passing the DOM code through
                // sent right-click to K_MOUSE3 (default +frag) — ADS threw a grenade.
                int b = val == 1 ? 2 : val == 2 ? 1 : val;
                Sys_QueEvent(0, SE_KEY, K_MOUSE1 + b, down, 0, nullptr); break; }
            case KIND_WHEEL: { int k = val ? K_MWHEELUP : K_MWHEELDOWN;
                               Sys_QueEvent(0, SE_KEY, k, 1, 0, nullptr);   // press + release
                               Sys_QueEvent(0, SE_KEY, k, 0, 0, nullptr); break; }
        }
    }
    __atomic_store_n(&g_kbi[KBI_TAIL], tail, __ATOMIC_RELEASE);
}

// Current mouse position (engine px) + accumulated raw movement (read and cleared).
extern "C" void WebInput_GetMouse(int *x, int *y, int *dx, int *dy) {
    *x  = __atomic_load_n(&g_kbi[KBI_MX], __ATOMIC_RELAXED);
    *y  = __atomic_load_n(&g_kbi[KBI_MY], __ATOMIC_RELAXED);
    *dx = __atomic_exchange_n(&g_kbi[KBI_DX], 0, __ATOMIC_RELAXED);
    *dy = __atomic_exchange_n(&g_kbi[KBI_DY], 0, __ATOMIC_RELAXED);
}

// One-time: ask the DOM thread (asynchronously — this worker never blocks) to wire up
// the JS listeners that fill g_kbi. Safe to call every frame; runs once.
static void WebInputInit() {
    if (s_inputInited) return;
    s_inputInited = true;
    MAIN_THREAD_ASYNC_EM_ASM(
        { if (typeof KBInputSetup === 'function') KBInputSetup($0, $1, $2); },
        (int)(uintptr_t)g_kbi, s_fbW, s_fbH);
}
#endif // __EMSCRIPTEN_PTHREADS__

void Sys_PumpSDLEvents(unsigned t) {
#if defined(__EMSCRIPTEN_PTHREADS__)
    // Ensure the DOM-thread listeners are wired up (once), then drain whatever they've
    // written into the shared ring into the engine event queue. No proxying, no block.
    (void)t;
    WebInputInit();
    WebInput_PumpKeys();
    return;
#else
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_KEYDOWN:
            case SDL_KEYUP: {
                int k = Sys_SDLKeyToKeyNum((int)e.key.keysym.sym);
                if (k != K_NONE)
                    Sys_QueEvent(t, SE_KEY, k, e.type == SDL_KEYDOWN ? 1 : 0, 0, nullptr);
                break;
            }
            case SDL_TEXTINPUT:
                for (const char *p = e.text.text; *p; ++p)
                    Sys_QueEvent(t, SE_CHAR, (unsigned char)*p, 0, 0, nullptr);
                break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP: {
                int btn = e.button.button - SDL_BUTTON_LEFT;  // 0-based -> K_MOUSE1..
                Sys_QueEvent(t, SE_KEY, K_MOUSE1 + btn, e.type == SDL_MOUSEBUTTONDOWN ? 1 : 0, 0, nullptr);
                break;
            }
            case SDL_MOUSEWHEEL: {
                int k = e.wheel.y > 0 ? K_MWHEELUP : K_MWHEELDOWN;
                Sys_QueEvent(t, SE_KEY, k, 1, 0, nullptr);  // wheel = press + release
                Sys_QueEvent(t, SE_KEY, k, 0, 0, nullptr);
                break;
            }
            default: break;
        }
    }
#endif // !__EMSCRIPTEN_PTHREADS__
}
