/* CHR Internet codec and streaming server control firmware
    (c) A.G. Doswell 5th March 2020

    Hardware details at andydoz.blogspot.com

    V1.0

    Arduino measures received (monitor) audio from the VU drive precision rectifier on A0, and if there's been no audio for a while,
    Check there's incoming audio from the VU drive precision rectifier on A3, and check the internet. If there's incoming audio present, and the internet's OK, then
    restart the receiving computer. If that doesn't put it right, restart the transmitting computer. If it all goes belly up , self re-set the controller.

    There's a push button connected to Pin 2, and also to the shutdown pins of the Pi's via a diode splitter. Pushing this will shutdown the Pi's and appear to shutdown the system,
    It won't restart until the mains is cycled.

*/
#include <EtherCard.h>
#define networkRSTPin 9
#define rstPin 4
#define ledPin 3
#define ETHERCARD_RETRY_LATECOLLISIONS 0
#define shutdownInterruptPin 2
#define lampPin 8
#define RXRebootPin 6
#define TXRebootPin 5
#define shutDownPin 7

// ethernet interface mac address, must be unique on the LAN
static byte mymac[] = { 0x74, 0x69, 0x69, 0x2D, 0x30, 0x31 };
byte Ethernet::buffer[400];
static uint32_t timer;
unsigned int error = 0;
unsigned int pingError;
static uint32_t failTimer;
const unsigned int audioThreshold = 2; // Dead air detector threshold
boolean RXresetFlag = 0;
int pingLoop;
boolean giveUp;
unsigned int internetFailCounter;
volatile boolean shutDownFlag;
unsigned long RXAverage;
unsigned long TXAverage;

void setup() {
  pinMode (shutdownInterruptPin, INPUT_PULLUP); // this sets up the shutdown interrupt
  attachInterrupt(digitalPinToInterrupt(shutdownInterruptPin), shutDown, FALLING);
  pinMode (lampPin, OUTPUT); // switch VU lamps on
  digitalWrite (lampPin, HIGH);
  pinMode (shutDownPin, INPUT); //These are the Pins to shutdown/reboot the Pi's - set to input so they're high impedance
  pinMode (RXRebootPin, INPUT);
  pinMode (TXRebootPin, INPUT);
  pinMode (ledPin, OUTPUT); // error led connected to pin 3
  Serial.begin(57600); //start the diagnostic interface
  timer = -9999999; // these are the ping return timers
  failTimer = -9999999;
  pinMode (networkRSTPin, OUTPUT);// extra pin to reset the ENC28J60 ethernet interface
  pinMode (rstPin, INPUT); //RST line to a high impedance state
  dhcpLease(); // get an IP address
  delay (180000); //  wait for 3 minutes to allow the Pi's to start up
  analogReference(INTERNAL);
}

void loop() {

  if (shutDownFlag) { // this is set true if the shut down interrupt is, er , interrupted.
    Serial.println(F("Shutting Down."));
    pinMode (shutDownPin, OUTPUT);
    digitalWrite (shutDownPin, LOW);
    delay (500);
    pinMode (shutDownPin, INPUT);
    for (int i = 0; i <= 120; i++) { // allow 1 minute for the Pi's to safely shut down, and blink the VU lamps & error LED while they shut down, to give the user a clue.
      digitalWrite (lampPin, LOW);// VU lamps off
      digitalWrite (ledPin, LOW); // error LED off
      Serial.print (F("."));
      delay (250);
      digitalWrite (lampPin, HIGH);  //VU lamps on
      digitalWrite (ledPin, HIGH); // error LED on
      delay (250);
    }
    Serial.println(F(""));
    digitalWrite (lampPin, LOW); // VU lamps off.
    digitalWrite (ledPin, LOW); // Error LED off, unit appears to be off.
    Serial.println(F("In Standby. Power cycle mains to restart."));
    while (1); // Wait forever
  }

  if (giveUp) { // All the resetting etc just isn't working, restart the monitor
    error = 5;
    error_output();
    Serial.println(F("Resetting controller"));
    delay (1000);
    pinMode (rstPin, OUTPUT);
    digitalWrite(rstPin, LOW); //reset the arduino, restarting everything.
    //nothing ever gets here
  }

  if (!testRX()) { // Test to see if we have monitor audio, if we haven't read on..
    if (!testTX()) { // test to see if we have incoming audio, if we haven't there's not much we can do, so flash the error led.
      Serial.println(F("TX Audio fail"));
      error = 3;
      pingLoop = 0;
    }
    else { // if the incoming audio is good, check the internet
      Serial.println(F("TX Audio OK"));
      pingLoop = 0;
      if (!testInternet()) { // tests internet connectivity
        Serial.println(F("Internet fail"));
        internetFailCounter++;
        error = 4;
        if (internetFailCounter >= 10) { // if the internet fails 10 times, reset the monitor in a vain attempt to restore some connectivity.
          giveUp = true;
          internetFailCounter = 0;
        }
      }
      else {// we've got incoming audio and internet, restart the receiving pi
        Serial.println(F("Monitor audio fail, internet OK"));
        if (!RXresetFlag) { // restart the receiving pi
          resetRX();
          RXresetFlag = true;
        }
        else { // ... and if that doesn't work, restart the transmitting pi
          resetTX();
          RXresetFlag = false;
          giveUp = true;
        }
        delay (10000);
      }
    }

  }
  else {
    RXresetFlag = false;
  }
  error_output ();
}

