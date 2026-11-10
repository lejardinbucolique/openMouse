// openMouse — interface language
// SPDX-License-Identifier: MIT
//
// Which language the windows and messages speak, and the messages of the
// engine. The settings window keeps its own, much larger, tables
// (settings.cpp) but asks this file which table to read.
//
// The text is Unicode throughout and the fonts come from Windows, so languages
// the Latin alphabet does not cover (Russian, Chinese, Arabic) only need their
// tables. Nothing is mirrored for right-to-left languages yet: UiRtl() is the
// one place that will tell the layout to.
#include "openmouse.h"
#include <atomic>

namespace om {

namespace {

constexpr wchar_t kHelpEn[] = L"https://github.com/lejardinbucolique/openMouse/blob/main/docs/HELP.md";
constexpr wchar_t kHelpFr[] = L"https://github.com/lejardinbucolique/openMouse/blob/main/docs/AIDE.md";
constexpr wchar_t kHelpDe[] = L"https://github.com/lejardinbucolique/openMouse/blob/main/docs/HILFE.md";
constexpr wchar_t kHelpEs[] = L"https://github.com/lejardinbucolique/openMouse/blob/main/docs/AYUDA.md";
constexpr wchar_t kHelpRu[] = L"https://github.com/lejardinbucolique/openMouse/blob/main/docs/SPRAVKA.md";
constexpr wchar_t kHelpZh[] = L"https://github.com/lejardinbucolique/openMouse/blob/main/docs/BANGZHU.md";

// In the order of the Lang enum.
const LangInfo kLangs[kLangCount] = {
    { L"en", L"English",  LANG_ENGLISH, false, L'.', kHelpEn },
    { L"fr", L"Français", LANG_FRENCH,  false, L',', kHelpFr },
    { L"de", L"Deutsch",  LANG_GERMAN,  false, L',', kHelpDe },
    { L"es", L"Español",  LANG_SPANISH, false, L',', kHelpEs },
    { L"ru", L"Русский",  LANG_RUSSIAN, false, L',', kHelpRu },
    { L"zh-CN", L"简体中文", LANG_CHINESE, false, L'.', kHelpZh },
};

std::atomic<int> g_choice{ kLangAuto };

const wchar_t* const kMsgEn[(int)Msg::Count] = {
    L"openMouse — click for settings or to stop (panic: Ctrl+Alt+Shift+Q)",
    L"openMouse without administrator rights",
    L"Task Manager and windows started as administrator will escape the mice. Start openMouse again and accept the permission prompt.",
    L"openMouse — %d seats%s",
    L", keyboards steered",
    L"Settings…",
    L"Open log folder",
    L"Stop openMouse\tCtrl+Alt+Shift+Q",
    L"Left button",
    L"Right button",
    L"Middle button",
    L"Back button (4)",
    L"Forward button (5)",
    L"Special button (0x%X)",
    L"Mouse key \"%s\"",
    L"Media key",
    L"Manufacturer button",
    L"System control",
};

const wchar_t* const kMsgFr[(int)Msg::Count] = {
    L"openMouse : clic pour les réglages ou l'arrêt (urgence : Ctrl+Alt+Maj+Q)",
    L"openMouse sans droits d'administrateur",
    L"Le Gestionnaire des tâches et les fenêtres lancées en administrateur échapperont aux souris. Relancez openMouse en acceptant la demande d'autorisation.",
    L"openMouse : %d souris%s",
    L", claviers dirigés",
    L"Réglages…",
    L"Ouvrir le dossier du journal",
    L"Arrêter openMouse\tCtrl+Alt+Maj+Q",
    L"Bouton gauche",
    L"Bouton droit",
    L"Bouton du milieu",
    L"Bouton précédent (4)",
    L"Bouton suivant (5)",
    L"Bouton spécial (0x%X)",
    L"Touche « %s » de la souris",
    L"Touche multimédia",
    L"Bouton du fabricant",
    L"Commande système",
};

const wchar_t* const kMsgDe[(int)Msg::Count] = {
    L"openMouse – Klicken für die Einstellungen oder zum Beenden (Notfall: Strg+Alt+Umschalt+Q)",
    L"openMouse ohne Administratorrechte",
    L"Der Task-Manager und als Administrator gestartete Fenster entziehen sich den Mäusen. Starten Sie openMouse erneut und bestätigen Sie die Berechtigungsabfrage.",
    L"openMouse – Mäuse: %d%s",
    L", Tastaturen gesteuert",
    L"Einstellungen…",
    L"Protokollordner öffnen",
    L"openMouse beenden	Strg+Alt+Umschalt+Q",
    L"Linke Taste",
    L"Rechte Taste",
    L"Mittlere Taste",
    L"Zurück-Taste (4)",
    L"Vorwärts-Taste (5)",
    L"Sondertaste (0x%X)",
    L"Maustaste „%s“",
    L"Multimedia-Taste",
    L"Herstellertaste",
    L"Systembefehl",
};

const wchar_t* const kMsgEs[(int)Msg::Count] = {
    L"openMouse: haga clic para abrir la configuración o salir (emergencia: Ctrl+Alt+Mayús+Q)",
    L"openMouse sin permisos de administrador",
    L"El Administrador de tareas y las ventanas iniciadas como administrador escaparán al control de los ratones. Vuelva a iniciar openMouse y acepte la solicitud de permiso.",
    L"openMouse: %d ratones%s",
    L", teclados dirigidos",
    L"Configuración…",
    L"Abrir la carpeta del registro",
    L"Salir de openMouse	Ctrl+Alt+Mayús+Q",
    L"Botón izquierdo",
    L"Botón derecho",
    L"Botón central",
    L"Botón Atrás (4)",
    L"Botón Adelante (5)",
    L"Botón especial (0x%X)",
    L"Tecla «%s» del ratón",
    L"Tecla multimedia",
    L"Botón del fabricante",
    L"Control del sistema",
};

const wchar_t* const kMsgRu[(int)Msg::Count] = {
    L"openMouse: щёлкните, чтобы открыть параметры или выйти (экстренно: Ctrl+Alt+Shift+Q)",
    L"openMouse без прав администратора",
    L"Диспетчер задач и окна, запущенные от имени администратора, выйдут из-под управления мышей. Запустите openMouse снова и подтвердите запрос разрешения.",
    L"openMouse: мышей — %d%s",
    L", клавиатуры управляются",
    L"Параметры…",
    L"Открыть папку журнала",
    L"Выйти из openMouse	Ctrl+Alt+Shift+Q",
    L"Левая кнопка",
    L"Правая кнопка",
    L"Средняя кнопка",
    L"Кнопка «Назад» (4)",
    L"Кнопка «Вперёд» (5)",
    L"Особая кнопка (0x%X)",
    L"Клавиша мыши «%s»",
    L"Мультимедийная клавиша",
    L"Кнопка производителя",
    L"Системная команда",
};

const wchar_t* const kMsgZh[(int)Msg::Count] = {
    L"openMouse：点击打开设置或退出（紧急退出：Ctrl+Alt+Shift+Q）",
    L"openMouse 未以管理员身份运行",
    L"任务管理器和以管理员身份启动的窗口将不受鼠标控制。请重新启动 openMouse，并在权限提示中选择“是”。",
    L"openMouse：%d 个鼠标%s",
    L"，键盘已受控",
    L"设置…",
    L"打开日志文件夹",
    L"退出 openMouse	Ctrl+Alt+Shift+Q",
    L"左键",
    L"右键",
    L"中键",
    L"后退键 (4)",
    L"前进键 (5)",
    L"特殊按键 (0x%X)",
    L"鼠标按键“%s”",
    L"多媒体键",
    L"厂商专用按键",
    L"系统控制",
};

// In the order of the Lang enum.
const wchar_t* const* const kMsgs[kLangCount] = { kMsgEn, kMsgFr, kMsgDe, kMsgEs, kMsgRu, kMsgZh };

} // namespace

const LangInfo& LangData(Lang l) {
    const int i = (int)l;
    return kLangs[i >= 0 && i < kLangCount ? i : 0];
}

int LanguageFromCode(const std::wstring& code) {
    for (int i = 0; i < kLangCount; ++i)
        if (_wcsicmp(code.c_str(), kLangs[i].code) == 0) return i;
    return kLangAuto;
}

const wchar_t* LanguageCode(int choice) {
    return (choice >= 0 && choice < kLangCount) ? kLangs[choice].code : L"auto";
}

void SetLanguageChoice(int choice) {
    g_choice = (choice >= 0 && choice < kLangCount) ? choice : kLangAuto;
}

int LanguageChoice() { return g_choice; }

Lang UiLang() {
    const int c = g_choice;
    if (c >= 0 && c < kLangCount) return (Lang)c;
    const WORD primary = PRIMARYLANGID(GetUserDefaultUILanguage());
    for (int i = 0; i < kLangCount; ++i)
        if (kLangs[i].primary == primary) return (Lang)i;
    return Lang::En;
}

bool UiRtl() { return LangData(UiLang()).rtl; }

const wchar_t* T(Msg m) {
    const int i = (int)m;
    if (i < 0 || i >= (int)Msg::Count) return L"";
    const wchar_t* s = kMsgs[(int)UiLang()][i];
    return (s && *s) ? s : kMsgEn[i];
}

} // namespace om
