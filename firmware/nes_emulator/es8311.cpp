/*
 * SPDX-FileCopyrightText: 2015-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "es8311.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"

#include "es8311_reg.h"

// =============================================================================
// A PROPOS DE CE FICHIER (pilote du codec audio ES8311)
// =============================================================================
// Pilote bas niveau (registre par registre, via I2C) du codec audio ES8311
// de la carte, fourni par Espressif (en-tete SPDX ci-dessus, licence
// Apache-2.0). Le code est celui d'Espressif ; seuls des commentaires en
// francais ont ete ajoutes.
//
// Role : le codec ES8311 convertit le flux numerique I2S envoye par l'ESP32
// en signal analogique pour le haut-parleur (et inversement pour le micro,
// non utilise ici). Ce fichier se contente de configurer l'horloge, le
// format I2S et le volume materiel de la puce, une seule fois au demarrage.
//
// Utilise par audio_codec.cpp (audio_codec_init()) : es8311_init() (reset +
// horloge + format), es8311_sample_frequency_config(),
// es8311_voice_volume_set() (volume materiel de reference) et
// es8311_microphone_config() (micro non utilise, mais la puce a besoin d'une
// configuration coherente). Le volume du jeu est ensuite gere cote logiciel
// (osd.cpp), pas en rappelant ces fonctions.
//
// Plusieurs fonctions publiques (es8311_voice_mute, es8311_voice_volume_get,
// es8311_microphone_gain_set, es8311_voice_fade, es8311_microphone_fade,
// es8311_register_dump, es8311_create) ne sont pas appelees par ce projet ;
// elles sont documentees au cas ou elles deviendraient utiles.
// =============================================================================

// Structure "handle" alternative pour un chemin d'API different (creation
// dynamique via es8311_create(), voir plus bas). Cette structure et
// es8311_create()/es8311_dev_t restent presents dans le fichier pour
// compatibilite avec l'API d'origine d'Espressif, mais sont du code mort dans
// ce projet.
typedef struct {
    i2c_port_t port;
    uint16_t dev_addr;
} es8311_dev_t;

/*
 * Clock coefficient structure
 */
// Une ligne de cette structure decrit UN reglage materiel complet permettant
// d'obtenir une frequence d'echantillonnage ("rate", ex. 44100 Hz) a partir
// d'une frequence d'horloge maitre MCLK donnee ("mclk"), via une combinaison
// de diviseurs/multiplicateurs d'horloge internes a la puce ES8311. Ces
// valeurs proviennent directement de la documentation constructeur (datasheet
// ES8311) : ce ne sont pas des reglages "devinables", d'ou la table de
// correspondance ci-dessous plutot qu'un calcul.
struct _coeff_div {
    uint32_t mclk;        /* mclk frequency */
    uint32_t rate;        /* sample rate */
    uint8_t pre_div;      /* the pre divider with range from 1 to 8 */
    uint8_t pre_multi;    /* the pre multiplier with 0: 1x, 1: 2x, 2: 4x, 3: 8x selection */
    uint8_t adc_div;      /* adcclk divider */
    uint8_t dac_div;      /* dacclk divider */
    uint8_t fs_mode;      /* double speed or single speed, =0, ss, =1, ds */
    uint8_t lrck_h;       /* adclrck divider and daclrck divider */
    uint8_t lrck_l;
    uint8_t bclk_div;     /* sclk divider */
    uint8_t adc_osr;      /* adc osr */
    uint8_t dac_osr;      /* dac osr */
};

