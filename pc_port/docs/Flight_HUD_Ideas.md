# Flight HUD — guida e idee

L'HUD stile caccia (`flight_hud`): cosa mostra in gioco, stato delle idee,
dove sta il codice. Aggiornare lo stato quando una voce viene provata o
cambiata (e annotare il commit).

Stato: `[ ]` da fare · `[~]` implementato, da provare in gioco · `[x]` provato in gioco

---

## Cosa vedi in gioco

L'HUD si vede solo mentre si gioca: sparisce in pausa, nei menu, nella mappa e
nelle scene d'intermezzo. Si sceglie con **Flight HUD** nel menu rapido
(pagina HUD & Audio): **Modern** (default), **Classic** o **Off**.

### Sempre a schermo (stile Modern)

| Dove | Cosa | Cosa significa |
|------|------|----------------|
| In alto a sinistra | `TIME: 00:05:55` | Tempo di gioco della partita (quello del salvataggio). |
| | `SCORE: 1000` | 1000 punti per ogni mostro ucciso. |
| | `TARGET: GROANER +1000` | Il mostro più vicino entro 40 m. Assente se non ce ne sono. |
| | `TGT REMAINING: 3` | Mostri vivi caricati nella zona. Assente se non ce ne sono. |
| Ai lati del centro | `SPEED` | Velocità di Harry in km/h (0 da fermo). |
| | `ALT` | Altezza di Harry in piedi rispetto al suolo della mappa (0 al piano terra). |
| Centro | Mirino tondo | Puramente decorativo; sparisce se miri con il crosshair attivo. |
| | `SHOOT` | Compare sotto il mirino se stai mirando e il bersaglio più vicino è lì, entro 20 m. |
| In basso a sinistra | Radar quadrato | Harry è il triangolo al centro, rivolto in alto. I triangoli rossi sono i mostri (fino a 25 m). N/E/S/W ruotano mentre Harry gira. |
| In basso a destra | `> HANDGUN 12/40` | Arma in mano, colpi nel caricatore / colpi di riserva. Per le armi corpo a corpo `---`. |
| | `FLR 4` | Flare disponibili (massimo 4). Accanto, una barra bordata che si riempie: dopo 10 s torna un flare. Su telefono sta sotto `FLR` nella riga in basso. |
| | `DMG 28%` | Danno subito: 100 meno la salute di Harry. |
| | Harry in wireframe | Il modello vero di Harry a fil di ferro, fermo in piedi e girato di tre quarti. Verde (salute ≥ 75), giallo (≥ 50), arancione (≥ 25), rosso lampeggiante (sotto 25). Se il modello non è disponibile, la sagoma stilizzata di prima. |

Su telefono, con i controlli touch, la colonna in basso a destra diventa una
riga in basso al centro (`DMG 74%  HANDGUN 8/0  FLR 4` con Harry accanto),
perché quell'angolo è occupato dai pulsanti.

### In prima persona

