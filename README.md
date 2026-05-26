# ESP32 TUYA + SATEL Bridge

Kompletny projekt demonstracyjny dla **ESP32**, którego celem jest integracja chmury **TUYA** z centralą **SATEL INTEGRA** wyposażoną w moduł **ETHM-1 PLUS**, z lokalnym panelem WWW, szyfrowaniem danych, mechanizmem reguł, diagnostyką, MQTT, webhookami oraz przygotowaną warstwą bezpieczeństwa panelu administracyjnego. [cite:1]

## Cel projektu

Projekt realizuje most integracyjny uruchamiany na ESP32, który:

- łączy się z TUYA Cloud i z centralą SATEL przez ETHM-1 PLUS,
- pobiera urządzenia TUYA oraz stany i zmienne SATEL,
- pozwala budować reguły automatyki w obu kierunkach,
- udostępnia panel konfiguracyjny przez WWW,
- przechowuje konfigurację lokalnie,
- obsługuje reconnect, logowanie zdarzeń i podstawowe bezpieczeństwo dostępu. [cite:1]

Przykładowy scenariusz działania jest następujący: naruszenie czujki w strefie SATEL może włączyć światło TUYA, a włączenie światła TUYA może wywołać komendę w SATEL przez wejście IP. Taki model został odzwierciedlony w kodzie `main.cpp` oraz w callbackach integracyjnych dla obu systemów. [cite:1]

## Zakres funkcjonalny

### 1. Konfiguracja urządzenia

Projekt przewiduje konfigurację sieci i usług urządzenia, w tym:

- SSID i hasło Wi-Fi,
- nazwę hosta,
- DHCP albo adresację statyczną,
- adresy DNS,
- serwer NTP,
- dane dostępowe do TUYA,
- dane dostępowe do SATEL,
- dane logowania do panelu WWW. [cite:1]

Konfiguracja jest przechowywana lokalnie w NVS przez `config_manager`, a dane krytyczne mogą być dodatkowo zabezpieczane przez `crypto_manager`. Panel WWW został przygotowany jako frontend w `spiffs_data/index.html`, a serwer API działa przez komponent `http_server`. [cite:1]

### 2. Integracja TUYA

Warstwa TUYA znajduje się w komponentach `tuya_client` oraz `tuya_http`. Odpowiada ona za połączenie z TUYA Cloud, odczyt urządzeń i obsługę datapointów, takich jak przykładowy `switch_led`, który jest wykorzystywany w logice reguł. [cite:1]

### 3. Integracja SATEL

Warstwa SATEL znajduje się w komponentach `satel_client` i `satel_protocol`. Odpowiada za komunikację z centralą INTEGRA przez moduł ETHM-1 PLUS, odbiór zmian stref i wejść oraz wywoływanie akcji zwrotnych, takich jak aktywacja wejścia IP. [cite:1]

### 4. Reguły automatyki

Projekt posiada elementarny silnik reguł rozłożony między `rule_engine`, `automation_modes` i logikę główną w `main.cpp`. W obecnym stanie pokazane są między innymi:

- tryby pracy `HOME`, `AWAY`, `NIGHT`, `MANUAL`,
- warunki czasowe dla reguł,
- automatyczne wyłączenie światła po określonym czasie,
- reguły w kierunku SATEL -> TUYA,
- reguły w kierunku TUYA -> SATEL. [cite:1]

### 5. Diagnostyka i odporność

Projekt zawiera kilka mechanizmów zwiększających niezawodność:

- `event_log` przechowuje lokalny bufor zdarzeń,
- `watchdog` oraz `hw_watchdog` wspierają odzyskanie po zawieszeniu,
- `rate_limit` ogranicza częstotliwość wywołań API i callbacków,
- `mqtt_bridge` publikuje statusy i zdarzenia do MQTT,
- `webhook_client` wysyła powiadomienia HTTP POST do systemów zewnętrznych. [cite:1]