// Table de correspondance (MCLK, frequence d'echantillonnage) -> reglages
// materiels, issue de la datasheet ES8311. get_coeff() ci-dessous fait une
// recherche lineaire dedans : on evite ainsi tout calcul de diviseur "a la
// main", qui serait fragile et difficile a valider sans equipement de mesure.
/* codec hifi mclk clock divider coefficients */
static const struct _coeff_div coeff_div[] = {
    /*!<mclk     rate   pre_div  mult  adc_div dac_div fs_mode lrch  lrcl  bckdiv osr */
    /* 8k */
    {12288000, 8000, 0x06, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {18432000, 8000, 0x03, 0x01, 0x03, 0x03, 0x00, 0x05, 0xff, 0x18, 0x10, 0x10},
    {16384000, 8000, 0x08, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {8192000, 8000, 0x04, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {6144000, 8000, 0x03, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {4096000, 8000, 0x02, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {3072000, 8000, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {2048000, 8000, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {1536000, 8000, 0x03, 0x02, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {1024000, 8000, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},

    /* 11.025k */
    {11289600, 11025, 0x04, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {5644800, 11025, 0x02, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {2822400, 11025, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {1411200, 11025, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},

    /* 12k */
    {12288000, 12000, 0x04, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {6144000, 12000, 0x02, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {3072000, 12000, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {1536000, 12000, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},

    /* 16k */
    {12288000, 16000, 0x03, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {18432000, 16000, 0x03, 0x01, 0x03, 0x03, 0x00, 0x02, 0xff, 0x0c, 0x10, 0x10},
    {16384000, 16000, 0x04, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {8192000, 16000, 0x02, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {6144000, 16000, 0x03, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {4096000, 16000, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {3072000, 16000, 0x03, 0x02, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {2048000, 16000, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {1536000, 16000, 0x03, 0x03, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {1024000, 16000, 0x01, 0x02, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},

    /* 22.05k */
    {11289600, 22050, 0x02, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {5644800, 22050, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {2822400, 22050, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {1411200, 22050, 0x01, 0x02, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {705600, 22050, 0x01, 0x03, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},

    /* 24k */
    {12288000, 24000, 0x02, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {18432000, 24000, 0x03, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {6144000, 24000, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {3072000, 24000, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {1536000, 24000, 0x01, 0x02, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},

    /* 32k */
    {12288000, 32000, 0x03, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {18432000, 32000, 0x03, 0x02, 0x03, 0x03, 0x00, 0x02, 0xff, 0x0c, 0x10, 0x10},
    {16384000, 32000, 0x02, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {8192000, 32000, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {6144000, 32000, 0x03, 0x02, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {4096000, 32000, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {3072000, 32000, 0x03, 0x03, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {2048000, 32000, 0x01, 0x02, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {1536000, 32000, 0x03, 0x03, 0x01, 0x01, 0x01, 0x00, 0x7f, 0x02, 0x10, 0x10},
    {1024000, 32000, 0x01, 0x03, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},

    /* 44.1k */
    {11289600, 44100, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {5644800, 44100, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {2822400, 44100, 0x01, 0x02, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {1411200, 44100, 0x01, 0x03, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},

    /* 48k */
    {12288000, 48000, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {18432000, 48000, 0x03, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {6144000, 48000, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {3072000, 48000, 0x01, 0x02, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {1536000, 48000, 0x01, 0x03, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},

    /* 64k */
    {12288000, 64000, 0x03, 0x02, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {18432000, 64000, 0x03, 0x02, 0x03, 0x03, 0x01, 0x01, 0x7f, 0x06, 0x10, 0x10},
    {16384000, 64000, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {8192000, 64000, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {6144000, 64000, 0x01, 0x02, 0x03, 0x03, 0x01, 0x01, 0x7f, 0x06, 0x10, 0x10},
    {4096000, 64000, 0x01, 0x02, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {3072000, 64000, 0x01, 0x03, 0x03, 0x03, 0x01, 0x01, 0x7f, 0x06, 0x10, 0x10},
    {2048000, 64000, 0x01, 0x03, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {1536000, 64000, 0x01, 0x03, 0x01, 0x01, 0x01, 0x00, 0xbf, 0x03, 0x18, 0x18},
    {1024000, 64000, 0x01, 0x03, 0x01, 0x01, 0x01, 0x00, 0x7f, 0x02, 0x10, 0x10},

    /* 88.2k */
    {11289600, 88200, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {5644800, 88200, 0x01, 0x02, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {2822400, 88200, 0x01, 0x03, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {1411200, 88200, 0x01, 0x03, 0x01, 0x01, 0x01, 0x00, 0x7f, 0x02, 0x10, 0x10},

    /* 96k */
    {12288000, 96000, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {18432000, 96000, 0x03, 0x02, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {6144000, 96000, 0x01, 0x02, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {3072000, 96000, 0x01, 0x03, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {1536000, 96000, 0x01, 0x03, 0x01, 0x01, 0x01, 0x00, 0x7f, 0x02, 0x10, 0x10},
};

static const char *TAG = "ES8311"; // Prefixe utilise par les logs ESP_LOGx de ce fichier

// Ecrit UN octet dans UN registre de l'ES8311. Trame I2C envoyee :
// [adresse_registre, valeur]. C'est la brique de base utilisee par toutes
// les fonctions de configuration ci-dessous (chaque reglage de la puce
// passe par une ou plusieurs ecritures de registre comme celle-ci).
static inline esp_err_t es8311_write_reg(i2c_master_dev_handle_t dev, uint8_t reg_addr, uint8_t data)
{
    const uint8_t write_buf[2] = {reg_addr, data};
    return i2c_master_transmit(dev, write_buf, sizeof(write_buf), pdMS_TO_TICKS(1000));
}

// Lit UN octet depuis UN registre de l'ES8311 : ecrit d'abord l'adresse du
// registre voulu, puis relit un octet en reponse (transaction I2C combinee
// ecriture+lecture). Utilise avant les ecritures "lire-modifier-ecrire" qui
// ne doivent changer que certains bits d'un registre sans toucher aux autres
// (ex. es8311_clock_config()).
static inline esp_err_t es8311_read_reg(i2c_master_dev_handle_t dev, uint8_t reg_addr, uint8_t *reg_value)
{
    return i2c_master_transmit_receive(dev, &reg_addr, 1, reg_value, 1, pdMS_TO_TICKS(1000));
}

/*
 * look for the coefficient in coeff_div[] table
 */
// Recherche lineaire dans coeff_div[] (defini plus haut) de la ligne
// correspondant exactement au couple (mclk, rate) demande. Renvoie l'index
// dans le tableau, ou -1 si aucune combinaison connue ne correspond (ex.
// frequence d'echantillonnage non standard) - dans ce cas
// es8311_sample_frequency_config() renverra ESP_ERR_INVALID_ARG.
static int get_coeff(uint32_t mclk, uint32_t rate)
{
    for (int i = 0; i < (sizeof(coeff_div) / sizeof(coeff_div[0])); i++) {
        if (coeff_div[i].rate == rate && coeff_div[i].mclk == mclk) {
            return i;
        }
    }

    return -1;
}

// Configure les diviseurs d'horloge internes (registres 0x02 a 0x08) pour
// obtenir "sample_frequency" a partir de "mclk_frequency", en utilisant la
// ligne correspondante de coeff_div[]. Chaque bloc "/* register 0xXX */"
// ci-dessous lit (si necessaire), modifie certains bits, puis reecrit un
// registre donne - c'est le motif "lire-modifier-ecrire" classique en
// programmation de registres, utilise pour ne pas perturber les autres bits
// deja configures.
esp_err_t es8311_sample_frequency_config(i2c_master_dev_handle_t dev, int mclk_frequency, int sample_frequency)
{
    uint8_t regv;

    /* Get clock coefficients from coefficient table */
    int coeff = get_coeff(mclk_frequency, sample_frequency);

    if (coeff < 0) {
        ESP_LOGE(TAG, "Unable to configure sample rate %dHz with %dHz MCLK", sample_frequency, mclk_frequency);
        return ESP_ERR_INVALID_ARG;
    }

    const struct _coeff_div *const selected_coeff = &coeff_div[coeff];

    /* register 0x02 */
    ESP_RETURN_ON_ERROR(es8311_read_reg(dev, ES8311_CLK_MANAGER_REG02, &regv), TAG, "I2C read/write error");
    regv &= 0x07;
    regv |= (selected_coeff->pre_div - 1) << 5;
    regv |= selected_coeff->pre_multi << 3;
    ESP_RETURN_ON_ERROR(es8311_write_reg(dev, ES8311_CLK_MANAGER_REG02, regv), TAG, "I2C read/write error");

    /* register 0x03 */
    const uint8_t reg03 = (selected_coeff->fs_mode << 6) | selected_coeff->adc_osr;
    ESP_RETURN_ON_ERROR(es8311_write_reg(dev, ES8311_CLK_MANAGER_REG03, reg03), TAG, "I2C read/write error");

    /* register 0x04 */
    ESP_RETURN_ON_ERROR(es8311_write_reg(dev, ES8311_CLK_MANAGER_REG04, selected_coeff->dac_osr), TAG, "I2C read/write error");

    /* register 0x05 */
    const uint8_t reg05 = ((selected_coeff->adc_div - 1) << 4) | (selected_coeff->dac_div - 1);
    ESP_RETURN_ON_ERROR(es8311_write_reg(dev, ES8311_CLK_MANAGER_REG05, reg05), TAG, "I2C read/write error");

    /* register 0x06 */
    ESP_RETURN_ON_ERROR(es8311_read_reg(dev, ES8311_CLK_MANAGER_REG06, &regv), TAG, "I2C read/write error");
    regv &= 0xE0;

    if (selected_coeff->bclk_div < 19) {
        regv |= (selected_coeff->bclk_div - 1) << 0;
    } else {
        regv |= (selected_coeff->bclk_div) << 0;
    }

    ESP_RETURN_ON_ERROR(es8311_write_reg(dev, ES8311_CLK_MANAGER_REG06, regv), TAG, "I2C read/write error");

    /* register 0x07 */
    ESP_RETURN_ON_ERROR(es8311_read_reg(dev, ES8311_CLK_MANAGER_REG07, &regv), TAG, "I2C read/write error");
    regv &= 0xC0;
    regv |= selected_coeff->lrck_h << 0;
    ESP_RETURN_ON_ERROR(es8311_write_reg(dev, ES8311_CLK_MANAGER_REG07, regv), TAG, "I2C read/write error");

    /* register 0x08 */
    ESP_RETURN_ON_ERROR(es8311_write_reg(dev, ES8311_CLK_MANAGER_REG08, selected_coeff->lrck_l), TAG, "I2C read/write error");

    return ESP_OK;
}

// Choisit la SOURCE de l'horloge maitre MCLK (broche MCLK dediee, ou derivee
// de la broche SCLK/BCLK de l'I2S) et sa polarite, puis delegue le calcul des
// diviseurs a es8311_sample_frequency_config().
static esp_err_t es8311_clock_config(i2c_master_dev_handle_t dev, const es8311_clock_config_t *const clk_cfg, es8311_resolution_t res)
{
    uint8_t reg06;
    uint8_t reg01 = 0x3F; // Enable all clocks
    int mclk_hz;

    /* Select clock source for internal MCLK and determine its frequency */
    if (clk_cfg->mclk_from_mclk_pin) {
        mclk_hz = clk_cfg->mclk_frequency;
    } else {
        mclk_hz = clk_cfg->sample_frequency * (int)res * 2;
        reg01 |= BIT(7); // Select BCLK (a.k.a. SCK) pin
    }

    if (clk_cfg->mclk_inverted) {
        reg01 |= BIT(6); // Invert MCLK pin
    }
    ESP_RETURN_ON_ERROR(es8311_write_reg(dev, ES8311_CLK_MANAGER_REG01, reg01), TAG, "I2C read/write error");

    ESP_RETURN_ON_ERROR(es8311_read_reg(dev, ES8311_CLK_MANAGER_REG06, &reg06), TAG, "I2C read/write error");
    if (clk_cfg->sclk_inverted) {
        reg06 |= BIT(5);
    } else {
        reg06 &= ~BIT(5);
    }
    ESP_RETURN_ON_ERROR(es8311_write_reg(dev, ES8311_CLK_MANAGER_REG06, reg06), TAG, "I2C read/write error");

    /* Configure clock dividers */
    return es8311_sample_frequency_config(dev, mclk_hz, clk_cfg->sample_frequency);
}

// Traduit une resolution audio (16/18/20/24/32 bits, enum es8311_resolution_t)
// en la valeur binaire attendue par les registres de format serie I2S de
// l'ES8311 (champ de 3 bits, decale de 2 positions - d'ou "<< 2"). Fonction
// interne (static), appelee par es8311_fmt_config() pour l'entree ET la sortie
// serie.
static esp_err_t es8311_resolution_config(const es8311_resolution_t res, uint8_t *reg)
{
    switch (res) {
    case ES8311_RESOLUTION_16:
        *reg |= (3 << 2);
        break;
    case ES8311_RESOLUTION_18:
        *reg |= (2 << 2);
        break;
    case ES8311_RESOLUTION_20:
        *reg |= (1 << 2);
        break;
    case ES8311_RESOLUTION_24:
        *reg |= (0 << 2);
        break;
    case ES8311_RESOLUTION_32:
        *reg |= (4 << 2);
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

// Configure le format du port serie I2S : force l'ES8311 en mode ESCLAVE, puis
// fixe la resolution des voies d'entree (microphone, non utilisee ici) et de
// sortie via es8311_resolution_config(). Fonction interne (static), appelee
// uniquement par es8311_init().
static esp_err_t es8311_fmt_config(i2c_master_dev_handle_t dev, const es8311_resolution_t res_in, const es8311_resolution_t res_out)
{
    uint8_t reg09 = 0; // SDP In
    uint8_t reg0a = 0; // SDP Out

    ESP_LOGI(TAG, "ES8311 in Slave mode and I2S format");
    uint8_t reg00;
    ESP_RETURN_ON_ERROR(es8311_read_reg(dev, ES8311_RESET_REG00, &reg00), TAG, "I2C read/write error");
    reg00 &= 0xBF;
    ESP_RETURN_ON_ERROR(es8311_write_reg(dev, ES8311_RESET_REG00, reg00), TAG, "I2C read/write error"); // Slave serial port - default

    /* Setup SDP In and Out resolution */
    es8311_resolution_config(res_in, &reg09);
    es8311_resolution_config(res_out, &reg0a);

    ESP_RETURN_ON_ERROR(es8311_write_reg(dev, ES8311_SDPIN_REG09, reg09), TAG, "I2C read/write error");
    ESP_RETURN_ON_ERROR(es8311_write_reg(dev, ES8311_SDPOUT_REG0A, reg0a), TAG, "I2C read/write error");

    return ESP_OK;
}

// Configure la voie microphone de l'ES8311 (analogique ou numerique PDM selon
// "digital_mic").
esp_err_t es8311_microphone_config(i2c_master_dev_handle_t dev, bool digital_mic)
{
    uint8_t reg14 = 0x1A; // enable analog MIC and max PGA gain

    /* PDM digital microphone enable or disable */
    if (digital_mic) {
        reg14 |= BIT(6);
    }
    es8311_write_reg(dev, ES8311_ADC_REG17, 0xA8); // Set ADC gain @todo move this to ADC config section

    return es8311_write_reg(dev, ES8311_SYSTEM_REG14, reg14);
}

// Point d'entree PRINCIPAL du pilote, appele en premier par
// audio_codec_init(). Deroulement :
//   1) Verifie que la frequence d'echantillonnage demandee est dans la
//      plage supportee par la puce (8 kHz - 96 kHz).
//   2) Reinitialise completement la puce (sequence reset registre 0x00 :
//      0x1F -> pause -> 0x00 -> 0x80, qui correspond a "mise en reset" puis
//      "sortie de reset avec alimentation activee" selon la datasheet).
//   3) Configure l'horloge (es8311_clock_config -> choix de la source MCLK
//      et calcul des diviseurs via la table coeff_div[]).
//   4) Configure le format serie I2S (es8311_fmt_config -> mode esclave,
//      resolution 16 bits pour ce projet).
//   5) Ecrit une sequence de registres "power up" (mise sous tension des
//      etages analogiques/DAC) : ce sont des valeurs NON PAR DEFAUT propres
//      a cette sequence de demarrage, deja documentees ligne par ligne par
//      Espressif via les commentaires "NOT default" ci-dessous - laissees
//      telles quelles, ce sont des reglages materiels valides par le
//      fabricant qu'il ne faut pas modifier sans datasheet sous les yeux.
esp_err_t es8311_init(i2c_master_dev_handle_t dev, const es8311_clock_config_t *const clk_cfg, const es8311_resolution_t res_in, const es8311_resolution_t res_out)
{
    ESP_RETURN_ON_FALSE(
        (clk_cfg->sample_frequency >= 8000) && (clk_cfg->sample_frequency <= 96000),
        ESP_ERR_INVALID_ARG, TAG, "ES8311 init needs frequency in interval [8000; 96000] Hz"
    );
    if (!clk_cfg->mclk_from_mclk_pin) {
        ESP_RETURN_ON_FALSE(res_out == res_in, ESP_ERR_INVALID_ARG, TAG, "Resolution IN/OUT must be equal if MCLK is taken from SCK pin");
    }


    /* Reset ES8311 to its default */
    ESP_RETURN_ON_ERROR(es8311_write_reg(dev, ES8311_RESET_REG00, 0x1F), TAG, "I2C read/write error");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(es8311_write_reg(dev, ES8311_RESET_REG00, 0x00), TAG, "I2C read/write error");
    ESP_RETURN_ON_ERROR(es8311_write_reg(dev, ES8311_RESET_REG00, 0x80), TAG, "I2C read/write error"); // Power-on command

    /* Setup clock: source, polarity and clock dividers */
    ESP_RETURN_ON_ERROR(es8311_clock_config(dev, clk_cfg, res_out), TAG, "");

    /* Setup audio format (fmt): master/slave, resolution, I2S */
    ESP_RETURN_ON_ERROR(es8311_fmt_config(dev, res_in, res_out), TAG, "");

    ESP_RETURN_ON_ERROR(es8311_write_reg(dev, ES8311_SYSTEM_REG0D, 0x01), TAG, "I2C read/write error"); // Power up analog circuitry - NOT default
    ESP_RETURN_ON_ERROR(es8311_write_reg(dev, ES8311_SYSTEM_REG0E, 0x02), TAG, "I2C read/write error"); // Enable analog PGA, enable ADC modulator - NOT default
    ESP_RETURN_ON_ERROR(es8311_write_reg(dev, ES8311_SYSTEM_REG12, 0x00), TAG, "I2C read/write error"); // power-up DAC - NOT default
    ESP_RETURN_ON_ERROR(es8311_write_reg(dev, ES8311_SYSTEM_REG13, 0x10), TAG, "I2C read/write error"); // Enable output to HP drive - NOT default
    ESP_RETURN_ON_ERROR(es8311_write_reg(dev, ES8311_ADC_REG1C, 0x6A), TAG, "I2C read/write error"); // ADC Equalizer bypass, cancel DC offset in digital domain
    ESP_RETURN_ON_ERROR(es8311_write_reg(dev, ES8311_DAC_REG37, 0x08), TAG, "I2C read/write error"); // Bypass DAC equalizer - NOT default

    return ESP_OK;
}

// Libere proprement le device I2C et le bus I2C associes a l'ES8311. Fournie
// pour completude de l'API / si un jour une mise en veille est ajoutee. Le "//
// free(dev)" en commentaire est un reliquat du code d'origine Espressif (le
// pointeur "dev" ici est un handle ESP-IDF, pas la structure es8311_dev_t du
// chemin d'API alternatif plus haut - il n'y a donc rien a liberer avec free()
// sur ce chemin).
void es8311_delete(i2c_master_bus_handle_t bus,i2c_master_dev_handle_t dev)
{
    ESP_ERROR_CHECK(i2c_master_bus_rm_device(dev));
    ESP_ERROR_CHECK(i2c_del_master_bus(bus));
   // free(dev);
}

// Fixe le volume de sortie MATERIEL (registre 0x32 du DAC), sur une echelle
// 0-100 mappee lineairement sur les 256 crans du registre. Appelee UNE SEULE
// FOIS par ce projet, au demarrage, pour fixer un niveau de reference materiel
// raisonnable.
esp_err_t es8311_voice_volume_set(i2c_master_dev_handle_t dev, int volume, int *volume_set)
{
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }

    int reg32;
    if (volume == 0) {
        reg32 = 0;
    } else {
        reg32 = ((volume) * 256 / 100) - 1;
    }

    // provide user with real volume set
    if (volume_set != NULL) {
        *volume_set = volume;
    }
    return es8311_write_reg(dev, ES8311_DAC_REG32, reg32);
}

// Lit le volume materiel actuel (operation inverse de
// es8311_voice_volume_set() ci-dessus). Non appelee par ce projet.
esp_err_t es8311_voice_volume_get(i2c_master_dev_handle_t dev, int *volume)
{
    uint8_t reg32;
    ESP_RETURN_ON_ERROR(es8311_read_reg(dev, ES8311_DAC_REG32, &reg32), TAG, "I2C read/write error");

    if (reg32 == 0) {
        *volume = 0;
    } else {
        *volume = ((reg32 * 100) / 256) + 1;
    }
    return ESP_OK;
}

// Coupe ou retablit la sortie audio DIRECTEMENT au niveau materiel (bits de
// mute du registre 0x31 du DAC), independamment du volume. Non appelee par
// ce projet.
esp_err_t es8311_voice_mute(i2c_master_dev_handle_t dev, bool mute)
{
    uint8_t reg31;
    ESP_RETURN_ON_ERROR(es8311_read_reg(dev, ES8311_DAC_REG31, &reg31), TAG, "I2C read/write error");

    if (mute) {
        reg31 |= BIT(6) | BIT(5);
    } else {
        reg31 &= ~(BIT(6) | BIT(5));
    }

    return es8311_write_reg(dev, ES8311_DAC_REG31, reg31);
}

// Regle le gain d'entree du microphone (registre 0x16). NON APPELEE par ce
// projet, qui n'utilise pas le micro. Disponible si une fonctionnalite
// d'enregistrement/micro etait ajoutee un jour.
esp_err_t es8311_microphone_gain_set(i2c_master_dev_handle_t dev, es8311_mic_gain_t gain_db)
{
    return es8311_write_reg(dev, ES8311_ADC_REG16, gain_db); // ADC gain scale up
}

// Regle la vitesse de "fondu" (ramp) materiel applique par le DAC lors des
// changements de volume/mute, pour eviter les "clics" audio. NON APPELEE par
// ce projet (le registre garde sa valeur par defaut de la puce). Disponible
// si un fondu plus doux est souhaite a l'avenir.
esp_err_t es8311_voice_fade(i2c_master_dev_handle_t dev, const es8311_fade_t fade)
{
    uint8_t reg37;
    ESP_RETURN_ON_ERROR(es8311_read_reg(dev, ES8311_DAC_REG37, &reg37), TAG, "I2C read/write error");
    reg37 &= 0x0F;
    reg37 |= (fade << 4);
    return es8311_write_reg(dev, ES8311_DAC_REG37, reg37);
}

// Equivalent de es8311_voice_fade() ci-dessus, mais pour la voie
// microphone (ADC). NON APPELEE par ce projet, pour la meme raison (pas
// d'utilisation du micro).
esp_err_t es8311_microphone_fade(i2c_master_dev_handle_t dev, const es8311_fade_t fade)
{
    uint8_t reg15;
    ESP_RETURN_ON_ERROR(es8311_read_reg(dev, ES8311_ADC_REG15, &reg15), TAG, "I2C read/write error");
    reg15 &= 0x0F;
    reg15 |= (fade << 4);
    return es8311_write_reg(dev, ES8311_ADC_REG15, reg15);
}

// Utilitaire de DEBOGAGE : relit et affiche (via printf) le contenu de tous
// les registres de la puce (0x00 a 0x49), un par ligne. NON APPELEE par ce
// projet en fonctionnement normal - utile ponctuellement, en la commentant
// dans le code d'un cran plus haut, pour verifier "a la main" qu'une sequence
// d'initialisation a bien ecrit ce qui etait attendu.
void es8311_register_dump(i2c_master_dev_handle_t dev)
{
    for (int reg = 0; reg < 0x4A; reg++) {
        uint8_t value;
        ESP_ERROR_CHECK(es8311_read_reg(dev, reg, &value));
        printf("REG:%02x: %02x", reg, value);
    }
}

// Chemin d'API ALTERNATIF (non utilise par ce projet, voir la remarque sur
// es8311_dev_t en haut du fichier) : alloue dynamiquement un handle
// "es8311_dev_t" a partir d'un simple port I2C et d'une adresse.
es8311_handle_t es8311_create(const i2c_port_t port, const uint16_t dev_addr)
{
    es8311_dev_t *sensor = (es8311_dev_t *) calloc(1, sizeof(es8311_dev_t));
    sensor->port = port;
    sensor->dev_addr = dev_addr;
    return (es8311_handle_t) sensor;
}

