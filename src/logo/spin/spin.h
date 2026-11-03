#pragma once

#include "options/logo.h"

#include <stdbool.h>
#include <stdint.h>

// spin.c (logo integration)
void ffSpinLogoPrint(void);
bool ffSpinLogoAnimationWanted(void);
bool ffSpinLogoAnimationActive(void);

typedef struct FFSpinAnimationInfo {
    bool active;
    uint32_t top;
    uint32_t left;
    uint32_t width;
    uint32_t height;
    float speed;
    float tilt;
    uint32_t fps;
    uint32_t frames;
} FFSpinAnimationInfo;

const FFSpinAnimationInfo* ffSpinGetAnimationInfo(void);
// The art of the logo printed last, which the animation spins.
const struct FFSpinArt* ffSpinGetArt(void);
// Art for a preview: "" or "auto" is the detected OS, otherwise a builtin
// logo name. Uses the logo's own colors. The caller destroys `art`.
bool ffSpinLoadPreviewArt(const FFstrbuf* source, FFLogoVariant variant, struct FFSpinArt* art);

// loop.c
typedef enum FFSpinKey {
    FF_SPIN_KEY_NONE = 0,
    FF_SPIN_KEY_UP,
    FF_SPIN_KEY_DOWN,
    FF_SPIN_KEY_LEFT,
    FF_SPIN_KEY_RIGHT,
    FF_SPIN_KEY_ENTER,
    FF_SPIN_KEY_ESCAPE,
    FF_SPIN_KEY_SPACE,
    FF_SPIN_KEY_CHAR,
    FF_SPIN_KEY_RESIZE,
    FF_SPIN_KEY_UNKNOWN,
} FFSpinKey;

void ffSpinTerminalEnter(void);
void ffSpinTerminalLeave(void);
void ffSpinTerminalClear(void);
bool ffSpinTerminalRead(FFSpinKey* key, char* ch, uint32_t timeoutMs);
uint32_t ffSpinTerminalRows(void);
uint32_t ffSpinTerminalColumns(void);
bool ffSpinTerminalIsRaw(void);

typedef void (*FFSpinRedrawFn)(void* data);
void ffSpinRunAnimation(FFSpinRedrawFn redraw, void* redrawData);
