/*
Monitor a JDB Battery Management System via RS485 communication

Influx DB can be created with command influx -execute "create database Monitor_JbdBms"

Monitors GPIO 0 pulled to ground as key press to toggle charge mosfet
Use builtin led to represent health status
Use defined pin LED to represent load status
Use pin 22 to toggle RS485 read/write
*/


#include <Arduino.h>

// Config for ESP8266 or ESP32
#if defined(ESP8266)
    #include <SoftwareSerial.h>
    SoftwareSerial rs485;  // Use SoftwareSerial port

    #define HEALTH_LED_ON LOW
    #define HEALTH_LED_OFF HIGH
    #define HEALTH_LED_PIN LED_BUILTIN
    #define LOAD_LED_ON HIGH
    #define LOAD_LED_OFF LOW
    #define LOAD_LED_PIN D5
    #define LOAD_BUTTON_PIN 0

    // Web Updater
    #include <ESP8266HTTPUpdateServer.h>
    #include <ESP8266WebServer.h>
    #include <ESP8266WiFi.h>
    #include <ESP8266mDNS.h>
    #include <WiFiClient.h>
    #define WebServer ESP8266WebServer
    #define HTTPUpdateServer ESP8266HTTPUpdateServer

    // Post to InfluxDB
    #include <ESP8266HTTPClient.h>

    // Time sync
    #include <NTPClient.h>
    #include <WiFiUdp.h>
    WiFiUDP ntpUDP;
    NTPClient ntp(ntpUDP, NTP_SERVER);
#elif defined(ESP32)
    HardwareSerial &rs485 = Serial2;

    #define HEALTH_LED_ON HIGH
    #define HEALTH_LED_OFF LOW
    #define HEALTH_LED_PIN LED_BUILTIN
    #define HEALTH_PWM_CH 0
    #define LOAD_LED_ON HIGH
    #define LOAD_LED_OFF LOW
    #define LOAD_LED_PIN 5
    #define LOAD_BUTTON_PIN 0

    // Web Updater
    #include <HTTPUpdateServer.h>
    #include <WebServer.h>
    #include <WiFi.h>
    #include <ESPmDNS.h>
    #include <WiFiClient.h>

    // Post to InfluxDB
    #include <HTTPClient.h>
    
    // Time sync
    #include <time.h>
#else
    #error "No ESP8266 or ESP32, define your rs485 stream, pins and includes here!"
#endif

// Infrastructure
#include <Syslog.h>
#include <WiFiManager.h>

// Web status page and OTA updater
#define WEBSERVER_PORT 80

WebServer web_server(WEBSERVER_PORT);
HTTPUpdateServer esp_updater;

// Post to InfluxDB
WiFiClient client;
HTTPClient http;
int influx_status = 0;
time_t post_time = 0;

// Breathing status LED
const uint32_t ok_interval = 5000;
const uint32_t err_interval = 1000;

uint32_t breathe_interval = ok_interval; // ms for one led breathe cycle
bool enabledBreathing = true;  // global flag to switch breathing animation on or off

#ifndef PWMRANGE
#define PWMRANGE 1023
#define PWMBITS 10
#endif

// Syslog
WiFiUDP logUDP;
Syslog syslog(logUDP, SYSLOG_PROTO_IETF);
char msg[512];  // one buffer for all syslog and json messages
char start_time[30];

// JbdBms device
#include <jbdbms.h>

#define RS485_DIR_PIN 22  // != -1: Use pin for explicit DE/!RE

JbdBms jbdbms(rs485);  // Serial port with RS485 converter


// Post data to InfluxDB
bool postInflux(const char *line) {
    static const char uri[] = "/write?db=" INFLUX_DB "&precision=s";

    http.begin(client, INFLUX_SERVER, INFLUX_PORT, uri);
    http.setUserAgent(PROGNAME);
    influx_status = http.POST(line);
    String payload;
    if (http.getSize() > 0) { // workaround for bug in getString()
        payload = http.getString();
    }
    http.end();

    if (influx_status < 200 || influx_status >= 300) {
        breathe_interval = err_interval;
        syslog.logf(LOG_ERR, "Post %s:%d%s status=%d line='%s' response='%s'",
            INFLUX_SERVER, INFLUX_PORT, uri, influx_status, line, payload.c_str());
        return false;
    }

    breathe_interval = ok_interval; // TODO mix with other possible errors
    post_time = time(NULL);         // TODO post_time?
    return true;
}


