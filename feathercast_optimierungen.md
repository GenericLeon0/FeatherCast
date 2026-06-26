# FeatherCast App-Optimierungsstrategie

Dieser Bericht konsolidiert die Ergebnisse der drei spezialisierten Subagents (**UX/UI-Design**, **Interaction/Animation** und **Feature/Functionality**). Er dient als umfassender Fahrplan, um FeatherCast von einem einfachen App-Launcher in ein hochmodernes, flüssiges und funktionales Premium-Produktivitätstool (im Stile von *Raycast* oder *PowerToys Run*) zu verwandeln.

---

## 🚀 1. UX/UI-Design & Visuelles Redesign

### Farbpalette & Kontraste (Obsidian-Thema)
Das aktuelle dunkle Farbschema basiert auf einem warmen, rötlich-violetten Unterton (`#212126`), der kontrastarm wirkt. Wir schlagen den Wechsel zu einer kühleren, professionellen **Obsidian-Palette** vor:

*   **Hintergrund-Overlay (`overlayBackground`):** `#101012D9` (85 % transparentes, tiefes Obsidian-Schwarz für transluzente Overlays).
*   **Settings-Hintergrund (`settingsBackground`):** `#141416FA` (Nahezu opakes, dunkles Anthrazit).
*   **Karten/Flächen (`surface`):** `#18181B` (Modernes Schiefergrau analog zu Tailwind Zinc-900).
*   **Selektionsbalken (Zeilen-Fokus):** Der aktuelle Selektionsbalken mischt die Windows-Akzentfarbe mit 22 % Opazität, was bei grellen Akzentfarben unruhig wirkt und Text schwer lesbar macht.
    *   *Empfehlung:* Reduzierung der Akzent-Opazität auf **8 % - 10 %** oder Verwendung eines edlen transluzenten Weißwerts (`#FFFFFF14`).

### Typografie-Modernisierung
*   **Schriftart-Stack:** Wechsel der Standard-Schriftart unter Windows 11 von `Segoe UI` auf **Segoe UI Variable** (bessere optische Skalierung für Display- und Textgrößen):
    ```cpp
    L"Segoe UI Variable Text, Segoe UI Variable, Inter, Segoe UI"
    ```
*   **Suchleisten-Eingabe:** Reduzierung von `19.0f` auf `18.0f` bei `REGULAR` Gewichtung für mehr Eleganz.
*   **Ergebnis-Name:** `14.0f` bei `DWRITE_FONT_WEIGHT_MEDIUM` (500) oder `SEMI_BOLD` (600) für stärkere Abgrenzung zum Untertitel.
*   **Sektionstitel (z. B. "Dienste"):** `10.0f` bei `DWRITE_FONT_WEIGHT_SEMI_BOLD` in Großbuchstaben mit zusätzlichem DirectWrite-Zeichenabstand (Tracking).

### Abstände, Radien & Ausrichtung (Alignment)
*   **Text-Jitter beheben:** Die horizontale Startposition von Zeilentexten springt aktuell zwischen `left + 50` (mit Icons) und `left + 56` (mit Emojis). Dieser Versatz muss auf einheitlich **`rowRect.left + 52px`** normiert werden.
*   **Eckenradien (Inner vs. Outer):** Bei einem Overlay-Radius von `14.0f` and einem Abstand der Zeilen zum Rand von `8px` muss der innere Zeilenradius mathematisch korrekt **`6.0f`** betragen (`Innerer Radius = Äußerer Radius - Abstand`), um gequetschte Ecken zu vermeiden.
*   **Zeilen-Padding:** Die Zeilenhöhe wird von `46px` auf **`50px`** oder **`52px`** erhöht, damit Untertitel nicht direkt am unteren Rand der Zeile anstoßen.
*   **Vertikale Suchleisten-Achse:** Alle vertikalen Offsets (Lupe, Texteingabe, Caret, Einstellungen-Zahnrad) werden auf ein gemeinsames Zentrum von exakt `y = 30.0f` (bei 60px Gesamthöhe) ausgerichtet.

### Windows 11 Mica & Acrylic Backdrop Integration
Nutzung der nativen Windows 11 DWM-Schnittstellen für transluzente Materialien:
```cpp
#include <dwmapi.h>

enum DWM_SYSTEMBACKDROP_TYPE {
    DWMSBT_AUTO = 0,
    DWMSBT_NONE = 1,
    DWMSBT_MAINWINDOW = 2,      // Mica (perfekt für Settings)
    DWMSBT_TRANSIENTWINDOW = 3, // Acrylic (perfekt für das Launcher-Overlay)
    DWMSBT_TABBEDWINDOW = 4
};

#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif

void ApplyModernBackdrop(HWND hwnd, DWM_SYSTEMBACKDROP_TYPE type) {
    DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &type, sizeof(type));
}
```

---

## 🎬 2. Interaktionsdesign & Animationen

### Panel-Easing beim Öffnen (`ComputePanelAnim`)
Das aktuelle kubische Ease-Out fühlt sich bei $150\text{ ms}$ sehr abrupt an. Wir empfehlen die Umstellung auf **Ease-Out Back (Overshoot)** für ein organischeres Gefühl der Trägheit:

$$\text{Formel (mit } s = 1.2 \text{): } f(t) = 1.0 + (s + 1.0) \cdot (t - 1.0)^3 + s \cdot (t - 1.0)^2$$

*   **Dauer:** Erhöhung von $150\text{ ms}$ auf **$200\text{ ms}$**.
*   **Start-Skalierung:** $0.97$.
*   **Rückfederungs-Verhalten (C++):**
```cpp
double ComputeEaseOutBack(double t, double s = 1.2) {
    t = t - 1.0;
    return 1.0 + (s + 1.0) * std::pow(t, 3.0) + s * std::pow(t, 2.0);
}
```

