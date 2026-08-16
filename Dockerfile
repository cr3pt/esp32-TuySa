# Obraz do budowania i wgrywania firmware ESP32-TuySa przy uzyciu ESP-IDF,
# bez instalowania toolchaina lokalnie. Kod projektu NIE jest kopiowany do
# obrazu - docker-compose.yml montuje repozytorium jako wolumen, wiec obraz
# trzeba zbudowac raz, a lokalne zmiany w kodzie sa widoczne od razu.
#
# Zgodnie z README projekt wymaga ESP-IDF 5.x - obraz bazowy to oficjalne
# srodowisko Espressif z ta sama galezia.
FROM espressif/idf:release-v5.1

WORKDIR /project