JbdBms::Hardware_t jbdHardware = {0};

bool json_Hardware(char *json, size_t maxlen, JbdBms::Hardware_t data) {
    static const char jsonFmt[] = "{\"Version\":" VERSION ",\"Id\":\"%.32s\"}";
    int len = snprintf(json, maxlen, jsonFmt, data.id);

    return len < maxlen;
}


void handle_jbdHardware() {
    static const uint32_t interval = 600000;
    static uint32_t prev = 0 - interval + 0;  // check at start first

    uint32_t now = millis();
    if( now - prev >= interval ) {
        prev += interval;
        JbdBms::Hardware_t data = {0};
        if (jbdbms.getHardware(data)) {
            if (strncmp((const char *)data.id, (const char *)jbdHardware.id, sizeof(data.id))) {
                // found a new/different JBD BMS
                static const char lineFmt[] =
                    "Hardware,Id=%.32s,Version=" VERSION " "
                    "Host=\"%s\"";

                jbdHardware = data;
                json_Hardware(msg, sizeof(msg), data);
                Serial.println(msg);
                syslog.log(LOG_INFO, msg);
                // TODO mqtt.publish(topic, msg);
                snprintf(msg, sizeof(msg), lineFmt, (char *)data.id, WiFi.getHostname());
                postInflux(msg);
            }
        }
        else {
            Serial.println("getHardware error");
        }
    }
}


JbdBms::Status_t jbdStatus = {0};

bool json_Status(char *json, size_t maxlen, JbdBms::Status_t data) {
    static const char jsonFmt[] =
        "{\"Version\":" VERSION ",\"Id\":\"%.32s\",\"Status\":{"
        "\"voltage\":%u,"
        "\"current\":%d,"
        "\"remainingCapacity\":%u,"
        "\"nominalCapacity\":%u,"
        "\"cycles\":%u,"
        "\"productionDate\":\"%04u-%02u-%02u\","
        "\"balance\":\"%s\","
        "\"fault\":%u,"
        "\"version\":%u,"
        "\"currentCapacity\":%u,"
        "\"mosfetStatus\":%u,"
        "\"cells\":%u,"
        "\"ntcs\":%u,"
        "\"temperatures\":%s]}}";
    char temps[sizeof(data.temperatures)/sizeof(*data.temperatures) * 6 + 1] = "[";
    int len = 0;

    for (size_t i = 0; i < data.ntcs && i < sizeof(data.temperatures)/sizeof(*data.temperatures) && len < sizeof(temps); i++) {
        char *str = &temps[len];
        len += snprintf(str, sizeof(temps) - len, ",%d", JbdBms::deciCelsius(data.temperatures[i]));
    }
    temps[0] = '[';  // replace first comma

    if (len < sizeof(temps)) {
        len = snprintf(json, maxlen, jsonFmt, jbdHardware.id,
            data.voltage, data.current, data.remainingCapacity, data.nominalCapacity, data.cycles, 
            JbdBms::year(data.productionDate), JbdBms::month(data.productionDate), JbdBms::day(data.productionDate), 
            JbdBms::balance(data), data.fault, data.version, 
            data.currentCapacity, data.mosfetStatus, data.cells, data.ntcs, temps);

        return len < maxlen;
    }
    return false;
}


void handle_jbdStatus() {
    static const uint32_t interval = 10000;
    static uint32_t prev = 0 - interval + 600;

    uint32_t now = millis();
    if( now - prev >= interval ) {
        prev += interval;
        JbdBms::Status_t data = {0};
        if (jbdbms.getStatus(data)) {
            if (memcmp(&data, &jbdStatus, sizeof(data))) {
                // some voltage has changed
                static const char lineFmt[] =
                    "Status,Id=%.32s,Version=" VERSION " "
                    "Host=\"%s\","
                    "voltage=%u,"
                    "current=%d,"
                    "remainingCapacity=%u,"
                    "nominalCapacity=%u,"
                    "cycles=%u,"
                    "productionDate=\"%04u-%02u-%02u\","
                    "balance=\"%s\","
                    "fault=%u,"
                    "version=%u,"
                    "currentCapacity=%u,"
                    "mosfetStatus=%u,"
                    "cells=%u,"
                    "ntcs=%u";

                jbdStatus = data;
                json_Status(msg, sizeof(msg), data);
                Serial.println(msg);
                syslog.log(LOG_INFO, msg);
                // TODO mqtt.publish(topic, msg);
                
                size_t len = snprintf(msg, sizeof(msg), lineFmt, jbdHardware.id, WiFi.getHostname(), 
                    data.voltage, data.current, data.remainingCapacity, data.nominalCapacity, data.cycles,
                    JbdBms::year(data.productionDate), JbdBms::month(data.productionDate), JbdBms::day(data.productionDate), 
                    JbdBms::balance(data), data.fault, data.version,
                    data.currentCapacity, data.mosfetStatus, data.cells, data.ntcs);

                for (size_t i = 0; i < sizeof(data.temperatures)/sizeof(*data.temperatures) && i < data.ntcs && len < sizeof(msg); i++) {
                    char *str = &msg[len];
                    len += snprintf(str, sizeof(msg) - len, ",temperature%u=%d", i+1, JbdBms::deciCelsius(data.temperatures[i]));
                }

                postInflux(msg);
            }
        }
        else {
            Serial.println("getStatus error");
        }
    }
}