### Zeilen-Stagger-Animation (`ComputeRowAnim`)
*   **Mono-direktionale Bewegung:** Zeilen sollten nicht mehr quer (horizontal) einfliegen, während das Hauptfenster vertikal nach oben fährt. Die Verschiebung der Zeilen wird auf eine reine **vertikale Aufwärtsbewegung** (Y-Achse) oder ein reines **Fade-In** umgestellt.
*   **Slide-Weg reduzieren:** Reduzierung von `kRowSlidePx` von $26\text{ px}$ auf **$6\text{ px} - 8\text{ px}$**.
*   **Timing straffen:** Reduzierung der Zeilendauer auf $120\text{ ms}$ und des Staggers auf $18\text{ ms}$ (Gesamtanimation abgeschlossen nach ca. $250\text{ ms}$).

### Die "Sliding Selection Pill"
Statt sprunghaften Farbwechseln bei Zeilen-Fokus-Wechseln zeichnen wir ein einziges abgerundetes Rechteck im Hintergrund der aktiven Zeile, dessen Y-Koordinate über einen Delta-Time-unabhängigen Lerp weich an die Zielposition fährt:

```cpp
void UpdateSelectionAnimation(float deltaTime) {
    const float targetY = resultsTop + selected_ * 48.0f; // 48px Zeilenabstand
    if (visualSelectedY_ < 0.0f) {
        visualSelectedY_ = targetY;
        return;
    }
    const float dy = targetY - visualSelectedY_;
    if (std::abs(dy) > 0.5f) {
        visualSelectedY_ += dy * (1.0f - std::pow(0.001f, deltaTime));
        animatingSelection_ = true;
    } else {
        visualSelectedY_ = targetY;
        animatingSelection_ = false;
    }
}
```

### Framerate- & Paint-Kopplung (VSync & QPC)
1.  **Zeitmessung:** Nutzung von `QueryPerformanceCounter` statt `GetTickCount64()` für mikrosekundengenaue Animationen.
2.  **Paint-Bypass:** Nach der Berechnung des Animation-Ticks wird sofort `UpdateWindow(hwnd_)` (bzw. `RedrawWindow` mit `RDW_UPDATENOW`) aufgerufen, was die Win32-Message-Queue umgeht und den Frame sofort zeichnet.
3.  **DWM-Kopplung:** Während einer Animation wird `DwmFlush()` in einer Thread-Schleife aufgerufen, um sich exakt an die native Bildwiederholrate des Monitors (60 Hz, 120 Hz, 144 Hz) zu binden.

---

## 🛠️ 3. Features & funktionale Komponenten

### Rechner- & Konverter-Erweiterung (`calculator.hpp` & `converter.hpp`)
*   **Prozentwert-Bugbehebung:** Der Prozent-Parser dividiert in `ParseFactor` aktuell blind durch 100, wodurch `100 + 10%` zu `100.1` evaluiert wird. 
    *   *Lösung:* Übergabe eines Kontext-Flags in `ParseExpression`, damit Prozentwerte relativ zum linken Operanden berechnet werden: `value = value + (value * (percentVal / 100.0))`.
*   **Wissenschaftliche Funktionen:** Unterstützung für `sin()`, `cos()`, `tan()`, Exponentiation (`^` oder `**`) und Quadratwurzel `sqrt()`.
*   **Erweiterte Einheiten:** Ergänzung fehlender IT-Datenraten (`bps`, `kbps`, `mbps`, `gbps`), Leistungen (`W`, `kW`, `HP`) sowie Energien (`kWh`).

### Websuche über Kürzel & System-Aktionen
*   **Suchkürzel:** Erkennung von Präfix-Triggern (z. B. `g C++` -> Google Suche, `yt lo-fi` -> YouTube Suche, `w Raycast` -> Wikipedia).
*   **Windows-Systembefehle:** Integration nativer Windows-API-Aktionen:
    *   `Sperren` -> Aufruf von `LockWorkStation()`
    *   `Herunterfahren` -> Aufruf von `ExitWindowsEx()`
    *   `Stummschalten` -> Aufruf der Core Audio APIs (`IAudioEndpointVolume`)
    *   `Papierkorb leeren` -> Aufruf von `SHEmptyRecycleBinW()`

### Fuzzy-Search-Balancierung (`core.hpp`)
*   **Präfix-Boost anpassen:** Der aktuell Präfix-Name-Match-Boost von `+1200` ist so hoch, dass er den gesamten Verlaufs- und Häufigkeits-Boost (`recentApps` & `usageCount`, max. `+1110`) überholt. 
    *   *Auswirkung:* "CodeBlocks" (nie genutzt) überholt bei Eingabe von "code" das täglich genutzte "Visual Studio Code".
    *   *Lösung:* Reduzierung des Präfix-Boosts von `+1200` auf **`+600`**.
*   **Tippfehlertoleranz:** Integration eines Damerau-Levenshtein-Distanz-Checks für Suchbegriffe mit mehr als 4 Zeichen.

### Plugin-Host Stabilität (`extension_manager.hpp`)
*   **Graceful Timeouts:** Die harte 250ms-Timeout-Grenze, die Plugins beim ersten Überschreiten dauerhaft deaktiviert (`available = false`), muss durch ein **3-Strike-System** entschärft werden. 
*   **API-Erweiterungen:** Unterstützung von Markdown-Rendering-Detailfenstern (Rich UI) im Launcher und bidirektionaler Kommunikation, damit Plugins den Suchschlitz des Launchers aktualisieren können.
