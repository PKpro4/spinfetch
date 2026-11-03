#include "detect/spin_wizard.h"

#include "fastfetch.h"
#include "common/io.h"
#include "common/strutil.h"
#include "common/time.h"
#include "detection/os/os.h"
#include "logo/logo.h"
#include "logo/spin/render.h"
#include "logo/spin/spin.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
    #define M_PI 3.14159265358979323846
#endif

#define WIZARD_MIN_COLUMNS 40
#define WIZARD_MIN_ROWS 14

// A short list of popular logos. Any builtin fastfetch logo name works in the
// config; these are just the ones offered here.
static const char* const wizardLogoNames[] = {
    "arch", "gentoo", "debian", "ubuntu", "fedora", "nixos", "opensuse", "manjaro", "linuxmint", "pop",
    "void", "alpine", "endeavouros", "cachyos", "kali", "bedrock", "freebsd", "macos", "windows",
};
static const char* const wizardLogoLabels[] = {
    "Arch Linux", "Gentoo", "Debian", "Ubuntu", "Fedora", "NixOS", "openSUSE", "Manjaro", "Linux Mint", "Pop!_OS",
    "Void Linux", "Alpine", "EndeavourOS", "CachyOS", "Kali", "Bedrock Linux", "FreeBSD", "macOS", "Windows",
};
#define WIZARD_LOGO_COUNT (sizeof(wizardLogoNames) / sizeof(wizardLogoNames[0]))

static const char* const wizardModuleNames[] = {
    "title",
    "os",
    "host",
    "kernel",
    "uptime",
    "packages",
    "shell",
    "terminal",
    "de",
    "wm",
    "cpu",
    "gpu",
    "memory",
    "disk",
    "localip",
};

static const bool wizardModuleDefaults[] = {
    true,
    true,
    true,
    true,
    true,
    true,
    true,
    true,
    false,
    false,
    true,
    true,
    true,
    true,
    false,
};

#define WIZARD_MODULE_COUNT (sizeof(wizardModuleNames) / sizeof(wizardModuleNames[0]))

static const float wizardSpeeds[] = { 0.5f, 1.0f, 2.0f, 3.0f };
static const uint32_t wizardFps[] = { 10, 15, 20, 24, 30 };
static const int32_t wizardTilts[] = { -15, 0, 10, 15, 25, 35 };
// Index 0 keeps the logo's own colors; the others replace its first color.
static const char* const wizardAccentNames[] = { "logo colors", "white", "cyan", "yellow", "blue", "magenta", "green", "red", "light_white" };
static const char* const wizardAccentCodes[] = { "", "37", "36", "33", "34", "35", "32", "31", "97" };

#define WIZARD_ACCENT_COUNT (sizeof(wizardAccentNames) / sizeof(wizardAccentNames[0]))

typedef struct FFSpinWizardState {
    uint32_t screen;
    uint32_t cursor;
    uint32_t logoCursor;
    uint32_t moduleCursor;
    char detectedLogo[64];
    FFLogoVariant variant;
    uint32_t speedIndex;
    uint32_t fpsIndex;
    uint32_t tiltIndex;
    uint32_t accentIndex;
    bool sshSafe;
    bool modules[WIZARD_MODULE_COUNT];
    bool confirming;
    bool needsClear;
    uint32_t lastColumns;
    uint32_t lastRows;
    char status[192];
    float angle;
} FFSpinWizardState;

static uint32_t wizardColumns(void) {
    uint32_t columns = ffSpinTerminalColumns();
    return columns > 0 ? columns : 80;
}

static uint32_t wizardRows(void) {
    uint32_t rows = ffSpinTerminalRows();
    return rows > 0 ? rows : 24;
}

static uint32_t wizardLogoCursorFromSource(const FFstrbuf* source) {
    for (uint32_t i = 0; i < WIZARD_LOGO_COUNT; ++i) {
        if (ffStrbufIgnCaseEqualS(source, wizardLogoNames[i])) {
            return i + 1;
        }
    }
    return 0;
}

static const char* wizardSelectedSource(const FFSpinWizardState* state) {
    return state->logoCursor == 0 ? "auto" : wizardLogoNames[state->logoCursor - 1];
}

