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
            { "Play",              new[]{ "Jugar", "Jogar", "Jouer", "Spielen", "Gioca", "プレイ", "开始游戏", "Играть", "Graj" } },
            { "Check for Updates", new[]{ "Buscar actualizaciones", "Procurar atualizações", "Rechercher des mises à jour", "Nach Updates suchen", "Cerca aggiornamenti", "更新を確認", "检查更新", "Проверить обновления", "Sprawdź aktualizacje" } },
            { "Changelog",         new[]{ "Novedades", "Novidades", "Journal des modifications", "Änderungsprotokoll", "Novità", "変更履歴", "更新日志", "Список изменений", "Lista zmian" } },
            { "View Changelog",    new[]{ "Ver novedades", "Ver novidades", "Voir les modifications", "Änderungen ansehen", "Vedi novità", "変更履歴を見る", "查看更新日志", "Показать изменения", "Zobacz zmiany" } },
            { "Controls",          new[]{ "Controles", "Controles", "Commandes", "Steuerung", "Comandi", "操作設定", "控制设置", "Управление", "Sterowanie" } },
            { "Build Settings",    new[]{ "Ajustes de compilación", "Config. da build", "Paramètres de build", "Build-Einstellungen", "Impostazioni build", "ビルド設定", "版本设置", "Настройки сборки", "Ustawienia kompilacji" } },
            { "Download Build",    new[]{ "Descargar compilación", "Baixar build", "Télécharger la build", "Build herunterladen", "Scarica build", "ビルドをダウンロード", "下载版本", "Скачать сборку", "Pobierz kompilację" } },
            { "Redownload Build",  new[]{ "Volver a descargar", "Baixar novamente", "Retélécharger", "Erneut herunterladen", "Riscarica build", "再ダウンロード", "重新下载", "Скачать заново", "Pobierz ponownie" } },
            { "Help",              new[]{ "Ayuda", "Ajuda", "Aide", "Hilfe", "Aiuto", "ヘルプ", "帮助", "Справка", "Pomoc" } },
            { "Report Bug",        new[]{ "Reportar error", "Reportar erro", "Signaler un bug", "Fehler melden", "Segnala bug", "バグを報告", "报告问题", "Сообщить об ошибке", "Zgłoś błąd" } },
            { "Reset",             new[]{ "Restablecer", "Redefinir", "Réinitialiser", "Zurücksetzen", "Ripristina", "リセット", "重置", "Сброс", "Resetuj" } },

            // ---- main window: setting labels ----
            { "Skip Intros:",      new[]{ "Saltar intros:", "Pular intros:", "Passer les intros :", "Intros überspringen:", "Salta intro:", "イントロをスキップ:", "跳过开场:", "Пропуск заставок:", "Pomiń intra:" } },
            { "Display:",          new[]{ "Pantalla:", "Tela:", "Écran :", "Anzeige:", "Schermo:", "ディスプレイ:", "显示器:", "Дисплей:", "Ekran:" } },
            { "VSync:",            new[]{ "VSync:", "VSync:", "VSync :", "VSync:", "VSync:", "垂直同期:", "垂直同步:", "Вертик. синхр.:", "VSync:" } },
            { "Resolution:",       new[]{ "Resolución:", "Resolução:", "Résolution :", "Auflösung:", "Risoluzione:", "解像度:", "分辨率:", "Разрешение:", "Rozdzielczość:" } },
            { "Pillarboxing:",     new[]{ "Bandas laterales:", "Barras laterais:", "Bandes latérales :", "Seitenbalken:", "Bande laterali:", "ピラーボックス:", "黑边显示:", "Боковые поля:", "Pasy boczne:" } },
            { "FPS Limit:",        new[]{ "Límite de FPS:", "Limite de FPS:", "Limite FPS :", "FPS-Limit:", "Limite FPS:", "FPS制限:", "帧率限制:", "Лимит кадров:", "Limit FPS:" } },
            { "Preload Chunks:",   new[]{ "Precargar bloques:", "Pré-carregar blocos:", "Préchargement :", "Vorladen:", "Precarica blocchi:", "事前読み込み:", "预加载区块:", "Предзагрузка:", "Wstępne ładowanie:" } },
            { "Filtering:",        new[]{ "Filtrado:", "Filtragem:", "Filtrage :", "Filterung:", "Filtro:", "フィルタ:", "纹理过滤:", "Фильтрация:", "Filtrowanie:" } },
            { "Use PGXP:",         new[]{ "Usar PGXP:", "Usar PGXP:", "Utiliser PGXP :", "PGXP verwenden:", "Usa PGXP:", "PGXPを使う:", "启用 PGXP:", "Использовать PGXP:", "Użyj PGXP:" } },
            { "Enable Logging:",   new[]{ "Registro:", "Registro:", "Journalisation :", "Protokollierung:", "Log:", "ログ出力:", "启用日志:", "Вести журнал:", "Logowanie:" } },
            { "External Console:", new[]{ "Consola externa:", "Console externo:", "Console externe :", "Externe Konsole:", "Console esterna:", "外部コンソール:", "外部控制台:", "Внешняя консоль:", "Konsola zewnętrzna:" } },
            { "Antialiasing:",     new[]{ "Suavizado:", "Suavização:", "Anticrénelage :", "Kantenglättung:", "Antialiasing:", "アンチエイリアス:", "抗锯齿:", "Сглаживание:", "Antialiasing:" } },
            { "Post Effect:",      new[]{ "Postproceso:", "Pós-efeito:", "Post-traitement :", "Nachbearbeitung:", "Post-effetto:", "ポストエフェクト:", "后期效果:", "Постобработка:", "Efekt końcowy:" } },
            { "Tone Map:",         new[]{ "Mapeo tonal:", "Mapa de tons:", "Mappage tonal :", "Tone Mapping:", "Mappatura toni:", "トーンマップ:", "色调映射:", "Тонмаппинг:", "Mapowanie tonów:" } },
            { "Flashlight:",       new[]{ "Linterna:", "Lanterna:", "Lampe torche :", "Taschenlampe:", "Torcia:", "懐中電灯:", "手电筒:", "Фонарик:", "Latarka:" } },
            { "Disk Image:",       new[]{ "Imagen de disco:", "Imagem de disco:", "Image disque :", "Datenträgerabbild:", "Immagine disco:", "ディスクイメージ:", "光盘镜像:", "Образ диска:", "Obraz płyty:" } },
            { "Audio Out:",        new[]{ "Salida de audio:", "Saída de áudio:", "Sortie audio :", "Audioausgabe:", "Uscita audio:", "音声出力:", "音频输出:", "Вывод звука:", "Wyjście audio:" } },
            { "Level:",            new[]{ "Nivel:", "Nível:", "Niveau :", "Level:", "Livello:", "レベル:", "关卡:", "Уровень:", "Poziom:" } },
            { "Menus:",            new[]{ "Menús:", "Menus:", "Menus :", "Menüs:", "Menu:", "メニュー:", "菜单:", "Меню:", "Menu:" } },

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
            { "Update available!",             new[]{ "¡Actualización disponible!", "Atualização disponível!", "Mise à jour disponible !", "Update verfügbar!", "Aggiornamento disponibile!", "更新があります!", "有可用更新!", "Доступно обновление!", "Dostępna aktualizacja!" } },
            { "Update failed (see message).",  new[]{ "Fallo al actualizar (ver mensaje).", "Falha ao atualizar (ver mensagem).", "Échec de la mise à jour (voir message).", "Update fehlgeschlagen (siehe Meldung).", "Aggiornamento fallito (vedi messaggio).", "更新に失敗しました(メッセージ参照)", "更新失败(见提示)。", "Не удалось обновить (см. сообщение).", "Aktualizacja nieudana (zobacz komunikat)." } },
            { "Checking selected build...",    new[]{ "Comprobando la compilación...", "Verificando a build...", "Vérification de la build...", "Build wird geprüft...", "Controllo della build...", "ビルドを確認中...", "正在检查版本...", "Проверка сборки...", "Sprawdzanie kompilacji..." } },
            { "Download cancelled.",           new[]{ "Descarga cancelada.", "Download cancelado.", "Téléchargement annulé.", "Download abgebrochen.", "Download annullato.", "ダウンロードを中止しました。", "下载已取消。", "Загрузка отменена.", "Pobieranie anulowane." } },
            { "Download failed (see message).",new[]{ "Fallo al descargar (ver mensaje).", "Falha no download (ver mensagem).", "Échec du téléchargement (voir message).", "Download fehlgeschlagen (siehe Meldung).", "Download fallito (vedi messaggio).", "ダウンロードに失敗しました(メッセージ参照)", "下载失败(见提示)。", "Ошибка загрузки (см. сообщение).", "Pobieranie nieudane (zobacz komunikat)." } },
            { "Disc: Auto",                    new[]{ "Disco: automático", "Disco: automático", "Disque : auto", "Disc: Automatisch", "Disco: automatico", "ディスク: 自動", "光盘: 自动", "Диск: авто", "Płyta: auto" } },
            { "Change background",             new[]{ "Cambiar fondo", "Alterar fundo", "Changer de fond", "Hintergrund ändern", "Cambia sfondo", "背景を変更", "更换背景", "Сменить фон", "Zmień tło" } },
            { "Dark mode",                     new[]{ "Modo oscuro", "Modo escuro", "Mode sombre", "Dunkler Modus", "Modalità scura", "ダークモード", "深色模式", "Тёмная тема", "Tryb ciemny" } },
            { "Language",                      new[]{ "Idioma", "Idioma", "Langue", "Sprache", "Lingua", "言語", "语言", "Язык", "Język" } },
            { "Launcher language",             new[]{ "Idioma del lanzador", "Idioma do inicializador", "Langue du lanceur", "Launcher-Sprache", "Lingua del launcher", "ランチャーの言語", "启动器语言", "Язык лаунчера", "Język launchera" } },

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
        };
    }
}
