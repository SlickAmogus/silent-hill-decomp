/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc_options_help.h"
#include <string.h>

#define H(k, en, ru, de, dr, p) { k, { en, ru }, { de, dr }, p }
static const PcOptionHelp s_help[] = {
    H("check_updates", "Check for updates", "Проверять обновления",
      "Android checks for a newer realtime build when the app starts. Applies on the next launch; it does not change the installed version immediately.",
      "Android проверяет наличие новой realtime-сборки при запуске. Изменение действует со следующего запуска и не заменяет текущую версию сразу.", 0),
    H("chara_per_pixel_light", "Improved model lighting", "Улучшенное освещение моделей",
      "Lights replacement glTF characters per pixel, including their normal maps. Off uses per-vertex lighting. This does not affect stock Harry without a model replacement. The example uses a separate test character, not an installed mod.",
      "Только для заменённых glTF-персонажей, не обычного Гарри. В примере — тестовая модель, не установленный мод. Вкл.: свет попиксельно; выкл.: по вершинам.", 0),
    H("per_pixel_lighting", "Per-pixel world lighting", "Попиксельное освещение мира",
      "Evaluates lighting per pixel for supported world geometry and stock characters, instead of only at polygon vertices. The example uses a fixed school scene with the engine's fog-enabled world-lighting path; other rendering paths may show little or no difference.",
      "Свет на геометрии и обычных персонажах: попиксельно вместо вершин. Пример — школа с включённым проходом тумана. Эффект зависит от сцены и прохода отрисовки.", 0),
    H("hero_light", "Character fill light", "Заполняющий свет персонажей",
      "A soft virtual light above and to the left of the camera reveals the dark sides of characters. Levels: off, low, medium, high. It does not brighten the flashlight or light walls. The stock-character example keeps per-pixel lighting enabled.",
      "Мягкая подсветка персонажа сверху-слева от камеры, не фонарика и стен. Уровни: выкл., слабый, средний, сильный. В примере попиксельное освещение включено.", 0),
    H("width", "Resolution", "Разрешение",
      "Configured game resolution in pixels. Higher resolutions can improve image clarity but require more GPU power. Native-resolution rendering has its own internal resolution.",
      "Заданное разрешение игры в пикселях. Большее разрешение повышает чёткость, но увеличивает нагрузку. У режима исходного разрешения отдельное внутреннее разрешение.", 0),
    H("fullscreen", "Window mode", "Режим окна",
      "Choose a window, fullscreen or borderless window. Your display mode is not changed by resetting PC options.",
      "Окно, полный экран или окно без рамки. Сброс настроек ПК не меняет разрешение и режим окна.", 0),
    H("vsync", "VSync", "Вертикальная синхронизация",
      "Synchronizes presentation with the display to reduce tearing. May add input latency. The FPS limit is a separate setting.",
      "Синхронизирует вывод с монитором, уменьшая разрывы изображения. Может увеличить задержку ввода. Лимит FPS задаётся отдельно.", 0),
    H("texture_filter", "Texture filtering", "Фильтрация текстур",
      "Off preserves sharp original texels. Bilinear/trilinear filtering smooths textures; anisotropic filtering improves surfaces viewed at an angle.",
      "Без фильтрации видны исходные пиксели текстур. Билинейная и трилинейная фильтрация сглаживают их; анизотропная улучшает поверхности под углом.", 0),
    H("use_pgxp", "PGXP geometry correction", "Коррекция геометрии PGXP",
      "Uses higher-precision projected coordinates to reduce PlayStation-style polygon wobble and texture distortion.",
      "Повышает точность проекции, уменьшая дрожание полигонов и искажения текстур, характерные для PlayStation.", 0),
    H("msaa", "Antialiasing", "Сглаживание",
      "Smooths polygon edges. Higher sample counts use more GPU memory and processing power. Requires restarting the game.",
      "Сглаживает края полигонов. Больше выборок — выше расход видеопамяти и нагрузка на видеокарту. Нужен перезапуск игры.", 0),
    H("post_process", "Post-processing", "Постобработка",
      "Applies a screen effect such as CRT, scanlines, film grain or sharpening. This changes the final image, not the game geometry.",
      "Применяет эффект экрана: ЭЛТ, строки развёртки, зерно или резкость. Меняет итоговое изображение, а не геометрию игры.", 0),
    H("tonemap", "Tone mapping", "Тональная компрессия",
      "Changes how scene brightness and contrast are mapped to the display. Off keeps the unmodified output.",
      "Меняет передачу яркости и контраста сцены на экране. Выключение сохраняет исходное изображение.", 0),
    H("map", "New-game map", "Карта новой игры",
      "Starting map for a NEW game, not a teleport and not a save selector. Unused/debug maps may be incomplete or crash. Does not move your current character.",
      "Стартовая карта НОВОЙ игры, не телепорт и не выбор сохранения. Неиспользуемые карты могут быть неполными или вызывать вылет. Не перемещает текущего персонажа.", 0),
    H("flashlight_mode", "Flashlight", "Фонарик",
      "Classic approximates the original lighting. Modern uses a per-pixel spotlight. Shadow variants add real-time shadows and cost more GPU time.",
      "Классический режим приближен к оригиналу. Современный использует попиксельный свет. Варианты с тенями добавляют динамические тени и повышают нагрузку.", 0),
    H("flashlight_intensity", "Beam intensity", "Яркость луча",
      "Intensity of the flashlight beam. The school preview uses a dark scene; actual rooms, materials and flashlight modes can look different.",
      "Яркость луча фонарика. Школьный пример показывает тёмную сцену; в других комнатах, на разных материалах и в разных режимах освещения результат может отличаться.", 0),
    H("flashlight_size", "Beam size", "Размер луча",
      "Width of the flashlight beam. Its appearance also depends on the selected flashlight mode.",
      "Ширина луча фонарика. Его вид также зависит от выбранного режима освещения.", 0),
    H("fog_strength", "Fog strength", "Плотность тумана",
      "Controls fog density, not the simulation speed. Lower density reveals more distant geometry, if that geometry is being drawn.",
      "Плотность тумана, не скорость его движения. При уменьшении видна дальняя геометрия, если она отрисовывается.", 0),
    H("disable_culling", "Disable Culling", "Выключить Culling",
      "Culling skips map chunks considered invisible by the original visibility/frustum tests. ON disables those tests: more geometry may appear, but performance can drop and unfinished areas may become visible. This is not back-face culling.",
      "Culling исключает части карты, невидимые по исходным проверкам видимости и камеры. ВКЛ. отключает эти проверки: геометрии может стать больше, но FPS может снизиться, а незавершённые области — стать видимыми. Это не отсечение обратных граней.", PC_PREVIEW_CULLING),
    H("fps_cap", "FPS limit", "Лимит FPS",
      "Maximum rendered frames per second. Off removes the cap. High limits increase GPU load; VSync may impose a lower effective limit.",
      "Максимум кадров в секунду. Выключение снимает ограничение. Высокий лимит увеличивает нагрузку; VSync может ограничить частоту сильнее.", 0),
    H("fmv_volume", "Movie volume", "Громкость видеороликов",
      "Volume of pre-rendered movie audio, separate from music and gameplay sound effects.",
      "Громкость звука заранее записанных видеороликов, отдельно от музыки и игровых эффектов.", 0),
    H("weather_sim_hz", "Weather simulation rate", "Частота обновления осадков",
      "Rain/snow simulation: 30 Hz keeps the console cadence; 60 Hz selects the per-render-frame path (so its actual cadence follows FPS). This is not the amount of rain. The street example stays animated.",
      "Симуляция дождя и снега: 30 Гц сохраняет консольный ритм; 60 Гц включает обновление на каждом кадре (реальная частота зависит от FPS). Это не количество осадков. Уличный пример остаётся анимированным.", 0),
    H("control_2d", "2D controls", "2D управление",
      "ON: movement directions are relative to the screen/camera. OFF: classic tank controls, with forward/back relative to Harry and left/right turning him. Alternate camera modes may handle movement separately.",
      "ВКЛ.: направление движения задаётся относительно экрана и камеры. ВЫКЛ.: танковое управление — вперёд и назад относительно Гарри, влево и вправо поворачивают его. Альтернативные камеры могут обрабатывать движение отдельно.", PC_PREVIEW_CONTROLS),
    H("mouse_sensitivity", "Mouse sensitivity", "Чувствительность мыши",
      "Scales mouse-look input in the alternate camera modes. Does not change the menu pointer speed.",
      "Множитель движения мыши для обзора в альтернативных режимах камеры. Не меняет скорость указателя в меню.", 0),
    H("controller_sensitivity", "Gamepad sensitivity", "Чувствительность геймпада",
      "Scales gamepad camera-look input in alternate camera modes. Does not change Harry's walking speed.",
      "Множитель ввода геймпада для обзора в альтернативных режимах камеры. Не меняет скорость ходьбы Гарри.", 0),
    H("invert_mouse_y", "Invert mouse Y", "Инверсия мыши по Y",
      "Reverses vertical mouse-look input: moving the mouse up looks down, and vice versa.",
      "Меняет направление вертикального обзора мышью: движение вверх направляет взгляд вниз, и наоборот.", 0),
    H("invert_controller_y", "Invert gamepad Y", "Инверсия геймпада по Y",
      "Reverses vertical camera-look input on the gamepad.",
      "Меняет направление вертикального обзора камеры на геймпаде.", 0),
    H("bullet_decals", "Bullet marks", "Следы от пуль",
      "Displays bullet impact marks on supported surfaces. Cosmetic only; does not change weapon damage.",
      "Показывает следы попаданий на поддерживаемых поверхностях. Не влияет на урон оружия.", 0),
    H("aim_assist", "Aim assist", "Помощь в прицеливании",
      "Enables PC aiming assistance in third-person/over-shoulder camera modes. Separate from the original extra-options auto-aim setting; does not apply to first-person aiming.",
      "Включает помощь в прицеливании ПК-порта от третьего лица и из-за плеча. Это не автоприцел из дополнительных опций оригинала; не применяется к прицеливанию от первого лица.", 0),
    H("fps_fov", "First-person FOV", "FOV от 1-го лица",
      "Field of view for the first-person camera. A larger angle shows more surroundings and makes objects look smaller. Independent of the other camera modes.",
      "Угол поля зрения камеры от первого лица. Больший угол показывает больше окружения, но объекты выглядят меньше. Не меняет FOV других камер.", PC_PREVIEW_FOV),
    H("tps_fov", "Third-person FOV", "FOV от 3-го лица",
      "Field of view for the third-person camera. A larger angle shows more surroundings. Does not change the classic fixed-camera view.",
      "Угол поля зрения камеры от третьего лица. Больший угол показывает больше окружения. Не меняет классическую фиксированную камеру.", PC_PREVIEW_FOV),
    H("ots_fov", "Over-the-shoulder FOV", "FOV из-за плеча",
      "Field of view for the over-the-shoulder camera. Independent of first-person and third-person FOV.",
      "Угол поля зрения камеры из-за плеча. Задаётся независимо от FOV первого и третьего лица.", PC_PREVIEW_FOV),
    H("tps_aim_zoom_amount", "Aim zoom (third person)", "Приближение при прицеливании",
      "Third-person aim zoom: 0 removes zoom, 100 is the original amount. Negative values pull the aim camera back. Not a FOV setting for the classic camera.",
      "Приближение при прицеливании от третьего лица: 0 — без приближения, 100 — исходное. Отрицательные значения отдаляют камеру. Не меняет FOV классической камеры.", 0),
    H("ots_aim_zoom_amount", "Aim zoom (over shoulder)", "Приближение из-за плеча",
      "Aim zoom for the over-the-shoulder camera: 0 removes zoom, 100 is the original amount; negative values pull the camera back.",
      "Приближение при прицеливании из-за плеча: 0 — без приближения, 100 — исходное. Отрицательные значения отдаляют камеру.", 0),
    H("tps_ots_aim", "Over-shoulder aiming in TPS", "Прицеливание из-за плеча",
      "Switches to an over-the-shoulder aiming view while using the third-person camera.",
      "Переключает на вид прицеливания из-за плеча при использовании камеры от третьего лица.", 0),
    H("tps_camera_collision", "Camera collision", "Коллизия камеры",
      "Pulls the third-person/over-shoulder camera closer when a wall would block it, reducing camera clipping through geometry. OFF allows the camera to pass through walls. Does not change Harry's collisions.",
      "Приближает камеру от третьего лица или из-за плеча, если ей мешает стена, уменьшая прохождение камеры сквозь геометрию. ВЫКЛ. позволяет камере проходить сквозь стены. Не меняет столкновения Гарри.", PC_PREVIEW_CAMERA),
    H("minimap", "Minimap", "Миникарта",
      "Shows a square or circular minimap. Off hides it. Availability can also depend on the paper-map requirement below.",
      "Квадратная или круглая миникарта. Выключение скрывает её. Доступность также зависит от требования бумажной карты ниже.", PC_PREVIEW_MAP),
    H("minimap_scale", "Minimap scale", "Масштаб миникарты",
      "Changes the minimap display scale. The paper map and the game world's scale are unchanged.",
      "Меняет масштаб отображения миникарты. Не меняет бумажную карту и масштаб игрового мира.", PC_PREVIEW_MAP),
    H("minimap_corner", "Minimap corner", "Положение миникарты",
      "Chooses which corner of the screen holds the minimap.",
      "Выбирает угол экрана, в котором отображается миникарта.", PC_PREVIEW_MAP),
    H("minimap_opacity", "Minimap opacity", "Прозрачность миникарты",
      "Opacity, not transparency: 0 is invisible, 100 is fully opaque. The Russian transparency label uses the same stored value, without reversing the setting.",
      "Степень НЕПРОЗРАЧНОСТИ: 0 — полностью прозрачная миникарта, 100 — непрозрачная. Значение настройки не перевёрнуто: чем выше число, тем меньше прозрачность.", PC_PREVIEW_MAP),
    H("minimap_require_map", "Minimap requires paper map", "Миникарта: нужна карта района",
      "ON: the minimap normally appears only after Harry finds the area's paper map. OFF: allows it before finding that map. The quick-options menu may temporarily show it for adjustment.",
      "ВКЛ.: обычно миникарта появляется только после нахождения Гарри бумажной карты района. ВЫКЛ.: позволяет показывать её до находки. Быстрое меню может временно показать миникарту для настройки.", PC_PREVIEW_MAP),
    H("low_health_glow", "Low-health warning", "Сигнал малого здоровья",
      "Pulsing red screen-edge glow when Harry's health falls below 20. A visual warning only; does not heal or prevent damage.",
      "Пульсирующее красное свечение по краям экрана при здоровье Гарри ниже 20. Только предупреждение: не лечит и не защищает от урона.", 0),
    H("crosshair", "Crosshair", "Прицел",
      "Shows the PC aiming crosshair in supported camera modes. Does not change weapon accuracy.",
      "Показывает прицельную метку ПК-порта в поддерживаемых режимах камеры. Не меняет точность оружия.", 0),
    H("crosshair_size", "Crosshair size", "Размер прицела",
      "Display size of the aiming crosshair. Cosmetic only; does not change spread or hit detection.",
      "Размер прицельной метки. Не влияет на разброс пуль и определение попадания.", 0),
    H("text_size", "Text size", "Размер текста",
      "Scales subtitles, memos and map messages. Does not scale fixed-layout original menus or this responsive PC settings screen.",
      "Масштаб субтитров, записок и сообщений карт. Не меняет исходные меню с фиксированной раскладкой и этот адаптивный экран настроек ПК.", 0),
    H("game_data", "Game data location", "Расположение данных",
      "Choose the data folder on next launch. Existing disc images, settings and saves are not moved automatically. Confirm only if you want to change folders.",
      "Выбор папки данных при следующем запуске. Образ диска, настройки и сохранения автоматически не перемещаются. Подтверждайте только для смены папки.", 0),
    H("allow_loose_files", "Load mods", "Загружать модификации",
      "Allows replacement textures, models, sounds and text from gamedata/load. Loaded rooms keep their current assets until reloaded. Does not download mods.",
      "Разрешает замену текстур, моделей, звуков и текста из gamedata/load. Уже загруженные ресурсы обновятся после перезагрузки. Модификации не скачивает.", 0),
    H("touch_controls", "Touch controls", "Сенсорное управление",
      "Automatic hides touch controls when using a gamepad and restores them on touch. Always on keeps them visible; Always off hides the on-screen controls.",
      "Авто скрывает сенсорные кнопки при игре с геймпадом и возвращает при касании. Можно включить или выключить их постоянно.", 0),
    H("touch_look_sensitivity", "Touch look speed", "Скорость обзора пальцем",
      "Sensitivity of camera look when dragging a finger. Does not change character speed or menu touch targets.",
      "Чувствительность обзора при движении пальцем. Не меняет скорость персонажа и размеры кнопок меню.", 0),
    H("one_button_combat", "One-button combat", "Бой одной кнопкой",
      "Combines aiming and attack on the touch combat button. Turn off to use separate aim and attack controls.",
      "Объединяет прицеливание и атаку на сенсорной кнопке боя. Выключите для раздельного управления прицелом и атакой.", 0),
    H("touch_style", "Touch layout", "Схема сенсорных кнопок",
      "Context uses a floating movement stick and contextual buttons. Fixed context pins the stick. Gamepad displays a fixed virtual PlayStation controller.",
      "Контекстная схема: плавающий стик и кнопки по ситуации. Фиксированная закрепляет стик. Геймпад показывает виртуальные кнопки PlayStation.", 0),
    H("touch_quicksave_buttons", "Quick-save buttons", "Кнопки быстрого сохранения",
      "Shows quick-save and quick-load buttons in the touch overlay. Quick-load replaces current progress with the quick save; use it carefully.",
      "Показывает сенсорные кнопки быстрого сохранения и загрузки. Быстрая загрузка заменяет текущий прогресс сохранённым — используйте осторожно.", 0),
    H("preferred_controller", "Active gamepad", "Активный геймпад",
      "Automatic selects a connected controller. Choose a specific device if a TV remote or another gamepad takes the input slot. Disconnected saved devices remain selectable.",
      "Авто выбирает подключённый контроллер. Укажите конкретный, если пульт или другой геймпад перехватывает управление. Сохранённый выбор остаётся после отключения.", 0),
    H("achievements_account", "Achievements account", "Аккаунт достижений",
      "Opens the RetroAchievements sign-in dialog. If already signed in, tapping signs out. Requires an internet connection; account credentials are not part of the game disc.",
      "Открывает вход в RetroAchievements. Если вход уже выполнен, нажатие выходит из аккаунта. Нужно подключение к интернету.", 0),
    H("render_scale", "Render scale", "Масштаб рендера",
      "Internal 3D rendering resolution. Lower values reduce GPU load but make the scene less sharp. The menu stays at screen resolution. Applies immediately.",
      "Внутреннее разрешение 3D-сцены. Уменьшение снижает нагрузку, но делает сцену менее чёткой. Меню остаётся в разрешении экрана. Применяется сразу.", 0),
    H("resident_textures", "Resident textures", "Текстуры в памяти",
      "Keeps an expanded texture pool in memory. Turn off to reduce memory use when not using replacement textures. Requires restarting the game.",
      "Держит расширенный набор текстур в памяти. Без пакетов замены можно выключить для экономии памяти. Требуется перезапуск игры.", 0),
    H("return_to_title", "Return to title", "В главное меню",
      "Tap twice to return to the title. Unsaved progress is lost. This does not delete saved games or reset settings.",
      "Нажмите дважды для возврата в главное меню. Несохранённый прогресс будет потерян. Сохранения и настройки не удаляются.", 0),
    H("reset_settings", "Reset settings", "Сброс настроек",
      "Restores the shipped settings after confirmation. This is not a reset of your saved game progress.",
      "После подтверждения восстанавливает исходные настройки приложения. Игровые сохранения не сбрасываются.", 0),
};
#undef H