static uint32_t wizardAccentFromConfig(const FFstrbuf* color) {
    if (color->length == 0) {
        return 0;
    }
    for (uint32_t i = 1; i < WIZARD_ACCENT_COUNT; ++i) {
        if (ffStrbufIgnCaseEqualS(color, wizardAccentNames[i])
            || ffStrbufEqualS(color, wizardAccentCodes[i])) {
            return i;
        }
    }
    return 0;
}

static void wizardSetDefaults(FFSpinWizardState* state) {
    for (uint32_t i = 0; i < WIZARD_MODULE_COUNT; ++i) {
        state->modules[i] = wizardModuleDefaults[i];
    }
}

static void wizardClearModules(FFSpinWizardState* state) {
    for (uint32_t i = 0; i < WIZARD_MODULE_COUNT; ++i) {
        state->modules[i] = false;
    }
}

static bool wizardAnyModule(const FFSpinWizardState* state) {
    for (uint32_t i = 0; i < WIZARD_MODULE_COUNT; ++i) {
        if (state->modules[i]) {
            return true;
        }
    }
    return false;
}

static void wizardSeedModules(FFSpinWizardState* state, FFdata* data) {
    wizardSetDefaults(state);

    if (data->structure.length > 0) {
        wizardClearModules(state);
        const char* start = data->structure.chars;
        while (*start != '\0') {
            const char* end = strchr(start, ':');
            uint32_t length = end ? (uint32_t) (end - start) : (uint32_t) strlen(start);
            FF_STRBUF_AUTO_DESTROY name = ffStrbufCreateNS(length, start);
            for (uint32_t i = 0; i < WIZARD_MODULE_COUNT; ++i) {
                if (ffStrbufIgnCaseEqualS(&name, wizardModuleNames[i])) {
                    state->modules[i] = true;
                }
            }
            if (!end) {
                break;
            }
            start = end + 1;
        }
        if (!wizardAnyModule(state)) {
            wizardSetDefaults(state);
        }
        return;
    }

    if (data->configDoc) {
        yyjson_val* root = yyjson_doc_get_root(data->configDoc);
        yyjson_val* modules = yyjson_obj_get(root, "modules");
        if (yyjson_is_arr(modules)) {
            wizardClearModules(state);
            yyjson_val* item;
            size_t index, max;
            yyjson_arr_foreach (modules, index, max, item) {
                const char* type = yyjson_is_str(item)
                    ? unsafe_yyjson_get_str(item)
                    : yyjson_is_obj(item)
                    ? unsafe_yyjson_get_str(yyjson_obj_get(item, "type"))
                    : nullptr;
                if (!type) {
                    continue;
                }
                for (uint32_t i = 0; i < WIZARD_MODULE_COUNT; ++i) {
                    if (ffStrEqualsIgnCase(type, wizardModuleNames[i])) {
                        state->modules[i] = true;
                    }
                }
            }
            if (!wizardAnyModule(state)) {
                wizardSetDefaults(state);
            }
        }
    }
}

static bool wizardSeed(FFSpinWizardState* state, FFdata* data) {
    snprintf(state->detectedLogo, sizeof(state->detectedLogo), "%s", ffLogoGetBuiltinDetected(FF_LOGO_SIZE_NORMAL)->names[0]);
    state->variant = instance.config.logo.variant;
    state->angle = 0.0f;
    state->needsClear = true;
    state->status[0] = '\0';

    state->logoCursor = wizardLogoCursorFromSource(&instance.config.logo.source);
    state->cursor = state->logoCursor;

    state->speedIndex = 1;
    state->fpsIndex = 2;
    state->tiltIndex = 3;
    for (uint32_t i = 0; i < ARRAY_SIZE(wizardSpeeds); ++i) {
        if (fabsf(instance.config.logo.spin.speed - wizardSpeeds[i]) < 0.01f) {
            state->speedIndex = i;
        }
    }
    if (instance.config.logo.spin.fps > 0) {
        for (uint32_t i = 0; i < ARRAY_SIZE(wizardFps); ++i) {
            if (instance.config.logo.spin.fps == wizardFps[i]) {
                state->fpsIndex = i;
            }
        }
    }
    for (uint32_t i = 0; i < ARRAY_SIZE(wizardTilts); ++i) {
        if ((int32_t) lroundf(instance.config.logo.spin.tilt) == wizardTilts[i]) {
            state->tiltIndex = i;
        }
    }

    state->accentIndex = wizardAccentFromConfig(&instance.config.logo.colors[0]);
    state->sshSafe = instance.config.logo.spin.sshSafe;

    state->moduleCursor = 0;
    wizardSeedModules(state, data);
    return true;
}

