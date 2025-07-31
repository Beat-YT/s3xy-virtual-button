#include <Arduino.h>
#include "S3XYButton.h"

const int LED_PIN = 2;

void onConnected() {
  Serial.println("[user] connected");
  digitalWrite(LED_PIN, HIGH);
}

void onDisconnected() {
  Serial.println("[user] disconnected");
  digitalWrite(LED_PIN, LOW);
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  s3xy_on_connect(onConnected);
  s3xy_on_disconnect(onDisconnected);

  s3xy_begin("ENH_BTN");
}

void loop() {
  s3xy_loop();

  if (s3xy_ready() && Serial.available()) {
    char input = Serial.read();

    if (input == 'p') {
      s3xy_send_single();
    } else if (input == 'd') {
      s3xy_send_double();
    } else if (input == 'l') {
      s3xy_send_long();
    }
  }

  delay(100);
}
