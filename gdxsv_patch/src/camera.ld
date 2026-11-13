/* SPDX-FileCopyrightText: 2026 gdxsv contributors */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Guest code and data of the god's eye view, in guest RAM the payload leaves unused. */
SECTIONS
{
    . = 0x0C4E8000;
    gdx.cam.data : { *(gdx.cam.data) }
    . = 0x0C4F4000;
    gdx.cam.func ALIGN(16) : {
        *(gdx.cam.func)
        *(.text*)
        *(.rodata*)
    }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