static bool wizardConfigPath(FFstrbuf* path) {
    FF_LIST_FOR_EACH (FFstrbuf, dir, instance.state.platform.configDirs) {
        ffStrbufSet(path, dir);
        ffStrbufAppendS(path, "spinfetch/config.jsonc");
        return true;
    }
    return false;
}

static bool wizardWriteConfig(const FFSpinWizardState* state, const FFstrbuf* path) {
    FF_STRBUF_AUTO_DESTROY out = ffStrbufCreateA(4096);

    ffStrbufAppendS(&out,
        "{\n"
        "    \"$schema\": \"https://github.com/fastfetch-cli/fastfetch/raw/master/doc/json_schema.json\",\n"
        "    \"logo\": {\n"
        "        \"type\": \"spin\",\n"
        "        \"source\": \"");
    ffStrbufAppendS(&out, wizardSelectedSource(state));
    ffStrbufAppendF(&out,
        "\",\n"
        "        \"variant\": \"%s\",\n",
        state->variant == FF_LOGO_VARIANT_MINIMAL ? "minimal" : "full");
    if (state->accentIndex > 0) {
        ffStrbufAppendF(&out,
            "        \"color\": {\n"
            "            \"1\": \"%s\"\n"
            "        },\n",
            wizardAccentNames[state->accentIndex]);
    }
    ffStrbufAppendF(&out,
        "        \"spin\": {\n"
        "            \"speed\": %.1f,\n"
        "            \"tilt\": %d,\n"
        "            \"fps\": %u,\n"
        "            \"sshSafe\": %s\n"
        "        }\n"
        "    },\n"
        "    \"modules\": [\n",
        (double) wizardSpeeds[state->speedIndex],
        (int) wizardTilts[state->tiltIndex],
        wizardFps[state->fpsIndex],
        state->sshSafe ? "true" : "false");

    uint32_t selected = 0;
    for (uint32_t i = 0; i < WIZARD_MODULE_COUNT; ++i) {
        if (!state->modules[i]) {
            continue;
        }
        if (selected > 0) {
            ffStrbufAppendS(&out, ",\n");
        }
        ffStrbufAppendF(&out, "        \"%s\"", wizardModuleNames[i]);
        ++selected;
    }
    if (selected > 0) {
        ffStrbufAppendC(&out, '\n');
    }
    ffStrbufAppendS(&out, "    ]\n}\n");

    return ffWriteFileBuffer(path->chars, &out);
}

static void wizardApply(const FFSpinWizardState* state, FFdata* data) {
    FFOptionsLogo* options = &instance.config.logo;
    options->type = FF_LOGO_TYPE_SPIN;
    ffStrbufSetS(&options->source, wizardSelectedSource(state));
    options->variant = state->variant;
    options->spin.speed = wizardSpeeds[state->speedIndex];
    options->spin.tilt = (float) wizardTilts[state->tiltIndex];
    options->spin.fps = wizardFps[state->fpsIndex];
    options->spin.sshSafe = state->sshSafe;
    if (state->accentIndex > 0) {
        ffOptionParseColor(wizardAccentNames[state->accentIndex], &options->colors[0]);
    } else {
        ffStrbufClear(&options->colors[0]);
    }

    ffStrbufClear(&data->structure);
    uint32_t selected = 0;
    for (uint32_t i = 0; i < WIZARD_MODULE_COUNT; ++i) {
        if (!state->modules[i]) {
            continue;
        }
        if (selected > 0) {
            ffStrbufAppendC(&data->structure, ':');
        }
        ffStrbufAppendS(&data->structure, wizardModuleNames[i]);
        ++selected;
    }
}

static uint32_t wizardSelectedModuleCount(const FFSpinWizardState* state) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < WIZARD_MODULE_COUNT; ++i) {
        count += state->modules[i] ? 1 : 0;
    }
    return count;
}