### 6. Bezpieczeństwo panelu WWW

Dostęp do panelu WWW jest chroniony przez Basic Auth. Dane logowania nie są przechowywane jako jawne hasło, lecz jako nazwa użytkownika, 16-bajtowa sól i hash SHA-256 liczony z połączenia soli i hasła, co zostało zaimplementowane w `http_server` oraz `config_manager`. [cite:1]

Warstwa TLS jest realizowana przez `web_tls`, które przy pierwszym uruchomieniu generuje samopodpisany certyfikat X.509 i klucz prywatny przy użyciu mbedTLS, a następnie zapisuje je do NVS. Kolejne uruchomienia korzystają z już zapisanych danych certyfikatu i klucza. [cite:1]

## Struktura projektu

Najważniejsze katalogi i pliki:

| Ścieżka | Rola |
|---|---|
| `main/main.cpp` | Główna logika startu i spinanie komponentów [cite:1] |
| `components/config_manager/` | Zapis i odczyt konfiguracji z NVS [cite:1] |
| `components/crypto_manager/` | Obsługa ochrony danych wrażliwych [cite:1] |
| `components/tuya_client/` | Integracja z TUYA Cloud [cite:1] |
| `components/satel_client/` | Integracja z SATEL ETHM-1 PLUS [cite:1] |
| `components/http_server/` | API HTTP i panel administracyjny [cite:1] |
| `components/web_tls/` | Certyfikat self-signed X.509 i klucz TLS [cite:1] |
| `components/mqtt_bridge/` | Publikacja do MQTT i Home Assistant Discovery [cite:1] |
| `components/webhook_client/` | Wysyłka webhooków HTTP [cite:1] |
| `components/event_log/` | Lokalny log zdarzeń [cite:1] |
| `components/automation_modes/` | Tryby pracy i warunki czasowe [cite:1] |
| `components/rate_limit/` | Ograniczanie częstotliwości wywołań [cite:1] |
| `components/hw_watchdog/` | Sprzętowy watchdog oparty o `esp_task_wdt` [cite:1] |
| `spiffs_data/index.html` | Frontend panelu WWW [cite:1] |
| `sdkconfig.defaults` | Domyślne ustawienia ESP-IDF [cite:1] |
| `partitions.csv` | Tabela partycji flash [cite:1] |

## Przepływ działania

Po uruchomieniu urządzenie inicjalizuje NVS, log zdarzeń, warstwę trybów, limiter wywołań, watchdog, konfigurację sieci, TLS, OTA, MQTT, webhooki oraz klientów TUYA i SATEL. Następnie rejestrowane są callbacki zdarzeń i startuje główna pętla monitorująca stan systemu. [cite:1]

W typowym scenariuszu wygląda to tak:

1. ESP32 startuje i ładuje konfigurację z NVS. [cite:1]
2. Jeśli nie ma certyfikatu TLS, generowany jest self-signed X.509 przez mbedTLS i zapisywany do NVS. [cite:1]
3. Jeśli nie ma danych logowania panelu, tworzony jest użytkownik domyślny z hashem SHA-256 z solą. [cite:1]
4. Uruchamiane są usługi systemowe, integracje i serwer HTTP. [cite:1]
5. Zdarzenia z SATEL i TUYA są przetwarzane przez callbacki i przekazywane do reguł, MQTT, webhooków i logu lokalnego. [cite:1]

## Endpointy HTTP

Projekt zawiera następujące endpointy API:

