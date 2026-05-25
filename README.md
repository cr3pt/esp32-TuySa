# ESP32 TUYA + SATEL INTEGRA Bridge

Platforma integracyjna ESP32 łącząca urządzenia TUYA Cloud
z centralą alarmową SATEL INTEGRA przez moduł ETHM-1 PLUS.

## Wymagania sprzętowe

- ESP32 (minimum 4 MB Flash, 520 KB SRAM)
- Moduł ETHM-1 PLUS podłączony do centrali SATEL INTEGRA
- 3× dioda LED (zielona GPIO2, żółta GPIO4, czerwona GPIO5)
- Sieć WiFi 2.4 GHz

## Wymagania programowe

- ESP-IDF v5.1 lub nowszy
- Python 3.8+
- CMake 3.16+

## Pierwsze uruchomienie

```bash
# 1. Sklonuj / wypakuj projekt
cd esp32-tuya-satel

# 2. Ustaw target
idf.py set-target esp32

# 3. Konfiguracja (opcjonalnie)
idf.py menuconfig

# 4. Zbuduj i wgraj
idf.py build flash monitor
```

## Konfiguracja urządzenia

Po pierwszym uruchomieniu ESP32 tworzy punkt dostępowy:
  **SSID**: `ESP32-Bridge-Setup`

1. Połącz się z tym AP
2. Otwórz przeglądarkę → `http://192.168.4.1`
3. Skonfiguruj sieć WiFi, hostname, DNS, NTP
4. Po restarcie otwórz panel pod adresem IP urządzenia
5. Wprowadź dane TUYA (region, client_id, secret, uid)
6. Wprowadź dane SATEL (IP, port 7094, hasło integracji)
7. Kliknij "Test połączenia" dla każdego systemu
8. Po pozytywnym teście — ustaw klucz szyfrujący
9. Urządzenie restartuje się i pobiera urządzenia/zmienne

## Architektura komponentów

```
components/
├── boot_manager/     Tryby SETUP/NORMAL, restart z powodem
├── config_manager/   Konfiguracja sieciowa i dane dostępowe (NVS)
├── crypto_manager/   AES-256-GCM + PBKDF2, szyfrowanie sekretów
├── wifi_manager/     WiFi STA/AP, DHCP/statyczne IP, callback stanu
├── captive_portal/   DNS redirect + formularz konfiguracji (tryb SETUP)
├── spiffs_www/       Panel WWW serwowany z SPIFFS
├── http_server/      REST API + Server-Sent Events (SSE)
├── tuya_client/      TUYA OpenAPI v1.0 (auth, urządzenia, DP, komendy)
├── satel_client/     Protokół binarny SATEL INTEGRA przez TCP
├── rule_engine/      Silnik reguł automatyki (NVS, JSON API)
└── watchdog/         Monitor łączności, LED, backoff reconnect
```

## REST API

| Metoda | Endpoint            | Opis                          |
|--------|---------------------|-------------------------------|
| GET    | /api/status         | Health check całego systemu   |
| GET    | /api/tuya/devices   | Lista urządzeń TUYA           |
| GET    | /api/satel/state    | Stan centrali SATEL           |
| GET    | /api/rules          | Lista reguł automatyki        |
| POST   | /api/rules          | Dodaj regułę (JSON)           |
| DELETE | /api/rules/{id}     | Usuń regułę                   |
| POST   | /api/rules/{id}/test| Ręczne wyzwolenie reguły      |
| POST   | /api/test/{target}  | Test połączenia (tuya/satel)  |
| POST   | /api/key            | Ustaw klucz szyfrujący        |
| POST   | /api/reset          | Factory reset                 |
| GET    | /events             | SSE stream (stan na żywo)     |

## SSE Events

| Event           | Opis                              |
|-----------------|-----------------------------------|
| system_ready    | System w pełni uruchomiony        |
| sys_state       | Zmiana stanu systemu              |
| sys_heartbeat   | Heartbeat co 30s                  |
| tuya_state      | Zmiana stanu klienta TUYA         |
| satel_state     | Zmiana stanu klienta SATEL        |
| satel_heartbeat | Heartbeat SATEL (naruszone wejścia)|
| rule_fired      | Wykonanie reguły automatyki       |
| wifi_state      | Zmiana stanu WiFi                 |
| key_required    | Wymagane ustawienie klucza        |
| sys_critical    | Błąd krytyczny                    |

## Sygnalizacja LED

| Stan                | GPIO2 (zielona) | GPIO4 (żółta)   | GPIO5 (czerwona) |
|---------------------|-----------------|-----------------|------------------|
| BOOT                | -               | 10 Hz           | -                |
| ALL_OK              | 1 Hz            | -               | -                |
| WIFI_DOWN           | -               | -               | 5 Hz             |
| TUYA_RECONNECT      | -               | 1 Hz            | -                |
| SATEL_RECONNECT     | -               | 0.5 Hz          | 0.5 Hz (razem)   |
| BOTH_RECONNECT      | -               | naprzemiennie   | naprzemiennie    |
| ERROR_CRITICAL      | -               | -               | SOS Morse        |

## Integracja TUYA

Wymaga konta TUYA Cloud (https://iot.tuya.com):
- Region: eu / us / cn / in
- Client ID i Secret z projektu IoT
- User UID z konta aplikacji

## Integracja SATEL INTEGRA

Wymaga modułu ETHM-1 PLUS:
- Adres IP modułu w sieci lokalnej
- Port: 7094 (domyślny)
- Hasło integracji (maks. 8 cyfr, ustawione w INTEGRA przez DLOADX)
- Aktywowane wejścia IP w module (dla komend IP INPUT ON/OFF)

## Licencja

MIT License — Projekt edukacyjny / prototypowy.
Przed wdrożeniem produkcyjnym należy przeprowadzić audyt bezpieczeństwa.