static void wizardLine(FFstrbuf* out, uint32_t row, uint32_t columns, const char* text) {
    if (row == 0) {
        return;
    }
    ffStrbufAppendF(out, "\e[%u;1H\e[K", row);
    uint32_t length = (uint32_t) strlen(text);
    if (length >= columns) {
        length = columns > 0 ? columns - 1 : 0;
    }
    ffStrbufAppendNS(out, length, text);
}

static void wizardSeparator(FFstrbuf* out, uint32_t row, uint32_t columns) {
    FF_STRBUF_AUTO_DESTROY line = ffStrbufCreateA(columns);
    uint32_t length = columns > 1 ? columns - 1 : 0;
    for (uint32_t i = 0; i < length; ++i) {
        ffStrbufAppendC(&line, '-');
    }
    wizardLine(out, row, columns, line.chars);
}

static float wizardTiltRadians(const FFSpinWizardState* state) {
    return (float) wizardTilts[state->tiltIndex] * (float) M_PI / 180.0f;
}

// The selected logo's art in `variant`, with the chosen accent applied.
static bool wizardLoadArt(const FFSpinWizardState* state, FFLogoVariant variant, FFSpinArt* art) {
    FF_STRBUF_AUTO_DESTROY source = ffStrbufCreateS(wizardSelectedSource(state));
    if (!ffSpinLoadPreviewArt(&source, variant, art)) {
        return false;
    }
    if (state->accentIndex > 0) {
        ffStrbufSetS(&art->palette[1], wizardAccentCodes[state->accentIndex]);
    }
    return true;
}

// Draws the spinning logo centered in the box at (top, left), falling back to
// the small logo when the chosen one does not fit. Returns the rows used.
static uint32_t wizardDrawPreview(FFstrbuf* out, const FFSpinWizardState* state,
    uint32_t top, uint32_t left, uint32_t boxColumns, uint32_t boxRows) {
    const FFLogoVariant variants[2] = { state->variant, FF_LOGO_VARIANT_MINIMAL };
    for (uint32_t i = 0; i < 2; ++i) {
        FFSpinArt art = {};
        uint32_t width = 0, height = 0;
        if (wizardLoadArt(state, variants[i], &art)) {
            ffSpinFrameSize(&art, wizardTiltRadians(state), &width, &height);
        }
        if (width == 0 || width > boxColumns || height > boxRows) {
            ffSpinArtDestroy(&art);
            continue;
        }
        FFSpinFrame frame = {};
        if (ffSpinFrameInit(&frame, width, height)) {
            ffSpinRenderArt(&art, state->angle, wizardTiltRadians(state), &frame);
            uint32_t x = left + (boxColumns - width) / 2;
            for (uint32_t y = 0; y < height; ++y) {
                ffStrbufAppendF(out, "\e[%u;%uH", top + y, x);
                ffSpinAppendCells(out, &frame, &art, y * width, width, true, instance.config.display.brightColor);
            }
            ffSpinFrameDestroy(&frame);
        }
        ffSpinArtDestroy(&art);
        return height;
    }
    return 0;
}

static void wizardDrawHeader(FFstrbuf* out, uint32_t columns, uint32_t step, const char* title) {
    FF_STRBUF_AUTO_DESTROY line = ffStrbufCreateA(96);
    ffStrbufAppendF(&line, "spinfetch setup - step %u of 4 - %s", step, title);
    wizardLine(out, 1, columns, line.chars);
    wizardSeparator(out, 2, columns);
}

