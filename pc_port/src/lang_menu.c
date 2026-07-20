#include "lang_text.h"

#include <string.h>

#include "game.h"
#include "bodyprog/text/text_draw.h"
#include "font_region.h"
#include "main/fileinfo.h"
#include "pc_config.h"

/* Port-written menu translations (DE/FR/ES/IT). Retail PAL shipped ENGLISH
 * menus in every language (probe-verified from its OPTION.BIN/SAVELOAD.BIN)
 * — the disc has no menu strings to reuse — so this table is our own
 * translation layer, applied through the Gfx_StringDraw chokepoint: any
 * drawn string that exactly matches a key renders translated, everything
 * else (PC Options, ranking, debug) stays English. Keys are the exact
 * compiled literals, including \x07 color prefixes and \x01 kerning bytes.
 *
 * Writing rules for entries:
 * - '_' is the space stand-in, '\n' breaks lines (US menu dialect).
 * - Accents are Latin-1 bytes resolved by the PAL font (font_region.c).
 *   Uppercase accents beyond A-acute/E-acute/A-O-U-diaeresis degrade to a
 *   combining mark + '*', so translations avoid them (French caps drop
 *   accents per its own typography).
 * - A hex escape followed by a hex-digit letter must be split ("\xFC" "ber"),
 *   or C consumes the letter into the escape.
 * - NULL = keep the English text for that language. */

typedef struct {
    const char* us;
    const char* tr[4]; /* de, fr, es, it */
} s_MenuTranslation;

