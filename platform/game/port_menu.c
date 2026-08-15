// The port's own options, reached from the pause menu.
//
// Game-layer code the port owns (ADR 0022). It is written against the game's
// public helpers rather than copied from `option_menu.c`: the decompilation is
// a submodule this repository fetches and never redistributes, and lifting its
// scaffolding in here would change that.
//
// What it lists is whatever display stages are registered (host_render.h), so
// adding a filter adds a line here without this file knowing about it. What it
// writes is per save (host_options.h), because SaveBlock2 belongs to the
// cartridge and a display mode is not worth spending its bits on.

#include "global.h"
#include "gflib.h"
#include "menu.h"
#include "task.h"
#include "text_window.h"
#include "new_menu_helpers.h"
#include "overworld.h"
#include "field_fadetransition.h"
#include "field_weather.h"
#include "constants/songs.h"

#include "host_options.h"
#include "host_render.h"

// Ours rather than the option menu's: that one's colours are static to its own
// translation unit, and reaching for them would be borrowing a private thing.
static const u8 sTextColour[3] = {
    TEXT_COLOR_TRANSPARENT, TEXT_COLOR_DARK_GRAY, TEXT_COLOR_LIGHT_GRAY,
};

// STD_WINDOW_PALETTE_NUM in new_menu_helpers.c, which is static to it.
#define STD_FRAME_PALETTE 14

#define ROW_HEIGHT 16
#define MAX_ROWS 6

// The pause-menu entry. Our own string: every other word on that menu came off
// the cartridge, this one is the port's.
const u8 agb_port_menu_label[] = _("PORT");

// Shown in the pause menu's help bar while PORT is highlighted. Not optional:
// the description table is indexed positionally by the menu enum, and an entry
// without one is read past the end of the array.
const u8 agb_port_menu_desc[] = _("Settings this port adds.");

static const u8 sHeading[] = _("PORT OPTIONS");
static const u8 sOn[] = _("ON");
static const u8 sOff[] = _("OFF");
static const u8 sNothing[] = _("Nothing to configure yet.");
static const u8 sFooter[] = _("B: back");

static const struct BgTemplate sBgTemplates[] = {
    {
        .bg = 0,
        .charBaseIndex = 2,
        .mapBaseIndex = 31,
        .screenSize = 0,
        .paletteMode = 0,
        .priority = 0,
        .baseTile = 0,
    },
};

static const struct WindowTemplate sWindows[] = {
    {
        .bg = 0,
        .tilemapLeft = 1,
        .tilemapTop = 1,
        .width = 28,
        .height = 17,
        .paletteNum = STD_FRAME_PALETTE,
        .baseBlock = 16,
    },
    DUMMY_WIN_TEMPLATE,
};

struct PortMenu
{
    u8 rows[MAX_ROWS];  // stage ids, in the order they are shown
    u8 count;
    u8 cursor;
    u8 state;
};

static EWRAM_DATA struct PortMenu *sMenu = NULL;
static EWRAM_DATA u8 *sTilemap = NULL;

static void Task_PortMenu(u8 taskId);

static void CollectRows(void)
{
    int id;

    sMenu->count = 0;
    for (id = 0; id < host_render_count() && sMenu->count < MAX_ROWS; id++)
    {
        if (host_render_name(id) != NULL)
            sMenu->rows[sMenu->count++] = id;
    }
}

// The stage's name is a C string from the host layer, not game text, so it is
// copied through the game's own conversion rather than printed directly.
static void PrintAscii(u8 window, const char *text, u8 x, u8 y, u8 colour)
{
    u8 buffer[32];
    u8 i = 0;

    while (text[i] != '\0' && i < (u8)(sizeof(buffer) - 1))
    {
        u8 c = (u8)text[i];

        if (c >= 'a' && c <= 'z')
            buffer[i] = CHAR_a + (c - 'a');
        else if (c >= 'A' && c <= 'Z')
            buffer[i] = CHAR_A + (c - 'A');
        else if (c >= '0' && c <= '9')
            buffer[i] = CHAR_0 + (c - '0');
        else
            buffer[i] = CHAR_SPACE;
        i++;
    }
    buffer[i] = EOS;
    AddTextPrinterParameterized3(window, FONT_NORMAL, x, y, sTextColour,
                                 TEXT_SKIP_DRAW, buffer);
    (void)colour;
}

static void DrawRows(void)
{
    u8 i;

    FillWindowPixelBuffer(0, PIXEL_FILL(1));
    AddTextPrinterParameterized3(0, FONT_NORMAL, 8, 1, sTextColour,
                                 TEXT_SKIP_DRAW, sHeading);

    if (sMenu->count == 0)
    {
        AddTextPrinterParameterized3(0, FONT_NORMAL, 8, 20, sTextColour,
                                     TEXT_SKIP_DRAW, sNothing);
    }

    for (i = 0; i < sMenu->count; i++)
    {
        u8 y = 20 + i * ROW_HEIGHT;
        int id = sMenu->rows[i];

        PrintAscii(0, host_render_name(id), 16, y, 1);
        AddTextPrinterParameterized3(0, FONT_NORMAL, 168, y, sTextColour,
                                     TEXT_SKIP_DRAW,
                                     host_render_is_enabled(id) ? sOn : sOff);
    }

    AddTextPrinterParameterized3(0, FONT_NORMAL, 8, 20 + MAX_ROWS * ROW_HEIGHT,
                                 sTextColour, TEXT_SKIP_DRAW, sFooter);

    // The cursor is drawn as a character rather than a sprite: one glyph, no
    // OAM to set up or tear down.
    if (sMenu->count != 0)
    {
        static const u8 arrow[] = _("{RIGHT_ARROW}");

        AddTextPrinterParameterized3(0, FONT_NORMAL, 4,
                                     20 + sMenu->cursor * ROW_HEIGHT,
                                     sTextColour, TEXT_SKIP_DRAW, arrow);
    }

    CopyWindowToVram(0, COPYWIN_GFX);
}