JbdBms::Cells_t jbdCells = {0};

bool json_Cells(char *json, size_t maxlen, JbdBms::Cells_t data) {
    static const char jsonFmt[] = "{\"Version\":" VERSION ",\"Id\":\"%.32s\",\"Cells\":%s]}";
    char voltages[sizeof(data.voltages)/sizeof(*data.voltages) * 6 + 1] = "[";
    int len = 0;

    for (size_t i = 0; i < jbdStatus.cells && i < sizeof(data.voltages)/sizeof(*data.voltages) && len < sizeof(voltages); i++) {
        char *str = &voltages[len];
        len += snprintf(str, sizeof(voltages) - len, ",%u", data.voltages[i]);
    }
    voltages[0] = '[';  // replace first comma

    if (len < sizeof(voltages)) {
        len = snprintf(json, maxlen, jsonFmt, jbdHardware.id, voltages);

        return len < maxlen;
    }
    return false;
}


void handle_jbdCells() {
    static const uint32_t interval = 10000;
    static uint32_t prev = 0 - interval + 700;  // after handle_jbdStatus() so we have valid jbdStatus.cells

    uint32_t now = millis();
    if( now - prev >= interval ) {
        prev += interval;
        JbdBms::Cells_t data = {0};
        if (jbdbms.getCells(data)) {
            if (memcmp(&data, &jbdCells, sizeof(data))) {
                // some voltage has changed
                static const char lineFmt[] =
                    "Cells,Id=%.32s,Version=" VERSION " "
                    "Host=\"%s\"";

                jbdCells = data;
                json_Cells(msg, sizeof(msg), data);
                Serial.println(msg);
                syslog.log(LOG_INFO, msg);
                // TODO mqtt.publish(topic, msg);
                size_t len = snprintf(msg, sizeof(msg), lineFmt, jbdHardware.id, WiFi.getHostname());
                for (size_t i=0; i < sizeof(data.voltages)/sizeof(*data.voltages) && len < sizeof(msg) && i < jbdStatus.cells; i++) {
                    char *str = &msg[len];
                    len += snprintf(str, sizeof(msg) - len, ",voltage%u=%u", i+1, data.voltages[i]);
                }
                postInflux(msg);
            }
        }
        else {
            Serial.println("getCells error");
        }
    }
}