static const s_MenuTranslation s_MenuTr[] = {
    /* --- Title menu + difficulty --- */
    { "LOAD",     { "LADEN",  "CHARGER",   "CARGAR",    "CARICA"    } },
    { "CONTINUE", { "WEITER", "CONTINUER", "CONTINUAR", "CONTINUA"  } },
    { "START",    { NULL,     "COMMENCER", "EMPEZAR",   "INIZIA"    } },
    { "OPTION",   { "OPTIONEN", "OPTIONS", "OPCIONES",  "OPZIONI"   } },
    /* Port-added main-menu row that closes the game. Distinct key from the
     * mixed-case "Exit" below, which is the options screen's BACK button
     * (German "Zur\xFC" "ck") — these must not share a translation. */
    { "EXIT",     { "BEENDEN", "QUITTER",  "SALIR",     "ESCI"      } },
    { "EASY",     { "LEICHT", "FACILE",    "FACIL",     "FACILE"    } },
    { "NORMAL",   { NULL,     NULL,        NULL,        "NORMALE"   } },
    { "HARD",     { "SCHWER", "DIFFICILE", "DIFICIL",   "DIFFICILE" } },

    /* --- Options screens --- */
    { "OPTION_\x01\x01\x01\x01\x01S",
                  { "OPTIONEN", "OPTIONS", "OPCIONES", "OPZIONI" } },
    { "EXTRA_OPTION_\x01\x01\x01\x01\x01S",
                  { "EXTRA-OPTIONEN", "OPTIONS_EXTRA", "OPCIONES_EXTRA", "OPZIONI_EXTRA" } },
    { "Exit",              { "Zur\xFC" "ck",       "Quitter",         "Salir",            "Esci"              } },
    { "Brightness_Level",  { "Helligkeit",         "Luminosit\xE9",   "Brillo",           "Luminosit\xE0"     } },
    { "Controller_Config", { "Controller-Konfig.", "Config._manette", "Config._de_mando", "Config._controller" } },
    { "Vibration",         { NULL,                 NULL,              "Vibraci\xF3n",     "Vibrazione"        } },
    { "Auto_Load",         { "Auto-Laden",         "Chargement_auto", "Carga_autom.",     "Caricam._auto"     } },
    { "Language",          { "Sprache",            "Langue",          "Idioma",           "Lingua"            } },
    { "Sound",             { "Ton",                "Son",             "Sonido",           "Audio"             } },
    { "BGM_Volume",        { "Musiklautst\xE4rke", "Volume_musique",  "Volumen_m\xFAsica", "Volume_musica"    } },
    { "SE_Volume",         { "Effektlautst.",      "Volume_effets",   "Volumen_efectos",  "Volume_effetti"    } },
    { "Voice",             { "Stimmen",            "Voix",            "Voces",            "Voci"              } },
    { "On",                { "Ein",                "Oui",             "S\xED",            "S\xEC"             } },
    { "Off",               { "Aus",                "Non",             "No",               "No"                } },
    { "Stereo",            { NULL,                 "St\xE9r\xE9o",    "Est\xE9reo",       NULL                } },
    { "Monaural",          { "Mono",               "Mono",            "Mono",             "Mono"              } },

    /* --- Extra options --- */
    { "Weapon_Control", { "Waffensteuerung", "Contr\xF4le_arme", "Control_de_arma", "Controllo_arma" } },
    { "Blood_Color",    { "Blutfarbe",       "Couleur_sang",     "Color_de_sangre", "Colore_sangue"  } },
    { "View_Control",   { "Blicksteuerung",  "Contr\xF4le_vue",  "Control_vista",   "Controllo_vista" } },
    { "Retreat_Turn",   { "Kehrtwende",      "Demi-tour",        "Media_vuelta",    "Dietrofront"    } },
    { "\x01W\x01a\x01l\x01k/R\x01\x01u\x01n_\x01\x01\x01\x01" "Co\x01n\x01t\x01ro\x01l",
                        { "Gehen/Rennen",    "Marche/Course",    "Andar/Correr",    "Cammina/Corri"  } },
    { "Auto_Aiming",    { "Auto-Zielen",     "Vis\xE9" "e_auto", "Punter\xED" "a_auto", "Mira_autom." } },
    { "View_Mode",      { "Ansichtsmodus",   "Mode_de_vue",      "Modo_de_vista",   "Modo_visuale"   } },
    { "Bullet_Adjust",  { "Munitionsbonus",  "Bonus_munitions",  "Ajuste_de_balas", "Regola_proiett." } },
    { "Press",          { "Dr\xFC" "cken",   "Presser",          "Pulsar",          "Premi"          } },
    { "Switch",         { "Umschalten",      "Bascule",          "Alternar",        "Alterna"        } },
    { "Normal",         { NULL,              NULL,               NULL,              "Normale"        } },
    { "Green",          { "Gr\xFCn",         "Vert",             "Verde",           "Verde"          } },
    { "Violet",         { "Violett",         NULL,               "Violeta",         "Viola"          } },
    { "Black",          { "Schwarz",         "Noir",             "Negra",           "Nero"           } },
    { "Reverse",        { "Invertiert",      "Invers\xE9",       "Invertido",       "Invertito"      } },
    { "Self_View",      { "Egosicht",        "Vue_subj.",        "Vista_propia",    "Soggettiva"     } },

    /* --- Pause --- */
    { "\x07PAUSE",  { NULL,               NULL,                 "\x07PAUSA",    "\x07PAUSA"    } },
    { "\x07PAUSED", { "\x07PAUSIERT",     "\x07" "EN_PAUSE",    "\x07" "EN_PAUSA", "\x07IN_PAUSA" } },

    /* --- Save dialogs (inventory save + saveload screen) --- */
    { "\x07Is_it_OK_to_save?",
        { "\x07Wirklich_speichern?", "\x07Sauvegarder_?", "\x07\xBFGuardar_partida?", "\x07Salvare?" } },
    { "\x07Yes_____________No",
        { "\x07Ja____________Nein", "\x07Oui____________Non", "\x07S\xED____________No", "\x07S\xEC____________No" } },
    { "\x07Yes__________No",
        { "\x07Ja_________Nein", "\x07Oui_________Non", "\x07S\xED_________No", "\x07S\xEC_________No" } },
    { "NEXT_GAME_MODE",
        { "N\xC4" "CHSTER_MODUS", "MODE_SUIVANT", "SIGUIENTE_MODO", "MODO_SUCCESSIVO" } },
    { "\x07Is_it_OK_to_overwrite?",
        { "\x07Wirklich_\xFC" "berschreiben?", "\x07Vraiment_\xE9" "craser_?", "\x07\xBFSobrescribir?", "\x07Sovrascrivere?" } },
    { "\x07Is_it_OK_to_format?",
        { "\x07Wirklich_formatieren?", "\x07Vraiment_formater_?", "\x07\xBF" "Formatear?", "\x07" "Formattare?" } },

    /* --- Saveload screen chrome + memory card dialogs --- */
    { "FILE", { "DATEI", "FICHIER", "ARCHIVO", NULL   } },
    { "Data", { "Daten", "Fich.",   "Datos",   "Dati" } },
    { "Save", { "Stand", "Sauv.",   "Guard",   "Salv." } },
    { "Time", { "Zeit",  "Temps",   "Hora",    "Ora"  } },
    { "You_need_1_free_block\n__to_create_a_new_file.",
        { "1_freier_Block_wird\n__ben\xF6tigt.",
          "1_bloc_libre_requis\n__pour_cr\xE9" "er_un_fichier.",
          "Se_necesita_1_bloque\n__libre_para_crear.",
          "Serve_1_blocco_libero\n__per_un_nuovo_file." } },
    { "\x07You_\x01\x01removed_\x01\x01the_\x01\x01MEMORY_\x01\x01" "CARD!",
        { "\x07Memory_Card_entfernt!", "\x07" "Carte_m\xE9moire_retir\xE9" "e_!",
          "\x07\xA1Memory_Card_retirada!", "\x07Memory_Card_rimossa!" } },
    { "\x07Now_formatting...",
        { "\x07" "Formatiere...", "\x07" "Formatage...", "\x07" "Formateando...", "\x07" "Formattazione..." } },
    { "\x07Now_saving...",
        { "\x07Speichere...", "\x07Sauvegarde...", "\x07Guardando...", "\x07Salvataggio..." } },
    { "\x07Unable_to_create_a_new_file.",
        { "\x07" "Datei_nicht_erstellbar.", "\x07" "Cr\xE9" "ation_impossible.",
          "\x07No_se_pudo_crear.", "\x07Impossibile_creare." } },
    { "\x07" "Finished_saving.",
        { "\x07Gespeichert.", "\x07Sauvegarde_termin\xE9" "e.", "\x07Guardado_completado.", "\x07Salvataggio_completato." } },
    { "\x07" "Failed_to_save!",
        { "\x07Speichern_fehlgeschlagen!", "\x07" "\xC9" "chec_de_sauvegarde_!",
          "\x07\xA1" "Error_al_guardar!", "\x07Salvataggio_fallito!" } },
    { "\x07The_data_is_not_found!",
        { "\x07Keine_Daten_gefunden!", "\x07" "Donn\xE9" "es_introuvables_!",
          "\x07" "Datos_no_encontrados!", "\x07" "Dati_non_trovati!" } },
    { "\x07The_data_is_damaged!",
        { "\x07" "Daten_besch\xE4" "digt!", "\x07" "Donn\xE9" "es_endommag\xE9" "es_!",
          "\x07" "Datos_da\xF1" "ados!", "\x07" "Dati_danneggiati!" } },
    { "\x07" "Failed_to_load!",
        { "\x07Laden_fehlgeschlagen!", "\x07" "\xC9" "chec_du_chargement_!",
          "\x07\xA1" "Error_al_cargar!", "\x07" "Caricamento_fallito!" } },
    { "\x07" "Finished_loading.",
        { "\x07Geladen.", "\x07" "Chargement_termin\xE9.", "\x07" "Carga_completada.", "\x07" "Caricamento_completato." } },
    { "\x07Now_loading...",
        { "\x07Lade...", "\x07" "Chargement...", "\x07" "Cargando...", "\x07" "Caricamento..." } },

    /* --- Save location names --- */
    { "Anywhere",     { "Irgendwo",      "N'importe_o\xF9", "Donde_sea",     "Ovunque"       } },
    { "Cafe",         { NULL,            "Caf\xE9",         "Caf\xE9",       "Caff\xE8"      } },
    { "Store",        { "Laden",         "Magasin",         "Tienda",        "Negozio"       } },
    { "Infirmary",    { "Krankenzimmer", "Infirmerie",      "Enfermer\xED" "a", "Infermeria" } },
    { "Doghouse",     { "Hundeh\xFCtte", "Niche",           "Caseta",        "Cuccia"        } },
    { "Church",       { "Kirche",        "\xC9" "glise",    "Iglesia",       "Chiesa"        } },
    { "Garage",       { NULL,            NULL,              "Taller",        NULL            } },
    { "Police",       { "Polizei",       NULL,              "Polic\xED" "a", "Polizia"       } },
    { "Reception",    { "Rezeption",     "R\xE9" "ception", "Recepci\xF3n",  NULL            } },
    { "Room_302",     { "Zimmer_302",    "Chambre_302",     "Cuarto_302",    "Stanza_302"    } },
    { "Director's",   { "Direktion",     "Bureau_dir.",     "Direcci\xF3n",  "Direzione"     } },
    { "Jewelry_shop", { "Juwelier",      "Bijouterie",      "Joyer\xED" "a", "Gioielleria"   } },
    { "Pool_hall",    { "Billardsalon",  "Billard",         "Billar",        "Sala_biliardo" } },
    { "Antique_shop", { "Antiquit\xE4ten", "Antiquaire",    "Antig\xFC" "edades", "Antiquario" } },
    { "Theme_park",   { "Freizeitpark",  "Parc",            "Parque",        "Luna_park"     } },
    { "Boat",         { "Boot",          "Bateau",          "Barco",         "Barca"         } },
    { "Bridge",       { "Br\xFC" "cke",  "Pont",            "Puente",        "Ponte"         } },
    { "Lighthouse",   { "Leuchtturm",    "Phare",           "Faro",          "Faro"          } },
    { "Sewer",        { "Kanal",         "\xC9" "gouts",    "Cloaca",        "Fogna"         } },
    { "Nowhere",      { "Nirgendwo",     "Nulle_part",      "Ninguna_parte", "Nessun_luogo"  } },
    { "Child's_room", { "Kinderzimmer",  "Ch._enfant",      "Cuarto_ni\xF1o", "Camera_bimbo" } },
    { "Next_fear",    { "N\xE4" "chste_Angst", "Peur_suivante", "Nuevo_miedo", "Nuova_paura" } },

    /* --- Inventory commands + labels --- */
    { "Use",       { "Benutzen",       "Utiliser",        "Usar",     "Usa"        } },
    { "Equip",     { "Ausr\xFCsten",   "\xC9" "quiper",   "Equipar",  "Equipaggia" } },
    { "Unequip",   { "Ablegen",        "Retirer",         "Quitar",   "Rimuovi"    } },
    { "Reload",    { "Nachladen",      "Recharger",       "Recargar", "Ricarica"   } },
    { "Detail",    { "Details",        "D\xE9" "tails",   "Detalles", "Dettagli"   } },
    { "Look",      { "Ansehen",        "Regarder",        "Mirar",    "Guarda"     } },
    { "Equipment", { "Ausr\xFCstung",  "\xC9" "quipement", "Equipo",  "Dotazione"  } },
    { "Option",    { "Optionen",       "Options",         "Opciones", "Opzioni"    } },
    { "Map",       { "Karte",          "Carte",           "Mapa",     "Mappa"      } },
    { "Command",   { "Befehle",        "Commandes",       "Comandos", "Comandi"    } },
    { "Status",    { "Zustand",        "\xC9" "tat",      "Estado",   "Stato"      } },
    { "Name:",     { NULL,             "Nom_:",           "Nombre:",  "Nome:"      } },

    /* --- PC Options (port-added screens — translated per user request; the
     * value labels shared with retail rows reuse the entries above) --- */
    { "PC_Options",        { "PC-Optionen",        "Options_PC",          "Opciones_PC",        "Opzioni_PC"     } },
    { "Resolution",        { "Aufl\xF6sung",       "R\xE9solution",       "Resoluci\xF3n",      "Risoluzione"    } },
    { "Window_Mode",       { "Fenstermodus",       "Mode_fen\xEAtre",     "Modo_ventana",       "Modo_finestra"  } },
    { "Texture_Filter",    { "Texturfilter",       "Filtre_texture",      "Filtro_textura",     "Filtro_texture" } },
    { "Antialiasing",      { NULL,                 "Anticr\xE9nelage",    "Antialias",          "Antialias"      } },
    { "Post_Process",      { "Nachbearbeitung",    "Post-traitement",     "Postproceso",        "Post-processo"  } },
    { "Tone_Mapping",      { NULL,                 NULL,                  "Mapeo_tonal",        NULL             } },
    { "Next_Page",         { "N\xE4" "chste_Seite", "Page_suivante",      "Sig._p\xE1gina",     "Pagina_succ."   } },
    { "Prev_Page",         { "Vorherige_Seite",    "Page_pr\xE9" "c.",    "P\xE1gina_ant.",     "Pagina_prec."   } },
    { "Back",              { "Zur\xFC" "ck",       "Retour",              "Atr\xE1s",           "Indietro"       } },
    { "FPS_Limit",         { NULL,                 "Limite_FPS",          "L\xEDmite_FPS",      "Limite_FPS"     } },
    { "Disable_Culling",   { "Culling_aus",        "Sans_culling",        "Sin_culling",        "No_culling"     } },
    { "Preload_Chunks",    { "Chunks_vorladen",    "Pr\xE9" "chargement", "Precarga",           "Precarica"      } },
    { "PP_Flashlight",     { "PP-Taschenlampe",    "Lampe_PP",            "Linterna_PP",        "Torcia_PP"      } },
    { "PP_Shadows",        { "PP-Schatten",        "Ombres_PP",           "Sombras_PP",         "Ombre_PP"       } },
    { "Beam_Intensity",    { "Lichtst\xE4rke",     "Intensit\xE9",        "Intensidad",         "Intensit\xE0"   } },
    { "Beam_Size",         { "Lichtkegel",         "Taille_faisceau",     "Tama\xF1o_del_haz",  "Ampiezza"       } },
    { "First_Person_FOV",  { "Ego-FOV",            "FOV_1re_pers.",       "FOV_1ra_pers.",      "FOV_1a_pers."   } },
    { "Mouse_Sensitivity", { "Maus-Sensibilit\xE4t", "Sensib._souris",    "Sensib._rat\xF3n",   "Sensib._mouse"  } },
    { "Pad_Sensitivity",   { "Pad-Sensibilit\xE4t", "Sensib._manette",    "Sensib._mando",      "Sensib._pad"    } },
    { "Invert_Mouse_Y",    { "Maus_Y_invert.",     "Inverser_Y_souris",   "Invertir_Y_rat\xF3n", "Inverti_Y_mouse" } },
    { "Invert_Pad_Y",      { "Pad_Y_invert.",      "Inverser_Y_pad",      "Invertir_Y_mando",   "Inverti_Y_pad"  } },
    { "Aim_Assist",        { "Zielhilfe",          "Aide_\xE0_la_vis\xE9" "e", "Ayuda_de_apunt.", "Assist._mira"  } },
    { "Crosshair",         { "Fadenkreuz",         "R\xE9ticule",         "Ret\xED" "cula",     "Mirino"         } },
    { "2D_Controls",       { "2D-Steuerung",       "Contr\xF4les_2D",     "Controles_2D",       "Controlli_2D"   } },
    { "FMV_Movie_Vol",     { "FMV-Lautst\xE4rke",  "Volume_FMV",          "Volumen_FMV",        "Volume_FMV"     } },
    { "Windowed",          { "Fenster",            "Fen\xEAtre",          "Ventana",            "Finestra"       } },
    { "Fullscreen",        { "Vollbild",           "Plein_\xE9" "cran",   "Completa",           "Intero"         } },
    { "Borderless",        { "Randlos",            "Sans_bord",           "Sin_bordes",         "Senza_bordi"    } },
    { "Bilinear",          { NULL,                 "Bilin\xE9" "aire",    "Bilineal",           "Bilineare"      } },
    { "External",          { "Extern",             "Externe",             "Externa",            "Esterna"        } },
    { "In_Game",           { "Im_Spiel",           "En_jeu",              "En_juego",           "In_gioco"       } },
    { "Both",              { "Beide",              "Les_deux",            "Ambas",              "Entrambe"       } },
    { "Scanlines",         { NULL,                 "Balayage",            "L\xEDneas",          "Scanline"       } },
    { "Vignette",          { NULL,                 NULL,                  "Vi\xF1" "eta",       "Vignetta"       } },
    { "Color_Grade",       { "Farbton",            "\xC9talonnage",       "Color",              "Colore"         } },
    { "Film_Grain",        { "Filmkorn",           "Grain",               "Grano",              "Grana"          } },
    { "Sharpen",           { "Sch\xE4rfen",        "Nettet\xE9",          "Nitidez",            "Nitidezza"      } },
    { "Cinematic",         { "Kino",               "Cin\xE9ma",           "Cine",               "Cinema"         } },
    { "Filmic",            { "Filmisch",           "Filmique",            "F\xEDlmico",         "Filmico"        } },

    /* --- Paper-map prompts --- */
    { "Too_dark_to_look_at\n\t\tthe_map_here.",
        { "Zu_dunkel,_um_die\n\t\tKarte_zu_lesen.",
          "Trop_sombre_pour\n\t\tlire_la_carte.",
          "Demasiado_oscuro\n\t\tpara_ver_el_mapa.",
          "Troppo_buio_per\n\t\tleggere_la_mappa." } },
    { "I_don't_have_the_map\n\t\tfor_this_place.",
        { "Ich_habe_keine_Karte\n\t\tvon_diesem_Ort.",
          "Je_n'ai_pas_la_carte\n\t\tde_ce_lieu.",
          "No_tengo_el_mapa\n\t\tde_este_lugar.",
          "Non_ho_la_mappa\n\t\tdi_questo_posto." } },
};

