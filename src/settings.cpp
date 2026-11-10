// openMouse — settings window
// SPDX-License-Identifier: MIT
//
// Runs as its own process (openmouse.exe --settings), started from the tray
// menu. Deliberately not a window of the engine: the engine's thread is the
// input path of every mouse on the machine, and a settings window has no
// business sharing it. A crash or a hang here costs a window, never the
// pointers.
//
// Every change is applied at once: the seat values are merged into
// openmouse.ini and the engine is told to re-read them (kReloadSettingsMsg).
// The file is re-read before each write, so device bindings the engine may
// have saved in the meantime are kept. "Undo" puts back the values the window
// opened with.
//
// Two tabs: "General" (start with Windows, language, help, credits) and "Mice"
// (one section per mouse). The sections of the mice sit in a panel of their own
// that scrolls: with a button section on top of the speed ones they no longer
// fit a small screen. The panel and the general page only host the controls and
// forward their notifications. The window takes the height of the tab it shows.
//
// Text: every string lives in the tables below, one per language (lang.cpp says
// which one is in use). The layout measures its text, so a longer language
// needs no change here.
#include "openmouse.h"
#include <commctrl.h>
#include <shellapi.h>   // ShellExecuteEx — not pulled in by WIN32_LEAN_AND_MEAN
#include <cstdio>
#include <cmath>

#ifdef _MSC_VER
// Version 6 of the common controls, for themed trackbars and buttons. Without
// it the controls fall back to their Windows 95 look.
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