// Standard web page
const char *main_page( const char *body ) {
    static const char fmt[] =
        "<html>\n"
        " <head>\n"
        "  <title>" PROGNAME " %.32s v" VERSION "</title>\n"
        "  <meta http-equiv=\"expires\" content=\"5\">\n"
        " </head>\n"
        " <body>\n"
        "  <h1>" PROGNAME " %.32s v" VERSION "</h1>\n"
        "  <table><form action=\"mosfets\" method=\"post\"><tr>\n"
        "    <td><input type=\"checkbox\" name=\"charge\" id=\"charge\" value=\"Charge\" %s/><label for=\"charge\">Charge</label></td>\n"
        "    <td><input type=\"checkbox\" name=\"discharge\" id=\"discharge\" value=\"Discharge\" %s/><label for=\"discharge\">Discharge</label></td>\n"
        "    <td><input type=\"submit\" name=\"mosfets\" value=\"Set Mosfets\" />\n"
        "  </tr></form></table></p>\n"
        "  <p><strong>%s</strong></p>\n"
        "  <p><table>\n"
        "   <tr><td>Status</td><td><a href=\"/json/Status\">JSON</a></td></tr>\n"
        "   <tr><td>Cells</td><td><a href=\"/json/Cells\">JSON</a></td></tr>\n"
        "   <tr><td>Post firmware image to</td><td><a href=\"/update\">/update</a></td></tr>\n"
        "   <tr><td>Last start time</td><td>%s</td></tr>\n"
        "   <tr><td>Last web update</td><td>%s</td></tr>\n"
        "   <tr><td>Last influx update</td><td>%s</td></tr>\n"
        "   <tr><td>Influx status</td><td>%d</td></tr>\n"
        "  </table></p>\n"
        "  <p><table><tr>\n"
        "   <td><form action=\"/\" method=\"get\">\n"
        "    <input type=\"submit\" name=\"reload\" value=\"Reload\" />\n"
        "   </form></td>\n"
        "   <td><form action=\"breathe\" method=\"post\">\n"
        "    <input type=\"submit\" name=\"breathe\" value=\"Toggle Breathe\" />\n"
        "   </form></td>\n"
        "   <td><form action=\"reset\" method=\"post\">\n"
        "    <input type=\"submit\" name=\"reset\" value=\"Reset ESP\" />\n"
        "   </form></td>\n"
        "  </tr></table></p>\n"
        " </body>\n"
        "</html>\n";
    static char page[sizeof(fmt) + 500] = "";
    static char curr_time[30], influx_time[30];
    time_t now;
    time(&now);
    strftime(curr_time, sizeof(curr_time), "%FT%T%Z", localtime(&now));
    strftime(influx_time, sizeof(influx_time), "%FT%T%Z", localtime(&post_time));
    snprintf(page, sizeof(page), fmt, jbdHardware.id, jbdHardware.id, 
        jbdStatus.mosfetStatus & JbdBms::MOSFET_CHARGE ? "checked " : "", 
        jbdStatus.mosfetStatus & JbdBms::MOSFET_DISCHARGE ? "checked " : "", 
        body, start_time, curr_time, influx_time, influx_status);
    return page;
}


// Define web pages for update, reset or for event infos
void setup_webserver() {
    web_server.on("/mosfets", HTTP_POST, []() {
        uint8_t mosfetStatus = 0;
        const char *msg = "Mosfet status unchanged";

        if (web_server.hasArg("charge") && web_server.arg("charge") == "Charge") {
            mosfetStatus |= JbdBms::MOSFET_CHARGE;
        }
        if (web_server.hasArg("discharge") && web_server.arg("discharge") == "Discharge") {
            mosfetStatus |= JbdBms::MOSFET_DISCHARGE;
        }
        if (mosfetStatus != jbdStatus.mosfetStatus) {
            if (jbdbms.setMosfetStatus((JbdBms::mosfet_t)mosfetStatus)) {
                jbdStatus.mosfetStatus = mosfetStatus;
                switch (mosfetStatus) {
                    case JbdBms::MOSFET_NONE:
                        msg = "Charge and discharge OFF";
                        break; 
                    case JbdBms::MOSFET_CHARGE:
                        msg = "Charge ON and discharge OFF";
                        break; 
                    case JbdBms::MOSFET_DISCHARGE:
                        msg = "Charge OFF and discharge ON";
                        break; 
                    case JbdBms::MOSFET_BOTH:
                        msg = "Charge and discharge ON";
                        break; 
                }
            }
            else {
                msg = "Set mosfet status failed";
            }
        }

        web_server.send(200, "text/html", main_page(msg)); 
    });


    web_server.on("/json/Status", []() {
        json_Status(msg, sizeof(msg), jbdStatus);
        web_server.send(200, "application/json", msg);
    });

    web_server.on("/json/Cells", []() {
        json_Cells(msg, sizeof(msg), jbdCells);
        web_server.send(200, "application/json", msg);
    });


    // Call this page to reset the ESP
    web_server.on("/reset", HTTP_POST, []() {
        syslog.log(LOG_NOTICE, "RESET");
        web_server.send(200, "text/html",
                        "<html>\n"
                        " <head>\n"
                        "  <title>" PROGNAME " v" VERSION "</title>\n"
                        "  <meta http-equiv=\"refresh\" content=\"7; url=/\"> \n"
                        " </head>\n"
                        " <body>Resetting...</body>\n"
                        "</html>\n");
        delay(200);
        ESP.restart();
    });

    // Index page
    web_server.on("/", []() { 
        web_server.send(200, "text/html", main_page(""));
    });

    // Toggle breathing status led if you dont like it or ota does not work
    web_server.on("/breathe", HTTP_POST, []() {
        enabledBreathing = !enabledBreathing; 
        web_server.send(200, "text/html", main_page(enabledBreathing ? "breathing enabled" : "breathing disabled")); 
    });

    web_server.on("/breathe", HTTP_GET, []() {
        web_server.send(200, "text/html", main_page(enabledBreathing ? "breathing enabled" : "breathing disabled")); 
    });

    // Catch all page
    web_server.onNotFound( []() { 
        web_server.send(404, "text/html", main_page("<h2>page not found</h2>\n")); 
    });

    web_server.begin();

    MDNS.addService("http", "tcp", WEBSERVER_PORT);
    syslog.logf(LOG_NOTICE, "Serving HTTP on port %d", WEBSERVER_PORT);
}


