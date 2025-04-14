#include <Arduino.h>
#include <WiFi.h>
#include "WiFiClientSecure.h"
#include "time.h"
#include <NostrEvent.h>
#include <NostrRelayManager.h>
#include <NostrQueueProcessor.h>
#include <vector>
#include <Preferences.h>
#include <WebServer.h>

//--------how to use the device--------
// After successful compiling to your ESP32, plug in the device and connect to the WIFI GMGNcontroller with your smartphone
// or computer. Use password gmgnprotocol to connect to it. When you are connected to this wifi 
// go to 192.168.4.1 and configure the device. You can set the button messages for GM or GN, random sticky strings which should
// be separated by <<<>>> are used for appending random string to your button message, your wifi credentials,
// allowed time windows to disable buttons in certain times of the day, Nostr keys, and relays you like to post your messages to.
// After saving the configuration, the device will restart and show the wifi access point again. Check if everything
// is set up correctly. WiFi AP is always on for 7 min after start of the device and it will turn off wifi after this time expires
// for security reasons. After that you can start using your overcomplicated GM,GN controller.
// To access the settings you can always turn off/on your GMGN controller and visit the settings page.

// -----------------------------------------------------
// Global Variables and Definitions
// -----------------------------------------------------

String deviceName = "GMGNcontroller";
String configSSID, configPass, configNsec, configNpub, btn1Msg, btn1Sticky, btn2Msg, btn2Sticky, userName;
std::vector<String> configRelays;

// New time window variables (for UTC allowed periods)
// CODE IS USING UTC TIME NOT LOCAL !!!
String btn1_start = "04:00"; // Allowed start for GM button (which sends btn1)
String btn1_end   = "11:00"; // Allowed end for GM button
String btn2_start = "19:00"; // Allowed start for GN button (which sends btn2)
String btn2_end   = "03:59"; // Allowed end for GN button

#define BUTTON_GN 14    // Button for GN (posts btn2 message)
#define BUTTON_GM 27    // Button for GM (posts btn1 message)
#define LED_GREEN 33    // Green LED: success feedback (active LOW)
#define LED_RED   32    // Red LED: failure feedback (active LOW)
#define LED_YELLOW 25   // Yellow LED: processing feedback (active LOW)

unsigned long lastPostTime = 0;
// const unsigned long cooldownTime = 5000; // 5 sec cooldown for testing
const unsigned long cooldownTime = 1800000; // 30min cooldown (a time period we need to wait for another button press to work)

// I tried to include a custom tag device in nostr event JSON but I was getting a bad event id because modifying the JSON (to add the device tag) invalidates the precomputed hash and signature.
// I need to either generate the event with tags in one step (by changing the library) or avoid modifying the JSON afterward. This might come in the future maybe.
// String configDeviceName;  // New: holds the configurable device name

// For button edge detection
bool lastStateGN = false;
bool lastStateGM = false;
unsigned long pressTimeGN = 0;
unsigned long pressTimeGM = 0;

// For event feedback (non‑blocking)
bool eventInProgress = false;         // True while an event is being processed
unsigned long eventSentTime = 0;        // Time when the event was sent
const unsigned long CONFIRM_TIMEOUT = 20000; // 20 sec timeout for confirmation
String pendingPrefix = "";              // For Morse-code feedback

// For request confirmation (using event requests)
String lastEventID = "";  // Holds the event ID of the note we just sent

// Global flags set by callbacks:
volatile bool eventConfirmed = false;   // Set by our kind1 callback when the event is found
volatile bool isRelayConnected = false;   // Set by relayConnectionHandler callback

// We'll store the boot time to later disable the AP after 7 minutes.
unsigned long bootTime = 0; // Global variable (already declared above)
bool apDisabled = false;    // Flag to indicate if AP has been disabled

// -----------------------------------------------------
// Library Objects
// -----------------------------------------------------
NostrEvent nostr;
NostrRelayManager nostrRelayManager;
NostrQueueProcessor nostrQueue;
Preferences preferences;
WebServer server(80);


// -----------------------------------------------------
// Helper Functions for Btns Time Window Checking
// -----------------------------------------------------
int timeStringToMinutes(String timeStr) {
  // Expects timeStr in "HH:MM" format.
  int sep = timeStr.indexOf(":");
  if (sep == -1) return 0;
  int hour = timeStr.substring(0, sep).toInt();
  int minute = timeStr.substring(sep+1).toInt();
  return hour * 60 + minute;
}

bool inTimeWindow(String startStr, String endStr) {
  int startMin = timeStringToMinutes(startStr);
  int endMin = timeStringToMinutes(endStr);

  // Get current UTC time from getLocalTime (if configTime was called with offset 0)
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return false;
  int nowMin = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  
  if (startMin <= endMin) {
    return (nowMin >= startMin && nowMin < endMin);
  } else {
    // Window wraps around midnight.
    return (nowMin >= startMin || nowMin < endMin);
  }
}

// -----------------------------------------------------
// Callback Functions
// -----------------------------------------------------
void relayConnectionHandler(const std::string &key, const char *payload) {
  static bool lastStateConnected = false;
  if (key == "connected") {
    if (!lastStateConnected) {
      Serial.println("✅ Successfully connected to a relay!");
    }
    isRelayConnected = true;
    lastStateConnected = true;
  } else if (key == "disconnected") {
    if (lastStateConnected) {
      Serial.println("❌ Disconnected from relay.");
    }
    isRelayConnected = false;
    lastStateConnected = false;
  }
}