static void wizardDrawScreen(FFSpinWizardState* state) {
    uint32_t columns = wizardColumns();
    uint32_t rows = wizardRows();

    if (columns != state->lastColumns || rows != state->lastRows) {
        state->needsClear = true;
        state->lastColumns = columns;
        state->lastRows = rows;
    }

    FF_STRBUF_AUTO_DESTROY out = ffStrbufCreateA(8192);
    ffStrbufAppendS(&out, state->needsClear ? "\e[2J\e[H" : "\e[H");
    state->needsClear = false;

    if (columns < WIZARD_MIN_COLUMNS || rows < WIZARD_MIN_ROWS) {
        char line[96];
        snprintf(line, sizeof(line), "Terminal too small: need at least %ux%u cells.", WIZARD_MIN_COLUMNS, WIZARD_MIN_ROWS);
        wizardLine(&out, 1, columns, line);
        wizardLine(&out, 3, columns, "Resize the window, or press Esc to cancel.");
        ffStrbufAppendS(&out, "\e[0m");
        ffWriteFDBuffer(FFUnixFD2NativeFD(STDOUT_FILENO), &out);
        fflush(stdout);
        return;
    }

    char line[256];

    switch (state->screen) {
        case 0: {
            wizardDrawHeader(&out, columns, 1, "logo");
            const FFOSResult* os = ffDetectOS();
            const char* distro = os && os->name.length > 0
                ? os->name.chars
                : os && os->id.length > 0
                ? os->id.chars
                : "this system";
            snprintf(line, sizeof(line), "Detected: %s", distro);
            wizardLine(&out, 4, columns, line);
            wizardLine(&out, 6, columns, "Choose the logo that spins next to your system information:");

            const uint32_t listTop = 8, listWidth = 30;
            uint32_t listRows = rows > listTop + 1 ? rows - listTop - 1 : 1;
            uint32_t total = (uint32_t) WIZARD_LOGO_COUNT + 1;
            uint32_t offset = state->logoCursor >= listRows ? state->logoCursor - listRows + 1 : 0;
            for (uint32_t i = 0; i < listRows; ++i) {
                uint32_t index = offset + i;
                if (index >= total) {
                    wizardLine(&out, listTop + i, columns, "");
                } else if (index == 0) {
                    snprintf(line, sizeof(line), "%c Auto (detected: %s)", state->logoCursor == 0 ? '>' : ' ', state->detectedLogo);
                    wizardLine(&out, listTop + i, columns, line);
                } else {
                    snprintf(line, sizeof(line), "%c %s", state->logoCursor == index ? '>' : ' ', wizardLogoLabels[index - 1]);
                    wizardLine(&out, listTop + i, columns, line);
                }
            }
            if (columns > listWidth + 12) {
                wizardDrawPreview(&out, state, listTop, listWidth + 2, columns - listWidth - 2, listRows);
            }
            wizardLine(&out, rows, columns, "Up/Down choose   Enter next   Esc cancel");
            break;
        }
        case 1: {
            wizardDrawHeader(&out, columns, 2, "size");
            wizardLine(&out, 4, columns, "Pick a size. The preview spins while you decide.");

            const uint32_t top = 6;
            uint32_t boxRows = rows > top + 5 ? rows - top - 5 : 0;
            uint32_t used = wizardDrawPreview(&out, state, top, 1, columns, boxRows);

            FFSpinArt full = {}, small = {};
            bool hasFull = wizardLoadArt(state, FF_LOGO_VARIANT_FULL, &full);
            bool hasSmall = wizardLoadArt(state, FF_LOGO_VARIANT_MINIMAL, &small);
            bool sameSize = hasFull && hasSmall && full.width == small.width && full.height == small.height;
            uint32_t menuRow = top + used + 1;
            for (uint32_t i = 0; i < 2; ++i) {
                const FFSpinArt* art = i == 0 ? &full : &small;
                bool loaded = i == 0 ? hasFull : hasSmall;
                snprintf(line, sizeof(line), "%c %-6s %ux%u cells%s",
                    state->cursor == i ? '>' : ' ',
                    i == 0 ? "Full" : "Small",
                    loaded ? art->width : 0u,
                    loaded ? art->height : 0u,
                    i == 1 && sameSize ? " (this logo has no small version)" : "");
                wizardLine(&out, menuRow + i, columns, line);
            }
            ffSpinArtDestroy(&full);
            ffSpinArtDestroy(&small);
            if (used == 0) {
                wizardLine(&out, rows - 1, columns, "The terminal is too small to preview this logo.");
            }
            wizardLine(&out, rows, columns, "Up/Down or Left/Right size   Enter next   Esc cancel");
            break;
        }
        case 2: {
            wizardDrawHeader(&out, columns, 3, "motion and color");
            const char* labels[] = { "Speed", "FPS", "Tilt", "Accent", "SSH safe" };
            char values[5][32];
            snprintf(values[0], sizeof(values[0]), "%.1f rad/s", (double) wizardSpeeds[state->speedIndex]);
            snprintf(values[1], sizeof(values[1]), "%u fps", wizardFps[state->fpsIndex]);
            snprintf(values[2], sizeof(values[2]), "%d deg", wizardTilts[state->tiltIndex]);
            snprintf(values[3], sizeof(values[3]), "%s", wizardAccentNames[state->accentIndex]);
            snprintf(values[4], sizeof(values[4]), "%s", state->sshSafe ? "on (10 fps max)" : "off");

            for (uint32_t i = 0; i < 5; ++i) {
                snprintf(line, sizeof(line), "%c %-10s %s", state->cursor == i ? '>' : ' ', labels[i], values[i]);
                wizardLine(&out, 4 + i * 2, columns, line);
            }
            wizardLine(&out, 15, columns, "SSH safe caps the frame rate and is meant for remote sessions.");
            wizardLine(&out, rows, columns, "Up/Down choose   Left/Right change   Enter next   Esc cancel");
            break;
        }
        default: {
            wizardDrawHeader(&out, columns, 4, "modules");
            snprintf(line, sizeof(line), "Choose modules (%u selected). Space toggles.", wizardSelectedModuleCount(state));
            wizardLine(&out, 4, columns, line);

            uint32_t listTop = 6;
            uint32_t listRows = rows > listTop + 2 ? rows - listTop - 2 : 1;
            uint32_t offset = state->moduleCursor >= listRows ? state->moduleCursor - listRows + 1 : 0;
            for (uint32_t i = 0; i < listRows && offset + i < WIZARD_MODULE_COUNT; ++i) {
                uint32_t index = offset + i;
                snprintf(line, sizeof(line), "%c [%c] %s",
                    state->moduleCursor == index ? '>' : ' ',
                    state->modules[index] ? 'x' : ' ',
                    wizardModuleNames[index]);
                wizardLine(&out, listTop + i, columns, line);
            }

            if (state->confirming) {
                wizardLine(&out, rows - 1, columns, "Config exists. Overwrite it? Enter/y overwrite, n/Esc keep it.");
            } else if (state->status[0]) {
                wizardLine(&out, rows - 1, columns, state->status);
            } else {
                wizardLine(&out, rows - 1, columns, "Space toggle   Enter save   Esc cancel (nothing is written)");
            }
            wizardLine(&out, rows, columns, "The wizard only writes spinfetch/config.jsonc.");
            break;
        }
    }

    ffStrbufAppendS(&out, "\e[0m");
    ffWriteFDBuffer(FFUnixFD2NativeFD(STDOUT_FILENO), &out);
    fflush(stdout);
}

