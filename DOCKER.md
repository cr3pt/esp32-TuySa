# Budowanie i wgrywanie przez Docker

Ten projekt to firmware na ESP32 (ESP-IDF, C++), a nie usługa serwerowa —
docker nie "uruchamia" tu aplikacji, tylko dostarcza gotowe środowisko
ESP-IDF 5.x do kompilacji i wgrywania kodu na płytkę, bez instalowania
toolchaina lokalnie. Sam firmware nadal działa na fizycznym ESP32, łącząc
się z prawdziwym Wi-Fi, centralą SATEL po TCP i chmurą TUYA.

## Wymagania

- Docker + [Docker Compose v2](https://docs.docker.com/compose/) (`docker compose ...`, nie starsze `docker-compose`).
- Do samego budowania: nic więcej.
- Do wgrywania/monitora: podłączony ESP32 i port szeregowy widoczny z hosta
  (patrz ograniczenia macOS/Windows niżej).

## Budowanie firmware

Nie wymaga podłączonej płytki — działa też w CI/chmurze:

```bash
docker compose run --rm build
```

Wynik trafia do `build-docker/` (celowo inny katalog niż `build/`, żeby nie
kolidować z ewentualną kompilacją zrobioną lokalnym ESP-IDF na innej
maszynie — `CMakeCache.txt` w `build/` przechowuje ścieżki bezwzględne i nie
da się go bezpiecznie współdzielić między środowiskami).

Interaktywna konfiguracja (`idf.py menuconfig`):

```bash
docker compose run --rm menuconfig
```

Czyszczenie:

```bash
docker compose run --rm clean
```

## Wgrywanie na płytkę i podgląd portu

Te usługi są w profilu `hardware` (nie startują domyślnie, bo wymagają
realnego portu szeregowego) i potrzebują zmiennej `ESP32_PORT`:

```bash
cp .env.example .env   # ustaw tam ESP32_PORT, jeśli inny niż /dev/ttyUSB0
docker compose --profile hardware run --rm flash
docker compose --profile hardware run --rm monitor
docker compose --profile hardware run --rm flash-monitor   # flash + monitor jednym poleceniem
docker compose --profile hardware run --rm shell           # interaktywna powłoka z idf.py
```

### Ograniczenia w zależności od systemu

- **Linux**: przekazywanie `/dev/ttyUSB0` (lub `/dev/ttyACM0`) do kontenera
  działa bezpośrednio — patrz `devices:` w `docker-compose.yml`.
- **macOS**: Docker Desktop **nie przekazuje** portów USB/szeregowych do
  kontenerów (ograniczenie samego Docker Desktop, nie tego projektu).
  Budowanie w Dockerze działa normalnie, ale flashowanie i monitor trzeba
  wykonać lokalnym ESP-IDF (`idf.py -p /dev/cu.usbserial... flash monitor`)
  albo w innym środowisku z bezpośrednim dostępem do USB.
- **Windows**: przez WSL2 trzeba najpierw przekazać port USB do dystrybucji
  WSL2 (np. [usbipd-win](https://github.com/dorssel/usbipd-win)), dopiero
  wtedy urządzenie pojawi się jako `/dev/ttyUSB0` wewnątrz WSL2 i będzie
  widoczne dla Dockera.

## Co jest w środku

- `Dockerfile` — bazuje na oficjalnym obrazie `espressif/idf:release-v5.1`;
  kod projektu nie jest w niego wbudowywany (COPY), tylko montowany jako
  wolumen w `docker-compose.yml`, więc lokalne zmiany są widoczne od razu
  i obraz wystarczy zbudować raz.
- `docker-compose.yml` — usługi `build`/`menuconfig`/`clean` (bezpieczne
  wszędzie) oraz `flash`/`monitor`/`flash-monitor`/`shell` (profil
  `hardware`, wymagają portu szeregowego).
- `.env.example` — szablon zmiennej `ESP32_PORT`.
- `.dockerignore` — wyklucza `.git`, `build/`, `build-docker/` z kontekstu
  budowania obrazu.

## Uwaga o katalogu `build/`

W repozytorium jest już zacommitowany katalog `build/` z artefaktami
kompilacji z innej maszyny (m.in. `CMakeCache.txt` ze ścieżkami
bezwzględnymi tamtego środowiska). To nie jest coś, czego wymaga ten
projekt — zwykle katalog `build/` powinien być w `.gitignore`, a nie w
repozytorium. Dockerowy przepływ celowo go omija (buduje do
`build-docker/`), żeby nie mieszać się z tymi starymi artefaktami, ale
warto rozważyć usunięcie `build/` z repozytorium i dodanie go do
`.gitignore` przy najbliższej okazji.