void kind1EventCallback(const std::string &key, const char *payload) {
  String payloadStr = String(payload);
  if (payloadStr.indexOf(lastEventID) != -1) {
    Serial.println("✅ Event found in relay response!");
      digitalWrite(LED_GREEN, LOW);
      delay(700);
      digitalWrite(LED_GREEN, HIGH); 
    eventConfirmed = true;
  }
}

void okEventReceived(const std::string &key, const char *payload) {
  Serial.println("OK event received (not used for confirmation).");
  Serial.println(payload);
}

// -----------------------------------------------------
// First time configuration portal when no settings has been added
// -----------------------------------------------------
const char* apSSID_Config = "GMGNcontroller";
const char* apPassword_Config = "gmgnprotocol";
const char* htmlForm = R"rawliteral(
<!DOCTYPE html>
<html>
  <head>
    <meta charset="UTF-8">
    <title>GMGNcontroller Configuration</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
      /* Background pattern using CSS variables */
      html {
        --s: 60px; /* control the size*/
        --c1: #000000;
        --c2: #6a51cd;
        
        --c:#0000 71%,var(--c1) 0 79%,#0000 0;
        --_s:calc(var(--s)/2)/calc(2*var(--s)) calc(2*var(--s));
        background:
          linear-gradient(45deg,var(--c))
          calc(var(--s)/-2) var(--_s),
          linear-gradient(135deg,var(--c))
          calc(var(--s)/2) var(--_s),
          radial-gradient(var(--c1) 35%,var(--c2) 37%)
          0 0/var(--s) var(--s);
      }
      /* Form container with semi‑transparent background */
      body {
        font-family: Arial, sans-serif;
        margin: 20px;
        background-color: rgba(255,255,255,0.8);
        padding: 20px;
        border-radius: 8px;
        max-width: 600px;
        margin-left: auto;
        margin-right: auto;
        box-shadow: 0 0 10px rgba(0,0,0,0.3);
      }
      /* Input field styling */
      input {
        margin: 5px 0;
        padding: 5px;
        width: 100%;
        box-sizing: border-box;
      }
      /* Textarea field styling */
      #stickyStrings {
        width: 100%;
        height: 120px;
        box-sizing: border-box;
        padding: 8px;
        resize: vertical;
        }
      /* Style for buttons */
      input[type="submit"], input[type="button"] {
        width: auto;
        padding: 10px 20px;
        margin: 10px 5px;
        border: none;
        background-color: #8134df;
        color: white;
        border-radius: 4px;
        cursor: pointer;
      }
      /* Container for buttons to position them left and right */
      .single-btn-container {
        display: flex;
        justify-content: center;
        margin-top: 20px;
      }
    </style>
  </head>
  <body>
    <h1>Configure GMGNcontroller</h1>
    <form action="/save" method="POST">
      <label>Your Name:</label>
      <input type="text" name="userName" placeholder="Enter your name"><br>

      <!-- <label>Device Name:</label>
      <input type="text" name="device" placeholder="Give device a name or use default GMGNcontroller"><br>-->
      
      <label>Button 1 Message:</label>
      <input type="text" name="btn1" placeholder="Enter message for button 1"><br>
      <label>Random Sticky Strings for Button 1:</label>
      <textarea id="stickyStrings" name="btn1_sticky" placeholder="Enter sticky strings separated by <<<>>>"></textarea><br>
      <label>Button 1 Allowed Start Time (UTC, HH:MM):</label>
      <input type="time" name="btn1_start" placeholder="e.g., 04:00"><br>
      <label>Button 1 Allowed End Time (UTC, HH:MM):</label>
      <input type="time" name="btn1_end" placeholder="e.g., 11:00"><br>
      
      <label>Button 2 Message:</label>
      <input type="text" name="btn2" placeholder="Enter message for button 2"><br>
      <label>Sticky Strings for Button 2:</label>
      <textarea id="stickyStrings" name="btn2_sticky" placeholder="Enter sticky strings separated by <<<>>>"></textarea><br>
      <label>Button 2 Allowed Start Time (UTC, HH:MM):</label>
      <input type="time" name="btn2_start" placeholder="e.g., 19:00"><br>
      <label>Button 2 Allowed End Time (UTC, HH:MM):</label>
      <input type="time" name="btn2_end" placeholder="e.g., 03:59"><br>
      
      <label>WiFi SSID:</label>
      <input type="text" name="wifi_ssid" placeholder="Enter your WiFi SSID"><br>
      <label>WiFi Password:</label>
      <input type="password" name="wifi_pass" placeholder="Enter your WiFi Password"><br>
      
      <label>Nostr nsecHex:</label>
      <input type="password" name="nsecHex" placeholder="Enter your nsecHex"><br>
      <label>Nostr npubHex:</label>
      <input type="text" name="npubHex" placeholder="Enter your npubHex"><br>
      
      <label>Relays (comma separated):</label>
      <input type="text" name="relays" placeholder="e.g., wss://nos.lol/,wss://relay.damus.io/..."><br>
      
      <div class="single-btn-container">
        <input type="submit" value="Save">
      </div>
    </form>
  </body>
</html>
)rawliteral";

