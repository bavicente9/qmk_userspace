// Efecto RGB del Lily58 (las dos mitades).
//
// - Fondo (teclas + underglow): cambia al azar cada 5 min con un fundido suave. Fondos:
//   arcoíris que se desplaza, ciclo de color, remolino desde el centro de cada mitad y
//   degradado lento entre 2 colores al azar.
// - Al pulsar una tecla sale una onda de color desde ella, de un color distinto cada vez.
// - Teclas guía: WASD en QWERTY, AOEU + HTNS en Dvorak. Se buscan por keycode en la capa por
//   defecto, así que siguen al keymap. Respiran suave (nunca bajan a 0) con un color propio que
//   se elige al azar cada vez que cambias de layout, siempre bien distinto del anterior.
//   El fondo y las ondas no las tocan.
// - Capas 2, 3 y 4 (mientras las mantienes): fondo atenuado y solo se encienden las teclas que
//   hacen algo, con color según el tipo. Las XXXXXXX quedan apagadas.
//   Capa 4: QK_BOOT late en rojo, las de brillo se ven al brillo actual, RM_NEXT cambia de fondo.
// - Se apaga a los 2 min sin uso (RGB_MATRIX_TIMEOUT en config.h).
//
// Todo lo aleatorio lo decide la mitad con el USB (master) y se lo manda a la otra por FX_SYNC,
// así las dos mitades pintan exactamente lo mismo.

#include QMK_KEYBOARD_H
#include <string.h>
#include "transactions.h"
#include "split_util.h"
#include "sync_timer.h"
#include <lib/lib8tion/lib8tion.h>

// Definida en hlc_tft_display.c (ruido del oscilador del RP2040)
uint32_t get_random_32bit(void);

#define FX_BG_COUNT     4
#define FX_BG_PERIOD    300000 // ms entre cambios de fondo (5 min)
#define FX_BG_FADE      3000   // ms de fundido entre fondos
#define FX_SPLASH_MAX   8      // ondas a la vez
#define FX_SPLASH_LIFE  900    // ms que dura una onda
#define FX_SPLASH_WIDTH 24     // grosor del anillo de la onda
#define FX_SYNC_EVERY   500    // reenvío periódico por si la otra mitad se reinicia
#define FX_DIM          48     // brillo del fondo en las capas 2-4 (de 255)
#define FX_NO_SPLASH    0xFF
#define FX_NO_POS       0xFF

enum { BG_RAINBOW, BG_CYCLE, BG_SWIRL, BG_GRADIENT };

typedef struct __attribute__((packed)) {
    uint8_t  led;   // LED donde empezó (FX_NO_SPLASH = libre)
    uint8_t  hue;
    uint32_t start; // sync_timer, igual en las dos mitades
} fx_splash_t;

typedef struct __attribute__((packed)) {
    uint8_t     bg, bg_prev;    // fondo actual y el anterior (para el fundido)
    uint8_t     bg_a, bg_b;     // colores del degradado del fondo actual
    uint8_t     prev_a, prev_b; // colores del degradado del fondo anterior
    uint32_t    bg_start;       // cuándo empezó el fondo actual
    uint8_t     guide_hue;      // color de las teclas guía
    fx_splash_t splash[FX_SPLASH_MAX];
} fx_state_t;

_Static_assert(sizeof(fx_state_t) <= RPC_M2S_BUFFER_SIZE, "fx_state_t no cabe en RPC_M2S_BUFFER_SIZE");

// Tipos de tecla para las capas 2-4
enum {
    KT_TRANS,  // _______ : hace lo de la capa de abajo, se ve el fondo atenuado
    KT_NONE,   // XXXXXXX : apagada
    KT_ARROW,
    KT_ACCENT, // RALT(...) : á é í ó ú ñ
    KT_NAV,    // Home/End/Del/PgUp/PgDn/Ins
    KT_FKEY,
    KT_NUMBER,
    KT_SYMBOL,
    KT_LAYER,  // MO/DF/TG...
    KT_MOD,
    KT_BOOT,
    KT_RGB_VAL,
    KT_RGB_HUE,
    KT_RGB_SAT,
    KT_RGB_MODE,
    KT_RGB_TOGG,
    KT_OTHER,
};

