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
#include "FS.h"
#include "LittleFS.h"

//--------how to use the device--------
// After successful compiling to your ESP32, plug in the device and connect to the WIFI GMGNcontroller with your smartphone
// or computer. Use password gmgnprotocol to connect to it. When you are connected to this wifi 
// go to 192.168.4.1 and configure the device. You have 7 min to do so. You can set the button messages for GM or GN, your wifi credentials,
// allowed time windows to disable buttons in certain times of the day, Nostr keys, and relays you like to post your messages to.
// Don't add more then 3 relays, because the nostrArduino library does not suport more than 3 relays at the same time.
// After saving the configuration, the device will restart and show the wifi access point again. Visit the conf page again to set up
// more options like sticky strings. Stickies are additional messages that will be appended to the base message you set for GM or GN button.
// By clicking on the txt file link you can edit the sticky strings in the browser. You can also upload a txt file with your sticky strings to the device.
// but keep in mind the structure which should be like this: sticky1<<<>>>sticky2<<<>>>sticky3<<<>>>sticky4
// There's a tool in the repository https://github.com/StellarStoic/GMGNcontroller to generate the sticky strings for you from the json file.
// Ok back to our conf page. WiFi AP is always on for 7 min after start of the device and it will
// turn off wifi after this time expires for security reasons. (on top right corner you'll see the countdown timer).
// After that you can start using your overcomplicated GM,GN controller.
// To access the settings you can always turn off/on your GMGN controller and visit the settings page.

// -----------------------------------------------------
// Global Variables and Definitions
// -----------------------------------------------------

String deviceName = "GMGNcontroller";
String configSSID, configPass, configNsec, configNpub, btn1Msg, btn1Sticky, btn2Msg, btn2Sticky, userName;
std::vector<String> configRelays;
// Stores byte offsets for each sticky string in the file
std::vector<uint32_t> btn1StickyOffsets;
std::vector<uint32_t> btn2StickyOffsets;

// time window variables (for UTC allowed periods)
// CODE IS USING UTC TIME NOT LOCAL !!!
String btn1_start = "04:00"; // Allowed start for GM button (btn1)
String btn1_end   = "11:00"; // Allowed end for GM button
String btn2_start = "19:00"; // Allowed start for GN button (btn2)
String btn2_end   = "03:59"; // Allowed end for GN button

#define FS_NAMESPACE LittleFS

#define BUTTON_GN 14    // Button for GN (posts btn2 message)
#define BUTTON_GM 27    // Button for GM (posts btn1 message)
#define LED_GREEN 33    // Green LED: success feedback (active LOW)
#define LED_RED   32    // Red LED: failure feedback (active LOW)
#define LED_YELLOW 25   // Yellow LED: processing feedback (active LOW)

const unsigned long apAccessTimer = 420000; // 7 minutes in milliseconds
unsigned long lastPostTime = 0;
const unsigned long cooldownTime = 900000; // 15min cooldown (a time period we need to wait for another button press to work)

// I tried to include a custom tag device in nostr event JSON but I was getting a bad event id because modifying the JSON (to add the device tag) invalidates the precomputed hash and signature.
// I need to either generate the event with tags in one step (by changing the library) or avoid modifying the JSON afterward. This might come in the future maybe.
// String configDeviceName;  // New: holds the configurable device name

// For button edge detection
bool lastStateGN = false;
bool lastStateGM = false;
unsigned long pressTimeGN = 0;
unsigned long pressTimeGM = 0;

// Disable internal relay loop after heat cleanup
bool allowRelayLoop = false;

// For event feedback (non‑blocking)
bool eventInProgress = false;                 // True while an event is being processed
unsigned long eventSentTime = 0;              // Time when the event was sent
const unsigned long CONFIRM_TIMEOUT = 22000;  // 22 sec timeout for confirmation
String pendingPrefix = "";                    // holds the Morse-code feedback

// For request confirmation (using event requests)
String lastEventID = "";  // Holds the event ID of the note we just sent

// This help us disconnect cleanly when nothing is happening anymore.
unsigned long lastRelayUse = 0;
const unsigned long RELAY_IDLE_TIMEOUT = 20000;  // 20 seconds

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
// Heap cleaner 
// -----------------------------------------------------
// Frees up memory by trimming large strings and yields control briefly.
// Also logs the available heap after cleanup for debugging and monitoring.
void resetHeapAndLog(const String& reason = "manual") {
  Serial.println("🧹 Cleaning up memory after " + reason + "...");

  // Trims unused whitespace from key strings to free RAM (especially sticky strings and messages)
  btn1Sticky.trim();
  btn2Sticky.trim();
  btn1Msg.trim();
  btn2Msg.trim();
  userName.trim();

  delay(50);  // Give time for cleanup to settle (non-critical delay)
  yield();    // Allow background tasks like button press) to run

  // Log current heap for memory monitoring
  Serial.println("📉 Heap after cleanup (" + reason + "): " + String(ESP.getFreeHeap()));
}

