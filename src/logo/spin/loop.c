#include "logo/spin/spin.h"

#include "common/io.h"
#include "detection/terminalsize/terminalsize.h"
#include "logo/spin/render.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef M_PI
    #define M_PI 3.14159265358979323846
#endif

#ifndef _WIN32
    #include <poll.h>
    #include <signal.h>
    #include <termios.h>
    #include <unistd.h>
#else
    #include <conio.h>
    #include <windows.h>
#endif

#ifndef _WIN32
static struct termios gSavedTermios;
#endif
static bool gRawMode = false;
static bool gAltScreen = false;
static bool gHandlersInstalled = false;
static volatile sig_atomic_t gResizeRequested = 0;
static char gInputBuffer[32];
static uint32_t gInputLength = 0;
static uint32_t gInputOffset = 0;

static const char gEnterSequence[] = "\e[?1049h\e[2J\e[H\e[?25l\e[?7l";
static const char gLeaveSequence[] = "\e[0m\e[?25h\e[?7h\e[?1049l";

#ifndef _WIN32
static void terminalResizeSignal(int sig) {
    (void) sig;
    gResizeRequested = 1;
}
#endif

static void terminalRestore(void) {
    if (!gAltScreen && !gRawMode) {
        return;
    }
#ifndef _WIN32
    if (gRawMode) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &gSavedTermios);
        gRawMode = false;
    }
#endif
    if (gAltScreen) {
        fputs(gLeaveSequence, stdout);
        fflush(stdout);
        gAltScreen = false;
    }
}

#ifndef _WIN32
static void terminalSignal(int sig) {
    if (gRawMode) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &gSavedTermios);
        gRawMode = false;
    }
    if (gAltScreen) {
        // write(2) is async-signal-safe; stdio is not.
        ssize_t ignored = write(STDOUT_FILENO, gLeaveSequence, sizeof(gLeaveSequence) - 1);
        (void) ignored;
        gAltScreen = false;
    }
    _exit(128 + sig);
}
#else
static BOOL WINAPI terminalSignal(DWORD sig) {
    terminalRestore();
    return FALSE;
    (void) sig;
}
#endif

static void installTerminalHandlers(void) {
    if (gHandlersInstalled) {
        return;
    }
    gHandlersInstalled = true;
    atexit(terminalRestore);
#ifndef _WIN32
    struct sigaction action = {};
    action.sa_handler = terminalSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    for (int sig = 0; sig < 32; ++sig) {
        switch (sig) {
            case SIGINT:
            case SIGTERM:
            case SIGQUIT:
            case SIGABRT:
            case SIGSEGV:
            case SIGFPE:
            case SIGILL:
                sigaction(sig, &action, nullptr);
                break;
            default:
                break;
        }
    }
    struct sigaction resize = {};
    resize.sa_handler = terminalResizeSignal;
    sigemptyset(&resize.sa_mask);
    resize.sa_flags = 0;
    sigaction(SIGWINCH, &resize, nullptr);
#else
    SetConsoleCtrlHandler(terminalSignal, TRUE);
#endif
}

void ffSpinTerminalEnter(void) {
    installTerminalHandlers();
    if (!gAltScreen) {
#ifndef _WIN32
        if (!gRawMode && tcgetattr(STDIN_FILENO, &gSavedTermios) == 0) {
            struct termios raw = gSavedTermios;
            raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO | ISIG | IEXTEN);
            raw.c_iflag &= (tcflag_t) ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
            raw.c_cflag |= CS8;
            raw.c_cc[VMIN] = 0;
            raw.c_cc[VTIME] = 0;
            if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0) {
                gRawMode = true;
            }
        }
#endif
        fputs(gEnterSequence, stdout);
        fflush(stdout);
        gAltScreen = true;
    }
}

void ffSpinTerminalLeave(void) {
    terminalRestore();
}

void ffSpinTerminalClear(void) {
    fputs("\e[2J\e[H", stdout);
    fflush(stdout);
}

static bool readInputByte(char* out) {
    if (gInputOffset < gInputLength) {
        *out = gInputBuffer[gInputOffset++];
        return true;
    }
    gInputLength = 0;
    gInputOffset = 0;
    ssize_t n = ffReadFDData(FFUnixFD2NativeFD(STDIN_FILENO), sizeof(gInputBuffer), gInputBuffer);
    if (n <= 0) {
        return false;
    }
    gInputLength = (uint32_t) n;
    *out = gInputBuffer[gInputOffset++];
    return true;
}

