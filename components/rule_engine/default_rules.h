#pragma once
/**
 * Przykładowe reguły ładowane przy pierwszym uruchomieniu
 * (gdy NVS jest pusty). Pokazują typowe scenariusze integracji.
 */
#include "rule_engine.h"

static inline void rule_engine_load_defaults(void) {
    if (rule_engine_count() > 0) return;

    /* ── Reguła 1: Naruszenie czujki → włącz światło ─────────────── */
    rule_t r1 = {};
    strncpy(r1.name, "Czujka PIR → Światło ON", sizeof(r1.name)-1);
    r1.enabled                 = false;   /* domyślnie wyłączona */
    r1.dry_run                 = true;    /* tryb testowy */
    r1.priority                = 0;
    r1.trigger.type            = TRIGGER_SATEL_ZONE_VIOLATION;
    r1.trigger.satel_number    = 1;       /* wejście nr 1 — zmień wg instalacji */
    r1.trigger.expect_bool     = true;    /* aktywacja (naruszenie) */
    r1.trigger.expect_exact    = true;
    r1.trigger.cooldown_s      = 30;      /* min. 30s między włączeniami */
    r1.action.type             = ACTION_TUYA_SEND_BOOL;
    /* device_id i dp_code uzupełnić po pobraniu urządzeń z TUYA */
    strncpy(r1.action.tuya_dp_code, "switch_1", sizeof(r1.action.tuya_dp_code)-1);
    r1.action.value_bool       = true;
    r1.action.delay_ms         = 0;
    rule_engine_add(&r1);

    /* ── Reguła 2: Włączenie światła → wejście IP ETHM-1 PLUS ──── */
    rule_t r2 = {};
    strncpy(r2.name, "Światło ON → IP Input #1", sizeof(r2.name)-1);
    r2.enabled                 = false;
    r2.dry_run                 = true;
    r2.priority                = 1;
    r2.trigger.type            = TRIGGER_TUYA_DP_BOOL;
    strncpy(r2.trigger.tuya_dp_code, "switch_1", sizeof(r2.trigger.tuya_dp_code)-1);
    r2.trigger.expect_bool     = true;
    r2.trigger.expect_exact    = true;
    r2.trigger.cooldown_s      = 5;
    r2.action.type             = ACTION_SATEL_IP_INPUT_ON;
    r2.action.satel_number     = 1;      /* Wejście IP nr 1 ETHM-1 PLUS */
    r2.action.delay_ms         = 500;
    rule_engine_add(&r2);

    /* ── Reguła 3: Alarm strefy → wyłącz wszystkie światła ───────── */
    rule_t r3 = {};
    strncpy(r3.name, "Alarm strefy 1 → Światła OFF", sizeof(r3.name)-1);
    r3.enabled                 = false;
    r3.dry_run                 = true;
    r3.priority                = 0;
    r3.trigger.type            = TRIGGER_SATEL_PART_ALARM;
    r3.trigger.satel_number    = 1;      /* strefa 1 */
    r3.trigger.expect_bool     = true;
    r3.trigger.expect_exact    = true;
    r3.trigger.cooldown_s      = 60;
    r3.action.type             = ACTION_TUYA_SEND_BOOL;
    strncpy(r3.action.tuya_dp_code, "switch_all", sizeof(r3.action.tuya_dp_code)-1);
    r3.action.value_bool       = false;
    r3.action.delay_ms         = 2000;
    rule_engine_add(&r3);

    /* ── Reguła 4: Uzbrojenie strefy → wyjście SATEL (np. sygnalizator) */
    rule_t r4 = {};
    strncpy(r4.name, "Uzbrojenie → Sygnalizator ON", sizeof(r4.name)-1);
    r4.enabled                 = false;
    r4.dry_run                 = true;
    r4.priority                = 2;
    r4.trigger.type            = TRIGGER_SATEL_PART_ARMED;
    r4.trigger.satel_number    = 0;      /* dowolna strefa */
    r4.trigger.expect_bool     = true;
    r4.trigger.expect_exact    = true;
    r4.trigger.cooldown_s      = 10;
    r4.action.type             = ACTION_SATEL_OUTPUT_ON;
    r4.action.satel_number     = 1;      /* wyjście nr 1 INTEGRA */
    r4.action.delay_ms         = 1000;
    rule_engine_add(&r4);
}