// Indexes the positions (byte offsets) of sticky messages in a given file.
// This avoids reading the entire file every time we want a random sticky.
// Each offset marks the start of a sticky message (delimited by <<<>>>).
void indexStickyFile(const char* path, std::vector<uint32_t>& offsets) {
  offsets.clear();  // Clear any existing offsets to start fresh

  File file = LittleFS.open(path, "r");  // Open the sticky file in read mode
  if (!file || file.isDirectory()) {
    Serial.println("⚠️ Failed to open sticky file for indexing: " + String(path));
    return;  // Exit if file is missing or invalid
}

  uint32_t position = 0;  // Tracks current byte position in the file
  offsets.push_back(position);  // First sticky always starts at 0

  // Look for <<<>>> delimiter
  String delimiter = "<<<>>>";  // Delimiter marking the end of each sticky
  const size_t delimLen = delimiter.length();
  String buffer = "";  // Temporary buffer to detect the delimiter

  // Read file byte-by-byte
  while (file.available()) {
    char c = file.read();
    buffer += c;
    position++;  // Update byte position

    // Keep only the last few chars to match delimiter efficiently
    if (buffer.length() > delimLen)
      buffer = buffer.substring(buffer.length() - delimLen);

    // If we found delimiter, record the start of the next sticky
    if (buffer == delimiter && file.available()) {
      offsets.push_back(position); // Next sticky starts after the delimiter
    }

    yield();  // Prevent ESP32 watchdog resets during long file reads
  }

  file.close();  // Always close the file after reading
  Serial.println("✅ Indexed " + String(offsets.size()) + " stickies from: " + String(path));
}

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
    // if (!lastStateConnected) {
    //   Serial.println("✅ Successfully connected to a relay!");
    // }
    isRelayConnected = true;
    lastStateConnected = true;
  } else if (key == "disconnected") {
    // if (lastStateConnected) {
    //   Serial.println("❌ Disconnected from relay.");
    // }
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

// check if the files exists and has contents.
void checkStickyFile() {
  if (LittleFS.exists("/btn1_sticky.txt")) {
      File f = LittleFS.open("/btn1_sticky.txt", FILE_READ);
      Serial.println("btn1_sticky.txt exists. Size = " + String(f.size()) + " bytes");
      f.close();
  } else {
      Serial.println("btn1_sticky.txt does not exist.");
  }
}

void okEventReceived(const std::string &key, const char *payload) {
  Serial.println("OK event received (not used for confirmation).");
  Serial.println(payload);
}

// Reads all non-empty lines from a file into a vector
std::vector<String> readStickyFile(const char* path) {
  std::vector<String> lines;
  File f = LittleFS.open(path, FILE_READ);
  if (f) {
      while (f.available()) {
          String line = f.readStringUntil('\n');
          line.trim();
          if (line.length() > 0)
              lines.push_back(line);
      }
      f.close();
  }
  return lines;
}

// Saves all lines from a vector to a file, one line per line
void saveStickyFile(const char* path, const std::vector<String>& lines) {
  File f = LittleFS.open(path, FILE_WRITE);
  if (f) {
      for (const auto& line : lines) {
          f.println(line);
      }
      f.close();
  }
}

// -------------------------
// STORAGE CHECK UTILITIES FOR THE CONFIG PAGE
// -------------------------

// --- CONFIG ---
#define STICKY_SOFT_LIMIT 0.6  // 60% usage (warn)
#define STICKY_HARD_LIMIT 0.8  // 80% usage (block)

// --- Count delimiters for strings counter for each Btn ---
size_t countDelimiters(const String &s) {
  size_t count = 0;
  int index = 0;
  while ((index = s.indexOf("<<<>>>", index)) != -1) {
      count++;
      index += 6;
  }
  return count;
} 

// --- Return memory usage info + sticky counts as HTML block ---
String getStorageInfoHTML() {
  size_t total = LittleFS.totalBytes();
  size_t used = LittleFS.usedBytes();
  float usageRatio = (float)used / (float)total;

  String color = "green";
  if (usageRatio > STICKY_HARD_LIMIT) {
      color = "red";
  } else if (usageRatio > STICKY_SOFT_LIMIT) {
      color = "orange";
  }

  // -------------------------------
  //  Count GM strings by delimiter
  // -------------------------------
  String btn1_content = "";
  File f1 = LittleFS.open("/btn1_sticky.txt", FILE_READ);
  if (f1) {
      while (f1.available()) {
          btn1_content += (char)f1.read();
      }
      f1.close();
  }
  int btn1_count = countDelimiters(btn1_content);
  if (btn1_count > 0) btn1_count++; // because N strings = N-1 delimiters + 1

  // -------------------------------
  //  Count GN strings by delimiter
  // -------------------------------
  String btn2_content = "";
  File f2 = LittleFS.open("/btn2_sticky.txt", FILE_READ);
  if (f2) {
      while (f2.available()) {
          btn2_content += (char)f2.read();
      }
      f2.close();
  }
  int btn2_count = countDelimiters(btn2_content);
  if (btn2_count > 0) btn2_count++; // because N strings = N-1 delimiters + 1

  // -------------------------------
  //  Build HTML
  // -------------------------------
  String html = "<div style='text-align:center; margin:10px;'><h3>Storage Info</h3>";
  html += "<p style='color:" + color + "; font-size:1.2em;'>";
  html += "Used: " + String(used / 1024) + " KB / " + String(total / 1024) + " KB (" + String(int(usageRatio * 100)) + "%)";
  html += "</p>";
  html += "<p>GM Stickies: " + String(btn1_count) + " | GN Stickies: " + String(btn2_count) + "</p>";
  html += "</div>";

  return html;
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
        --c2:#468758;
        
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
        background-color:#518c69;
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
    <h1>First time GMGNcontroller Configuration</h1>
    <h4>After this setup visit this configuration page again for more options</h4>
    <form action="/save" method="POST">
      <label>Your Name:</label>
      <input type="text" name="userName" placeholder="Enter your name"><br>

      <!-- <label>Device Name:</label>
      <input type="text" name="device" placeholder="Give device a name or use default GMGNcontroller"><br> -->
      
      <label>Button 1 Message:</label>
      <input type="text" name="btn1" placeholder="Enter message for button 1"><br>
      <!-- <label>Random Sticky Strings for Button 1:</label>
      <textarea id="stickyStrings" name="btn1_sticky" placeholder="Enter sticky strings separated by <<<>>>"></textarea><br> -->
      <label>Button 1 Allowed Start Time (UTC, HH:MM):</label>
      <input type="time" name="btn1_start" placeholder="e.g., 04:00"><br>
      <label>Button 1 Allowed End Time (UTC, HH:MM):</label>
      <input type="time" name="btn1_end" placeholder="e.g., 11:00"><br>
      
      <label>Button 2 Message:</label>
      <input type="text" name="btn2" placeholder="Enter message for button 2"><br>
      <!-- <label>Sticky Strings for Button 2:</label>
      <textarea id="stickyStrings" name="btn2_sticky" placeholder="Enter sticky strings separated by <<<>>>"></textarea><br> -->
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
  bool isEmpty = !preferences.isKey("wifi_ssid") || preferences.getString("wifi_ssid") == "";
  preferences.end();

  unsigned long timeLeft = 0;
  if (!apDisabled) {
    timeLeft = (apAccessTimer > millis() - bootTime) ? (apAccessTimer - (millis() - bootTime)) : 0;   // timer showing when the AP will switch off
  }
  int secondsLeft = timeLeft / 1000;

  if (isEmpty) {
      // First time setup page
      server.send(200, "text/html", htmlForm);
      return;
  }

  // Proceed directly to load preferences and sticky files
  preferences.begin("config", true);
  String savedUserName = preferences.getString("userName", "");
  // String configDeviceName = preferences.getString("device", "GMGNcontroller");
  String savedBtn1 = preferences.getString("btn1", "");
  String savedBtn1Sticky = "";
  bool first1 = true;
  File f1 = LittleFS.open("/btn1_sticky.txt", FILE_READ);
  if (f1) {
      while (f1.available()) {
          String line = f1.readStringUntil('\n');
          line.trim();
          if (line.length() > 0) {
              if (!first1) savedBtn1Sticky += "<<<>>>";
              savedBtn1Sticky += line;
              first1 = false;
          }
      }
      f1.close();
  }
  
  String savedBtn1Start = preferences.getString("btn1_start", "04:00");
  String savedBtn1End   = preferences.getString("btn1_end", "11:00");
  String savedBtn2 = preferences.getString("btn2", "");
  String savedBtn2Sticky = "";
  bool first2 = true;
  File f2 = LittleFS.open("/btn2_sticky.txt", FILE_READ);
  if (f2) {
      while (f2.available()) {
          String line = f2.readStringUntil('\n');
          line.trim();
          if (line.length() > 0) {
              if (!first2) savedBtn2Sticky += "<<<>>>";
              savedBtn2Sticky += line;
              first2 = false;
          }
      }
      f2.close();
  }

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

  String storageInfo = getStorageInfoHTML();

  // Create a dynamic HTML page with saved values inserted in the value attributes
  String page = "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>GMGNcontroller Configuration</title>"
                "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                "<style>"
                "html {"
                "--s: 60px;"
                "--c1:#6a51cd;"
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
                "#apTimerBox { position:fixed; top:10px; right:10px; background:#f0f0f0; padding:6px 12px; border-radius:5px; font-family:monospace; font-size:14px; }"
                "#stickyStrings {"
                  "width: 100%; "
                  "height: 120px; "
                  "box-sizing: border-box;"
                  "padding: 8px;"
                  "resize: vertical;"
                "}"
                "progress{"
                  "width:100%;"
                  "height:20px;"
                  "background-color: #f3f3f3;"
                  "border-radius: 10px;"
                "}"

                "progress::-webkit-progress-bar {"
                  "background-color: #f3f3f3;"
                  "border-radius: 10px;"
                "}"

                "progress::-webkit-progress-value {"
                  "background-color:#4c32b5;"
                  "border-radius: 10px;"
                "}"

               "progress::-moz-progress-bar {"
                  "background-color:#4c32b5;"
                  "border-radius: 10px;"
                "}"
                ".upload-banner {"
                  "position: fixed;"
                  "top: 20px; /* Adjust this value for vertical position */"
                  "left: 50%;"
                  "transform: translateX(-50%); /* Center horizontally */"
                  "text-align: center;"
                  "padding: 15px;"
                  "background-color: #518c69;"
                  "color: white;"
                  "font-size: 1.2em;"
                  "border-radius: 6px;"
                  "transition: opacity 0.5s ease;"
                  "z-index: 1000; /* Ensure it's above other content */"
                  "width: auto;"
                  "white-space: nowrap;"
                "}"
                "input { margin: 5px 0; padding: 5px; width: 100%; }"
                "input[type='submit'], input[type='button'] { width: auto; }"
                ".btn-container {"
                  "display: flex;"
                  "justify-content: space-between;"
                  "margin-top: 20px;"
                "}"
                ".btn-container input { width: auto; margin: 0 10px; }"
                "</style>"

                "</head><body>"
                "<!-- file Upload Success Banners -->"
                "<div id='uploadSuccess2' class='upload-banner' style='display:none;'>✅ btn2_sticky.txt uploaded!</div>"
                "<div id='uploadSuccess1' class='upload-banner' style='display:none;'>✅ btn1_sticky.txt uploaded!</div>"

                "<h1 style='text-align: center'>Configure GMGNcontroller</h1>"
                "<div id='apTimerBox'>🛜 <span id='apCountdown'></span> left</div>"
                + storageInfo +
                "<hr>"
                "<form action='/save' method='POST'>"
                "<label>Your Name:</label>"
                "<input type='text' name='userName' placeholder='Enter your name' value='" + savedUserName + "'><br><br>"

                // "<label>Device Name:</label>"
                // "<input type='text' name='device' placeholder='Give device a name or use default GMGNcontroller' value='" + configDeviceName + "'><br>"

                "<label>Button 1 Base Message:</label>"
                "<input type='text' name='btn1' placeholder='Enter message for button 1' value='" + savedBtn1 + "'><br><br>"
                "<label>Random Sticky Strings for Button 1:</label><br>"
                "<p style='text-align: center; font-size: 1.3em; font-style: bold; color:purple; margin: 0;'>"
                  "<a href='/edit?file=btn1_sticky.txt' target='_blank'>📝 Edit btn1_sticky.txt</a>"
                "</p><br><br>"
                "<label>Upload ready-made sticky strings .txt file for btn1:</label><br>"
                "<input type='file' id='file1' onchange='showFileInfo(\"file1\",\"fileLabel1\")'><br>"
                "<span id='fileLabel1'>No file selected</span><br>"
                "<progress id='progress1' value='0' max='100'></progress><br>"
                "<button type='button' onclick='uploadFile1()'>Upload Button 1 Sticky</button><br><br>"
                "<label>GM Button Allowed Start Time (UTC, HH:MM):</label>"
                "<input type='time' name='btn1_start' placeholder='HH:MM' value='" + savedBtn1Start + "'><br><br>"
                "<label>GM Button Allowed End Time (UTC, HH:MM):</label>"
                "<input type='time' name='btn1_end' placeholder='HH:MM' value='" + savedBtn1End + "'><br><br>"
                "<hr>"
                "<label>Button 2 Base Message:</label>"
                "<input type='text' name='btn2' placeholder='Enter message for button 2' value='" + savedBtn2 + "'><br><br>"
                "<label>Random Sticky Strings for Button 2:</label><br>"
                "<p style='text-align: center; font-size: 1.3em; font-style: bold; color:purple; margin: 0;'>"
                  "<a href='/edit?file=btn2_sticky.txt' target='_blank'>📝 Edit btn2_sticky.txt</a>"
                "</p><br><br>"
                "<label>Upload ready-made sticky strings .txt file for btn2:</label><br>"
                "<input type='file' id='file2' onchange='showFileInfo(\"file2\",\"fileLabel2\")'><br>"
                "<span id='fileLabel2'>No file selected</span><br>"
                "<progress id='progress2' value='0' max='100'></progress><br>"
                "<button type='button' onclick='uploadFile2()'>Upload Button 2 Sticky</button><br><br>"
                "<label>GN Button Allowed Start Time (UTC, HH:MM):</label>"
                "<input type='time' name='btn2_start' placeholder='HH:MM' value='" + savedBtn2Start + "'><br><br>"
                "<label>GN Button Allowed End Time (UTC, HH:MM):</label>"
                "<input type='time' name='btn2_end' placeholder='HH:MM' value='" + savedBtn2End + "'><br><br>"
                "<hr>"
                "<label>WiFi SSID:</label>"
                "<input type='text' name='wifi_ssid' placeholder='Enter your WiFi SSID' value='" + savedSSID + "'><br><br>"
                "<label>WiFi Password:</label>"
                "<input type='password' name='wifi_pass' placeholder='Enter your WiFi Password' value='" + savedPass + "'><br><br>"
                "<label>Nostr nsecHex:</label>"
                "<input type='password' name='nsecHex' placeholder='Enter your nsecHex' value='" + savedNsec + "'><br><br>"
                "<label>Nostr npubHex:</label>"
                "<input type='password' name='npubHex' placeholder='Enter your npubHex' value='" + savedNpub + "'><br><br>"
                "<label>Relays (comma separated):</label>"
                "<input type='text' name='relays' placeholder='e.g., wss://relay.damus.io/,wss://relay.nostr.band...' value='" + savedRelays + "'><br><br>"
              "<div class='btn-container'>"
              "  <input type='button' value='Nuke settings' onclick=\"if(confirm('Are you sure you want to reset all the settings? This will erase all your saved configuration.')){window.location.href='/reset';}\" style='background-color: red; color: white; padding: 10px 20px; border: none; border-radius: 5px; margin-right: 10px;'>"
              "  <!-- <input type='button' value='Delete ALL Files Only' onclick=\"if(confirm('This will delete ALL files stored in memory, but will not reset your settings. Proceed?')){window.location.href='/deletefiles';}\" style='background-color: orange; color: white; padding: 10px 20px; border: none; border-radius: 5px; margin-right: 10px;'> -->"
              "  <input type='submit' value='Save' style='background-color: green; color: white; padding: 10px 20px; border: none; border-radius: 5px; margin-left: 10px;'>"
              "</div>"
              "</form>"


              "<!-- file Upload Success Banners -->"
              "<div id='uploadSuccess2' class='upload-banner' style='display:none;'>✅ btn2_sticky.txt uploaded!</div>"

              "<script>"

              "function formatSize(bytes){"
              "  if(bytes < 1024) return bytes + ' bytes';"
              "  return (bytes/1024).toFixed(1) + ' KB';"
              "}"

              "let secondsLeft = " + String(secondsLeft) + ";"
              "function updateCountdown() {"
              "  const el = document.getElementById('apCountdown');"
              "  if (secondsLeft > 0) {"
              "    el.textContent = secondsLeft + 's';"
              "    secondsLeft--;"
              "    setTimeout(updateCountdown, 1000);"
              "  } else {"
              "    el.textContent = 'Expired';"
              "  }"
              "}"
              "window.onload = updateCountdown;"

              "function showFileInfo(inputId, labelId) {"
              "  var file = document.getElementById(inputId).files[0];"
              "  if (file) {"
              "    document.getElementById(labelId).innerText = file.name + ' (' + formatSize(file.size) + ')';"
              "  } else {"
              "    document.getElementById(labelId).innerText = 'No file selected';"
              "  }"
              "}"

              "function showUploadSuccess(id){"
              "  var banner=document.getElementById(id);"
              "  banner.style.opacity='0';"
              "  banner.style.display='block';"
              "  setTimeout(function(){banner.style.opacity='1';},10);"
              "  setTimeout(function(){"
              "    banner.style.opacity='0';"
              "    setTimeout(function(){banner.style.display='none';},500);"
              "  },4000);"
              "}"

              "function uploadFile1(){"
              "  var file=document.getElementById('file1').files[0];"
              "  if(!file){alert('No file selected!');return;}"
              "  var xhr=new XMLHttpRequest();"
              "  xhr.open('POST','/upload1',true);"
              "  xhr.upload.onprogress=function(e){"
              "    if(e.lengthComputable){"
              "      document.getElementById('progress1').value=e.loaded/e.total*100;"
              "    }"
              "  };"
              "  xhr.onload=function(){"
              "    if(xhr.status==200){"
              "      showUploadSuccess('uploadSuccess1');"
              "      setTimeout(() => location.reload(), 5000);"
              "    }"
              "    else {"
              "     alert('❌ Upload failed. Try again.');"
              "   }"
              "  };"
              "  var fd=new FormData();"
              "  fd.append('upload',file);"
              "  xhr.send(fd);"
              "}"

              "function uploadFile2(){"
              "  var file=document.getElementById('file2').files[0];"
              "  if(!file){alert('No file selected!');return;}"
              "  var xhr=new XMLHttpRequest();"
              "  xhr.open('POST','/upload2',true);"
              "  xhr.upload.onprogress=function(e){"
              "    if(e.lengthComputable){"
              "      document.getElementById('progress2').value=e.loaded/e.total*100;"
              "    }"
              "  };"
              "  xhr.onload=function(){"
              "    if(xhr.status==200){"
              "      showUploadSuccess('uploadSuccess2');"
              "      setTimeout(() => location.reload(), 5000);"
              "    }"
              "    else {"
              "     alert('❌ Upload failed. Try again.');"
              "   }"
              "  };"
              "  var fd=new FormData();"
              "  fd.append('upload',file);"
              "  xhr.send(fd);"
              "}"

              "</script>"
              "</body></html>";
  delay(500); // 500ms
  server.send(200, "text/html", page);
}

// Shared upload file handle
File uploadFile;

// ⭕ SIZE CHECK FUNCTION
bool checkStickyFileSize(size_t incomingSize) {
    size_t projectedUsage = LittleFS.usedBytes() + incomingSize;
    if (projectedUsage > LittleFS.totalBytes() * STICKY_HARD_LIMIT) {
        return false; // reject upload
    }
    return true;
}

// ----------------------
// ✅ Upload1 with check
// ----------------------
void handleUpload1() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        Serial.println("Uploading file to /btn1_sticky.txt...");
        if (!checkStickyFileSize(upload.totalSize)) {
            Serial.println("❌ Upload rejected (would exceed storage limit)");
            server.send(400, "text/html", "<h1 style='color:red;'>Upload rejected: File too large for safe storage!</h1>");
            return;
        }
        uploadFile = LittleFS.open("/btn1_sticky.txt", FILE_WRITE);
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (uploadFile) uploadFile.write(upload.buf, upload.currentSize);
        Serial.println("Writing chunk");
    } else if (upload.status == UPLOAD_FILE_END) {
      if (uploadFile) {
          uploadFile.flush();
          uploadFile.close();
          Serial.println("✅ btn1_sticky.txt uploaded successfully");
  
          File f = LittleFS.open("/btn1_sticky.txt", "r");
          if (f) {
              Serial.println("📄 Final size: " + String(f.size()) + " bytes");
              while (f.available()) {
                Serial.write(f.read());  // dump raw contents
              }
              f.close();
            }
  
          server.send(200, "text/plain", "Upload1 success");
      } else {
          server.send(500, "text/plain", "Upload1 failed");
      }
  }
}

