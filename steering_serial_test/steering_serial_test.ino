/*
 * Standalone ESP32 steering motor test using the existing driver wiring.
 * LPWM = GPIO32, RPWM = GPIO33. Serial Monitor = 115200 baud.
 * A = left, D = right, S or 0 = stop, + / - = adjust PWM by 10.
 * No CAN, WiFi, sensors, or external libraries are required.
 * Motion continues until stopped; this test has no steering angle limits.
 */
#include <Arduino.h>

constexpr int MOTOR_LPWM = 32;
constexpr int MOTOR_RPWM = 33;
int motorSpeed = 60;  // PWM duty: 0 to 255, not measured motor RPM.
char direction = 'S';

void stopMotor() {
  analogWrite(MOTOR_LPWM, 0);
  analogWrite(MOTOR_RPWM, 0);
}

void applyMotor() {
  if (direction == 'A') {
    analogWrite(MOTOR_RPWM, 0);
    analogWrite(MOTOR_LPWM, motorSpeed);
  } else if (direction == 'D') {
    analogWrite(MOTOR_LPWM, 0);
    analogWrite(MOTOR_RPWM, motorSpeed);
  } else {
    stopMotor();
  }
}

void setup() {
  pinMode(MOTOR_LPWM, OUTPUT);
  pinMode(MOTOR_RPWM, OUTPUT);
  stopMotor();
  Serial.begin(115200);
  Serial.println("A=LEFT | D=RIGHT | S/0=STOP | +=FASTER | -=SLOWER");
  Serial.println("Starting speed: 60/255. Motor stopped.");
}

void loop() {
  if (!Serial.available()) return;
  char command = Serial.read();
  if (command >= 'a' && command <= 'z') command -= ('a' - 'A');

  char nextDirection = direction;
  switch (command) {
    case 'A': nextDirection = 'A'; break;
    case 'D': nextDirection = 'D'; break;
    case 'S':
    case '0': nextDirection = 'S'; break;
    case '+': motorSpeed = min(motorSpeed + 10, 255); break;
    case '-': motorSpeed = max(motorSpeed - 10, 0); break;
    default: return;  // Ignore line endings and unsupported characters.
  }

  if (motorSpeed == 0) nextDirection = 'S';

  // Switch both PWM outputs off briefly before reversing direction.
  if (direction != 'S' && nextDirection != 'S' && nextDirection != direction) {
    stopMotor();
    delay(100);
  }
  direction = nextDirection;
  applyMotor();

  Serial.printf("%s | Speed setting: %d/255\n",
                direction == 'A' ? "LEFT" : direction == 'D' ? "RIGHT" : "STOP",
                motorSpeed);
}