// toggle charge mosfet on key press
// pin is pulled up if released and pulled down if pressed
void handle_load_button( bool loadOn ) {
    static uint32_t prevTime = 0;
    static uint32_t debounceStatus = 1;
    static bool pressed = false;

    uint32_t now = millis();
    if( now - prevTime > 2 ) {  // debounce check every 2 ms, decision after 2ms/bit * 32bit = 64ms
        prevTime = now;

        // shift bits left, set lowest bit if button pressed
        debounceStatus = (debounceStatus << 1) | ((digitalRead(LOAD_BUTTON_PIN) == LOW) ? 1 : 0);

        if( debounceStatus == 0 && pressed ) {
            pressed = false;
        }
        else if( debounceStatus == 0xffffffff && !pressed ) {
            pressed = true;
            JbdBms::Status_t data = {0};
            jbdbms.getStatus(data);
            data.mosfetStatus ^= JbdBms::MOSFET_CHARGE;
            if (jbdbms.setMosfetStatus((JbdBms::mosfet_t)data.mosfetStatus)) {
                if( data.mosfetStatus & JbdBms::MOSFET_CHARGE ) {
                    Serial.println("Charge mosfet switched ON");
                }
                else {
                    Serial.println("Charge mosfet switched OFF");
                }
            }
            else {
                Serial.println("Charge mosfet status UNKNOWN");
            }
        }
    }
}


// check once every 500ms if load status has changed
// return true if load is on (or unknown)
bool handle_load_led() {
    static uint32_t prevTime = 0;
    static bool prevStatus = false;  // status unknown
    static bool prevLoad = true;     // assume load is on

    uint32_t now = millis();
    if( now - prevTime > 500 ) {
        prevTime = now;
        JbdBms::Status_t data = {0};
        bool rc = jbdbms.getStatus(data);
        bool loadOn = data.mosfetStatus & JbdBms::MOSFET_CHARGE;
        if( rc ) {
            if( !prevStatus || loadOn != prevLoad ) {
                if( loadOn ) {
                    digitalWrite(LOAD_LED_PIN, LOAD_LED_ON);
                    Serial.println("Charge mosfet is ON");
                }
                else {
                    digitalWrite(LOAD_LED_PIN, LOAD_LED_OFF);
                    Serial.println("Charge mosfet is OFF");
                }
                prevStatus = true;
                prevLoad = loadOn;
            }
        }
        else {
            if( prevStatus ) {
                digitalWrite(LOAD_LED_PIN, LOAD_LED_ON);  // assume ON
                Serial.println("Charge mosfet is UNKNOWN");
                prevStatus = false;
                prevLoad = true;
            }
        }
    }

    return prevLoad;
}


// check ntp status
// return true if time is valid
bool check_ntptime() {
    static bool have_time = false;

    #if defined(ESP32)
        bool valid_time = time(0) > 1582230020;
    #else
        ntp.update();
        bool valid_time = ntp.isTimeSet();
    #endif

    if (!have_time && valid_time) {
        have_time = true;
        time_t now = time(NULL);
        strftime(start_time, sizeof(start_time), "%FT%T%Z", localtime(&now));
        syslog.logf(LOG_NOTICE, "Got valid time at %s", start_time);
    }

    return have_time;
}