void handleRoot() {
  // Load saved configuration values
  preferences.begin("config", true);
  String savedUserName = preferences.getString("userName", "");
  // String configDeviceName = preferences.getString("device", "GMGNcontroller");
  String savedBtn1 = preferences.getString("btn1", "");
  String savedBtn1Sticky = preferences.getString("btn1_sticky", "");
  String savedBtn1Start = preferences.getString("btn1_start", "04:00");
  String savedBtn1End   = preferences.getString("btn1_end", "11:00");
  String savedBtn2 = preferences.getString("btn2", "");
  String savedBtn2Sticky = preferences.getString("btn2_sticky", "");
  String savedBtn2Start = preferences.getString("btn2_start", "19:00");
  String savedBtn2End   = preferences.getString("btn2_end", "03:59");
  String savedSSID = preferences.getString("wifi_ssid", "");
  String savedPass = preferences.getString("wifi_pass", "");
  String savedNsec = preferences.getString("nsecHex", "");
  String savedNpub = preferences.getString("npubHex", "");
  String savedRelays = preferences.getString("relays", "");
  preferences.end();

  Serial.println("Loaded config:");
  Serial.println("userName: " + savedUserName);
  // Serial.println("device: " + configDeviceName);
  Serial.println("btn1: " + savedBtn1);
  Serial.println("btn1_sticky: " + savedBtn1Sticky);
  Serial.println("btn1_start: " + savedBtn1Start);
  Serial.println("btn1_end: " + savedBtn1End);
  Serial.println("btn2: " + savedBtn2);
  Serial.println("btn2_sticky: " + savedBtn2Sticky);
  Serial.println("btn2_start: " + savedBtn2Start);
  Serial.println("btn2_end: " + savedBtn2End);
  Serial.println("wifi_ssid: " + savedSSID);
  Serial.println("wifi_pass: WiFi password NOT SHOWED FOR SECURITY REASONS");
  Serial.println("nsecHex: nsec NOT SHOWED FOR SECURITY REASONS");
  Serial.println("npubHex: " + savedNpub);

  // Check if savedRelays is empty and assign a default value if necessary
if (savedRelays.isEmpty()) {
  savedRelays = "wss://relay.damus.io,wss://nos.lol,wss://relay.nostr.band"; // Default relay list
}
  Serial.println("relays: " + savedRelays);

  // Create a dynamic HTML page with saved values inserted in the value attributes
  String page = "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>GMGNcontroller Configuration</title>"
                "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                "<style>"
                "html {"
                "--s: 60px;"
                "--c1: #4e6600;"
                "--c2: #000000;"
                
                "--c:#0000 71%,var(--c1) 0 79%,#0000 0;"
                "--_s:calc(var(--s)/2)/calc(2*var(--s)) calc(2*var(--s));"
                "background:"
                  "linear-gradient(45deg,var(--c))"
                  "calc(var(--s)/-2) var(--_s),"
                  "linear-gradient(135deg,var(--c))"
                  "calc(var(--s)/2) var(--_s),"
                  "radial-gradient(var(--c1) 35%,var(--c2) 37%)"
                  "0 0/var(--s) var(--s);"
                "}"
                "body {"
                "  font-family: Arial, sans-serif; "
                "  margin: 20px; "
                "  background-color: rgba(255, 255, 255, 0.8); /* white layer with opacity for better readability */"
                "  padding: 20px;"
                "}"
                "#stickyStrings {"
                  "width: 100%; "
                  "height: 120px; "
                  "box-sizing: border-box;"
                  "padding: 8px;"
                  "resize: vertical;"
                  "}"
                "input { margin: 5px 0; padding: 5px; width: 100%; }"
                "input[type='submit'], input[type='button'] { width: auto; }"
                ".btn-container {"
                "  display: flex;"
                "  justify-content: space-between;"
                "  margin-top: 20px;"
                "}"
                ".btn-container input { width: auto; margin: 0 10px; }"
                "</style>"
                "</head><body>"
                "<h1>Configure GMGNcontroller</h1>"
                "<form action='/save' method='POST'>"
                "<label>Your Name:</label>"
                "<input type='text' name='userName' placeholder='Enter your name' value='" + savedUserName + "'><br>"

                // "<label>Device Name:</label>"
                // "<input type='text' name='device' placeholder='Give device a name or use default GMGNcontroller' value='" + configDeviceName + "'><br>"

                "<label>Button 1 Message:</label>"
                "<input type='text' name='btn1' placeholder='Enter message for button 1' value='" + savedBtn1 + "'><br>"
                "<label>Random Sticky Strings for Button 1:</label>"
                "<textarea id='stickyStrings' name='btn1_sticky' placeholder='Enter sticky strings separated by <<<>>>' style='height:80px;'>" + savedBtn1Sticky + "</textarea><br>"
                "<label>GM Button Allowed Start Time (UTC, HH:MM):</label>"
                "<input type='time' name='btn1_start' placeholder='HH:MM' value='" + savedBtn1Start + "'><br>"
                "<label>GM Button Allowed End Time (UTC, HH:MM):</label>"
                "<input type='time' name='btn1_end' placeholder='HH:MM' value='" + savedBtn1End + "'><br>"
                "<label>Button 2 Message:</label>"
                "<input type='text' name='btn2' placeholder='Enter message for button 2' value='" + savedBtn2 + "'><br>"
                "<label>Random Sticky Strings for Button 2:</label>"
                "<textarea id='stickyStrings' name='btn2_sticky' placeholder='Enter sticky strings separated by <<<>>>' style='height:80px;'>" + savedBtn2Sticky + "</textarea><br>"
                "<label>GN Button Allowed Start Time (UTC, HH:MM):</label>"
                "<input type='time' name='btn2_start' placeholder='HH:MM' value='" + savedBtn2Start + "'><br>"
                "<label>GN Button Allowed End Time (UTC, HH:MM):</label>"
                "<input type='time' name='btn2_end' placeholder='HH:MM' value='" + savedBtn2End + "'><br>"
                "<label>WiFi SSID:</label>"
                "<input type='text' name='wifi_ssid' placeholder='Enter your WiFi SSID' value='" + savedSSID + "'><br>"
                "<label>WiFi Password:</label>"
                "<input type='password' name='wifi_pass' placeholder='Enter your WiFi Password' value='" + savedPass + "'><br>"
                "<label>Nostr nsecHex:</label>"
                "<input type='password' name='nsecHex' placeholder='Enter your nsecHex' value='" + savedNsec + "'><br>"
                "<label>Nostr npubHex:</label>"
                "<input type='password' name='npubHex' placeholder='Enter your npubHex' value='" + savedNpub + "'><br>"
                "<label>Relays (comma separated):</label>"
                "<input type='text' name='relays' placeholder='e.g., wss://relay.damus.io/,wss://relay.nostr.band...' value='" + savedRelays + "'><br>"
              "<div class='btn-container'>"
              "  <input type='button' value='Nuke settings' onclick=\"if(confirm('Are you sure you want to reset all the settings? This will erase all your saved configuration.')){window.location.href='/reset';}\" style='background-color: red; color: white; padding: 10px 20px; border: none; border-radius: 5px; margin-right: 10px;'>"
              "  <input type='submit' value='Save' style='background-color: green; color: white; padding: 10px 20px; border: none; border-radius: 5px; margin-left: 10px;'>"
              "</div>"
              "</form></body></html>";
  
  server.send(200, "text/html", page);
}

