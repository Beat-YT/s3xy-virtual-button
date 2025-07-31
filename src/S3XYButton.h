#pragma once
#include <stdint.h>

// Init as spoofed button. Optional custom name.
void s3xy_begin(const char* deviceName = "ENH_BTN");

// Call occasionally (okay to skip).
void s3xy_loop();

// Ready = connected + notifications enabled
bool s3xy_ready();

// Actions
void s3xy_send_single();
void s3xy_send_long();
void s3xy_send_double();

// Optional: set 10-byte ID exposed on ID characteristic
void s3xy_set_id(const uint8_t id[10]);

// --- User hooks (set from main.cpp) ---
typedef void (*s3xy_cb_t)();
void s3xy_on_connect(s3xy_cb_t cb);    // called after BLE connects
void s3xy_on_disconnect(s3xy_cb_t cb); // called after BLE disconnects