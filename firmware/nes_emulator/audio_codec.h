// audio_codec.h - initialisation du codec audio ES8311 par I2C.
//
// Sur cette carte, le son passe par un codec ES8311 qui doit etre configure
// par I2C (frequence d'echantillonnage, volume materiel, MCLK) avant que le
// flux I2S ait le moindre effet - contrairement au DAC I2S externe
// (MAX98357A) du depot d'origine.
//
// A appeler UNE SEULE FOIS dans setup(), AVANT osd_init_sound() (appelee
// par nofrendo_main() -> osd_init()) : sinon le son peut rester muet,
// deforme ou au mauvais volume.
#ifndef AUDIO_CODEC_H
#define AUDIO_CODEC_H

#include <esp_err.h>
#include "driver/i2c_master.h"

// Renvoie le bus I2C partage (cree par audio_codec_init()), pour que
// touch_controls.cpp puisse y attacher le tactile FT6336 EN PLUS du
// codec ES8311, sans creer un second bus sur les memes broches (ce que
// l'API i2c_master d'ESP-IDF refuse - un seul bus par port I2C materiel).
// Renvoie NULL si audio_codec_init() n'a pas encore ete appelee, ou si
// la creation du bus a elle-meme echoue.
i2c_master_bus_handle_t audio_i2c_bus_handle(void);

// Initialise le bus I2C, y attache le codec ES8311 et le configure pour
// la frequence d'echantillonnage utilisee par l'emulateur (16000 Hz -
// AUDIO_SAMPLE_RATE dans osd.cpp). Renvoie ESP_OK si tout s'est bien
// passe. En cas d'echec, le son restera muet mais le reste (ecran, SD,
// manette) continue de fonctionner normalement - ce n'est pas une
// erreur bloquante comme un echec de montage de la carte SD.
esp_err_t audio_codec_init(void);

#endif
