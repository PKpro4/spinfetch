#include "logo/spin/spin.h"

#include "common/io.h"
#include "common/strutil.h"
#include "detection/terminalsize/terminalsize.h"
#include "logo/logo.h"
#include "logo/spin/render.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
    #define M_PI 3.14159265358979323846
#endif

static FFSpinAnimationInfo animationInfo;
static FFSpinArt art;

static void terminalSize(uint32_t* columns, uint32_t* rows) {
    *columns = *rows = 0;
    if (ffIsTerminal(STDOUT_FILENO)) {
        FFTerminalSizeResult size = {};
        if (ffDetectTerminalSize(&size, true)) {
            *columns = size.columns;
            *rows = size.rows;
        }
    }
}

static bool spinIsAuto(const FFstrbuf* source) {
    return source->length == 0 || ffStrbufIgnCaseEqualS(source, "auto");
}

static const FFlogo* spinBuiltin(const FFstrbuf* source, FFLogoSize size) {
    if (spinIsAuto(source)) {
        const FFlogo* logo = ffLogoGetBuiltinDetected(size);
        // Detection returns the "?" logo when there is no small version.
        return size == FF_LOGO_SIZE_SMALL && logo == &ffLogoUnknown ? nullptr : logo;
    }
    return ffLogoGetBuiltinForName(source, size);
}

static bool spinLoadFile(const FFstrbuf* source, FFstrbuf* text) {
    FF_STRBUF_AUTO_DESTROY path = ffStrbufCreate();
    if (ffPathExpandEnv(source->chars, &path) && ffAppendFileBuffer(path.chars, text)) {
        return text->length > 0;
    }
    return ffAppendFileBuffer(source->chars, text) && text->length > 0;
}

// The logo's own colors, with any configured `logo.color.N` on top.
static bool spinParse(FFSpinArt* out, const char* text, const FFlogo* logo, const FFstrbuf* overrides) {
    FFstrbuf colors[FASTFETCH_LOGO_MAX_COLORS];
    for (uint32_t i = 0; i < FASTFETCH_LOGO_MAX_COLORS; ++i) {
        if (overrides && overrides[i].length > 0) {
            ffStrbufInitStatic(&colors[i], overrides[i].chars);
        } else {
            ffStrbufInitStatic(&colors[i], logo && logo->colors[i] ? logo->colors[i] : "");
        }
    }
    return ffSpinArtParse(out, text, colors, FASTFETCH_LOGO_MAX_COLORS);
}

bool ffSpinLoadPreviewArt(const FFstrbuf* source, FFLogoVariant variant, FFSpinArt* out) {
    const FFlogo* logo = variant == FF_LOGO_VARIANT_MINIMAL ? spinBuiltin(source, FF_LOGO_SIZE_SMALL) : nullptr;
    if (!logo) logo = spinBuiltin(source, FF_LOGO_SIZE_NORMAL);
    if (!logo) logo = spinBuiltin(source, FF_LOGO_SIZE_UNKNOWN);
    if (!logo) return false;
    if (spinParse(out, logo->lines, logo, nullptr)) {
        return true;
    }
    ffSpinArtDestroy(out);
    return false;
}

