# This adds module functionality to your keyboard (files found in users/halcyon_modules)
USER_NAME := halcyon_modules

# Fotos de Machi en la pantalla derecha
SRC += my_display.c \
       graphics/machi.c

# Efecto RGB propio (rgb_matrix_user.inc -> rgb_fx.c)
RGB_MATRIX_CUSTOM_USER = yes
SRC += rgb_fx.c
