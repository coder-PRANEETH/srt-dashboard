/*
 * ESP32 accelerator ADC test. Open Serial Monitor at 115200 baud.
 * Prints only the raw GPIO34 reading (0-4095), every 100 ms.
 * GPIO34 input must stay within 0-3.3 V, with a shared ground.
 * Do not directly connect a pedal output that reaches 4.52 V.
 */

constexpr int ACCELERATOR_PIN = 34;

void setup() {
  Serial.begin(115200);
  pinMode(ACCELERATOR_PIN, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(ACCELERATOR_PIN, ADC_11db);
}

void loop() {
  Serial.println(analogRead(ACCELERATOR_PIN));
  delay(100);
}