namespace om {

namespace {

constexpr wchar_t kSettingsClass[] = L"openMouse.Settings";
constexpr wchar_t kPanelClass[]    = L"openMouse.Settings.Panel";
constexpr wchar_t kPageClass[]     = L"openMouse.Settings.Page";
constexpr wchar_t kPreviewClass[]  = L"openMouse.Settings.Preview";
constexpr wchar_t kEngineClass[]   = L"openMouse.Engine";

// ---------------------------------------------------------------- strings

enum Str {
    sTitle, sSeat, sSensitivity, sAccel, sSlow, sFast, sSpeed, sScreen, sScreenNone,
    sScreenItem, sPrimary, sUnplugged, sDefaults, sUndo, sClose, sHint, sApplied, sNotRunning, sSaveFailed,
    sLearn, sForget, sListening, sNothing, sAbsent, sEngineOff, sLearned, sAlready, sFull,
    sNoButtons, sNative, sActNone, sActLeft, sActRight, sActMiddle, sActBack, sActForward, sActKeys,
    sKeysNone, sKeysCapture, sWin, sCtrlName, sAltName, sShiftName, sWinName,
    sGeneral, sAutostart, sAutostartOn, sAutostartOff, sAutostartWait, sAutostartFail,
    sAutostartDenied, sAutostartUnsafe, sAutostartTaskUnsafe,
    sLook, sColorLabel, sStyleLabel, sColorCustom,
    sStyle0, sStyle1, sStyle2, sStyle3, sStyle4,
    sCol0, sCol1, sCol2, sCol3, sCol4, sCol5, sCol6, sCol7, sCol8, sCol9,
    sFixed, sExplainFixed, sExplainAccel, sSeatAbsent, sAbout,
    sTouchpad, sPadCut, sPadMode, sPadAuto, sPadOn, sPadOff,
    sDisable, sBrandOne, sBrandMany, sLastActive,
    sTabMice, sLanguage, sLangAuto, sHelp, sHelpFail, sTipModes, sTipLearn, sStrCount
};

const wchar_t* const kStrEn[sStrCount] = {
    L"openMouse settings",
    L"Mouse %d",
    L"Sensitivity",
    L"Progressive acceleration",
    L"Slow motion (precision)",
    L"Fast motion (crossing the screen)",
    L"Acceleration threshold",
    L"Keep inside screen",
    L"No, the whole desktop",
    L"Screen %d: %d × %d",
    L" (main)",
    L"%s (unplugged)",
    L"Default values",
    L"Undo my changes",
    L"Close",
    L"Changes apply at once.",
    L"Applied and saved.",
    L"Saved. openMouse is not running: it will be used at next start.",
    L"Could not save %s",
    L"Learn a button",
    L"Forget",
    L"Press the button to learn (8 s)…",
    L"Nothing received: this button sends nothing to Windows (only the manufacturer's software "
    L"can read it), or it is not on this mouse.",
    L"This mouse is not plugged in.",
    L"openMouse is not running: there is nothing to listen to.",
    L"Button learned. Choose what it does.",
    L"This button is already learned.",
    L"This mouse already has %d learned buttons: forget one first.",
    L"No learned button. Learn one to give it an action.",
    L"Windows also keeps its own action for this button.",
    L"Do nothing",
    L"Left click",
    L"Right click",
    L"Middle click",
    L"Back button",
    L"Forward button",
    L"Keyboard shortcut",
    L"Choose the shortcut…",
    L"Press the shortcut…",
    L"Win +",
    L"Ctrl", L"Alt", L"Shift", L"Win",
    L"General",
    L"Start openMouse with Windows",
    L"openMouse will start by itself when you log on to Windows.",
    L"openMouse will no longer start with Windows.",
    L"Waiting for Windows to confirm…",
    L"Windows did not take the change. Run openMouse as administrator and try again.",
    L"Cancelled: administrator permission was refused, nothing changed.",
    L"Starting with Windows is only available once openMouse has been installed by its installer, in a protected folder.",
    L"The start-with-Windows task points to a folder that ordinary programs can change. Untick the box to remove it.",
    L"Pointer look",
    L"Colour",
    L"Pointer set",
    L"Custom (%06X)",
    L"Classic (coloured body, black edge)",
    L"Bright rim (seen on any background)",
    L"White body, coloured edge",
    L"Large, bright rim",
    L"Soft glow",
    L"Yellow", L"Orange", L"Red", L"Pink", L"Purple", L"Blue", L"Cyan", L"Green", L"Lime", L"White",
    L"Fixed sensitivity",
    L"Every movement is multiplied by the same value.",
    L"Slow for small gestures, fast for wide ones. The threshold sets where it speeds up.",
    L" (unplugged)",
    L"About: openMouse, created by Alston Mendonca, improved and developed by Calepitool.",
    L"Touchpad",
    L" (cut off)",
    L"Touchpad mode",
    L"Automatic (cut off from two mice on)",
    L"Always on",
    L"Cut off (kept when no other mouse)",
    L"Disable",
    L"%s mouse",
    L"%s mouse (%d)",
    L"Cannot disable the last working mouse: one pointer has to stay.",
    L"Mice",
    L"Language",
    L"Automatic (Windows language)",
    L"Help",
    L"Could not open the help page.",
    L"A mouse takes one of the two modes: the other one's sliders are greyed out.",
    L"A learned button acts at the position of the pointer of its own mouse.",
};

const wchar_t* const kStrFr[sStrCount] = {
    L"Réglages d'openMouse",
    L"Souris %d",
    L"Sensibilité",
    L"Accélération progressive",
    L"Gestes lents (précision)",
    L"Gestes rapides (traverser l'écran)",
    L"Seuil d'accélération",
    L"Rester dans l'écran",
    L"Non, tout le bureau",
    L"Écran %d : %d × %d",
    L" (principal)",
    L"%s (débranché)",
    L"Valeurs par défaut",
    L"Annuler mes changements",
    L"Fermer",
    L"Les changements s'appliquent tout de suite.",
    L"Appliqué et enregistré.",
    L"Enregistré. openMouse n'est pas lancé : ce réglage servira au prochain démarrage.",
    L"Impossible d'enregistrer %s",
    L"Apprendre un bouton",
    L"Oublier",
    L"Appuie sur le bouton à apprendre (8 s)…",
    L"Rien reçu : ce bouton n'envoie rien à Windows (seul le logiciel du fabricant sait le lire), "
    L"ou il n'est pas sur cette souris.",
    L"Cette souris n'est pas branchée.",
    L"openMouse n'est pas lancé : il n'y a rien à écouter.",
    L"Bouton appris. Choisis ce qu'il fait.",
    L"Ce bouton est déjà appris.",
    L"Cette souris a déjà %d boutons appris : oublie-en un d'abord.",
    L"Aucun bouton appris. Apprends-en un pour lui donner une action.",
    L"Windows garde aussi sa propre action pour ce bouton.",
    L"Ne rien faire",
    L"Clic gauche",
    L"Clic droit",
    L"Clic milieu",
    L"Bouton précédent",
    L"Bouton suivant",
    L"Raccourci clavier",
    L"Choisir le raccourci…",
    L"Appuie sur le raccourci…",
    L"Win +",
    L"Ctrl", L"Alt", L"Maj", L"Win",
    L"Général",
    L"Lancer openMouse au démarrage de Windows",
    L"openMouse se lancera tout seul à l'ouverture de votre session Windows.",
    L"openMouse ne se lancera plus avec Windows.",
    L"En attente de la confirmation de Windows…",
    L"Windows n'a pas pris le changement. Lancez openMouse en administrateur et recommencez.",
    L"Annulé : l'autorisation administrateur a été refusée, rien n'a changé.",
    L"Le démarrage automatique n'est disponible qu'une fois openMouse installé par son programme d'installation, dans un dossier protégé.",
    L"La tâche de démarrage automatique pointe vers un dossier que des programmes ordinaires peuvent modifier. Décochez la case pour la retirer.",
    L"Apparence de la flèche",
    L"Couleur",
    L"Jeu de curseurs",
    L"Personnalisée (%06X)",
    L"Classique (corps coloré, bord noir)",
    L"Liseré brillant (visible sur tous les fonds)",
    L"Corps blanc, bord coloré",
    L"Grand, liseré brillant",
    L"Halo lumineux",
    L"Jaune", L"Orange", L"Rouge", L"Rose", L"Violet", L"Bleu", L"Cyan", L"Vert", L"Citron vert", L"Blanc",
    L"Sensibilité fixe",
    L"Chaque mouvement est multiplié par la même valeur.",
    L"Lent pour les petits gestes, rapide pour les grands. Le seuil règle où ça accélère.",
    L" (débranchée)",
    L"À propos : openMouse, créé par Alston Mendonca, amélioré et mis au point par Calepitool.",
    L"Pavé tactile",
    L" (coupé)",
    L"Mode du pavé tactile",
    L"Automatique (coupé dès deux souris)",
    L"Toujours actif",
    L"Coupé (gardé s'il n'y a pas d'autre souris)",
    L"Désactiver",
    L"Souris %s",
    L"Souris %s (%d)",
    L"Impossible de désactiver la dernière souris active : il faut garder un pointeur.",
    L"Souris",
    L"Langue",
    L"Automatique (langue de Windows)",
    L"Aide",
    L"Impossible d'ouvrir la page d'aide.",
    L"Une souris suit l'un des deux modes : les curseurs de l'autre sont grisés.",
    L"Un bouton appris agit à la position de la flèche de sa propre souris.",
};

const wchar_t* const kStrDe[sStrCount] = {
    L"openMouse-Einstellungen",
    L"Maus %d",
    L"Empfindlichkeit",
    L"Progressive Beschleunigung",
    L"Langsame Bewegungen (Präzision)",
    L"Schnelle Bewegungen (Distanz)",
    L"Beschleunigungsschwelle",
    L"Im Bildschirm bleiben",
    L"Nein, gesamter Desktop",
    L"Bildschirm %d: %d × %d",
    L" (Hauptbildschirm)",
    L"%s (getrennt)",
    L"Standardwerte",
    L"Meine Änderungen rückgängig",
    L"Schließen",
    L"Änderungen werden sofort übernommen.",
    L"Übernommen und gespeichert.",
    L"Gespeichert. openMouse läuft nicht: Die Einstellung gilt ab dem nächsten Start.",
    L"%s konnte nicht gespeichert werden",
    L"Taste anlernen",
    L"Vergessen",
    L"Drücken Sie die Taste, die angelernt werden soll (8 s)…",
    L"Nichts empfangen: Diese Taste sendet nichts an Windows (nur die Software des Herstellers "
    L"kann sie lesen), oder sie gehört nicht zu dieser Maus.",
    L"Diese Maus ist nicht angeschlossen.",
    L"openMouse läuft nicht, daher kann keine Taste erkannt werden.",
    L"Taste angelernt. Wählen Sie, was sie bewirkt.",
    L"Diese Taste ist bereits angelernt.",
    L"Diese Maus hat bereits %d angelernte Tasten: Vergessen Sie zuerst eine.",
    L"Keine Taste angelernt. Lernen Sie eine an, um ihr eine Aktion zuzuweisen.",
    L"Windows behält für diese Taste außerdem seine eigene Aktion.",
    L"Nichts tun",
    L"Linksklick",
    L"Rechtsklick",
    L"Mittelklick",
    L"Zurück-Taste",
    L"Vorwärts-Taste",
    L"Tastenkombination",
    L"Tastenkombination wählen…",
    L"Tastenkombination drücken…",
    L"Win +",
    L"Strg", L"Alt", L"Umschalt", L"Win",
    L"Allgemein",
    L"openMouse mit Windows starten",
    L"openMouse startet automatisch, wenn Sie sich bei Windows anmelden.",
    L"openMouse startet nicht mehr mit Windows.",
    L"Warten auf die Bestätigung von Windows…",
    L"Windows hat die Änderung nicht übernommen. Starten Sie openMouse als Administrator und versuchen Sie es erneut.",
    L"Abgebrochen: Die Administratorberechtigung wurde verweigert, nichts wurde geändert.",
    L"Der automatische Start ist erst verfügbar, wenn openMouse mit seinem Installationsprogramm in einem geschützten Ordner installiert wurde.",
    L"Die Aufgabe für den automatischen Start verweist auf einen Ordner, den gewöhnliche Programme ändern können. Deaktivieren Sie das Kontrollkästchen, um sie zu entfernen.",
    L"Aussehen des Zeigers",
    L"Farbe",
    L"Zeigersatz",
    L"Benutzerdefiniert (%06X)",
    L"Klassisch (farbiger Körper, schwarzer Rand)",
    L"Heller Rand (überall sichtbar)",
    L"Weißer Körper, farbiger Rand",
    L"Groß, heller Rand",
    L"Weicher Schein",
    L"Gelb", L"Orange", L"Rot", L"Rosa", L"Violett", L"Blau", L"Cyan", L"Grün", L"Hellgrün", L"Weiß",
    L"Feste Empfindlichkeit",
    L"Jede Bewegung wird mit demselben Wert multipliziert.",
    L"Langsam bei kleinen Bewegungen, schnell bei großen. Die Schwelle legt fest, ab wann beschleunigt wird.",
    L" (getrennt)",
    L"Info: openMouse, erstellt von Alston Mendonca, weiterentwickelt und verbessert von Calepitool.",
    L"Touchpad",
    L" (aus)",
    L"Touchpad-Modus",
    L"Automatisch (aus ab zwei Mäusen)",
    L"Immer aktiv",
    L"Aus (außer ohne andere Maus)",
    L"Deaktivieren",
    L"%s-Maus",
    L"%s-Maus (%d)",
    L"Die letzte funktionierende Maus kann nicht deaktiviert werden: Ein Zeiger muss bleiben.",
    L"Mäuse",
    L"Sprache",
    L"Automatisch (Windows-Sprache)",
    L"Hilfe",
    L"Die Hilfeseite konnte nicht geöffnet werden.",
    L"Eine Maus folgt einem von zwei Modi: Die Regler des anderen sind abgegraut.",
    L"Eine angelernte Taste wirkt an der Position des Zeigers ihrer eigenen Maus.",
};

const wchar_t* const kStrEs[sStrCount] = {
    L"Configuración de openMouse",
    L"Ratón %d",
    L"Sensibilidad",
    L"Aceleración progresiva",
    L"Movimiento lento (precisión)",
    L"Movimiento rápido (distancia)",
    L"Umbral de aceleración",
    L"Mantener dentro de la pantalla",
    L"No, todo el escritorio",
    L"Pantalla %d: %d × %d",
    L" (principal)",
    L"%s (desconectado)",
    L"Valores predeterminados",
    L"Deshacer mis cambios",
    L"Cerrar",
    L"Los cambios se aplican al instante.",
    L"Aplicado y guardado.",
    L"Guardado. openMouse no se está ejecutando: se usará en el próximo inicio.",
    L"No se pudo guardar %s",
    L"Aprender un botón",
    L"Olvidar",
    L"Pulse el botón que desea aprender (8 s)…",
    L"No se recibió nada: este botón no envía nada a Windows (solo el software del fabricante "
    L"puede leerlo) o no pertenece a este ratón.",
    L"Este ratón no está conectado.",
    L"openMouse no se está ejecutando: no hay nada que escuchar.",
    L"Botón aprendido. Elija qué hace.",
    L"Este botón ya está aprendido.",
    L"Este ratón ya tiene %d botones aprendidos: olvide uno primero.",
    L"Ningún botón aprendido. Aprenda uno para asignarle una acción.",
    L"Windows también conserva su propia acción para este botón.",
    L"No hacer nada",
    L"Clic izquierdo",
    L"Clic derecho",
    L"Clic central",
    L"Botón Atrás",
    L"Botón Adelante",
    L"Combinación de teclas",
    L"Elegir la combinación…",
    L"Pulse la combinación…",
    L"Win +",
    L"Ctrl", L"Alt", L"Mayús", L"Win",
    L"General",
    L"Iniciar openMouse con Windows",
    L"openMouse se iniciará solo al iniciar sesión en Windows.",
    L"openMouse ya no se iniciará con Windows.",
    L"Esperando la confirmación de Windows…",
    L"Windows no aplicó el cambio. Ejecute openMouse como administrador e inténtelo de nuevo.",
    L"Cancelado: se rechazó el permiso de administrador, no se cambió nada.",
    L"El inicio con Windows solo está disponible cuando openMouse se ha instalado con su instalador, en una carpeta protegida.",
    L"La tarea de inicio con Windows apunta a una carpeta que los programas normales pueden modificar. Desmarque la casilla para quitarla.",
    L"Aspecto del puntero",
    L"Color",
    L"Estilo del puntero",
    L"Personalizado (%06X)",
    L"Clásico (cuerpo de color, borde negro)",
    L"Borde claro (visible sobre cualquier fondo)",
    L"Cuerpo blanco, borde de color",
    L"Grande, borde claro",
    L"Resplandor suave",
    L"Amarillo", L"Naranja", L"Rojo", L"Rosa", L"Morado", L"Azul", L"Cian", L"Verde", L"Lima", L"Blanco",
    L"Sensibilidad fija",
    L"Cada movimiento se multiplica por el mismo valor.",
    L"Lento en gestos pequeños, rápido en los amplios. El umbral fija cuándo acelera.",
    L" (desconectado)",
    L"Acerca de: openMouse, creado por Alston Mendonca, mejorado y desarrollado por Calepitool.",
    L"Panel táctil",
    L" (desactivado)",
    L"Modo del panel táctil",
    L"Automático (desactivado con dos ratones)",
    L"Siempre activo",
    L"Desactivado (se mantiene sin otro ratón)",
    L"Desactivar",
    L"Ratón %s",
    L"Ratón %s (%d)",
    L"No se puede desactivar el último ratón que funciona: debe quedar un puntero.",
    L"Ratones",
    L"Idioma",
    L"Automático (idioma de Windows)",
    L"Ayuda",
    L"No se pudo abrir la página de ayuda.",
    L"Un ratón sigue uno de los dos modos: los controles del otro aparecen atenuados.",
    L"Un botón aprendido actúa en la posición del puntero de su propio ratón.",
};

const wchar_t* const kStrRu[sStrCount] = {
    L"Параметры openMouse",
    L"Мышь %d",
    L"Чувствительность",
    L"Прогрессивное ускорение",
    L"Медленное движение (точность)",
    L"Быстрое движение (по экрану)",
    L"Порог ускорения",
    L"Не выпускать за пределы экрана",
    L"Нет, весь рабочий стол",
    L"Экран %d: %d × %d",
    L" (основной)",
    L"%s (отключена)",
    L"Значения по умолчанию",
    L"Отменить мои изменения",
    L"Закрыть",
    L"Изменения применяются сразу.",
    L"Применено и сохранено.",
    L"Сохранено. openMouse не запущен: параметры будут использованы при следующем запуске.",
    L"Не удалось сохранить %s",
    L"Запомнить кнопку",
    L"Забыть",
    L"Нажмите кнопку, которую нужно запомнить (8 с)…",
    L"Ничего не получено: эта кнопка ничего не передаёт в Windows (её может прочитать только "
    L"программа производителя) или она не принадлежит этой мыши.",
    L"Эта мышь не подключена.",
    L"openMouse не запущен: слушать нечего.",
    L"Кнопка запомнена. Выберите её действие.",
    L"Эта кнопка уже запомнена.",
    L"У этой мыши уже запомнено кнопок: %d. Сначала забудьте одну из них.",
    L"Нет запомненных кнопок. Запомните кнопку, чтобы назначить ей действие.",
    L"Windows также сохраняет собственное действие для этой кнопки.",
    L"Ничего не делать",
    L"Щелчок левой кнопкой",
    L"Щелчок правой кнопкой",
    L"Щелчок средней кнопкой",
    L"Кнопка «Назад»",
    L"Кнопка «Вперёд»",
    L"Сочетание клавиш",
    L"Выбрать сочетание…",
    L"Нажмите сочетание…",
    L"Win +",
    L"Ctrl", L"Alt", L"Shift", L"Win",
    L"Общие",
    L"Запускать openMouse вместе с Windows",
    L"openMouse будет запускаться сам при входе в Windows.",
    L"openMouse больше не будет запускаться вместе с Windows.",
    L"Ожидание подтверждения Windows…",
    L"Windows не приняла изменение. Запустите openMouse от имени администратора и повторите попытку.",
    L"Отменено: в разрешении администратора отказано, ничего не изменилось.",
    L"Запуск вместе с Windows доступен только после установки openMouse с помощью установщика, в защищённую папку.",
    L"Задача запуска вместе с Windows указывает на папку, которую могут изменять обычные программы. Снимите флажок, чтобы удалить её.",
    L"Вид указателя",
    L"Цвет",
    L"Стиль указателя",
    L"Другой (%06X)",
    L"Классический (цветная заливка, чёрный контур)",
    L"Светлый контур (виден на любом фоне)",
    L"Белая заливка, цветной контур",
    L"Крупный, светлый контур",
    L"Мягкое свечение",
    L"Жёлтый", L"Оранжевый", L"Красный", L"Розовый", L"Фиолетовый", L"Синий", L"Голубой", L"Зелёный", L"Салатовый", L"Белый",
    L"Фиксированная чувствительность",
    L"Каждое движение умножается на одно и то же значение.",
    L"Медленно при малых движениях, быстро при широких. Порог определяет, когда начинается ускорение.",
    L" (отключена)",
    L"О программе: openMouse, автор Alston Mendonca, доработана и развивается Calepitool.",
    L"Сенсорная панель",
    L" (выкл.)",
    L"Режим сенсорной панели",
    L"Авто (выкл. при двух мышах)",
    L"Всегда включена",
    L"Выкл. (работает, если мышь одна)",
    L"Отключить",
    L"Мышь %s",
    L"Мышь %s (%d)",
    L"Нельзя отключить последнюю рабочую мышь: должен остаться один указатель.",
    L"Мыши",
    L"Язык",
    L"Автоматически (язык Windows)",
    L"Справка",
    L"Не удалось открыть страницу справки.",
    L"Мышь работает в одном из двух режимов: ползунки другого режима неактивны.",
    L"Запомненная кнопка действует там, где находится указатель её собственной мыши.",
};

const wchar_t* const kStrZh[sStrCount] = {
    L"openMouse 设置",
    L"鼠标 %d",
    L"灵敏度",
    L"渐进加速",
    L"慢速移动（精确）",
    L"快速移动（跨越屏幕）",
    L"加速阈值",
    L"限制在屏幕内",
    L"否，整个桌面",
    L"屏幕 %d：%d × %d",
    L"（主屏幕）",
    L"%s（已拔出）",
    L"默认值",
    L"撤销我的更改",
    L"关闭",
    L"更改立即生效。",
    L"已应用并保存。",
    L"已保存。openMouse 未运行：将在下次启动时使用。",
    L"无法保存 %s",
    L"学习按键",
    L"忘记",
    L"请按下要学习的按键（8 秒）…",
    L"未收到任何信号：此按键不会向 Windows 发送任何信息（只有厂商软件才能读取），"
    L"或者它不属于这个鼠标。",
    L"此鼠标未连接。",
    L"openMouse 未运行：无法监听。",
    L"按键已学习。请选择它的功能。",
    L"此按键已学习过。",
    L"此鼠标已学习了 %d 个按键：请先忘记其中一个。",
    L"尚无已学习的按键。学习一个按键，即可为它指定功能。",
    L"Windows 也为此按键保留了自己的功能。",
    L"不执行任何操作",
    L"左键单击",
    L"右键单击",
    L"中键单击",
    L"后退键",
    L"前进键",
    L"键盘快捷键",
    L"选择快捷键…",
    L"请按下快捷键…",
    L"Win +",
    L"Ctrl", L"Alt", L"Shift", L"Win",
    L"常规",
    L"随 Windows 启动 openMouse",
    L"登录 Windows 时，openMouse 将自动启动。",
    L"openMouse 将不再随 Windows 启动。",
    L"正在等待 Windows 确认…",
    L"Windows 未接受此更改。请以管理员身份运行 openMouse 后重试。",
    L"已取消：管理员权限被拒绝，未作任何更改。",
    L"只有通过安装程序将 openMouse 安装到受保护的文件夹后，才能随 Windows 启动。",
    L"随 Windows 启动的任务指向一个普通程序可以修改的文件夹。取消勾选此框即可将其删除。",
    L"指针外观",
    L"颜色",
    L"指针样式",
    L"自定义 (%06X)",
    L"经典（彩色填充，黑色边缘）",
    L"亮色边缘（任何背景下都清晰可见）",
    L"白色填充，彩色边缘",
    L"大号，亮色边缘",
    L"柔和光晕",
    L"黄色", L"橙色", L"红色", L"粉色", L"紫色", L"蓝色", L"青色", L"绿色", L"黄绿色", L"白色",
    L"固定灵敏度",
    L"每次移动都乘以相同的数值。",
    L"小幅动作慢，大幅动作快。阈值决定从何处开始加速。",
    L"（已拔出）",
    L"关于：openMouse 由 Alston Mendonca 创建，由 Calepitool 改进并持续开发。",
    L"触摸板",
    L"（已关闭）",
    L"触摸板模式",
    L"自动（两个鼠标时关闭）",
    L"始终开启",
    L"关闭（无其他鼠标时保留）",
    L"禁用",
    L"%s 鼠标",
    L"%s 鼠标 (%d)",
    L"无法禁用最后一个可用的鼠标：必须保留一个指针。",
    L"鼠标",
    L"语言",
    L"自动（Windows 语言）",
    L"帮助",
    L"无法打开帮助页面。",
    L"每个鼠标使用两种模式之一：另一种模式的滑块呈灰色。",
    L"已学习的按键在其所属鼠标的指针位置生效。",
};

const wchar_t* const kStrAr[sStrCount] = {
    L"إعدادات openMouse",
    L"الماوس %d",
    L"الحساسية",
    L"التسارع التدريجي",
    L"الحركة البطيئة (للدقة)",
    L"الحركة السريعة (لعبور الشاشة)",
    L"عتبة التسارع",
    L"الحصر داخل الشاشة",
    L"لا، سطح المكتب بأكمله",
    L"الشاشة %d: %d × %d",
    L" (الرئيسية)",
    L"%s (غير متصل)",
    L"القيم الافتراضية",
    L"التراجع عن تغييراتي",
    L"إغلاق",
    L"تُطبَّق التغييرات فورًا.",
    L"تم التطبيق والحفظ.",
    L"تم الحفظ. openMouse لا يعمل: ستُستخدم الإعدادات عند التشغيل التالي.",
    L"تعذّر حفظ %s",
    L"تعلّم زر",
    L"نسيان",
    L"اضغط على الزر المراد تعلّمه (8 ثوانٍ)…",
    L"لم تصل أي إشارة: هذا الزر لا يرسل شيئًا إلى Windows (لا يقرؤه إلا برنامج الشركة المصنّعة)، "
    L"أو أنه ليس في هذا الماوس.",
    L"هذا الماوس غير متصل.",
    L"openMouse لا يعمل: لا يوجد ما يمكن الاستماع إليه.",
    L"تم تعلّم الزر. اختر ما يفعله.",
    L"تم تعلّم هذا الزر من قبل.",
    L"لدى هذا الماوس %d أزرار متعلَّمة بالفعل: انسَ أحدها أولًا.",
    L"لا يوجد زر متعلَّم. تعلّم زرًّا لتعيين إجراء له.",
    L"يحتفظ Windows أيضًا بإجراء خاص به لهذا الزر.",
    L"عدم فعل أي شيء",
    L"نقرة بالزر الأيسر",
    L"نقرة بالزر الأيمن",
    L"نقرة بالزر الأوسط",
    L"زر الرجوع",
    L"زر التقدّم",
    L"اختصار لوحة المفاتيح",
    L"اختر الاختصار…",
    L"اضغط على الاختصار…",
    L"Win +‎",
    L"Ctrl", L"Alt", L"Shift", L"Win",
    L"عام",
    L"تشغيل openMouse مع Windows",
    L"سيبدأ openMouse تلقائيًا عند تسجيل دخولك إلى Windows.",
    L"لن يبدأ openMouse مع Windows بعد الآن.",
    L"في انتظار تأكيد Windows…",
    L"لم يقبل Windows هذا التغيير. شغّل openMouse بصلاحيات المسؤول ثم أعد المحاولة.",
    L"تم الإلغاء: رُفض إذن المسؤول ولم يتغيّر شيء.",
    L"لا يتوفر التشغيل مع Windows إلا بعد تثبيت openMouse بواسطة برنامج التثبيت في مجلد محمي.",
    L"تشير مهمة التشغيل مع Windows إلى مجلد يمكن للبرامج العادية تعديله. ألغِ تحديد المربع لإزالتها.",
    L"مظهر المؤشر",
    L"اللون",
    L"نمط المؤشر",
    L"مخصّص (%06X)",
    L"كلاسيكي (جسم ملوّن وحافة سوداء)",
    L"حافة ساطعة (تظهر على أي خلفية)",
    L"جسم أبيض وحافة ملوّنة",
    L"كبير بحافة ساطعة",
    L"توهّج ناعم",
    L"أصفر", L"برتقالي", L"أحمر", L"وردي", L"أرجواني", L"أزرق", L"سماوي", L"أخضر", L"أخضر ليموني", L"أبيض",
    L"حساسية ثابتة",
    L"تُضرب كل حركة في القيمة نفسها.",
    L"بطيء مع الحركات الصغيرة وسريع مع الحركات الواسعة. تحدّد العتبة النقطة التي يبدأ عندها التسارع.",
    L" (غير متصل)",
    L"حول البرنامج: ابتكر Alston Mendonca برنامج openMouse، وتولّت Calepitool تحسينه ومواصلة تطويره.",
    L"لوحة اللمس",
    L" (مقطوعة)",
    L"وضع لوحة اللمس",
    L"تلقائي (تُقطع عند وجود ماوسين)",
    L"مفعّلة دائمًا",
    L"مقطوعة (تبقى عند عدم وجود ماوس آخر)",
    L"تعطيل",
    L"ماوس %s",
    L"ماوس %s (%d)",
    L"لا يمكن تعطيل آخر ماوس يعمل: يجب أن يبقى مؤشر واحد على الأقل.",
    L"الماوسات",
    L"اللغة",
    L"تلقائي (لغة Windows)",
    L"مساعدة",
    L"تعذّر فتح صفحة المساعدة.",
    L"يعمل كل ماوس بأحد الوضعين: تظهر منزلقات الوضع الآخر بلون رمادي.",
    L"يعمل الزر المتعلَّم عند موضع مؤشر الماوس الخاص به.",
};

// In the order of the Lang enum. An entry left empty falls back to English.
const wchar_t* const* const kStr[kLangCount] = { kStrEn, kStrFr, kStrDe, kStrEs, kStrRu, kStrZh, kStrAr };

// The colours offered for a mouse; the first one is the default of seat 0.
const COLORREF kColors[10] = {
    RGB(0xFF, 0xDD, 0x00), RGB(0xE8, 0x6A, 0x33), RGB(0xE0, 0x3C, 0x3C), RGB(0xF0, 0x6E, 0xAA),
    RGB(0xB0, 0x60, 0xC0), RGB(0x4A, 0x90, 0xD9), RGB(0x20, 0xC0, 0xD0), RGB(0x5C, 0xB8, 0x5C),
    RGB(0xB8, 0xD8, 0x2A), RGB(0xFF, 0xFF, 0xFF),
};

const wchar_t* S(Str id) {
    const wchar_t* s = kStr[(int)UiLang()][id];
    return (s && *s) ? s : kStrEn[id];
}

// ---------------------------------------------------------------- sliders

// A trackbar holds integers; each value is stored as value * scale.
struct Range { double lo, hi, scale; const wchar_t* fmt; };

enum Field { fSens, fSlow, fFast, fSpeed, fFieldCount };

const Range kRange[fFieldCount] = {
    { 0.10, 10.0, 20, L"×%.2f" },   // steps of 0.05
    { 0.10,  5.0, 20, L"×%.2f" },
    { 1.0,  20.0, 10, L"×%.1f" },   // steps of 0.1
    { 0.2,  10.0, 10, L"%.1f"  },
};

// Control ids: one block of 32 per seat.
constexpr int kPerSeat   = 40;
constexpr int kIdBase    = 1000;
constexpr int kIdUndo    = 900;
constexpr int kIdClose   = 901;
constexpr int kIdAutostart = 902;
constexpr int kIdSite    = 903;
constexpr int kIdLang    = 904;
constexpr int kIdHelp    = 905;
constexpr int kIdTab     = 906;
enum Tab { tabGeneral = 0, tabMice = 1 };
constexpr wchar_t kSiteUrl[] = L"https://calepitool.fr/";
enum SeatCtl { cLabel = 0, cValue = 4, cSlider = 8, cAccel = 12, cScreenLabel, cScreen, cDefaults,
               cLearn = 16, cNote, cList, cAction, cKeys, cWin, cForget,
               cColorLabel = 23, cColor, cStyleLabel, cStyle, cPreview, cFixed, cExplain,
               cPadLabel, cPad, cOff };
int CtlId(int seat, int ctl) { return kIdBase + seat * kPerSeat + ctl; }

constexpr UINT_PTR kTimerApply = 1, kTimerLearn = 2, kTimerAutostart = 3;
constexpr UINT_PTR kSubclassKeys = 1;

// ---------------------------------------------------------------- state

struct SeatUi {
    HWND label[fFieldCount]{}, value[fFieldCount]{}, slider[fFieldCount]{};
    HWND fixed = nullptr, explain = nullptr;   // the two speed modes, and a phrase on the one chosen
    HWND accel = nullptr, screenLabel = nullptr, screen = nullptr, defaults = nullptr;
    HWND learn = nullptr, note = nullptr, list = nullptr, action = nullptr, keys = nullptr,
         win = nullptr, forget = nullptr;
    HWND colorLabel = nullptr, color = nullptr, styleLabel = nullptr, style = nullptr, preview = nullptr;
    HWND padLabel = nullptr, pad = nullptr;   // the touchpad mode, shown in its section only
    HWND off = nullptr;                       // "Disable", beside the title; not for the touchpad
    RECT header{};   // swatch and title, painted by the panel (content coordinates)
};

struct State {
    std::wstring path;
    Config  opened;          // as the window found it, for "undo"
    Config  cur;             // what the window shows
    int     seats = 2;
    SeatUi  ui[kMaxSeats];
    HWND    panel = nullptr;
    HWND    self = nullptr;          // the settings window
    HWND    tab = nullptr;           // the two tabs
    HWND    page = nullptr;          // the "General" page, a sibling of the panel
    HWND    tip = nullptr;           // the tooltips
    HWND    hint = nullptr, status = nullptr, about = nullptr, site = nullptr, undo = nullptr, close = nullptr;
    HWND    langLabel = nullptr, lang = nullptr, help = nullptr;
    HWND    autostart = nullptr;     // "start with Windows", in the general page
    HWND    autoNote = nullptr;      // why it is refused, under the checkbox
    bool    autoSafe = false;        // this copy sits where the task may be created
    bool    autoRoom = false;        // the note has its place in the layout
    int     autoState = -1;          // the task as last read: 1, 0, or -1
    int     curTab = tabGeneral;
    bool    laidOut = false;         // the first layout is done
    RECT    statusRow{};             // where the status sits, in the window; centred on its text
    struct Tip { HWND tool, parent; Str id; };
    std::vector<Tip> tips;
    bool    autoWait = false;        // a change is in flight, polled by kTimerAutostart
    bool    autoWant = false;
    int     autoTries = 0;
    HANDLE  autoHelper = nullptr;    // the elevated one-shot copy, when the engine could not do it
    HFONT   font = nullptr, bold = nullptr;
    UINT    dpi = 96;
    bool    pending = false;
    std::vector<ScreenInfo> screens;   // plugged in when the window opened