static void Toggle(void)
{
    int id;

    if (sMenu->count == 0)
        return;

    id = sMenu->rows[sMenu->cursor];
    if (host_render_is_retired(id))
        return;

    host_render_set_enabled(id, !host_render_is_enabled(id));

    // Written under a key of the stage's own name, so the file says what it
    // means and a stage that goes away does not take another's setting with it.
    host_option_set("screen-filter", host_render_is_enabled(id));
    host_options_flush();
}

static void Task_PortMenu(u8 taskId)
{
    if (gPaletteFade.active)
        return;

    if (JOY_NEW(B_BUTTON))
    {
        BeginNormalPaletteFade(PALETTES_ALL, 0, 0, 16, RGB_BLACK);
        gTasks[taskId].func = NULL;
        DestroyTask(taskId);
        SetMainCallback2(gMain.savedCallback);
        return;
    }

    if (sMenu->count == 0)
        return;

    if (JOY_NEW(DPAD_UP))
    {
        sMenu->cursor = sMenu->cursor == 0 ? sMenu->count - 1 : sMenu->cursor - 1;
        PlaySE(SE_SELECT);
        DrawRows();
    }
    else if (JOY_NEW(DPAD_DOWN))
    {
        sMenu->cursor = (sMenu->cursor + 1) % sMenu->count;
        PlaySE(SE_SELECT);
        DrawRows();
    }
    else if (JOY_NEW(A_BUTTON) || JOY_NEW(DPAD_LEFT) || JOY_NEW(DPAD_RIGHT))
    {
        Toggle();
        PlaySE(SE_SELECT);
        DrawRows();
    }
}

static void VBlankCB(void)
{
    LoadOam();
    ProcessSpriteCopyRequests();
    TransferPlttBuffer();
}

static void MainCB(void)
{
    RunTasks();
    AnimateSprites();
    BuildOamBuffer();
    UpdatePaletteFade();
}

static void CB2_PortMenu(void)
{
    switch (sMenu->state)
    {
    case 0:
        SetVBlankCallback(NULL);
        SetHBlankCallback(NULL);
        break;
    case 1:
        ResetBgsAndClearDma3BusyFlags(0);
        InitBgsFromTemplates(0, sBgTemplates, NELEMS(sBgTemplates));
        sTilemap = AllocZeroed(BG_SCREEN_SIZE);
        SetBgTilemapBuffer(0, sTilemap);
        break;
    case 2:
        ResetPaletteFade();
        ResetSpriteData();
        FreeAllSpritePalettes();
        break;
    case 3:
        InitWindows(sWindows);
        DeactivateAllTextPrinters();
        LoadStdWindowFrameGfx();
        break;
    case 4:
        // Palette 14 to match the frame's own, so the border and what is inside
        // it are not drawn from two different sets of colours.
        FillBgTilemapBufferRect(0, 0, 0, 0, 32, 32, STD_FRAME_PALETTE);
        DrawStdWindowFrame(0, FALSE);
        CopyBgTilemapBufferToVram(0);
        break;
    case 5:
        CollectRows();
        DrawRows();
        CopyWindowToVram(0, COPYWIN_FULL);
        break;
    case 6:
        SetGpuReg(REG_OFFSET_DISPCNT, DISPCNT_MODE_0 | DISPCNT_OBJ_ON
                                      | DISPCNT_OBJ_1D_MAP);
        ShowBg(0);
        BeginNormalPaletteFade(PALETTES_ALL, 0, 16, 0, RGB_BLACK);
        SetVBlankCallback(VBlankCB);
        break;
    default:
        CreateTask(Task_PortMenu, 0);
        SetMainCallback2(MainCB);
        return;
    }
    sMenu->state++;
}

// Entered from the pause menu's action table, patched in by
// tools/patch_port_menu.py. Same shape as the game's own entries: refuse while
// a fade is running, tidy the overworld away, and leave savedCallback pointing
// at the way back.
bool8 agb_port_menu_open(void)
{
    if (gPaletteFade.active)
        return FALSE;

    PlayRainStoppingSoundEffect();
    CleanupOverworldWindowsAndTilemaps();

    sMenu = AllocZeroed(sizeof(*sMenu));
    sMenu->state = 0;
    sMenu->cursor = 0;
    sMenu->count = 0;

    gMain.savedCallback = CB2_ReturnToFieldWithOpenMenu;
    SetMainCallback2(CB2_PortMenu);
    return TRUE;
}
