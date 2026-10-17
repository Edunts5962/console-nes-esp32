// audio_codec.cpp - initialisation du codec audio ES8311 de la carte.
//
// Bus I2C maitre (pilote i2c_master d'ESP-IDF, partage avec le tactile
// FT6336), puis configuration du codec par es8311.cpp : horloge MCLK
// (NES_AUDIO_SAMPLE_RATE x 256), format I2S 16 bits, volume materiel.
// L'amplificateur est active par setup() (AP_ENABLE, hw_config.h). Le flux
// audio lui-meme est envoye par osd.cpp (i2s_write()).
#include <Arduino.h>
#include "driver/gpio.h"
#include "driver/i2s.h"   // pour l'enum i2s_mclk_multiple_t / I2S_MCLK_MULTIPLE_256 (pilote I2S "legacy", comme osd.cpp)
#include "hw_config.h"
#include "es8311.h"

#define ES8311_MCLK_MULTIPLE   I2S_MCLK_MULTIPLE_256
#define ES8311_MCLK_FREQ_HZ    (NES_AUDIO_SAMPLE_RATE * ES8311_MCLK_MULTIPLE)
#define ES8311_VOICE_VOLUME    90   // 0..100 : volume materiel du codec, fixe au demarrage

static i2c_master_bus_handle_t s_bus_handle = NULL;
static i2c_master_dev_handle_t s_es8311_handle = NULL;

i2c_master_bus_handle_t audio_i2c_bus_handle(void) {
  return s_bus_handle;
}

static esp_err_t i2c_init(void) {
  i2c_master_bus_config_t i2c_cfg = {
    .i2c_port = I2C_NUM_0,
    .sda_io_num = (gpio_num_t)AUDIO_I2C_SDA,
    .scl_io_num = (gpio_num_t)AUDIO_I2C_SCL,
    .clk_source = I2C_CLK_SRC_DEFAULT,
    .glitch_ignore_cnt = 7,
    .flags = {
      .enable_internal_pullup = true,
    }
  };

  esp_err_t err = i2c_new_master_bus(&i2c_cfg, &s_bus_handle);
  if (err != ESP_OK) {
    Serial.printf("[AUDIO] Creation du bus I2C ECHOUEE: %s\n", esp_err_to_name(err));
    return err;
  }

  i2c_device_config_t dev_config = {
    .dev_addr_length = I2C_ADDR_BIT_LEN_7,
    .device_address = ES8311_ADDRRES_0,
    .scl_speed_hz = AUDIO_I2C_SPEED,
  };
  err = i2c_master_bus_add_device(s_bus_handle, &dev_config, &s_es8311_handle);
  if (err != ESP_OK) {
    Serial.printf("[AUDIO] Ajout du codec ES8311 sur le bus I2C ECHOUE: %s\n", esp_err_to_name(err));
    return err;
  }

  Serial.println("[AUDIO] Bus I2C pret (codec ES8311)");
  return ESP_OK;
}

esp_err_t audio_codec_init(void) {
  Serial.println("[AUDIO] Initialisation du codec ES8311...");

  esp_err_t err = i2c_init();
  if (err != ESP_OK) {
    return err;
  }

  const es8311_clock_config_t es_clk = {
    .mclk_inverted = false,
    .sclk_inverted = false,
    .mclk_from_mclk_pin = true,
    .mclk_frequency = ES8311_MCLK_FREQ_HZ,
    .sample_frequency = NES_AUDIO_SAMPLE_RATE
  };

  err = es8311_init(s_es8311_handle, &es_clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
  if (err != ESP_OK) {
    Serial.printf("[AUDIO] es8311_init ECHOUE: %s\n", esp_err_to_name(err));
    return err;
  }

  err = es8311_sample_frequency_config(s_es8311_handle, ES8311_MCLK_FREQ_HZ, NES_AUDIO_SAMPLE_RATE);
  if (err != ESP_OK) {
    Serial.printf("[AUDIO] Reglage frequence ES8311 ECHOUE: %s\n", esp_err_to_name(err));
    return err;
  }

  err = es8311_voice_volume_set(s_es8311_handle, ES8311_VOICE_VOLUME, NULL);
  if (err != ESP_OK) {
    Serial.printf("[AUDIO] Reglage volume ES8311 ECHOUE: %s\n", esp_err_to_name(err));
    return err;
  }

  err = es8311_microphone_config(s_es8311_handle, false);
  if (err != ESP_OK) {
    Serial.printf("[AUDIO] Desactivation micro ES8311 ECHOUEE: %s\n", esp_err_to_name(err));
    return err;
  }

  Serial.println("[AUDIO] Codec ES8311 pret");
  return ESP_OK;
}