Solo con la telecamera in prima persona (non nelle scene d'intermezzo):

- **Scala di beccheggio** al centro: una linea ogni 5° con i gradi ai due capi,
  piene sopra l'orizzonte e tratteggiate sotto, con le stanghette rivolte
  verso l'orizzonte. L'orizzonte è la linea lunga senza numeri. Sale e scende
  con lo sguardo e si inclina se la vista si inclina (movimento della testa).
  Si vedono circa ±15° attorno a dove guardi.
- **Nastro della bussola** in alto: la direzione dello sguardo in gradi nel
  riquadro, i punti cardinali (N, NE, E, SE, S, SW, W, NW) e i gradi ogni 15°
  ai lati. Guardando a 199° si legge `S 199 SW`.
- Nello stile Classic il nastro della bussola prende il posto del suo nastro
  della direzione, che segue invece il corpo di Harry.
- Su telefono con Quick Save / Quick Load attivi, il riquadro radio scende sotto
  il nastro.

### Sui mostri

- Ogni mostro entro 40 m e inquadrato ha un **riquadrino** con `TGT` sopra e
  la distanza in metri a destra.
- Il più vicino ha anche un **rombo**, il **nome** (o il callsign, vedi sotto)
  e una **barra della sua salute**.
- Se il mostro è fuori inquadratura, una **freccia sul bordo dello schermo**
  indica da che parte è, con la distanza.

### Quando miri (armi da fuoco)

- Sul mostro scelto dalla mira automatica del gioco compare un **rombo** largo
  che in 0,6 s si stringe sul suo riquadro, con un **ronzio basso** che
  tremola. Quando è chiuso diventa rosso, lampeggia **LOCK ON** e il tono
  diventa **acuto e fisso**.
- **HIT** sale dal mostro quando lo colpisci senza ucciderlo (all'uccisione
  resta DESTROYED). **MISS** sopra il mirino quando un colpo non fa danno a
  nessuno entro 0,4 s.

### Danno e morte

- Quando Harry viene colpito, l'HUD **trema e si strappa** (scanline) per un
  attimo, in proporzione al danno, e lampeggia **DAMAGE**.
- Sotto 25 di salute lampeggia in rosso **WARNING: LOW HEALTH** con un doppio
  bip lento (se nessun altro tono suona).
- Quando Harry muore: statica, tutto rosso, poi **MISSION FAILED** e
  **SIGNAL LOST**, con Cybil alla radio ("HARRY! HARRY, RESPOND!"), finché il
  GAME OVER del gioco prende lo schermo.

I toni hanno una priorità: allarmi dei mostri, poi seeker, poi salute bassa.

### Quando un mostro ti punta

1. Un mostro entro 12 m che guarda verso Harry inizia a "puntarlo": compare
   **WARNING** al centro, il suo riquadro lampeggia, e due volte al secondo si
   sente un breve trillo su due note.
2. Se continua a guardarlo per quasi un secondo **ha fatto lock**: lampeggia
   **MISSILE ALERT**, **tutto l'HUD diventa rosso** (anche crosshair e
   pulsanti touch), bip veloci su una nota, circa 8 al secondo, e Cybil avvisa
   via radio. Il mostro sul radar e nel riquadro diventa `LOCK`.
3. Se chi ha fatto lock è a meno di 2,5 m compare anche **EVADE**, i bordi
   dello schermo pulsano di rosso e il tono diventa una sirena continua che
   sale e scende.

I toni sono generati dal gioco stesso (vedi "Dove sta il codice") e seguono il
volume degli effetti.

La sagoma di Harry resta del colore della sua salute anche quando tutto il
resto è rosso.

### Flare

- **L3 + R3 insieme** (i due stick premuti), o il pulsante **F** sul touch.
- Partono scie luminose dietro Harry, compare **FLARE**, e ogni lock si rompe:
  per 3 secondi nessun mostro può agganciarti di nuovo.
- Se non ne hai compare **NO FLARES**.
- Con l'HUD attivo, un click singolo su L3 (menu rapido) o R3 (cambio camera)
  funziona al rilascio del tasto, così la combinazione dei flare non li attiva.

### Eventi

| Cosa | Quando |
|------|--------|
| **DESTROYED** sul mostro e **+1000** che vola verso lo SCORE | Ogni uccisione. |
| **Radio** in alto al centro: nome di chi parla, frase tra `<< >>`, ritratto a destra | Cybil parla al primo lock, a un'uccisione, con salute sotto 25, a caricatore vuoto, all'ultimo flare, in una zona nuova, a boss sconfitto. A volte, prima di lei, parla in rosso il mostro che ti ha agganciato ("RAWR!", "SKREEEE!"). Ogni messaggio resta circa 3,5 s. |
| **MISSION UPDATE** con il nome della zona | La prima volta che entri in una zona durante la sessione (non quella in cui carichi la partita). |
| **MISSION COMPLETE** | Dopo un boss, o la prima volta che entri in un nuovo capitolo: tempo, punteggio, uccisioni, colpi sparati / a segno, precisione e voto S/A/B/C. Resta 7 s e non blocca il gioco. Non compare se nel tratto non hai combattuto. |

### Modalità arcade (`flight_gameplay`)

Spenta di default: senza, l'HUD è solo scena. Accesa (e con l'HUD attivo):

- **Flare con effetto vero**: per i 3 s in cui i flare accecano i seeker,
  nessun colpo di un mostro arriva a Harry (niente danno né animazione).
  I boss non ne sono toccati. Una presa già in corso non si rompe.