// Status led update
void handle_breathe() {
    static uint32_t start = 0;  // start of last breath
    static uint32_t min_duty = PWMRANGE / 20;  // limit min brightness
    static uint32_t max_duty = PWMRANGE / 2;  // limit max brightness
    static uint32_t prev_duty = 0;

    // map elapsed in breathing intervals
    uint32_t now = millis();
    uint32_t elapsed = now - start;
    if (elapsed > breathe_interval) {
        start = now;
        elapsed -= breathe_interval;
    }

    // map min brightness to max brightness twice in one breathe interval
    uint32_t duty = (max_duty - min_duty) * elapsed * 2 / breathe_interval + min_duty;
    if (duty > max_duty) {
        // second range: breathe out aka get darker
        duty = 2 * max_duty - duty;
    }

    duty = duty * duty / max_duty;  // generally reduce lower brightness levels

    if (duty != prev_duty) {
        // adjust pwm duty cycle
        prev_duty = duty;
        #if defined(ESP32)
            ledcWrite(HEALTH_PWM_CH, duty);
        #else
            analogWrite(HEALTH_LED_PIN, PWMRANGE - duty);
        #endif
    }
}


// Startup
void setup() {
    WiFi.mode(WIFI_STA);
    String host(HOSTNAME);
    host.toLowerCase();
    WiFi.hostname(host.c_str());

    pinMode(HEALTH_LED_PIN, OUTPUT);
    digitalWrite(HEALTH_LED_PIN, HEALTH_LED_ON);

    Serial.begin(BAUDRATE);
    Serial.println("\nStarting " PROGNAME " v" VERSION " " __DATE__ " " __TIME__);

    // Syslog setup
    syslog.server(SYSLOG_SERVER, SYSLOG_PORT);
    syslog.deviceHostname(WiFi.getHostname());
    syslog.appName("Joba1");
    syslog.defaultPriority(LOG_KERN);

    digitalWrite(HEALTH_LED_PIN, HEALTH_LED_OFF);

    WiFiManager wm;
    // wm.resetSettings();
    if (!wm.autoConnect()) {
        Serial.println("Failed to connect WLAN");
        for (int i = 0; i < 1000; i += 200) {
            digitalWrite(HEALTH_LED_PIN, HEALTH_LED_ON);
            delay(100);
            digitalWrite(HEALTH_LED_PIN, HEALTH_LED_OFF);
            delay(100);
        }
        ESP.restart();
        while (true)
            ;
    }

    digitalWrite(HEALTH_LED_PIN, HEALTH_LED_ON);
    char msg[80];
    snprintf(msg, sizeof(msg), "%s Version %s, WLAN IP is %s", PROGNAME, VERSION,
        WiFi.localIP().toString().c_str());
    Serial.println(msg);
    syslog.logf(LOG_NOTICE, msg);

    #if defined(ESP8266)
        ntp.begin();
    #else
        configTime(0, 0, NTP_SERVER);
    #endif

    MDNS.begin(WiFi.getHostname());

    esp_updater.setup(&web_server);
    setup_webserver();

#if defined(ESP8266)
    rs485.begin(9600, SWSERIAL_8N1, 13, 15);  // Use pins 13 and 15 for RX and TX
    analogWriteRange(PWMRANGE);  // for health led breathing steps
#elif defined(ESP32)
    rs485.begin(9600, SERIAL_8N1);  // Use Serial2 default pins 16 and 17 for RX and TX
    ledcAttachPin(HEALTH_LED_PIN, HEALTH_PWM_CH);
    ledcSetup(HEALTH_PWM_CH, 1000, PWMBITS);
#else
    analogWriteRange(PWMRANGE);  // for health led breathing steps
    // init your non ESP serial port here
#endif

    pinMode(LOAD_BUTTON_PIN, INPUT_PULLUP);  // to toggle load status
    pinMode(LOAD_LED_PIN, OUTPUT);  // to show load status
    digitalWrite(LOAD_LED_PIN, LOAD_LED_OFF);

    jbdbms.begin(RS485_DIR_PIN);

    Serial.println("Setup done");
}


// Main loop
void loop() {
    // TODO set/reset err_interval for breathing
    handle_jbdHardware();
    bool have_time = check_ntptime();
    if( jbdHardware.id[0] ) {  // we have required infos
        if (have_time && enabledBreathing) {
            handle_breathe();
        }
        handle_jbdStatus();
        handle_jbdCells();
    }
    handle_load_button(handle_load_led());
    web_server.handleClient();
}