void handleSave() {
  if (server.method() == HTTP_POST) {
    userName = server.arg("userName");
    // String device = server.arg("device");
    String btn1 = server.arg("btn1");
    String btn1Sticky = server.arg("btn1_sticky");
    String btn1_start_val = server.arg("btn1_start");
    String btn1_end_val   = server.arg("btn1_end");
    String btn2 = server.arg("btn2");
    String btn2Sticky = server.arg("btn2_sticky");
    String btn2_start_val = server.arg("btn2_start");
    String btn2_end_val   = server.arg("btn2_end");
    String wifi_ssid = server.arg("wifi_ssid");
    String wifi_pass = server.arg("wifi_pass");
    String nsecHex = server.arg("nsecHex");
    String npubHex = server.arg("npubHex");
    String relays = server.arg("relays");
    
    preferences.begin("config", false);
    preferences.putString("userName", userName);
    // preferences.putString("device", device);
    preferences.putString("btn1", btn1);
    preferences.putString("btn1_sticky", btn1Sticky);
    preferences.putString("btn1_start", btn1_start_val);
    preferences.putString("btn1_end", btn1_end_val);
    preferences.putString("btn2", btn2);
    preferences.putString("btn2_sticky", btn2Sticky);
    preferences.putString("btn2_start", btn2_start_val);
    preferences.putString("btn2_end", btn2_end_val);
    preferences.putString("wifi_ssid", wifi_ssid);
    preferences.putString("wifi_pass", wifi_pass);
    preferences.putString("nsecHex", nsecHex);
    preferences.putString("npubHex", npubHex);
    preferences.putString("relays", relays);
    preferences.end();
    
    server.send(200, "text/html", "<h1>Configuration saved. Restarting...</h1>");
    delay(2000);
    ESP.restart();
  } else {
    server.send(405, "text/html", "Method Not Allowed");
  }
}

void handleReset() {
  // Clear all preferences and reboot.
  preferences.begin("config", false);
  preferences.clear();
  preferences.end();
  server.send(200, "text/html", "<h1>Configuration reset. Restarting...</h1>");
  delay(2000);
  ESP.restart();
}

// This function starts the configuration portal.
// It is called when no valid Wi‑Fi credentials exist (or you wish to reconfigure).
void startConfigPortal() {
  // Set dual mode so both AP and STA remain active.
  WiFi.mode(WIFI_AP_STA);
  
  // Start the AP with your chosen SSID and password.
  WiFi.softAP("GMGNcontroller", "gmgnprotocol");
  Serial.print("AP IP address: ");
  Serial.println(WiFi.softAPIP());
  
  // Set up web server routes.
  server.on("/", handleRoot);
  server.on("/save", handleSave);
  server.on("/reset", handleReset);
  server.begin();
  Serial.println("HTTP server started in AP mode");
}


// -----------------------------------------------------
// Utility Functions: WiFi, Config, Time
// -----------------------------------------------------
bool connectToWiFi() {
  preferences.begin("config", true);
  String storedSSID = preferences.getString("wifi_ssid", "");
  String storedPass = preferences.getString("wifi_pass", "");
  preferences.end();
  
  // We already set dual-mode (AP+STA) and started our AP in setup().
  // If there are no stored credentials, return false.
  if (storedSSID == "") {
    Serial.println("⚠️ No stored WiFi credentials found!");
    return false;
  }
  
  Serial.print("📡 Connecting to WiFi: ");
  Serial.println(storedSSID);
  
  // Begin connection as STA.
  WiFi.begin(storedSSID.c_str(), storedPass.c_str());
  
  Serial.print("Connecting to WiFi");
  int timeout = 0;
  while (WiFi.status() != WL_CONNECTED && timeout < 20) {
    delay(500);
    Serial.print(".");
    timeout++;
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("✅ WiFi connected!");
    digitalWrite(LED_GREEN, LOW);
    delay(700);
    digitalWrite(LED_GREEN, HIGH); 
    Serial.print("📍 IP Address (STA): ");
    Serial.println(WiFi.localIP());
    return true;
  }
  return false;
}