static fx_state_t fx;
static bool       fx_dirty = false;
static uint32_t   fx_rng   = 0x9E3779B9;

static uint8_t led_row[RGB_MATRIX_LED_COUNT];
static uint8_t led_col[RGB_MATRIX_LED_COUNT];
static uint8_t key_type[RGB_MATRIX_LED_COUNT];
static bool    guide[RGB_MATRIX_LED_COUNT];
static uint8_t cached_default = 0xFF;
static uint8_t cached_top     = 0xFF;

static const uint16_t guide_qwerty[] = {KC_W, KC_A, KC_S, KC_D};
static const uint16_t guide_dvorak[] = {KC_A, KC_O, KC_E, KC_U, KC_H, KC_T, KC_N, KC_S};

// ---------------------------------------------------------------- aleatorio (solo master)

static uint32_t fx_random(void) {
    fx_rng ^= fx_rng << 13;
    fx_rng ^= fx_rng >> 17;
    fx_rng ^= fx_rng << 5;
    return fx_rng;
}

// Un tono a 90°-270° del dado: claramente distinto
static uint8_t far_hue(uint8_t from) {
    return from + 64 + (fx_random() % 128);
}

static void fx_pick_background(bool fade) {
    uint32_t now = sync_timer_read32();
    fx.bg_prev   = fx.bg;
    fx.prev_a    = fx.bg_a;
    fx.prev_b    = fx.bg_b;
    uint8_t next = fx_random() % (FX_BG_COUNT - 1);
    if (next >= fx.bg) next++; // nunca el mismo
    fx.bg       = next;
    fx.bg_a     = fx_random();
    fx.bg_b     = far_hue(fx.bg_a);
    fx.bg_start = fade ? now : now - FX_BG_FADE;
    fx_dirty    = true;
}

static void fx_keypress(uint8_t row, uint8_t col) {
    if (row >= MATRIX_ROWS || col >= MATRIX_COLS) return;
    uint8_t led = g_led_config.matrix_co[row][col];
    if (led >= RGB_MATRIX_LED_COUNT) return; // NO_LED

    static uint8_t slot = 0;
    static uint8_t hue  = 0;
    hue += 40 + (fx_random() % 80); // siempre distinto del anterior
    fx.splash[slot].led   = led;
    fx.splash[slot].hue   = hue;
    fx.splash[slot].start = sync_timer_read32();
    slot                  = (slot + 1) % FX_SPLASH_MAX;
    fx_dirty              = true;
}

// ---------------------------------------------------------------- sincronización entre mitades

static void fx_sync_slave(uint8_t in_len, const void *in_buf, uint8_t out_len, void *out_buf) {
    if (in_len == sizeof(fx)) {
        memcpy(&fx, in_buf, sizeof(fx));
    }
}

// ---------------------------------------------------------------- tablas por capa

static uint8_t classify(uint16_t kc) {
    if (kc == KC_TRNS) return KT_TRANS;
    if (kc == KC_NO) return KT_NONE;
    if (kc >= KC_RIGHT && kc <= KC_UP) return KT_ARROW;
    if (IS_QK_MODS(kc)) return QK_MODS_GET_MODS(kc) == MOD_RALT ? KT_ACCENT : KT_SYMBOL;
    if ((kc >= KC_F1 && kc <= KC_F12) || (kc >= KC_F13 && kc <= KC_F24)) return KT_FKEY;
    if (kc >= KC_1 && kc <= KC_0) return KT_NUMBER;
    if (kc >= KC_INSERT && kc <= KC_PAGE_DOWN) return KT_NAV;
    if (kc >= KC_MINUS && kc <= KC_SLASH) return KT_SYMBOL;
    if (kc >= KC_LEFT_CTRL && kc <= KC_RIGHT_GUI) return KT_MOD;
    switch (kc) {
        case QK_BOOT: return KT_BOOT;
        case RM_VALU:
        case RM_VALD: return KT_RGB_VAL;
        case RM_HUEU:
        case RM_HUED: return KT_RGB_HUE;
        case RM_SATU:
        case RM_SATD: return KT_RGB_SAT;
        case RM_NEXT:
        case RM_PREV: return KT_RGB_MODE;
        case RM_TOGG: return KT_RGB_TOGG;
    }
    if (IS_QK_MOMENTARY(kc) || IS_QK_DEF_LAYER(kc) || IS_QK_TO(kc) || IS_QK_TOGGLE_LAYER(kc) || IS_QK_LAYER_TAP(kc) || IS_QK_ONE_SHOT_LAYER(kc) || IS_QK_LAYER_MOD(kc) || IS_QK_LAYER_TAP_TOGGLE(kc)) return KT_LAYER;
    return KT_OTHER;
}

