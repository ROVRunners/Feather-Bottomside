//?--------------------------------
//? PROPERTY OF CASEY SPARKS
//? ORIGINAL FEATHER PROTOTYPE
//? IF YOU STEAL THIS CODE MY LAWYER WILL BE IN TOUCH
//?--------------------------------

#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <Ethernet.h>          // for the Ethernet FeatherWing
#include <PubSubClient.h>      // MQTT — the feather listens and obeys
#include <Wire.h>              // I2C, the feather talks to its friends
#include <mavlink.h>           // flight controller comms (do not touch)
#include <ESP32Servo.h>        // because the thrusters yearn for microseconds
// #include <grammarly.h>        // Just needed this for a sec

enum PinModeType {
  MODE_NONE,
  MODE_STEADY,
  MODE_PWM_US,
  MODE_PWM_PERCENT,
  MODE_READ_DIGITAL,
  MODE_READ_ANALOG
};

struct ManagedPin {
  bool active = false;
  String name = "";
  int pinNumber = -1;
  PinModeType mode = MODE_NONE;
  int value = 0;      // digital value, microseconds, percent, or last read value
  int frequency = 50; // used for PWM%
};

static const int MAX_PINS = 16;

ManagedPin pins[MAX_PINS];
Servo servoOutputs[MAX_PINS];
bool servoAttached[MAX_PINS] = {false};

String rovStatus = "booting";

// Change these to match your network.
byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0x01 };
IPAddress mqttServer(192, 168, 1, 200);
EthernetClient ethClient;
PubSubClient mqtt(ethClient);

unsigned long lastPublishMs = 0;
const unsigned long publishIntervalMs = 100;  // 10 Hz telemetry to start

// ESC/truster sanity zone. if you send garbage here the prop gods will collect.
const int PWM_US_MIN = 1100;
const int PWM_US_MAX = 1900;
const int PWM_US_NEUTRAL = 1500;

PinModeType parseMode(const String& s) {
  if (s == "Steady") return MODE_STEADY;
  if (s == "PWMus") return MODE_PWM_US;
  if (s == "PWM%") return MODE_PWM_PERCENT;
  if (s == "ReadDigital") return MODE_READ_DIGITAL;
  if (s == "ReadAnalog") return MODE_READ_ANALOG;
  return MODE_NONE;
}

ManagedPin* findPinByName(const String& name) { // wish i had this function for my college. to find my ex.
  for (int i = 0; i < MAX_PINS; i++) {
    if (pins[i].active && pins[i].name == name) return &pins[i];
  }
  return nullptr;
}

int getPinIndexByName(const String& name) {
  for (int i = 0; i < MAX_PINS; i++) {
    if (pins[i].active && pins[i].name == name) return i;
  }
  return -1;
}

ManagedPin* getOrCreatePin(const String& name) {  
  ManagedPin* p = findPinByName(name);
  if (p) return p;

  for (int i = 0; i < MAX_PINS; i++) {
    if (!pins[i].active) {
      pins[i].active = true;
      pins[i].name = name;
      pins[i].pinNumber = -1;
      pins[i].mode = MODE_NONE;
      pins[i].value = 0;
      pins[i].frequency = 50;
      return &pins[i];
    }
  }
  return nullptr;
}

void detachServoIfNeeded(int index) {
  if (index < 0 || index >= MAX_PINS) return;

  if (servoAttached[index]) {
    servoOutputs[index].detach();
    servoAttached[index] = false;
  }
}

void attachServoIfNeeded(int index, int pinNumber) {
  if (index < 0 || index >= MAX_PINS) return;
  if (pinNumber < 0) return;

  if (!servoAttached[index]) {
    // behold: a ritual binding between silicon and spinny water knife
    servoOutputs[index].setPeriodHertz(50);
    servoOutputs[index].attach(pinNumber, 1000, 2000);
    servoAttached[index] = true;
  }
}

void applyPin(ManagedPin& p) {
  if (p.pinNumber < 0) return;

  int index = getPinIndexByName(p.name);
  if (index < 0) return;

  switch (p.mode) {
    case MODE_STEADY:
      detachServoIfNeeded(index);
      pinMode(p.pinNumber, OUTPUT);
      digitalWrite(p.pinNumber, p.value ? HIGH : LOW);
      break;

    case MODE_PWM_US: {
      attachServoIfNeeded(index, p.pinNumber);

      int us = constrain(p.value, PWM_US_MIN, PWM_US_MAX);
      servoOutputs[index].writeMicroseconds(us);
      break;
    }

    case MODE_PWM_PERCENT: {
      detachServoIfNeeded(index);
      pinMode(p.pinNumber, OUTPUT);

      // this mode is not the chosen one. it is the forgotten prince.
      int pwm = map(constrain(p.value, 0, 100), 0, 100, 0, 255);
      analogWrite(p.pinNumber, pwm);
      break;
    }

    case MODE_READ_DIGITAL:
      detachServoIfNeeded(index);
      pinMode(p.pinNumber, INPUT);
      break;

    case MODE_READ_ANALOG:
      detachServoIfNeeded(index);
      // analogRead-capable pins only. choose wisely or perish noisily.
      break;

    default:
      detachServoIfNeeded(index);
      break;
  }
}