// ----------------------
// ✅ Upload2 with check
// ----------------------
void handleUpload2() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        Serial.println("Uploading file to /btn2_sticky.txt...");
        if (!checkStickyFileSize(upload.totalSize)) {
            Serial.println("❌ Upload rejected (would exceed storage limit)");
            server.send(400, "text/html", "<h1 style='color:red;'>Upload rejected: File too large for safe storage!</h1>");
            return;
        }
        uploadFile = LittleFS.open("/btn2_sticky.txt", FILE_WRITE);
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (uploadFile) uploadFile.write(upload.buf, upload.currentSize);
        Serial.println("Writing chunk");
    } else if (upload.status == UPLOAD_FILE_END) {
      if (uploadFile) {
          uploadFile.flush();
          uploadFile.close();
          Serial.println("✅ btn2_sticky.txt uploaded successfully");
  
          File f = LittleFS.open("/btn2_sticky.txt", "r");
          if (f) {
              Serial.println("📄 Final size: " + String(f.size()) + " bytes");
              while (f.available()) {
                Serial.write(f.read());  // dump raw contents
              }
              f.close();
            }
  
          server.send(200, "text/plain", "Upload2 success");
      } else {
          server.send(500, "text/plain", "Upload2 failed");
      }
  }
}

void handleSave() {
  if (server.method() == HTTP_POST) {
    // Load form inputs
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
    
    // Save simple fields into Preferences
    preferences.begin("config", false);
    preferences.putString("userName", userName);
    // preferences.putString("device", device);
    preferences.putString("btn1", btn1);
    preferences.putString("btn1_start", btn1_start_val);
    preferences.putString("btn1_end", btn1_end_val);
    preferences.putString("btn2", btn2);
    preferences.putString("btn2_start", btn2_start_val);
    preferences.putString("btn2_end", btn2_end_val);
    preferences.putString("wifi_ssid", wifi_ssid);
    preferences.putString("wifi_pass", wifi_pass);
    preferences.putString("nsecHex", nsecHex);
    preferences.putString("npubHex", npubHex);
    preferences.putString("relays", relays);
    preferences.end();

        // Optional: show storage usage
        Serial.println("💾 Used: " + String(LittleFS.usedBytes() / 1024.0, 2) + " KB / " + String(LittleFS.totalBytes() / 1024.0, 2) + " KB");

        // Optional: ensure flush to flash is done before restart
        LittleFS.end();
        delay(2000);

        // Show confirmation page
        server.send(200, "text/html", "<h1>Settings saved! Restarting...</h1>");
        delay(3000);
        ESP.restart();
    } else {
        server.send(405, "text/html", "Method Not Allowed");
    }
}

