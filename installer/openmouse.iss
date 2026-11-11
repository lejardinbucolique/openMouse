; openMouse - installer script (Inno Setup 6).
; SPDX-License-Identifier: MIT
;
; Build with build-installer.cmd (it builds openmouse.exe first, then runs ISCC).
; This file is saved as UTF-8 WITH a byte order mark: Inno Setup reads it as ANSI
; otherwise, and the Russian and Arabic strings below would be corrupted.
;
; Decisions this script carries (task OP10):
;  - the program goes to Program Files, a folder only administrators can change,
;    and the folder cannot be changed on the wizard: the "start with Windows"
;    task runs it with full rights, so it must never sit in a writable folder;
;  - "start with Windows" is unticked by default and is done by the program
;    itself (openmouse.exe --autostart=on), which re-checks the folder;
;  - uninstalling stops the engine, removes the logon task and gives the cursor
;    back (--restore-cursor) before any file is deleted;
;  - the settings file in %APPDATA%\openMouse is kept unless the user says no.

#ifndef MyVersion
  #define MyVersion "0.1.0"
#endif
#ifndef MyExe
  #define MyExe "..\openmouse.exe"
#endif

#define MyName "openMouse"

[Setup]
; Fixed identity of the product: an install over an older one is an upgrade.
AppId={{6F0C2B1E-4D7A-4C57-9B3E-0A5D8E2C71F4}
AppName={#MyName}
AppVersion={#MyVersion}
AppPublisher=openMouse (original author: Alston Mendonca, MIT licence)
AppComments=Multi-pointer input for Windows: one on-screen pointer per mouse.
VersionInfoVersion=0.1.0.0
DefaultDirName={autopf}\{#MyName}
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir=..\dist
OutputBaseFilename=openMouse-setup-{#MyVersion}
SetupIconFile=..\res\openmouse.ico
UninstallDisplayIcon={app}\openmouse.exe
UninstallDisplayName={#MyName}
InfoBeforeFile=..\LICENSE
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ShowLanguageDialog=auto
LanguageDetectionMethod=uilanguage
CloseApplications=yes
RestartApplications=no

; English is first: it is the fallback for any system language not listed, and
; Simplified Chinese (no official Inno Setup file) falls back to it.
[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "french";  MessagesFile: "compiler:Languages\French.isl"
Name: "german";  MessagesFile: "compiler:Languages\German.isl"
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"
Name: "arabic";  MessagesFile: "compiler:Languages\Arabic.isl"

[CustomMessages]
english.AutostartTask=Start openMouse when I log on to Windows
english.AutostartFailed=The start-with-Windows task could not be created. You can try again from the openMouse settings window.
english.SettingsShortcut=settings
english.KeepSettings=Do you also want to delete your openMouse settings (mouse bindings, speeds, colours)?
french.AutostartTask=Lancer openMouse au démarrage de Windows
french.AutostartFailed=La tâche de démarrage avec Windows n'a pas pu être créée. Vous pouvez réessayer depuis la fenêtre de réglages d'openMouse.
french.SettingsShortcut=réglages
french.KeepSettings=Voulez-vous aussi supprimer vos réglages d'openMouse (souris associées, vitesses, couleurs) ?
german.AutostartTask=openMouse beim Anmelden an Windows starten
german.AutostartFailed=Die Aufgabe für den Start mit Windows konnte nicht erstellt werden. Sie können es im openMouse-Einstellungsfenster erneut versuchen.
german.SettingsShortcut=Einstellungen
german.KeepSettings=Möchten Sie auch Ihre openMouse-Einstellungen löschen (Mauszuordnungen, Geschwindigkeiten, Farben)?
spanish.AutostartTask=Iniciar openMouse al iniciar sesión en Windows
spanish.AutostartFailed=No se pudo crear la tarea de inicio con Windows. Puede volver a intentarlo desde la ventana de configuración de openMouse.
spanish.SettingsShortcut=configuración
spanish.KeepSettings=¿Desea eliminar también su configuración de openMouse (ratones asignados, velocidades, colores)?
russian.AutostartTask=Запускать openMouse при входе в Windows
russian.AutostartFailed=Не удалось создать задачу автозапуска вместе с Windows. Можно повторить попытку в окне параметров openMouse.
russian.SettingsShortcut=параметры
russian.KeepSettings=Удалить также ваши параметры openMouse (привязки мышей, скорости, цвета)?
arabic.AutostartTask=تشغيل openMouse عند تسجيل الدخول إلى Windows
arabic.AutostartFailed=تعذر إنشاء مهمة التشغيل مع Windows. يمكنك إعادة المحاولة من نافذة إعدادات openMouse.
arabic.SettingsShortcut=الإعدادات
arabic.KeepSettings=هل تريد حذف إعدادات openMouse أيضاً (ربط الماوسات والسرعات والألوان)؟

[Tasks]
Name: "autostart"; Description: "{cm:AutostartTask}"; Flags: unchecked

[Files]
Source: "{#MyExe}"; DestDir: "{app}"; DestName: "openmouse.exe"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\docs\*.md"; DestDir: "{app}\docs"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#MyName}"; Filename: "{app}\openmouse.exe"
Name: "{autoprograms}\{#MyName} ({cm:SettingsShortcut})"; Filename: "{app}\openmouse.exe"; Parameters: "--settings"
Name: "{autoprograms}\{cm:UninstallProgram,{#MyName}}"; Filename: "{uninstallexe}"

[Run]
; Started as the person who ran the installer, not as the administrator account
; the prompt may have switched to: the engine asks for its own elevation.
Filename: "{app}\openmouse.exe"; Description: "{cm:LaunchProgram,{#MyName}}"; Flags: nowait postinstall skipifsilent runasoriginaluser

[Code]
const
  WM_CLOSE = $0010;
  EngineClass = 'openMouse.Engine';
  SettingsClass = 'openMouse.Settings';

function FindWindowByClass(ClassName: String; WindowName: Integer): HWND;
  external 'FindWindowW@user32.dll stdcall';

// Ask a window of the given class to close; false when there is none.
function CloseWindowOfClass(ClassName: String): Boolean;
var
  W: HWND;
begin
  W := FindWindowByClass(ClassName, 0);
  Result := W <> 0;
  if Result then
    PostMessage(W, WM_CLOSE, 0, 0);
end;

// Stops a running engine the polite way: WM_CLOSE makes it free the capture and
// show the cursor again. Waits up to ten seconds for it to go.
procedure StopEngine;
var
  I: Integer;
begin
  CloseWindowOfClass(SettingsClass);
  if not CloseWindowOfClass(EngineClass) then
    Exit;
  for I := 1 to 50 do
  begin
    if FindWindowByClass(EngineClass, 0) = 0 then
      Break;
    Sleep(200);
  end;
  // The janitor process watching the engine puts the cursor back, then leaves.
  Sleep(800);
end;

// Runs the installed program with arguments, hidden, and waits for it.
function RunOpenMouse(Params: String): Integer;
var
  Code: Integer;
begin
  if not Exec(ExpandConstant('{app}\openmouse.exe'), Params, '', SW_HIDE, ewWaitUntilTerminated, Code) then
    Code := -1;
  Result := Code;
end;

// Install over an older version: the engine must let go of the file first.
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  StopEngine;
  Result := '';
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and WizardIsTaskSelected('autostart') then
    // The program creates the task itself and refuses a writable folder.
    if RunOpenMouse('--autostart=on') <> 0 then
      SuppressibleMsgBox(ExpandConstant('{cm:AutostartFailed}'), mbError, MB_OK, IDOK);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    StopEngine;
    RunOpenMouse('--autostart=off');   // removes the "openMouse" logon task
    RunOpenMouse('--restore-cursor');  // the pointer is always given back
  end;
  if CurUninstallStep = usPostUninstall then
  begin
    // Kept by default: the bindings are measured by hand and cost a few minutes.
    if SuppressibleMsgBox(ExpandConstant('{cm:KeepSettings}'), mbConfirmation, MB_YESNO or MB_DEFBUTTON2, IDNO) = IDYES then
      DelTree(ExpandConstant('{userappdata}\openMouse'), True, True, True);
  end;
end;