static void fx_update_caches(void) {
    uint8_t dl  = get_highest_layer(default_layer_state);
    uint8_t top = get_highest_layer(layer_state | default_layer_state);

    if (dl != cached_default) {
        const uint16_t *set = NULL;
        uint8_t         n   = 0;
        if (dl == 0) {
            set = guide_qwerty;
            n   = sizeof(guide_qwerty) / sizeof(guide_qwerty[0]);
        } else if (dl == 1) {
            set = guide_dvorak;
            n   = sizeof(guide_dvorak) / sizeof(guide_dvorak[0]);
        }
        for (uint8_t i = 0; i < RGB_MATRIX_LED_COUNT; i++) {
            guide[i] = false;
            if (led_row[i] == FX_NO_POS) continue;
            uint16_t kc = keymap_key_to_keycode(dl, MAKE_KEYPOS(led_row[i], led_col[i]));
            for (uint8_t k = 0; k < n; k++) {
                if (kc == set[k]) guide[i] = true;
            }
        }
        cached_default = dl;
    }

    if (top != cached_top) {
        for (uint8_t i = 0; i < RGB_MATRIX_LED_COUNT; i++) {
            key_type[i] = led_row[i] == FX_NO_POS ? KT_TRANS : classify(keymap_key_to_keycode(top, MAKE_KEYPOS(led_row[i], led_col[i])));
        }
        cached_top = top;
    }
}

// ---------------------------------------------------------------- colores

static rgb_t rgb_mix(rgb_t a, rgb_t b, uint8_t amount_of_b) {
    return (rgb_t){lerp8by8(a.r, b.r, amount_of_b), lerp8by8(a.g, b.g, amount_of_b), lerp8by8(a.b, b.b, amount_of_b)};
}

static rgb_t rgb_scale(rgb_t c, uint8_t s) {
    return (rgb_t){scale8(c.r, s), scale8(c.g, s), scale8(c.b, s)};
}

static rgb_t hsv(uint8_t h, uint8_t s, uint8_t v) {
    return hsv_to_rgb((hsv_t){h, s, v});
}

// Fase 0-255 de las animaciones del fondo. Usa la velocidad de RGB (por defecto ~15 s por vuelta).
static uint8_t fx_time8(uint32_t now) {
    return (uint8_t)((now * (uint32_t)(rgb_matrix_config.speed + 16)) >> 13);
}

static uint8_t bg_hue(uint8_t bg, uint8_t a, uint8_t b, uint8_t i, uint8_t t) {
    int16_t x = g_led_config.point[i].x;
    int16_t y = g_led_config.point[i].y;
    switch (bg) {
        case BG_RAINBOW:
            return (uint8_t)(x - t);
        case BG_CYCLE:
            return t;
        case BG_SWIRL: {
            bool    right = x >= 112;
            int16_t dx    = x - (right ? 181 : 43);
            int16_t dy    = y - 32;
            if (right) dx = -dx; // espejo: las dos mitades giran igual
            uint8_t dist = sqrt16((uint16_t)(dx * dx + dy * dy));
            return atan2_8(dy, dx) + dist * 2 - t * 2;
        }
        default: { // BG_GRADIENT: ola lenta entre los dos colores
            uint8_t m    = sin8((uint8_t)(x / 2 + t));
            int8_t  diff = (int8_t)(uint8_t)(b - a);
            return a + (int8_t)(((int16_t)diff * m) / 255);
        }
    }
}

