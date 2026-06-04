// sdl_events.cpp — SDL → engine input translation.
#include "sdl_events.h"

#include <SDL2/SDL.h>
#include <ui/keycodes.h>      // keyNum_t (the engine's key codes)
#include <win32/win_local.h>  // sysEventType_t (SE_KEY / SE_CHAR / ...)

#if defined(__EMSCRIPTEN_PTHREADS__)
// The MT web build never creates an SDL window (the GL context is made directly via
// emscripten_webgl on the render worker — see glcontext_sdl.cpp), so SDL registers no
// DOM event handlers and SDL_PollEvent/SDL_GetMouseState stay empty. Bypass SDL for
// input the same way we bypass it for GL: register Emscripten HTML5 callbacks on the
// game worker (the thread that pumps events) and feed the engine's Sys_QueEvent /
// IN_Frame paths directly. Mouse state lives in plain globals read by IN_Frame on the
// same worker thread (the callbacks fire between Com_Frame ticks on that worker).
#include <emscripten/html5.h>
#include <cstring>
#include <cctype>
#include <cstdlib>
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
// --- Emscripten HTML5 input (MT web build) ---------------------------------
// Shared with IN_Frame (src/platform/linux/sys_platform.cpp). Written by the HTML5
// mouse callbacks and read by IN_Frame, both on the game worker — benign single-
// thread access. g_webMouse{X,Y} are in engine backbuffer pixels (scaled from CSS).
extern "C" {
double g_webMouseX = 0.0, g_webMouseY = 0.0;   // canvas-relative, scaled to engine res
double g_webMouseDx = 0.0, g_webMouseDy = 0.0; // accumulated pointer-lock look deltas
}

