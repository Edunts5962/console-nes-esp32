// wifi_portal.cpp - portail web de la console (voir wifi_portal.h) : une
// page (PORTAL_HTML ci-dessous, servie depuis la flash) et des routes API en
// JSON.
//
// La page est 100 % autonome (CSS et JavaScript dans le HTML), rien n'est
// charge depuis Internet : en point d'acces, la console n'a aucun acces a
// Internet.
//
// Fonctions de la page :
//  - deux emplacements de jeux : carte SD et memoire interne (voir
//    rom_storage.h), deux jauges d'espace ;
//  - envoi de jeux (glisser-deposer ou bouton), destination au choix, place
//    verifiee avant l'envoi (navigateur) et au debut de l'envoi (console) ;
//  - liste filtrable (Tous / SD / Interne), deplacement d'un jeu d'un
//    emplacement a l'autre, suppression, jeux ecartes (quarantaine) ;
//  - panneau "Maintenance" : supprimer tous les jeux de la carte SD (les
//    autres fichiers restent), formater la carte SD en FAT32 (confirmation
//    en tapant FORMATER), effacer la memoire interne ;
//  - reseau Wi-Fi de la box (enregistre en NVS) ;
//  - mise a jour du logiciel (fichier .bin, OTA) ;
//  - diagnostic de la memoire interne et de la carte SD.
// Pendant un deplacement ou un formatage, la console attend la fin de
// l'operation (quelques secondes) avant de repondre a nouveau.
#include "wifi_boot.h"  // ENABLE_WIFI_PORTAL, g_wifi_boot_status
#include "wifi_portal.h"

// Definie HORS du "#if ENABLE_WIFI_PORTAL" (voir wifi_portal.h) : reste a
// false pour toujours quand le portail est desactive.
volatile bool g_portal_games_changed = false;
volatile bool g_portal_wifi_reconnect = false;

#if ENABLE_WIFI_PORTAL
#include <WiFi.h>
#include <WebServer.h>
#include "FS.h"
#include "rom_storage.h"
#include <esp_heap_caps.h>   // Journal des requetes
#include <Update.h>          // Mise a jour du logiciel par le portail
#include "hw_config.h"       // NES_EMULATOR_VERSION_SHORT
#include "crash_guard.h"     // Jeux ecartes remis dans la liste

static WebServer server(80);
static File upload_file;
static bool upload_success = false;
static bool upload_write_error = false;
static const char *upload_err = "";   // raison d'un echec, renvoyee au navigateur
// Ecrit dans "/NESUPLD.TMP" (nom court, ROM_UPLOAD_TMP_NAME).
// L'envoi est ecrit dans "/nom.nes~tmp", renomme en "/nom.nes" une fois
// complet et verifie - un envoi rate ne detruit donc jamais un jeu du meme nom
// deja present (celui-ci est mis de cote en "/nom.nes~old" le temps du
// remplacement, puis supprime ; voir aussi rom_storage_cleanup_leftovers() au
// demarrage, en cas de coupure).
static String upload_tmp_path, upload_final_path;
static int upload_where = -1;