| Endpoint | Metoda | Opis |
|---|---|---|
| `/` | GET | Prosty ekran informacyjny API, chroniony Basic Auth [cite:1] |
| `/api/status` | GET | Status serwera WWW [cite:1] |
| `/api/system` | GET | Status usług systemowych, czasu i hosta [cite:1] |
| `/api/tls` | GET | Informacje o certyfikacie i kluczu TLS [cite:1] |
| `/api/ota` | GET | Status OTA [cite:1] |
| `/api/ota` | POST | Wywołanie aktualizacji OTA po URL [cite:1] |
| `/api/mode` | GET | Aktualny tryb pracy [cite:1] |
| `/api/mode` | POST | Zmiana trybu pracy [cite:1] |
| `/api/rate-limit` | GET | Status limiterów wywołań [cite:1] |
| `/api/watchdog` | GET | Status watchdoga sprzętowego [cite:1] |
| `/api/events` | GET | Bufor zdarzeń lokalnych [cite:1] |
| `/api/mqtt` | GET | Status integracji MQTT [cite:1] |
| `/api/webhook` | GET | Status klienta webhook [cite:1] |
| `/api/auth` | POST | Zmiana danych logowania panelu [cite:1] |

## Wymagania

Do pracy z projektem potrzebne są:

- ESP32,
- ESP-IDF 5.x lub zgodne środowisko zawierające wymagane komponenty,
- konto TUYA Cloud z poprawnymi danymi API,
- centrala SATEL INTEGRA z ETHM-1 PLUS,
- dostęp do lokalnej sieci IP,
- opcjonalnie broker MQTT i system Home Assistant. [cite:1]

## Budowanie projektu

Przykładowa procedura dla ESP-IDF:

```bash
idf.py set-target esp32
idf.py menuconfig
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Pliki `sdkconfig.defaults`, `CMakeLists.txt`, `main/` oraz `components/` są już obecne w projekcie, więc repozytorium ma podstawową strukturę zgodną z ESP-IDF. [cite:1]

## Konfiguracja po pierwszym uruchomieniu

Zalecana kolejność:

1. Uzupełnić konfigurację sieci i hosta urządzenia. [cite:1]
2. Wprowadzić dane TUYA i SATEL. [cite:1]
3. Zweryfikować połączenia z oboma systemami. [cite:1]
4. Zmienić domyślne dane logowania panelu przez `POST /api/auth`. [cite:1]
5. Zweryfikować certyfikat TLS i docelowo podpiąć go do serwera HTTPS. [cite:1]
6. Skonfigurować reguły, MQTT i webhooki. [cite:1]

## Przykładowe użycie API

### Zmiana trybu pracy

```bash
curl -u admin:StrongPass123! -X POST http://ESP32-IP/api/mode \
  -H "Content-Type: application/json" \
  -d '{"mode":"NIGHT"}'
```

### Zmiana danych logowania panelu

```bash
curl -u admin:StrongPass123! -X POST http://ESP32-IP/api/auth \
  -H "Content-Type: application/json" \
  -d '{"username":"operator","password":"NoweSilneHaslo123"}'
```

### Odczyt logu zdarzeń

```bash
curl -u operator:NoweSilneHaslo123 http://ESP32-IP/api/events
```

## Ograniczenia obecnej wersji

Mimo że projekt zawiera wszystkie główne moduły i pełny zestaw plików, należy go traktować jako zaawansowany szkielet integracyjny, a nie gotowy produkt komercyjny. W szczególności warto jeszcze dopracować pełne spięcie z `esp_https_server`, wykonać pełny build i testy integracyjne oraz rozszerzyć frontend WWW o kompletną konfigurację wszystkich reguł i encji. [cite:1]

## Rekomendowane dalsze kroki

Najbardziej praktyczne dalsze prace to:

- przełączenie warstwy WWW z HTTP na pełne HTTPS z użyciem wygenerowanego certyfikatu, [cite:1]
- użycie porównania hashy w czasie stałym zamiast zwykłego `memcmp`, [cite:1]
- walidacja pełnej kompilacji `idf.py build`, [cite:1]
- testy z realnym TUYA Cloud i prawdziwą centralą SATEL, [cite:1]
- rozbudowa interfejsu WWW o pełny edytor reguł i diagnostykę połączeń. [cite:1]
