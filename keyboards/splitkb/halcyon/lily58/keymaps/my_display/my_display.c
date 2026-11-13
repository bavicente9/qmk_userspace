// Pantalla derecha (secundaria): fotos de Machi, una al azar cada 10 s.
// La pantalla izquierda sigue con lo de serie de splitkb (número de capa, Caps/Num/Scroll).
// Las fotos se regeneran con: python tools/img2qp.py <keymap>/graphics/machi <keymap>/graphics/src/*.png

#include <stdlib.h>
#include QMK_KEYBOARD_H
#include "halcyon.h"
#include "hlc_tft_display/hlc_tft_display.h"
#include "graphics/machi.h"

#define MACHI_INTERVAL 10000 // ms entre fotos

// Definida en hlc_tft_display.c (ruido del oscilador del RP2040)
uint32_t get_random_32bit(void);

static void draw_machi(uint8_t idx) {
    qp_viewport(lcd_surface, 0, 0, GFX_MACHI_WIDTH - 1, GFX_MACHI_HEIGHT - 1);
    qp_pixdata(lcd_surface, gfx_machi[idx], GFX_MACHI_WIDTH * GFX_MACHI_HEIGHT);
}

bool display_module_housekeeping_task_user(bool second_display) {
    if (!second_display) {
        return true; // pantalla principal: dibujo de serie
    }

    static uint8_t  current   = 0xFF;
    static uint32_t last_swap = 0;

    if (current == 0xFF) {
        srand(get_random_32bit());
        current   = rand() % GFX_MACHI_COUNT;
        draw_machi(current);
        last_swap = timer_read32();
    } else if (timer_elapsed32(last_swap) >= MACHI_INTERVAL) {
        last_swap = timer_read32();
        if (is_backlight_enabled() && GFX_MACHI_COUNT > 1) { // con la pantalla apagada no se cambia
            uint8_t next = rand() % (GFX_MACHI_COUNT - 1);
            if (next >= current) next++; // nunca la misma dos veces seguidas
            current = next;
            draw_machi(current);
        }
    }

    qp_surface_draw(lcd_surface, lcd, 0, 0, 0);
    qp_flush(lcd);
    return false; // sustituye al juego de la vida de serie
}