- **Missili dei mostri**: un mostro che tiene il lock per 1,2 s lancia un
  missile a ricerca (lo stesso mostro di nuovo dopo 6 s, al massimo 3 in aria,
  mai i boss). Vira piano: correndo di lato si schiva. Un flare entro 8 m lo
  prende e lo fa esplodere lontano da Harry. Se colpisce fa il danno
  dell'attacco normale di quel mostro, con l'animazione del colpo al busto
  (mai una presa). Finché è in volo restano MISSILE ALERT, EVADE e la sirena;
  sul missile c'è un **rombo rosso** che lampeggia più veloce man mano che si
  avvicina.
- **Missili di Harry**: con un'arma da fuoco in mira e il seeker chiuso
  (LOCK ON), **Cerchio** (il tasto della torcia) lancia un missile sul
  bersaglio; in quel momento la torcia non cambia. Senza LOCK ON Cerchio resta
  la torcia. 2 missili, uno torna ogni 12 s; `MSL` accanto a `FLR`,
  `NO MISSILES` a stock vuoto. Accanto a MSL due icone di missile: piene se
  pronte, vuote se lanciate, quella in ricarica si riempie man mano. Colpisce come due colpi di fucile.
- **Scie**: fumo chiaro nella scena (coperto da muri e nebbia) e bagliore
  sulla testa del missile nell'HUD (arancio i mostri, verde Harry).
- Limite noto: i missili non collidono con i muri, solo con il pavimento.

### Ritratto della radio

- Il ritratto è il **volto vero del personaggio**, ritagliato dall'immagine del
  gioco, con scanline e una tinta del colore dell'HUD.
- Il volto viene "fotografato" quando il personaggio è vicino, inquadrato e
  girato verso la camera, anche durante le scene d'intermezzo. Il primo piano
  di Cybil si prende nella scena iniziale del bar.
- Il mostro che ti aggancia appare **in diretta** se è inquadrato; altrimenti
  si vede il suo miglior primo piano già catturato.
- I primi piani si salvano in `gamedata/hud_portraits/` e restano dopo il
  riavvio.
- Finché non c'è un primo piano si vede un **busto stilizzato a fil di ferro**.

**Ritratto 3D (`flight_hud_portrait_3d`, acceso di default).**
Il riquadro mostra il **modello 3D vero** di chi parla, convertito al volo dal
pool globale dei personaggi e disegnato dall'HUD in una sua texture, con una sua
telecamera (tre quarti, testa e spalle, leggera oscillazione) e una sua luce: non
può uscire dal riquadro e non passa dal renderer del gioco. Il mostro in scena
appare con la sua posa dal vivo; Cybil e chi non è in scena sono un "manichino"
nella prima posa della loro animazione. Richiede `global_chara_pool = 1`
(default); senza, resta il ritaglio dall'immagine.

### Stile Classic

Stesse funzioni con un altro aspetto: nastro della bussola in alto, nastri
scorrevoli di SPEED e ALT, mirino a "W", radar rotondo in basso a destra,
`DMG` con barra della salute in basso a sinistra e Harry in wireframe accanto
(su telefono accanto a `DMG` nella riga in basso).

### Opzioni

Tutte le opzioni qui sotto stanno nella pagina **Flight** delle Opzioni (dopo
HUD) e del menu rapido (dopo HUD): Flight HUD, Arcade Mode, Warning Tones, HUD
Opacity, Target Labels, 3D Radio Portrait.

| Opzione (`config.cfg`) | Valori |
|------------------------|--------|
| `flight_hud` | 0 off, 1 Modern (default), 2 Classic. Anche nel menu rapido e nelle Opzioni. |
| `flight_hud_sound` | 1 bip di allarme (default), 0 silenzio. |
| `flight_hud_opacity` | 10–100, trasparenza dell'HUD. |
| `flight_gameplay` | 0 spento (default), 1 modalità arcade (vedi sopra). Serve `flight_hud` acceso. |
| `flight_hud_callsigns` | 0 nomi dei mostri (default), 1 BOGEY / BANDIT (BANDIT se ti sta puntando), 2 TGT-01, TGT-02… Nelle Opzioni solo su telefono ("Target_Labels"). |

---

## Stato