void loadConfig() {
  preferences.begin("config", true);
  configSSID = preferences.getString("wifi_ssid", "");
  configPass = preferences.getString("wifi_pass", "");

  configNsec = preferences.getString("nsecHex", "");
  configNpub = preferences.getString("npubHex", "");

  btn1Msg    = preferences.getString("btn1", "GM 🌻");
  btn2Msg    = preferences.getString("btn2", "GN 🌒");

  btn1Sticky = preferences.getString("btn1_sticky", "");
  btn2Sticky = preferences.getString("btn2_sticky", "");

  btn1_start = preferences.getString("btn1_start", "04:00");
  btn1_end   = preferences.getString("btn1_end", "11:00");
  btn2_start = preferences.getString("btn2_start", "19:00");
  btn2_end   = preferences.getString("btn2_end", "03:59");

  userName   = preferences.getString("userName", "DefaultName");
  // configDeviceName = preferences.getString("device", "GMGNcontroller");

  String relaysStr = preferences.getString("relays", "");
  preferences.end();
  
  configRelays.clear();
  if (relaysStr.length() > 0) {
    int start = 0;
    int commaIndex = relaysStr.indexOf(',');
    while (commaIndex != -1) {
      String token = relaysStr.substring(start, commaIndex);
      token.trim();
      if (token.length() > 0)
        configRelays.push_back(token);
      start = commaIndex + 1;
      commaIndex = relaysStr.indexOf(',', start);
    }
    String token = relaysStr.substring(start);
    token.trim();
    if (token.length() > 0)
      configRelays.push_back(token);
  }
}

unsigned long getUnixTimestamp() {
  time_t now;
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("⚠️ Failed to obtain time from NTP server!");
    return 0;
  }
  time(&now);
  Serial.print("⏰ Current Unix Timestamp: ");
  Serial.println(now);
  return now;
}

// -----------------------------------------------------
// LED & Morse Code Functions for greeting you back in morse code
// -----------------------------------------------------
String getMorse(char c) {
  switch(toupper(c)) {
    case 'A': return ".-";
    case 'B': return "-...";
    case 'C': return "-.-.";
    case 'D': return "-..";
    case 'E': return ".";
    case 'F': return "..-.";
    case 'G': return "--.";
    case 'H': return "....";
    case 'I': return "..";
    case 'J': return ".---";
    case 'K': return "-.-";
    case 'L': return ".-..";
    case 'M': return "--";
    case 'N': return "-.";
    case 'O': return "---";
    case 'P': return ".--.";
    case 'Q': return "--.-";
    case 'R': return ".-.";
    case 'S': return "...";
    case 'T': return "-";
    case 'U': return "..-";
    case 'V': return "...-";
    case 'W': return ".--";
    case 'X': return "-..-";
    case 'Y': return "-.--";
    case 'Z': return "--..";
    case '0': return "-----";
    case '1': return ".----";
    case '2': return "..---";
    case '3': return "...--";
    case '4': return "....-";
    case '5': return ".....";
    case '6': return "-....";
    case '7': return "--...";
    case '8': return "---..";
    case '9': return "----.";
    default: return "";
  }
}

void blinkMorse(String message) {
  // Before starting Morse feedback, ensure the yellow LED is off.
  digitalWrite(LED_YELLOW, HIGH);
  for (int i = 0; i < message.length(); i++) {
    char c = message.charAt(i);
    if (c == ' ') {
      delay(1400);
      continue;
    }
    String code = getMorse(c);
    for (int j = 0; j < code.length(); j++) {
      char symbol = code.charAt(j);
      digitalWrite(LED_GREEN, LOW);  // Turn green LED on (active LOW)
      if (symbol == '.')
        delay(200);
      else if (symbol == '-')
        delay(600);
      digitalWrite(LED_GREEN, HIGH); // Turn green LED off
      delay(200);
    }
    delay(600);
  }
}

void blinkAllLEDs(unsigned long duration) {
  unsigned long startTime = millis();
  while (millis() - startTime < duration) {
    digitalWrite(LED_GREEN, LOW);
    digitalWrite(LED_RED, LOW);
    digitalWrite(LED_YELLOW, LOW);
    delay(200);
    digitalWrite(LED_GREEN, HIGH);
    digitalWrite(LED_RED, HIGH);
    digitalWrite(LED_YELLOW, HIGH);
    delay(200);
  }
}

void countdownLEDBlinker(unsigned long duration) {
  unsigned long startTime = millis();
  while (millis() - startTime < duration) {
    // Blink red LED once
    digitalWrite(LED_RED, LOW);   // Turn red ON (active LOW)
    delay(250);
    digitalWrite(LED_RED, HIGH);  // Turn red OFF
    delay(250);

    // Blink yellow LED once
    digitalWrite(LED_YELLOW, LOW);   // Turn yellow ON
    delay(250);
    digitalWrite(LED_YELLOW, HIGH);  // Turn yellow OFF
    delay(250);

    // Blink green LED once
    digitalWrite(LED_GREEN, LOW);   // Turn green ON
    delay(250);
    digitalWrite(LED_GREEN, HIGH);  // Turn green OFF
    delay(250);
  }
}

void GreenYellowLEDBlinker(unsigned long duration) {
  unsigned long startTime = millis();
  while (millis() - startTime < duration) {

    // Blink yellow LED twice
    digitalWrite(LED_YELLOW, LOW);   // Turn yellow ON
    delay(150);
    digitalWrite(LED_YELLOW, HIGH);  // Turn yellow OFF
    delay(150);
    digitalWrite(LED_YELLOW, LOW);   // Turn yellow ON
    delay(150);
    digitalWrite(LED_YELLOW, HIGH);  // Turn yellow OFF
    delay(150);

    // Blink green LED once
    digitalWrite(LED_GREEN, LOW);   // Turn green ON
    delay(250);
    digitalWrite(LED_GREEN, HIGH);  // Turn green OFF
    delay(250);
  }
}

