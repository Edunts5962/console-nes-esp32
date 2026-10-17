/*----------------------------------------------------------------------------/
  nes_cvbs.hpp - sortie video composite de la carte video TV.

  COPIE ADAPTEE de lgfx::Panel_CVBS (LovyanGFX 1.2.x, fichiers
  src/lgfx/v1/platforms/esp32/Panel_CVBS.hpp et .inl), licence FreeBSD :
  voir LICENCE_LovyanGFX.txt dans ce dossier.

  Original Source : https://github.com/lovyan03/LovyanGFX/
  Author : lovyan03 - Contributors : ciniml, mongonta0716, tobozo
  Inspiration Sources : Roger Cheng (ESP_8_BIT_composite), rossum (esp_8_bit)

  Ce qui change par rapport a l'original (le reste est identique, y compris
  la generation du signal en assembleur) :
   - palette 24 bits au choix (setPaletteRGB888) : les 256 valeurs d'un
     pixel ne sont plus forcement des couleurs RGB332 (3-3-2 bits) mais les
     256 couleurs exactes envoyees par la console (palette NES complete) ;
     changement de palette sans scintillement (deux tables, echange pendant
     le retour de trame) ;
   - image progressive au choix (setProgressive) : 288 lignes (PAL) ou 240
     lignes (NTSC) non entrelacees, comme une vraie NES, au lieu d'un signal
     entrelace a lignes doublees ;
   - norme PAL 60 (PAL_60) : couleur PAL, 525 lignes a 60 Hz, comme
     les consoles europeennes en mode 60 Hz - les jeux NES (60 Hz) sans image
     sautee, avec 3 echantillons par pixel comme en PAL ;
   - nom de classe et espace de noms propres (lgfx::nestv::Panel_NesCVBS) :
     pas de conflit avec lgfx::Panel_CVBS de la bibliotheque.
/----------------------------------------------------------------------------*/
#pragma once

#include <LovyanGFX.hpp>

namespace lgfx
{
 inline namespace v1
 {
 namespace nestv
 {
//----------------------------------------------------------------------------

  struct Panel_NesCVBS : public Panel_FrameBufferBase
  {
  public:
    Panel_NesCVBS();

    ~Panel_NesCVBS();

    struct config_detail_t
    {
      enum signal_type_t
      {
        NTSC,    // black = 7.5IRE
        NTSC_J,  // black = 0IRE (for Japan)
        PAL,
        PAL_M,
        PAL_N,
        PAL_60,  // Couleur PAL (4,43 MHz), 525 lignes / 60 Hz
        signal_type_max,     // dummy, can't select this.
      };

      signal_type_t signal_type = signal_type_t::NTSC;

      // output gpio (ESP32: only 25 or 26)
      int8_t pin_dac = 26;

      // luminance_gain default:128  0=no signal
      uint8_t output_level = 128;

      // default:128  0=monochrome
      uint8_t chroma_level = 128;

      // 0=SRAM only (no use PSRAM) / 1=both(half PSRAM and half SRAM) / 2=PSRAM only (no use SRAM)
      uint8_t use_psram = 0;

      /// background PSRAM read task priority
      uint8_t task_priority = 24;

      /// background PSRAM read task pinned core. (APP_CPU_NUM or PRO_CPU_NUM)
      uint8_t task_pinned_core = -1;
    };

    color_depth_t setColorDepth(color_depth_t) override;
    void setResolution(uint16_t width, uint16_t height, config_detail_t::signal_type_t type = config_detail_t::signal_type_max, int output_width = -1, int output_height = -1, int offset_x = -1, int offset_y = -1);
    void setOutputLevel(uint8_t output_level);
    void setChromaLevel(uint8_t chroma);
    void setSignalType(config_detail_t::signal_type_t signal_type) { _config_detail.signal_type = signal_type; config_detail(_config_detail); }
    void setPsram( bool use_psram ) { _config_detail.use_psram = use_psram; config_detail(_config_detail); }

    bool init(bool) override;

    int32_t getScanLine(void) override;

    const config_detail_t& config_detail(void) const { return _config_detail; }
    void config_detail(const config_detail_t& config_detail);

    // Palette 24 bits : rgb = 256 x 3 octets (R, G, B), la valeur v d'un pixel
    // s'affiche dans la couleur rgb[3v..3v+2]. nullptr : palette RGB332
    // d'origine (dessins LovyanGFX : ecrans d'attente). Calcul (2 ms environ)
    // dans la table de reserve ; si apply_now, elle remplace la table affichee
    // des la ligne suivante (appeler pendant le retour de trame, avec l'image
    // qui va avec) ; sinon, seulement au prochain applyPendingPalette().
    // Renvoie faux si la sortie n'est pas demarree (la palette sera appliquee
    // a init()).
    bool setPaletteRGB888(const uint8_t* rgb, bool apply_now = true);
    void applyPendingPalette(void);
    bool palettePending(void) const;

    // Signal progressif (true) ou entrelace (false, comportement d'origine).
    // Applique au debut de l'image suivante.
    void setProgressive(bool progressive);
    bool getProgressive(void) const { return _progressive; }

    // Premiere et derniere ligne de la trame (numero de getScanLine()) ou
    // l'image est dessinee : en dehors, on peut changer l'image sans
    // dechirure.
    int32_t imageFirstScanLine(void) const;
    // Vrai si le faisceau est hors de l'image (retour de trame, bordures
    // noires du haut et du bas), avec margin lignes de marge.
    bool inBlankingWindow(int margin) const;

  protected:

    config_detail_t _config_detail;

    void deinit(void);

    void updateSignalLevel(void);
    bool initFrameBuffer(size_t w, size_t h, uint8_t use_psram);
    void deinitFrameBuffer(void);

  private:

    void fillPalette(uint32_t* dst);

    bool _started = false;
    bool _progressive = false;
    bool _use_custom_palette = false;
    uint8_t _custom_rgb[256 * 3];
  };

//----------------------------------------------------------------------------
 }
 }
}
