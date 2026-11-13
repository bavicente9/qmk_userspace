// Efecto RGB propio. Toda la lógica está en rgb_fx.c
RGB_MATRIX_EFFECT(halcyon_fx)

#ifdef RGB_MATRIX_CUSTOM_EFFECT_IMPLS
bool rgb_fx_render(effect_params_t *params);

static bool halcyon_fx(effect_params_t *params) {
    return rgb_fx_render(params);
}
#endif