void blinkYellow(unsigned long duration) {
  unsigned long startTime = millis();
  while (millis() - startTime < duration) {
    if (((millis() / 300) % 2) == 0)
      digitalWrite(LED_YELLOW, LOW);
    else
      digitalWrite(LED_YELLOW, HIGH);
    delay(10);
  }
  digitalWrite(LED_YELLOW, HIGH);
}

void blinkRedTimes(int times) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_RED, LOW);
    delay(500);
    digitalWrite(LED_RED, HIGH);
    delay(500);
  }
}

// -----------------------------------------------------
// Normal Operation Setup
// -----------------------------------------------------
void normalOperationSetup() {
  loadConfig();
  WiFi.setHostname(deviceName.c_str());
  Serial.println("🔹 Setting up relays...");

  pinMode(BUTTON_GN, INPUT_PULLUP);
  pinMode(BUTTON_GM, INPUT_PULLUP);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_YELLOW, OUTPUT);
  digitalWrite(LED_GREEN, HIGH);
  digitalWrite(LED_RED, HIGH);
  digitalWrite(LED_YELLOW, HIGH);

  // Setup NTP using direct IP for time.nist.gov
  configTime(0, 3600, "129.6.15.28");
  delay(2000);
  struct tm timeinfo;
  Serial.print("Synchronizing time");
  unsigned long ntpStart = millis();
  bool timeSynced = false;
  while ((millis() - ntpStart) < 120000) {
    if (getLocalTime(&timeinfo)) {
      timeSynced = true;
      break;
    }
    Serial.print(".");
    delay(500);
  }
  if (!timeSynced) {
    Serial.println("\n❌ Failed to synchronize time within 2 minutes.");
  } else {
    Serial.println("\n✅ Time synchronized.");
    digitalWrite(LED_GREEN, LOW);
    delay(700);
    digitalWrite(LED_GREEN, HIGH); 
  }
  
  // Use default relays if none configured.
  if (configRelays.size() == 0) {
    configRelays.push_back("nos.lol");
    configRelays.push_back("relay.damus.io");
    configRelays.push_back("relay.nostr.band");
  }
  // Format relay URLs: remove trailing slashes and protocol prefixes.
  for (size_t i = 0; i < configRelays.size(); i++) {
    configRelays[i].trim();
    if (configRelays[i].endsWith("/"))
      configRelays[i] = configRelays[i].substring(0, configRelays[i].length() - 1);
    if (configRelays[i].startsWith("wss://"))
      configRelays[i] = configRelays[i].substring(6);
    else if (configRelays[i].startsWith("ws://"))
      configRelays[i] = configRelays[i].substring(5);
    Serial.print("📡 Using relay: ");
    digitalWrite(LED_GREEN, LOW);
    delay(700);
    digitalWrite(LED_GREEN, HIGH);
    Serial.println(configRelays[i]);
  }
  
  nostrRelayManager.setRelays(configRelays);
  nostrRelayManager.setMinRelaysAndTimeout(1, 10000);
  
  // Register callbacks.
  nostrRelayManager.setEventCallback("connected", relayConnectionHandler);
  nostrRelayManager.setEventCallback("disconnected", relayConnectionHandler);
  nostrRelayManager.setEventCallback("OK", okEventReceived);
  nostrRelayManager.setEventCallback(1, kind1EventCallback);
  
  nostr.setLogging(false);
  // Do not connect at boot; connection is on-demand.
}

// -----------------------------------------------------
// Helper: Extract Event ID from Note JSON
// -----------------------------------------------------
String extractEventID(String noteJson) {
  int idIndex = noteJson.indexOf("\"id\":\"");
  if (idIndex == -1) return "";
  int start = idIndex + 6;
  int end = noteJson.indexOf("\"", start);
  if (end == -1) return "";
  return noteJson.substring(start, end);
}

// -----------------------------------------------------
// Helper: for properly sending Emojis as Emojis not an HTML entity
// -----------------------------------------------------

// Converts a Unicode code point into a UTF-8 encoded String.
String encodeUTF8(uint32_t codepoint) {
  String result = "";
  if (codepoint < 0x80) {
    result += char(codepoint);
  } else if (codepoint < 0x800) {
    result += char(0xC0 | (codepoint >> 6));
    result += char(0x80 | (codepoint & 0x3F));
  } else if (codepoint < 0x10000) {
    result += char(0xE0 | (codepoint >> 12));
    result += char(0x80 | ((codepoint >> 6) & 0x3F));
    result += char(0x80 | (codepoint & 0x3F));
  } else if (codepoint < 0x110000) {
    result += char(0xF0 | (codepoint >> 18));
    result += char(0x80 | ((codepoint >> 12) & 0x3F));
    result += char(0x80 | ((codepoint >> 6) & 0x3F));
    result += char(0x80 | (codepoint & 0x3F));
  }
  return result;
}

// Scans the input string and replaces all numeric HTML entities (e.g., "&#128154;") 
// with their corresponding UTF-8 characters.
String decodeHtmlEntities(String input) {
  int start = 0;
  while (true) {
    int index = input.indexOf("&#", start);
    if (index == -1) break; // No more entities found
    int semicolonIndex = input.indexOf(";", index);
    if (semicolonIndex == -1) break; // Malformed entity, exit loop
    String entity = input.substring(index, semicolonIndex + 1);
    // Ensure the entity has a numeric value
    if (entity.length() > 3) {
      String numberStr = entity.substring(2, entity.length() - 1);
      long codepoint = numberStr.toInt();
      String utf8Char = encodeUTF8(codepoint);
      input.replace(entity, utf8Char);
    }
    start = index + 1; // Move past the replaced entity
  }
  return input;
}


