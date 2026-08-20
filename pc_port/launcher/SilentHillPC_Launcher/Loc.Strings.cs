using System;
using System.Collections.Generic;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// The launcher's UI strings, keyed by their English text.
    /// Columns, in order: ES, PT, FR, DE, IT, JA, ZH, RU, PL.
    ///
    /// Only strings listed here are ever replaced (Loc.T passes anything else
    /// through), so this file is also the definition of what is localized.
    /// Proper nouns and acronyms are deliberately absent: PGXP, RA, VSync, FPS,
    /// BC7, DDS, PNG, TIM, ILM, PLM, TMD, ANM, VAB, OBJ.
    /// </summary>
    public static partial class Loc
    {
        static readonly Dictionary<string, string[]> Table =
            new Dictionary<string, string[]>(StringComparer.Ordinal)
        {
            // ---- main window: actions ----
            // These land on fixed-width buttons (104px for the update pair, 98
            // for Build Settings, 70 for Report Bug, 53 for Reset, 39 for Help),
            // and WinForms clips button text mid-word rather than wrapping it.
            // Every column below was measured against its button — keep new ones
            // at or under the English string's width.
            { "Play",              new[]{ "Jugar", "Jogar", "Jouer", "Spielen", "Gioca", "プレイ", "开始游戏", "Играть", "Graj" } },
            { "Check for Updates", new[]{ "Actualizaciones", "Atualizações", "Mises à jour", "Updates suchen", "Aggiornamenti", "更新を確認", "检查更新", "Обновления", "Aktualizacje" } },
            { "Changelog",         new[]{ "Novedades", "Novidades", "Changements", "Änderungen", "Novità", "変更履歴", "更新日志", "Изменения", "Lista zmian" } },
            { "View Changelog",    new[]{ "Ver novedades", "Ver novidades", "Voir les modifications", "Änderungen ansehen", "Vedi novità", "変更履歴を見る", "查看更新日志", "Показать изменения", "Zobacz zmiany" } },
            { "Controls",          new[]{ "Controles", "Controles", "Commandes", "Steuerung", "Comandi", "操作設定", "控制设置", "Управление", "Sterowanie" } },
            { "Build Settings",    new[]{ "Compilación", "Config. build", "Réglages build", "Build-Optionen", "Impostazioni", "ビルド設定", "版本设置", "Опции сборки", "Ustawienia" } },
            { "Download Build",    new[]{ "Descargar build", "Baixar build", "Télécharger build", "Build laden", "Scarica build", "ビルド取得", "下载版本", "Скачать сборку", "Pobierz build" } },
            { "Redownload Build",  new[]{ "Redescargar", "Baixar de novo", "Retélécharger", "Erneut laden", "Riscarica", "再ダウンロード", "重新下载", "Скачать заново", "Pobierz ponownie" } },
            // 39px button, so the usable text width is ~33px: any language whose
            // word for Help is wider gets "?" instead, because a clipped "Pom"
            // is worse than the universal glyph.
            { "Help",              new[]{ "?", "?", "Aide", "Hilfe", "Aiuto", "?", "帮助", "?", "?" } },
            { "Report Bug",        new[]{ "Reportar", "Reportar", "Signaler", "Fehler", "Segnala", "バグ報告", "报告问题", "Ошибка", "Zgłoś błąd" } },
            { "Reset",             new[]{ "Reiniciar", "Repor", "Réinit.", "Zurück", "Reset", "リセット", "重置", "Сброс", "Resetuj" } },

            // ---- main window: setting labels ----
            { "Skip Intros:",      new[]{ "Saltar intros:", "Pular intros:", "Intros :", "Intros:", "Salta intro:", "イントロ:", "跳过开场:", "Заставки:", "Pomiń intra:" } },
            { "Display:",          new[]{ "Pantalla:", "Tela:", "Écran :", "Anzeige:", "Schermo:", "ディスプレイ:", "显示器:", "Дисплей:", "Ekran:" } },
            { "VSync:",            new[]{ "VSync:", "VSync:", "VSync :", "VSync:", "VSync:", "垂直同期:", "垂直同步:", "Вертик. синхр.:", "VSync:" } },
            { "Resolution:",       new[]{ "Resolución:", "Resolução:", "Résolution :", "Auflösung:", "Risoluzione:", "解像度:", "分辨率:", "Разрешение:", "Rozdzielczość:" } },
            { "Pillarboxing:",     new[]{ "Bandas lat.:", "Barras laterais:", "Bandes lat. :", "Seitenbalken:", "Bande laterali:", "ピラーボックス:", "黑边显示:", "Боковые поля:", "Pasy boczne:" } },
            { "FPS Limit:",        new[]{ "Límite FPS:", "Limite FPS:", "Limite FPS :", "FPS-Limit:", "Limite FPS:", "FPS制限:", "帧率限制:", "Лимит FPS:", "Limit FPS:" } },
            { "Preload Chunks:",   new[]{ "Precargar:", "Pré-carregar:", "Préchargement :", "Vorladen:", "Precarica:", "事前読み込み:", "预加载区块:", "Предзагрузка:", "Wstępne ład.:" } },
            { "Filtering:",        new[]{ "Filtrado:", "Filtragem:", "Filtrage :", "Filterung:", "Filtro:", "フィルタ:", "纹理过滤:", "Фильтрация:", "Filtrowanie:" } },
            { "Use PGXP:",         new[]{ "Usar PGXP:", "Usar PGXP:", "Utiliser PGXP :", "PGXP nutzen:", "Usa PGXP:", "PGXPを使う:", "启用 PGXP:", "Исп. PGXP:", "Użyj PGXP:" } },
            { "Enable Logging:",   new[]{ "Registro:", "Registro:", "Journalisation :", "Protokollierung:", "Log:", "ログ出力:", "启用日志:", "Вести журнал:", "Logowanie:" } },
            { "External Console:", new[]{ "Consola externa:", "Console externo:", "Console ext. :", "Ext. Konsole:", "Console esterna:", "外部コンソール:", "外部控制台:", "Внеш. консоль:", "Konsola zewn.:" } },
            { "Antialiasing:",     new[]{ "Suavizado:", "Suavização:", "Anticrénelage :", "Kantenglättung:", "Antialiasing:", "アンチエイリアス:", "抗锯齿:", "Сглаживание:", "Antialiasing:" } },
            { "Post Effect:",      new[]{ "Postproceso:", "Pós-efeito:", "Post-traitem. :", "Post-Effekt:", "Post-effetto:", "ポストエフェクト:", "后期效果:", "Постобраб.:", "Efekt końcowy:" } },
            { "Tone Map:",         new[]{ "Mapeo tonal:", "Mapa de tons:", "Mappage tonal:", "Tone Mapping:", "Mappatura toni:", "トーンマップ:", "色调映射:", "Тонмаппинг:", "Mapow. tonów:" } },
            { "Flashlight:",       new[]{ "Linterna:", "Lanterna:", "Lampe torche :", "Taschenlampe:", "Torcia:", "懐中電灯:", "手电筒:", "Фонарик:", "Latarka:" } },
            { "Disk Image:",       new[]{ "Imagen disco:", "Imagem disco:", "Image disque :", "Disk-Image:", "Immagine disco:", "ディスクイメージ:", "光盘镜像:", "Образ диска:", "Obraz płyty:" } },
            { "Audio Out:",        new[]{ "Salida audio:", "Saída áudio:", "Sortie audio :", "Audio:", "Uscita audio:", "音声出力:", "音频输出:", "Вывод звука:", "Wyjście audio:" } },
            { "Level:",            new[]{ "Nivel:", "Nível:", "Niveau :", "Level:", "Livello:", "レベル:", "关卡:", "Уровень:", "Poziom:" } },
            { "Menus:",            new[]{ "Menús:", "Menus:", "Menus:", "Menüs:", "Menu:", "メニュー:", "菜单:", "Меню:", "Menu:" } },

            // ---- common values / buttons ----
            { "Yes",    new[]{ "Sí", "Sim", "Oui", "Ja", "Sì", "はい", "是", "Да", "Tak" } },
            { "No",     new[]{ "No", "Não", "Non", "Nein", "No", "いいえ", "否", "Нет", "Nie" } },
            { "OK",     new[]{ "Aceptar", "OK", "OK", "OK", "OK", "OK", "确定", "OK", "OK" } },
            { "Cancel", new[]{ "Cancelar", "Cancelar", "Annuler", "Abbrechen", "Annulla", "キャンセル", "取消", "Отмена", "Anuluj" } },
            { "Apply",  new[]{ "Aplicar", "Aplicar", "Appliquer", "Übernehmen", "Applica", "適用", "应用", "Применить", "Zastosuj" } },
            { "Close",  new[]{ "Cerrar", "Fechar", "Fermer", "Schließen", "Chiudi", "閉じる", "关闭", "Закрыть", "Zamknij" } },
            { "Update", new[]{ "Actualizar", "Atualizar", "Mettre à jour", "Aktualisieren", "Aggiorna", "更新", "更新", "Обновить", "Aktualizuj" } },
            { "Not now",new[]{ "Ahora no", "Agora não", "Pas maintenant", "Später", "Non ora", "後で", "以后再说", "Не сейчас", "Nie teraz" } },
            { "Refresh",new[]{ "Actualizar", "Atualizar", "Actualiser", "Aktualisieren", "Aggiorna", "更新", "刷新", "Обновить", "Odśwież" } },

            // ---- main window: status ----
            { "Checking for updates...",       new[]{ "Buscando actualizaciones...", "Procurando atualizações...", "Recherche de mises à jour...", "Suche nach Updates...", "Ricerca aggiornamenti...", "更新を確認中...", "正在检查更新...", "Проверка обновлений...", "Sprawdzanie aktualizacji..." } },
            // Goes on the 104px update button, not the status line — hence the
            // short forms.
            { "Update available!",             new[]{ "¡Disponible!", "Disponível!", "Disponible !", "Verfügbar!", "Disponibile!", "更新あり!", "有更新!", "Обновление!", "Dostępna!" } },
            { "Update failed (see message).",  new[]{ "Fallo al actualizar (ver mensaje).", "Falha ao atualizar (ver mensagem).", "Échec de la mise à jour (voir message).", "Update fehlgeschlagen (siehe Meldung).", "Aggiornamento fallito (vedi messaggio).", "更新に失敗しました(メッセージ参照)", "更新失败(见提示)。", "Не удалось обновить (см. сообщение).", "Aktualizacja nieudana (zobacz komunikat)." } },
            { "Checking selected build...",    new[]{ "Comprobando la compilación...", "Verificando a build...", "Vérification de la build...", "Build wird geprüft...", "Controllo della build...", "ビルドを確認中...", "正在检查版本...", "Проверка сборки...", "Sprawdzanie kompilacji..." } },
            { "Download cancelled.",           new[]{ "Descarga cancelada.", "Download cancelado.", "Téléchargement annulé.", "Download abgebrochen.", "Download annullato.", "ダウンロードを中止しました。", "下载已取消。", "Загрузка отменена.", "Pobieranie anulowane." } },
            { "Download failed (see message).",new[]{ "Fallo al descargar (ver mensaje).", "Falha no download (ver mensagem).", "Échec du téléchargement (voir message).", "Download fehlgeschlagen (siehe Meldung).", "Download fallito (vedi messaggio).", "ダウンロードに失敗しました(メッセージ参照)", "下载失败(见提示)。", "Ошибка загрузки (см. сообщение).", "Pobieranie nieudane (zobacz komunikat)." } },
            { "Disc: Auto",                    new[]{ "Disco: automático", "Disco: automático", "Disque : auto", "Disc: Automatisch", "Disco: automatico", "ディスク: 自動", "光盘: 自动", "Диск: авто", "Płyta: auto" } },
            { "Change background",             new[]{ "Cambiar fondo", "Alterar fundo", "Changer de fond", "Hintergrund ändern", "Cambia sfondo", "背景を変更", "更换背景", "Сменить фон", "Zmień tło" } },
            { "Dark mode",                     new[]{ "Modo oscuro", "Modo escuro", "Mode sombre", "Dunkler Modus", "Modalità scura", "ダークモード", "深色模式", "Тёмная тема", "Tryb ciemny" } },
            { "Language",                      new[]{ "Idioma", "Idioma", "Langue", "Sprache", "Lingua", "言語", "语言", "Язык", "Język" } },
            { "Launcher language",             new[]{ "Idioma del lanzador", "Idioma do inicializador", "Langue du lanceur", "Launcher-Sprache", "Lingua del launcher", "ランチャーの言語", "启动器语言", "Язык лаунчера", "Język launchera" } },


            // {0} is a build/version number, so it stays put in every language.
            { "Update available: {0}",   new[]{ "Actualización disponible: {0}", "Atualização disponível: {0}", "Mise à jour disponible : {0}", "Update verfügbar: {0}", "Aggiornamento disponibile: {0}", "更新があります: {0}", "有可用更新: {0}", "Доступно обновление: {0}", "Dostępna aktualizacja: {0}" } },
            { "Up to date ({0}).",       new[]{ "Actualizado ({0}).", "Atualizado ({0}).", "À jour ({0}).", "Aktuell ({0}).", "Aggiornato ({0}).", "最新です ({0})。", "已是最新 ({0})。", "Обновлений нет ({0}).", "Aktualne ({0})." } },
            { "Update {0} skipped.",     new[]{ "Actualización {0} omitida.", "Atualização {0} ignorada.", "Mise à jour {0} ignorée.", "Update {0} übersprungen.", "Aggiornamento {0} saltato.", "更新 {0} をスキップしました。", "已跳过更新 {0}。", "Обновление {0} пропущено.", "Pominięto aktualizację {0}." } },
            { "Build {0} is installed.", new[]{ "La compilación {0} ya está instalada.", "A build {0} já está instalada.", "La build {0} est déjà installée.", "Build {0} ist bereits installiert.", "La build {0} è già installata.", "ビルド {0} はインストール済みです。", "版本 {0} 已安装。", "Сборка {0} уже установлена.", "Kompilacja {0} jest zainstalowana." } },
            { "Build {0} installed.",    new[]{ "Compilación {0} instalada.", "Build {0} instalada.", "Build {0} installée.", "Build {0} installiert.", "Build {0} installata.", "ビルド {0} をインストールしました。", "已安装版本 {0}。", "Сборка {0} установлена.", "Zainstalowano kompilację {0}." } },
            { "Installing files...",     new[]{ "Instalando archivos...", "Instalando arquivos...", "Installation des fichiers...", "Dateien werden installiert...", "Installazione file...", "ファイルをインストール中...", "正在安装文件...", "Установка файлов...", "Instalowanie plików..." } },
            { "Downloading update...",   new[]{ "Descargando actualización...", "Baixando atualização...", "Téléchargement de la mise à jour...", "Update wird heruntergeladen...", "Download aggiornamento...", "更新をダウンロード中...", "正在下载更新...", "Загрузка обновления...", "Pobieranie aktualizacji..." } },
            { "Extracting + verifying...", new[]{ "Extrayendo y verificando...", "Extraindo e verificando...", "Extraction et vérification...", "Entpacken + Prüfen...", "Estrazione e verifica...", "展開・検証中...", "正在解压和校验...", "Распаковка и проверка...", "Rozpakowywanie i weryfikacja..." } },
            // Verbatim strings here: the trailing path separator would otherwise
            // have to be double-escaped in ten places.
            { @"No disc image found in gamedata\", new[]{ @"No hay imagen de disco en gamedata\", @"Nenhuma imagem de disco em gamedata\", @"Aucune image disque dans gamedata\", @"Kein Datenträgerabbild in gamedata\", @"Nessuna immagine disco in gamedata\", @"gamedata\ にディスクイメージがありません", @"gamedata\ 中未找到光盘镜像", @"В gamedata\ нет образа диска", @"Brak obrazu płyty w gamedata\" } },

            // ---- dropdown values (the items stay English; only the painted
            // text changes -- see Loc.LocalizeItems). Acronyms and proper nouns
            // are absent on purpose: CRT, PSX Retro, Reinhard, ACES, 2x/4x/8x.
            { "Fullscreen",        new[]{ "Pantalla completa", "Tela cheia", "Plein écran", "Vollbild", "Schermo intero", "フルスクリーン", "全屏", "Полный экран", "Pełny ekran" } },
            { "Windowed",          new[]{ "En ventana", "Em janela", "Fenêtré", "Fenster", "Finestra", "ウィンドウ", "窗口", "В окне", "W oknie" } },
            { "Borderless",        new[]{ "Sin bordes", "Sem bordas", "Sans bordure", "Randlos", "Senza bordi", "ボーダーレス", "无边框", "Без рамки", "Bez ramki" } },
            { "Don't Skip",        new[]{ "No saltar", "Não pular", "Ne pas passer", "Nicht überspringen", "Non saltare", "スキップしない", "不跳过", "Не пропускать", "Nie pomijaj" } },
            { "Skip to Menu",      new[]{ "Saltar al menú", "Pular para o menu", "Aller au menu", "Zum Menü", "Vai al menu", "メニューへ", "跳到菜单", "К меню", "Do menu" } },
            { "Skip to Game",      new[]{ "Saltar al juego", "Pular para o jogo", "Aller au jeu", "Zum Spiel", "Vai al gioco", "ゲームへ", "跳到游戏", "К игре", "Do gry" } },
            { "Menus Only",        new[]{ "Solo menús", "Somente menus", "Menus seulement", "Nur Menüs", "Solo menu", "メニューのみ", "仅菜单", "Только меню", "Tylko menu" } },
            { "Off",               new[]{ "Desactivado", "Desligado", "Désactivé", "Aus", "Disattivato", "オフ", "关闭", "Выкл.", "Wył." } },
            { "On",                new[]{ "Activado", "Ligado", "Activé", "Ein", "Attivato", "オン", "开启", "Вкл.", "Wł." } },
            { "Auto",              new[]{ "Automático", "Automático", "Auto", "Automatisch", "Automatico", "自動", "自动", "Авто", "Auto" } },
            { "Dithering",         new[]{ "Tramado", "Pontilhado", "Tramage", "Dithering", "Dithering", "ディザリング", "抖动", "Дизеринг", "Dithering" } },
            { "Bilinear",          new[]{ "Bilineal", "Bilinear", "Bilinéaire", "Bilinear", "Bilineare", "バイリニア", "双线性", "Билинейная", "Dwuliniowe" } },
            { "Scanlines",         new[]{ "Líneas de barrido", "Linhas de varredura", "Lignes de balayage", "Scanlines", "Scanline", "走査線", "扫描线", "Строки развёртки", "Linie skan." } },
            { "Vignette",          new[]{ "Viñeta", "Vinheta", "Vignettage", "Vignette", "Vignettatura", "ビネット", "暗角", "Виньетка", "Winieta" } },
            { "Color Grade",       new[]{ "Corrección de color", "Correção de cor", "Étalonnage", "Farbkorrektur", "Correzione colore", "カラーグレーディング", "调色", "Цветокоррекция", "Korekcja koloru" } },
            { "Film Grain",        new[]{ "Grano de película", "Granulação", "Grain argentique", "Filmkorn", "Grana pellicola", "フィルムグレイン", "胶片颗粒", "Зернистость", "Ziarno filmowe" } },
            { "Sharpen",           new[]{ "Nitidez", "Nitidez", "Netteté", "Schärfen", "Nitidezza", "シャープ", "锐化", "Резкость", "Wyostrzanie" } },
            { "Cinematic",         new[]{ "Cinematográfico", "Cinematográfico", "Cinématique", "Filmisch", "Cinematografico", "シネマティック", "电影感", "Кинематографический", "Filmowy" } },
            { "Filmic",            new[]{ "Fílmico", "Fílmico", "Filmique", "Filmisch", "Filmico", "フィルム調", "胶片", "Плёночный", "Filmowy" } },
            { "Classic",           new[]{ "Clásico", "Clássico", "Classique", "Klassisch", "Classico", "クラシック", "经典", "Классический", "Klasyczny" } },
            { "Classic + Shadows", new[]{ "Clásico + sombras", "Clássico + sombras", "Classique + ombres", "Klassisch + Schatten", "Classico + ombre", "クラシック+影", "经典+阴影", "Классический + тени", "Klasyczny + cienie" } },
            { "Modern",            new[]{ "Moderno", "Moderno", "Moderne", "Modern", "Moderno", "モダン", "现代", "Современный", "Nowoczesny" } },
            { "Modern + Shadows",  new[]{ "Moderno + sombras", "Moderno + sombras", "Moderne + ombres", "Modern + Schatten", "Moderno + ombre", "モダン+影", "现代+阴影", "Современный + тени", "Nowoczesny + cienie" } },
            { "Stereo",            new[]{ "Estéreo", "Estéreo", "Stéréo", "Stereo", "Stereo", "ステレオ", "立体声", "Стерео", "Stereo" } },
            { "Quad",              new[]{ "Cuadrafónico", "Quadrifônico", "Quadriphonie", "Quadro", "Quadrifonico", "4チャンネル", "四声道", "Квадро", "Kwadrofonia" } },
            { "5.1 Surround",      new[]{ "Envolvente 5.1", "Surround 5.1", "Surround 5.1", "5.1 Surround", "Surround 5.1", "5.1サラウンド", "5.1 环绕", "5.1 объёмный", "Dźwięk 5.1" } },
            { "7.1 Surround",      new[]{ "Envolvente 7.1", "Surround 7.1", "Surround 7.1", "7.1 Surround", "Surround 7.1", "7.1サラウンド", "7.1 环绕", "7.1 объёмный", "Dźwięk 7.1" } },
            { "HRTF (Headphones)", new[]{ "HRTF (auriculares)", "HRTF (fones)", "HRTF (casque)", "HRTF (Kopfhörer)", "HRTF (cuffie)", "HRTF (ヘッドホン)", "HRTF (耳机)", "HRTF (наушники)", "HRTF (słuchawki)" } },

            // ---- mod manager: converter menus (file-format names stay put) ----
            { "Character (.ILM / .PLM)…",     new[]{ "Personaje (.ILM / .PLM)…", "Personagem (.ILM / .PLM)…", "Personnage (.ILM / .PLM)…", "Charakter (.ILM / .PLM)…", "Personaggio (.ILM / .PLM)…", "キャラクター (.ILM / .PLM)…", "角色 (.ILM / .PLM)…", "Персонаж (.ILM / .PLM)…", "Postać (.ILM / .PLM)…" } },
            { "Item model (.TMD)…",           new[]{ "Modelo de objeto (.TMD)…", "Modelo de item (.TMD)…", "Modèle d'objet (.TMD)…", "Gegenstandsmodell (.TMD)…", "Modello oggetto (.TMD)…", "アイテムモデル (.TMD)…", "物品模型 (.TMD)…", "Модель предмета (.TMD)…", "Model przedmiotu (.TMD)…" } },
            { "Character — high-poly…",       new[]{ "Personaje — alta densidad…", "Personagem — alta densidade…", "Personnage — haute densité…", "Charakter — High-Poly…", "Personaggio — high-poly…", "キャラクター — ハイポリ…", "角色 — 高模…", "Персонаж — высокополигональный…", "Postać — high-poly…" } },
            { "Character — simple…",          new[]{ "Personaje — simple…", "Personagem — simples…", "Personnage — simple…", "Charakter — einfach…", "Personaggio — semplice…", "キャラクター — シンプル…", "角色 — 简易…", "Персонаж — простой…", "Postać — prosta…" } },
            { "Item model (.TMD) — reshape…", new[]{ "Modelo de objeto (.TMD) — remodelar…", "Modelo de item (.TMD) — remodelar…", "Modèle d'objet (.TMD) — remodeler…", "Gegenstandsmodell (.TMD) — umformen…", "Modello oggetto (.TMD) — rimodella…", "アイテムモデル (.TMD) — 形状変更…", "物品模型 (.TMD) — 改形…", "Модель предмета (.TMD) — изменить форму…", "Model przedmiotu (.TMD) — przekształć…" } },
            { "Item model (.TMD) — replace…", new[]{ "Modelo de objeto (.TMD) — reemplazar…", "Modelo de item (.TMD) — substituir…", "Modèle d'objet (.TMD) — remplacer…", "Gegenstandsmodell (.TMD) — ersetzen…", "Modello oggetto (.TMD) — sostituisci…", "アイテムモデル (.TMD) — 置き換え…", "物品模型 (.TMD) — 替换…", "Модель предмета (.TMD) — заменить…", "Model przedmiotu (.TMD) — zamień…" } },
            { "Convert folder → BC7…",        new[]{ "Convertir carpeta → BC7…", "Converter pasta → BC7…", "Convertir le dossier → BC7…", "Ordner konvertieren → BC7…", "Converti cartella → BC7…", "フォルダを変換 → BC7…", "转换文件夹 → BC7…", "Конвертировать папку → BC7…", "Konwertuj folder → BC7…" } },
            // ---- mod manager: buttons ----
            { "Edit Mod",           new[]{ "Editar mod", "Editar mod", "Modifier le mod", "Mod bearbeiten", "Modifica mod", "Modを編集", "编辑模组", "Изменить мод", "Edytuj mod" } },
            { "Extract Disc Image", new[]{ "Extraer imagen de disco", "Extrair imagem de disco", "Extraire l'image disque", "Datenträgerabbild entpacken", "Estrai immagine disco", "ディスクイメージを展開", "解包光盘镜像", "Извлечь образ диска", "Wypakuj obraz płyty" } },
            { "Move Up",            new[]{ "Subir", "Mover para cima", "Monter", "Nach oben", "Sposta su", "上へ", "上移", "Вверх", "W górę" } },
            { "Move Down",          new[]{ "Bajar", "Mover para baixo", "Descendre", "Nach unten", "Sposta giù", "下へ", "下移", "Вниз", "W dół" } },
            { "Open Folder",        new[]{ "Abrir carpeta", "Abrir pasta", "Ouvrir le dossier", "Ordner öffnen", "Apri cartella", "フォルダを開く", "打开文件夹", "Открыть папку", "Otwórz folder" } },
            { "Open containing folder", new[]{ "Abrir carpeta contenedora", "Abrir pasta do item", "Ouvrir le dossier parent", "Übergeordneten Ordner öffnen", "Apri cartella superiore", "格納フォルダを開く", "打开所在文件夹", "Открыть папку с файлом", "Otwórz folder nadrzędny" } },
            { "Model Viewer",       new[]{ "Visor de modelos", "Visualizador de modelos", "Visionneuse de modèles", "Modellbetrachter", "Visualizzatore modelli", "モデルビューア", "模型查看器", "Просмотр моделей", "Podgląd modeli" } },
            { "Audio",              new[]{ "Audio", "Áudio", "Audio", "Audio", "Audio", "オーディオ", "音频", "Звук", "Dźwięk" } },
            { "Delete mod…",        new[]{ "Eliminar mod…", "Excluir mod…", "Supprimer le mod…", "Mod löschen…", "Elimina mod…", "Modを削除…", "删除模组…", "Удалить мод…", "Usuń mod…" } },
            { "Edit name && notes…",new[]{ "Editar nombre y notas…", "Editar nome e notas…", "Modifier le nom et les notes…", "Name && Notizen bearbeiten…", "Modifica nome e note…", "名前とメモを編集…", "编辑名称和备注…", "Изменить имя и заметки…", "Edytuj nazwę i notatki…" } },
            { "One texture…",       new[]{ "Una textura…", "Uma textura…", "Une texture…", "Eine Textur…", "Una texture…", "テクスチャ1枚…", "单个纹理…", "Одна текстура…", "Jedna tekstura…" } },
            { "Every texture…",     new[]{ "Todas las texturas…", "Todas as texturas…", "Toutes les textures…", "Alle Texturen…", "Tutte le texture…", "すべてのテクスチャ…", "所有纹理…", "Все текстуры…", "Wszystkie tekstury…" } },
            { "Enable loose file support (required for load-folder mods)",
                                    new[]{ "Activar archivos sueltos (necesario para mods de carpeta)", "Ativar arquivos avulsos (necessário para mods de pasta)", "Activer les fichiers libres (requis pour les mods de dossier)", "Lose Dateien aktivieren (für Ordner-Mods nötig)", "Abilita file sciolti (necessario per i mod da cartella)", "ルーズファイルを有効化(フォルダMODに必要)", "启用松散文件(文件夹模组必需)", "Включить свободные файлы (нужно для модов из папки)", "Włącz luźne pliki (wymagane dla modów z folderu)" } },
            { "Renderer:",      new[]{ "Renderizador:", "Renderizador:", "Rendu :", "Renderer:", "Renderer:", "レンダラー:", "渲染器:", "Рендерер:", "Renderer:" } },
            // comboShadow items are pure numbers ("1024 x 1024") so they are not
            // listed; comboMinimap pairs its label to the config value by INDEX
            // (ShadowResValues / the minimap mapping in SaveConfig), so translating
            // these cannot change what is written to config.cfg.
            { "Shadow Res:",                       new[]{ "Sombras:", "Sombras:", "Ombres :", "Schatten:", "Ombre:", "影の解像度:", "阴影分辨率:", "Тени:", "Cienie:" } },
            { "Minimap:",                          new[]{ "Minimapa:", "Minimapa:", "Minicarte :", "Minikarte:", "Minimappa:", "ミニマップ:", "小地图:", "Миникарта:", "Minimapa:" } },
            { "Circle + Top Left",                 new[]{ "Círculo + sup. izq.", "Círculo + sup. esq.", "Cercle + h. gauche", "Kreis + oben links", "Cerchio + alto sx", "円 + 左上", "圆形 + 左上", "Круг + сверху-слева", "Koło + lewy górny" } },
            { "Circle + Top Right",                new[]{ "Círculo + sup. der.", "Círculo + sup. dir.", "Cercle + h. droite", "Kreis + oben rechts", "Cerchio + alto dx", "円 + 右上", "圆形 + 右上", "Круг + сверху-справа", "Koło + prawy górny" } },
            { "Circle + Bottom Left",              new[]{ "Círculo + inf. izq.", "Círculo + inf. esq.", "Cercle + b. gauche", "Kreis + unten links", "Cerchio + basso sx", "円 + 左下", "圆形 + 左下", "Круг + снизу-слева", "Koło + lewy dolny" } },
            { "Circle + Bottom Right",             new[]{ "Círculo + inf. der.", "Círculo + inf. dir.", "Cercle + b. droite", "Kreis + unten rechts", "Cerchio + basso dx", "円 + 右下", "圆形 + 右下", "Круг + снизу-справа", "Koło + prawy dolny" } },
            { "Square + Top Left",                 new[]{ "Cuadrado + sup. izq.", "Quadrado + sup. esq.", "Carré + h. gauche", "Quadrat + oben links", "Quadrato + alto sx", "四角 + 左上", "方形 + 左上", "Квадрат + сверху-слева", "Kwadrat + lewy górny" } },
            { "Square + Top Right",                new[]{ "Cuadrado + sup. der.", "Quadrado + sup. dir.", "Carré + h. droite", "Quadrat + oben rechts", "Quadrato + alto dx", "四角 + 右上", "方形 + 右上", "Квадрат + сверху-справа", "Kwadrat + prawy górny" } },
            { "Square + Bottom Left",              new[]{ "Cuadrado + inf. izq.", "Quadrado + inf. esq.", "Carré + b. gauche", "Quadrat + unten links", "Quadrato + basso sx", "四角 + 左下", "方形 + 左下", "Квадрат + снизу-слева", "Kwadrat + lewy dolny" } },
            { "Square + Bottom Right",             new[]{ "Cuadrado + inf. der.", "Quadrado + inf. dir.", "Carré + b. droite", "Quadrat + unten rechts", "Quadrato + basso dx", "四角 + 右下", "方形 + 右下", "Квадрат + снизу-справа", "Kwadrat + prawy dolny" } },
            { "Bullet Decals:", new[]{ "Marcas de bala:", "Marcas de bala:", "Impacts :", "Kugellöcher:", "Fori proiettili:", "弾痕:", "弹孔:", "Следы от пуль:", "Ślady po kulach:" } },
            // ---- controls window ----
            { "Keyboard Controls",                    new[]{ "Teclado", "Teclado", "Clavier", "Tastatur", "Tastiera", "キーボード", "键盘控制", "Клавиатура", "Klawiatura" } },
            { "Controller Controls",                  new[]{ "Mando", "Controle", "Manette", "Controller", "Controller", "コントローラー", "手柄控制", "Геймпад", "Kontroler" } },
            { "Experimental",                         new[]{ "Experimental", "Experimental", "Expérimental", "Experimentell", "Sperimentale", "実験的機能", "实验功能", "Экспериментальное", "Eksperymentalne" } },
            { "Alternate",                            new[]{ "Altern.", "Altern.", "Autre", "Alt.", "Altern.", "代替", "备用", "Альт.", "Alt." } },
            { "Turn Left",                            new[]{ "Girar izq.", "Virar esq.", "Tourner à gauche", "Links drehen", "Gira a sinistra", "左を向く", "左转", "Поворот влево", "Obrót w lewo" } },
            { "Turn Right",                           new[]{ "Girar der.", "Virar dir.", "Tourner à droite", "Rechts drehen", "Gira a destra", "右を向く", "右转", "Поворот вправо", "Obrót w prawo" } },
            { "Action / Shoot",                       new[]{ "Acción / Disparo", "Ação / Atirar", "Action / Tir", "Aktion / Schuss", "Azione / Sparo", "決定 / 射撃", "动作 / 射击", "Действие / Огонь", "Akcja / Strzał" } },
            { "Flashlight",                           new[]{ "Linterna", "Lanterna", "Lampe torche", "Taschenlampe", "Torcia", "懐中電灯", "手电筒", "Фонарик", "Latarka" } },
            { "Map",                                  new[]{ "Mapa", "Mapa", "Carte", "Karte", "Mappa", "マップ", "地图", "Карта", "Mapa" } },
            { "Run",                                  new[]{ "Correr", "Correr", "Courir", "Rennen", "Corri", "ダッシュ", "奔跑", "Бег", "Bieg" } },
            { "Sidestep Left",                        new[]{ "Paso izq.", "Passo esq.", "Pas à gauche", "Schritt links", "Passo sinistra", "左ステップ", "左侧步", "Шаг влево", "Krok w lewo" } },
            { "Sidestep Right",                       new[]{ "Paso der.", "Passo dir.", "Pas à droite", "Schritt rechts", "Passo destra", "右ステップ", "右侧步", "Шаг вправо", "Krok w prawo" } },
            { "View",                                 new[]{ "Vista", "Visão", "Vue", "Sicht", "Vista", "視点", "视角", "Обзор", "Widok" } },
            { "Aim",                                  new[]{ "Apuntar", "Mirar", "Viser", "Zielen", "Mira", "構える", "瞄准", "Прицел", "Celowanie" } },
            { "Pause",                                new[]{ "Pausa", "Pausa", "Pause", "Pause", "Pausa", "ポーズ", "暂停", "Пауза", "Pauza" } },
            { "Inventory",                            new[]{ "Inventario", "Inventário", "Inventaire", "Inventar", "Inventario", "アイテム", "物品栏", "Инвентарь", "Ekwipunek" } },
            { "Reload",                               new[]{ "Recargar", "Recarregar", "Recharger", "Nachladen", "Ricarica", "リロード", "装弹", "Перезарядка", "Przeładuj" } },
            { "Quick Save",                           new[]{ "Guardado ráp.", "Salvar rápido", "Sauv. rapide", "Schnellspeichern", "Salv. rapido", "クイックセーブ", "快速保存", "Быстр. сохран.", "Szybki zapis" } },
            { "Quick Load",                           new[]{ "Carga rápida", "Carregar rápido", "Charg. rapide", "Schnellladen", "Caric. rapido", "クイックロード", "快速读取", "Быстр. загрузка", "Szybki odczyt" } },
            { "Rear Look",                            new[]{ "Mirar atrás", "Olhar atrás", "Regard arrière", "Zurückblicken", "Guarda dietro", "後方確認", "回头看", "Взгляд назад", "Widok wstecz" } },
            { "Change Camera",                        new[]{ "Cambiar cámara", "Trocar câmera", "Changer caméra", "Kamera wechseln", "Cambia camera", "カメラ切替", "切换镜头", "Смена камеры", "Zmień kamerę" } },
            { "Cycle Weapons",                        new[]{ "Cambiar arma", "Trocar arma", "Arme suivante", "Waffe wechseln", "Cambia arma", "武器切替", "切换武器", "Смена оружия", "Zmień broń" } },
            { "Quick Heal",                           new[]{ "Curación ráp.", "Cura rápida", "Soin rapide", "Schnellheilung", "Cura rapida", "クイック回復", "快速治疗", "Быстр. лечение", "Szybkie lecz." } },
            { "Quick Turn",                           new[]{ "Giro rápido", "Giro rápido", "Demi-tour", "Schnelldrehung", "Giro rapido", "クイックターン", "快速转身", "Разворот", "Szybki obrót" } },
            { "Control Style",                        new[]{ "Estilo", "Estilo", "Style", "Stil", "Stile", "スタイル", "风格", "Стиль", "Styl" } },
            { "Mouse Sensitivity",                    new[]{ "Sensib. ratón", "Sensib. mouse", "Sensib. souris", "Maus-Empfindl.", "Sensib. mouse", "マウス感度", "鼠标灵敏度", "Чувств. мыши", "Czułość myszy" } },
            { "Controller Sensitivity",               new[]{ "Sensib. mando", "Sensib. controle", "Sensib. manette", "Pad-Empfindl.", "Sensib. pad", "パッド感度", "手柄灵敏度", "Чувств. пада", "Czułość pada" } },
            { "First-person FOV",                     new[]{ "FOV 1ª persona", "FOV 1ª pessoa", "FOV 1re pers.", "Ego-FOV", "FOV 1a pers.", "一人称FOV", "第一人称FOV", "FOV 1-го лица", "FOV 1. osoby" } },
            { "Thirdperson FOV",                      new[]{ "FOV 3ª persona", "FOV 3ª pessoa", "FOV 3e pers.", "3rd-Person-FOV", "FOV 3a pers.", "三人称FOV", "第三人称FOV", "FOV 3-го лица", "FOV 3. osoby" } },
            { "TPS/OTS Aim Zoom",                     new[]{ "Zoom de mira", "Zoom de mira", "Zoom de visée", "Ziel-Zoom", "Zoom mira", "照準ズーム", "瞄准缩放", "Зум прицела", "Zoom celow." } },
            { "Allow debug controls:",                new[]{ "Controles debug:", "Controles debug:", "Contrôles debug :", "Debug-Steuerung:", "Comandi debug:", "デバッグ操作:", "调试控制:", "Отладка:", "Sterow. debug:" } },
            { "Alt. Cam Controls",                    new[]{ "Controles alt.", "Controles alt.", "Contrôles alt.", "Alt.-Steuerung", "Comandi alt.", "代替カメラ操作", "备用镜头控制", "Альт. камера", "Sterow. alt." } },
            { "Invert Mouse Y",                       new[]{ "Invertir Y ratón", "Inverter Y mouse", "Inverser Y souris", "Maus-Y invertieren", "Inverti Y mouse", "マウスY反転", "反转鼠标Y轴", "Инверсия Y мыши", "Odwróć Y myszy" } },
            { "Invert Controller Y",                  new[]{ "Invertir Y mando", "Inverter Y controle", "Inverser Y manette", "Pad-Y invertieren", "Inverti Y pad", "パッドY反転", "反转手柄Y轴", "Инверсия Y пада", "Odwróć Y pada" } },
            { "OTS aiming in Thirdperson",            new[]{ "Apuntado OTS en 3ª pers.", "Mira OTS em 3ª pessoa", "Visée OTS en 3e pers.", "OTS-Zielen in 3rd-Person", "Mira OTS in 3a pers.", "三人称でOTS照準", "第三人称越肩瞄准", "OTS-прицел в 3-м лице", "Celowanie OTS w 3. os." } },
            { "Crosshair",                            new[]{ "Retícula", "Mira", "Réticule", "Fadenkreuz", "Mirino", "照準", "准星", "Прицел", "Celownik" } },
            { "2D Controls (screen-relative)",        new[]{ "Controles 2D (rel. pantalla)", "Controles 2D (rel. à tela)", "Contrôles 2D (écran)", "2D-Steuerung (bildschirmrel.)", "Comandi 2D (rel. schermo)", "2D操作(画面基準)", "2D 控制(相对屏幕)", "2D-управление (по экрану)", "Sterowanie 2D (wzgl. ekranu)" } },
            { "Aim Assist (TPS/OTS)",                 new[]{ "Ayuda de mira (TPS/OTS)", "Auxílio de mira (TPS/OTS)", "Aide à la visée (TPS/OTS)", "Zielhilfe (TPS/OTS)", "Assist. mira (TPS/OTS)", "エイムアシスト (TPS/OTS)", "瞄准辅助 (TPS/OTS)", "Помощь прицела (TPS/OTS)", "Wspom. celowania (TPS/OTS)" } },
            { "Always use button based sprinting",    new[]{ "Correr siempre con botón", "Correr sempre com botão", "Toujours courir au bouton", "Sprinten immer per Taste", "Corsa sempre con tasto", "常にボタンでダッシュ", "始终使用按键冲刺", "Всегда бег по кнопке", "Zawsze bieg przyciskiem" } },
            { "Allow thirdperson camera collision",   new[]{ "Colisión de cámara en 3ª pers.", "Colisão de câmera em 3ª pes.", "Collision caméra 3e pers.", "3rd-Person-Kamerakollision", "Collisione camera 3a pers.", "三人称カメラの衝突判定", "第三人称镜头碰撞", "Коллизия камеры (3-е лицо)", "Kolizja kamery w 3. osobie" } },
            { "Disable D-pad for movement",           new[]{ "Desactivar cruceta al mover", "Desativar direcional ao mover", "Désactiver la croix (déplac.)", "Steuerkreuz: keine Bewegung", "Disattiva croce per movimento", "移動に十字キーを使わない", "禁用方向键移动", "Откл. крестовину для движения", "Wyłącz krzyżak do ruchu" } },
            { "Bullet decals",                        new[]{ "Marcas de bala", "Marcas de bala", "Impacts de balles", "Kugellöcher", "Fori dei proiettili", "弾痕", "弹孔", "Следы от пуль", "Ślady po kulach" } },
            { "Press Del to unbind",                  new[]{ "Supr para desasignar", "Del para desatribuir", "Suppr pour effacer", "Entf zum Löschen", "Canc per rimuovere", "Delキーで解除", "按 Del 解除绑定", "Del — снять привязку", "Del, aby usunąć" } },
            { "Reset to Defaults",                    new[]{ "Restaurar valores", "Restaurar padrões", "Réinitialiser", "Zurücksetzen", "Ripristina", "初期設定に戻す", "恢复默认", "Сбросить", "Przywróć domyślne" } },
            { "Save",                                 new[]{ "Guardar", "Salvar", "Enregistrer", "Speichern", "Salva", "保存", "保存", "Сохранить", "Zapisz" } },
            { "Alternate Camera Controls",            new[]{ "Controles de cámara alternativa", "Controles de câmera alternativa", "Commandes de caméra alternative", "Alternative Kamerasteuerung", "Comandi camera alternativa", "代替カメラの操作", "备用镜头控制", "Управление альт. камерой", "Sterowanie kamerą alternatywną" } },
            { "Leave the box unchecked to set classic controls, check it to set controls for modern TPS/OTS modes. Each control style also has alternates so that you can use more than one button for the same action.",  new[]{ "Deja la casilla sin marcar para configurar los controles clásicos, o márcala para configurar los de los modos modernos TPS/OTS. Cada estilo de control tiene además alternativas, así que puedes usar más de un botón para la misma acción.", "Deixe a caixa desmarcada para configurar os controles clássicos, ou marque-a para configurar os dos modos modernos TPS/OTS. Cada estilo de controle também tem alternativas, então você pode usar mais de um botão para a mesma ação.", "Laissez la case décochée pour régler les commandes classiques, ou cochez-la pour régler celles des modes modernes TPS/OTS. Chaque style de commandes possède aussi des touches alternatives, vous pouvez donc utiliser plusieurs boutons pour une même action.", "Lassen Sie das Kästchen leer, um die klassische Steuerung festzulegen, oder aktivieren Sie es für die modernen TPS/OTS-Modi. Jeder Steuerungsstil hat außerdem Alternativbelegungen, sodass Sie mehrere Tasten für dieselbe Aktion verwenden können.", "Lascia la casella deselezionata per impostare i comandi classici, oppure selezionala per impostare quelli delle modalità moderne TPS/OTS. Ogni stile di comandi ha anche delle alternative, così puoi usare più di un tasto per la stessa azione.", "チェックを外すとクラシック操作、チェックを入れると最新の TPS/OTS モード用の操作を設定します。どちらの操作スタイルにも代替割り当てがあるため、同じ動作に複数のボタンを使えます。", "取消勾选可设置经典控制，勾选则设置现代 TPS/OTS 模式的控制。每种控制风格还提供备用绑定，因此同一个动作可以使用多个按键。", "Оставьте флажок снятым, чтобы задать классическое управление, или установите его, чтобы задать управление для современных режимов TPS/OTS. У каждого стиля управления есть ещё и альтернативные привязки, поэтому одно действие можно назначить на несколько кнопок.", "Pozostaw pole niezaznaczone, aby ustawić klasyczne sterowanie, lub zaznacz je, aby ustawić sterowanie dla nowoczesnych trybów TPS/OTS. Każdy styl sterowania ma też alternatywne przypisania, więc tego samego działania możesz używać na kilku przyciskach." } },
        };
    }
}