const char* Pc_LangMenuText(const char* str)
{
    int lang = g_PcConfig.language;
    int i;

    /* EUR discs always; USA only when a fan-translated disc is active (its
     * story/item text comes from the disc, these tables cover the menus the
     * patch can't reach — the port renders menus from compiled strings). */
    if (!(g_GameRegion == Region_EUR || (g_GameRegion == Region_USA && Pc_FanTextActive())) ||
        lang < 1 || lang > 4 || str == NULL)
    {
        return str;
    }

    for (i = 0; i < (int)(sizeof(s_MenuTr) / sizeof(s_MenuTr[0])); i++)
    {
        if (s_MenuTr[i].us[0] == str[0] && strcmp(s_MenuTr[i].us, str) == 0)
        {
            return s_MenuTr[i].tr[lang - 1] ? s_MenuTr[i].tr[lang - 1] : str;
        }
    }

    return str;
}

/* Pixel width of a (single-line prefix of a) menu string, for the centered
 * title/difficulty entries whose US x-offsets are hardcoded constants. */
int Pc_LangMenuTextWidth(const char* str)
{
    int         width = 0;
    s_GlyphEmit emits[2];
    int         count;
    int         k;
    unsigned char c;

    for (; *str != '\0'; str++)
    {
        c = (unsigned char)*str;

        if (c == '\n')
            break;
        if (c == '_')
        {
            width += FONT_12X16_SPACE_SIZE;
            continue;
        }
        if (c == '\v')
        {
            width += 10;
            continue;
        }
        if (c == '\x01')
        {
            width -= 1;
            continue;
        }
        if (c < 0x08)
            continue; /* color codes */

        if (c == '!')
            c = '\\';
        else if (c == '&')
            c = '^';

        if (c >= GLYPH_TABLE_ASCII_OFFSET && (c <= 'z' || c >= 0x80))
        {
            count = Font_MapChar(c, emits);
            for (k = 0; k < count; k++)
            {
                width += emits[k].advance;
            }
        }
    }

    return width;
}
