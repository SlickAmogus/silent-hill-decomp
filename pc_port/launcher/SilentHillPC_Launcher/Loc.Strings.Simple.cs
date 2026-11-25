using System;
using System.Collections.Generic;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// Strings of the simple view. Same layout and rules as Loc.Strings.cs:
    /// keyed by English, columns ES, PT, FR, DE, IT, JA, ZH, RU, PL.
    /// </summary>
    public static partial class Loc
    {
        static readonly Dictionary<string, string[]> SimpleTable =
            new Dictionary<string, string[]>(StringComparer.Ordinal)
        {
            { "Quality",
                new[]{ "Calidad", "Qualidade", "Qualité", "Qualität", "Qualità", "画質", "画质", "Качество", "Jakość" } },
            { "Options",
                new[]{ "Opciones", "Opções", "Options", "Optionen", "Opzioni", "オプション", "选项", "Параметры", "Opcje" } },
            { "Logging",
                new[]{ "Registro", "Registro", "Journalisation", "Protokollierung", "Registro", "ログ記録", "日志记录", "Журнал", "Rejestrowanie" } },
            { "CRT Filter",
                new[]{ "Filtro CRT", "Filtro CRT", "Filtre CRT", "CRT-Filter", "Filtro CRT", "CRTフィルター", "CRT 滤镜", "CRT-фильтр", "Filtr CRT" } },
            { "Mode:",
                new[]{ "Modo:", "Modo:", "Mode :", "Modus:", "Modalità:", "モード:", "模式:", "Режим:", "Tryb:" } },
            { "Simple",
                new[]{ "Simple", "Simples", "Simple", "Einfach", "Semplice", "シンプル", "简单", "Простой", "Prosty" } },
            { "Advanced",
                new[]{ "Avanzado", "Avançado", "Avancé", "Erweitert", "Avanzata", "詳細", "高级", "Расширенный", "Zaawansowany" } },
            { "Advanced mode",
                new[]{ "Modo avanzado", "Modo avançado", "Mode avancé", "Erweiterter Modus", "Modalità avanzata", "詳細モード", "高级模式", "Расширенный режим", "Tryb zaawansowany" } },
            { "Custom",
                new[]{ "Personalizado", "Personalizado", "Personnalisé", "Benutzerdefiniert", "Personalizzato", "カスタム", "自定义", "Свои настройки", "Własne" } },
            { "Settings changed in Advanced mode or in the game.",
                new[]{
                    "Ajustes cambiados en el modo avanzado o en el juego.",
                    "Configurações alteradas no modo avançado ou no jogo.",
                    "Réglages modifiés en mode avancé ou dans le jeu.",
                    "Im erweiterten Modus oder im Spiel geänderte Einstellungen.",
                    "Impostazioni modificate in modalità avanzata o nel gioco.",
                    "詳細モードまたはゲーム内で変更された設定。",
                    "在高级模式或游戏中更改过的设置。",
                    "Настройки, изменённые в расширенном режиме или в игре.",
                    "Ustawienia zmienione w trybie zaawansowanym lub w grze." } },
            { "Simple Mode",
                new[]{ "Modo simple", "Modo simples", "Mode simple", "Einfacher Modus", "Modalità semplice", "シンプルモード", "简单模式", "Простой режим", "Tryb prosty" } },
            { "Simple Mode has been added to the launcher, would you like to switch to it now?",
                new[]{
                    "Se ha añadido el modo simple al lanzador. ¿Quieres cambiar a él ahora?",
                    "O modo simples foi adicionado ao launcher. Deseja mudar para ele agora?",
                    "Le mode simple a été ajouté au lanceur. Voulez-vous y passer maintenant ?",
                    "Der einfache Modus wurde zum Launcher hinzugefügt. Möchten Sie jetzt dorthin wechseln?",
                    "La modalità semplice è stata aggiunta al launcher. Vuoi passarci ora?",
                    "ランチャーにシンプルモードが追加されました。今すぐ切り替えますか?",
                    "启动器新增了简单模式，要现在切换吗?",
                    "В лаунчер добавлен простой режим. Переключиться на него сейчас?",
                    "Do launchera dodano tryb prosty. Czy chcesz teraz się na niego przełączyć?" } },
            { "Enhanced: Normal",
                new[]{ "Mejorado: Normal", "Aprimorado: Normal", "Amélioré : Normal", "Verbessert: Normal", "Migliorato: Normale", "強化: 標準", "增强: 标准", "Улучшенный: обычный", "Ulepszony: normalny" } },
            { "Enhanced: Low Spec",
                new[]{ "Mejorado: Equipo modesto", "Aprimorado: PC modesto", "Amélioré : Petite config", "Verbessert: Schwache Hardware", "Migliorato: PC modesto", "強化: 低スペック", "增强: 低配置", "Улучшенный: слабый ПК", "Ulepszony: słabszy sprzęt" } },
            { "Enhanced: High Quality",
                new[]{ "Mejorado: Calidad alta", "Aprimorado: Qualidade alta", "Amélioré : Haute qualité", "Verbessert: Hohe Qualität", "Migliorato: Qualità alta", "強化: 高画質", "增强: 高画质", "Улучшенный: высокое качество", "Ulepszony: wysoka jakość" } },
            { "Enhanced: Ultra Quality",
                new[]{ "Mejorado: Calidad ultra", "Aprimorado: Qualidade ultra", "Amélioré : Qualité ultra", "Verbessert: Ultra-Qualität", "Migliorato: Qualità ultra", "強化: 最高画質", "增强: 极致画质", "Улучшенный: ультра", "Ulepszony: jakość ultra" } },
            { "The game as it was on the PlayStation: its own resolution, 30 fps and the original lighting.",
                new[]{
                    "El juego tal como era en PlayStation: su propia resolución, 30 fps y la iluminación original.",
                    "O jogo como era no PlayStation: sua própria resolução, 30 fps e a iluminação original.",
                    "Le jeu tel qu'il était sur PlayStation : sa propre résolution, 30 ips et l'éclairage d'origine.",
                    "Das Spiel wie auf der PlayStation: eigene Auflösung, 30 fps und die originale Beleuchtung.",
                    "Il gioco com'era su PlayStation: la sua risoluzione, 30 fps e l'illuminazione originale.",
                    "PlayStation版そのままの見た目: 本来の解像度、30fps、オリジナルのライティング。",
                    "与 PlayStation 原版一致: 原始分辨率、30 帧和原版光照。",
                    "Игра как на PlayStation: родное разрешение, 30 кадров/с и исходное освещение.",
                    "Gra taka jak na PlayStation: własna rozdzielczość, 30 kl./s i oryginalne oświetlenie." } },
            { "Desktop resolution, no FPS limit, 2x antialiasing, sharper textures and flashlight shadows.",
                new[]{
                    "Resolución del escritorio, sin límite de FPS, antialiasing 2x, texturas más nítidas y sombras de la linterna.",
                    "Resolução da área de trabalho, sem limite de FPS, antialiasing 2x, texturas mais nítidas e sombras da lanterna.",
                    "Résolution du bureau, pas de limite d'ips, anticrénelage 2x, textures plus nettes et ombres de la lampe.",
                    "Desktop-Auflösung, kein FPS-Limit, 2x Kantenglättung, schärfere Texturen und Taschenlampenschatten.",
                    "Risoluzione del desktop, nessun limite FPS, antialiasing 2x, texture più nitide e ombre della torcia.",
                    "デスクトップ解像度、FPS制限なし、2xアンチエイリアス、より鮮明なテクスチャ、ライトの影。",
                    "桌面分辨率、不限帧率、2x 抗锯齿、更清晰的纹理和手电筒阴影。",
                    "Разрешение рабочего стола, без ограничения FPS, сглаживание 2x, чёткие текстуры и тени от фонарика.",
                    "Rozdzielczość pulpitu, bez limitu FPS, antyaliasing 2x, ostrzejsze tekstury i cienie latarki." } },
            { "Desktop resolution and smooth textures with no extra effects. For older or integrated graphics.",
                new[]{
                    "Resolución del escritorio y texturas suavizadas sin efectos adicionales. Para gráficas antiguas o integradas.",
                    "Resolução da área de trabalho e texturas suavizadas sem efeitos extras. Para placas antigas ou integradas.",
                    "Résolution du bureau et textures lissées sans effets supplémentaires. Pour les cartes anciennes ou intégrées.",
                    "Desktop-Auflösung und geglättete Texturen ohne zusätzliche Effekte. Für ältere oder integrierte Grafik.",
                    "Risoluzione del desktop e texture levigate senza effetti aggiuntivi. Per schede vecchie o integrate.",
                    "デスクトップ解像度と滑らかなテクスチャのみ、追加効果なし。古いGPUや内蔵GPU向け。",
                    "桌面分辨率和平滑纹理，无额外特效。适合较旧的显卡或集成显卡。",
                    "Разрешение рабочего стола и сглаженные текстуры без доп. эффектов. Для старых и встроенных видеокарт.",
                    "Rozdzielczość pulpitu i wygładzone tekstury bez dodatkowych efektów. Dla starszych lub zintegrowanych kart." } },
            { "4x antialiasing and the modern flashlight with detailed shadows.",
                new[]{
                    "Antialiasing 4x y la linterna moderna con sombras detalladas.",
                    "Antialiasing 4x e a lanterna moderna com sombras detalhadas.",
                    "Anticrénelage 4x et la lampe moderne avec des ombres détaillées.",
                    "4x Kantenglättung und die moderne Taschenlampe mit detaillierten Schatten.",
                    "Antialiasing 4x e la torcia moderna con ombre dettagliate.",
                    "4xアンチエイリアスと、精細な影付きのモダンなライト。",
                    "4x 抗锯齿，以及带精细阴影的现代手电筒。",
                    "Сглаживание 4x и современный фонарик с детальными тенями.",
                    "Antyaliasing 4x i nowoczesna latarka ze szczegółowymi cieniami." } },
            { "8x antialiasing and the highest shadow detail. For powerful graphics cards.",
                new[]{
                    "Antialiasing 8x y el máximo detalle de sombras. Para tarjetas gráficas potentes.",
                    "Antialiasing 8x e o máximo detalhe de sombras. Para placas de vídeo potentes.",
                    "Anticrénelage 8x et le plus haut niveau de détail des ombres. Pour les cartes graphiques puissantes.",
                    "8x Kantenglättung und die höchste Schattenqualität. Für leistungsstarke Grafikkarten.",
                    "Antialiasing 8x e il massimo dettaglio delle ombre. Per schede grafiche potenti.",
                    "8xアンチエイリアスと最高品質の影。高性能GPU向け。",
                    "8x 抗锯齿和最高的阴影细节。适合高性能显卡。",
                    "Сглаживание 8x и максимальная детализация теней. Для мощных видеокарт.",
                    "Antyaliasing 8x i najwyższa szczegółowość cieni. Dla wydajnych kart graficznych." } },
        };
    }
}