// ---------------------------------------------------------------------
// Page web (autonome : HTML + CSS + JS inline). Pensee d'abord pour un
// telephone : une colonne, gros boutons, confirmation en deux temps.
static const char PORTAL_HTML[] PROGMEM = R"HTMLDOC(
<!DOCTYPE html>
<html lang="fr">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>NES-WiFi</title>
<style>
  :root {
    --bg: #14161c;
    --panel: #1e212b;
    --panel2: #262a36;
    --accent: #e63946;
    --ok: #4cd964;
    --text: #f2f2f2;
    --muted: #9aa0ab;
    --border: #2c3040;
    --sd: #8c9ba8;
    --flash: #b18cff;
    --warn: #f2b34a;
  }
  * { box-sizing: border-box; }
  [hidden] { display: none !important; }
  .st-note { font-size: 12px; color: #f2b34a; margin-top: 6px; line-height: 1.4; }
  body {
    margin: 0;
    background: var(--bg);
    color: var(--text);
    font-family: -apple-system, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
    padding: 16px;
  }
  .wrap { max-width: 480px; margin: 0 auto; }
  h1 { font-size: 20px; letter-spacing: 1px; margin: 4px 0 2px; color: var(--accent); }
  .ver { font-size: 12px; color: var(--muted); margin: 0 0 14px; font-variant-numeric: tabular-nums; }
  h2 { font-size: 12px; letter-spacing: 1.5px; text-transform: uppercase; color: var(--muted); margin: 0 0 10px; font-weight: 600; }
  .panel { background: var(--panel); border: 1px solid var(--border); border-radius: 10px; padding: 14px; margin-bottom: 14px; }
  .st + .st { margin-top: 12px; }
  .st-head { display: flex; align-items: baseline; gap: 8px; font-size: 14px; margin-bottom: 6px; }
  .st-head .st-val { margin-left: auto; font-size: 12px; color: var(--muted); font-variant-numeric: tabular-nums; }
  .dot { width: 10px; height: 10px; border-radius: 50%; display: inline-block; flex: none; }
  .dot.sd { background: var(--sd); } .dot.flash { background: var(--flash); }
  .bar { height: 8px; background: #0d0f14; border-radius: 4px; overflow: hidden; }
  .bar .fill { height: 100%; width: 0%; transition: width .3s ease; }
  .bar .fill.sd { background: var(--sd); } .bar .fill.flash { background: var(--flash); }
  .st.off { opacity: .55; }
  .seg { display: grid; grid-template-columns: 1fr 1fr; gap: 6px; background: #0d0f14; padding: 4px; border-radius: 9px; margin-bottom: 10px; }
  .seg button { border: 0; border-radius: 6px; padding: 9px 6px; background: transparent; color: var(--muted); font-size: 14px; line-height: 1.2; }
  .seg button small { display: block; font-size: 11px; opacity: .8; margin-top: 2px; }
  .seg button[aria-checked="true"].sd { background: var(--sd); color: #10141a; }
  .seg button[aria-checked="true"].flash { background: var(--flash); color: #150d24; }
  .seg button:disabled { opacity: .35; }
  .dropzone { border: 2px dashed var(--border); border-radius: 10px; padding: 22px 14px; text-align: center; color: var(--muted); font-size: 14px; transition: border-color .2s, background .2s; }
  .dropzone.drag { border-color: var(--accent); background: rgba(230,57,70,.08); }
  .dropzone label { color: var(--accent); text-decoration: underline; cursor: pointer; }
  .dropzone input[type=file] { display: none; }
  .dz-dest { margin-top: 8px; font-size: 13px; }
  .dz-dest b.sd { color: var(--sd); } .dz-dest b.flash { color: var(--flash); }
  .dz-hint { margin-top: 6px; font-size: 12px; opacity: .8; }
  .progress-wrap { margin-top: 10px; }
  .progress-wrap .bar .fill { background: var(--accent); }
  .ptext { font-size: 12px; color: var(--muted); margin-top: 6px; }
  .search { width: 100%; min-height: 38px; background: #0d0f14; color: var(--text); border: 1px solid var(--border); border-radius: 8px; padding: 0 12px; font-size: 15px; margin-bottom: 8px; }
  .filters { display: flex; gap: 6px; margin-bottom: 6px; flex-wrap: wrap; }
  .filters button { border: 1px solid var(--border); background: transparent; color: var(--muted); border-radius: 16px; padding: 5px 12px; font-size: 13px; }
  .filters button[aria-pressed="true"] { color: var(--text); border-color: var(--text); }
  .filters button .n { font-variant-numeric: tabular-nums; opacity: .8; }
  .selbar { display: flex; align-items: center; gap: 6px; flex-wrap: wrap; padding: 8px 4px; border-bottom: 1px solid var(--border); font-size: 13px; color: var(--muted); }
  .selbar label { display: flex; align-items: center; gap: 6px; margin-right: auto; }
  .selbar .btn { min-height: 32px; }
  .game-list { list-style: none; margin: 0; padding: 0; }
  .game-list li.game { display: flex; align-items: center; gap: 8px; padding: 10px 4px 10px 8px; border-bottom: 1px solid var(--border); border-left: 3px solid var(--sd); }
  .game-list li.game.flash { border-left-color: var(--flash); }
  .game-list li.game.sel { background: rgba(230,57,70,.07); }
  .game-list li.game:last-child { border-bottom: none; }
  .game-list input[type=checkbox], .selbar input[type=checkbox] { width: 20px; height: 20px; accent-color: var(--accent); flex: none; margin: 0; }
  .g-main { flex: 1; min-width: 0; }
  .game-name { display: block; font-size: 15px; word-break: break-word; }
  .g-meta { display: flex; align-items: center; gap: 6px; font-size: 12px; color: var(--muted); margin-top: 3px; font-variant-numeric: tabular-nums; flex-wrap: wrap; }
  .badge { font-size: 11px; font-weight: 600; border-radius: 4px; padding: 1px 6px; letter-spacing: .3px; }
  .badge.sd { background: rgba(140,155,168,.18); color: var(--sd); }
  .badge.flash { background: rgba(177,140,255,.18); color: var(--flash); }
  .badge.dup { background: rgba(242,179,74,.16); color: var(--warn); }
  .g-actions { display: flex; gap: 6px; flex: none; }
  .game-list li.empty { padding: 18px 4px; color: var(--muted); text-align: center; font-size: 14px; border: 0; }
  .more { display: block; width: 100%; margin-top: 8px; }
  .btn { min-height: 36px; padding: 0 10px; border: 1px solid var(--border); background: transparent; color: var(--muted); border-radius: 6px; font-size: 13px; }
  .btn:disabled { opacity: .35; }
  .btn.mv.to-flash { color: var(--flash); border-color: rgba(177,140,255,.45); }
  .btn.mv.to-sd { color: var(--sd); border-color: rgba(140,155,168,.45); }
  .btn.ok:not(:disabled) { color: var(--ok); border-color: rgba(76,217,100,.45); }
  .btn.main:not(:disabled) { color: #fff; background: var(--accent); border-color: var(--accent); }
  .btn.danger:not(:disabled) { color: var(--accent); border-color: rgba(230,57,70,.5); }
  .btn.confirm, .btn.danger.confirm { color: #fff; background: var(--accent); border-color: var(--accent); }
  .q-intro { font-size: 13px; color: var(--muted); margin: 0 0 6px; line-height: 1.4; }
  details.maint > summary { cursor: pointer; font-size: 14px; color: var(--muted); list-style: none; }
  details.maint > summary::before { content: "\25B8  "; }
  details.maint[open] > summary::before { content: "\25BE  "; }
  .m-row { padding: 12px 0; border-top: 1px solid var(--border); }
  .m-row:first-of-type { margin-top: 10px; }
  .m-row p { margin: 0 0 8px; font-size: 13px; color: var(--muted); line-height: 1.4; }
  .m-row p b { color: var(--text); }
  .m-row p .warn { color: var(--warn); }
  .m-row .fmt { display: flex; gap: 6px; flex-wrap: wrap; }
  .m-row input:not([type=checkbox]):not([type=file]) { flex: 1; min-width: 0; min-height: 36px; background: #0d0f14; color: var(--text); border: 1px solid var(--border); border-radius: 6px; padding: 0 10px; font-size: 14px; }
  #fmtInput { letter-spacing: 1px; }
  .m-row .chk { display: flex; align-items: center; gap: 8px; font-size: 13px; color: var(--muted); margin: 8px 0; }
  .m-row .chk input { width: 18px; height: 18px; accent-color: var(--accent); }
  .nets { list-style: none; margin: 6px 0 0; padding: 0; max-height: 220px; overflow-y: auto; }
  .nets li button { width: 100%; text-align: left; display: flex; gap: 8px; align-items: center; background: transparent; border: 0; border-bottom: 1px solid var(--border); color: var(--text); padding: 9px 4px; font-size: 14px; }
  .nets li button span { margin-left: auto; color: var(--muted); font-size: 12px; font-variant-numeric: tabular-nums; }
  .busy { position: fixed; inset: 0; background: rgba(10,12,16,.72); display: flex; align-items: center; justify-content: center; z-index: 5; }
  .busy div { background: var(--panel); border: 1px solid var(--border); border-radius: 10px; padding: 16px 20px; font-size: 14px; max-width: 80%; text-align: center; line-height: 1.4; }
  .toast { position: fixed; left: 50%; bottom: 20px; transform: translateX(-50%); background: var(--panel); border: 1px solid var(--border); color: var(--text); padding: 10px 16px; border-radius: 8px; font-size: 13px; box-shadow: 0 4px 16px rgba(0,0,0,.4); z-index: 6; max-width: 90%; }
  .toast.error { border-color: var(--accent); color: var(--accent); }
</style>
</head>
<body>
<div class="wrap">
  <h1>NES&#8209;WIFI &mdash; PORTAIL DE JEUX</h1>
  <p class="ver" id="ver">&nbsp;</p>

  <div class="panel">
    <div class="st" id="stSd">
      <div class="st-head"><span class="dot sd"></span>Carte SD<span class="st-val" id="sdText">&hellip;</span></div>
      <div class="bar"><div class="fill sd" id="sdFill"></div></div>
      <div class="st-note" id="sdNote" hidden></div>
    </div>
    <div class="st" id="stFlash">
      <div class="st-head"><span class="dot flash"></span>M&eacute;moire interne<span class="st-val" id="flashText">&hellip;</span></div>
      <div class="bar"><div class="fill flash" id="flashFill"></div></div>
      <div class="st-note" id="flashNote" hidden></div>
    </div>
  </div>

  <div class="panel">
    <h2>Envoyer vers</h2>
    <div class="seg" role="radiogroup" aria-label="Destination des jeux envoy&eacute;s">
      <button class="sd" data-dest="sd" role="radio" aria-checked="true">Carte SD<small id="segSd">&nbsp;</small></button>
      <button class="flash" data-dest="flash" role="radio" aria-checked="false">M&eacute;moire interne<small id="segFlash">&nbsp;</small></button>
    </div>
    <div class="dropzone" id="dropzone">
      Glisse des fichiers .nes ou .zip ici<br>
      ou <label>parcourir<input type="file" id="fileInput" multiple accept=".nes,.NES,.zip,.ZIP"></label>
      <div class="dz-dest">&rarr; <b id="dzDest" class="sd">Carte SD</b></div>
      <div class="dz-hint">Les .zip sont ouverts ici, dans le navigateur : seuls les jeux .nes qu&rsquo;ils contiennent sont envoy&eacute;s.</div>
    </div>
    <div class="progress-wrap" id="uploadProgress" hidden>
      <div class="bar"><div class="fill" id="uploadFill"></div></div>
      <div class="ptext" id="uploadText"></div>
    </div>
  </div>

  <div class="panel">
    <input class="search" id="search" type="search" placeholder="Rechercher un jeu" autocomplete="off" aria-label="Rechercher un jeu">
    <div class="filters" id="filters">
      <button data-f="all" aria-pressed="true">Tous <span class="n" id="nAll">0</span></button>
      <button data-f="sd" aria-pressed="false">SD <span class="n" id="nSd">0</span></button>
      <button data-f="flash" aria-pressed="false">Interne <span class="n" id="nFlash">0</span></button>
      <button data-f="dup" aria-pressed="false" id="fDup" hidden>Doublons <span class="n" id="nDup">0</span></button>
    </div>
    <div class="selbar" id="selbar">
      <label><input type="checkbox" id="selAll" aria-label="Tout s&eacute;lectionner"><span id="selText">Tout s&eacute;lectionner</span></label>
      <button class="btn mv to-sd" id="selToSd" hidden>&rarr; SD</button>
      <button class="btn mv to-flash" id="selToFlash" hidden>&rarr; Interne</button>
      <button class="btn danger" id="selDel" hidden>Supprimer</button>
    </div>
    <ul class="game-list" id="gameList"><li class="empty">Chargement&hellip;</li></ul>
    <button class="btn more" id="more" hidden>Afficher plus</button>
  </div>

  <div class="panel" id="quarPanel" hidden>
    <h2>Jeux &eacute;cart&eacute;s</h2>
    <p class="q-intro">Ces jeux ont fait red&eacute;marrer la console au lancement : ils ont &eacute;t&eacute; mis de c&ocirc;t&eacute; (dossier <b>quarantaine</b>) au lieu d&rsquo;&ecirc;tre effac&eacute;s.</p>
    <ul class="game-list" id="quarList"></ul>
  </div>

  <details class="panel maint" id="wifiBox">
    <summary>R&eacute;seau Wi&#8209;Fi de la box</summary>
    <div class="m-row">
      <p>Box utilis&eacute;e : <b id="wifiCur">&hellip;</b> <span id="wifiSrc"></span></p>
      <p id="wifiApNote" hidden><span class="warn">La console n&rsquo;a pas pu rejoindre la box</span> : elle a ouvert son propre r&eacute;seau (point d&rsquo;acc&egrave;s). Indique ici le bon r&eacute;seau.</p>
      <div class="fmt">
        <input id="wSsid" autocomplete="off" autocapitalize="none" spellcheck="false" placeholder="Nom du r&eacute;seau" aria-label="Nom du r&eacute;seau" maxlength="32">
        <button class="btn" id="wScan">Chercher</button>
      </div>
      <ul class="nets" id="nets" hidden></ul>
      <div class="fmt" style="margin-top:6px">
        <input id="wPass" type="password" autocomplete="off" placeholder="Mot de passe" aria-label="Mot de passe" maxlength="63">
        <button class="btn" id="wShow" aria-pressed="false">Voir</button>
      </div>
      <label class="chk"><input type="checkbox" id="wApply" checked>Se connecter tout de suite</label>
      <p>Si la connexion r&eacute;ussit, rejoins la box sur ce t&eacute;l&eacute;phone et ouvre l&rsquo;adresse affich&eacute;e sur la console (R&eacute;glages, onglet Portail). Si elle &eacute;choue, la console rouvre son point d&rsquo;acc&egrave;s <b>NES-WiFi</b> (mode Auto).</p>
      <div class="fmt">
        <button class="btn main" id="wSave">Enregistrer</button>
        <button class="btn" id="wForget" hidden>Oublier (revenir &agrave; wifi_config.h)</button>
      </div>
    </div>
  </details>

  <details class="panel maint" id="maint">
    <summary>Maintenance</summary>
    <div class="m-row">
      <p><b>Mettre &agrave; jour le logiciel de la console.</b> Fichier <b>nes_emulator.ino.bin</b> (Arduino IDE : Croquis &gt; Exporter les binaires compil&eacute;s). Jeux, sauvegardes et r&eacute;glages sont gard&eacute;s. La console red&eacute;marre ensuite.</p>
      <div class="fmt">
        <input type="file" id="binInput" accept=".bin" aria-label="Fichier .bin">
        <button class="btn danger" id="binGo" disabled>Installer</button>
      </div>
      <div class="progress-wrap" id="binProgress" hidden>
        <div class="bar"><div class="fill" id="binFill"></div></div>
        <div class="ptext" id="binText"></div>
      </div>
    </div>
    <div class="m-row">
      <p><b>Supprimer tous les jeux de la carte SD.</b> Seuls les fichiers .nes sont effac&eacute;s ; le reste de la carte (musique, dossiers) ne bouge pas.</p>
      <button class="btn danger" id="wipeSdGames">Supprimer les jeux de la SD</button>
    </div>
    <div class="m-row">
      <p><b>Effacer la m&eacute;moire interne.</b> Tous les jeux qu&rsquo;elle contient sont effac&eacute;s, avec leurs sauvegardes.</p>
      <button class="btn danger" id="wipeFlash">Effacer la m&eacute;moire interne</button>
    </div>
    <div class="m-row">
      <p><b>Formater la carte SD en FAT32.</b> <span class="warn">Efface TOUT son contenu</span>, y compris ce qui n&rsquo;est pas un jeu (musique de mp3_player, sauvegardes&hellip;). Tape <b>FORMATER</b> pour confirmer.</p>
      <div class="fmt">
        <input id="fmtInput" autocomplete="off" autocapitalize="characters" placeholder="FORMATER" aria-label="Taper FORMATER pour confirmer">
        <button class="btn danger" id="fmtSd" disabled>Formater</button>
      </div>
    </div>
  </details>
</div>
<div class="busy" id="busy" hidden><div id="busyText">Patiente&hellip;</div></div>
<div class="toast" id="toast" hidden></div>
<script>
var S = { status: null, games: [], quar: [], filter: 'all', dest: 'sd', q: '', sel: {}, limit: 200, dups: {} };
var WNAME = { sd: 'Carte SD', flash: 'Mémoire interne' };
var MARGIN = 16 * 1024;
try { var d = localStorage.getItem('nesDest'); if (d === 'sd' || d === 'flash') S.dest = d; } catch (e) {}

function $(id) { return document.getElementById(id); }
function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, function (c) { return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]; });
}
function fmtKb(kb) {
  if (kb >= 1024 * 1024) return (kb / 1048576).toFixed(1).replace('.', ',') + ' Go';
  if (kb >= 1024) return (kb / 1024).toFixed(1).replace('.', ',') + ' Mo';
  return Math.round(kb) + ' Ko';
}
function showToast(msg, isError) {
  var t = $('toast');
  t.textContent = msg;
  t.className = 'toast' + (isError ? ' error' : '');
  t.hidden = false;
  clearTimeout(showToast._t);
  showToast._t = setTimeout(function () { t.hidden = true; }, isError ? 7000 : 3000);
}
function busy(msg) { $('busyText').textContent = msg || ''; $('busy').hidden = !msg; }
function avail(w) { return S.status && S.status[w] && S.status[w].ok; }
function freeB(w) { return avail(w) ? S.status[w].free * 1024 : 0; }
function key(x) { return x.where + '|' + x.name; }

function renderStatus() {
  ['sd', 'flash'].forEach(function (w) {
    var st = S.status[w], el = $(w === 'sd' ? 'stSd' : 'stFlash');
    var txt = $(w + 'Text'), fill = $(w + 'Fill');
    el.classList.toggle('off', !st.ok);
    // racine a nombre de noms fixe (FAT12/16)
    var root = '';
    if (st.ok && st.rootTotal) {
      if (w === 'sd') root = 'Carte en FAT' + st.fat + ' : la racine ne tient que ' + st.rootTotal + ' noms (' + st.rootUsed +
        ' utilisés), soit environ 150 jeux, quelle que soit la place libre. Pour en mettre plus : formate la carte en FAT32 (Maintenance ci-dessous, ou sur un PC).';
      else if (st.rootUsed >= st.rootTotal * 0.8) root = 'Noms de fichiers : ' + st.rootUsed + ' / ' + st.rootTotal +
        ' (la mémoire interne tient environ 150 jeux au plus).';
    }
    if (w === 'flash') {   // partitions presentes, taille de la flash
      var note = $('flashNote');
      note.textContent = (!st.ok && st.detail) ? st.detail : root;
      note.hidden = !note.textContent;
    } else {
      $('sdNote').textContent = root;
      $('sdNote').hidden = !root;
    }
    if (!st.ok) {
      txt.textContent = w === 'sd' ? 'absente ou illisible' : 'indisponible' + (st.why ? ' : ' + st.why : '');
      fill.style.width = '0%';
    } else {
      var used = st.total - st.free;
      txt.textContent = fmtKb(st.free) + ' libres sur ' + fmtKb(st.total) + (st.fat ? ' · FAT' + st.fat : '');
      fill.style.width = (st.total > 0 ? used / st.total * 100 : 0).toFixed(1) + '%';
    }
    var seg = document.querySelector('.seg button[data-dest="' + w + '"]');
    seg.disabled = !st.ok;
    $(w === 'sd' ? 'segSd' : 'segFlash').textContent = st.ok ? fmtKb(st.free) + ' libres' : (w === 'sd' ? 'absente' : 'indisponible');
  });
  if (!avail(S.dest)) S.dest = avail('sd') ? 'sd' : (avail('flash') ? 'flash' : S.dest);
  $('wipeSdGames').disabled = !avail('sd');
  $('wipeFlash').disabled = !avail('flash');
  $('fmtInput').disabled = !avail('sd');
  updateFmtBtn();
  renderDest();
  // version et reseau de la box
  if (S.status.version) $('ver').textContent = 'Console ' + S.status.version;
  var wf = S.status.wifi;
  if (wf) {
    $('wifiCur').textContent = wf.ssid || '(aucun)';
    $('wifiSrc').textContent = wf.src === 'portail' ? '(enregistré ici)' : '(wifi_config.h)';
    $('wForget').hidden = wf.src !== 'portail';
    $('wifiApNote').hidden = !wf.ap;
    if (wf.ap && !renderStatus._opened) { renderStatus._opened = true; $('wifiBox').open = true; }
  }
}
function renderDest() {
  document.querySelectorAll('.seg button').forEach(function (b) {
    b.setAttribute('aria-checked', b.dataset.dest === S.dest ? 'true' : 'false');
  });
  var dz = $('dzDest');
  dz.textContent = WNAME[S.dest];
  dz.className = S.dest;
}
function loadStatus() {
  return fetch('/api/status').then(function (r) { return r.json(); }).then(function (s) { S.status = s; renderStatus(); renderGames(); });
}

// ---- recherche, doublons, selection ----
// Recherche : tous les mots tapes doivent se trouver dans le nom (comme la
// recherche de la console).
function matches(name, q) {
  if (!q) return true;
  var n = name.replace(/\.nes$/i, '').toLowerCase();
  return q.toLowerCase().split(/\s+/).every(function (w) { return !w || n.indexOf(w) >= 0; });
}
// Doublons : meme nom une fois retires numero de tete, indications entre
// parentheses ou crochets et ponctuation - ET meme taille.
function normName(name) {
  return name.replace(/\.nes$/i, '').replace(/\([^)]*\)|\[[^\]]*\]/g, ' ').replace(/^\s*\d+\s*[-._]?\s+/, '')
    .toLowerCase().replace(/[^a-z0-9]+/g, '');
}
function computeDups() {
  var groups = {};
  S.games.forEach(function (x) { var k = normName(x.name) + '|' + x.size; (groups[k] = groups[k] || []).push(x); });
  S.dups = {};
  var n = 0;
  Object.keys(groups).forEach(function (k) {
    if (groups[k].length > 1) groups[k].forEach(function (x) { S.dups[key(x)] = k; n++; });
  });
  return n;
}
function visibleGames() {
  var g = S.games.slice().sort(function (a, b) { return a.name.localeCompare(b.name) || (a.where < b.where ? 1 : -1); });
  if (S.filter === 'dup') {
    g = g.filter(function (x) { return S.dups[key(x)]; });
    g.sort(function (a, b) { return S.dups[key(a)].localeCompare(S.dups[key(b)]) || a.name.localeCompare(b.name); });
  } else if (S.filter !== 'all') g = g.filter(function (x) { return x.where === S.filter; });
  return g.filter(function (x) { return matches(x.name, S.q); });
}
function selected() { return S.games.filter(function (x) { return S.sel[key(x)]; }); }

function renderGames() {
  var list = $('gameList');
  var all = S.games;
  var nSd = all.filter(function (x) { return x.where === 'sd'; }).length;
  var nDup = computeDups();
  $('nAll').textContent = all.length; $('nSd').textContent = nSd; $('nFlash').textContent = all.length - nSd;
  $('nDup').textContent = nDup; $('fDup').hidden = nDup === 0 && S.filter !== 'dup';
  document.querySelectorAll('#filters button').forEach(function (b) { b.setAttribute('aria-pressed', b.dataset.f === S.filter ? 'true' : 'false'); });
  // selection : seulement des jeux encore presents
  var present = {};
  all.forEach(function (x) { present[key(x)] = 1; });
  Object.keys(S.sel).forEach(function (k) { if (!present[k]) delete S.sel[k]; });
  var g = visibleGames();
  renderSelbar(g);
  if (g.length === 0) {
    list.innerHTML = '<li class="empty">' + (S.q ? 'Aucun jeu ne correspond à « ' + escapeHtml(S.q) + ' »' :
      S.filter === 'dup' ? 'Aucun doublon' : (all.length ? 'Aucun jeu ici' : 'Aucun jeu pour l’instant')) + '</li>';
    $('more').hidden = true;
    return;
  }
  var html = '';
  g.slice(0, S.limit).forEach(function (x) {
    var other = x.where === 'sd' ? 'flash' : 'sd';
    var canMove = avail(other) && x.size + MARGIN <= freeB(other);
    var mvTitle = !avail(other) ? WNAME[other] + ' indisponible' : (canMove ? 'Déplacer vers : ' + WNAME[other] : 'Pas assez de place : ' + WNAME[other]);
    var k = key(x), on = !!S.sel[k];
    html += '<li class="game ' + x.where + (on ? ' sel' : '') + '">' +
      '<input type="checkbox" data-k="' + escapeHtml(k) + '"' + (on ? ' checked' : '') + ' aria-label="Choisir ' + escapeHtml(x.name) + '">' +
      '<div class="g-main"><span class="game-name">' + escapeHtml(x.name.replace(/\.nes$/i, '')) + '</span>' +
      '<span class="g-meta"><span class="badge ' + x.where + '">' + (x.where === 'sd' ? 'SD' : 'Interne') + '</span>' + fmtKb(x.size / 1024) +
      (S.dups[k] ? '<span class="badge dup">doublon</span>' : '') + '</span></div>' +
      '<div class="g-actions">' +
      '<button class="btn mv to-' + other + '" data-act="mv" data-name="' + escapeHtml(x.name) + '" data-where="' + x.where + '" title="' + escapeHtml(mvTitle) + '"' + (canMove ? '' : ' disabled') + '>&rarr; ' + (other === 'sd' ? 'SD' : 'Interne') + '</button>' +
      '<button class="btn" data-act="del" data-name="' + escapeHtml(x.name) + '" data-where="' + x.where + '">Suppr.</button>' +
      '</div></li>';
  });
  list.innerHTML = html;
  $('more').hidden = g.length <= S.limit;
  $('more').textContent = 'Afficher plus (' + (g.length - S.limit) + ' autres)';
}
function renderSelbar(g) {
  var sel = selected(), n = sel.length;
  var visOn = g.length > 0 && g.every(function (x) { return S.sel[key(x)]; });
  $('selAll').checked = visOn;
  $('selAll').indeterminate = !visOn && g.some(function (x) { return S.sel[key(x)]; });
  $('selText').textContent = n ? n + ' choisi' + (n > 1 ? 's' : '') : 'Tout sélectionner';
  var nSd = sel.filter(function (x) { return x.where === 'sd'; }).length;
  $('selDel').hidden = n === 0; $('selDel').textContent = 'Supprimer (' + n + ')';
  $('selToFlash').hidden = nSd === 0 || !avail('flash'); $('selToFlash').textContent = '→ Interne (' + nSd + ')';
  $('selToSd').hidden = n - nSd === 0 || !avail('sd'); $('selToSd').textContent = '→ SD (' + (n - nSd) + ')';
}
function loadGames() {
  return fetch('/api/games').then(function (r) { return r.json(); }).then(function (games) { S.games = games; renderGames(); });
}
function renderQuar() {
  $('quarPanel').hidden = S.quar.length === 0;
  var html = '';
  S.quar.forEach(function (x) {
    html += '<li class="game ' + x.where + '"><div class="g-main"><span class="game-name">' + escapeHtml(x.name.replace(/\.nes$/i, '')) + '</span>' +
      '<span class="g-meta"><span class="badge ' + x.where + '">' + (x.where === 'sd' ? 'SD' : 'Interne') + '</span>' + fmtKb(x.size / 1024) + '</span></div>' +
      '<div class="g-actions"><button class="btn ok" data-q="restore" data-name="' + escapeHtml(x.name) + '" data-where="' + x.where + '">Remettre</button>' +
      '<button class="btn" data-q="delete" data-name="' + escapeHtml(x.name) + '" data-where="' + x.where + '">Suppr.</button></div></li>';
  });
  $('quarList').innerHTML = html;
}
function loadQuar() {
  return fetch('/api/quarantine').then(function (r) { return r.json(); }).then(function (q) { S.quar = q; renderQuar(); }).catch(function () {});
}
function refresh() { return Promise.all([loadStatus(), loadGames(), loadQuar()]); }

function post(url, fields) {
  var fd = new FormData();
  Object.keys(fields).forEach(function (k) { fd.append(k, fields[k]); });
  return fetch(url, { method: 'POST', body: fd }).then(function (r) { return r.json(); });
}
var ERR = {
  space: 'pas assez de place', exists: 'un jeu du même nom y est déjà', unavail: 'emplacement indisponible',
  nofile: 'jeu introuvable', io: 'erreur de lecture/écriture', ext: 'fichier non .nes', open: 'ouverture impossible', write: 'erreur d’écriture (plein ?)',
  name: 'nom trop long (63 caractères au plus, extension comprise)',
  rootsd: 'plus de place pour les noms à la racine de la carte SD (FAT16 : 512 noms au plus, ~150 jeux) : formate-la en FAT32 (Maintenance)',
  rootflash: 'plus de place pour les noms dans la mémoire interne (512 noms au plus, ~150 jeux)',
  ssid: 'nom de réseau invalide (1 à 32 caractères)', pass: 'mot de passe invalide (vide pour un réseau ouvert, sinon 8 à 63 caractères)',
  binext: 'il faut un fichier .bin', notimg: 'ce fichier n’est pas un logiciel ESP32',
  chip: 'logiciel prévu pour une autre puce (celui de la carte vidéo ?) : choisis nes_emulator.ino.bin',
  begin: 'mise à jour impossible (partition)', verify: 'fichier incomplet ou abîmé : rien n’a été changé'
};
function isRootErr(j) { return j && (j.err === 'rootsd' || j.err === 'rootflash'); }
function errText(j) { return ERR[j && j.err] || 'échec'; }

// Confirmation en deux temps (3 s), sans confirm() natif.
// Un 2e appui arrivant moins de 0,4 s apres le 1er (double-tape) est
// ignore : la confirmation doit etre un geste volontaire.
function armed(btn, label, msg) {
  if (btn.classList.contains('confirm')) {
    if (Date.now() - (btn._armedAt || 0) <= 400) return false;
    disarm(btn);            // une confirmation = une seule action
    return true;
  }
  btn._label = btn.textContent;
  btn._armedAt = Date.now();
  btn.classList.add('confirm');
  btn.textContent = label || 'Confirmer ?';
  if (msg) showToast(msg);
  btn._timer = setTimeout(function () { disarm(btn); }, 3000);
  return false;
}
function disarm(btn) {
  clearTimeout(btn._timer);
  if (!btn.classList.contains('confirm')) return;
  btn.classList.remove('confirm');
  btn.textContent = btn._label;
}

$('gameList').addEventListener('change', function (e) {
  var c = e.target.closest('input[data-k]');
  if (!c) return;
  if (c.checked) S.sel[c.dataset.k] = 1; else delete S.sel[c.dataset.k];
  c.closest('li').classList.toggle('sel', c.checked);
  renderSelbar(visibleGames());
});
$('gameList').addEventListener('click', function (e) {
  var btn = e.target.closest('button[data-act]');
  if (!btn || btn.disabled) return;
  var name = btn.dataset.name, where = btn.dataset.where;
  if (btn.dataset.act === 'del') {
    if (!armed(btn, 'Confirmer ?', 'Touche « Confirmer ? » sous 3 s pour supprimer ' + name)) return;
    post('/api/delete', { name: name, where: where }).then(function (j) {
      if (j.ok) showToast('Supprimé : ' + name); else showToast('Échec de la suppression', true);
      refresh();
    }).catch(function () { showToast('Erreur réseau', true); });
  } else {
    var to = where === 'sd' ? 'flash' : 'sd';
    busy('Déplacement de ' + name + ' vers : ' + WNAME[to] + '…');
    post('/api/move', { name: name, from: where }).then(function (j) {
      busy('');
      if (j.ok) showToast(name + ' → ' + WNAME[to]); else showToast('Déplacement impossible : ' + errText(j), true);
      refresh();
    }).catch(function () { busy(''); showToast('Erreur réseau', true); refresh(); });
  }
});
$('selAll').addEventListener('change', function () {
  var on = this.checked;
  visibleGames().forEach(function (x) { if (on) S.sel[key(x)] = 1; else delete S.sel[key(x)]; });
  renderGames();
});
// Actions groupees : une requete apres l'autre (la console traite un jeu a la fois).
function batch(items, label, fn, done) {
  var i = 0, ok = 0, fails = [];
  function next() {
    if (i >= items.length) {
      busy('');
      S.sel = {};
      if (fails.length === 0) showToast(done(ok));
      else showToast(done(ok) + ' — ' + fails.length + ' en échec (' + fails[fails.length - 1] + ')', true);
      refresh();
      return;
    }
    var x = items[i++];
    busy(label + ' (' + i + '/' + items.length + ') : ' + x.name);
    fn(x).then(function (j) { if (j.ok) ok++; else fails.push(x.name + ' : ' + errText(j)); next(); })
      .catch(function () { fails.push(x.name + ' : erreur réseau'); next(); });
  }
  next();
}
$('selDel').addEventListener('click', function () {
  var sel = selected();
  if (!sel.length) return;
  if (!armed(this, 'Confirmer ?', 'Touche « Confirmer ? » sous 3 s : ' + sel.length + ' jeu(x) seront supprimés')) return;
  batch(sel, 'Suppression', function (x) { return post('/api/delete', { name: x.name, where: x.where }); },
        function (n) { return n + ' jeu(x) supprimé(s)'; });
});
function moveSel(from) {
  var sel = selected().filter(function (x) { return x.where === from; });
  var to = from === 'sd' ? 'flash' : 'sd';
  batch(sel, 'Déplacement vers : ' + WNAME[to], function (x) { return post('/api/move', { name: x.name, from: from }); },
        function (n) { return n + ' jeu(x) → ' + WNAME[to]; });
}
$('selToFlash').addEventListener('click', function () { moveSel('sd'); });
$('selToSd').addEventListener('click', function () { moveSel('flash'); });
$('more').addEventListener('click', function () { S.limit += 300; renderGames(); });
$('search').addEventListener('input', function () { S.q = this.value.trim(); S.limit = 200; renderGames(); });

$('filters').addEventListener('click', function (e) {
  var b = e.target.closest('button[data-f]');
  if (!b) return;
  S.filter = b.dataset.f;
  S.limit = 200;
  renderGames();
});

$('quarList').addEventListener('click', function (e) {
  var btn = e.target.closest('button[data-q]');
  if (!btn) return;
  var act = btn.dataset.q, name = btn.dataset.name;
  if (act === 'delete' && !armed(btn, 'Confirmer ?', 'Touche « Confirmer ? » sous 3 s pour supprimer définitivement ' + name)) return;
  post('/api/quarantine', { act: act, name: name, where: btn.dataset.where }).then(function (j) {
    if (j.ok) showToast(act === 'restore' ? name + ' est de retour dans la liste' : 'Supprimé : ' + name);
    else showToast((act === 'restore' ? 'Impossible de le remettre : ' : 'Échec : ') + errText(j), true);
    refresh();
  }).catch(function () { showToast('Erreur réseau', true); });
});

document.querySelector('.seg').addEventListener('click', function (e) {
  var b = e.target.closest('button[data-dest]');
  if (!b || b.disabled) return;
  S.dest = b.dataset.dest;
  try { localStorage.setItem('nesDest', S.dest); } catch (err) {}
  renderDest();
});

// ---- fichiers .zip ouverts dans le navigateur ----
function inflate(method, data) {
  if (method === 0) return Promise.resolve(data);
  if (method !== 8) return Promise.reject(new Error('compression non prise en charge'));
  if (typeof DecompressionStream === 'undefined') return Promise.reject(new Error('navigateur trop ancien pour ouvrir les .zip'));
  var stream = new Blob([data]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
  return new Response(stream).arrayBuffer().then(function (b) { return new Uint8Array(b); });
}
function readZip(file) {
  return file.arrayBuffer().then(function (buf) {
    var dv = new DataView(buf), n = buf.byteLength, eocd = -1;
    for (var i = n - 22; i >= 0 && i >= n - 22 - 65535; i--) if (dv.getUint32(i, true) === 0x06054b50) { eocd = i; break; }
    if (eocd < 0) throw new Error('fichier .zip illisible');
    var count = dv.getUint16(eocd + 10, true), off = dv.getUint32(eocd + 16, true);
    var jobs = [];
    for (var k = 0; k < count && off + 46 <= n; k++) {
      if (dv.getUint32(off, true) !== 0x02014b50) break;
      var flags = dv.getUint16(off + 8, true), method = dv.getUint16(off + 10, true);
      var csize = dv.getUint32(off + 20, true), nlen = dv.getUint16(off + 28, true);
      var xlen = dv.getUint16(off + 30, true), clen = dv.getUint16(off + 32, true), lho = dv.getUint32(off + 42, true);
      var name = new TextDecoder(flags & 0x800 ? 'utf-8' : 'latin1').decode(new Uint8Array(buf, off + 46, nlen));
      off += 46 + nlen + xlen + clen;
      var base = name.split('/').pop().split('\\').pop();
      if (!/\.nes$/i.test(base) || (flags & 1)) continue;   // pas un jeu, ou chiffre
      var start = lho + 30 + dv.getUint16(lho + 26, true) + dv.getUint16(lho + 28, true);
      if (start + csize > n) continue;
      jobs.push((function (nm, m, data) {
        return inflate(m, data).then(function (bytes) { return new File([bytes], nm); });
      })(base, method, new Uint8Array(buf, start, csize)));
    }
    return Promise.all(jobs);
  });
}
function expandFiles(files) {
  var out = [], zips = [], other = 0;
  Array.prototype.forEach.call(files, function (f) {
    if (/\.nes$/i.test(f.name)) out.push(f);
    else if (/\.zip$/i.test(f.name)) zips.push(f);
    else other++;
  });
  if (!zips.length) return Promise.resolve({ files: out, other: other, zipNote: '' });
  busy('Ouverture de ' + (zips.length > 1 ? zips.length + ' fichiers .zip' : zips[0].name) + '…');
  var found = 0, bad = [];
  return zips.reduce(function (p, z) {
    return p.then(function () {
      return readZip(z).then(function (list) { found += list.length; out = out.concat(list); })
        .catch(function (e) { bad.push(z.name + ' : ' + e.message); });
    });
  }, Promise.resolve()).then(function () {
    busy('');
    var note = found + ' jeu(x) trouvé(s) dans ' + (zips.length > 1 ? zips.length + ' .zip' : zips[0].name);
    if (bad.length) note += ' — ' + bad[bad.length - 1];
    return { files: out, other: other, zipNote: note, zipBad: bad.length };
  });
}

function uploadFiles(files) {
  expandFiles(files).then(function (r) {
    if (r.zipNote) showToast(r.zipNote, r.zipBad > 0 && r.files.length === 0);
    if (r.files.length === 0) {
      if (!r.zipNote) showToast('Seuls les fichiers .nes et .zip sont acceptés', true);
      return;
    }
    sendList(r.files);
  });
}
function sendList(list) {
  var dest = S.dest;
  if (!avail(dest)) { showToast(WNAME[dest] + ' indisponible', true); return; }
  var wrap = $('uploadProgress'), fill = $('uploadFill'), text = $('uploadText');
  wrap.hidden = false;
  var i = 0, okCount = 0, failCount = 0, room = freeB(dest), lastErr = '';
  // Le dernier message d'erreur est repris dans le bilan final (sinon il
  // serait remplace par un simple "1 jeu(x) en echec", sans la raison).
  function fail(msg) { failCount++; lastErr = msg; showToast(msg, true); }
  function next() {
    if (i >= list.length) {
      wrap.hidden = true;
      refresh();
      if (failCount === 0) showToast(okCount + ' jeu(x) envoyé(s) → ' + WNAME[dest]);
      else if (okCount === 0 && failCount === 1) showToast(lastErr, true);
      else showToast(okCount + ' envoyé(s), ' + failCount + ' en échec (dernier : ' + lastErr + ')', true);
      return;
    }
    var f = list[i];
    if (f.size + MARGIN > room) {
      fail('Pas assez de place (' + WNAME[dest] + ') pour ' + f.name + ' : ' +
           fmtKb(f.size / 1024) + ', ' + fmtKb(room / 1024) + ' libres');
      i++; next(); return;
    }
    text.textContent = 'Envoi de ' + f.name + ' (' + (i + 1) + '/' + list.length + ') → ' + WNAME[dest];
    fill.style.width = '0%';
    var fd = new FormData();
    fd.append('file', f, f.name);
    var xhr = new XMLHttpRequest();
    xhr.open('POST', '/api/upload?dest=' + dest + '&size=' + f.size);
    xhr.upload.onprogress = function (e) { if (e.lengthComputable) fill.style.width = (e.loaded / e.total * 100).toFixed(0) + '%'; };
    xhr.onload = function () {
      var j = null;
      try { j = JSON.parse(xhr.responseText); } catch (err) {}
      if (j && j.ok) { okCount++; room -= Math.ceil(f.size / 4096) * 4096; }
      else fail('Échec de l’envoi de ' + f.name + ' : ' + errText(j));
      i++;
      if (isRootErr(j) && i < list.length) {
        // racine pleine, les suivants echoueraient aussi : arret
        failCount += list.length - i;
        lastErr = errText(j) + ' — ' + (list.length - i) + ' jeu(x) non envoyé(s)';
        i = list.length;
      }
      next();
    };
    xhr.onerror = function () { fail('Erreur réseau pendant l’envoi de ' + f.name); i++; next(); };
    xhr.send(fd);
  }
  next();
}

var dropzone = $('dropzone');
['dragenter', 'dragover'].forEach(function (ev) { dropzone.addEventListener(ev, function (e) { e.preventDefault(); dropzone.classList.add('drag'); }); });
['dragleave', 'drop'].forEach(function (ev) { dropzone.addEventListener(ev, function (e) { e.preventDefault(); dropzone.classList.remove('drag'); }); });
dropzone.addEventListener('drop', function (e) { if (e.dataTransfer.files.length) uploadFiles(e.dataTransfer.files); });
$('fileInput').addEventListener('change', function (e) { if (e.target.files.length) uploadFiles(e.target.files); e.target.value = ''; });

// ---- reseau Wi-Fi de la box ----
function scanPoll(tries) {
  fetch('/api/wifi/scan').then(function (r) { return r.json(); }).then(function (j) {
    if (j.scanning) {
      if (tries > 15) { $('wScan').disabled = false; $('wScan').textContent = 'Chercher'; showToast('Recherche trop longue : réessaie', true); return; }
      setTimeout(function () { scanPoll(tries + 1); }, 1000);
      return;
    }
    $('wScan').disabled = false; $('wScan').textContent = 'Chercher';
    var nets = (j.nets || []).sort(function (a, b) { return b.rssi - a.rssi; });
    var ul = $('nets');
    ul.hidden = false;
    ul.innerHTML = nets.length ? nets.map(function (x) {
      var bars = x.rssi > -60 ? '▂▄▆' : x.rssi > -75 ? '▂▄' : '▂';
      return '<li><button data-ssid="' + escapeHtml(x.ssid) + '">' + escapeHtml(x.ssid) + (x.open ? ' (ouvert)' : '') + '<span>' + bars + '</span></button></li>';
    }).join('') : '<li class="empty" style="padding:10px 4px;color:var(--muted);font-size:13px">Aucun réseau trouvé</li>';
  }).catch(function () {
    /* pendant la recherche, la radio change de canal : une requete peut se perdre */
    if (tries < 15) { setTimeout(function () { scanPoll(tries + 1); }, 1500); return; }
    $('wScan').disabled = false; $('wScan').textContent = 'Chercher'; showToast('Erreur réseau', true);
  });
}
$('wScan').addEventListener('click', function () {
  this.disabled = true; this.textContent = 'Recherche…';
  scanPoll(0);
});
$('nets').addEventListener('click', function (e) {
  var b = e.target.closest('button[data-ssid]');
  if (!b) return;
  $('wSsid').value = b.dataset.ssid;
  $('nets').hidden = true;
  $('wPass').focus();
});
$('wShow').addEventListener('click', function () {
  var p = $('wPass'), on = p.type === 'password';
  p.type = on ? 'text' : 'password';
  this.textContent = on ? 'Cacher' : 'Voir';
  this.setAttribute('aria-pressed', on ? 'true' : 'false');
});
$('wSave').addEventListener('click', function () {
  var ssid = $('wSsid').value, pass = $('wPass').value, apply = $('wApply').checked;
  if (!ssid.trim()) { showToast(ERR.ssid, true); return; }
  if (pass.length && (pass.length < 8 || pass.length > 63)) { showToast(ERR.pass, true); return; }
  post('/api/wifi', { act: 'save', ssid: ssid, pass: pass, apply: apply ? '1' : '0' }).then(function (j) {
    if (!j.ok) { showToast('Non enregistré : ' + errText(j), true); return; }
    $('wPass').value = '';
    if (apply) busy('Enregistré. La console se connecte à « ' + ssid + ' »… Cette page va perdre la console : rejoins ce réseau et ouvre l’adresse affichée sur la console.');
    else { showToast('Enregistré : utilisé au prochain allumage du Wi-Fi'); refresh(); }
  }).catch(function () { showToast('Erreur réseau', true); });
});
$('wForget').addEventListener('click', function () {
  if (!armed(this, 'Confirmer ?', 'Touche « Confirmer ? » sous 3 s : la console reprendra le réseau de wifi_config.h')) return;
  post('/api/wifi', { act: 'forget' }).then(function (j) {
    showToast(j.ok ? 'Réseau oublié : retour à wifi_config.h' : 'Échec', !j.ok);
    refresh();
  }).catch(function () { showToast('Erreur réseau', true); });
});

// ---- mise a jour du logiciel ----
$('binInput').addEventListener('change', function () {
  var f = this.files[0];
  $('binGo').disabled = !f;
  $('binText').textContent = f ? f.name + ' (' + fmtKb(f.size / 1024) + ')' : '';
  $('binProgress').hidden = !f;
  $('binFill').style.width = '0%';
});
$('binGo').addEventListener('click', function () {
  var f = $('binInput').files[0];
  if (!f || this.disabled) return;
  if (!/\.bin$/i.test(f.name)) { showToast(ERR.binext, true); return; }
  if (!armed(this, 'Confirmer ?', 'Touche « Confirmer ? » sous 3 s pour installer ' + f.name)) return;
  var btn = this;
  btn.disabled = true;
  $('binProgress').hidden = false;
  var fd = new FormData();
  fd.append('file', f, f.name);
  var xhr = new XMLHttpRequest();
  xhr.open('POST', '/api/update');
  xhr.upload.onprogress = function (e) {
    if (!e.lengthComputable) return;
    var p = e.loaded / e.total * 100;
    $('binFill').style.width = p.toFixed(0) + '%';
    $('binText').textContent = 'Envoi de ' + f.name + ' : ' + p.toFixed(0) + ' % (ne coupe pas la console)';
  };
  xhr.onload = function () {
    var j = null;
    try { j = JSON.parse(xhr.responseText); } catch (err) {}
    if (j && j.ok) {
      busy('Logiciel installé. La console redémarre… (cette page se recharge toute seule)');
      var t0 = Date.now();
      (function wait() {
        setTimeout(function () {
          fetch('/api/status', { cache: 'no-store' }).then(function () { location.reload(); })
            .catch(function () { if (Date.now() - t0 < 120000) wait(); else busy('La console a redémarré mais ne répond pas ici : si besoin, rejoins de nouveau son réseau, puis rouvre cette page.'); });
        }, 3000);
      })();
    } else {
      btn.disabled = false;
      $('binText').textContent = 'Échec : ' + errText(j);
      showToast('Mise à jour refusée : ' + errText(j), true);
    }
  };
  xhr.onerror = function () { btn.disabled = false; showToast('Connexion coupée pendant l’envoi : rien n’a été changé', true); };
  xhr.send(fd);
});

// ---- Maintenance ----
function wipe(what, extra, msg, done) {
  busy(msg);
  var fields = { what: what };
  if (extra) Object.keys(extra).forEach(function (k) { fields[k] = extra[k]; });
  post('/api/wipe', fields).then(function (j) {
    busy('');
    if (j.ok) showToast(done(j)); else showToast('Échec : ' + errText(j), true);
    refresh();
  }).catch(function () { busy(''); showToast('Erreur réseau', true); refresh(); });
}
$('wipeSdGames').addEventListener('click', function () {
  if (!armed(this, 'Confirmer ?', 'Touche « Confirmer ? » sous 3 s : tous les jeux de la carte SD seront supprimés')) return;
  wipe('sd_games', null, 'Suppression des jeux de la carte SD…', function (j) { return j.count + ' jeu(x) supprimé(s) de la carte SD'; });
});
$('wipeFlash').addEventListener('click', function () {
  if (!armed(this, 'Confirmer ?', 'Touche « Confirmer ? » sous 3 s : la mémoire interne sera effacée')) return;
  wipe('flash', null, 'Effacement de la mémoire interne… (jusqu’à 2 minutes, ne coupe pas la console)', function () { return 'Mémoire interne effacée'; });
});
function updateFmtBtn() { $('fmtSd').disabled = !avail('sd') || $('fmtInput').value.trim().toUpperCase() !== 'FORMATER'; }
$('fmtInput').addEventListener('input', updateFmtBtn);
$('fmtSd').addEventListener('click', function () {
  if (this.disabled) return;
  $('fmtInput').value = '';
  updateFmtBtn();
  wipe('sd_format', { confirm: 'FORMATER' }, 'Formatage de la carte SD en FAT32… (jusqu’à une minute, ne coupe pas la console)', function () { return 'Carte SD formatée'; });
});

renderDest();
refresh();
</script>
</body>
</html>
)HTMLDOC";

// ---------------------------------------------------------------------
// Sanitize minimal d'un nom de fichier venu du navigateur : ne garde que
// le nom de base (jamais de "/", donc jamais de remontee de dossier type
// "../").
static String sanitize_rom_name(String name) {
  int slash = name.lastIndexOf('/');
  if (slash >= 0) name = name.substring(slash + 1);
  int backslash = name.lastIndexOf('\\');
  if (backslash >= 0) name = name.substring(backslash + 1);
  // Caracteres interdits dans un nom FAT changes en '_' (sinon l'ouverture en
  // ecriture echoue sans explication claire).
  for (size_t i = 0; i < name.length(); i++) {
    char c = name[i];
    if (c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || (uint8_t)c < 0x20) name.setCharAt(i, '_');
  }
  return name;
}

// 63 caracteres au plus (taille des noms dans la liste de la console, osd.cpp)
// - un nom plus long serait tronque et injouable.
static bool name_fits(const String &name) { return name.length() > 0 && name.length() <= 63; }

static bool has_nes_extension(const String &name) {
  return name.endsWith(".nes") || name.endsWith(".NES");
}

// "sd" / "flash" -> ROM_WHERE_* (-1 si inconnu)
static int parse_where(const String &w) {
  if (w == "flash") return ROM_WHERE_FLASH;
  if (w == "sd") return ROM_WHERE_SD;
  return -1;
}

// Chaine JSON : echappe " \ et les caracteres de controle.
static void json_str(String &out, const String &s) {
  out += '"';
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if ((uint8_t)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", (unsigned)(uint8_t)c); out += b; }
    else out += c;
  }
  out += '"';
}

static void send_ok(const char *extra = NULL) {
  String j = "{\"ok\":true";
  if (extra) { j += ","; j += extra; }
  j += "}";
  server.send(200, "application/json", j);
}

static void send_err(const char *err, int code = 200) {
  String j = "{\"ok\":false,\"err\":\"";
  j += err;
  j += "\"}";
  server.send(code, "application/json", j);
}

static void handle_root() {
  server.send_P(200, "text/html", PORTAL_HTML);
}

// GET /api/status - espace des deux emplacements, en Ko (entiers : une
// carte de 32 Go fait ~32 millions de Ko, bien en dessous des limites).
static void handle_api_status() {
  String j = "{";
  for (int w = 0; w < 2; w++) {
    uint64_t total = rom_storage_total_bytes(w), freeb = rom_storage_free_bytes(w);
    char b[96];
    snprintf(b, sizeof(b), "%s\"%s\":{\"ok\":%s,\"free\":%lu,\"total\":%lu", w ? "," : "",
             w == ROM_WHERE_FLASH ? "flash" : "sd", rom_storage_available(w) ? "true" : "false",
             (unsigned long)(freeb / 1024), (unsigned long)(total / 1024));
    j += b;
    // Type de FAT ; en FAT12/16, entrees de la racine (noms)
    if (rom_storage_available(w)) {
      int used = 0, rt = 0;
      snprintf(b, sizeof(b), ",\"fat\":%d", rom_storage_fat_type(w));
      j += b;
      if (rom_storage_root_entries(w, &used, &rt)) {
        snprintf(b, sizeof(b), ",\"rootUsed\":%d,\"rootTotal\":%d", used, rt);
        j += b;
      }
    }
    if (w == ROM_WHERE_FLASH && !rom_storage_flash_ok()) {
      // diagnostic (raison + partitions presentes)
      j += ",\"why\":";
      json_str(j, String(rom_storage_flash_reason()));
      j += ",\"detail\":";
      json_str(j, String(rom_storage_flash_detail()));
    }
    j += "}";
  }
  // Version du logiciel et reseau de la box
  j += ",\"version\":";
  json_str(j, String(NES_EMULATOR_VERSION_SHORT));
  j += ",\"wifi\":{\"ssid\":";
  json_str(j, String(wifi_boot_sta_ssid()));
  j += ",\"src\":\"";
  j += wifi_boot_sta_from_portal() ? "portail" : "config";
  j += "\",\"mode\":" + String((int)wifi_boot_get_mode());
  j += ",\"ap\":";
  j += g_wifi_boot_status == WIFI_BOOT_AP_FALLBACK ? "true" : "false";
  j += "}}";
  server.send(200, "application/json", j);
}

// GET /api/games - jeux des deux emplacements (meme filtrage .nes que la
// liste de la console, osd.cpp).
static void handle_api_games() {
  String json = "[";
  bool first = true;
  for (int w = 0; w < 2; w++) {
    fs::FS *fs = rom_storage_fs(w);
    if (!fs) continue;
    File root = fs->open("/");
    if (!root) continue;
    File entry = root.openNextFile();
    while (entry) {
      if (!entry.isDirectory()) {
        String name = entry.name();
        if (has_nes_extension(name)) {
          if (!first) json += ",";
          first = false;
          json += "{\"name\":";
          json_str(json, name);
          json += ",\"size\":" + String((uint32_t)entry.size());
          json += w == ROM_WHERE_FLASH ? ",\"where\":\"flash\"}" : ",\"where\":\"sd\"}";
        }
      }
      entry.close();
      entry = root.openNextFile();
    }
    root.close();
  }
  json += "]";
  server.send(200, "application/json", json);
}

// POST /api/delete (champs "name", "where") - supprime un .nes.
static void handle_api_delete() {
  String name = sanitize_rom_name(server.arg("name"));
  int where = server.hasArg("where") ? parse_where(server.arg("where")) : ROM_WHERE_SD;
  if (!has_nes_extension(name) || where < 0) { send_err("ext", 400); return; }
  char path[ROM_PATH_MAX];
  if (!rom_storage_make_path(where, name.c_str(), path, sizeof(path))) { send_err("name", 400); return; }
  bool ok = rom_storage_remove(path);
  Serial.printf("[PORTAIL] Suppression de \"%s\" : %s\n", path, ok ? "OK" : "ECHEC");
  if (ok) { send_ok(); g_portal_games_changed = true; }
  else send_err("nofile");
}

// POST /api/move (champs "name", "from") - deplace un jeu vers l'autre
// emplacement. Bloquant pendant la copie (quelques secondes au plus).
static void handle_api_move() {
  String name = sanitize_rom_name(server.arg("name"));
  int from = parse_where(server.arg("from"));
  if (!has_nes_extension(name) || from < 0) { send_err("ext", 400); return; }
  int to = from == ROM_WHERE_SD ? ROM_WHERE_FLASH : ROM_WHERE_SD;
  int r = rom_storage_move(name.c_str(), from, to);
  static const char *errs[] = {"", "unavail", "nofile", "exists", "space", "io"};
  if (r == ROM_MOVE_OK) { send_ok(); g_portal_games_changed = true; }
  else if (r == ROM_MOVE_ERR_ROOT) send_err(to == ROM_WHERE_FLASH ? "rootflash" : "rootsd");
  else send_err(r > 0 && r <= ROM_MOVE_ERR_IO ? errs[r] : "io");
}

// POST /api/wipe (champ "what" : "sd_games" | "flash" | "sd_format" ; pour
// "sd_format", le champ "confirm" doit valoir "FORMATER" - deuxieme
// verrou cote console, en plus de celui de la page).
static void handle_api_wipe() {
  String what = server.arg("what");
  if (upload_file) upload_file.close();   // aucun fichier ouvert pendant l'operation
  if (what == "sd_games") {
    int n = rom_storage_delete_all_games(ROM_WHERE_SD);
    if (n < 0) { send_err("unavail"); return; }
    char b[24];
    snprintf(b, sizeof(b), "\"count\":%d", n);
    send_ok(b);
  } else if (what == "flash") {
    if (!rom_storage_flash_ok()) { send_err("unavail"); return; }
    if (!rom_storage_flash_erase()) { send_err("io"); g_portal_games_changed = true; return; }
    send_ok();
  } else if (what == "sd_format") {
    if (server.arg("confirm") != "FORMATER") { send_err("confirm", 400); return; }
    if (!rom_storage_sd_ok()) { send_err("unavail"); return; }
    if (!rom_storage_sd_format()) { send_err("io"); g_portal_games_changed = true; return; }
    send_ok();
  } else {
    send_err("what", 400);
    return;
  }
  g_portal_games_changed = true;
}

// Reponse a la requete POST /api/upload elle-meme (une fois le fichier
// entierement recu par handle_upload_data ci-dessous) : le VRAI resultat.
static void handle_api_upload_done() {
  if (!upload_success) {
    Serial.printf("[PORTAIL] Upload signale en echec au navigateur (%s).\n", upload_err[0] ? upload_err : "?");
    send_err(upload_err[0] ? upload_err : "write");
    return;
  }
  send_ok();
  g_portal_games_changed = true;
}

// Coeur de l'upload : WebServer.h appelle cette fonction plusieurs fois
// PENDANT la reception d'une requete multipart (UPLOAD_FILE_START, puis
// UPLOAD_FILE_WRITE plusieurs fois, puis UPLOAD_FILE_END). Les parametres
// de l'adresse (?dest=sd|flash&size=...) sont deja lus a ce moment-la.
static void upload_discard() {
  fs::FS *fs = upload_where >= 0 ? rom_storage_fs(upload_where) : NULL;
  if (fs && upload_tmp_path.length()) fs->remove(upload_tmp_path);
}

// Racine pleine (FAT12/16 : nombre fixe de noms, voir rom_storage.h) : code
// d'erreur pour la page, ou NULL. need : entrees de repertoire qu'il faut
// encore trouver libres.
static int s_upload_need = 0;
static const char *root_full_err(int where, int need) {
  int used = 0, total = 0;
  if (!rom_storage_root_entries(where, &used, &total) || total - used >= need) return NULL;
  Serial.printf("[PORTAIL] Racine pleine (%s, FAT%d) : %d/%d entrees, %d necessaire(s)\n",
                where == ROM_WHERE_FLASH ? "memoire interne" : "carte SD", rom_storage_fat_type(where), used, total, need);
  return where == ROM_WHERE_FLASH ? "rootflash" : "rootsd";
}

static void handle_upload_data() {
  HTTPUpload &upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    upload_success = false;
    upload_write_error = false;
    upload_err = "";
    upload_file = File();
    upload_where = -1;
    upload_tmp_path = "";
    upload_final_path = "";
    String name = sanitize_rom_name(upload.filename);
    if (!has_nes_extension(name)) {
      Serial.printf("[PORTAIL] Upload refuse (extension) : \"%s\"\n", upload.filename.c_str());
      upload_err = "ext";
      return;
    }
    if (!name_fits(name)) {
      Serial.printf("[PORTAIL] Upload refuse (nom de plus de 63 caracteres) : \"%s\"\n", name.c_str());
      upload_err = "name";
      return;
    }
    int where = server.hasArg("dest") ? parse_where(server.arg("dest")) : ROM_WHERE_SD;
    fs::FS *fs = where >= 0 ? rom_storage_fs(where) : NULL;
    if (!fs) { upload_err = "unavail"; return; }
    // Place : verifiee ici quand le navigateur annonce la taille (sinon,
    // une ecriture incomplete est detectee plus bas).
    if (server.hasArg("size")) {
      uint64_t size = (uint64_t)strtoul(server.arg("size").c_str(), NULL, 10);
      if (size + 16 * 1024 > rom_storage_free_bytes(where)) {
        Serial.printf("[PORTAIL] Upload refuse : pas assez de place (%s)\n", where == ROM_WHERE_FLASH ? "memoire interne" : "carte SD");
        upload_err = "space";
        return;
      }
    }
    upload_where = where;
    upload_final_path = "/" + name;
    // Nom temporaire COURT (une entree de repertoire au lieu de 3 ou 4 pour
    // "nom.nes~tmp") : compte en FAT12/16, ou la racine a un nombre fixe
    // d'entrees.
    upload_tmp_path = ROM_UPLOAD_TMP_NAME;
    fs->remove(upload_tmp_path);   // reste d'un envoi precedent interrompu
    // Place pour le nom a la racine (FAT12/16) - refus tout de suite plutot
    // qu'apres l'envoi de tout le fichier.
    s_upload_need = rom_storage_name_entries(name.c_str()) + 1;
    if (fs->exists(upload_final_path)) s_upload_need += rom_storage_name_entries((name + ROM_UPLOAD_OLD_SUFFIX).c_str());
    const char *rf = root_full_err(where, s_upload_need);
    if (rf) {
      upload_err = rf;
      upload_where = -1;
      return;
    }
    Serial.printf("[PORTAIL] Upload demarre : %s (%s)\n", upload_final_path.c_str(), where == ROM_WHERE_FLASH ? "memoire interne" : "carte SD");
    upload_file = fs->open(upload_tmp_path, FILE_WRITE);
    if (!upload_file) {
      Serial.println("[PORTAIL] Impossible d'ouvrir le fichier en ecriture.");
      const char *rf2 = root_full_err(where, s_upload_need + 8);   // trous entre les entrees
      upload_err = rf2 ? rf2 : "open";
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (upload_file) {
      // Nombre d'octets REELLEMENT ecrits : un support plein ecrit moins
      // que demande sans erreur bloquante.
      size_t written = upload_file.write(upload.buf, upload.currentSize);
      if (written != upload.currentSize) {
        if (!upload_write_error) {
          Serial.printf("[PORTAIL] Erreur d'ecriture pendant l'upload (%u/%u octets ecrits - plein ?)\n",
                        (unsigned)written, (unsigned)upload.currentSize);
        }
        upload_write_error = true;
        upload_err = "write";
      }
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!upload_file) { upload_success = false; return; }
    upload_file.close();
    fs::FS *fs = rom_storage_fs(upload_where);
    bool ok = fs && !upload_write_error && upload.totalSize > 0;
    if (ok) {
      // close() ne signale pas un echec de la derniere ecriture (tampon
      // vide sur un support plein) : on relit la taille reelle.
      File chk = fs->open(upload_tmp_path, FILE_READ);
      ok = chk && (size_t)chk.size() == upload.totalSize;
      if (chk) chk.close();
      if (!ok) upload_err = "write";
    }
    if (ok) {
      // Le jeu du meme nom eventuellement present n'est remplace qu'une fois le
      // nouveau fichier complet : il est d'abord mis de cote (~old), et remis en
      // place si le renommage du nouveau echoue.
      String old_path = upload_final_path + ROM_UPLOAD_OLD_SUFFIX;
      bool had_old = fs->exists(upload_final_path);
      if (had_old) {
        fs->remove(old_path);
        ok = fs->rename(upload_final_path, old_path);
      }
      if (ok) {
        ok = fs->rename(upload_tmp_path, upload_final_path);
        if (ok) { if (had_old) fs->remove(old_path); }
        else if (had_old) fs->rename(old_path, upload_final_path);
      }
      if (!ok) {
        // Le plus souvent, racine pleine (FAT12/16)
        const char *rf = root_full_err(upload_where, s_upload_need + 8);
        upload_err = rf ? rf : "io";
      }
    }
    if (!ok) upload_discard();
    upload_success = ok;
    Serial.printf("[PORTAIL] Upload termine : %u octets (%s)\n", (unsigned)upload.totalSize, ok ? "OK" : "ECHEC");
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    // Connexion coupee en cours d'envoi (WiFi instable, page fermee).
    Serial.println("[PORTAIL] Upload interrompu (connexion coupee).");
    if (upload_file) {
      upload_file.close();
      upload_discard();
    }
    upload_success = false;
    upload_err = "io";
  }
}

// ======================= jeux ecartes, Wi-Fi, mise a jour =======================
// GET /api/quarantine - jeux ecartes apres un plantage (rom_storage.h).
static String s_q_json;
static bool s_q_first;
static void q_add(const char *name, int where, uint32_t size, void *ctx) {
  (void)ctx;
  if (!s_q_first) s_q_json += ",";
  s_q_first = false;
  s_q_json += "{\"name\":";
  json_str(s_q_json, String(name));
  s_q_json += ",\"size\":" + String(size);
  s_q_json += where == ROM_WHERE_FLASH ? ",\"where\":\"flash\"}" : ",\"where\":\"sd\"}";
}
static void handle_api_quarantine_get() {
  s_q_json = "[";
  s_q_first = true;
  rom_storage_quarantine_list(q_add, NULL);
  s_q_json += "]";
  server.send(200, "application/json", s_q_json);
  s_q_json = String();
}

// POST /api/quarantine (champs "act" : restore | delete, "name", "where")
static void handle_api_quarantine_post() {
  String name = sanitize_rom_name(server.arg("name"));
  const int where = parse_where(server.arg("where"));
  const String act = server.arg("act");
  if (!has_nes_extension(name) || where < 0) { send_err("ext", 400); return; }
  if (act == "delete") {
    if (rom_storage_quarantine_remove(where, name.c_str())) send_ok();
    else send_err("nofile");
    return;
  }
  if (act != "restore") { send_err("act", 400); return; }
  const int r = rom_storage_quarantine_restore(where, name.c_str());
  if (r == ROM_MOVE_OK) {
    char path[ROM_PATH_MAX];
    if (rom_storage_make_path(where, name.c_str(), path, sizeof(path))) crash_guard_forget(path);
    send_ok();
    g_portal_games_changed = true;
    return;
  }
  static const char *errs[] = {"", "unavail", "nofile", "exists", "space", "io"};
  if (r == ROM_MOVE_ERR_ROOT) send_err(where == ROM_WHERE_FLASH ? "rootflash" : "rootsd");
  else send_err(r > 0 && r <= ROM_MOVE_ERR_IO ? errs[r] : "io");
}

// GET /api/wifi/scan - reseaux Wi-Fi alentour (recherche en arriere-plan :
// la page redemande toutes les secondes tant que "scanning").
static void handle_api_wifi_scan() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_FAILED) {
    WiFi.scanNetworks(true, false);
    server.send(200, "application/json", "{\"scanning\":true}");
    return;
  }
  if (n == WIFI_SCAN_RUNNING) {
    server.send(200, "application/json", "{\"scanning\":true}");
    return;
  }
  String j = "{\"scanning\":false,\"nets\":[";
  int shown = 0;
  for (int i = 0; i < n && shown < 30; i++) {
    const String ssid = WiFi.SSID(i);
    if (!ssid.length()) continue;
    bool dup = false;   // meme nom deja liste (plusieurs bornes) : le plus fort suffit
    for (int k = 0; k < i && !dup; k++) dup = WiFi.SSID(k) == ssid && WiFi.RSSI(k) >= WiFi.RSSI(i);
    if (dup) continue;
    if (shown++) j += ",";
    j += "{\"ssid\":";
    json_str(j, ssid);
    j += ",\"rssi\":" + String(WiFi.RSSI(i));
    j += WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? ",\"open\":true}" : ",\"open\":false}";
  }
  j += "]}";
  WiFi.scanDelete();
  server.send(200, "application/json", j);
}

// POST /api/wifi : act=save (ssid, pass, apply=1 : connexion tout de suite)
// ou act=forget (retour au reseau de wifi_config.h).
static void handle_api_wifi() {
  const String act = server.arg("act");
  if (act == "forget") {
    wifi_boot_forget_credentials();
    send_ok();
    return;
  }
  if (act != "save") { send_err("act", 400); return; }
  const String ssid = server.arg("ssid"), pass = server.arg("pass");
  const int r = wifi_boot_set_credentials(ssid.c_str(), pass.c_str());
  if (r == -1) { send_err("ssid"); return; }
  if (r == -2) { send_err("pass"); return; }
  if (r != 0) { send_err("io"); return; }
  if (server.arg("apply") == "1") {
    if (wifi_boot_get_mode() == NESWIFI_MODE_AP) wifi_boot_set_mode(NESWIFI_MODE_AUTO);   // la box doit etre essayee
    g_portal_wifi_reconnect = true;
  }
  send_ok();
}

// POST /api/update - mise a jour du logiciel de la console (fichier .bin de
// l'Arduino IDE : Croquis > Exporter les binaires compiles,
// nes_emulator.ino.bin). Controles : en-tete d'image ESP (0xE9) et puce
// ESP32-S3 (un .bin de la carte video, ESP32, est refuse) ; l'image entiere
// est verifiee par Update.end() avant d'etre activee. Redemarrage 1,5 s
// apres la reponse. Les reglages (NVS), jeux et sauvegardes ne bougent pas.
static const char *s_upd_err = "";
static bool s_upd_ok = false, s_upd_active = false;
// Mise a jour du logiciel : l'en-tete (octet magique 0xE9) est controle sur
// le PREMIER morceau recu seulement. Update.progress() ne peut pas servir a
// le reperer : la bibliotheque Update ne compte les octets qu'une fois un
// secteur de 4 Ko ecrit.
static bool s_upd_hdr_checked = false;
static uint32_t s_upd_next_log = 0;
static uint32_t s_restart_at = 0;

static void handle_api_update_done() {
  if (!s_upd_ok) {
    Serial.printf("[MAJ] Echec : %s\n", s_upd_err[0] ? s_upd_err : "?");
    send_err(s_upd_err[0] ? s_upd_err : "io");
    return;
  }
  send_ok();
  s_restart_at = (millis() + 1500) | 1;
}

static void handle_update_data() {
  HTTPUpload &u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    s_upd_ok = false;
    s_upd_err = "";
    s_upd_active = false;
    s_upd_hdr_checked = false;
    s_upd_next_log = 256 * 1024;
    if (!u.filename.endsWith(".bin")) { s_upd_err = "binext"; return; }
    if (upload_file) upload_file.close();
    Serial.printf("[MAJ] Reception de %s...\n", u.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) { s_upd_err = "begin"; Update.printError(Serial); return; }
    s_upd_active = true;
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (!s_upd_active) return;
    if (!s_upd_hdr_checked) {
      // 1er morceau (1436 octets en pratique) : en-tete d'image ESP
      // (esp_image_header_t) : magique 0xE9, identifiant de puce (octets
      // 12-13) = 9 pour l'ESP32-S3.
      s_upd_hdr_checked = true;
      if (u.currentSize < 16 || u.buf[0] != 0xE9) {
        Serial.printf("[MAJ] Refuse : pas une image ESP (1er octet 0x%02X, %u octets).\n",
                      u.currentSize ? u.buf[0] : 0, (unsigned)u.currentSize);
        s_upd_err = "notimg"; Update.abort(); s_upd_active = false; return;
      }
      const uint16_t chip = (uint16_t)(u.buf[12] | (u.buf[13] << 8));
      if (chip != 9) {
        Serial.printf("[MAJ] Refuse : logiciel pour une autre puce (identifiant %u, 9 attendu).\n", (unsigned)chip);
        s_upd_err = "chip"; Update.abort(); s_upd_active = false; return;
      }
    }
    if (Update.write(u.buf, u.currentSize) != u.currentSize) {
      s_upd_err = "write";
      Update.printError(Serial);
      Update.abort();
      s_upd_active = false;
      return;
    }
    if (u.totalSize + u.currentSize >= s_upd_next_log) {
      Serial.printf("[MAJ] %u Ko recus\n", (unsigned)((u.totalSize + u.currentSize) / 1024));
      s_upd_next_log += 256 * 1024;
    }
  } else if (u.status == UPLOAD_FILE_END) {
    if (!s_upd_active) return;
    s_upd_active = false;
    if (!s_upd_hdr_checked) { s_upd_err = "notimg"; Update.abort(); return; }   // fichier vide
    if (Update.end(true)) {
      s_upd_ok = true;
      Serial.printf("[MAJ] Logiciel recu et verifie (%u octets) : redemarrage.\n", (unsigned)u.totalSize);
    } else {
      s_upd_err = "verify";
      Update.printError(Serial);
    }
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    if (s_upd_active) Update.abort();
    s_upd_active = false;
    s_upd_err = "io";
  }
}

// Journal de chaque requete servie (port serie) et compteur affiche dans
// l'onglet Portail : si un telephone est relie au point d'acces mais que le
// compteur reste a 0, ses demandes n'arrivent pas jusqu'a la console (sur
// Android, elles partent souvent par les donnees mobiles quand le reseau Wi-Fi
// n'a pas acces a Internet). L'adresse du client est relevee avant la reponse
// (connexion parfois deja fermee apres). Compteur lu et ecrit par la seule
// tache Arduino (handleClient() tourne dans la boucle des menus) : pas besoin
// de volatile.
static uint32_t s_request_count = 0;

static bool log_request(WebServer &s, Middleware::Callback next) {
  const uint32_t t0 = millis();
  const IPAddress from = s.client().remoteIP();
  const bool handled = next();
  s_request_count++;
  Serial.printf("[PORTAIL] %s %s de %s : code %d en %lu ms (memoire interne libre %u octets, minimum %u)\n",
                http_method_str(s.method()), s.uri().c_str(), from.toString().c_str(), s.responseCode(),
                (unsigned long)(millis() - t0),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  return handled;
}

static void handle_not_found() {
  server.send(404, "text/plain; charset=utf-8", "Adresse inconnue de la console NES. Ouvrez la page d'accueil : /");
}

uint32_t wifi_portal_request_count() {
  return s_request_count;
}

// Garde-fou : wifi_portal_start() peut etre rappelee sans empiler des
// gestionnaires en double (WebServer.h ne deduplique pas).
// Les routes ne sont inscrites qu'une fois par mise sous tension - avant,
// chaque arret/redemarrage du Wi-Fi depuis les Reglages les inscrivait de
// nouveau (sans gene visible : la premiere repond).
static bool s_portal_started = false;
static bool s_routes_registered = false;

void wifi_portal_start() {
  if (s_portal_started) return;
  if (!s_routes_registered) {
    s_routes_registered = true;
    server.addMiddleware(log_request);
    server.on("/", HTTP_GET, handle_root);
    server.on("/api/status", HTTP_GET, handle_api_status);
    server.on("/api/games", HTTP_GET, handle_api_games);
    server.on("/api/delete", HTTP_POST, handle_api_delete);
    server.on("/api/move", HTTP_POST, handle_api_move);
    server.on("/api/wipe", HTTP_POST, handle_api_wipe);
    server.on("/api/upload", HTTP_POST, handle_api_upload_done, handle_upload_data);
    server.on("/api/quarantine", HTTP_GET, handle_api_quarantine_get);
    server.on("/api/quarantine", HTTP_POST, handle_api_quarantine_post);
    server.on("/api/wifi/scan", HTTP_GET, handle_api_wifi_scan);
    server.on("/api/wifi", HTTP_POST, handle_api_wifi);
    server.on("/api/update", HTTP_POST, handle_api_update_done, handle_update_data);
    server.onNotFound(handle_not_found);
  }
  server.begin();
  s_portal_started = true;
  Serial.println("[PORTAIL] Serveur web demarre (port 80).");
}

void wifi_portal_loop() {
  server.handleClient();
  // Logiciel mis a jour : redemarrage une fois la reponse partie
  if (s_restart_at && (int32_t)(millis() - s_restart_at) >= 0) {
    Serial.println("[MAJ] Redemarrage sur le nouveau logiciel (Wi-Fi rallume au demarrage).");
    wifi_boot_keep_on_after_restart();
    Serial.flush();
    delay(100);
    ESP.restart();
  }
  // Memoire interne une fois par minute tant que le portail tourne.
  static uint32_t next_mem_log = 0;
  if (s_portal_started && (int32_t)(millis() - next_mem_log) >= 0) {
    next_mem_log = millis() + 60000;
    char when[80];
    if (g_wifi_boot_status == WIFI_BOOT_AP_FALLBACK)
      snprintf(when, sizeof(when), "point d'acces, %d appareil(s) relie(s), %lu requete(s) servie(s)",
               wifi_boot_ap_clients(), (unsigned long)s_request_count);
    else
      snprintf(when, sizeof(when), "portail actif, %lu requete(s) servie(s)", (unsigned long)s_request_count);
    wifi_boot_log_memory(when);
  }
}

void wifi_portal_stop() {
  server.stop();
  s_portal_started = false;
  Serial.println("[PORTAIL] Serveur web arrete.");
}

#else  // !ENABLE_WIFI_PORTAL

// Talons vides : permet a osd.cpp d'appeler ces fonctions sans les
// entourer de "#if ENABLE_WIFI_PORTAL" a chaque appel.
void wifi_portal_start() {}
void wifi_portal_loop() {}
void wifi_portal_stop() {}
uint32_t wifi_portal_request_count() { return 0; }

#endif  // ENABLE_WIFI_PORTAL
