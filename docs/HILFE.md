# openMouse: Anleitung

*[English version](HELP.md) · [Version française](AIDE.md) · [Versión en español](AYUDA.md) · [Русская версия](SPRAVKA.md) · [简体中文版](BANGZHU.md)*

> Diese Übersetzung wurde mit Hilfe einer KI erstellt und von keiner Person, die Deutsch
> spricht, gegengelesen. Korrekturen sind willkommen: Bitte melden Sie sie als Ticket im
> [GitHub-Repository](https://github.com/lejardinbucolique/openMouse/issues).

openMouse gibt jeder angeschlossenen Maus ihren eigenen Zeiger auf demselben
Windows-Desktop. So können zwei Personen gleichzeitig an einem PC arbeiten, jede mit ihrer
eigenen Maus: Die eine klickt hier, während die andere dort eine Seite scrollt.

## Vorab

- openMouse ersetzt den Windows-Pfeil durch einen farbigen Zeiger pro Maus. Standardmäßig
  ist der erste gelb, der zweite orange.
- Es tippt immer nur eine Person: Windows schickt Tastatureingaben nur an das aktive
  Fenster. Die Mäuse sind unabhängig, die Tastaturen nicht.
- Sie brauchen 64-Bit-Windows 10 oder 11 und mindestens zwei Mäuse (das Touchpad eines
  Laptops zählt als eine).

## 1. Starten

1. Führen Sie `openmouse.exe` aus. Es gibt kein Installationsprogramm.
2. Windows fragt nach der Administratorberechtigung: Bestätigen Sie sie. Ohne sie
   funktioniert openMouse trotzdem, aber „als Administrator“ gestartete Fenster (zum
   Beispiel der Task-Manager) entziehen sich den Mäusen.
3. Neben der Uhr erscheint ein Symbol, und zwei Zeiger in verschiedenen Farben sind auf dem
   Bildschirm zu sehen.

Um es mit Windows zu starten: Klicken Sie auf das Symbol, dann auf **Einstellungen…**, auf
die Registerkarte **Allgemein** und aktivieren Sie **openMouse mit Windows starten**. Das
ist erst möglich, wenn openMouse in einem geschützten Ordner wie `Programme` installiert
ist, weil das Programm dann mit vollen Rechten startet. Aus einem anderen Ordner (Downloads,
Desktop…) ist das Kontrollkästchen abgegraut und weist darauf hin.

## 2. Zwei Mäuse anschließen

Schließen Sie einfach die zweite Maus an: Ein neuer Zeiger in einer anderen Farbe erscheint.
Wird sie getrennt, verschwindet ihr Zeiger; wird sie wieder angeschlossen, kehrt er dorthin
zurück, wo er stehen geblieben war. Der letzte Zeiger wird nie ausgeblendet, damit Sie nie
ohne Zeiger dastehen.

Jede Maus behält ihre Farbe, auch an einem anderen USB-Anschluss.

Sind zwei Zeiger vertauscht oder steuert eine Maus den falschen Zeiger (das kann bei
Funkempfängern vorkommen), ziehen Sie den Empfänger dieser Maus ab und stecken ihn wieder
an. Öffnen Sie danach **Einstellungen…**, um die erkannten Mäuse zu sehen.

## 3. Mit beiden Mäusen arbeiten

- **Bewegen**: Jede Maus bewegt ihren eigenen Zeiger, ohne den anderen zu stören.
- **Klicken**: Der Klick landet dort, wohin die klickende Maus zeigt. Ein Klick in ein
  Fenster, das nicht aktiv ist, aktiviert es und klickt in einem Zug.
- **Scrollen**: Das Mausrad scrollt das Fenster unter dem eigenen Zeiger, ganz ohne
  vorherigen Klick.
- **Ziehen** (eine Datei verschieben, markieren): Solange eine Person eine Taste gedrückt
  hält, warten die Klicks der anderen, bis die Geste beendet ist. Die Zeiger bleiben frei
  beweglich.
- **Zurück- und Vorwärts-Tasten** der Maus funktionieren ebenfalls, jeweils für die eigene
  Maus.

## 4. Einstellungen

Klicken Sie auf das Symbol neben der Uhr, dann auf **Einstellungen…**. Das Fenster hat
zwei Registerkarten (mit **Strg + Tab** wechseln Sie zwischen ihnen). Jede Änderung wird
sofort übernommen und gespeichert; **Meine Änderungen rückgängig** stellt die Mauswerte
wieder her, mit denen das Fenster geöffnet wurde. Diese beiden Schaltflächen und die
Statuszeile („Übernommen und gespeichert.“) befinden sich unten und gelten für beide
Registerkarten.

### Registerkarte „Allgemein“

- **openMouse mit Windows starten**: siehe „Starten“ oben.
- **Sprache**: „Automatisch (Windows-Sprache)“, „Français“, „English“, „Deutsch“, „Español“, „Русский“ oder „简体中文“. Die
  Auswahl gilt sofort für das Fenster, das Menü des Symbols und die Meldungen von
  openMouse und bleibt gespeichert.
- **Hilfe**: öffnet diese Anleitung in der gewählten Sprache auf GitHub.
- **Info**: die Danksagungen, mit dem Link zur Calepitool-Website.

### Registerkarte „Mäuse“

Ein Bereich pro Maus und einer für ein Laptop-Touchpad. Wenn Sie mit dem Mauszeiger über
**Feste Empfindlichkeit**, **Progressive Beschleunigung** oder **Taste anlernen** fahren,
erscheint ein kurzer Hinweistext.

### Eine Maus erkennen und beiseitelegen

Der Titel jedes Bereichs nennt die Marke der Maus („Logitech-Maus“), aus der Maus selbst
gelesen oder, falls das nicht geht, vom Hersteller. Lässt sich die Marke nicht lesen, lautet
der Titel „Maus“ gefolgt von einer Nummer; zwei Mäuse derselben Marke tragen ebenfalls
ihre Nummer.

Das Kontrollkästchen **Deaktivieren** rechts neben dem Titel legt die Maus beiseite, ohne
sie abzuziehen: Ihr Zeiger verschwindet, und nichts, was sie tut, wird berücksichtigt, und
zwar sofort. Ist das Kästchen wieder deaktiviert, kehrt sie dorthin zurück, wo sie
stehen geblieben war, mit ihrer Farbe und Geschwindigkeit. Die letzte funktionierende Maus
lässt sich nicht deaktivieren (das Touchpad zählt, solange es in Gebrauch ist): openMouse
lehnt das ab, damit Sie nie ohne Zeiger dastehen.

### Aussehen des Zeigers

- **Farbe**: zehn Farben, damit Sie Ihren Zeiger auf einen Blick erkennen.
- **Zeigersatz**: klassisch, heller Rand (am besten sichtbar, die Voreinstellung), weißer
  Körper mit farbigem Rand, groß oder weicher Schein. Die Vorschau zeigt das Ergebnis auf
  vier Hintergründen.

### Zeigergeschwindigkeit

Jede Maus folgt einem von zwei Modi (die Regler des anderen Modus sind abgegraut):

- **Feste Empfindlichkeit**: Jede Bewegung wird mit demselben Wert multipliziert. Der
  einfachste Modus: Erhöhen Sie den Wert, wenn der Zeiger zu langsam ist.
- **Progressive Beschleunigung**: langsam bei kleinen Bewegungen (Präzision), schnell bei
  großen (Bildschirm durchqueren). Die **Beschleunigungsschwelle** legt fest, ab wann
  beschleunigt wird.

**Standardwerte** stellt die ursprünglichen Einstellungen wieder her.

### Im Bildschirm bleiben

Für jede Maus können Sie den Zeiger auf einen Bildschirm beschränken (praktisch, wenn jede
Person an ihrem eigenen Monitor arbeitet) oder ihn über den gesamten Desktop wandern lassen.

### Taste anlernen

Manche Mäuse haben Tasten, die Windows nicht kennt. Klicken Sie auf **Taste anlernen** und
drücken Sie die Taste innerhalb von acht Sekunden. Wählen Sie dann, was sie bewirken soll:
einen Klick (links, rechts, Mitte, zurück, vorwärts), eine Tastenkombination oder nichts.
Der Klick erfolgt an der Position des Zeigers genau dieser Maus. **Vergessen** entfernt die
ausgewählte Taste.

Manche Tasten lassen sich nicht anlernen: Tasten, die nur die Software des Herstellers
lesen kann, senden nichts an Windows. Das Fenster weist Sie darauf hin.

## 5. Wenn etwas nicht funktioniert

| Problem | Was tun |
|---|---|
| Kein Zeiger, oder der Zeiger bleibt versteckt | Drücken Sie **Strg + Alt + Umschalt + Q**: openMouse beendet sich und gibt den Windows-Zeiger zurück. Andernfalls führen Sie `openmouse.exe --restore-cursor` aus. |
| Nach Strg + Alt + Entf oder dem Sperren der Sitzung | Normal: Windows übernimmt. Bei der Rückkehr setzt openMouse von selbst fort. |
| Ein Administratorfenster ignoriert die Mäuse | Starten Sie openMouse neu und bestätigen Sie die Administratorberechtigung. |
| Ein Zeiger folgt der falschen Maus | Stecken Sie die Maus neu ein und prüfen Sie dann die erkannten Mäuse unter **Einstellungen…**. |
| Alles andere | **Protokollordner öffnen** im Menü des Symbols: `openmouse.log` zeigt, was passiert ist. |

## 6. Bekannte Einschränkungen

- **Immer nur eine aktive Tastatur.** Zwei Personen können nicht gleichzeitig in zwei
  verschiedene Fenster tippen.
- **Taskleiste, Startmenü und Benachrichtigungen.** Dort ist nur der Zeiger der Maus
  sichtbar, die den echten Windows-Zeiger gerade „hält“. Der andere Zeiger verschwindet
  dahinter: Um sie zu bedienen, klicken Sie zuerst mit dieser Maus in ein gewöhnliches
  Fenster, oder überlassen Sie es der anderen Person.
- **Ein Ziehen, das direkt in einem inaktiven Fenster beginnt**, geht verloren: Klicken Sie
  einmal, um das Fenster zu aktivieren, und ziehen Sie dann.
- **Gleichzeitiges Scrollen**: Scrollen beide Personen im selben Moment, kann ein
  Mausradschritt gelegentlich im anderen Fenster landen.
- Ton, Zwischenablage und Fensterreihenfolge bleiben für beide Personen gemeinsam.

Technische Einzelheiten (Kompilieren, Befehlszeilenoptionen, Konfigurationsdatei) finden
Sie in der [README](../README.md).