void ffSpinLogoPrint(void) {
    FFOptionsLogo* options = &instance.config.logo;
    animationInfo = (FFSpinAnimationInfo) { 0 };
    instance.state.logoWidth = 0;
    instance.state.logoHeight = 0;
    ffSpinArtDestroy(&art);

    // The source is the detected OS ("" or "auto"), a builtin logo name, or a
    // text file with fastfetch logo syntax.
    FF_STRBUF_AUTO_DESTROY fileText = ffStrbufCreate();
    const FFlogo* full = spinBuiltin(&options->source, FF_LOGO_SIZE_NORMAL);
    if (!full) full = spinBuiltin(&options->source, FF_LOGO_SIZE_UNKNOWN);
    const FFlogo* small = full ? spinBuiltin(&options->source, FF_LOGO_SIZE_SMALL) : nullptr;
    if (!full && !spinLoadFile(&options->source, &fileText)) {
        if (instance.config.display.showErrors) {
            fprintf(stderr, "Logo (spin): no builtin logo or readable file named %s\n", options->source.chars);
        }
        full = ffLogoGetBuiltinDetected(FF_LOGO_SIZE_NORMAL);
    }
    // File logos borrow the detected logo's colors, as fastfetch does.
    ffLogoApplyColors(full ? full : ffLogoGetBuiltinDetected(FF_LOGO_SIZE_NORMAL), true);

    const char* fullText = full ? full->lines : fileText.chars;
    const char* smallText = small && small != full ? small->lines : nullptr;
    const char* candidates[2] = { fullText, smallText };
    if (options->variant == FF_LOGO_VARIANT_MINIMAL && smallText) {
        candidates[0] = smallText;
        candidates[1] = nullptr;
    }

    const bool animate = ffSpinLogoAnimationWanted();
    const float tilt = options->spin.tilt * (float) M_PI / 180.0f;
    uint32_t columns, rows;
    terminalSize(&columns, &rows);
    bool sideBySide = options->position == FF_LOGO_POSITION_LEFT || options->position == FF_LOGO_POSITION_AUTO;

    uint32_t width = 0, height = 0;
    bool chosen = false;
    for (uint32_t i = 0; i < 2 && !chosen; ++i) {
        if (!candidates[i] || !spinParse(&art, candidates[i], nullptr, options->colors)) {
            ffSpinArtDestroy(&art);
            continue;
        }
        width = art.width;
        height = art.height;
        if (animate) {
            ffSpinFrameSize(&art, tilt, &width, &height);
        }
        uint32_t neededColumns = width + options->paddingLeft + options->paddingRight + (sideBySide ? FF_LOGO_AUTO_MIN_INFO_WIDTH : 0);
        chosen = (columns == 0 || columns >= neededColumns) && (rows == 0 || rows >= height + options->paddingTop + 1);
        if (!chosen) {
            ffSpinArtDestroy(&art);
        }
    }
    if (!chosen) {
        // No room for the logo. The caller still prints its modules.
        return;
    }
    if (sideBySide && options->position == FF_LOGO_POSITION_AUTO && columns > 0) {
        options->position = FF_LOGO_POSITION_LEFT;
    }

    // The printed logo is the art exactly as authored; the animation starts
    // from it. The frame keeps room around it for the turning slab.
    FFSpinFrame frame = {};
    if (!ffSpinFrameInit(&frame, width, height)) {
        ffSpinArtDestroy(&art);
        return;
    }
    ffSpinFramePutArt(&frame, &art);
    FF_STRBUF_AUTO_DESTROY ansi = ffStrbufCreateA(width * height * 4);
    ffSpinFrameToAnsi(&frame, &art, !instance.config.display.pipe, instance.config.display.brightColor, &ansi);
    ffLogoPrintChars(ansi.chars, false);
    ffSpinFrameDestroy(&frame);

    uint32_t fps = options->spin.fps ? options->spin.fps : 20;
    if (options->spin.sshSafe && fps > 10) {
        fps = 10;
    }

    animationInfo = (FFSpinAnimationInfo) {
        .active = true,
        .top = 1 + options->paddingTop,
        .left = 1 + options->paddingLeft,
        .width = width,
        .height = height,
        .speed = options->spin.speed,
        .tilt = tilt,
        .fps = fps,
        .frames = options->spin.frames,
    };
}

bool ffSpinLogoAnimationWanted(void) {
    const FFOptionsLogo* options = &instance.config.logo;
    return options->type == FF_LOGO_TYPE_SPIN &&
        !instance.config.display.pipe &&
        ffIsTerminal(STDOUT_FILENO) &&
        ffIsTerminal(STDIN_FILENO) &&
        options->position != FF_LOGO_POSITION_RIGHT;
}

bool ffSpinLogoAnimationActive(void) {
    return animationInfo.active;
}

const FFSpinAnimationInfo* ffSpinGetAnimationInfo(void) {
    return &animationInfo;
}

const FFSpinArt* ffSpinGetArt(void) {
    return &art;
}