static rgb_t bg_color(uint8_t i, uint8_t t, uint32_t now) {
    uint8_t h = rgb_matrix_config.hsv.h; // RM_HUE desplaza todos los colores del fondo
    uint8_t s = rgb_matrix_config.hsv.s;
    uint8_t v = rgb_matrix_config.hsv.v;
    rgb_t   c = hsv(bg_hue(fx.bg, fx.bg_a, fx.bg_b, i, t) + h, s, v);

    int32_t el = (int32_t)(now - fx.bg_start);
    if (el < 0) el = 0;
    if (el < FX_BG_FADE) {
        rgb_t p = hsv(bg_hue(fx.bg_prev, fx.prev_a, fx.prev_b, i, t) + h, s, v);
        c       = rgb_mix(p, c, ease8InOutCubic((uint8_t)(el * 255 / FX_BG_FADE)));
    }
    return c;
}

static rgb_t apply_splashes(rgb_t c, uint8_t i, uint32_t now) {
    uint8_t v = rgb_matrix_config.hsv.v;
    for (uint8_t s = 0; s < FX_SPLASH_MAX; s++) {
        const fx_splash_t *sp = &fx.splash[s];
        if (sp->led >= RGB_MATRIX_LED_COUNT) continue;
        int32_t el = (int32_t)(now - sp->start);
        if (el < 0) el = 0;
        if (el >= FX_SPLASH_LIFE) continue;

        int16_t dx     = g_led_config.point[i].x - g_led_config.point[sp->led].x;
        int16_t dy     = g_led_config.point[i].y - g_led_config.point[sp->led].y;
        int16_t dist   = sqrt16((uint16_t)(dx * dx + dy * dy));
        int16_t radius = el / 4; // cruza el teclado entero en la vida de la onda
        int16_t gap    = dist > radius ? dist - radius : radius - dist;
        if (gap >= FX_SPLASH_WIDTH) continue;

        uint8_t k = 255 - (gap * 255) / FX_SPLASH_WIDTH;
        k         = scale8(k, 255 - (uint8_t)((el * 255) / FX_SPLASH_LIFE)); // se apaga al expandirse
        c         = rgb_mix(c, hsv(sp->hue, 255, v), k);
    }
    return c;
}

// Respiración: ~2,5 s por ciclo, entre el 25 % y el 100 %
static uint8_t breath(uint32_t now, uint8_t v) {
    uint8_t phase = (uint8_t)((now * 105u) >> 10);
    return scale8(v, 64 + scale8(sin8(phase), 191));
}

static rgb_t layer_color(uint8_t i, uint8_t t, uint32_t now) {
    uint8_t v = rgb_matrix_config.hsv.v;
    switch (key_type[i]) {
        case KT_NONE:
            return (rgb_t){0, 0, 0};
        case KT_ARROW:
            return hsv(128, 255, v); // cian
        case KT_ACCENT:
            return hsv(213, 255, v); // magenta
        case KT_NAV:
            return hsv(21, 255, v); // naranja
        case KT_FKEY:
            return hsv(43, 255, v); // amarillo
        case KT_NUMBER:
            return hsv(85, 255, v); // verde
        case KT_SYMBOL:
            return hsv(170, 255, v); // azul
        case KT_LAYER:
        case KT_OTHER:
        case KT_RGB_VAL: // blanco al brillo actual: se ve directamente cuánto brillo hay
            return (rgb_t){v, v, v};
        case KT_MOD:
        case KT_RGB_TOGG:
            return (rgb_t){v / 3, v / 3, v / 3};
        case KT_BOOT: {
            uint8_t phase = (uint8_t)((now * 205u) >> 10); // ~1,2 s por latido
            return hsv(0, 255, scale8(v, 40 + scale8(sin8(phase), 215)));
        }
        case KT_RGB_HUE:
            return hsv(rgb_matrix_config.hsv.h, 255, v);
        case KT_RGB_SAT:
            return hsv(rgb_matrix_config.hsv.h, rgb_matrix_config.hsv.s, v);
        case KT_RGB_MODE:
            return bg_color(i, t, now); // muestra el fondo actual a brillo completo
        default: // KT_TRANS y underglow: fondo atenuado (las ondas siguen pasando por aquí)
            return apply_splashes(rgb_scale(bg_color(i, t, now), FX_DIM), i, now);
    }
}