// ------------------ handleEdit() ------------------

void handleEdit() {
  if (!server.hasArg("file")) {
      server.send(400, "text/html", "<h1>Missing file argument!</h1>");
      return;
  }
  String fileName = "/" + server.arg("file");
  if (!LittleFS.exists(fileName)) {
      server.send(404, "text/html", "<h1>File not found.</h1>");
      return;
  }

  String page = R"rawliteral(
    <!DOCTYPE html>
    <html>
    <head>
        <meta charset='UTF-8'>
        <meta name='viewport' content='width=device-width, initial-scale=1'>
        <title>Text File Editor</title>
        <style>
            body { 
                font-family: Arial; 
                margin:0; 
                padding:0; 
                display:flex; 
                flex-direction:column; 
                height:100vh; 
            }
            .header {
                background:#4CAF50; 
                color:white; 
                padding:10px; 
                margin:0; 
                text-align:center; 
            }
            .separator-info {
                background:#e8f5e9;
                color:#2e7d32;
                padding:8px;
                text-align:center;
                margin:0;
                font-size:14px;
            }
            textarea { 
                flex:1; 
                width:100%; 
                box-sizing:border-box; 
                padding:10px; 
                font-family:monospace; 
                resize:none; 
                border:none; 
                outline:none; 
                font-size:14px; 
                border-top:1px solid #ddd;
                border-bottom:1px solid #ddd;
            }
            .controls { 
                display:flex; 
                justify-content:space-between; 
                background:#f1f1f1; 
                padding:10px; 
            }
            .controls a, .controls input { 
                padding:10px 20px; 
                border:none; 
                border-radius:5px; 
                background:#4CAF50; 
                color:white; 
                cursor:pointer; 
                text-decoration:none; 
            }
            .controls a:hover, .controls input:hover { 
                background:#45a049; 
            }
            form { 
                display:flex; 
                flex-direction:column; 
                height:100%; 
                margin:0;
            }
        </style>
    </head>
    <body>
        <div class="header">Edit )rawliteral" + fileName + R"rawliteral(</div>
        <p class="separator-info">Use <<<>>> as a separator between sentences</p>
        <form method='POST' action='/savefile'>
            <input type='hidden' name='file' value=')rawliteral" + server.arg("file") + R"rawliteral('>
            <textarea id="editor" name='content'>Loading...</textarea>
            <div class='controls'>
                <a href='/'>⬅ Back</a>
                <input type='submit' value='💾 Save'>
            </div>
        </form>
        <script>
            fetch('/loadfile?file=)rawliteral" + server.arg("file") + R"rawliteral(')
            .then(r => r.text())
            .then(t => { document.getElementById('editor').value = t; })
            .catch(e => { document.getElementById('editor').value = "⚠ Failed to load file!"; });
        </script>
    </body>
    </html>
    )rawliteral";
  delay(500); // 500ms
  server.send(200, "text/html", page);
}