void error_output() { // flash the error LED
  Serial.print(F("Error output:"));
  Serial.println(error);
  int counter = 0;
  while (counter != error) {
    digitalWrite (ledPin, HIGH);
    delay (500);
    digitalWrite (ledPin, LOW);
    delay (250);
    counter++;
  }
  delay (1000);

}

boolean testRX() {
  for (unsigned int i = 0;  i <= 30000; i++) { // get 3000 samples from our received audio, and average them
    unsigned long RXaudioValue = analogRead (A0);
    RXAverage = RXAverage + RXaudioValue;
  }
  RXAverage = RXAverage / 30000;
  if (RXAverage >= audioThreshold) { // if the audio is above our minimum threshold, then return true
    Serial.print(RXAverage);
    Serial.println(F(":Monitor good"));
    error = 0;
    return true;
  }
  else {
    Serial.print(RXAverage); // and if it isn't return false
    Serial.println(F(":Monitor fail"));
    return false;
  }
}

boolean testTX() {
  for (unsigned int i = 0; i <= 30000; i++) {// get 30000 samples from our incoming audio, and average them
    unsigned long TXaudioValue = analogRead (A3);
    TXAverage = TXAverage + TXaudioValue;
  }
  TXAverage = TXAverage / 30000;
  Serial.println (TXAverage);
  if (TXAverage >= audioThreshold) { // if the audio is above our minimum threshold, then return true
    Serial.println(F("Incoming audio OK"));
    return true;
  }
  else {
    Serial.println(F("Incoming audio failed, please check source")); // and if it isn't return false
    return false;
  }
}

boolean testInternet() { // ping google
  word len = ether.packetReceive(); // go receive new packets
  word pos = ether.packetLoop(len); // respond to incoming pings
  // report whenever a reply to our outgoing ping comes back
  if (len > 0 && ether.packetLoopIcmpCheckReply(ether.hisip)) {
    failTimer = micros();
    pingError = 0;
    Serial.print(F("Ping  "));
    Serial.print((micros() - timer) * 0.001, 3);
    Serial.print(F(" ms Errors:"));
    Serial.println(pingError);
  }
  if ((micros() - failTimer >= 999999 ) ) {
    Serial.println(F("Ping failed"));
    pingError++;
    failTimer = micros();
  }
  // ping a remote server once every few seconds
  if (micros() - timer >= 1000000) {
    Serial.print(pingLoop);
    Serial.print(F(" Errors:"));
    Serial.print(pingError);
    ether.printIp(" Pinging: ", ether.hisip);
    timer = micros();
    failTimer = micros();
    pingLoop++;
    ether.clientIcmpRequest(ether.hisip);
  }

  if (pingLoop >= 5) { // if we've not got good pings back, then return false
    if (pingError >= 1) {
      digitalWrite (ledPin, 1);
      pingError = 0;
      return false;
    }
    else {
      digitalWrite (ledPin, 0); // if we've got good pings, return true
      pingError = 0;
      return true;
    }
  }
  if (pingLoop < 5) {
    testInternet ();
  }
}

void resetRX () {
  Serial.println(F("Resetting monitor receiver")); // next lines pull the reboot pin low, and then return it to a high impedance state
  pinMode (RXRebootPin, OUTPUT);
  digitalWrite (RXRebootPin, LOW);
  delay (500); // wait a bit
  pinMode (RXRebootPin, INPUT);
  delay (120000); // delay for 3 mins to allow the pi to shutdown and restart
}

void resetTX () {
  Serial.println(F("Resetting transmitting computer"));
  pinMode (TXRebootPin, OUTPUT);
  digitalWrite (TXRebootPin, LOW);
  delay (500); // wait a bit
  pinMode (TXRebootPin, INPUT);
  delay (180000);
}

void dhcpLease () { // get an IP from the local DHCP server, and look up google's IP address.
  error = 0;
  Serial.println(F("DHCP lease renew"));
  digitalWrite (ledPin, HIGH); // Turn the Error LED on whilst this is running. If it gets stuck here, then the LED will warn us.
  digitalWrite (networkRSTPin, LOW); // reset ethernet interface
  delay (100);
  digitalWrite (networkRSTPin, HIGH);
  pinMode (networkRSTPin, INPUT);
  delay (1000);
  ether.begin(sizeof Ethernet::buffer, mymac, SS); //seems daft, but it doesn't always work first time.
  ether.begin(sizeof Ethernet::buffer, mymac, SS);
  ether.begin(sizeof Ethernet::buffer, mymac, SS);

  if (ether.begin(sizeof Ethernet::buffer, mymac, SS) == 0) {
    Serial.println(F("Failed to access Ethernet controller"));
  }

  Serial.println(F("Request DHCP..."));
  if (!ether.dhcpSetup()) {
    Serial.println(F("DHCP failed"));
    dhcpLease();
  }

  ether.printIp("IP:  ", ether.myip);
  ether.printIp("GW:  ", ether.gwip);

  // use DNS to locate the IP address we want to ping
  if (!ether.dnsLookup(PSTR("www.google.com"))) {
    Serial.println(F("DNS failed"));
    return false;
  }

  ether.printIp("SRV: ", ether.hisip);
  failTimer = micros ();
  digitalWrite (ledPin, LOW);
  return true;
}

void shutDown () {
  shutDownFlag = true;
}