typedef enum WizardAction {
    WIZARD_ACTION_CONTINUE = 0,
    WIZARD_ACTION_EXIT,
    WIZARD_ACTION_SAVED,
} WizardAction;

static WizardAction wizardSave(FFSpinWizardState* state, const FFstrbuf* path, FFdata* data) {
    bool existed = ffPathExists(path->chars, FF_PATHTYPE_FILE);
    if (existed && !state->confirming) {
        state->confirming = true;
        state->needsClear = true;
        return WIZARD_ACTION_CONTINUE;
    }

    if (!wizardWriteConfig(state, path)) {
        snprintf(state->status, sizeof(state->status), "Could not write %s", path->chars);
        state->confirming = false;
        state->needsClear = true;
        return WIZARD_ACTION_CONTINUE;
    }

    wizardApply(state, data);
    state->confirming = false;
    return WIZARD_ACTION_SAVED;
}

static void wizardAdjust(FFSpinWizardState* state, int direction) {
    switch (state->cursor) {
        case 0:
            state->speedIndex = (uint32_t) ((int32_t) state->speedIndex + direction + (int32_t) ARRAY_SIZE(wizardSpeeds)) % (uint32_t) ARRAY_SIZE(wizardSpeeds);
            break;
        case 1:
            state->fpsIndex = (uint32_t) ((int32_t) state->fpsIndex + direction + (int32_t) ARRAY_SIZE(wizardFps)) % (uint32_t) ARRAY_SIZE(wizardFps);
            break;
        case 2:
            state->tiltIndex = (uint32_t) ((int32_t) state->tiltIndex + direction + (int32_t) ARRAY_SIZE(wizardTilts)) % (uint32_t) ARRAY_SIZE(wizardTilts);
            break;
        case 3:
            state->accentIndex = (uint32_t) ((int32_t) state->accentIndex + direction + (int32_t) WIZARD_ACCENT_COUNT) % (uint32_t) WIZARD_ACCENT_COUNT;
            break;
        case 4:
            state->sshSafe = !state->sshSafe;
            break;
        default:
            break;
    }
}