    int     scrollY = 0;                         // panel scroll offset, pixels
    int     sel[kMaxSeats] = {};                 // learned button selected in each seat's list
    std::wstring note[kMaxSeats];                // last thing said under a seat's learn button
    int     learning = -1;                       // seat the engine is listening to, or -1
    int     capturing = -1;                      // seat whose shortcut is being typed, or -1
    UINT    present = ~0u;                       // seats whose mouse is plugged in (bit mask), as the engine said
    int     padSeat = -1;                        // the seat of the laptop touchpad, or -1 without one
    int     offW = 0;                            // width of the "Disable" box, from its text
    bool    off[kMaxSeats] = {};                 // mouse set aside ("Disable" ticked)
    bool    offOpened[kMaxSeats] = {};           // the same when the window opened, for "undo"
    std::wstring brand[kMaxSeats];               // make of each seat's mouse, "" when unknown
} g;

int Px(int dip) { return MulDiv(dip, (int)g.dpi, 96); }

UINT DpiOf(HWND hwnd) {
    using Fn = UINT (WINAPI*)(HWND);
    static Fn fn = (Fn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    if (fn && hwnd) return fn(hwnd);
    HDC dc = GetDC(nullptr);
    const UINT dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    return dpi;
}

int ScrollBarWidth() {
    using Fn = int (WINAPI*)(int, UINT);
    static Fn fn = (Fn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetSystemMetricsForDpi");
    return fn ? fn(SM_CXVSCROLL, g.dpi) : GetSystemMetrics(SM_CXVSCROLL);
}

double Get(const Config& c, int seat, Field f) {
    switch (f) {
    case fSens:  return c.sensitivity[seat];
    case fSlow:  return c.accel[seat].slow;
    case fFast:  return c.accel[seat].fast;
    default:     return c.accel[seat].speed;
    }
}

void Put(Config& c, int seat, Field f, double v) {
    switch (f) {
    case fSens:  c.sensitivity[seat] = v; break;
    case fSlow:  c.accel[seat].slow  = v; break;
    case fFast:  c.accel[seat].fast  = v; break;
    default:     c.accel[seat].speed = v; break;
    }
}

void EnsureSeats(Config& c, int n) {
    if ((int)c.sensitivity.size() < n) c.sensitivity.resize(n, 1.0);
    if ((int)c.accel.size()       < n) c.accel.resize(n);
    if ((int)c.screen.size()      < n) c.screen.resize(n);
    if ((int)c.seatColors.size()  < n) c.seatColors.resize(n, RGB(200, 200, 200));
    if ((int)c.cursorStyle.size() < n) c.cursorStyle.resize(n, (int)kStyleBright);
}

std::wstring Format(Field f, double v) {
    wchar_t buf[32];
    // The number keeps its left-to-right look (a mark in front) inside a right-to-left line.
    int lead = 0;
    if (UiRtl()) buf[lead++] = 0x200E;   // LEFT-TO-RIGHT MARK
    swprintf(buf + lead, 32 - lead, kRange[f].fmt, v);
    const wchar_t dec = LangData(UiLang()).decimal;
    if (dec != L'.')
        for (wchar_t* p = buf; *p; ++p) if (*p == L'.') *p = dec;
    return buf;
}

// The seat the engine gave the laptop touchpad: it keeps it even while the
// touchpad is cut off, so its section is there to switch it back on.
int TouchpadSeat(const Config& c) {
    for (const auto& kv : c.mouseBinding)
        if (IsIntegratedPointer(kv.first) && kv.second < c.seatCount) return kv.second;
    return -1;
}

// A seat is set aside when the mouse bound to it is in the [disabled] list.
bool SeatSetAside(const Config& c, int seat) {
    for (const auto& kv : c.mouseBinding)
        if (kv.second == seat && !IsIntegratedPointer(kv.first) && c.disabledMice.count(kv.first)) return true;
    return false;
}

// The make of each seat's mouse, read from the devices plugged in now (and from
// the vendor id alone for the unplugged ones). Done when the window opens and
// when the engine reports a change of devices, never while painting.
void RefreshBrands(const Config& c) {
    std::set<std::wstring> plugged;
    for (const auto& d : EnumerateDevices())
        if (d.type == RIM_TYPEMOUSE) plugged.insert(d.path);
    for (int i = 0; i < kMaxSeats; ++i) g.brand[i].clear();
    for (int i = 0; i < g.seats && i < kMaxSeats; ++i) {
        if (i == g.padSeat) continue;
        for (const auto& kv : c.mouseBinding) {
            if (kv.second != i || IsIntegratedPointer(kv.first)) continue;
            g.brand[i] = MouseBrand(kv.first, plugged.count(kv.first) != 0);
            if (!g.brand[i].empty()) break;
        }
    }
}

// What the title of a seat's section says: the make of the mouse, with the seat
// number when another mouse has the same make, "Mouse n" when the make is unknown.
std::wstring SeatTitle(int i) {
    wchar_t title[160];
    if (i == g.padSeat) return S(sTouchpad);
    const std::wstring& b = g.brand[i];
    if (b.empty()) { swprintf(title, 160, S(sSeat), i + 1); return title; }
    bool twin = false;
    for (int j = 0; j < g.seats && j < kMaxSeats; ++j)
        if (j != i && j != g.padSeat && _wcsicmp(g.brand[j].c_str(), b.c_str()) == 0) twin = true;
    if (twin) swprintf(title, 160, S(sBrandMany), b.c_str(), i + 1);
    else      swprintf(title, 160, S(sBrandOne), b.c_str());
    return title;
}

// ---------------------------------------------------------------- apply

int TextHeightOf(const wchar_t* text, int w);   // below, with the layout

// The status is at most two lines, centred on the row of the buttons.
void PlaceStatus() {
    const RECT& r = g.statusRow;
    if (r.right <= r.left) return;
    wchar_t text[512];
    GetWindowTextW(g.status, text, (int)(sizeof(text) / sizeof(text[0])));
    int h = TextHeightOf(text, r.right - r.left);
    const int maxH = r.bottom - r.top;
    if (h > maxH) h = maxH;
    SetWindowPos(g.status, nullptr, r.left, r.top + (maxH - h) / 2, r.right - r.left, h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void SetStatus(const std::wstring& text) {
    SetWindowTextW(g.status, text.c_str());
    PlaceStatus();
    if (g.self) InvalidateRect(g.self, &g.statusRow, TRUE);
}

// Merge the seat values into the file as it is now, then wake the engine.
void Apply() {
    g.pending = false;
    Config disk;
    disk.Load(g.path);
    EnsureSeats(disk, g.seats);
    for (int i = 0; i < g.seats; ++i) {
        disk.sensitivity[i] = g.cur.sensitivity[i];
        disk.accel[i]       = g.cur.accel[i];
        disk.screen[i]      = g.cur.screen[i];
        disk.seatColors[i]  = g.cur.seatColors[i];
        disk.cursorStyle[i] = g.cur.cursorStyle[i];
        disk.buttons[i]     = g.cur.buttons[i];
    }
    disk.touchpad = g.cur.touchpad;
    for (const auto& kv : disk.mouseBinding) {
        if (kv.second < 0 || kv.second >= g.seats || IsIntegratedPointer(kv.first)) continue;
        if (!g.off[kv.second])                     disk.disabledMice.erase(kv.first);
        else if (disk.disabledMice.count(kv.first) ||
                 disk.disabledMice.size() < Config::kMaxDisabled) disk.disabledMice.insert(kv.first);
    }
    disk.language = g.cur.language;
    if (!disk.Save(g.path)) {
        wchar_t buf[MAX_PATH + 64];
        swprintf(buf, MAX_PATH + 64, S(sSaveFailed), g.path.c_str());
        SetStatus(buf);
        return;
    }
    HWND engine = FindWindowW(kEngineClass, nullptr);
    if (engine && PostMessageW(engine, kReloadSettingsMsg, 0, 0)) SetStatus(S(sApplied));
    else                                                          SetStatus(S(sNotRunning));
}

// Coalesce a slider drag into a few writes a second.
void ApplySoon(HWND hwnd) {
    g.pending = true;
    SetTimer(hwnd, kTimerApply, 150, nullptr);
}

// ---------------------------------------------------------------- start with Windows

// The engine, when it is running elevated, is the one that owns the task: it
// can create and delete it without a prompt. Otherwise a one-shot elevated copy
// of this program does it, which costs one UAC prompt, once per change.
bool AskEngine(WPARAM request, DWORD_PTR& answer) {
    HWND engine = FindWindowW(kEngineClass, nullptr);
    answer = 0;
    return engine && SendMessageTimeoutW(engine, kAutostartMsg, request, 0, SMTO_ABORTIFHUNG,
                                         1000, &answer) != 0;
}

// The task as it is now: 1, 0, or -1 when nobody can tell.
int ReadAutostart() {
    DWORD_PTR r;
    if (AskEngine(kAutostartQuery, r) && (r & kAutoElevated) && (r & kAutoKnown))
        return (r & kAutoPresent) ? 1 : 0;
    return AutostartState();
}

void ShowAutostart(bool on) {
    SendMessageW(g.autostart, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
}

// The box is only usable to create the task from a protected copy. A task that
// exists can always be removed, and says so when it runs a program from a
// folder ordinary users can change.
void FitWindow(HWND hwnd);

void GateAutostart(int state) {
    g.autoState = state;
    bool taskBad = false;
    if (state == 1) {
        const std::wstring task = AutostartTaskPath();
        taskBad = !task.empty() && AutostartCheck(task) != AutostartVerdict::Ok;
    }
    EnableWindow(g.autostart, g.autoSafe || state == 1);
    const wchar_t* note = taskBad ? S(sAutostartTaskUnsafe) : (!g.autoSafe && state != 1) ? S(sAutostartUnsafe) : L"";
    SetWindowTextW(g.autoNote, note);
    ShowWindow(g.autoNote, *note ? SW_SHOW : SW_HIDE);
    // The note takes its room in the page, and gives it back when it goes.
    if ((*note != 0) != g.autoRoom) {
        g.autoRoom = *note != 0;
        if (g.laidOut) FitWindow(g.self);
    }
}

void FinishAutostart(HWND hwnd, bool ok, int state) {
    KillTimer(hwnd, kTimerAutostart);
    if (g.autoHelper) { CloseHandle(g.autoHelper); g.autoHelper = nullptr; }
    g.autoWait = false;
    ShowAutostart(state >= 0 ? state == 1 : !g.autoWant);   // what is real, not what was clicked
    GateAutostart(state >= 0 ? state : g.autoWant ? 0 : 1);
    SetStatus(ok ? S(g.autoWant ? sAutostartOn : sAutostartOff) : S(sAutostartFail));
}

void PollAutostart(HWND hwnd) {
    const int state = ReadAutostart();
    if (state == (g.autoWant ? 1 : 0)) { FinishAutostart(hwnd, true, state); return; }
    if (g.autoHelper) {
        // Waiting on the UAC prompt can take as long as the person likes.
        if (WaitForSingleObject(g.autoHelper, 0) == WAIT_OBJECT_0) FinishAutostart(hwnd, false, state);
    } else if (++g.autoTries > 40) {   // ten seconds
        FinishAutostart(hwnd, false, state);
    }
}

void OnAutostart(HWND hwnd) {
    if (g.autoWait) return;
    g.autoWant = SendMessageW(g.autostart, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g.autoTries = 0;
    if (g.autoWant && !g.autoSafe) {   // the engine would refuse too; no prompt for nothing
        ShowAutostart(false);          // the note under the box says why
        return;
    }

    DWORD_PTR r;
    const bool sent = AskEngine(kAutostartQuery, r) && (r & kAutoElevated) &&
                      AskEngine(g.autoWant ? kAutostartEnable : kAutostartDisable, r);
    if (!sent) {
        const std::wstring exe = SelfPath();
        if (exe.empty()) return;
        SHELLEXECUTEINFOW sei{};
        sei.cbSize       = sizeof(sei);
        sei.fMask        = SEE_MASK_NOCLOSEPROCESS;
        sei.hwnd         = hwnd;
        sei.lpVerb       = L"runas";
        sei.lpFile       = exe.c_str();
        sei.lpParameters = g.autoWant ? L"--autostart=on" : L"--autostart=off";
        sei.nShow        = SW_HIDE;
        if (!ShellExecuteExW(&sei)) {   // the prompt was refused, or nothing could start
            ShowAutostart(ReadAutostart() == 1);
            SetStatus(S(sAutostartDenied));
            return;
        }
        g.autoHelper = sei.hProcess;
    }
    g.autoWait = true;
    EnableWindow(g.autostart, FALSE);
    SetStatus(S(sAutostartWait));
    SetTimer(hwnd, kTimerAutostart, 250, nullptr);
}

// ---------------------------------------------------------------- learned buttons

const wchar_t* ActionName(int action) {
    static const Str ids[kActCount] = { sActNone, sActLeft, sActRight, sActMiddle, sActBack, sActForward, sActKeys };
    return S(ids[action < 0 || action >= kActCount ? 0 : action]);
}

std::wstring KeyName(UINT vk) {
    const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    wchar_t name[64] = L"";
    GetKeyNameTextW((LONG)((scan << 16) | (IsExtendedVk(vk) ? (1u << 24) : 0u)), name, 64);
    if (!*name) swprintf(name, 64, L"0x%X", vk);
    return name;
}

std::wstring ShortcutText(UINT mods, UINT vk) {
    std::wstring s;
    if (mods & kModCtrl)  s += std::wstring(S(sCtrlName))  + L"+";
    if (mods & kModAlt)   s += std::wstring(S(sAltName))   + L"+";
    if (mods & kModShift) s += std::wstring(S(sShiftName)) + L"+";
    if (mods & kModWin)   s += std::wstring(S(sWinName))   + L"+";
    return vk ? s + KeyName(vk) : s;
}

// What the list of a seat shows for one learned button.
std::wstring ListText(const ButtonMap& b) {
    std::wstring s = b.label.empty() ? b.sig : b.label;
    s += UiRtl() ? L"  ←  " : L"  →  ";   // the arrow points from the button to its action
    s += (b.action == kActKeys && b.vk) ? ShortcutText(b.mods, b.vk) : std::wstring(ActionName(b.action));
    return s;
}

// The action a standard mouse button has by itself, so that learning it does
// not take it away.
int NaturalAction(const ButtonMap& b) {
    if (b.kind != L'm') return kActNone;
    switch (b.code) {
    case RI_MOUSE_LEFT_BUTTON_DOWN:   return kActLeft;
    case RI_MOUSE_RIGHT_BUTTON_DOWN:  return kActRight;
    case RI_MOUSE_MIDDLE_BUTTON_DOWN: return kActMiddle;
    case RI_MOUSE_BUTTON_4_DOWN:      return kActBack;
    case RI_MOUSE_BUTTON_5_DOWN:      return kActForward;
    }
    return kActNone;
}

ButtonMap* Selected(int seat) {
    auto& list = g.cur.buttons[seat];
    if (list.empty()) return nullptr;
    if (g.sel[seat] < 0 || g.sel[seat] >= (int)list.size()) g.sel[seat] = 0;
    return &list[g.sel[seat]];
}

// The learned-button part of one seat's section.
void ShowButtons(int seat) {
    SeatUi& u = g.ui[seat];
    auto& list = g.cur.buttons[seat];
    ButtonMap* b = Selected(seat);

    SendMessageW(u.list, CB_RESETCONTENT, 0, 0);
    for (const ButtonMap& m : list) SendMessageW(u.list, CB_ADDSTRING, 0, (LPARAM)ListText(m).c_str());
    SendMessageW(u.list, CB_SETCURSEL, b ? g.sel[seat] : -1, 0);
    SendMessageW(u.action, CB_SETCURSEL, b ? b->action : -1, 0);

    const bool keys = b && b->action == kActKeys;
    std::wstring text = S(sKeysNone);
    if (g.capturing == seat)     text = S(sKeysCapture);
    else if (keys && b->vk)      text = ShortcutText(b->mods & ~kModWin, b->vk);
    SetWindowTextW(u.keys, text.c_str());
    SendMessageW(u.win, BM_SETCHECK, (keys && (b->mods & kModWin)) ? BST_CHECKED : BST_UNCHECKED, 0);

    EnableWindow(u.learn, g.learning < 0 && g.capturing < 0);
    EnableWindow(u.list, b != nullptr);
    EnableWindow(u.action, b != nullptr);
    EnableWindow(u.keys, keys);
    EnableWindow(u.win, keys);
    EnableWindow(u.forget, b != nullptr);

    // Under the buttons: what the engine is doing, else what just happened,
    // else what the selected button implies.
    std::wstring note;
    if (g.learning == seat)          note = S(sListening);
    else if (!g.note[seat].empty())  note = g.note[seat];
    else if (!b)                     note = S(sNoButtons);
    else if (b->WindowsAlsoActs())   note = S(sNative);
    SetWindowTextW(u.note, note.c_str());
}

void Note(int seat, const std::wstring& text) {
    g.note[seat] = text;
    ShowButtons(seat);
}

void OnLearnClick(HWND hwnd, int seat) {
    if (g.learning >= 0) return;
    if ((int)g.cur.buttons[seat].size() >= kMaxLearned) {
        wchar_t buf[160];
        swprintf(buf, 160, S(sFull), kMaxLearned);
        Note(seat, buf);
        return;
    }
    HWND engine = FindWindowW(kEngineClass, nullptr);
    if (!engine) { Note(seat, S(sEngineOff)); return; }
    if (g.pending) Apply();
    if (!PostMessageW(engine, kLearnMsg, (WPARAM)seat + 1, 0)) { Note(seat, S(sEngineOff)); return; }
    g.learning = seat;
    g.note[seat].clear();
    // The engine ends its own listening; this is only for the day it never answers.
    SetTimer(hwnd, kTimerLearn, kLearnListenMs + 4000, nullptr);
    for (int i = 0; i < g.seats; ++i) ShowButtons(i);
}

void EndLearning(HWND hwnd) {
    KillTimer(hwnd, kTimerLearn);
    const int seat = g.learning;
    g.learning = -1;
    if (seat >= 0 && seat < g.seats) for (int i = 0; i < g.seats; ++i) ShowButtons(i);
}

// The engine has finished listening to a seat's mouse.
void OnLearned(HWND hwnd, int seat, LPARAM result) {
    if (seat < 0 || seat >= g.seats) { EndLearning(hwnd); return; }
    g.learning = -1;
    KillTimer(hwnd, kTimerLearn);
    std::wstring note = S(sNothing);
    if (result == kLearnAbsent) note = S(sAbsent);
    if (result == kLearnHeard) {
        int heardSeat = -1;
        std::wstring sig, label;
        if (ReadLearnResult(heardSeat, sig, label) && heardSeat == seat) {
            auto& list = g.cur.buttons[seat];
            int found = -1;
            for (int i = 0; i < (int)list.size(); ++i) if (list[i].sig == sig) found = i;
            if (found >= 0) {
                g.sel[seat] = found;
                note = S(sAlready);
            } else if ((int)list.size() < kMaxLearned) {
                ButtonMap b;
                b.sig   = sig;
                b.label = label;
                if (b.Parse()) {
                    b.action = NaturalAction(b);
                    list.push_back(std::move(b));
                    g.sel[seat] = (int)list.size() - 1;
                    note = S(sLearned);
                    Apply();
                }
            }
        }
    }
    g.note[seat] = note;
    for (int i = 0; i < g.seats; ++i) ShowButtons(i);
}

void OnForget(int seat) {
    auto& list = g.cur.buttons[seat];
    if (list.empty()) return;
    const int at = g.sel[seat] < (int)list.size() ? g.sel[seat] : 0;
    list.erase(list.begin() + at);
    if (g.sel[seat] >= (int)list.size()) g.sel[seat] = (int)list.size() - 1;
    if (g.sel[seat] < 0) g.sel[seat] = 0;
    g.note[seat].clear();
    ShowButtons(seat);
    Apply();
}

void OnListSelect(int seat) {
    const int sel = (int)SendMessageW(g.ui[seat].list, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)g.cur.buttons[seat].size()) return;
    g.sel[seat] = sel;
    g.note[seat].clear();
    ShowButtons(seat);
}

void OnActionSelect(int seat) {
    ButtonMap* b = Selected(seat);
    const int act = (int)SendMessageW(g.ui[seat].action, CB_GETCURSEL, 0, 0);
    if (!b || act < 0 || act >= kActCount) return;
    b->action = act;
    // A shortcut action with no shortcut yet does nothing; ask for it at once.
    g.note[seat].clear();
    ShowButtons(seat);
    Apply();
    if (act == kActKeys && !b->vk) SendMessageW(g.ui[seat].keys, BM_CLICK, 0, 0);
}

void StopCapture(int seat) {
    g.capturing = -1;
    if (seat >= 0 && seat < g.seats) ShowButtons(seat);
}

void OnKeysClick(int seat) {
    if (g.capturing == seat) { StopCapture(seat); return; }
    if (!Selected(seat)) return;
    g.capturing = seat;
    ShowButtons(seat);
    SetFocus(g.ui[seat].keys);
}

void OnWinToggle(int seat) {
    ButtonMap* b = Selected(seat);
    if (!b) return;
    if (SendMessageW(g.ui[seat].win, BM_GETCHECK, 0, 0) == BST_CHECKED) b->mods |= kModWin;
    else                                                                 b->mods &= ~kModWin;
    ShowButtons(seat);
    Apply();
}

bool SeatOf(int id, int& seat, int& ctl);

// The shortcut is typed on the button itself: its subclass sees the keys (and
// asks the dialog loop to leave them alone) while capturing.
LRESULT CALLBACK KeysProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    int seat = -1, ctl = 0;
    const bool mine = SeatOf(GetDlgCtrlID(hwnd), seat, ctl) && ctl == cKeys;
    if (mine && g.capturing == seat) {
        switch (msg) {
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS | DLGC_WANTCHARS;
        case WM_CHAR: case WM_SYSCHAR: case WM_KEYUP: case WM_SYSKEYUP:
            return 0;
        case WM_KILLFOCUS:
            StopCapture(seat);
            break;
        case WM_KEYDOWN: case WM_SYSKEYDOWN: {
            const UINT vk = (UINT)wp;
            if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN)
                return 0;                          // a modifier alone: wait for the key
            if (vk == VK_ESCAPE) { StopCapture(seat); return 0; }
            if (ButtonMap* b = Selected(seat)) {
                UINT mods = b->mods & kModWin;
                if (GetKeyState(VK_CONTROL) & 0x8000) mods |= kModCtrl;
                if (GetKeyState(VK_MENU)    & 0x8000) mods |= kModAlt;
                if (GetKeyState(VK_SHIFT)   & 0x8000) mods |= kModShift;
                b->vk   = vk;
                b->mods = mods;
                g.note[seat].clear();
                g.capturing = -1;
                ShowButtons(seat);
                Apply();
            }
            return 0;
        }
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- look

// The two choices that make a pointer recognisable: its colour and its set of
// shapes, with a preview of the set on the backgrounds that hide pointers most.
void ShowLook(int seat) {
    SeatUi& u = g.ui[seat];
    const COLORREF cur = g.cur.seatColors[seat];
    SendMessageW(u.style, CB_SETCURSEL, g.cur.cursorStyle[seat], 0);
    SendMessageW(u.color, CB_RESETCONTENT, 0, 0);
    int sel = -1;
    for (int k = 0; k < 10; ++k) {
        SendMessageW(u.color, CB_ADDSTRING, 0, (LPARAM)S((Str)(sCol0 + k)));
        if (kColors[k] == cur) sel = k;
    }
    if (sel < 0) {      // a colour typed in the ini: kept, and shown
        wchar_t buf[64];
        swprintf(buf, 64, S(sColorCustom), (GetRValue(cur) << 16) | (GetGValue(cur) << 8) | GetBValue(cur));
        SendMessageW(u.color, CB_ADDSTRING, 0, (LPARAM)buf);
        sel = 10;
    }
    SendMessageW(u.color, CB_SETCURSEL, sel, 0);
    InvalidateRect(u.preview, nullptr, FALSE);
}

void OnColor(int seat) {
    const int sel = (int)SendMessageW(g.ui[seat].color, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= 10) return;          // the custom entry, unchanged
    g.cur.seatColors[seat] = kColors[sel];
    ShowLook(seat);
    InvalidateRect(g.panel, nullptr, FALSE);   // the swatch in the header
    Apply();
}

void OnStyle(int seat) {
    const int sel = (int)SendMessageW(g.ui[seat].style, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= kStyleCount) return;
    g.cur.cursorStyle[seat] = sel;
    ShowLook(seat);
    Apply();
}

// ---------------------------------------------------------------- controls

void ShowSeat(int seat) {
    SeatUi& u = g.ui[seat];
    const bool accel = g.cur.accel[seat].on;
    for (int f = 0; f < fFieldCount; ++f) {
        const double v = Get(g.cur, seat, (Field)f);
        const Range& r = kRange[f];
        const double clamped = v < r.lo ? r.lo : (v > r.hi ? r.hi : v);
        SendMessageW(u.slider[f], TBM_SETPOS, TRUE, (LPARAM)std::lround(clamped * r.scale));
        SetWindowTextW(u.value[f], Format((Field)f, v).c_str());
        // Acceleration replaces the fixed multiplier, so only one of the two
        // is ever live; grey out the other rather than hide it.
        const bool on = (f == fSens) ? !accel : accel;
        EnableWindow(u.label[f], on);
        EnableWindow(u.value[f], on);
        EnableWindow(u.slider[f], on);
    }
    SendMessageW(u.fixed, BM_SETCHECK, accel ? BST_UNCHECKED : BST_CHECKED, 0);
    SendMessageW(u.accel, BM_SETCHECK, accel ? BST_CHECKED : BST_UNCHECKED, 0);
    SetWindowTextW(u.explain, S(accel ? sExplainAccel : sExplainFixed));

    // Screen list: "none", every screen plugged in, and the chosen one if it
    // is unplugged, so opening the window never drops a choice by itself.
    SendMessageW(u.screen, CB_RESETCONTENT, 0, 0);
    SendMessageW(u.screen, CB_ADDSTRING, 0, (LPARAM)S(sScreenNone));
    const std::wstring& want = g.cur.screen[seat];
    int sel = 0;
    for (size_t k = 0; k < g.screens.size(); ++k) {
        const ScreenInfo& sc = g.screens[k];
        wchar_t buf[96];
        swprintf(buf, 96, S(sScreenItem), (int)k + 1,
                 (int)(sc.rc.right - sc.rc.left), (int)(sc.rc.bottom - sc.rc.top));
        std::wstring item = buf;
        if (sc.primary) item += S(sPrimary);
        SendMessageW(u.screen, CB_ADDSTRING, 0, (LPARAM)item.c_str());
        if (!want.empty() && _wcsicmp(sc.name.c_str(), want.c_str()) == 0) sel = (int)k + 1;
    }
    if (!want.empty() && sel == 0) {
        wchar_t buf[96];
        swprintf(buf, 96, S(sUnplugged), want.c_str());
        SendMessageW(u.screen, CB_ADDSTRING, 0, (LPARAM)buf);
        sel = (int)g.screens.size() + 1;
    }
    SendMessageW(u.screen, CB_SETCURSEL, sel, 0);

    const bool pad = seat == g.padSeat;
    ShowWindow(u.off, pad ? SW_HIDE : SW_SHOWNA);
    SendMessageW(u.off, BM_SETCHECK, g.off[seat] ? BST_CHECKED : BST_UNCHECKED, 0);
    EnableWindow(u.learn, !g.off[seat]);   // a mouse set aside sends nothing to listen to
    ShowWindow(u.padLabel, pad ? SW_SHOWNA : SW_HIDE);
    ShowWindow(u.pad, pad ? SW_SHOWNA : SW_HIDE);
    if (pad) SendMessageW(u.pad, CB_SETCURSEL, (WPARAM)g.cur.touchpad, 0);

    ShowLook(seat);
    ShowButtons(seat);
}

// Applied by the engine at once: it seats the touchpad or cuts it off.
void OnPadMode(int seat) {
    const int sel = (int)SendMessageW(g.ui[seat].pad, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel > 2) return;
    g.cur.touchpad = (Config::Touchpad)sel;
    Apply();
}

// "Disable": the mouse stays in its seat, colour and speed kept, but its input is
// ignored; the engine applies it at once. The last pointer that works cannot be
// set aside, so that the computer is never left without one (the touchpad counts
// when it is in use; a mouse that is unplugged does not).
void OnDisable(int seat) {
    const bool want = SendMessageW(g.ui[seat].off, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (want == g.off[seat]) return;
    if (want) {
        bool other = false;
        for (int j = 0; j < g.seats && j < kMaxSeats; ++j)
            if (j != seat && !g.off[j] && (g.present & (1u << j))) other = true;
        if (!other) {
            SendMessageW(g.ui[seat].off, BM_SETCHECK, BST_UNCHECKED, 0);
            SetStatus(S(sLastActive));
            return;
        }
    }
    g.off[seat] = want;
    ShowSeat(seat);
    InvalidateRect(g.panel, nullptr, TRUE);   // the title greys out
    Apply();
}

void OnScreen(int seat) {
    const int sel = (int)SendMessageW(g.ui[seat].screen, CB_GETCURSEL, 0, 0);
    if (sel == 0)                              g.cur.screen[seat].clear();
    else if (sel <= (int)g.screens.size())     g.cur.screen[seat] = g.screens[sel - 1].name;
    else                                       return;   // the unplugged one, unchanged
    ShowSeat(seat);
    Apply();
}

HWND Make(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
    return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0,
                           parent, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
}

BOOL CALLBACK SetFontProc(HWND child, LPARAM font) {
    SendMessageW(child, WM_SETFONT, (WPARAM)font, TRUE);
    return TRUE;
}

// Tooltips: a short explanation beside the setting it concerns. The text is
// read again when the language changes (RefreshTips).
void AddTip(HWND parent, HWND tool, Str id) {
    if (!g.tip || !tool) return;
    TOOLINFOW ti{};
    ti.cbSize   = sizeof(ti);
    ti.uFlags   = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd     = parent;
    ti.uId      = (UINT_PTR)tool;
    ti.lpszText = const_cast<LPWSTR>(S(id));
    SendMessageW(g.tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    g.tips.push_back({ tool, parent, id });
}

void RefreshTips() {
    if (!g.tip) return;
    for (const State::Tip& t : g.tips) {
        TOOLINFOW ti{};
        ti.cbSize   = sizeof(ti);
        ti.hwnd     = t.parent;
        ti.uId      = (UINT_PTR)t.tool;
        ti.lpszText = const_cast<LPWSTR>(S(t.id));
        SendMessageW(g.tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
    }
}

// Every text of one seat's section, in the language in use. The lists keep
// their selection; ShowSeat fills in what depends on the values.
void SetSeatTexts(int i) {
    SeatUi& u = g.ui[i];
    const Str names[fFieldCount] = { sSensitivity, sSlow, sFast, sSpeed };
    for (int f = 0; f < fFieldCount; ++f) SetWindowTextW(u.label[f], S(names[f]));
    SetWindowTextW(u.padLabel, S(sPadMode));
    SetWindowTextW(u.colorLabel, S(sColorLabel));
    SetWindowTextW(u.styleLabel, S(sStyleLabel));
    SetWindowTextW(u.fixed, S(sFixed));
    SetWindowTextW(u.accel, S(sAccel));
    SetWindowTextW(u.screenLabel, S(sScreen));
    SetWindowTextW(u.defaults, S(sDefaults));
    SetWindowTextW(u.learn, S(sLearn));
    SetWindowTextW(u.forget, S(sForget));
    SetWindowTextW(u.win, S(sWin));
    SetWindowTextW(u.off, S(sDisable));

    auto refill = [](HWND combo, int count, auto text) {
        const LRESULT sel = SendMessageW(combo, CB_GETCURSEL, 0, 0);
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        for (int k = 0; k < count; ++k) SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)text(k));
        SendMessageW(combo, CB_SETCURSEL, sel, 0);
    };
    refill(u.pad, 3, [](int k) { return S((Str)(sPadAuto + k)); });
    refill(u.style, kStyleCount, [](int k) { return S((Str)(sStyle0 + k)); });
    refill(u.action, kActCount, [](int k) { return ActionName(k); });
}

// The controls of one seat's section.
void CreateSeatControls(int i) {
    HWND p = g.panel;
    SeatUi& u = g.ui[i];
    u.padLabel = Make(p, L"STATIC", L"", SS_LEFT, CtlId(i, cPadLabel));
    u.pad = Make(p, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, CtlId(i, cPad));
    u.off = Make(p, L"BUTTON", L"", WS_TABSTOP | BS_AUTOCHECKBOX, CtlId(i, cOff));
    u.colorLabel = Make(p, L"STATIC", L"", SS_LEFT, CtlId(i, cColorLabel));
    u.color = Make(p, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, CtlId(i, cColor));
    u.styleLabel = Make(p, L"STATIC", L"", SS_LEFT, CtlId(i, cStyleLabel));
    u.style = Make(p, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, CtlId(i, cStyle));
    u.preview = Make(p, kPreviewClass, L"", 0, CtlId(i, cPreview));
    // Two modes, one choice: the radio buttons first, the sliders of the mode below.
    u.fixed   = Make(p, L"BUTTON", L"", WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON, CtlId(i, cFixed));
    u.accel   = Make(p, L"BUTTON", L"", WS_TABSTOP | BS_AUTORADIOBUTTON, CtlId(i, cAccel));
    u.explain = Make(p, L"STATIC", L"", SS_LEFT | SS_NOPREFIX | WS_GROUP, CtlId(i, cExplain));
    for (int f = 0; f < fFieldCount; ++f) {
        u.label[f]  = Make(p, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, CtlId(i, cLabel + f));
        u.value[f]  = Make(p, L"STATIC", L"", SS_RIGHT, CtlId(i, cValue + f));
        u.slider[f] = Make(p, TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
                           CtlId(i, cSlider + f));
        const Range& r = kRange[f];
        SendMessageW(u.slider[f], TBM_SETRANGEMIN, FALSE, (LPARAM)std::lround(r.lo * r.scale));
        SendMessageW(u.slider[f], TBM_SETRANGEMAX, FALSE, (LPARAM)std::lround(r.hi * r.scale));
        SendMessageW(u.slider[f], TBM_SETLINESIZE, 0, 1);
        SendMessageW(u.slider[f], TBM_SETPAGESIZE, 0, (LPARAM)std::lround(r.scale / 2));
    }
    u.screenLabel = Make(p, L"STATIC", L"", SS_LEFT, CtlId(i, cScreenLabel));
    u.screen = Make(p, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, CtlId(i, cScreen));
    u.defaults = Make(p, L"BUTTON", L"", WS_TABSTOP | BS_PUSHBUTTON, CtlId(i, cDefaults));

    u.learn  = Make(p, L"BUTTON", L"", WS_TABSTOP | BS_PUSHBUTTON, CtlId(i, cLearn));
    u.forget = Make(p, L"BUTTON", L"", WS_TABSTOP | BS_PUSHBUTTON, CtlId(i, cForget));
    u.note   = Make(p, L"STATIC", L"", SS_LEFT, CtlId(i, cNote));
    u.list   = Make(p, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, CtlId(i, cList));
    u.action = Make(p, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, CtlId(i, cAction));
    u.keys   = Make(p, L"BUTTON", L"", WS_TABSTOP | BS_PUSHBUTTON, CtlId(i, cKeys));
    u.win    = Make(p, L"BUTTON", L"", WS_TABSTOP | BS_AUTOCHECKBOX, CtlId(i, cWin));
    SetWindowSubclass(u.keys, KeysProc, kSubclassKeys, 0);
    SetSeatTexts(i);
    AddTip(p, u.fixed, sTipModes);
    AddTip(p, u.accel, sTipModes);
    AddTip(p, u.learn, sTipLearn);
    ShowSeat(i);
}

// The names in the list of languages: "automatic", then each language by its
// own name, which never changes with the language in use.
void FillLanguages() {
    SendMessageW(g.lang, CB_RESETCONTENT, 0, 0);
    SendMessageW(g.lang, CB_ADDSTRING, 0, (LPARAM)S(sLangAuto));
    for (int k = 0; k < kLangCount; ++k)
        SendMessageW(g.lang, CB_ADDSTRING, 0, (LPARAM)LangData((Lang)k).name);
    SendMessageW(g.lang, CB_SETCURSEL, g.cur.language + 1, 0);   // kLangAuto is -1
}

// The texts that do not belong to a mouse.
void SetCommonTexts(HWND hwnd) {
    SetWindowTextW(hwnd, S(sTitle));
    const Str tabs[2] = { sGeneral, sTabMice };
    for (int k = 0; k < 2; ++k) {
        TCITEMW ti{};
        ti.mask    = TCIF_TEXT;
        ti.pszText = const_cast<LPWSTR>(S(tabs[k]));
        SendMessageW(g.tab, TCM_SETITEMW, k, (LPARAM)&ti);
    }
    SetWindowTextW(g.autostart, S(sAutostart));
    SetWindowTextW(g.langLabel, S(sLanguage));
    FillLanguages();
    SetWindowTextW(g.help, S(sHelp));
    SetWindowTextW(g.about, S(sAbout));
    SetWindowTextW(g.hint, S(sHint));
    SetWindowTextW(g.undo, S(sUndo));
    SetWindowTextW(g.close, S(sClose));
}

void CreateControls(HWND hwnd, HINSTANCE inst) {
    // Created in the order the Tab key walks them: the tabs, the pages, the footer.
    g.tab = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                            0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)kIdTab, inst, nullptr);
    for (int k = 0; k < 2; ++k) {
        TCITEMW ti{};
        ti.mask    = TCIF_TEXT;
        ti.pszText = const_cast<LPWSTR>(L"");
        SendMessageW(g.tab, TCM_INSERTITEMW, k, (LPARAM)&ti);
    }
    g.tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                            WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                            hwnd, nullptr, inst, nullptr);
    if (g.tip) SendMessageW(g.tip, TTM_SETMAXTIPWIDTH, 0, Px(300));

    // The general page.
    g.page = CreateWindowExW(WS_EX_CONTROLPARENT, kPageClass, L"", WS_CHILD | WS_CLIPCHILDREN,
                             0, 0, 0, 0, hwnd, nullptr, inst, nullptr);
    HWND gp = g.page;
    g.autostart = Make(gp, L"BUTTON", L"", WS_TABSTOP | BS_AUTOCHECKBOX, kIdAutostart);
    g.autoNote  = Make(gp, L"STATIC", L"", SS_LEFT, 0);
    g.langLabel = Make(gp, L"STATIC", L"", SS_LEFT, 0);
    g.lang      = Make(gp, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, kIdLang);
    g.help      = Make(gp, L"BUTTON", L"", WS_TABSTOP | BS_PUSHBUTTON, kIdHelp);
    g.about     = Make(gp, L"STATIC", L"", SS_LEFT, 0);
    g.site      = Make(gp, L"STATIC", kSiteUrl, SS_LEFT | SS_NOTIFY, kIdSite);
    g.autoSafe  = AutostartCheck(SelfPath()) == AutostartVerdict::Ok;
    const int autoState = ReadAutostart();
    ShowAutostart(autoState == 1);

    // The mice page: a panel that scrolls.
    g.panel = CreateWindowExW(WS_EX_CONTROLPARENT, kPanelClass, L"",
                              WS_CHILD | WS_VSCROLL | WS_CLIPCHILDREN,
                              0, 0, 0, 0, hwnd, nullptr, inst, nullptr);
    for (int i = 0; i < g.seats; ++i) CreateSeatControls(i);

    // The footer, common to both tabs.
    g.hint   = Make(hwnd, L"STATIC", L"", SS_LEFT, 0);
    g.status = Make(hwnd, L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 0);
    g.undo   = Make(hwnd, L"BUTTON", L"", WS_TABSTOP | BS_PUSHBUTTON, kIdUndo);
    g.close  = Make(hwnd, L"BUTTON", L"", WS_TABSTOP | BS_DEFPUSHBUTTON, kIdClose);

    SetCommonTexts(hwnd);
    GateAutostart(autoState);   // after the texts: its note is one of them
}

// ---------------------------------------------------------------- scrolling

void ScrollTo(int pos) {
    SCROLLINFO si{ sizeof(si), SIF_RANGE | SIF_PAGE };
    GetScrollInfo(g.panel, SB_VERT, &si);
    int most = si.nMax - (int)si.nPage + 1;
    if (most < 0) most = 0;
    if (pos > most) pos = most;
    if (pos < 0) pos = 0;
    if (pos == g.scrollY) return;
    ScrollWindowEx(g.panel, 0, g.scrollY - pos, nullptr, nullptr, nullptr, nullptr,
                   SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
    g.scrollY = pos;
    si.fMask = SIF_POS;
    si.nPos  = pos;
    SetScrollInfo(g.panel, SB_VERT, &si, TRUE);
    UpdateWindow(g.panel);
}

void OnPanelScroll(WPARAM wp) {
    SCROLLINFO si{ sizeof(si), SIF_ALL };
    GetScrollInfo(g.panel, SB_VERT, &si);
    int pos = si.nPos;
    switch (LOWORD(wp)) {
    case SB_LINEUP:        pos -= Px(24); break;
    case SB_LINEDOWN:      pos += Px(24); break;
    case SB_PAGEUP:        pos -= (int)si.nPage; break;
    case SB_PAGEDOWN:      pos += (int)si.nPage; break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION: pos = si.nTrackPos; break;
    case SB_TOP:           pos = 0; break;
    case SB_BOTTOM:        pos = si.nMax; break;
    default: return;
    }
    ScrollTo(pos);
}

// ---------------------------------------------------------------- layout

constexpr int kMargin = 16, kColW = 250, kGap = 28, kPerRow = 4;

int Columns() { return g.seats < kPerRow ? g.seats : kPerRow; }

// Lays out one seat's section from (x, y); returns where it ends.
int LayoutSeat(int i, int x, int y) {
    SeatUi& u = g.ui[i];
    const int colW = Px(kColW);
    auto place = [](HWND h, int px, int py, int w, int hgt) {
        SetWindowPos(h, nullptr, px, py, w, hgt, SWP_NOZORDER | SWP_NOACTIVATE);
    };
    u.header = { x, y, x + colW, y + Px(24) };
    place(u.off, x + colW - g.offW, y + Px(1), g.offW, Px(22));
    y += Px(34);
    if (i == g.padSeat) {
        place(u.padLabel, x, y, colW, Px(18));
        y += Px(20);
        place(u.pad, x, y, colW, Px(200));
        y += Px(32);
    }
    place(u.colorLabel, x, y, colW, Px(18));
    y += Px(20);
    place(u.color, x, y, colW, Px(200));
    y += Px(32);
    place(u.styleLabel, x, y, colW, Px(18));
    y += Px(20);
    place(u.style, x, y, colW, Px(200));
    y += Px(32);
    place(u.preview, x, y, colW, Px(112));
    y += Px(112) + Px(20);
    place(u.fixed, x, y, colW, Px(22));
    y += Px(24);
    place(u.accel, x, y, colW, Px(22));
    y += Px(26);
    place(u.explain, x, y, colW, Px(54));
    y += Px(58);
    for (int f = 0; f < fFieldCount; ++f) {
        place(u.label[f], x, y, colW - Px(64), Px(18));
        place(u.value[f], x + colW - Px(64), y, Px(64), Px(18));
        y += Px(20);
        place(u.slider[f], x - Px(4), y, colW + Px(8), Px(28));
        y += Px(f == fSens ? 40 : 34);
    }
    y += Px(4);
    place(u.screenLabel, x, y, colW, Px(18));
    y += Px(20);
    place(u.screen, x, y, colW, Px(200));
    y += Px(38);
    place(u.defaults, x, y, Px(150), Px(28));
    y += Px(28) + Px(20);

    // Learned buttons.
    const int forgetW = Px(84);
    place(u.learn, x, y, colW - forgetW - Px(6), Px(28));
    place(u.forget, x + colW - forgetW, y, forgetW, Px(28));
    y += Px(34);
    place(u.note, x, y, colW, Px(50));
    y += Px(54);
    place(u.list, x, y, colW, Px(200));
    y += Px(30);
    place(u.action, x, y, colW, Px(200));
    y += Px(30);
    const int winW = Px(54);
    place(u.keys, x, y, colW - winW - Px(6), Px(26));
    place(u.win, x + colW - winW, y, winW, Px(26));
    y += Px(26);
    return y;
}

// Height a static text needs when wrapped to the given width, in the window
// font, so every block takes the room its text uses and no more.
int TextHeightOf(const wchar_t* text, int w) {
    HDC dc = GetDC(g.self);
    HGDIOBJ old = SelectObject(dc, g.font);
    RECT r{ 0, 0, w, 0 };
    DrawTextW(dc, text, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(g.self, dc);
    return r.bottom;
}

int TextHeight(HWND h, int w) {
    wchar_t text[512];
    GetWindowTextW(h, text, (int)(sizeof(text) / sizeof(text[0])));
    return TextHeightOf(text, w);
}

// Width a button needs for its text.
int ButtonWidth(HWND h, int minDip) {
    wchar_t text[128];
    GetWindowTextW(h, text, (int)(sizeof(text) / sizeof(text[0])));
    HDC dc = GetDC(g.self);
    HGDIOBJ old = SelectObject(dc, g.font);
    SIZE sz{};
    GetTextExtentPoint32W(dc, text, (int)wcslen(text), &sz);
    SelectObject(dc, old);
    ReleaseDC(g.self, dc);
    return sz.cx + Px(28) > Px(minDip) ? sz.cx + Px(28) : Px(minDip);
}

// A right-to-left language mirrors the whole window (the tabs, the footer, the
// pages, the scroll bar, the text alignment), and Windows does it: the layout
// below is always computed left to right and the system flips what is drawn.
void SetMirror(HWND h, bool rtl) {
    const LONG_PTR bits = WS_EX_LAYOUTRTL | WS_EX_RTLREADING;
    const LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    const LONG_PTR want = rtl ? (ex | bits) : (ex & ~bits);
    if (want != ex) SetWindowLongPtrW(h, GWL_EXSTYLE, want);
}

BOOL CALLBACK MirrorProc(HWND h, LPARAM lp) {
    SetMirror(h, lp != 0);
    return TRUE;
}

void ApplyDirection(HWND hwnd) {
    const bool rtl = UiRtl();
    SetMirror(hwnd, rtl);
    EnumChildWindows(hwnd, MirrorProc, rtl ? 1 : 0);
    if (g.tip) SetMirror(g.tip, rtl);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

// The font of the interface of Windows in the language Windows is set to: it is
// the one that has the letters of that language. Segoe UI when it cannot be read.
void FaceOfWindows(wchar_t (&face)[LF_FACESIZE]) {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0) && ncm.lfMessageFont.lfFaceName[0])
        wcscpy_s(face, ncm.lfMessageFont.lfFaceName);
    else
        wcscpy_s(face, L"Segoe UI");
}

constexpr int kMinWidth = 600;   // dip: room for the general page and the footer

// Lays out every control for the current DPI and the tab shown; returns the
// client size.
SIZE Layout(HWND hwnd) {
    if (g.font) DeleteObject(g.font);
    if (g.bold) DeleteObject(g.bold);
    wchar_t face[LF_FACESIZE];
    FaceOfWindows(face);
    g.font = CreateFontW(-MulDiv(9, (int)g.dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                         0, 0, CLEARTYPE_QUALITY, 0, face);
    g.bold = CreateFontW(-MulDiv(11, (int)g.dpi, 72), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                         0, 0, CLEARTYPE_QUALITY, 0, face);
    EnumChildWindows(hwnd, SetFontProc, (LPARAM)g.font);
    {
        // The "Disable" box is as wide as its text: the title beside it gets the rest.
        HDC dc = GetDC(hwnd);
        HGDIOBJ old = SelectObject(dc, g.font);
        SIZE sz{};
        GetTextExtentPoint32W(dc, S(sDisable), (int)wcslen(S(sDisable)), &sz);
        SelectObject(dc, old);
        ReleaseDC(hwnd, dc);
        g.offW = sz.cx + Px(24);
    }
    auto place = [](HWND h, int x, int y, int w, int hgt) {
        SetWindowPos(h, nullptr, x, y, w, hgt, SWP_NOZORDER | SWP_NOACTIVATE);
    };

    const int colW = Px(kColW);
    const int m = Px(kMargin);

    // The mice page is laid out unscrolled, then scrolled back to the top.
    g.scrollY = 0;
    int bottom = m, rowTop = m, rowEnd = 0;
    for (int i = 0; i < g.seats; ++i) {
        if (i > 0 && i % kPerRow == 0) { rowTop = rowEnd + Px(28); rowEnd = 0; }
        const int x = m + (i % kPerRow) * (colW + Px(kGap));
        const int end = LayoutSeat(i, x, rowTop);
        if (end > rowEnd) rowEnd = end;
        if (end > bottom) bottom = end;
    }
    const int seatsW   = m * 2 + Columns() * colW + (Columns() - 1) * Px(kGap);
    const int width    = seatsW > Px(kMinWidth) ? seatsW : Px(kMinWidth);
    const int contentH = bottom + m;

    // The tabs: a strip as high as its items.
    place(g.tab, 0, 0, width, Px(40));
    RECT item{};
    SendMessageW(g.tab, TCM_GETITEMRECT, 0, (LPARAM)&item);
    const int tabH = item.bottom + Px(3);

    // The footer: one short grey sentence, then the status on the row of the buttons.
    const int inner  = width - m * 2;
    const int hintH  = TextHeight(g.hint, inner);
    const int rowH   = Px(34);
    const int footerH = Px(10) + hintH + Px(6) + rowH + m;

    // The general page: the box, its note, the language, the help, the credits.
    int gy = m;
    place(g.autostart, m, gy, inner, Px(22));
    gy += Px(22);
    if (g.autoRoom) {
        const int noteW = inner - Px(20);
        const int noteH = TextHeight(g.autoNote, noteW);
        place(g.autoNote, m + Px(20), gy + Px(2), noteW, noteH);
        gy += noteH + Px(8);
    }
    gy += Px(16);
    place(g.langLabel, m, gy, inner, Px(18));
    gy += Px(20);
    const int langW = Px(300);
    place(g.lang, m, gy, langW, Px(200));
    gy += Px(38);
    const int helpW = ButtonWidth(g.help, 100);
    place(g.help, m, gy, helpW, Px(28));
    gy += Px(28) + Px(24);
    const int aboutH = TextHeight(g.about, inner);
    place(g.about, m, gy, inner, aboutH);
    gy += aboutH;
    place(g.site, m, gy, inner, Px(18));
    gy += Px(18) + m;
    const int generalH = gy;

    // The window takes the height of the tab it shows; what does not fit the
    // screen scrolls (the mice page only).
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    RECT wr{}, cr{};
    GetWindowRect(hwnd, &wr);
    GetClientRect(hwnd, &cr);
    const int chrome    = (wr.bottom - wr.top) - (cr.bottom - cr.top);
    const int maxClient = (mi.rcWork.bottom - mi.rcWork.top) - chrome - Px(8);
    int panelH = contentH;
    const bool scrolls = tabH + panelH + footerH > maxClient;
    if (scrolls) panelH = maxClient - tabH - footerH;
    if (panelH < Px(200)) panelH = Px(200);
    // The scroll bar widens both tabs, so the window does not change width with them.
    const int sbW = scrolls ? ScrollBarWidth() : 0;
    const int totalW = width + sbW;

    place(g.tab, 0, 0, totalW, tabH);
    place(g.page, 0, tabH, totalW, generalH);
    place(g.panel, 0, tabH, totalW, panelH);
    SCROLLINFO si{ sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS };
    si.nMin  = 0;
    si.nMax  = contentH - 1;
    si.nPage = (UINT)(scrolls ? panelH : contentH + 1);
    si.nPos  = 0;
    SetScrollInfo(g.panel, SB_VERT, &si, TRUE);
    ShowWindow(g.page, g.curTab == tabGeneral ? SW_SHOWNA : SW_HIDE);
    ShowWindow(g.panel, g.curTab == tabMice ? SW_SHOWNA : SW_HIDE);

    int y = tabH + (g.curTab == tabGeneral ? generalH : panelH);
    place(g.hint, m, y + Px(10), totalW - m * 2, hintH);
    y += Px(10) + hintH + Px(6);
    const int closeW = ButtonWidth(g.close, 90), undoW = ButtonWidth(g.undo, 90), h = Px(30);
    const int gap = Px(10);
    const int closeX = totalW - m - closeW;
    const int undoX  = closeX - gap - undoW;
    place(g.close, closeX, y + (rowH - h) / 2, closeW, h);
    place(g.undo,  undoX,   y + (rowH - h) / 2, undoW, h);
    // The status takes what is left of the row, on the side of the reading.
    const int statusW = undoX - gap - m;
    const int statusX = m;
    g.statusRow = { statusX, y, statusX + statusW, y + rowH };
    PlaceStatus();
    y += rowH + m;
    g.laidOut = true;
    InvalidateRect(g.panel, nullptr, TRUE);
    InvalidateRect(hwnd, nullptr, TRUE);
    return { totalW, y };
}

RECT WindowRectFor(SIZE client, DWORD style) {
    RECT r{ 0, 0, client.cx, client.cy };
    using Fn = BOOL (WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static Fn fn = (Fn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi");
    if (fn) fn(&r, style, FALSE, 0, g.dpi);
    else    AdjustWindowRectEx(&r, style, FALSE, 0);
    return r;
}

void PaintPanel(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    SetBkMode(dc, TRANSPARENT);
    SetViewportOrgEx(dc, 0, -g.scrollY, nullptr);
    HGDIOBJ old = SelectObject(dc, g.bold);
    for (int i = 0; i < g.seats; ++i) {
        const RECT& h = g.ui[i].header;
        const int sw = Px(16), top = h.top + (h.bottom - h.top - sw) / 2;
        RECT swatch{ h.left, top, h.left + sw, top + sw };
        HBRUSH b = CreateSolidBrush(g.cur.seatColors[i]);
        FillRect(dc, &swatch, b);
        DeleteObject(b);
        FrameRect(dc, &swatch, GetSysColorBrush(COLOR_GRAYTEXT));
        const bool pad = i == g.padSeat;
        std::wstring title = SeatTitle(i);
        const bool absent = !(g.present & (1u << i));
        if (absent) title += S(pad ? sPadCut : sSeatAbsent);
        // The "Disable" box sits at the right end of the row: the title gives it room.
        RECT tr{ h.left + sw + Px(8), h.top, pad ? h.right : h.right - g.offW - Px(4), h.bottom };
        SetTextColor(dc, GetSysColor(absent || g.off[i] ? COLOR_GRAYTEXT : COLOR_WINDOWTEXT));
        DrawTextW(dc, title.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS |
                  (UiRtl() ? DT_RTLREADING : 0));
        RECT line{ h.left, h.bottom + Px(4), h.right, h.bottom + Px(5) };
        FillRect(dc, &line, GetSysColorBrush(COLOR_3DLIGHT));
    }
    SelectObject(dc, old);
    EndPaint(hwnd, &ps);
}

// The preview of a seat: the arrow on four backgrounds that hide pointers (white,
// dark, green, yellow) and four other shapes of the same set underneath.
struct PreviewCache {
    COLORREF color = 0;
    int      style = -1;
    PointerImage img[5];     // arrow, hand, text bar, resize, forbidden
    bool     ok[5] = {};
} g_preview[kMaxSeats];

void PaintPreview(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    SetLayout(dc, 0);   // a mirrored window would flip the pointers: the preview is drawn as it is
    int seat = 0, ctl = 0;
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int W = rc.right, H = rc.bottom;
    if (SeatOf(GetDlgCtrlID(hwnd), seat, ctl) && W > 0 && H > 0) {
        static const PointerShape kShapes[5] = { PointerShape::Arrow, PointerShape::Hand,
            PointerShape::IBeam, PointerShape::SizeNWSE, PointerShape::No };
        static const int kRow[2][4] = { { 0, 0, 0, 0 }, { 1, 2, 3, 4 } };
        static const COLORREF kBack[4] = { RGB(255, 255, 255), RGB(32, 32, 36),
                                           RGB(60, 170, 70),   RGB(250, 230, 60) };
        PreviewCache& pc = g_preview[seat];
        const COLORREF color = g.cur.seatColors[seat];
        const int style = g.cur.cursorStyle[seat];
        if (pc.style != style || pc.color != color) {
            pc.style = style; pc.color = color;
            for (int k = 0; k < 5; ++k) pc.ok[k] = RenderPointer(kShapes[k], color, style, pc.img[k]);
        }

        BITMAPINFO bi{};
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = W;
        bi.bmiHeader.biHeight      = -H;
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (dib && bits) {
            BYTE* px = (BYTE*)bits;
            for (int row = 0; row < 2; ++row) {
                for (int col = 0; col < 4; ++col) {
                    const int x0 = W * col / 4, x1 = W * (col + 1) / 4;
                    const int y0 = H * row / 2, y1 = H * (row + 1) / 2;
                    const COLORREF back = kBack[col];
                    for (int y = y0; y < y1; ++y)
                        for (int x = x0; x < x1; ++x) {
                            BYTE* d = px + ((size_t)y * W + x) * 4;
                            d[0] = GetBValue(back); d[1] = GetGValue(back); d[2] = GetRValue(back); d[3] = 255;
                        }
                    const int k = kRow[row][col];
                    if (!pc.ok[k]) continue;
                    const PointerImage& im = pc.img[k];
                    const int ox = (x0 + x1 - im.w) / 2, oy = (y0 + y1 - im.h) / 2;
                    for (int y = 0; y < im.h; ++y) {
                        const int dy = oy + y;
                        if (dy < y0 || dy >= y1) continue;
                        for (int x = 0; x < im.w; ++x) {
                            const int dx = ox + x;
                            if (dx < x0 || dx >= x1) continue;
                            const BYTE* sp = im.bgra.data() + ((size_t)y * im.w + x) * 4;
                            BYTE* d = px + ((size_t)dy * W + dx) * 4;
                            const int inv = 255 - sp[3];       // premultiplied source over
                            for (int c = 0; c < 3; ++c) d[c] = (BYTE)(sp[c] + d[c] * inv / 255);
                        }
                    }
                }
            }
            HDC mem = CreateCompatibleDC(dc);
            HGDIOBJ old = SelectObject(mem, dib);
            BitBlt(dc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteDC(mem);
        }
        if (dib) DeleteObject(dib);
        FrameRect(dc, &rc, GetSysColorBrush(COLOR_GRAYTEXT));
    }
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK PreviewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT) { PaintPreview(hwnd); return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// The panel only hosts the controls: it hands their notifications to the
// window, scrolls, and paints the seat headers.
LRESULT CALLBACK PanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
    case WM_HSCROLL:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        return SendMessageW(GetParent(hwnd), msg, wp, lp);
    case WM_VSCROLL:
        OnPanelScroll(wp);
        return 0;
    case WM_MOUSEWHEEL:
        ScrollTo(g.scrollY - (int)((short)HIWORD(wp) * Px(48) / WHEEL_DELTA));
        return 0;
    case WM_PAINT:
        PaintPanel(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// The general page only hosts its controls, like the panel does.
LRESULT CALLBACK PageProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        return SendMessageW(GetParent(hwnd), msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- events

bool SeatOf(int id, int& seat, int& ctl) {
    if (id < kIdBase || id >= kIdBase + g.seats * kPerSeat) return false;
    seat = (id - kIdBase) / kPerSeat;
    ctl  = (id - kIdBase) % kPerSeat;
    return true;
}

// Lays the window out again for the seats it now has, keeping it on its screen.
void FitWindow(HWND hwnd) {
    const SIZE client = Layout(hwnd);
    const RECT wr = WindowRectFor(client, (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE));
    const int w = wr.right - wr.left, h = wr.bottom - wr.top;
    RECT cur{};
    GetWindowRect(hwnd, &cur);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& wa = mi.rcWork;
    int x = cur.left, y = cur.top;
    if (x + w > wa.right)  x = wa.right - w;
    if (y + h > wa.bottom) y = wa.bottom - h;
    if (x < wa.left) x = wa.left;
    if (y < wa.top)  y = wa.top;
    SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

// The engine says which mice are plugged in. A mouse plugged in for the first
// time has a seat in the file by now: give it its section, without closing the
// window. A mouse unplugged keeps its section (its values are kept), marked.
void OnSeats(HWND hwnd, UINT present) {
    g.present = present;
    Config disk;
    if (!disk.Load(g.path)) { InvalidateRect(g.panel, nullptr, TRUE); return; }
    const int pad = TouchpadSeat(disk);
    const bool padMoved = pad != g.padSeat;
    g.padSeat = pad;
    if (disk.seatCount > g.seats) {
        const int old = g.seats;
        g.seats = disk.seatCount;
        EnsureSeats(g.opened, g.seats);
        EnsureSeats(g.cur, g.seats);
        for (int i = old; i < g.seats; ++i) {
            for (Config* c : { &g.opened, &g.cur }) {
                c->sensitivity[i] = disk.sensitivity[i];
                c->accel[i]       = disk.accel[i];
                c->screen[i]      = disk.screen[i];
                c->buttons[i]     = disk.buttons[i];
                c->seatColors[i]  = disk.seatColors[i];
                c->cursorStyle[i] = disk.cursorStyle[i];
            }
            g.off[i] = g.offOpened[i] = SeatSetAside(disk, i);
            CreateSeatControls(i);
        }
        RefreshBrands(disk);
        FitWindow(hwnd);
    } else if (padMoved) {
        for (int i = 0; i < g.seats; ++i) ShowSeat(i);
        RefreshBrands(disk);
        FitWindow(hwnd);
    } else {
        RefreshBrands(disk);   // a mouse plugged in or out: its make can now be read, or not
    }
    InvalidateRect(g.panel, nullptr, TRUE);
}

void OnSlider(HWND hwnd, HWND slider) {
    int seat, ctl;
    if (!SeatOf(GetDlgCtrlID(slider), seat, ctl) || ctl < cSlider || ctl >= cSlider + fFieldCount)
        return;
    const Field f = (Field)(ctl - cSlider);
    const double v = (double)SendMessageW(slider, TBM_GETPOS, 0, 0) / kRange[f].scale;
    if (std::fabs(v - Get(g.cur, seat, f)) < 1e-9) return;
    Put(g.cur, seat, f, v);
    SetWindowTextW(g.ui[seat].value[f], Format(f, v).c_str());
    ApplySoon(hwnd);
}

// The window shows one tab at a time and takes the height of that one.
void SelectTab(HWND hwnd, int tab) {
    if (tab < 0 || tab > 1) return;
    g.curTab = tab;
    if ((int)SendMessageW(g.tab, TCM_GETCURSEL, 0, 0) != tab) SendMessageW(g.tab, TCM_SETCURSEL, tab, 0);
    FitWindow(hwnd);
}

// The help page of the language in use: a fixed address of the table, nothing
// that comes from outside is ever put into it.
void OnHelp(HWND hwnd) {
    const wchar_t* url = LangData(UiLang()).helpUrl;
    if ((INT_PTR)ShellExecuteW(hwnd, L"open", url, nullptr, nullptr, SW_SHOWNORMAL) <= 32)
        SetStatus(S(sHelpFail));
}

// A new language: every text of the window, then the size that fits them.
void RefreshTexts(HWND hwnd) {
    ApplyDirection(hwnd);
    SetCommonTexts(hwnd);
    for (int i = 0; i < g.seats; ++i) {
        g.note[i].clear();
        SetSeatTexts(i);
        ShowSeat(i);
    }
    GateAutostart(g.autoState);
    RefreshTips();
    SetWindowTextW(g.status, L"");
    FitWindow(hwnd);
}

void OnLanguage(HWND hwnd) {
    const int sel = (int)SendMessageW(g.lang, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel > kLangCount) return;
    const int choice = sel - 1;           // 0 is "automatic" = kLangAuto
    if (choice == g.cur.language) return;
    g.cur.language = choice;
    SetLanguageChoice(choice);
    RefreshTexts(hwnd);
    Apply();                              // saved, and the engine switches its own texts
}

void OnCommand(HWND hwnd, int id) {
    if (id == kIdClose || id == IDCANCEL) { SendMessageW(hwnd, WM_CLOSE, 0, 0); return; }
    if (id == kIdAutostart) { OnAutostart(hwnd); return; }
    if (id == kIdHelp) { OnHelp(hwnd); return; }
    if (id == kIdUndo) {
        for (int i = 0; i < g.seats; ++i) {
            g.cur.sensitivity[i] = g.opened.sensitivity[i];
            g.cur.accel[i]       = g.opened.accel[i];
            g.cur.screen[i]      = g.opened.screen[i];
            g.cur.seatColors[i]  = g.opened.seatColors[i];
            g.cur.cursorStyle[i] = g.opened.cursorStyle[i];
            g.cur.buttons[i]     = g.opened.buttons[i];
            g.cur.touchpad       = g.opened.touchpad;
            g.off[i]             = g.offOpened[i];
            g.sel[i] = 0;
            g.note[i].clear();
            ShowSeat(i);
        }
        Apply();
        return;
    }
    int seat, ctl;
    if (!SeatOf(id, seat, ctl)) return;
    if (ctl == cOff) {
        OnDisable(seat);
    } else if (ctl == cAccel || ctl == cFixed) {
        const bool on = ctl == cAccel;
        if (g.cur.accel[seat].on != on) {
            g.cur.accel[seat].on = on;
            Apply();
        }
        ShowSeat(seat);
    } else if (ctl == cDefaults) {
        g.cur.sensitivity[seat] = 1.0;
        g.cur.accel[seat]       = Config::Accel{};
        g.cur.screen[seat].clear();
        g.cur.cursorStyle[seat] = (int)kStyleBright;
        if (seat == g.padSeat) g.cur.touchpad = Config::Touchpad::Auto;
        ShowSeat(seat);
        Apply();
    } else if (ctl == cLearn) {
        OnLearnClick(hwnd, seat);
    } else if (ctl == cForget) {
        OnForget(seat);
    } else if (ctl == cKeys) {
        OnKeysClick(seat);
    } else if (ctl == cWin) {
        OnWinToggle(seat);
    }
}

LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_HSCROLL:
        if (lp) OnSlider(hwnd, (HWND)lp);
        return 0;
    case WM_MOUSEWHEEL:
        return SendMessageW(g.panel, msg, wp, lp);
    case WM_COMMAND:
        if (HIWORD(wp) == BN_CLICKED) OnCommand(hwnd, LOWORD(wp));
        else if (HIWORD(wp) == STN_CLICKED && LOWORD(wp) == kIdSite)
            ShellExecuteW(hwnd, L"open", kSiteUrl, nullptr, nullptr, SW_SHOWNORMAL);
        else if (HIWORD(wp) == CBN_SELCHANGE && LOWORD(wp) == kIdLang)
            OnLanguage(hwnd);
        else if (HIWORD(wp) == CBN_SELCHANGE) {
            int seat, ctl;
            if (SeatOf(LOWORD(wp), seat, ctl)) {
                if (ctl == cScreen)      OnScreen(seat);
                else if (ctl == cColor)  OnColor(seat);
                else if (ctl == cStyle)  OnStyle(seat);
                else if (ctl == cList)   OnListSelect(seat);
                else if (ctl == cAction) OnActionSelect(seat);
                else if (ctl == cPad)    OnPadMode(seat);
            }
        }
        return 0;
    case WM_NOTIFY: {
        const NMHDR* nh = (const NMHDR*)lp;
        if (nh && nh->hwndFrom == g.tab && nh->code == TCN_SELCHANGE)
            SelectTab(hwnd, (int)SendMessageW(g.tab, TCM_GETCURSEL, 0, 0));
        return 0;
    }
    case kLearnedMsg:
        OnLearned(hwnd, (int)wp, lp);
        return 0;
    case kSeatsMsg:
        OnSeats(hwnd, (UINT)wp);
        return 0;
    case WM_TIMER:
        if (wp == kTimerAutostart) { PollAutostart(hwnd); return 0; }
        if (wp == kTimerLearn) {            // the engine never answered
            const int seat = g.learning;
            EndLearning(hwnd);
            if (seat >= 0 && seat < g.seats) Note(seat, S(sNothing));
            return 0;
        }
        KillTimer(hwnd, wp);
        if (g.pending) Apply();
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        // The window is white; the controls default to the dialog grey.
        HDC dc = (HDC)wp;
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        const HWND ctl = (HWND)lp;
        COLORREF text = GetSysColor(COLOR_WINDOWTEXT);
        if (ctl == g.hint || ctl == g.about) text = GetSysColor(COLOR_GRAYTEXT);
        if (ctl == g.site) text = GetSysColor(COLOR_HOTLIGHT);
        SetTextColor(dc, text);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    case WM_SETCURSOR:
        if ((HWND)wp == g.site) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_DPICHANGED: {
        g.dpi = HIWORD(wp);
        const SIZE client = Layout(hwnd);
        const RECT* suggested = (const RECT*)lp;
        const RECT wr = WindowRectFor(client, (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE));
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     wr.right - wr.left, wr.bottom - wr.top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_CLOSE:
        if (g.pending) Apply();
        if (g.autoHelper) { CloseHandle(g.autoHelper); g.autoHelper = nullptr; }
        if (g.learning >= 0) {              // stop the engine listening to a mouse nobody watches
            if (HWND engine = FindWindowW(kEngineClass, nullptr)) PostMessageW(engine, kLearnMsg, 0, 0);
            g.learning = -1;
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int RunSettings(HINSTANCE inst) {
    // One window at a time: a second request brings the first one forward.
    if (HWND open = FindWindowW(kSettingsClass, nullptr)) {
        ShowWindow(open, SW_RESTORE);
        SetForegroundWindow(open);
        return 0;
    }

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_TAB_CLASSES | ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);

    g.path = Config::DefaultPath();
    g.opened.Load(g.path);
    SetLanguageChoice(g.opened.language);
    g.seats = g.opened.seatCount;
    EnsureSeats(g.opened, g.seats);
    g.cur = g.opened;
    g.padSeat = TouchpadSeat(g.opened);
    g.screens = ListScreens();
    for (int i = 0; i < g.seats && i < kMaxSeats; ++i) g.off[i] = g.offOpened[i] = SeatSetAside(g.opened, i);
    RefreshBrands(g.opened);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = SettingsProc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon         = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(kAppIconResource), IMAGE_ICON,
                                         GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
                                         LR_SHARED);
    wc.hIconSm       = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(kAppIconResource), IMAGE_ICON,
                                         GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                         LR_SHARED);
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = kSettingsClass;
    RegisterClassExW(&wc);

    WNDCLASSEXW pc{};
    pc.cbSize        = sizeof(pc);
    pc.lpfnWndProc   = PanelProc;
    pc.hInstance     = inst;
    pc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    pc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    pc.lpszClassName = kPanelClass;
    RegisterClassExW(&pc);

    WNDCLASSEXW gc{};
    gc.cbSize        = sizeof(gc);
    gc.lpfnWndProc   = PageProc;
    gc.hInstance     = inst;
    gc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    gc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    gc.lpszClassName = kPageClass;
    RegisterClassExW(&gc);

    WNDCLASSEXW vc{};
    vc.cbSize        = sizeof(vc);
    vc.lpfnWndProc   = PreviewProc;
    vc.hInstance     = inst;
    vc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    vc.lpszClassName = kPreviewClass;
    RegisterClassExW(&vc);

    // Not resizable, never topmost: it must not stand in the way of anything.
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, kSettingsClass, S(sTitle), style,
                                CW_USEDEFAULT, CW_USEDEFAULT, 100, 100,
                                nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;
    g.self = hwnd;
    // The engine runs elevated and this window may not: let its answer through.
    ChangeWindowMessageFilterEx(hwnd, kLearnedMsg, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hwnd, kSeatsMsg, MSGFLT_ALLOW, nullptr);

    // Open on the screen where the pointer that asked for it is.
    POINT pt{};
    GetCursorPos(&pt);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    SetWindowPos(hwnd, nullptr, mi.rcWork.left, mi.rcWork.top, 100, 100,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    g.dpi = DpiOf(hwnd);

    CreateControls(hwnd, inst);
    ApplyDirection(hwnd);
    if (!g.panel || !g.page || !g.tab) return 1;
    const SIZE client = Layout(hwnd);
    const RECT wr = WindowRectFor(client, style);
    const int w = wr.right - wr.left, h = wr.bottom - wr.top;
    const RECT& wa = mi.rcWork;
    SetWindowPos(hwnd, nullptr, wa.left + ((wa.right - wa.left) - w) / 2,
                 wa.top + ((wa.bottom - wa.top) - h) / 2, w, h, SWP_NOZORDER);

    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd);
    // Ask which mice are plugged in; the answer comes back as kSeatsMsg.
    if (HWND engine = FindWindowW(kEngineClass, nullptr)) PostMessageW(engine, kSeatsMsg, 0, 0);
    LogLine(L"[settings] window opened for %d seats (%s)", g.seats, g.path.c_str());

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // Ctrl+Tab and Ctrl+Shift+Tab change tab from anywhere in the window.
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_TAB && (GetKeyState(VK_CONTROL) & 0x8000) &&
            (msg.hwnd == hwnd || IsChild(hwnd, msg.hwnd))) {
            SelectTab(hwnd, g.curTab == tabGeneral ? tabMice : tabGeneral);
            continue;
        }
        if (IsDialogMessageW(hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g.font) DeleteObject(g.font);
    if (g.bold) DeleteObject(g.bold);
    return 0;
}

} // namespace om