const PcOptionHelp* Pc_OptionsHelp(const char* key)
{
    unsigned i;
    if (!key) return 0;
    for (i = 0; i < sizeof(s_help) / sizeof(s_help[0]); i++)
        if (strcmp(key, s_help[i].key) == 0) return &s_help[i];
    return 0;
}

int Pc_OptionsPreviewScene(const char* key)
{
    static const char* const school[]={"texture_filter","use_pgxp","post_process",
        "tonemap","flashlight_mode","flashlight_intensity","flashlight_size",
        "disable_culling","fps_fov","tps_fov","ots_fov","tps_camera_collision"};
    unsigned i;
    if (!key) return PC_SCENE_NONE;
    if (!strcmp(key,"fog_strength")) return PC_SCENE_STREET;
    if (!strcmp(key,"weather_sim_hz")) return PC_SCENE_WEATHER;
    for(i=0;i<sizeof(school)/sizeof(school[0]);i++)
        if(!strcmp(key,school[i])) return PC_SCENE_SCHOOL;
    return PC_SCENE_NONE;
}

PcOptionsLayout Pc_OptionsLayout(float width, float height)
{
    PcOptionsLayout l;
    float scale = height / 900.0f;
    if (scale < 0.45f) scale = 0.45f;
    if (scale > 2.0f) scale = 2.0f;
    l.left = width * 0.035f;
    l.top = height * 0.055f;
    l.width = width * 0.93f;
    l.height = height * 0.89f;
    l.padding = 18.0f * scale;
    l.gap = 24.0f * scale;
    l.font = 28.0f * scale;
    l.stacked = width < height * 1.2f;
    l.listWidth = l.stacked ? l.width : l.width * 0.53f;
    return l;
}