static WizardAction wizardHandleKey(FFSpinWizardState* state, FFSpinKey key, char ch, const FFstrbuf* path, FFdata* data) {
    if (key == FF_SPIN_KEY_ESCAPE || (key == FF_SPIN_KEY_CHAR && (ch == 3 || ch == 'q' || ch == 'Q'))) {
        return WIZARD_ACTION_EXIT;
    }

    if (state->confirming) {
        if (key == FF_SPIN_KEY_ENTER || (key == FF_SPIN_KEY_CHAR && (ch == 'y' || ch == 'Y'))) {
            return wizardSave(state, path, data);
        }
        state->confirming = false;
        state->needsClear = true;
        state->status[0] = '\0';
        return WIZARD_ACTION_CONTINUE;
    }

    switch (state->screen) {
        case 0:
            if (key == FF_SPIN_KEY_UP) {
                state->logoCursor = state->logoCursor == 0 ? (uint32_t) WIZARD_LOGO_COUNT : state->logoCursor - 1;
                state->needsClear = true;
            } else if (key == FF_SPIN_KEY_DOWN) {
                state->logoCursor = (state->logoCursor + 1) % ((uint32_t) WIZARD_LOGO_COUNT + 1);
                state->needsClear = true;
            } else if (key == FF_SPIN_KEY_ENTER) {
                state->screen = 1;
                state->cursor = state->variant == FF_LOGO_VARIANT_MINIMAL ? 1 : 0;
                state->needsClear = true;
            }
            break;
        case 1:
            if (key == FF_SPIN_KEY_UP || key == FF_SPIN_KEY_LEFT) {
                state->cursor = 0;
                state->variant = FF_LOGO_VARIANT_FULL;
                state->needsClear = true;
            } else if (key == FF_SPIN_KEY_DOWN || key == FF_SPIN_KEY_RIGHT) {
                state->cursor = 1;
                state->variant = FF_LOGO_VARIANT_MINIMAL;
                state->needsClear = true;
            } else if (key == FF_SPIN_KEY_ENTER) {
                state->screen = 2;
                state->cursor = 0;
                state->needsClear = true;
            }
            break;
        case 2:
            if (key == FF_SPIN_KEY_UP) {
                state->cursor = state->cursor == 0 ? 4 : state->cursor - 1;
            } else if (key == FF_SPIN_KEY_DOWN) {
                state->cursor = (state->cursor + 1) % 5;
            } else if (key == FF_SPIN_KEY_LEFT) {
                wizardAdjust(state, -1);
            } else if (key == FF_SPIN_KEY_RIGHT) {
                wizardAdjust(state, 1);
            } else if (key == FF_SPIN_KEY_ENTER) {
                state->screen = 3;
                state->cursor = 0;
                state->moduleCursor = 0;
                state->needsClear = true;
            } else if (key == FF_SPIN_KEY_SPACE && state->cursor == 4) {
                state->sshSafe = !state->sshSafe;
            }
            break;
        default:
            if (key == FF_SPIN_KEY_UP) {
                state->moduleCursor = state->moduleCursor == 0 ? WIZARD_MODULE_COUNT - 1 : state->moduleCursor - 1;
            } else if (key == FF_SPIN_KEY_DOWN) {
                state->moduleCursor = (state->moduleCursor + 1) % WIZARD_MODULE_COUNT;
            } else if (key == FF_SPIN_KEY_SPACE) {
                state->modules[state->moduleCursor] = !state->modules[state->moduleCursor];
                state->needsClear = true;
            } else if (key == FF_SPIN_KEY_ENTER) {
                if (wizardSelectedModuleCount(state) == 0) {
                    snprintf(state->status, sizeof(state->status), "Select at least one module before saving.");
                    state->needsClear = true;
                } else {
                    return wizardSave(state, path, data);
                }
            }
            break;
    }
    return WIZARD_ACTION_CONTINUE;
}

