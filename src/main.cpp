#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <Ticker.h>
#include "AsyncPing.h"            // https://github.com/akaJes/AsyncPing

#define WIFI_SSID "HuNor"        // Wi-Fi network SSID (network name)
#define WIFI_PSK  "1234554321"         // Wi-Fi network pre-shared key (password)

#define INTERNET_LOST     300     // seconds since last received ping before the Internet connection is deemed lost 
#define PING_INTERVAL      10     // seconds between ping requests 
#define REPORT_INTERVAL   120     // seconds between report on ping sent and received counts
#define PING_SAMPLE_SIZE   10     // minimum ping count for testing for targets that don't reliably reply, should be 10+
#define UNRELIABLE          7     // target unreliable if response rate is less than UNRELIABLE/10
#define RESET_COUNTER   90000     // ping sent and received counts reset after RESET_COUNTER requests sent to all targets 
#define TARGET_COUNT        1     // number of ping targets
#define RELAY_PIN D7
// target hosts that will be pinged on a regular basis, can be identified by host name or IP address
const char* pingHosts[TARGET_COUNT] = {"8.8.8.8"};  // with one bad host name to test setupTargets()

/*-------------------------------------------------------------------------------------*/

IPAddress targets[TARGET_COUNT];    // array of valid IP addresses of ping targets
int pingSentCount[TARGET_COUNT];    // number of pings sent to each target
int pingRcvdCount[TARGET_COUNT];    // number of replies received from each target
int hostsIndex[TARGET_COUNT];       // reverse index from last 3 arrays to pingHosts array
int targetCount = 0;                // number of valid IP addresses in targets array
int pingIndex = 0;                  // index of next target to ping

unsigned long lastValidPing = 0;    // the last time a ping was received from a target

AsyncPing targetPinger;             // object to send successive pings to the target sites
AsyncPing userPinger;               // object to send ping request to a user specified host

Ticker pingTimer;                   // object to time the sending of pings to target sites 
Ticker reportTimer;                 // object to time reporting on the status of sent ping requests


// print statistics about sent and received ICMP packets and warn about unreliable targets
void reportTargetStatus(void) {
     if (targetCount < 1) return;
     Serial.printf("\n%lu: Ping target : received / sent  counts\n", millis());
     for (int k=0; k<targetCount; k++) {
       bool unreliable = ( (pingSentCount[k] > PING_SAMPLE_SIZE) && (pingRcvdCount[k] < (int) ((UNRELIABLE*pingSentCount[k])/10)) );
       Serial.printf("     %s : %d / %d%s\n", pingHosts[hostsIndex[k]], pingRcvdCount[k], pingSentCount[k], (unreliable) ? " *** WARNING: unreliable target ***" : "");
     } 
     Serial.println();
}

// reset the send and receive statistics
void resetTargetStatus(void) {
     pingIndex = 0;
     memset(pingSentCount, 0, sizeof(pingSentCount));
     memset(pingRcvdCount, 0, sizeof(pingRcvdCount));
}

// pingHost URL's and IP addresses to IPAddress objects
void setupTargets(void) {
     targetCount = 0;
     for (int i = 0; i < TARGET_COUNT; i++) {
       if (WiFi.hostByName(pingHosts[i], targets[targetCount])) {
         hostsIndex[targetCount] = i;
         targetCount++;
       } else {
         Serial.printf("\"%s\" is not a valid host name or Ip address\n", pingHosts[i]);
       }  
     }
     resetTargetStatus();
}

// send a ping request to the next valid targer IP address and increment its sent statistic
void sendTargetPing() {  
     if (targetCount < 1) return;
     if ((pingIndex == 0) && (pingSentCount[0] > RESET_COUNTER)) resetTargetStatus();
     Serial.printf("%lu: Sending ping to target[%d] %s\n", millis(), pingIndex, pingHosts[hostsIndex[pingIndex]]);
     targetPinger.begin(targets[pingIndex], 1, 5000);  // 1 ping, timeout in 5 seconds
     pingSentCount[pingIndex]++;
     pingIndex = (pingIndex+1)%targetCount;
}

// function that is called when a ping reply arrives from one of the target hosts
bool targetPingerCallback(const AsyncPingResponse& response) {
     if (response.answer) {
       for (int j = 0; j < targetCount; j++) {
         if (response.addr == targets[j]) {
           Serial.printf("%lu: Ping reply from target[%d] %s received\n", millis(), j, pingHosts[hostsIndex[j]]);
           // if (millis() % 2 == 0)  // remove leading // to test unreliable target report
           pingRcvdCount[j]++;       
           break;
         }
       }  
       lastValidPing = millis();  // add leading // to test ping failure
     }  
     return true; // done
    }

