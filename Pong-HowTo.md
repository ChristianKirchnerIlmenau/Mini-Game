# Pong-HowTo

Hallo! Das ist eine kurze Anleitung fuer dein Pong-Spiel.

## Was du brauchst
- Das Display ist angeschlossen
- Rechte Taste: D13 (gegen GND)
- Linke Taste: D27 (gegen GND)
- Start und Pause: BOOT-Taste (GPIO0)

## Starten
1. Schalte den ESP32 ein.
2. Du siehst den Startbildschirm mit "PONG" und dem Highscore.
3. Druecke die BOOT-Taste, um zu starten.

## Spielen
- Bewege den Schlaeger mit den Tasten:
  - Rechts: D13
  - Links: D27
- Triff den Ball mit dem Schlaeger.
- Treffer = H (Hits)
- Fehler = F (Failures)

## Pause
- Druecke die BOOT-Taste einmal, um zu pausieren.
- Druecke sie nochmal, um weiter zu spielen.

## Spielende
- Bei 10 Failures ist das Spiel vorbei.
- Du kommst automatisch zum Startbildschirm zurueck.

## Highscore
- Dein bester Highscore wird gespeichert.
- Wenn du neue Firmware flashst, kann der Highscore ueberschrieben werden.

## Wichtiger Hinweis zur BOOT-Taste
- Halte die BOOT-Taste nicht gedrueckt, wenn du den ESP32 neu startest.
- Sonst startet er im Flash-Modus.

Viel Spass beim Spielen!