FFSpinWizardResult ffSpinWizardMaybeRun(FFdata* data) {
    const bool configure = instance.config.general.configure;
    const bool noWizard = instance.config.general.noWizard;
    const char* env = getenv("SPINFETCH_NO_WIZARD");
    bool envDisabled = env && env[0] != '\0' && !ffStrEqualsIgnCase(env, "0") && !ffStrEqualsIgnCase(env, "false");

    if (configure && noWizard) {
        fputs("Error: --configure cannot be combined with --no-wizard\n", stderr);
        return FF_SPIN_WIZARD_ERROR;
    }
    if (configure && envDisabled) {
        fputs("Error: --configure cannot be combined with SPINFETCH_NO_WIZARD\n", stderr);
        return FF_SPIN_WIZARD_ERROR;
    }
    if (configure && data->configNone) {
        fputs("Error: --configure cannot be combined with --config none\n", stderr);
        return FF_SPIN_WIZARD_ERROR;
    }

    bool quiet = data->genConfigPath.length > 0
        || data->resultDoc != nullptr
        || instance.config.display.pipe
        || noWizard
        || envDisabled;
    if (quiet) {
        if (configure) {
            fputs("Error: --configure requires an interactive terminal and cannot be used with --pipe, --format json, --gen-config or --no-wizard\n", stderr);
            return FF_SPIN_WIZARD_ERROR;
        }
        return FF_SPIN_WIZARD_SKIP;
    }

    if (!configure) {
        if (data->configLoaded || data->hadConfigFile) {
            return FF_SPIN_WIZARD_SKIP;
        }
        if (getenv("NO_CONFIG")) {
            return FF_SPIN_WIZARD_SKIP;
        }
        // Spin is the default type, so any other type or a source means the
        // user already chose a logo on the command line.
        if (instance.config.logo.type != FF_LOGO_TYPE_SPIN || instance.config.logo.source.length > 0) {
            return FF_SPIN_WIZARD_SKIP;
        }
    }

    if (!ffIsTerminal(STDIN_FILENO) || !ffIsTerminal(STDOUT_FILENO)) {
        if (configure) {
            fputs("Error: --configure requires an interactive terminal\n", stderr);
            return FF_SPIN_WIZARD_ERROR;
        }
        return FF_SPIN_WIZARD_SKIP;
    }

    FF_STRBUF_AUTO_DESTROY path = ffStrbufCreate();
    if (!wizardConfigPath(&path)) {
        if (configure) {
            fputs("Error: no configuration directory found for spinfetch/config.jsonc\n", stderr);
            return FF_SPIN_WIZARD_ERROR;
        }
        return FF_SPIN_WIZARD_SKIP;
    }

    FFSpinWizardState state = {};
    wizardSeed(&state, data);

    ffSpinTerminalEnter();
    ffSpinTerminalClear();

    bool running = true;
    bool saved = false;
    double last = ffTimeGetTick();
    while (running) {
        double now = ffTimeGetTick();
        if (state.screen <= 1) {
            state.angle += (float) ((now - last) / 1000.0) * 1.2f;
        }
        last = now;

        wizardDrawScreen(&state);

        FFSpinKey key = FF_SPIN_KEY_NONE;
        char ch = 0;
        if (ffSpinTerminalRead(&key, &ch, 50)) {
            WizardAction action = wizardHandleKey(&state, key, ch, &path, data);
            if (action == WIZARD_ACTION_SAVED) {
                saved = true;
                running = false;
            } else if (action == WIZARD_ACTION_EXIT) {
                running = false;
            }
        }
    }

    ffSpinTerminalLeave();

    if (saved) {
        if (configure) {
            printf("Configuration written to %s\n", path.chars);
        }
        return configure ? FF_SPIN_WIZARD_EXIT : FF_SPIN_WIZARD_CONTINUE;
    }

    // Cancelling the first-run wizard should still produce a normal fetch.
    // `--configure` is a dedicated action, so Esc exits without fetching.
    return configure ? FF_SPIN_WIZARD_EXIT : FF_SPIN_WIZARD_CONTINUE;
}
