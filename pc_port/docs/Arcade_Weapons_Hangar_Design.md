# Modalità arcade — missili, armi sostituite, hangar

Documento di design. Nulla di questo è ancora implementato: descrive cosa
costruire sopra la modalità arcade esistente (`flight_gameplay`, vedi
`Flight_HUD_Ideas.md`), in che ordine e con quali punti da decidere.

Stato: `[ ]` da fare · `[~]` implementato, da provare in gioco · `[x]` provato in gioco

---

## Obiettivo

Con la modalità arcade accesa, Harry combatte come un caccia di Ace Combat:
le armi da fuoco dell'originale diventano tipi di missile a ricerca (termica,
radar, …), e ai punti di salvataggio si apre un **hangar** dove scegliere
l'equipaggiamento.

## Principi

- **Tutto dietro `flight_gameplay`.** Spenta (default), il gioco è quello PSX,
  byte per byte dove possibile. Nessun codice originale viene rimosso.
- **La logica dell'inventario e della trama non si tocca.** Oggetti, pickup,
  chiavi, eventi legati agli oggetti (`g_ItemTriggerItemIds`) restano quelli
  originali. Cambia cosa *fa* un'arma equipaggiata, non cosa *è* per il gioco.
- **Codice nuovo nei file del port** (`pc_flight_arcade.c`, `pc_flight_hud.c`,
  file nuovi per l'hangar). Nel codice decompilato solo agganci piccoli e
  dietro `#ifdef SH_PC_PORT`.
- **I danni passano per le funzioni del gioco**, come già fanno i missili
  attuali (`Ar_HitNpc`): animazioni di ferita e morte restano quelle originali.
- Branch: tutto il comune su `pc-port`; touch e packaging APK su `android-port`.

## Cosa esiste già

| Cosa | Dove |
|------|------|
| Missili di Harry: guida, scia, esplosione, scorte 2 + ricarica 12 s | `pc_flight_arcade.c` (`Ar_HarryLaunch`, `Ar_FlyHarry`) |
| Missili dei mostri, flare che li deviano | `pc_flight_arcade.c` (`Ar_Fly`, `Af_PickDecoy`) |
| Seeker: aggancio, cambio bersaglio | `pc_flight_hud.c` (`Pc_FlightHud_SeekerLockedSlot`, `Pc_FlightHud_NextTarget`) |
| Mitragliatrice con surriscaldamento | `pc_flight_arcade.c` (`Ar_GunFire`, `Af_GunTick`) |
| Fisica comune dei missili | `pc_flight_missile.c` (`Af_MissileInit`, `Af_MissileStep`) |
| Modelli `.glb` per inventario, esame, raccolta | `pc_modern_mesh.c`, guida `Modern_Item_GLTF_Modding_Guide.md` |
| Touch: M (missile) e G (mitragliatrice) al posto di Mira | `pc_touch.c` (solo `android-port`) |

---

## 1. Tipi di missile

Ogni tipo è una variante dello stesso `AfMissile`, con parametri propri e una
regola di aggancio. Valori di partenza, da bilanciare giocando.

| Tipo | Sigla HUD | Portata | Aggancio | Velocità / virata | Danno | Contromisure |
|------|-----------|---------|----------|-------------------|-------|--------------|
| Termico (IR) | `IRM` | 20 m | 0,5 s, anche fuori asse (±30°) | 14 m/s, virata alta (5 rad/s) | 1× | ingannato da flare e fonti di calore vicine |
| Radar | `RDR` | 45 m | 1,5 s, serve linea di vista; un muro rompe l'aggancio | 22 m/s, virata bassa (2,5 rad/s) | 1,5× | non ingannato dai flare, perde il bersaglio dietro i muri |
| Testata pesante | `HVY` | 15 m | 1,0 s | 10 m/s, virata media | 3×, onda d'urto (danno ad area 2 m) | come IR |
| Speciale (Hyper Blaster) | `QAM` | 30 m | istantaneo | 18 m/s, virata altissima | 2× | nessuna; munizioni infinite con ricarica lenta |

`1×` = un missile di oggi (due colpi di fucile da caccia, `AR_HARRY_DMG_MULT`).

Regole comuni:

- **Distanza minima di innesco** 2 m: più vicino il missile non esplode
  (passa e si perde). Il corpo a corpo resta alla mitragliatrice e alle armi
  bianche (vedi §2).
- **Collisione con i muri** (oggi solo pavimento, limite noto): serve per il
  radar e per non colpire attraverso le pareti. Raycast lungo il passo del
  missile, come la collisione della telecamera TPS in `game_main.c`.
- Il seeker resta unico; il **tipo selezionato** decide portata e tempo di
  aggancio mostrati dall'HUD (il riquadro LOCK si riempie in base al tipo).

## 2. Armi originali → missili

Le armi da fuoco restano oggetti dell'inventario e si raccolgono dove
nell'originale. Raccolte, **sbloccano** un tipo di missile; equipaggiate,
lo selezionano.

| Oggetto originale | Diventa | Munizioni originali → |
|-------------------|---------|-----------------------|
| Handgun | Termico (IR) | Handgun Bullets: +1 IR per scatola (15 colpi) |
| Hunting Rifle | Radar | Rifle Shells: +1 RDR per scatola |
| Shotgun | Testata pesante | Shotgun Shells: +1 HVY per scatola |
| Hyper Blaster | Speciale | — (ricarica propria) |

- Scorte per tipo con un massimo (es. IR 4, RDR 2, HVY 2) e ricarica lenta
  nel tempo come oggi. Le munizioni raccolte riempiono subito.
- Il conteggio munizioni dell'inventario originale resta quello che è: la
  conversione avviene al momento della raccolta in un contatore del port,
  salvato a parte (vedi §7).
- **Armi bianche**: restano originali (corpo a corpo). Mitragliatrice (G / X)
  sempre disponibile. *Da decidere*: togliere anche quelle?
- **Arma in mano a Harry**: con un missile equipaggiato non si vede alcuna
  arma (i missili partono dalla spalla). Più semplice che sostituire il
  modello tenuto in mano, che non passa dal sistema `.glb`.
- **Mirare** (R2/Aim) con un missile equipaggiato: *da decidere* se resta
  (posa di mira senza arma) o se Aim diventa il cambio bersaglio.

### Comandi

| Azione | Pad (schema AC7) | Touch |
|--------|------------------|-------|
| Lancia missile del tipo selezionato | Cerchio (come oggi) | M |
| Mitragliatrice | X | G |
| Cambia tipo di missile | *da definire*: in AC7 è Quadrato, ma in SH1 Quadrato è la corsa | tocco lungo su M, o un piccolo pulsante accanto |
| Cambia bersaglio | Triangolo tocco (come oggi) | tocco sul bersaglio a schermo (da valutare) |

L'HUD mostra il tipo selezionato al posto di `MSL` (`IRM 3`, `RDR 1`…).

## 3. Boss e bilanciamento

Ogni boss va provato. Note già trovate nel codice:

- **Split Head** (`split_head.c:207`): a bocca aperta prende danno ×16, ×32
  se l'arma *equipaggiata* è il fucile a pompa (non a Hard). Con lo shotgun
  che diventa HVY il bonus si applica da solo se l'oggetto equipaggiato resta
  lo Shotgun: va verificato e voluto.
- **Floatstinger**, **Twinfeeler**: attacchi e aree vulnerabili propri.
- **Cybil posseduta**: con i missili muore in fretta; va tarato il danno o
  limitato il tipo usabile, perché ucciderla cambia il finale.
- I boss oggi sono esclusi dai missili *nemici* (`Pc_FlightHud_IsBoss`); i
  missili di Harry invece li colpiscono. Da confermare boss per boss.

Bilanciamento generale: le scorte devono bastare per i mostri normali senza
rendere inutile la mitragliatrice; i boss non devono cadere in pochi secondi.

## 4. Statistiche e rank

Il rank finale usa colpi sparati, colpi a corta/media/lunga distanza,
uccisioni corpo a corpo e a distanza (`firedShotCount`, `closeRangeShotCount`…).
In arcade: un missile lanciato conta come un colpo sparato, un missile a segno
come colpo a distanza secondo la distanza di lancio, un'uccisione da missile
come uccisione a distanza. Così il rank resta significativo senza toccarne il
calcolo.

## 5. Modelli e testi dell'inventario

- Modelli: `.glb` con i nomi degli oggetti originali (`UNQA0` Handgun, `UNQA1`
  Hunting Rifle, `UNQA2` Shotgun, `UNQA3` Hyper Blaster, `UNQC0..C2`
  munizioni). Vincoli in `Modern_Item_GLTF_Modding_Guide.md`: una mesh rigida,
  una texture PNG incorporata, max 16 MB.
- **Vanno caricati solo in arcade.** Oggi i `.glb` stanno in
  `gamedata/load/ITEM/` e valgono sempre. Proposta: cartella separata
  `gamedata/arcade/ITEM/`, cercata per prima solo con `flight_gameplay`
  acceso; spento, il gioco non la guarda.
- **Testi**: nome e descrizione degli oggetti ("IR Missile", "Radar Missile"…)
  sostituiti in arcade, nello stesso punto dove il port già traduce i nomi
  (`lang_pack.c`), con una tabella arcade dedicata.

## 6. Hangar al punto di salvataggio

### Flusso

Oggi (`SysState_SaveMenu_Update`, `game_sys_states.c`): la prima volta
compare "Vuoi salvare?" (Sì/No), poi dissolvenza e schermata degli slot.

In arcade, ogni volta:

```
  ▸ SAVE
    HANGAR
    CANCEL
```

- **SAVE**: il flusso originale, invariato (il messaggio della prima volta
  non compare più: la scelta l'ha già fatta il menu).
- **HANGAR**: apre l'hangar; all'uscita torna a questo menu.
- **CANCEL**: torna al gioco.

Il gioco è già fermo in quel momento, nessun nemico può attaccare.
Casi da controllare: il salvataggio Next Fear (`SaveLocationId_NextFear`)
e i taccuini con `g_MapEventParam == 0`, che oggi saltano il messaggio.

### Schermata

```
┌──────────────────────────────────────────────────────────────┐
│ HANGAR                                       SAVE POINT: CAFE │
│                                                              │
│                     [ modello 3D che ruota ]                 │
│                     ═══════ piattaforma ═══════              │
│                                                              │
│  ▸ IR MISSILE      4/4      RANGE  ██████░░░░                │
│    RADAR MISSILE   1/2      SPEED  ████████░░                │
│    HEAVY WARHEAD   --       TURN   █████████░                │
│                             DMG    ████░░░░░░                │
│                             LOCK   █████████░                │
│                                                              │
│  M: IR MISSILE                       [EQUIP]  [BACK]         │
└──────────────────────────────────────────────────────────────┘
```

- Sfondo: un'immagine a schermo intero (PNG) di un hangar. Un ambiente 3D
  vero è fuori portata per ora.
- Modello: il `.glb` del tipo selezionato, grande, su una piattaforma;
  ruota da solo, lo stick destro / il trascinamento lo gira.
- Tipi non ancora sbloccati: in lista come `--`, non selezionabili.
- Barre delle statistiche dai parametri del §1, normalizzati.
- Disegno con le primitive e il font dell'HUD di volo.
- Comandi: su/giù scorre, Croce equipaggia, Triangolo/Cerchio esce. Touch:
  tocco sulla voce, trascinamento per ruotare, pulsante BACK.
- *Da decidere*: entrare nell'hangar ricarica le scorte ("rifornimento alla
  base")? Più tematico, ma rende i taccuini più potenti.

## 7. Salvataggio dei dati arcade

Tipi sbloccati, scorte per tipo e tipo selezionato vanno salvati con la
partita. Il salvataggio PSX non ha spazio libero affidabile: file a parte
accanto al salvataggio, con lo stesso slot (come fanno le altre estensioni
del port, se ce ne sono; altrimenti un file `.arcade` per slot). Partita
caricata senza file arcade: si ricava lo stato dalle armi nell'inventario.

## 8. Asset e distribuzione

- Gli asset del port stanno in `pc_port/assets/` con la stessa struttura
  della cartella del gioco. CMake li copia accanto all'eseguibile; l'APK li
  include (`android_port/app/build.gradle`, `assets.srcDirs`) e
  `SilentHillActivity` li scompatta all'avvio. I modelli arcade andrebbero in
  `pc_port/assets/gamedata/arcade/ITEM/`, lo sfondo in
  `pc_port/assets/gamedata/arcade/hangar.png`.
- **Problema da risolvere prima**: `SilentHillActivity.copyAssetFile` copia
  un file solo se non esiste già. Un modello aggiornato in un nuovo APK non
  sostituirebbe quello vecchio sul telefono. Soluzione: un file
  `assets_version` scritto alla copia; se la versione dell'APK è più nuova,
  ricopiare i file del port (preservando quelli che l'utente ha modificato
  di proposito, es. `pl.lang`, oppure limitando la ricopia a `arcade/`).
- **Licenze**: solo asset nostri o con licenza compatibile (CC0/CC-BY);
  annotarli in `pc_port/assets/licenses/`. Nessun asset preso dal gioco.
- Peso: un modello low-poly con texture 512–1024 px pesa 100–500 KB; lo
  sfondo PNG 1–2 MB. L'APK resta piccolo.
- Sorgenti dei modelli: script generatori (es. Blender in modalità batch)
  in `pc_port/tools/arcade_models/`, così i `.glb` si rigenerano e si
  correggono senza lavoro manuale.

---

## Fasi

| # | Cosa | Branch | Stato |
|---|------|--------|-------|
| 1 | Tipi di missile (parametri, aggancio per tipo, collisione muri, innesco minimo) | `pc-port` | [ ] |
| 2 | Armi → missili: sblocco, munizioni → scorte, selezione, HUD | `pc-port` | [ ] |
| 3 | Comandi cambio tipo (pad) | `pc-port` | [ ] |
| 3b | Comandi cambio tipo (touch) | `android-port` | [ ] |
| 4 | Menu SAVE / HANGAR / CANCEL al taccuino | `pc-port` | [ ] |
| 5 | Schermata hangar (sfondo piatto, lista, statistiche, modello) | `pc-port` | [ ] |
| 5b | Hangar touch | `android-port` | [ ] |
| 6 | Modelli `.glb`, cartella `arcade/`, testi | `pc-port` | [ ] |
| 7 | Copia asset aggiornati nell'APK (`assets_version`) | `android-port` | [ ] |
| 8 | Salvataggio dati arcade | `pc-port` | [ ] |
| 9 | Boss, rank e bilanciamento (playtest) | `pc-port` | [ ] |

## Domande aperte

1. Armi bianche: restano o spariscono in arcade?
2. Aim con un missile equipaggiato: posa di mira, cambio bersaglio, o niente?
3. Tasto per cambiare tipo di missile sul pad (Quadrato è la corsa).
4. L'hangar ricarica le scorte?
5. Cybil: limitare i missili contro di lei o solo tararne il danno?
6. Quanti tipi: i quattro del §1 bastano, o servono varianti (es. a grappolo)?