// -----------------------------------------------------
// sendEvent() Function (Enqueue and Record Event ID)
// -----------------------------------------------------
void sendEvent(String message, String prefix) {
  Serial.println("🚀 Preparing to send event...");

  // Decode any numeric HTML entities (this will convert things like "&#128154;" into the actual emoji)
  message = decodeHtmlEntities(message);

  unsigned long timestamp = getUnixTimestamp();
  if (timestamp == 0) {
    Serial.println("❌ Error: No valid timestamp. Event cannot be signed.");
    return;
  }
  
  Serial.println("📝 Creating note event...");


  String noteString = nostr.getNote(configNsec.c_str(), configNpub.c_str(), timestamp, message);

  // // Inject the device tag into the note JSON.
  // String deviceTag = "\"tags\":[[\"device\",\"" + configDeviceName + "\"]]";
  // noteString.replace("\"tags\":[]", deviceTag);

  Serial.print("📄 Generated Note JSON: ");
  Serial.println(noteString);
  
  lastEventID = extractEventID(noteString);
  if (lastEventID == "") {
    Serial.println("❌ Failed to extract event ID from note JSON.");
    return;
  }
  Serial.print("Extracted Event ID: ");
  Serial.println(lastEventID);
  
  // --- Connect to Relay (max 10 sec) ---
  if (!isRelayConnected) {
    Serial.println("🔗 Not connected. Attempting to connect to relays...");
    nostrRelayManager.connect();
    unsigned long connStart = millis();
    while (!isRelayConnected && (millis() - connStart < 10000)) {
      nostrRelayManager.loop();
      delay(10);
    }
    if (!isRelayConnected) {
      Serial.println("❌ Failed to connect to any relay within 10 seconds.");
      blinkRedTimes(5);
      return;
    }
    Serial.println("✅ Relay connection established!");
  } else {
    Serial.println("✅ Relay already connected.");
  }
  
  // --- Enqueue the Event ---
  Serial.println("📡 Sending event to relay...");
  nostrRelayManager.enqueueMessage(noteString.c_str());
  Serial.println("📡 Event sent to relay queue!");
  
  eventSentTime = millis();
  pendingPrefix = prefix;
  eventConfirmed = false;
  eventInProgress = true;
  lastPostTime = millis();
}

// -----------------------------------------------------
// Check if the event has been published using a request
// -----------------------------------------------------
void checkEventPublished() {
  NostrRequestOptions *req = new NostrRequestOptions();
  String idArr[1] = { lastEventID };
  req->ids = idArr;
  req->ids_count = 1;
  req->limit = 1;
  nostrRelayManager.requestEvents(req);
  delete req;
}

// -----------------------------------------------------
// Process Event Feedback (Non-blocking)
// -----------------------------------------------------
void processEventFeedback() {
  if (eventInProgress) {
    unsigned long now = millis();
    // Every 3 sec, request confirmation for the event.
    if ((now - eventSentTime) % 3000 < 50) {
      Serial.println("🔎 Requesting event confirmation...");
      checkEventPublished();
    }
    
    if (eventConfirmed) {
      Serial.println("✅ Event confirmed!");
      // Turn off yellow LED before starting Morse feedback.
      digitalWrite(LED_YELLOW, HIGH);
      // Show green LED feedback in morse code.
      String morseMessage = pendingPrefix + " " + userName;
      Serial.print("📡 Sending Morse code feedback: ");
      Serial.println(morseMessage);
      blinkMorse(morseMessage);
      eventInProgress = false;
      nostrRelayManager.disconnect();
    }
    else if (now - eventSentTime > CONFIRM_TIMEOUT) {
      Serial.println("⚠️ No confirmation received within timeout; indicating failure.");
      blinkRedTimes(5);
      // Turn off yellow LED before starting Morse feedback.
      digitalWrite(LED_YELLOW, HIGH);
      eventInProgress = false;
      nostrRelayManager.disconnect();
    }
    else {
      // While waiting, softly blink the yellow LED.
      if (((now / 300) % 2) == 0)
        digitalWrite(LED_YELLOW, LOW);
      else
        digitalWrite(LED_YELLOW, HIGH);
    }
  }
}

// Helper function to split strings based on <<<>>> delimiter
std::vector<String> splitStickyStrings(String input) {
  std::vector<String> result;
  int start = 0;
  int end = input.indexOf("<<<>>>");

  while (end != -1) {
    String segment = input.substring(start, end);
    segment.trim();
    result.push_back(segment);
    start = end + 6;  // move past <<<>>> delimiter
    end = input.indexOf("<<<>>>", start);
  }

  String lastSegment = input.substring(start);
  lastSegment.trim();
  if (lastSegment.length() > 0) {
    result.push_back(lastSegment);
  }

  return result;
}

// Pick one random string
String getRandomSticky(String stickyStrings) {
  if (stickyStrings.length() == 0) return ""; // Return empty if nothing provided

  std::vector<String> segments = splitStickyStrings(stickyStrings);
  if (segments.size() == 0) return "";

  int randIndex = random(segments.size()); // pick random index
  return segments[randIndex];
}