// ------------------ handleLoadFile() ------------------

void handleLoadFile() {
  if (!server.hasArg("file")) {
      server.send(400, "text/plain", "Missing file argument");
      return;
  }
  String fileName = "/" + server.arg("file");
  File f = LittleFS.open(fileName, FILE_READ);
  if (!f) {
      server.send(404, "text/plain", "File not found");
      return;
  }

  // This is enough!
  server.streamFile(f, "text/plain");
  f.close();
}

// ===============================
// Handle File Saving (from editor form)
// ===============================
void handleSaveFile() {
  // Check if both required parameters are present
  if (!server.hasArg("file") || !server.hasArg("content")) {
      server.send(400, "text/html", "<h1>Missing arguments!</h1>");
      return;
  }

  // Get file name from URL argument
  String fileName = "/" + server.arg("file");

  // Open the file for writing (this will overwrite it)
  File f = LittleFS.open(fileName, FILE_WRITE);
  if (!f) {
      // If opening failed, show error
      server.send(500, "text/html", "<h1>Failed to open file for writing.</h1>");
      return;
  }

  // Save the content sent from the textarea
  f.print(server.arg("content")); // Save as-is
  f.close();

  // Redirect back to editor after save
  server.sendHeader("Location", "/edit?file=" + server.arg("file"), true);
  // NO restart needed
  server.send(200, "text/html", "<h1>File saved successfully! <a href='/edit?file=" + server.arg("file") + "'>Back to editor</a></h1>");
}