| # | Voce | Stato |
|---|------|-------|
| — | HUD base (Modern + Classic), lock, MISSILE ALERT, flare, radar su touch | [x] HUD base visto su Android; lock e flare da provare |
| 1 | DESTROYED + "+1000" | [~] |
| 2 | Messaggi radio | [~] |
| 2b | Ritratti presi dal gioco | [~] mai visti in gioco: priorità del prossimo test |
| 2c | Ritratto 3D (modello convertito e disegnato dall'HUD) | [~] acceso di default: da provare |
| 3 | Frecce sul bordo | [~] |
| 4 | MISSION UPDATE | [~] |
| 5 | Terzo allarme (EVADE) | [~] |
| 6 | MISSION COMPLETE | [~] |
| 7 | Barra salute del bersaglio | [~] |
| 8 | Callsign | [~] |
| 9 | Scala di beccheggio e nastro della bussola (prima persona) | [~] |
| 10 | Ritratto di Cybil centrato sul volto (era sul petto) | [~] |
| 11 | Toni d'allarme generati, stile avviso radar (al posto del bip del menu) | [~] |
| 12 | Harry in wireframe al posto della sagoma | [~] |
| 13 | Seeker con LOCK ON e toni del seeker | [~] |
| 14 | HIT / MISS | [~] |
| 15 | TGT REMAINING | [~] |
| 16 | HUD che si strappa quando Harry è colpito, DAMAGE | [~] |
| 17 | WARNING: LOW HEALTH con bip | [~] |
| 18 | MISSION FAILED alla morte, Cybil alla radio | [~] |
| 19 | Flare che fermano i colpi dei mostri (`flight_gameplay`) | [~] |
| 20 | Missili dei mostri, deviati dai flare | [~] |
| 21 | Missili di Harry con Cerchio a LOCK ON | [~] |
| 22 | Scie di fumo nella scena e bagliore dei missili | [~] |

### Da provare in gioco

- [ ] Bar iniziale: Cybil viene catturata? Il ritaglio è centrato sul volto?
- [ ] Ritratto 3D: al primo messaggio radio c'è Cybil in 3D (testa e spalle)?
      Texture giuste, niente pezzi mancanti o vertici "a spillo"?
- [ ] Il mostro che fa lock appare in diretta, nella sua posa vera?
- [ ] Primo mostro (Air Screamer nel bar): riquadro, nome, barra salute, frecce quando esce di scena.
- [ ] Lock: WARNING → MISSILE ALERT → tutto rosso, voce radio di Cybil con il suo ritratto.
- [ ] Mostro che fa lock da vicino: EVADE e bordi rossi; ritratto in diretta del mostro.
- [ ] Flare (L3+R3 o F): lock rotto, FLR che scende e si ricarica.
- [ ] Uccisione: DESTROYED, +1000, SCORE che sale.
- [ ] Uscita dal bar verso la città: MISSION UPDATE.
- [ ] Fine di un capitolo o un boss: MISSION COMPLETE con voto.
- [ ] Prima persona: la linea dell'orizzonte coincide con l'orizzonte vero
      della scena? Il nastro segna la stessa direzione del radar quando Harry
      cammina?
- [ ] Ritratto di Cybil (3D e ritaglio): ora si vede il volto?
- [ ] Toni: trillo per WARNING, bip veloci per MISSILE ALERT, sirena per
      EVADE. Si sentono con ogni renderer audio? La statica della radio
      continua a suonare insieme? Il tono si ferma in pausa, nell'inventario e
      nella mappa? Segue il volume degli effetti?
- [ ] Seeker: il rombo va sul mostro giusto (quello che colpiresti)? Ronzio,
      poi tono acuto al LOCK ON?
- [ ] HIT sui colpi a segno, MISS su quelli a vuoto (fucile a pompa compreso).
- [ ] Colpo subito: HUD che trema e DAMAGE, senza disturbare troppo.
- [ ] Morte: MISSION FAILED e Cybil prima del GAME OVER; dopo il continue
      l'HUD torna normale.
- [ ] Harry in wireframe: posa giusta (in piedi, braccia lungo i fianchi), non
      a testa in giù o di spalle? Leggibile anche piccolo su telefono?

- [ ] `flight_gameplay = 0`: tutto come prima (colpi dopo i flare, Cerchio
      sempre torcia, nessun missile, pannello in basso a destra invariato).
- [ ] Flare quando un mostro sta per colpire: 3 s senza colpi, poi tornano.
      Boss (Split Head) non toccati.
- [ ] Groaner che tiene il lock: dopo ~1,2 s parte un missile, EVADE e
      sirena; colpo = animazione al busto e danno. Correndo di lato si schiva.
- [ ] Missile in volo + flare: il missile va sui flare, niente danno.
- [ ] Missile in volo + cambio stanza: nella stanza nuova niente missile.
- [ ] Cerchio a LOCK ON: missile, la torcia non cambia; senza LOCK ON torcia.
      Terzo lancio di fila: suono d'errore e NO MISSILES; uno torna in 12 s.
- [ ] Cerchio proprio mentre il seeker si chiude: mai missile e torcia insieme.
- [ ] Scie: fumo che si allarga e sbiadisce, coperto da muri e nebbia; nessuno
      sfarfallio con 5 missili in aria.
- [ ] MSL in Modern, Classic e su telefono, senza sovrapposizioni.

Se un elemento è nel posto sbagliato o non si capisce, annotarlo qui con uno
screenshot.

---

## Idee future

- Riquadro radio con un'animazione d'apertura più lunga e audio di statico.
- Indicatore di direzione verso l'obiettivo della zona (porta/chiave), se si
  riesce a leggerlo dai flag evento.
- Testi dell'HUD tradotti (oggi solo inglese).
---

## Idee di gameplay

Oggi l'HUD è solo scena: il lock, l'allarme e i flare non cambiano nulla del
gioco. Queste idee lo cambierebbero davvero. Tutto va dietro un'opzione
(`flight_gameplay`, spenta di default), così il gioco base resta identico
all'originale PSX.

### Livelli di difficoltà del lavoro

| Livello | Cosa | Fattibilità |
|---------|------|-------------|
| 1 | I flare hanno un effetto vero: per qualche secondo i mostri perdono Harry (attacco annullato o rimandato) | Facile: tocca solo l'IA al momento dell'attacco |
| 2 | Missili "arcade" dei mostri: dopo MISSILE ALERT parte un proiettile che insegue Harry; i flare lo deviano, se colpisce fa il danno dell'attacco normale | Medio: un nuovo oggetto con la sua fisica, danno tramite le funzioni già esistenti |
| 3 | Missili di Harry: con un bersaglio agganciato, un tasto lancia un missile a ricerca (munizioni contate, si ricaricano come i flare) | Medio: stessa base del livello 2, colpisce con il danno delle armi da fuoco |
| 4 | Harry e i mostri che si muovono come aerei (volo, quota, virate) | Molto difficile: va riscritto il movimento di tutti i personaggi, le collisioni e le telecamere. Sconsigliato |

Consigliato: partire da 1, poi 2 e 3 insieme ("modalità arcade").

Livelli 1–3 e scie: [~] implementati (vedi "Modalità arcade"), da provare in
gioco. Prossimo: pulsante touch MSL su `android-port`, collisione dei missili
con i muri.

### Missili arcade (livelli 2–3)

- Guida semplice: il missile ruota verso il bersaglio con una velocità di
  virata limitata, così si può schivare correndo di lato.
- Durata massima di qualche secondo, poi esplode da solo.
- I flare attivi attirano i missili vicini invece del bersaglio.
- Il danno passa per le funzioni di danno del gioco, così animazioni di
  colpo e morte restano quelle originali.
- Suoni: riusare effetti già presenti (sparo, esplosione) invece di audio nuovo.
- HUD: il missile in arrivo ha il suo indicatore (rombo rosso che lampeggia
  più veloce man mano che si avvicina); il riquadro del bersaglio mostra
  i missili di Harry rimasti (MSL accanto a FLR).

### Scie dei missili

Rendering ibrido:

- **Fumo**: primitive PSX normali nella OT del mondo (OT0), quindi con la
  profondità giusta, nascosto dai muri e coperto dalla nebbia come il resto
  della scena. Una fila di quad semitrasparenti che sbiadiscono e si
  allargano col tempo.
- **Bagliore**: nello strato HUD, in additivo (come i flare), solo sulla testa
  del missile. È quello che lo rende leggibile da lontano.

### Livelli nuovi

- La pipeline TrenchBroom esistente permette di creare mappe, una alla volta.
- Idea: una piccola arena per provare la modalità arcade (spazio aperto,
  qualche ostacolo, ondate di mostri).
- Le stesse mappe nascoste possono servire da "scena" per altre cose
  (es. il ritratto radio, se un giorno servisse un fondale).

---

## Dove sta il codice

- `pc_port/src/pc_flight_hud.c` — tutto l'HUD.
  - Logica (thread di gioco, tempo di gioco): `Pc_FlightHud_Update` →
    `Ah_FlareSim`, `Ah_LockScan`, `Ah_Tones`, radio, uccisioni, banner,
    debriefing. Agganciato in `src/bodyprog/events/game_sys_states.c` accanto a
    `Pc_CrosshairDraw`.
  - Disegno: `Pc_FlightHud_Draw`, chiamato da `DbgOverlay_Render`
    (`pc_port/src/dbg_overlay.c`) dopo la cattura del frame. GL proprio, con
    salvataggio/ripristino dello stato.
  - Batch di geometria: `s_hud` (con passata ombra), `s_fill` (pannelli
    semitrasparenti, senza ombra), `s_glow` (additivo, per i flare).
  - Coordinate HUD: 480 unità di altezza, origine al centro, +y in basso,
    mezza larghezza `s_w2`.
  - Primitive: `Ah_Line`, `Ah_Box`, `Ah_Rect`, `Ah_Circle`, `Ah_Tri`,
    `Ah_Glow`, `Ah_Text` (font vettoriale `s_glyph`, solo maiuscole, cifre e
    pochi simboli: aggiungere glifi lì se servono).
  - Proiezione mondo → HUD: `Ah_Project` (metri, assi di gioco, Y verso il basso).
  - Stili: Modern = `Ah_BuildHud`, Classic = `Ah_BuildHudClassic`.
  - Toni: `Ah_Tones` sceglie il modo (WARNING, MISSILE ALERT, EVADE). Un'onda
    con armoniche dispari, generata e codificata in ADPCM all'avvio
    (`Ah_ToneEncode`, 112 campioni in loop), sta nei 64 byte di RAM SPU che la
    libsd lascia sotto l'area di riverbero (`SD_SPU_ALLOC_TOP` /
    `SD_PC_TONE_BYTES` in `include/bodyprog/libsd.h`); i registri di pitch e
    volume suonano i pattern sulla voce 21. Le voci 22 e 23 sono della radio.
    Mentre suona, la voce è fuori dal driver (`SdPcHoldVoice`, `SD_PC_HELD` nei
    cicli di ricerca voce di `smf_io.c` / `smf_snd.c`) e resta accesa: gli
    impulsi sono fatti col volume. Si ferma fuori dal gioco anche dal draw,
    perché inventario e mappa non chiamano l'update. Se la RAM SPU non c'è,
    torna il bip del menu.
  - Combattimento: `Ah_CombatTick` (prima di `Ah_LockScan`) gestisce seeker
    (`g_SysWork.targetNpcIdx`), finestra del MISS (`firedShotCount`), danno
    subito e morte; `Ah_LockScan` segna i colpi a segno dal calo di salute
    dei mostri (`Ah_OnHit`). Disegno: `Ah_SeekerMark`, `Ah_HitFx`,
    `Ah_Warnings` (strappi con `Ah_Static`, tremolio con `s_jx`/`s_jy` in
    `Ah_V`), `Ah_BuildDead`.
  - Harry in wireframe: `Ah_WireBuild` mette in posa `g_WorldGfxWork.harryModel`
    sul fotogramma 0 dell'animazione base (Harry fermo), ne estrae i lati
    senza doppioni e li normalizza; `Ah_WireHarry` li disegna al posto di
    `Ah_Silhouette`. Ricostruito se cambia modello, animazione o mappa.
  - Prima persona: `Ah_FirstPerson` (`g_PcFpsCam` e nessuna scena scriptata),
    `Ah_ViewAngles` legge direzione e beccheggio da `GsWSMATRIX`,
    `Ah_PitchLadder` proietta le direzioni dei gradini con `Ah_ProjectDir`
    (punti di fuga, quindi spaziatura e inclinazione sono quelle vere della
    vista), `Ah_CompassTape` disegna il nastro.
  - Layout touch: ramo `touch` (`Pc_Touch_IsDrivingInput()`) in entrambi i build.
  - Ritratti: `Ah_PortraitCapture` proietta la testa con `GsWSMATRIX` (la
    matrice con cui è stato disegnato il frame, quindi vale anche nelle scene
    d'intermezzo), ritaglia il frame con `glBlitFramebuffer` da
    `GR_ScreenReadFBO()` in una texture 128×128 per `charaId`; salvataggio in
    `gamedata/hud_portraits/<charaId>.rgba`. Disegno con `Ah_PortraitDraw`;
    `Ah_Portrait` è il ripiego a fil di ferro.
  - Ritratto 3D: `Ah_ModelPortraitRender`, dal post-capture draw. Prende il
    modello dal pool (`Pc_CharaPool_ModelOf`), la posa dalle ossa dell'NPC in
    scena o da `Anim_BoneUpdate` sulla prima chiave dell'animazione del pool,
    e lo converte in triangoli (`Ah_ModelBuild`): pezzi nell'ordine dello
    scheletro, vertici portati dal frame dell'osso al mondo e scritti nel
    "pool" di vertici a `ModelHeader.vertexOffset` come fa il gioco (i vertici
    di giunzione vengono da lì), texture per prim con
    `HiresOverride_LookupByTpageClut` (le slot GL del pool). Disegno in un FBO
    128×128 con profondità, prospettiva, luce e rim propri, direttamente in
    `s_portTex[charaId]`.
- Config: `flight_hud`, `flight_hud_sound`, `flight_hud_opacity`,
  `flight_hud_callsigns`, `flight_hud_portrait_3d` — `pc_port/src/pc_config.c`, `pc_port/include/pc_config.h`,
  documentati in `pc_port/config.cfg`. Riga "Flight HUD" in
  `src/screens/options/options.c` e nel menu rapido (`pc_port/src/pc_quick_options.c`).
  Le pagine Opzioni hanno un tetto di righe: la pagina HUD del telefono è piena.
- Modalità arcade:
  - `pc_port/src/pc_flight_missile.c` — volo puro (guida a virata limitata,
    colpo sul segmento percorso, esche, fumo, regola di lancio), senza header
    del gioco; test `pc_port/tests/pc_flight_missile_test.c`
    (`-DBUILD_TESTING=ON`, `ctest -R pc_flight_missile`).
  - `pc_port/src/pc_flight_arcade.c` — lanci, volo, danni (scritti in
    `damage.amount` / `damage.position` / `attackReceived`, come fa il gioco),
    tasto light, fumo nella OT del mondo (`Ar_SmokeDraw`, POLY_F4 additivi
    con `Vw_WorldScreenMatrixAtPositionGet` + `RotTransPers`). Chiamato da
    `Pc_FlightHud_Update` dopo `Ah_LockScan`; reset al cambio mappa.
  - Scudo dei flare: `Pc_FlightArcade_ShieldsHarryFrom` in `func_8008A0E4` e
    `func_8008B714` (`src/bodyprog/bodyprog_combat_8008A058.c`).
  - Torcia: `SysState_Gameplay_Update` non la cambia se
    `Pc_FlightArcade_ClaimsLightButton()`; lo stato è preso dal frame prima,
    lo stesso con cui l'update decide il lancio.
  - HUD: `Ah_BuildMissiles`, `Ah_MissileMarks`, `Ah_MslLine`.
- Touch: pulsante flare `TB_FLARE` / `TG_C_FLARE` in `pc_port/src/pc_touch.c`.

## Anteprima senza il gioco

`pc_port/tools/flight_hud_preview/` contiene un harness che include
`pc_flight_hud.c`, finge una scena (Harry, alcuni nemici, flare) e scrive i
triangoli dell'HUD; `render.py` li rasterizza in PNG. I ritratti presi dal
gioco non si vedono qui (servono GL e il frame vero): compare il fil di ferro.

```
# dalla cartella pc_port/tools/flight_hud_preview, con una build CMake in <build>
CMD=$(cd <build> && ninja -t commands CMakeFiles/SilentHillPC.dir/src/pc_flight_hud.c.o \
      | tail -1 | sed 's/ -MD .*//;s/-fPIE//')
$CMD -Wno-unused-label -o harness harness.c -lSDL2 -lm -Wl,--unresolved-symbols=ignore-all
./harness && python3 render.py   # richiede Pillow e numpy
```

Produce `normal.png`, `alert.png`, `flare.png`, `aim.png`, `classic.png`,
`touch.png`, `events.png` / `events_classic.png`, `danger.png`, `banner.png`,
`debrief.png`, `comm_*.png`, `fps.png` / `fps_classic.png` / `fps_touch.png`
(prima persona), `seek.png` / `seeklock.png` / `seeklock_classic.png`,
`hurt.png`, `dead.png`, `recharge.png`, `touch_classic.png`. Lo sfondo è finto: serve solo a controllare
layout e colori.