namespace {
double s_cssW = 1.0, s_cssH = 1.0;   // canvas display (CSS) size, px
int    s_fbW  = 1920, s_fbH = 1080;  // engine backbuffer size (set at device create)
bool   s_inputInited = false;

void RefreshCssSize() {
    double w = 0.0, h = 0.0;
    if (emscripten_get_element_css_size("#canvas", &w, &h) == EMSCRIPTEN_RESULT_SUCCESS
        && w > 0.0 && h > 0.0) { s_cssW = w; s_cssH = h; }
}

// DOM KeyboardEvent.code (physical key) -> engine keyNum_t. `key` (the produced
// character, shift-applied) is the fallback for punctuation and the SE_CHAR source.
int WebCodeToKeyNum(const char *code, const char *key) {
    if (!strncmp(code, "Key", 3)   && code[3] && !code[4]) return tolower((unsigned char)code[3]); // a..z
    if (!strncmp(code, "Digit", 5) && code[5] && !code[6]) return code[5];                          // 0..9
    if (!strcmp(code, "NumpadEnter")) return K_ENTER;
    if (!strncmp(code, "Numpad", 6) && code[6] >= '0' && code[6] <= '9' && !code[7]) return code[6];
    if (!strcmp(code, "Space"))      return K_SPACE;
    if (!strcmp(code, "Enter"))      return K_ENTER;
    if (!strcmp(code, "Escape"))     return K_ESCAPE;
    if (!strcmp(code, "Backspace"))  return K_BACKSPACE;
    if (!strcmp(code, "Tab"))        return K_TAB;
    if (!strcmp(code, "ArrowUp"))    return K_UPARROW;
    if (!strcmp(code, "ArrowDown"))  return K_DOWNARROW;
    if (!strcmp(code, "ArrowLeft"))  return K_LEFTARROW;
    if (!strcmp(code, "ArrowRight")) return K_RIGHTARROW;
    if (!strcmp(code, "ShiftLeft")   || !strcmp(code, "ShiftRight"))   return K_SHIFT;
    if (!strcmp(code, "ControlLeft") || !strcmp(code, "ControlRight")) return K_CTRL;
    if (!strcmp(code, "AltLeft")     || !strcmp(code, "AltRight"))     return K_ALT;
    if (!strcmp(code, "Insert"))     return K_INS;
    if (!strcmp(code, "Delete"))     return K_DEL;
    if (!strcmp(code, "Home"))       return K_HOME;
    if (!strcmp(code, "End"))        return K_END;
    if (!strcmp(code, "PageUp"))     return K_PGUP;
    if (!strcmp(code, "PageDown"))   return K_PGDN;
    if (!strcmp(code, "CapsLock"))   return K_CAPSLOCK;
    if (code[0] == 'F' && code[1] >= '1' && code[1] <= '9') {
        int n = atoi(code + 1);
        if (n >= 1 && n <= 12) return K_F1 + (n - 1);
    }
    if (key && key[0] && !key[1] && (unsigned char)key[0] >= 0x20 && (unsigned char)key[0] < 0x7F)
        return tolower((unsigned char)key[0]);   // punctuation: '-', '=', '[', ...
    return K_NONE;
}

EM_BOOL WebOnKey(int type, const EmscriptenKeyboardEvent *e, void *) {
    if (e->repeat && type == EMSCRIPTEN_EVENT_KEYDOWN) {
        // Engine wants repeats for held keys in menus/console; keep delivering SE_KEY.
    }
    int down = (type == EMSCRIPTEN_EVENT_KEYDOWN);
    int k = WebCodeToKeyNum(e->code, e->key);
    if (k != K_NONE) Sys_QueEvent(0, SE_KEY, k, down ? 1 : 0, 0, nullptr);
    if (down && e->key[0] && !e->key[1]
        && (unsigned char)e->key[0] >= 0x20 && (unsigned char)e->key[0] < 0x7F)
        Sys_QueEvent(0, SE_CHAR, (unsigned char)e->key[0], 0, 0, nullptr);
    // Let browser shortcuts (Ctrl/Cmd/Alt combos, F-keys) through; consume game keys
    // so Tab/Space/arrows don't scroll or move focus.
    if (e->ctrlKey || e->metaKey || e->altKey) return EM_FALSE;
    return (k != K_NONE) ? EM_TRUE : EM_FALSE;
}

void WebUpdateMousePos(const EmscriptenMouseEvent *e) {
    double sx = (s_cssW > 0.0) ? (double)s_fbW / s_cssW : 1.0;
    double sy = (s_cssH > 0.0) ? (double)s_fbH / s_cssH : 1.0;
    g_webMouseX = (double)e->targetX * sx;
    g_webMouseY = (double)e->targetY * sy;
}

EM_BOOL WebOnMouseMove(int, const EmscriptenMouseEvent *e, void *) {
    WebUpdateMousePos(e);
    g_webMouseDx += (double)e->movementX;   // raw deltas for pointer-locked look
    g_webMouseDy += (double)e->movementY;
    return EM_FALSE;
}

EM_BOOL WebOnMouseButton(int type, const EmscriptenMouseEvent *e, void *) {
    WebUpdateMousePos(e);
    int down = (type == EMSCRIPTEN_EVENT_MOUSEDOWN);
    // DOM button 0/1/2 = left/middle/right -> K_MOUSE1/2/3, matching the SDL path.
    Sys_QueEvent(0, SE_KEY, K_MOUSE1 + e->button, down ? 1 : 0, 0, nullptr);
    return EM_TRUE;
}

EM_BOOL WebOnWheel(int, const EmscriptenWheelEvent *e, void *) {
    int k = (e->deltaY < 0.0) ? K_MWHEELUP : K_MWHEELDOWN;
    Sys_QueEvent(0, SE_KEY, k, 1, 0, nullptr);   // wheel = press + release
    Sys_QueEvent(0, SE_KEY, k, 0, 0, nullptr);
    return EM_TRUE;
}

EM_BOOL WebOnResize(int, const EmscriptenUiEvent *, void *) { RefreshCssSize(); return EM_FALSE; }

void WebInputInit() {
    if (s_inputInited) return;
    s_inputInited = true;
    RefreshCssSize();
    const pthread_t self = EM_CALLBACK_THREAD_CONTEXT_CALLING_THREAD;
    const char *WIN = EMSCRIPTEN_EVENT_TARGET_WINDOW, *CAN = "#canvas";
    // Keyboard on the window so it works without canvas focus; mouse on the canvas.
    emscripten_set_keydown_callback_on_thread(WIN, nullptr, EM_TRUE, WebOnKey, self);
    emscripten_set_keyup_callback_on_thread  (WIN, nullptr, EM_TRUE, WebOnKey, self);
    emscripten_set_mousemove_callback_on_thread(CAN, nullptr, EM_TRUE, WebOnMouseMove,   self);
    emscripten_set_mousedown_callback_on_thread(CAN, nullptr, EM_TRUE, WebOnMouseButton, self);
    emscripten_set_mouseup_callback_on_thread  (CAN, nullptr, EM_TRUE, WebOnMouseButton, self);
    emscripten_set_wheel_callback_on_thread    (CAN, nullptr, EM_TRUE, WebOnWheel,       self);
    emscripten_set_resize_callback_on_thread   (WIN, nullptr, EM_TRUE, WebOnResize,      self);
}
} // namespace

// Called from the GL backend at device creation so mouse coords scale to the engine's
// actual backbuffer resolution (not the default 300x150 canvas).
extern "C" void WebInput_SetResolution(int w, int h) {
    if (w > 0) s_fbW = w;
    if (h > 0) s_fbH = h;
}
#endif // __EMSCRIPTEN_PTHREADS__

void Sys_PumpSDLEvents(unsigned t) {
#if defined(__EMSCRIPTEN_PTHREADS__)
    // HTML5 callbacks queue key/button events straight into Sys_QueEvent as the worker
    // processes its proxied DOM messages — nothing to drain here. Just ensure the
    // callbacks are registered (on this, the event-pumping, worker thread).
    (void)t;
    WebInputInit();
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
