# ESP32 TUYA + SATEL Bridge

Kompletny projekt demonstracyjny dla **ESP32**, którego celem jest integracja chmury **TUYA** z centralą **SATEL INTEGRA** wyposażoną w moduł **ETHM-1 PLUS**, z lokalnym panelem WWW, szyfrowaniem danych, mechanizmem reguł, diagnostyką, MQTT, webhookami oraz przygotowaną warstwą bezpieczeństwa panelu administracyjnego. 

## Cel projektu

Projekt realizuje most integracyjny uruchamiany na ESP32, który:

- łączy się z TUYA Cloud i z centralą SATEL przez ETHM-1 PLUS,
- pobiera urządzenia TUYA oraz stany i zmienne SATEL,
- pozwala budować reguły automatyki w obu kierunkach,
- udostępnia panel konfiguracyjny przez WWW,
- przechowuje konfigurację lokalnie,
- obsługuje reconnect, logowanie zdarzeń i podstawowe bezpieczeństwo dostępu. 

Przykładowy scenariusz działania jest następujący: naruszenie czujki w strefie SATEL może włączyć światło TUYA, a włączenie światła TUYA może wywołać komendę w SATEL przez wejście IP. Taki model został odzwierciedlony w kodzie `main.cpp` oraz w callbackach integracyjnych dla obu systemów. 

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
- dane logowania do panelu WWW. 

Konfiguracja jest przechowywana lokalnie w NVS przez `config_manager`, a dane krytyczne mogą być dodatkowo zabezpieczane przez `crypto_manager`. Panel WWW został przygotowany jako frontend w `spiffs_data/index.html`, a serwer API działa przez komponent `http_server`. 

### 2. Integracja TUYA

Warstwa TUYA znajduje się w komponentach `tuya_client` oraz `tuya_http`. Odpowiada ona za połączenie z TUYA Cloud, odczyt urządzeń i obsługę datapointów, takich jak przykładowy `switch_led`, który jest wykorzystywany w logice reguł. 

### 3. Integracja SATEL

Warstwa SATEL znajduje się w komponentach `satel_client` i `satel_protocol`. Odpowiada za komunikację z centralą INTEGRA przez moduł ETHM-1 PLUS, odbiór zmian stref i wejść oraz wywoływanie akcji zwrotnych, takich jak aktywacja wejścia IP. 

### 4. Reguły automatyki

Projekt posiada elementarny silnik reguł rozłożony między `rule_engine`, `automation_modes` i logikę główną w `main.cpp`. W obecnym stanie pokazane są między innymi:

- tryby pracy `HOME`, `AWAY`, `NIGHT`, `MANUAL`,
- warunki czasowe dla reguł,
- automatyczne wyłączenie światła po określonym czasie,
- reguły w kierunku SATEL -> TUYA,
- reguły w kierunku TUYA -> SATEL. 

### 5. Diagnostyka i odporność

Projekt zawiera kilka mechanizmów zwiększających niezawodność:

- `event_log` przechowuje lokalny bufor zdarzeń,
- `watchdog` oraz `hw_watchdog` wspierają odzyskanie po zawieszeniu,
- `rate_limit` ogranicza częstotliwość wywołań API i callbacków,
- `mqtt_bridge` publikuje statusy i zdarzenia do MQTT,
- `webhook_client` wysyła powiadomienia HTTP POST do systemów zewnętrznych. 

### 6. Bezpieczeństwo panelu WWW

Dostęp do panelu WWW jest chroniony przez Basic Auth. Dane logowania nie są przechowywane jako jawne hasło, lecz jako nazwa użytkownika, 16-bajtowa sól i hash SHA-256 liczony z połączenia soli i hasła, co zostało zaimplementowane w `http_server` oraz `config_manager`. 

Warstwa TLS jest realizowana przez `web_tls`, które przy pierwszym uruchomieniu generuje samopodpisany certyfikat X.509 i klucz prywatny przy użyciu mbedTLS, a następnie zapisuje je do NVS. Kolejne uruchomienia korzystają z już zapisanych danych certyfikatu i klucza. 

## Struktura projektu

Najważniejsze katalogi i pliki:

| Ścieżka | Rola |
|---|---|
| `main/main.cpp` | Główna logika startu i spinanie komponentów  |
| `components/config_manager/` | Zapis i odczyt konfiguracji z NVS  |
| `components/crypto_manager/` | Obsługa ochrony danych wrażliwych  |
| `components/tuya_client/` | Integracja z TUYA Cloud  |
| `components/satel_client/` | Integracja z SATEL ETHM-1 PLUS  |
| `components/http_server/` | API HTTP i panel administracyjny  |
| `components/web_tls/` | Certyfikat self-signed X.509 i klucz TLS  |
| `components/mqtt_bridge/` | Publikacja do MQTT i Home Assistant Discovery  |
| `components/webhook_client/` | Wysyłka webhooków HTTP  |
| `components/event_log/` | Lokalny log zdarzeń  |
| `components/automation_modes/` | Tryby pracy i warunki czasowe  |
| `components/rate_limit/` | Ograniczanie częstotliwości wywołań  |
| `components/hw_watchdog/` | Sprzętowy watchdog oparty o `esp_task_wdt`  |
| `spiffs_data/index.html` | Frontend panelu WWW  |
| `sdkconfig.defaults` | Domyślne ustawienia ESP-IDF  |
| `partitions.csv` | Tabela partycji flash  |

## Przepływ działania

Po uruchomieniu urządzenie inicjalizuje NVS, log zdarzeń, warstwę trybów, limiter wywołań, watchdog, konfigurację sieci, TLS, OTA, MQTT, webhooki oraz klientów TUYA i SATEL. Następnie rejestrowane są callbacki zdarzeń i startuje główna pętla monitorująca stan systemu. 

W typowym scenariuszu wygląda to tak:

1. ESP32 startuje i ładuje konfigurację z NVS. 
2. Jeśli nie ma certyfikatu TLS, generowany jest self-signed X.509 przez mbedTLS i zapisywany do NVS. 
3. Jeśli nie ma danych logowania panelu, tworzony jest użytkownik domyślny z hashem SHA-256 z solą. 
4. Uruchamiane są usługi systemowe, integracje i serwer HTTP. 
5. Zdarzenia z SATEL i TUYA są przetwarzane przez callbacki i przekazywane do reguł, MQTT, webhooków i logu lokalnego. 

## Endpointy HTTP

Projekt zawiera następujące endpointy API:

| Endpoint | Metoda | Opis |
|---|---|---|
| `/` | GET | Prosty ekran informacyjny API, chroniony Basic Auth  |
| `/api/status` | GET | Status serwera WWW  |
| `/api/system` | GET | Status usług systemowych, czasu i hosta  |
| `/api/tls` | GET | Informacje o certyfikacie i kluczu TLS  |
| `/api/ota` | GET | Status OTA  |
| `/api/ota` | POST | Wywołanie aktualizacji OTA po URL  |
| `/api/mode` | GET | Aktualny tryb pracy  |
| `/api/mode` | POST | Zmiana trybu pracy  |
| `/api/rate-limit` | GET | Status limiterów wywołań  |
| `/api/watchdog` | GET | Status watchdoga sprzętowego  |
| `/api/events` | GET | Bufor zdarzeń lokalnych  |
| `/api/mqtt` | GET | Status integracji MQTT  |
| `/api/webhook` | GET | Status klienta webhook  |
| `/api/auth` | POST | Zmiana danych logowania panelu  |

## Wymagania

Do pracy z projektem potrzebne są:

- ESP32,
- ESP-IDF 5.x lub zgodne środowisko zawierające wymagane komponenty,
- konto TUYA Cloud z poprawnymi danymi API,
- centrala SATEL INTEGRA z ETHM-1 PLUS,
- dostęp do lokalnej sieci IP,
- opcjonalnie broker MQTT i system Home Assistant. 

## Budowanie projektu

Przykładowa procedura dla ESP-IDF:

```bash
idf.py set-target esp32
idf.py menuconfig
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Pliki `sdkconfig.defaults`, `CMakeLists.txt`, `main/` oraz `components/` są już obecne w projekcie, więc repozytorium ma podstawową strukturę zgodną z ESP-IDF. 

## Konfiguracja po pierwszym uruchomieniu

Zalecana kolejność:

1. Uzupełnić konfigurację sieci i hosta urządzenia. 
2. Wprowadzić dane TUYA i SATEL. 
3. Zweryfikować połączenia z oboma systemami. 
4. Zmienić domyślne dane logowania panelu przez `POST /api/auth`. 
5. Zweryfikować certyfikat TLS i docelowo podpiąć go do serwera HTTPS. 
6. Skonfigurować reguły, MQTT i webhooki. 

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

Mimo że projekt zawiera wszystkie główne moduły i pełny zestaw plików, należy go traktować jako zaawansowany szkielet integracyjny, a nie gotowy produkt komercyjny. W szczególności warto jeszcze dopracować pełne spięcie z `esp_https_server`, wykonać pełny build i testy integracyjne oraz rozszerzyć frontend WWW o kompletną konfigurację wszystkich reguł i encji. 
