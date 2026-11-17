#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "telemetry_ring.h"
#include "telemetry_json.h"

int main(void)
{
    heater_message_t message = {.kind = HEATER_MESSAGE_DATA, .run_id = 7};
    assert(telemetry_message_recordable(&message, true, 7));
    assert(!telemetry_message_recordable(&message, false, 7));
    assert(!telemetry_message_recordable(&message, true, 8));
    assert(!telemetry_message_recordable(&message, true, 0));
    message.kind = HEATER_MESSAGE_REPLY;
    assert(!telemetry_message_recordable(&message, true, 7));
    telemetry_ring_t ring;
    telemetry_record_t memory[3], read[4];
    assert(!telemetry_ring_init(&ring, memory, 0));
    assert(telemetry_ring_init(&ring, memory, 3));
    uint64_t next, lost;
    assert(telemetry_ring_read(&ring, 0, read, 4, &next, &lost) == 0 && next == 1 && lost == 0);
    heater_telemetry_t frame = {.valid_mask = 15, .resistance_ohm = {100, 110, 120, 130}};
    for (unsigned i = 1; i <= 5; ++i) { frame.sequence = i; telemetry_ring_push(&ring, &frame); }
    telemetry_stats_t stats = telemetry_ring_stats(&ring);
    assert(stats.oldest == 3 && stats.next == 6 && stats.overwritten == 2);
    assert(telemetry_ring_read(&ring, 1, read, 2, &next, &lost) == 2 && next == 5 && lost == 2);
    assert(read[0].cursor == 3 && read[0].frame.sequence == 3 && read[1].cursor == 4);
    assert(telemetry_ring_read(&ring, next, read, 4, &next, &lost) == 1 && next == 6 && lost == 0);
    assert(telemetry_ring_read(&ring, 0, read, 4, &next, &lost) == 3 && lost == 0);
    assert(telemetry_ring_read(&ring, UINT64_MAX, read, 4, &next, &lost) == 0 && next == 6);
    frame.resistance_ohm[1] = NAN; frame.valid_mask = 5;
    telemetry_record_t record = {.cursor = 9, .frame = frame};
    char json[320];
    assert(telemetry_record_json(&record, json, sizeof(json)) > 0);
    assert(strstr(json, "[100.000000,null,120.000000,null]"));
    assert(!strstr(json, "nan") && !strstr(json, "inf"));
    assert(telemetry_record_json(&record, json, 8) == -1);
    assert(telemetry_parse_cursor("18446744073709551615", &next) && next == UINT64_MAX);
    assert(!telemetry_parse_cursor("18446744073709551616", &next));
    assert(!telemetry_parse_cursor("-1", &next) && !telemetry_parse_cursor("1x", &next));
    assert(!telemetry_parse_cursor("", &next));
    char field[33];
    assert(telemetry_form_value("ssid=lab+wifi%26test&password=12345678", "ssid", field, sizeof(field)));
    assert(!strcmp(field, "lab wifi&test"));
    assert(telemetry_form_value("password=", "password", field, sizeof(field)) && !*field);
    assert(!telemetry_form_value("ssid=one&ssid=two", "ssid", field, sizeof(field)));
    assert(!telemetry_form_value("ssid=%00", "ssid", field, sizeof(field)));
    assert(!telemetry_form_value("ssid=%0A", "ssid", field, sizeof(field)));
    assert(!telemetry_form_value("ssid=%", "ssid", field, sizeof(field)));
    assert(!telemetry_form_value("ssid=toolong", "ssid", field, 4));
    puts("Telemetry wraparound, replay, JSON and input validation tests passed");
    return 0;
}