// ------------------------------------
// 💣 Full factory reset = clears preferences + wipes both sticky files
// ------------------------------------
void handleReset() {
  // --- Reset all preferences ---
  preferences.begin("config", false);
  preferences.clear();
  preferences.end();
  Serial.println("✅ Preferences cleared");

  // --- Remount LittleFS ---
  LittleFS.end();
  delay(100);
  if (!LittleFS.begin(true)) {
      Serial.println("❌ Failed to remount LittleFS");
      server.send(500, "text/html", "<h1 style='color:red;'>Failed to remount LittleFS.</h1>");
      return;
  }
  Serial.println("✅ LittleFS re-mounted");

  // --- Wipe btn1_sticky.txt ---
  File f1 = LittleFS.open("/btn1_sticky.txt", FILE_WRITE);
  if (f1) {
      f1.close();
      Serial.println("✅ Wiped: btn1_sticky.txt");
  } else {
      Serial.println("⚠️ Could not open btn1_sticky.txt for wiping (may not exist)");
  }

  // --- Wipe btn2_sticky.txt ---
  File f2 = LittleFS.open("/btn2_sticky.txt", FILE_WRITE);
  if (f2) {
      f2.close();
      Serial.println("✅ Wiped: btn2_sticky.txt");
  } else {
      Serial.println("⚠️ Could not open btn2_sticky.txt for wiping (may not exist)");
  }

  // --- Confirm ---
  server.send(200, "text/html", "<h1 style='color:red;'>Preferences reset & files wiped. Rebooting...</h1>");
  delay(3000);
  ESP.restart();
}

// 🔍 Debugging helper - lists ALL files in LittleFS
void listAllFiles() {
  Serial.println("📂 Listing files in LittleFS:");
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  while (file) {
    Serial.println(" - " + String(file.name()) + " | " + String(file.size()) + " bytes");
    file = root.openNextFile();
  }
}

