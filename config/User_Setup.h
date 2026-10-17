// User_Setup.h pour la bibliotheque TFT_eSPI - carte LCDWIKI ES3C28P
// (ESP32-S3, ecran ILI9341 2,8" 320x240 en SPI).
//
// Copier ce fichier a la place de Arduino/libraries/TFT_eSPI/User_Setup.h
// (garder une copie de l'original). Teste avec TFT_eSPI 2.5.43.

#define USER_SETUP_INFO "ES3C28P"

#define ILI9341_DRIVER

// Retro-eclairage : pilote en PWM par le croquis (LCD_BL_PIN, hw_config.h)
#define TFT_BL   45
#define TFT_BACKLIGHT_ON HIGH

// Bus SPI de l'ecran
#define TFT_MISO 13
#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_CS   10
#define TFT_DC   46
#define TFT_RST  -1   // reset de l'ecran relie au reset de la carte

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

#define SPI_FREQUENCY       80000000
#define SPI_READ_FREQUENCY  20000000
#define SPI_TOUCH_FREQUENCY  2500000

// L'ecran prend SPI3 (HSPI) ; la liaison vers la carte video utilise SPI2.
#define USE_HSPI_PORT
