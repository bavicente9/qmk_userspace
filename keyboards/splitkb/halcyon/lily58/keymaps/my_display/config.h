#pragma once

// RGB apagado a los 2 min sin tocar nada (igual que las pantallas)
#define RGB_MATRIX_TIMEOUT 120000

// Canal propio para que la mitad derecha use exactamente los mismos colores/ondas que la izquierda
#define SPLIT_TRANSACTION_IDS_USER FX_SYNC
// El estado del efecto (fondo + color de las teclas guía + ondas activas) ocupa 59 bytes
#define RPC_M2S_BUFFER_SIZE 64