// This function starts the configuration portal.
// It is called when no valid Wi‑Fi credentials exist (or if you wish to reconfigure).
void startConfigPortal() {
  // Set dual mode so both AP and STA remain active.
  WiFi.mode(WIFI_AP_STA);
  
  // Start the AP with your chosen SSID and password.
  WiFi.softAP("GMGNcontroller", "gmgnprotocol");
  Serial.print("AP IP address: ");
  Serial.println(WiFi.softAPIP());
  
  // Set up web server routes.
  server.on("/", handleRoot);
  server.on("/upload1", HTTP_POST, [](){ server.send(200, "text/html", "<h1>✅ btn1_sticky.txt uploaded! Restarting...</h1>"); delay(2000); }, handleUpload1);
  server.on("/upload2", HTTP_POST, [](){ server.send(200, "text/html", "<h1>✅ btn2_sticky.txt uploaded! Restarting...</h1>"); delay(2000); }, handleUpload2);
  // server.on("/upload1", HTTP_POST, handleUpload1);
  // server.on("/upload2", HTTP_POST, handleUpload2);
  server.on("/save", handleSave);
  server.on("/reset", handleReset);
  server.on("/edit", handleEdit);
  server.on("/savefile", HTTP_POST, handleSaveFile);
  server.on("/loadfile", handleLoadFile);

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

  Serial.println("btn1_sticky.txt exists: " + String(LittleFS.exists("/btn1_sticky.txt")));
  Serial.println("btn2_sticky.txt exists: " + String(LittleFS.exists("/btn2_sticky.txt")));

  btn1_start = preferences.getString("btn1_start", "04:00");
  btn1_end   = preferences.getString("btn1_end", "11:00");
  btn2_start = preferences.getString("btn2_start", "19:00");
  btn2_end   = preferences.getString("btn2_end", "03:59");

  userName   = preferences.getString("userName", "DefaultName");
  // configDeviceName = preferences.getString("device", "GMGNcontroller");

  String relaysStr = preferences.getString("relays", "");
  preferences.end();

   // ---------- Lazy-Load Sticky Strings ----------

  // BTN1 Sticky
  btn1Sticky = "";

  File f1 = LittleFS.open("/btn1_sticky.txt", FILE_READ);
  if (f1) {
    while (f1.available()) {
      btn1Sticky += (char)f1.read();
    }
    f1.close();
    Serial.println("✅ Loaded btn1_sticky.txt (" + String(btn1Sticky.length()) + " chars)");
  } else {
    Serial.println("⚠️ Could not load btn1_sticky.txt (file not found)");
  }

  // BTN2 Sticky
  btn2Sticky = "";

  File f2 = LittleFS.open("/btn2_sticky.txt", FILE_READ);
  if (f2) {
    while (f2.available()) {
      btn2Sticky += (char)f2.read();
    }
    f2.close();
    Serial.println("✅ Loaded btn2_sticky.txt (" + String(btn2Sticky.length()) + " chars)");
  } else {
    Serial.println("⚠️ Could not load btn2_sticky.txt (file not found)");
  }

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
  indexStickyFile("/btn1_sticky.txt", btn1StickyOffsets);
  indexStickyFile("/btn2_sticky.txt", btn2StickyOffsets);
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
    resetHeapAndLog("timestamp failure");
    return;
  }
  
  Serial.println("📝 Creating note event...");


  // Serial.println("configNsec: " + configNsec);
  // Serial.println("configNpub: " + configNpub);
  Serial.println("message: " + message);
  Serial.println("✅ DEBUG: Final message inside sendEvent() is:");
  Serial.println(message);
  String noteString = nostr.getNote(configNsec.c_str(), configNpub.c_str(), timestamp, message);

  // // Inject the device tag into the note JSON.
  // String deviceTag = "\"tags\":[[\"device\",\"" + configDeviceName + "\"]]";
  // noteString.replace("\"tags\":[]", deviceTag);

  Serial.print("📄 Generated Note JSON: ");
  Serial.println(noteString);

  // 👉 Debug the characters around byte 2048
  // for (int i = 2040; i < 2060 && i < noteString.length(); i++) {
  //   Serial.print("[" + String(i) + "] ");
  //   Serial.print((int)noteString[i]); // ASCII/UTF-8 byte value
  //   Serial.print(" ('");
  //   Serial.print(noteString[i]);      // Actual char (if printable)
  //   Serial.println("')");
  // }
  
  lastEventID = extractEventID(noteString);
  if (lastEventID == "") {
    Serial.println("❌ Failed to extract event ID from note JSON.");
    eventInProgress = false;
    resetHeapAndLog("extractEventID failed");
    return;
  }
  Serial.print("Extracted Event ID: ");
  Serial.println(lastEventID);
  
  // --- Connect to Relay (max 10 sec) ---
  if (!isRelayConnected) {
    Serial.println("🔗 Not connected. Resetting and attempting to connect to relays...");
    nostrRelayManager.disconnect();
    delay(300); // Important to allow sockets to clean up
    nostrRelayManager.connect();
    unsigned long connStart = millis();
    while (!isRelayConnected && (millis() - connStart < 10000)) {
      nostrRelayManager.loop();
      delay(20); yield();  // critical in long-running loops
    }
    if (!isRelayConnected) {
      Serial.println("❌ Failed to connect to any relay within 10 seconds.");
      blinkRedTimes(5);
      eventInProgress = false;  // mark event done if failed
      Serial.println("🧹 Cleaning up memory after relay connection failure...");
      resetHeapAndLog("relay connection fail");
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

  // Serial.print("📉 Free heap after enqueue:: ");
  // Serial.println(ESP.getFreeHeap());
  
  eventSentTime = millis();  // Timestamp when the event was sent (used for confirmation timeout).
  lastRelayUse = millis();   // Timestamp of last relay activity (used to disconnect when idle).
  pendingPrefix = prefix;    // Stores the prefix (GM/GN) to show in Morse code feedback.
  eventConfirmed = false;    // Reset confirmation flag; will be set true when event is seen in relay.
  eventInProgress = true;    // Marks that we are currently processing an event.
  allowRelayLoop = true;     // Enables relay loop to process send and confirmation.
  lastPostTime = millis();   // Timestamp of last event sent (used to manage cooldown).
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
      Serial.print("📉 Heap before disconnect: ");
      Serial.println(ESP.getFreeHeap());

      resetHeapAndLog("confirmed event");

      eventInProgress = false;
      allowRelayLoop = false; // 🔻 Turn off relay loop now!
      nostrRelayManager.disconnect();
    }

    else if (now - eventSentTime > CONFIRM_TIMEOUT) {
      Serial.println("⚠️ No confirmation received within timeout; indicating failure.");
      blinkRedTimes(5);
      // Turn off yellow LED before starting Morse feedback.
      digitalWrite(LED_YELLOW, HIGH);

      resetHeapAndLog("event timeout");
    
      eventInProgress = false;
      allowRelayLoop = false;  // 🔻 Turn off relay loop now too!
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

String getRandomStickyFromFileFast(const char* path, std::vector<uint32_t>& offsets) {
  if (offsets.empty()) {
    Serial.println("⚠️ Sticky index is empty for: " + String(path));
    return "";
  }

  int index = random(offsets.size());
  uint32_t start = offsets[index];
  uint32_t end;

  if (index < offsets.size() - 1) {
    end = offsets[index + 1] - 6; // subtract length of <<<>>>
  } else {
    // End of file
    File temp = LittleFS.open(path, "r");
    end = temp.size();
    temp.close();
  }

  uint32_t length = end - start;
  if (length <= 0 || length > 2048) { // sanity check
    Serial.println("⚠️ Invalid sticky length or index range.");
    return "";
  }

  File file = LittleFS.open(path, "r");
  if (!file) {
    Serial.println("⚠️ Failed to open file for reading sticky: " + String(path));
    return "";
  }

  file.seek(start); // Move to sticky start
  String sticky = "";
  for (uint32_t i = 0; i < length && file.available(); i++) {
    char c = file.read();
    sticky += c;
  }
  file.close();

  sticky.trim();
  Serial.println("🎯 Picked sticky [" + String(index) + "] (bytes " + String(start) + "-" + String(end) + ")");
  return sticky;
}

String mergeSticky(String baseMsg, const char* path, std::vector<uint32_t>& offsets) {
  String sticky = getRandomStickyFromFileFast(path, offsets);
  if (sticky.length() > 0) {
    baseMsg += "\n" + sticky;
    Serial.print("✅ Final merged message: ");
    Serial.println(baseMsg);
  } else {
    Serial.println("⚠️ No sticky added.");
  }
  return baseMsg;
}

// -----------------------------------------------------
// Normal Operation Loop (Button Handling)
// -----------------------------------------------------
void normalOperationLoop() {

  bool currentGM = (digitalRead(BUTTON_GM) == LOW);
  bool currentGN = (digitalRead(BUTTON_GN) == LOW);

  // Track button press (falling edge)
  if (currentGM && !lastStateGM) pressTimeGM = millis();
  if (currentGN && !lastStateGN) pressTimeGN = millis();

  // Relay & feedback
  if (allowRelayLoop) {
    nostrRelayManager.loop();
    nostrRelayManager.broadcastEvents();
  }
  processEventFeedback();

  // Timeout cleanup
  if (eventInProgress && millis() - eventSentTime > (CONFIRM_TIMEOUT + 10000)) {
    Serial.println("🛑 Failsafe triggered: eventInProgress stuck too long, resetting...");
    eventInProgress = false;
    resetHeapAndLog("failsafe reset");
    nostrRelayManager.disconnect();
  }

  // Cooldown
  if (millis() - lastPostTime < cooldownTime) {
    unsigned long remainingSec = (cooldownTime - (millis() - lastPostTime)) / 1000;
    static unsigned long lastPrintMinute = 999999;
    unsigned long currentMinute = remainingSec / 60;

    if (currentMinute != lastPrintMinute) {
      Serial.println("⏳ Cooldown active: " + String(remainingSec) + " sec left");
      lastPrintMinute = currentMinute;
    }

    if (!currentGM && lastStateGM)
      Serial.println("GM button pressed during cooldown period; please wait."), countdownLEDBlinker(5000);
    if (!currentGN && lastStateGN)
      Serial.println("GN button pressed during cooldown period; please wait."), countdownLEDBlinker(5000);

    // 🔁 Jump out — we'll still update lastState below
    goto END;
  }

  if (eventInProgress) {
    Serial.println("Event still in progress — waiting...");
    goto END;
  }

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
        String messageToSend = mergeSticky(btn1Msg, "/btn1_sticky.txt", btn1StickyOffsets);
        Serial.println("📦 Full message to send:\n" + messageToSend);
        delay(2000);  // Give time for string operations to settle
        sendEvent(messageToSend, "GM");
        delay(20); yield();  // Let ESP breathe
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
        String messageToSend = mergeSticky(btn2Msg, "/btn2_sticky.txt", btn2StickyOffsets);
        Serial.println("📦 Full message to send:\n" + messageToSend);
        delay(2000);  // Give time for string operations to settle
        sendEvent(messageToSend, "GN");
        delay(20); yield();  // Let ESP breathe
        lastPostTime = millis();
      }
    }
    pressTimeGN = 0;
  }