// send a ping request to a specific host. ipaddress can be a URL or an IP address
void sendUserPing(const char* ipaddress, u8_t count = 3, u32_t timeout = 1000) {
     IPAddress ip;
      if (WiFi.hostByName(ipaddress, ip)) {
       Serial.printf("%lu: Sending ping to %s (%s)\n", millis(), ipaddress, ip.toString().c_str());
       userPinger.begin(ip, count, timeout);  // 3 pings, timeout 1000 these are the default values
     } else {             
       Serial.printf("%lu: Could not create valid IP address for %s\n", millis(), ipaddress);
     }    
}

// function that is called when a ping reply arrives from the user specified host
bool userPingerRecvCallback(const AsyncPingResponse& response) {
     IPAddress addr(response.addr); //to prevent with no const toString() in 2.3.0
     if (response.answer)
       Serial.printf("%lu: %d bytes from %s: icmp_seq=%d ttl=%d time=%lu ms\n", millis(), response.size, addr.toString().c_str(), response.icmp_seq, response.ttl, response.time);
     else
       Serial.printf("%lu: no reply yet from %s icmp_seq=%d\n", millis(), addr.toString().c_str(), response.icmp_seq);
     return false; //do not stop
}

// function that is called when the user ping request times out
bool userPingerFinalCallback(const AsyncPingResponse& response) {
     IPAddress addr(response.addr); //to prevent with no const toString() in 2.3.0
     Serial.printf("%lu: %d pings sent to %s, %d received, time: %lu ms\n", millis(), response.total_sent, addr.toString().c_str(), response.total_recv, response.total_time);
     if (response.mac)
       Serial.printf("  detected eth address " MACSTR "\n",MAC2STR(response.mac->addr));
     Serial.println();
     return true;  // done (does not matter)
}


void setup() {
     // setup the serial connection
     Serial.begin(115200);
     while(!Serial) delay(10);
     Serial.println();
     Serial.println();
     pinMode(RELAY_PIN, OUTPUT); // Set the relay pin as an output
     // setup the Wi-Fi connection
     WiFi.disconnect(true);
     WiFi.mode(WIFI_STA);
     WiFi.begin(WIFI_SSID, WIFI_PSK);

     Serial.print("Wait for WiFi ");

     while (WiFi.status() != WL_CONNECTED) {
       delay(500);
       Serial.print(".");
     }

     Serial.print("\nWiFi connected, IP address: ");
     Serial.print(WiFi.localIP());
     Serial.print(", gateway IP address: ");
     Serial.println(WiFi.gatewayIP());
     Serial.println("\n");

     // intialize the targets[] array of IP addresses based on the given pingHosts
     setupTargets();

     // setup targetPinger, the targets pinger
     targetPinger.on(true,  targetPingerCallback);

     // setup userPinger, the one-off ping to a user specified host
     userPinger.on(true, userPingerRecvCallback);
     userPinger.on(false, userPingerFinalCallback);

     // setup the timers that will run target pinging and status reporting in the background
     pingTimer.attach(PING_INTERVAL, sendTargetPing);  // send a ping to a target every 10 seconds
     reportTimer.attach(REPORT_INTERVAL, reportTargetStatus);  // report the status every two minutes

     // initialize the on-board LED
     pinMode(LED_BUILTIN, OUTPUT);
     digitalWrite(LED_BUILTIN, HIGH); // turn LED off (using a Lolin/Wemos D1 mini for testing)

     Serial.printf("Setup completed with %d ping targets in place\n", targetCount);
     lastValidPing = millis();

     delay(1000);
     Serial.printf("Remaining free mem: %u\n\n", ESP.getFreeHeap());
}


void blinkLED(void) {
     for (int i=0; i<4; i++) {  // loop limit should be an even integer
       digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
       delay(50);
     }  
}

unsigned long lastping = 0;        // time of last "user" ping
unsigned long waitTime = 60*1000;  // interval before next "user" ping (1 to 2 minutes)

void loop() {

     // report ping failure
     if (millis() - lastValidPing > INTERNET_LOST*1000) {
       Serial.printf("%lu: **** PINGING FAILED **** NO PING IN LAST %d SECONDS ****\n", millis(), INTERNET_LOST);
       lastValidPing = millis();  
        digitalWrite(RELAY_PIN, HIGH); // Turn on the relay
        delay(20000);                   // Wait for 20 second
        digitalWrite(RELAY_PIN, LOW);  // Turn off the relay                 // restart test
     }

     delay(2000);
     blinkLED();
}