// ---------------------------------------------------------------- efecto (rgb_matrix_user.inc)

bool rgb_fx_render(effect_params_t *params) {
    RGB_MATRIX_USE_LIMITS(led_min, led_max);
    fx_update_caches();

    uint32_t now        = g_rgb_timer;
    uint8_t  t          = fx_time8(now);
    bool     layer_view = cached_top > cached_default;
    rgb_t    guide_rgb  = hsv(fx.guide_hue, 255, breath(now, rgb_matrix_config.hsv.v));

    for (uint8_t i = led_min; i < led_max; i++) {
        RGB_MATRIX_TEST_LED_FLAGS();
        rgb_t c;
        if (layer_view) {
            c = layer_color(i, t, now);
        } else if (guide[i]) {
            c = guide_rgb;
        } else {
            c = apply_splashes(bg_color(i, t, now), i, now);
        }
        rgb_matrix_set_color(i, c.r, c.g, c.b);
    }
    return rgb_matrix_check_finished_leds(led_max);
}

// ---------------------------------------------------------------- hooks de QMK

void keyboard_post_init_user(void) {
    for (uint8_t i = 0; i < RGB_MATRIX_LED_COUNT; i++) {
        led_row[i] = FX_NO_POS;
        led_col[i] = FX_NO_POS;
    }
    for (uint8_t r = 0; r < MATRIX_ROWS; r++) {
        for (uint8_t c = 0; c < MATRIX_COLS; c++) {
            uint8_t led = g_led_config.matrix_co[r][c];
            if (led < RGB_MATRIX_LED_COUNT) {
                led_row[led] = r;
                led_col[led] = c;
            }
        }
    }
    for (uint8_t s = 0; s < FX_SPLASH_MAX; s++) {
        fx.splash[s].led = FX_NO_SPLASH;
    }

    transaction_register_rpc(FX_SYNC, fx_sync_slave);

    if (is_keyboard_master()) {
        fx_rng = get_random_32bit() | 1; // xorshift no puede empezar en 0
        fx.bg  = fx_random() % FX_BG_COUNT;
        fx_pick_background(false); // elige otro distinto al azar, sin fundido
        fx.guide_hue = fx_random();
        rgb_matrix_mode_noeeprom(RGB_MATRIX_CUSTOM_halcyon_fx);
    }
}

void housekeeping_task_user(void) {
    if (!is_keyboard_master()) return;

    // Cambio de layout (DF) -> color nuevo para las teclas guía
    static uint8_t last_default = 0xFF;
    uint8_t        dl           = get_highest_layer(default_layer_state);
    if (dl != last_default) {
        if (last_default != 0xFF) {
            fx.guide_hue = far_hue(fx.guide_hue);
            fx_dirty     = true;
        }
        last_default = dl;
    }

    // Fondo nuevo cada 5 min
    if (sync_timer_read32() - fx.bg_start >= FX_BG_PERIOD) {
        fx_pick_background(true);
    }

    // Mandar el estado a la otra mitad
    static uint32_t last_sync = 0;
    if ((fx_dirty || timer_elapsed32(last_sync) >= FX_SYNC_EVERY) && is_transport_connected()) {
        if (transaction_rpc_send(FX_SYNC, sizeof(fx), &fx)) {
            fx_dirty = false;
        }
        last_sync = timer_read32();
    }
}

bool process_record_user(uint16_t keycode, keyrecord_t *record) {
    if (record->event.pressed && IS_KEYEVENT(record->event)) {
        fx_keypress(record->event.key.row, record->event.key.col);
    }
    switch (keycode) {
        case RM_NEXT:
        case RM_PREV:
            if (record->event.pressed) {
                fx_pick_background(true); // cambia el fondo del efecto en vez del modo de RGB
            }
            return false;
    }
    return true;
}
