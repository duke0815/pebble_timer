# Pebble Timer (Pebble Time 2)

Countdown-Timer für die Pebble Time 2 (Plattform `emery`) mit Touch-Bedienung.

## Bedienung

**Einstellen**
- Zwei Rädchen: links **Minuten** (0–99), rechts **Sekunden** (0–59)
- Mit dem Finger hoch/runter wischen – mit Schwung dreht das Rad nach
- **UP / DOWN** drehen das aktive (orange) Rad
- **SELECT lang** oder Tippen auf ein Rad wechselt das aktive Rad
- **SELECT** oder Tippen auf **Start** startet den Timer

**Läuft**
- **SELECT** / Tippen: Pause / Weiter
- **UP / DOWN**: +1 / −1 Minute
- **SELECT lang** oder **Stopp**: Timer abbrechen
- **BACK**: App verlassen – der Timer läuft weiter, bei 0:00 startet die App per Wakeup

**Abgelaufen**
- Vibriert (ca. 30 s lang alle 3 s) und schaltet das Licht ein
- **Neuer Timer** oder **Beenden** (UP/DOWN + SELECT oder Tippen)

## Bauen

```sh
pebble build
pebble install --emulator emery   # oder --phone <IP>
```