void stopAllOutputs() {
  for (int i = 0; i < MAX_PINS; i++) {
    if (!pins[i].active) continue;

    if (pins[i].mode == MODE_STEADY) {
      pins[i].value = 0;
      applyPin(pins[i]);
    }

    if (pins[i].mode == MODE_PWM_US) {
      // neutral. because full send during stop would be an act of terrorism.
      pins[i].value = PWM_US_NEUTRAL;
      applyPin(pins[i]);
    }

    if (pins[i].mode == MODE_PWM_PERCENT) {
      pins[i].value = 0;
      applyPin(pins[i]);
    }
  }
}

void publishPinTelemetry() {
  char topic[96];
  char payload[24];

  for (int i = 0; i < MAX_PINS; i++) {
    if (!pins[i].active) continue;
    if (pins[i].pinNumber < 0) continue;

    int reading = 0;  // academic performance simulator
    bool canPublish = false;

    switch (pins[i].mode) {
      case MODE_READ_DIGITAL:
        reading = digitalRead(pins[i].pinNumber);
        canPublish = true;
        break;

      case MODE_READ_ANALOG:
        reading = analogRead(pins[i].pinNumber);
        canPublish = true;
        break;

      default:
        // for outputs we publish commanded value because asking the feather what it "feels" is not science
        reading = pins[i].value;
        canPublish = true;
        break;
    }

    if (canPublish) {
      snprintf(topic, sizeof(topic), "ROV/gpio/%s", pins[i].name.c_str());
      snprintf(payload, sizeof(payload), "%d", reading);
      mqtt.publish(topic, payload, true);
    }
  }

  // if the rov is full of water this thing aint workin. its fried bro.
  mqtt.publish("ROV/status", rovStatus.c_str(), true);
}

void handleCommand(const String& command) {
  if (command == "stop") {  // stop means stop!
    rovStatus = "stopped";
    stopAllOutputs();
  } else if (command == "restart") {
    rovStatus = "running";
  } else if (command == "shutdown") {
    rovStatus = "shutdown";
    stopAllOutputs();
  }
}

void handlePinsTopic(const String& pinName, const String& field, const String& value) {
  if (rovStatus == "stopped") return;

  ManagedPin* p = getOrCreatePin(pinName);
  if (!p) return;

  if (field == "id") {
    p->pinNumber = value.toInt();
  } else if (field == "mode") {
    p->mode = parseMode(value);
  } else if (field == "val") {
    p->value = value.toInt();
  } else if (field == "freq") {
    p->frequency = value.toInt();
  }

  applyPin(*p);
}

// expects topics like:
// PC/commands/stop
// PC/pins/light/id
// PC/pins/light/mode
// PC/pins/light/val
// PC/pins/light/freq
void mqttCallback(char* topicC, byte* payload, unsigned int length) {
  String topic = String(topicC);
  String value;
  for (unsigned int i = 0; i < length; i++) value += (char)payload[i];

  int s1 = topic.indexOf('/');
  int s2 = topic.indexOf('/', s1 + 1);
  int s3 = topic.indexOf('/', s2 + 1);
  int s4 = topic.indexOf('/', s3 + 1);

  if (s1 < 0 || s2 < 0) return;

  String sender = topic.substring(0, s1);
  String category = topic.substring(s1 + 1, s2);

  if (sender != "PC") return;

  if (category == "commands") {
    String cmd = topic.substring(s2 + 1);
    handleCommand(cmd);
    return;
  }

  if (category == "pins") {
    if (s3 < 0 || s4 < 0) return;
    String pinName = topic.substring(s2 + 1, s3);
    String field = topic.substring(s3 + 1);
    handlePinsTopic(pinName, field, value);
    return;
  }

  // Future:
  // wife     == "Grace VanderWaal"
  // category == "i2c"
  // category == "mavlink"
}

void reconnectMQTT() {
  while (!mqtt.connected()) {
    if (mqtt.connect("ROV")) {
      mqtt.subscribe("PC/#");
      rovStatus = "running";
      mqtt.publish("ROV/status", rovStatus.c_str(), true);
    } else {
      delay(1000);
    }
  }
}

void setup() {
  rovStatus = "booting";

  Ethernet.begin(mac);   // DHCP

  /*
  This exact line of code is what landed me my big break working for Mcdonald's Software Engineering program.
  The fryer needed to block wait instead of busy wait to make sure there are no interrupts to the FPGA running the advanced fry timer display.
  I introduced a block wait and the DMA handled the fry timer while the frier hold on.

  Anyways this made sure there was a huge delay from the time the timer went off for the fries and the fryer turning off so that the employees are frustrated.
  It's a technique used to condition them to love the corporation. I was a part of the biggest social manipulation of food service employees in the world.
  When I told them I introduced the delay because I pretty much know everything, not just about computer architecture but everything, they instantly hired my ass.
  When they saw my deviant art profile they instantly fired my ass. You live you learn I guess. Guy can't have a private personal life anymore. smh
  */
  delay(1000);

  mqtt.setServer(mqttServer, 1883);
  mqtt.setCallback(mqttCallback);

  reconnectMQTT();
  rovStatus = "running";
}

void loop() {
  if (!mqtt.connected()) reconnectMQTT();
  mqtt.loop();

  unsigned long now = millis();
  if (now - lastPublishMs >= publishIntervalMs) {
    lastPublishMs = now;
    publishPinTelemetry();
  }
}