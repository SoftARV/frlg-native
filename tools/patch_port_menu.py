#!/usr/bin/env python3
"""Add the port's own entry to the pause menu.

Three insertions into the preprocessed copy of `start_menu.c`: an enum value, a
row in the action table, and one call in the builder. The screen behind it is
the port's own source in `platform/game/`, so this file's whole job is to make
that screen reachable -- see ADR 0022 for why a patch and not an override.

Each anchor is matched exactly and counted, so a submodule bump that moves any
of them fails the build here rather than quietly shipping a game with no way
into the port's options.

The builder anchor is the fiddly one: `AppendToStartMenuItems(STARTMENU_EXIT)`
appears three times, once per menu variant. Only the normal field offers SAVE,
which is what makes the three-line sequence unique to it -- the link and safari
menus must not grow a PORT entry they cannot return from cleanly.
"""
import argparse
import os
import sys

ENUM_OLD = """    STARTMENU_PLAYER2,
    MAX_STARTMENU_ITEMS
};"""

ENUM_NEW = """    STARTMENU_PLAYER2,
    STARTMENU_PORT,
    MAX_STARTMENU_ITEMS
};

extern const u8 agb_port_menu_label[];
extern bool8 agb_port_menu_open(void);"""

TABLE_OLD = """    [STARTMENU_PLAYER2] = { gText_MenuPlayer, {.u8_void = StartMenuLinkPlayerCallback} }
};"""

TABLE_NEW = """    [STARTMENU_PLAYER2] = { gText_MenuPlayer, {.u8_void = StartMenuLinkPlayerCallback} },
    [STARTMENU_PORT] = { agb_port_menu_label, {.u8_void = agb_port_menu_open} }
};"""

BUILDER_OLD = """    AppendToStartMenuItems(STARTMENU_SAVE);
    AppendToStartMenuItems(STARTMENU_OPTION);
    AppendToStartMenuItems(STARTMENU_EXIT);"""

BUILDER_NEW = """    AppendToStartMenuItems(STARTMENU_SAVE);
    AppendToStartMenuItems(STARTMENU_OPTION);
    AppendToStartMenuItems(STARTMENU_PORT);
    AppendToStartMenuItems(STARTMENU_EXIT);"""

REPLACEMENTS = {
    "start_menu.c": [
        ("enum value and the screen's declarations", ENUM_OLD, ENUM_NEW, 1),
        ("action table row", TABLE_OLD, TABLE_NEW, 1),
        ("normal field menu builder", BUILDER_OLD, BUILDER_NEW, 1),
    ],
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    args = ap.parse_args()

    text = open(args.file).read()
    base = os.path.basename(args.file)

    for what, old, new, expect in REPLACEMENTS.get(base, []):
        found = text.count(old)
        if found != expect:
            sys.exit(
                f"patch_port_menu: expected {expect} occurrence(s) of the "
                f"{what} in {args.file}, found {found}. Upstream has moved it; "
                "the pause menu would ship with no way into the port's options."
            )
        text = text.replace(old, new, expect)

    open(args.file, "w").write(text)


if __name__ == "__main__":
    main()