// -----------------------------------------------------
// Normal Operation Loop (Button Handling)
// -----------------------------------------------------
void normalOperationLoop() {
  nostrRelayManager.loop();
  nostrRelayManager.broadcastEvents();
  
  if (millis() - lastPostTime < cooldownTime) {
      // If either button is pressed during the cooldown, provide feedback
      if (digitalRead(BUTTON_GN) == LOW || digitalRead(BUTTON_GM) == LOW) {
          Serial.println("Button pressed during cooldown period; please wait.");
          countdownLEDBlinker(10000);  // Blink red-yellow-green cycle for 10 seconds
      }
      return;
  }
  
  if (eventInProgress)
    return;
  
  // Get current button states (true if pressed)
  bool currentGM = (digitalRead(BUTTON_GM) == LOW);
  bool currentGN = (digitalRead(BUTTON_GN) == LOW);
  
  // Process individual button release events (rising edge detection)
  if (!currentGM && lastStateGM && pressTimeGM > 0) { // GM button released
    // Check if current time is within allowed window for GM button (btn1 message)
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      int nowMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
      int allowedStart = timeStringToMinutes(btn1_start);
      int allowedEnd = timeStringToMinutes(btn1_end);
      bool allowed;
      if (allowedStart <= allowedEnd)
        allowed = (nowMinutes >= allowedStart && nowMinutes < allowedEnd);
      else
        allowed = (nowMinutes >= allowedStart || nowMinutes < allowedEnd);
      if (!allowed) {
        Serial.println("GM button pressed outside allowed time window; event not sent.");
        GreenYellowLEDBlinker(10000);
      } else {
        Serial.println("GM button released within allowed window. Broadcasting event...");
        blinkYellow(1000);
        String sticky = getRandomSticky(btn1Sticky);
        String messageToSend = btn1Msg;
        if (sticky.length() > 0) {
          messageToSend += "\n" + sticky;
        }
        sendEvent(messageToSend, "GM");
        lastPostTime = millis();
      }
    }
    pressTimeGM = 0;
  }
  if (!currentGN && lastStateGN && pressTimeGN > 0) { // GN button released
    // Check if current time is within allowed window for GN button (btn2 message)
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      int nowMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
      int allowedStart = timeStringToMinutes(btn2_start);
      int allowedEnd = timeStringToMinutes(btn2_end);
      bool allowed;
      if (allowedStart <= allowedEnd)
        allowed = (nowMinutes >= allowedStart && nowMinutes < allowedEnd);
      else
        allowed = (nowMinutes >= allowedStart || nowMinutes < allowedEnd);
      if (!allowed) {
        Serial.println("GN button pressed outside allowed time window; event not sent.");
        GreenYellowLEDBlinker(10000);
      } else {
        Serial.println("GN button released within allowed window. Broadcasting event...");
        blinkYellow(1000);
        String sticky = getRandomSticky(btn2Sticky);
        String messageToSend = btn2Msg;
        if (sticky.length() > 0) {
          messageToSend += "\n" + sticky;
        }
        sendEvent(messageToSend, "GN");
        lastPostTime = millis();
      }
    }
    pressTimeGN = 0;
  }
  
  // On falling edge, record press times.
  if (currentGM && !lastStateGM && pressTimeGM == 0) {
    pressTimeGM = millis();
  }
  if (currentGN && !lastStateGN && pressTimeGN == 0) {
    pressTimeGN = millis();
  }

  // Update last state variables.
  lastStateGM = currentGM;
  lastStateGN = currentGN;
  
  processEventFeedback();
}

// -----------------------------------------------------
// Setup and Main Loop
// -----------------------------------------------------
void setup() {
  Serial.begin(115200);
  
  // Initialize LED pins as outputs
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_YELLOW, OUTPUT);
  digitalWrite(LED_GREEN, HIGH);
  digitalWrite(LED_RED, HIGH);
  digitalWrite(LED_YELLOW, HIGH);
  
  Serial.println("🚀 Booting GMGNcontroller...");
  blinkAllLEDs(5000);  // Blink all LEDs for 5 sec during boot
  
  // Record boot time
  bootTime = millis();

  // Set dual-mode and start the AP so that the configuration portal is always available at boot.
  startConfigPortal();

  // Now attempt to connect to Wi-Fi.
  if (connectToWiFi()) {
    Serial.println("✅ WiFi is connected! Proceeding with NTP synchronization...");
    configTime(0, 3600, "129.6.15.28");  // Use direct IP for time.nist.gov
    delay(2000);
    struct tm timeinfo;
    Serial.print("Synchronizing time");
    unsigned long ntpStart = millis();
    bool timeSynced = false;
    while ((millis() - ntpStart) < 120000) {
      if (getLocalTime(&timeinfo)) {
        timeSynced = true;
        break;
      }
      Serial.print(".");
      delay(500);
    }
    if (!timeSynced) {
      Serial.println("\n❌ Failed to synchronize time within 2 minutes.");
      digitalWrite(LED_RED, LOW);
      delay(700);
      digitalWrite(LED_RED, HIGH); 
    } else {
      Serial.println("\n✅ Time synchronized.");
    }
    // Indicate readiness
    digitalWrite(LED_GREEN, LOW);
    delay(700);
    digitalWrite(LED_GREEN, HIGH);

    // Initialize lastPostTime so the first event can be sent immediately without waiting for cooldownTime
    lastPostTime = millis() - cooldownTime;

    normalOperationSetup();
    }
  else {
    Serial.println("No valid WiFi credentials found. Running configuration portal...");
    // In this case, the AP is active and the config portal is available.
  }
}

void loop() {
  // Keep handling configuration portal if AP is active (either AP or AP+STA mode)
  if (WiFi.getMode() & WIFI_AP) {
    server.handleClient();
  } else {
    normalOperationLoop();
    nostrRelayManager.loop();
    nostrRelayManager.broadcastEvents();
    processEventFeedback();
  }
  
  // After 7 minutes, disable the AP if STA is connected.
  if (!apDisabled && (millis() - bootTime > 420000)) { // 7 minutes = 420000 ms
    Serial.println("🔌 7 minutes elapsed: disabling AP mode to reduce power consumption.");
    WiFi.softAPdisconnect(true);  // Turn off the AP interface.
    apDisabled = true;
  }
}
