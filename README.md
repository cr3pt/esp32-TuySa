# ESP32 TUYA + SATEL INTEGRA Bridge

Platforma ESP32 integrująca TUYA Cloud z SATEL INTEGRA przez ETHM-1 PLUS.

## Funkcje
- Captive portal do pierwszej konfiguracji WiFi i hosta
- WWW + REST API do statusu, testów i reguł
- Szyfrowanie danych dostępowych kluczem ustawianym przy pierwszej konfiguracji
- Klient TUYA Cloud z autoryzacją HMAC-SHA256
- Klient SATEL / ETHM-1 PLUS
- Silnik reguł automatyki w obu kierunkach
- Watchdog łączności z sygnalizacją LED

## Budowanie
```bash
idf.py set-target esp32
idf.py build
idf.py flash monitor
```

## Etapy startu
1. SETUP AP `ESP32-Bridge-Setup`
2. Zapis sieci i restart
3. Połączenie WiFi
4. Start WWW i REST API
5. Ustawienie klucza szyfrującego
6. Start TUYA + SATEL
7. Start reguł
8. Start watchdog

## Struktura katalogu spiffs_data/
Pliki z katalogu `spiffs_data/` są wgrywane na partycję SPIFFS:
```bash
idf.py spiffs-flash --partition-name=spiffs --base-dir=./spiffs_data
```
W `index.html` jest pełny panel WWW dostępny pod adresem IP urządzenia.

## Schemat LED (GPIO 2/4/5)
| GPIO 2 (zielona) | GPIO 4 (żółta) | GPIO 5 (czerwona) | Stan |
|------------------|----------------|-------------------|------|
| ON               | OFF            | OFF               | OK   |
| OFF              | miga           | OFF               | TUYA reconnect |
| OFF              | miga           | OFF               | SATEL reconnect |
| OFF              | OFF            | miga              | WiFi DOWN |
| OFF              | OFF            | ON                | BŁĄD KRYTYCZNY |