bool ffSpinTerminalRead(FFSpinKey* key, char* ch, uint32_t timeoutMs) {
    *key = FF_SPIN_KEY_NONE;
    if (ch) *ch = 0;
    if (gResizeRequested) {
        gResizeRequested = 0;
        *key = FF_SPIN_KEY_RESIZE;
        return true;
    }

    // Keys left over from an earlier read come first; waiting would only
    // report new input and leave them stuck until the next key press.
    if (gInputOffset >= gInputLength) {
#ifndef _WIN32
        struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
        int result = poll(&pfd, 1, (int) timeoutMs);
        if (result < 0) {
            if (errno == EINTR) {
                if (gResizeRequested) {
                    gResizeRequested = 0;
                    *key = FF_SPIN_KEY_RESIZE;
                    return true;
                }
            }
            return false;
        }
        if (result == 0 || !(pfd.revents & POLLIN)) {
            return false;
        }
#else
        (void) timeoutMs;
        if (!_kbhit()) {
            return false;
        }
#endif
    }

    char c = 0;
    if (!readInputByte(&c)) {
        return false;
    }
    if (c == '\r' || c == '\n') {
        *key = FF_SPIN_KEY_ENTER;
        return true;
    }
    if (c == ' ') {
        *key = FF_SPIN_KEY_SPACE;
        return true;
    }
    if (c != '\x1b') {
        if (ch) *ch = c;
        *key = FF_SPIN_KEY_CHAR;
        return true;
    }

    char next = 0;
    if (!readInputByte(&next)) {
        *key = FF_SPIN_KEY_ESCAPE;
        return true;
    }
    if (next != '[' && next != 'O') {
        *key = FF_SPIN_KEY_ESCAPE;
        return true;
    }
    char code = 0;
    while (readInputByte(&code)) {
        if (code >= 'A' && code <= 'D') {
            switch (code) {
                case 'A': *key = FF_SPIN_KEY_UP; break;
                case 'B': *key = FF_SPIN_KEY_DOWN; break;
                case 'C': *key = FF_SPIN_KEY_RIGHT; break;
                case 'D': *key = FF_SPIN_KEY_LEFT; break;
            }
            return true;
        }
        if (!((code >= '0' && code <= '9') || code == ';')) {
            break;
        }
    }
    *key = FF_SPIN_KEY_UNKNOWN;
    return true;
}

uint32_t ffSpinTerminalRows(void) {
    FFTerminalSizeResult size = {};
    if (ffDetectTerminalSize(&size, true) && size.rows > 0) {
        return size.rows;
    }
    return 24;
}

uint32_t ffSpinTerminalColumns(void) {
    FFTerminalSizeResult size = {};
    if (ffDetectTerminalSize(&size, true) && size.columns > 0) {
        return size.columns;
    }
    return 80;
}

bool ffSpinTerminalIsRaw(void) {
    return gRawMode;
}

static uint64_t monotonicMs(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000u + (uint64_t) ts.tv_nsec / 1000000u;
#endif
}

static void appendCursorMove(FFstrbuf* out, uint32_t row, uint32_t col) {
    ffStrbufAppendF(out, "\e[%u;%uH", row, col);
}

static bool cellEqual(const FFSpinFrame* a, const FFSpinFrame* b, size_t index) {
    return a->glyphs[index] == b->glyphs[index] && a->colors[index] == b->colors[index];
}

static void drawDiff(const FFSpinFrame* oldFrame, const FFSpinFrame* newFrame, const FFSpinAnimationInfo* info, const FFSpinArt* art, bool bold) {
    FF_STRBUF_AUTO_DESTROY out = ffStrbufCreateA(4096);
    for (uint32_t y = 0; y < newFrame->height; ++y) {
        size_t row = (size_t) y * newFrame->width;
        uint32_t x = 0;
        while (x < newFrame->width) {
            if (cellEqual(oldFrame, newFrame, row + x)) {
                ++x;
                continue;
            }
            uint32_t start = x;
            while (x < newFrame->width && !cellEqual(oldFrame, newFrame, row + x)) {
                ++x;
            }
            appendCursorMove(&out, info->top + y, info->left + start);
            ffSpinAppendCells(&out, newFrame, art, (uint32_t) row + start, x - start, true, bold);
        }
    }
    if (out.length > 0) {
        fwrite(out.chars, 1, out.length, stdout);
        fflush(stdout);
    }
}

