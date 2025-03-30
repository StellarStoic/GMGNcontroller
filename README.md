# GMGNcontroller

The GMGNcontroller is an ESP32-based project that triggers Nostr Kind 1 plaintext note sending events via button presses. This lightweight device lets you send 'Good Morning' (GM) or 'Good Night' (GN) messages—with optional additional text—by simply pressing a button. It leverages the [arduino-nostr](https://github.com/lnbits/arduino-nostr) library. Of course GM and GN are not hardcoded and you can chose whatever you like to be posted.

## Overview

- **Core Functionality:** Trigger Nostr Kind 1 plaintext note events with button presses  
- **Buttons:**
  - **Btn1:** Posts GM (Good Morning) - Can be whatever
  - **Btn2:** Posts GN (Good Night) - Can be whatever
  - **stickyStrings** Are used to randomly append strings like links, quotes, emojis to existing Btn1 or Btn2 message. Useful for more diverse and unique posing
- **Configuration:** Easily configurable via a webserver hosted on `192.168.4.1`
- **WiFi Access Point:** When the ESP32 boots, it creates the `GMGNcontroller` WiFi network (Password: `gmgnprotocol`) for 7 minutes, allowing you to configure settings.

## Features

- **Customizable Messages:** Change the GM/GN messages via the webserver.
- **Customizable Sticky Messages** Add a list of sentences which will be randomly appended to the button message. (use <<<>>> as a delimiter between sentences)
- **Configurable Button Timing:** Set specific UTC times during which each button is active.
- **Cooldown Timer:** Define a pause interval between button presses.
- **User Personalization:** Set your user name, and receive personalized greetings (e.g., GM `{userName}` or GN `{userName}`) with a blinking green LED in Morse code.
- **Relay Selection:** By default, the system selects 3 relays. However, you can choose any relay by leaving empty or modifying the relay settings but don't chose more then 3! **<- This is very important**
- **Visual Feedback:** Three LEDs indicate different statuses and processes running in the background.
- **Battery Optimization:** The configuration mode is only active for 7 minutes after boot to enhance security and save battery if running on battery power.

## Setup & Usage

1. **Hardware:**
   - The unit I used and build on was ESP WROOM 32 (ESP32-D0WD-V3 (revision v3.0)) microcontroller but other esp32 with wifi module will probably work as well. (Please let me know which ones you used)
   - 3 LEDs Green, Yellow, Red
   - 3 Resistors for LEDs
   - Buttons (any kind of buttons you want or think of something else like triggering with sensors)

2. **Firmware Upload:**
   - Open the new project in your Arduino IDE.
   - Ensure that the [arduino-nostr](https://github.com/lnbits/arduino-nostr) library is installed.
   - You'll also need a few more libraries arduino-nostr is dependant on - **ArduinoJson 7.3.1**, **uBitcoin 0.2.0**, and **WebSockets 2.6.1**
   - Verify and compile and that's it. If all went well, all 3 LEDs should start blinking on boot for a few seconds.

   ![GMGNcontroller Image](GMGNcontrollerIRL.jpg)

3. **Configuration:**
   - On boot, the ESP32 creates a WiFi network named `GMGNcontroller`. Use password `gmgnprotocol`.
   - Connect to this network within the 7-minute window.
   - Navigate to `http://192.168.4.1` in your web browser to access the configuration interface.
   - Configure:
     - Custom GM and GN messages
     - Add quotes, emojis, links separated by <<<>>>. for example like; sticky message 1 <<<>>> sticky message 2 <<<>>> sticky message 3 (delimiter should not be the first nor last)
     - Active times for each button (UTC time only)
     - Cooldown timer between button presses is hardcoded and set to 30min
     - Your user name for personalized greetings
     - Relay preferences if you want a different relay than the default (leaving the relay input empty will chose default relays: `nos.lol`,`relay.damus.io`, `relay.nostr.band`)
     - home wifi credentials
     - nostr credentials for the key which will sign and post events
    - Click save and the device should reboot. After all three LEDs stop blinking, wait for 7 min before you press the button 

4. **Operation:**
   - After configuration,the device will automatically disable the WiFi network after 7 minutes for security and power saving.
   - After every reboot there's a 7 min waiting period before Buttons start triggering events and in this time WiFi AP is active. Let's say that buttons are warming up.
   - After the warming up period, press Btn1 or Btn2 to post a message to Nostr.

![GMGNcontroller Image](GMGN_ButtonBox.jpg)

## Contributing

Feel free to fork the repository, open issues, or submit pull requests if you have suggestions or improvements. Contributions are always welcome!