END:
  lastStateGM = currentGM;
  lastStateGN = currentGN;
}

// -----------------------------------------------------
// Setup and Main Loop
// -----------------------------------------------------
void setup() {
  Serial.begin(115200);

  if (!LittleFS.begin(true)) {
    Serial.println("❌ LittleFS Mount Failed");
  } else {
      Serial.println("✅ LittleFS mounted successfully");
      Serial.println("btn1_sticky.txt exists: " + String(LittleFS.exists("/btn1_sticky.txt")));
      Serial.println("btn2_sticky.txt exists: " + String(LittleFS.exists("/btn2_sticky.txt")));
  }
  checkStickyFile();

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

bool offsetsPrinted = false;

void loop() {
  // Keep handling configuration portal if AP is active (either AP or AP+STA mode)
  if (WiFi.getMode() & WIFI_AP) {
    server.handleClient();
  } else {
    normalOperationLoop();

    // // for debugging
    // if (digitalRead(BUTTON_GN) == LOW || digitalRead(BUTTON_GM) == LOW) {
    //   Serial.println("👀 Button is physically pressed.");
    // }

    // for debugging
    if (!offsetsPrinted && (btn1StickyOffsets.size() > 0 || btn2StickyOffsets.size() > 0)) {
      Serial.println("✅ Offset count:");
      Serial.println(" - GM stickies: " + String(btn1StickyOffsets.size()));
      Serial.println(" - GN stickies: " + String(btn2StickyOffsets.size()));
      offsetsPrinted = true; // Prints stickies count once
    }

    // // 🧠 Monitor heap every 10 seconds
    // static unsigned long lastHeapCheck = 0;
    // if (millis() - lastHeapCheck > 10000) {
    //   Serial.print("📉 Free heap after enqueue:: ");
    //   Serial.println(ESP.getFreeHeap());
    //   lastHeapCheck = millis();
    // }

    // Automatically disconnect from relay if idle too long (saves memory)
    if (!eventInProgress && isRelayConnected && millis() - lastRelayUse > RELAY_IDLE_TIMEOUT) {
      // Serial.println("📴 Disconnecting from relay (idle timeout cleanup).");
      resetHeapAndLog("failsafe reset");
      nostrRelayManager.disconnect();
    }
  }
  
  // After set amount of time, disable the AP if STA is connected.
  if (!apDisabled && (millis() - bootTime > apAccessTimer)) {
    Serial.println("🔌 7 minutes elapsed: disabling AP mode to reduce power consumption.");
    WiFi.softAPdisconnect(true);  // Turn off the AP interface.
    apDisabled = true;
  }
}