static void drawFull(const FFSpinFrame* frame, const FFSpinAnimationInfo* info, const FFSpinArt* art, bool bold) {
    FF_STRBUF_AUTO_DESTROY out = ffStrbufCreateA((uint32_t) frame->width * frame->height * 4);
    for (uint32_t y = 0; y < frame->height; ++y) {
        appendCursorMove(&out, info->top + y, info->left);
        ffSpinAppendCells(&out, frame, art, y * frame->width, frame->width, true, bold);
    }
    fwrite(out.chars, 1, out.length, stdout);
    fflush(stdout);
}

static bool frameInitForInfo(FFSpinFrame* frame, const FFSpinAnimationInfo* info) {
    return ffSpinFrameInit(frame, info->width, info->height);
}

void ffSpinRunAnimation(FFSpinRedrawFn redraw, void* redrawData) {
    if (!ffSpinLogoAnimationWanted() || !ffSpinGetAnimationInfo()->active) {
        return;
    }

    const bool bold = instance.config.display.brightColor;
    const FFSpinArt* art = ffSpinGetArt();

    ffSpinTerminalEnter();
    ffSpinTerminalClear();

    // The fetch output was printed before the alternate screen was entered.
    // Reprint it inside the animation screen so the logo has its information
    // panel during the whole animation, including after a resize.
    if (redraw) {
        redraw(redrawData);
    }

    FFSpinAnimationInfo info = *ffSpinGetAnimationInfo();
    if (!info.active) {
        ffSpinTerminalLeave();
        return;
    }

    FFSpinFrame current = {};
    FFSpinFrame next = {};
    if (!frameInitForInfo(&current, &info) || !frameInitForInfo(&next, &info)) {
        ffSpinFrameDestroy(&current);
        ffSpinFrameDestroy(&next);
        ffSpinTerminalLeave();
        return;
    }
    ffSpinRenderArt(art, 0.0f, info.tilt, &current);
    drawFull(&current, &info, art, bold);

    uint64_t start = monotonicMs();
    uint64_t nextFrame = start;
    uint64_t frameMs = 1000u / (info.fps ? info.fps : 20u);
    if (frameMs == 0) frameMs = 1;
    uint32_t frameNumber = 0;
    bool exitRequested = false;

    while (!exitRequested) {
        uint64_t now = monotonicMs();
        uint32_t timeout = 0;
        if (now < nextFrame) {
            timeout = (uint32_t) (nextFrame - now);
        }
        FFSpinKey key = FF_SPIN_KEY_NONE;
        char ch = 0;
        if (ffSpinTerminalRead(&key, &ch, timeout)) {
            if (key == FF_SPIN_KEY_RESIZE) {
                ffSpinTerminalClear();
                if (redraw) {
                    redraw(redrawData);
                }
                info = *ffSpinGetAnimationInfo();
                if (!info.active) {
                    break;
                }
                ffSpinFrameDestroy(&current);
                ffSpinFrameDestroy(&next);
                if (!frameInitForInfo(&current, &info) || !frameInitForInfo(&next, &info)) {
                    break;
                }
                ffSpinRenderArt(art, 0.0f, info.tilt, &current);
                drawFull(&current, &info, art, bold);
                frameMs = 1000u / (info.fps ? info.fps : 20u);
                if (frameMs == 0) frameMs = 1;
                nextFrame = monotonicMs();
                frameNumber = 0;
                start = nextFrame;
                continue;
            }
            if (key != FF_SPIN_KEY_NONE) {
                exitRequested = true;
                break;
            }
        }

        now = monotonicMs();
        double seconds = (double) (now - start) / 1000.0;
        float yaw = (float) seconds * info.speed;
        ffSpinRenderArt(art, yaw, info.tilt, &next);
        drawDiff(&current, &next, &info, art, bold);
        FFSpinFrame previous = current;
        current = next;
        next = previous;
        ++frameNumber;
        if (info.frames > 0 && frameNumber >= info.frames) {
            break;
        }
        nextFrame += frameMs;
        if (nextFrame <= now) {
            nextFrame = now + frameMs;
        }
    }

    ffSpinFrameDestroy(&current);
    ffSpinFrameDestroy(&next);
    ffSpinTerminalLeave();
}
