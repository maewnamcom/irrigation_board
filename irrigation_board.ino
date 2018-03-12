#include <FS.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h> 
#include <ESP8266mDNS.h> 
#include <WiFiManager.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>

#include <SPI.h>
#include <Wire.h>
#include <OneWire.h>
#include <TimeLib.h>
#include <NtpClientLib.h>
#include <RtcDateTime.h>
#include <RtcDS3231.h>
#include <DallasTemperature.h>
#include <RtcUtility.h>
#include "Adafruit_MCP23017.h"
#include <Adafruit_MCP3008.h>

MDNSResponder mdns;  //Create an instance 
ESP8266WebServer server(80);

RtcDS3231<TwoWire> RTC(Wire);

Adafruit_MCP23017 mcp;

#define ONE_WIRE_BUS D3
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature dstemp(&oneWire);
Adafruit_MCP3008 adc;

bool wifiFirstConnected = false;

void onSTAConnected (WiFiEventStationModeConnected ipInfo) {
    Serial.printf ("Connected to %s\r\n", ipInfo.ssid.c_str ());
}

////// CONFIG /////////////////
int8_t mhour = 7;
int8_t mmin = 30;
int8_t ehour = 17;
int8_t emin = 0;
int VOD = 15;               // ระยะเวลาในการเปิดวาวล์น้ำ หน่วยเป็นนาที
int maxVOD = 45;            // ระยะเวลาในการเปิดวาวล์น้ำสูงสุด กรณีควบคุมด้วย Sensor หน่วยเป็นนาที
char* NTPServerName = "time.navy.mi.th";
int8_t timeZone = 7;        // Time Zone in GMT ref
int8_t minuteTimeZone = 0;
char admin_username[20] = "admin";
char admin_password[40] = "P@ssw0rd";
float snapMultiplier = 0.04;
//flag for saving data
bool shouldSaveConfig = false;

//callback notifying us of the need to save config
void saveConfigCallback () {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

////// NTP SYNC ///////////////

// Start NTP only after IP network is connected
void onSTAGotIP (WiFiEventStationModeGotIP ipInfo) {
    Serial.printf ("Got IP: %s\r\n", ipInfo.ip.toString ().c_str ());
    Serial.printf ("Connected: %s\r\n", WiFi.status () == WL_CONNECTED ? "yes" : "no");
    //digitalWrite (ONBOARDLED, LOW); // Turn on LED
    mcp.digitalWrite( 2, 1); // Turn on LED
    wifiFirstConnected = true;
}

// Manage network disconnection
void onSTADisconnected (WiFiEventStationModeDisconnected event_info) {
    Serial.printf ("Disconnected from SSID: %s\n", event_info.ssid.c_str ());
    Serial.printf ("Reason: %d\n", event_info.reason);
    //digitalWrite (ONBOARDLED, HIGH); // Turn off LED
    mcp.digitalWrite( 2, 0); // Turn off LED
    NTP.stop(); // NTP sync can be disabled to avoid sync errors
}

void processSyncEvent (NTPSyncEvent_t ntpEvent) {
    if (ntpEvent) {
        Serial.print ("Time Sync error: ");
        if (ntpEvent == noResponse)
            Serial.println ("NTP server not reachable");
        else if (ntpEvent == invalidAddress)
            Serial.println ("Invalid NTP server address");
    } else {
        Serial.print ("Got NTP time: ");
        Serial.println (NTP.getTimeDateString (NTP.getLastNTPSync ()));
    }
}

boolean syncEventTriggered = false; // True if a time even has been triggered
NTPSyncEvent_t ntpEvent; // Last triggered event

///////////////////////////////

int     pcell;
int     moisture;
int     light;
float   stemp = 0;
float   h = 0;

//int page = 1;         // OLED Screen page
byte manual;            // การสั่งเปิดปิดด้วยมือ
byte timerOn = 0;           // Timer trigger
byte userOn;            // User trigger
byte sensorOn = 0;    // Sensor trigger
byte prevManual = 1;
byte prevTimerOn = 0;
byte prevUserOn = 1;
byte prevSensorOn = 0;
byte mstate = 0;
byte ustate = 0;
byte tstate = 0;
//byte sstate = 0;
time_t tempTime = 0;

////////////////////////////////

RtcDateTime currentTime;

void setup() {
  static WiFiEventHandler e1, e2, e3;
  
  Serial.begin(115200);          // Start the Serial communication to send messages to the computer
  Serial.setDebugOutput(true);
  while (! Serial);

  /////////////////////////////////////////

  /////// MCP23017 ////////////////////////
  
  
  mcp.begin();      // use default address 0

  for(int i = 0;i < 8;i++){
    mcp.pinMode(i+8, INPUT);
    //mcp.pullUp(i, HIGH);  // turn on a 100K pullup internally
    mcp.pullUp(i+8, HIGH);  // turn on a 100K pullup internally
    mcp.pinMode(8-(i+1), OUTPUT);  // use the p13 LED as debugging
    if(i <= 3){
      mcp.digitalWrite(i,1);
    }else{
      mcp.digitalWrite(i,0);
    }
  }
  delay(1000);
  mcp.writeGPIOAB(0);
  /////// MCP3008 /////////////////////////

  adc.begin(D5, D7, D6, D8);

  /////////////////////////////////////////
  
  dstemp.begin();
  
  /////// WiFi Connect ////////////////////
  
  startWiFi();                   // Try to connect to some given access points. Then wait for a connection

  // Set up mDNS responder:
  // - first argument is the domain name, in this example
  //   the fully-qualified domain name is "esp8266.local"
  // - second argument is the IP address to advertise
  //   we send our IP address on the WiFi network
  if (!MDNS.begin("smartgarden")) {
    Serial.println("Error setting up MDNS responder!");
    while(1) { 
      delay(1000);
    }
  }
  
  MDNS.addService("http", "tcp", 80);
  ////////////////////////////////////////
  SPIFFS.begin();
  
  Dir dir = SPIFFS.openDir("/");
  while (dir.next()) {
    String fileName = dir.fileName();
    size_t fileSize = dir.fileSize();
    Serial.printf("FS File: %s, size: %s\n", fileName.c_str(), String(fileSize).c_str());
  }
  Serial.println();

  //RTC.Begin();

  /////// Open NTP Connection ////////////

  Serial.print("IP number assigned by DHCP is ");
  Serial.println(WiFi.localIP());

  NTP.onNTPSyncEvent([](NTPSyncEvent_t error) {
    if (error) {
      Serial.print("Time Sync error: ");
      if (error == noResponse)
        Serial.println("NTP server not reachable");
      else if (error == invalidAddress)
        Serial.println("Invalid NTP server address");
      }
    else {
      Serial.print("Got NTP time: ");
      Serial.println(NTP.getTimeDateString(NTP.getLastNTPSync()));
    }
  });
  // Serveur NTP, decalage horaire, heure été - NTP Server, time offset, daylight 
  NTP.begin(NTPServerName, 0, false);
  NTP.setTimeZone(timeZone, minuteTimeZone);
  NTP.setInterval(60000);
  
  //tempTime = NTP.getTime();

  //currentTime = RtcDateTime(year(tempTime),month(tempTime),day(tempTime),hour(tempTime),minute(tempTime),second(tempTime)); //define date and time object
  //RTC.SetDateTime(currentTime);

  NTP.onNTPSyncEvent ([](NTPSyncEvent_t event) {
    ntpEvent = event;
    syncEventTriggered = true;
  });

  e1 = WiFi.onStationModeGotIP (onSTAGotIP);// As soon WiFi is connected, start NTP Client
  e2 = WiFi.onStationModeDisconnected (onSTADisconnected);
  e3 = WiFi.onStationModeConnected (onSTAConnected);

  /////// Web Server /////////////////////

  server.serveStatic("/js", SPIFFS, "/js");
  server.serveStatic("/css", SPIFFS, "/css");
  server.serveStatic("/img", SPIFFS, "/img");
  
  server.on("/", handleRoot);
  server.on("/login", handleLogin);
  server.on("/inline", [](){
    server.send(200, "text/plain", "this works without need of authentification");
  });

  server.onNotFound(handleNotFound);
  //here the list of headers to be recorded
  const char * headerkeys[] = {"User-Agent","Cookie"} ;
  size_t headerkeyssize = sizeof(headerkeys)/sizeof(char*);
  //ask server to track these headers
  server.collectHeaders(headerkeys, headerkeyssize );


  server.on("/",[](){
   //
  });

  server.begin();
  Serial.println("HTTP server started");
}

//Test
//mhour = 14;
//mmin = 30;
//ehour = 17;
//emin = 0;

long lastValveOpened;
long lastTempRequest;

int analogResolution = 1024;
float prevResponsiveValue;
float responsiveValue;
bool responsiveValueHasChanged;
float activityThreshold = 4.0;
float smoothValue;
unsigned long lastActivityMS;
float errorEMA = 0.0;
boolean sleepEnable =  true;
boolean edgeSnapEnable = true;
bool sleeping = false;
char strt[40];  
void loop() {
  //RTC TIME
  //if (!RTC.IsDateTimeValid()) 
  //{
  //    Serial.println("RTC lost confidence in the DateTime!");
  //    currentTime = RtcDateTime(year(tempTime),month(tempTime),day(tempTime),hour(tempTime),minute(tempTime),second(tempTime)); //define date and time object
  //    RTC.SetDateTime(currentTime);
  //}

  //NTP TIME
  if (timeStatus() != timeNotSet) {
    if (now() != tempTime) { //update the display only if time has changed
      tempTime = now();
    }
  }
  
  RtcDateTime nowt = RTC.GetDateTime();
  


  if (millis() - lastTempRequest >= 2000)
  {
    digitalWrite(13, LOW);
    dstemp.requestTemperatures(); 
    stemp = dstemp.getTempCByIndex(0);

    pcell = 1023 - analogRead(A0);
    delay(100);
    moisture = 1023 - getResponsiveValue(adc.readADC(7));
  
    //sprintf(strt,"%02u-%02u-%04u %02u:%02u:%02u",day(),month(),year(),hour(),minute(),second());
    //Serial.print(strt);
    //Serial.println("|MEASURE_STAT : photocell =\'"+String(pcell)+"\' moisture = \'"+String(moisture)+"\' Temperature = \'"+String(stemp)+"\'");
    lastTempRequest = millis(); 
  } 



  if((mcp.digitalRead(8) == 0) || (pcell < 250)){
    manual = 1;
  }else{
    manual = 0;
  }

  mcp.digitalWrite(3,manual);  // manual override LED show
  
  if(manual == 1){
    userOn = mcp.digitalRead(9);
    if(userOn == 0 && prevUserOn == 1){
      if(ustate == 0){
        mcp.digitalWrite(7, 1);
        sprintf(strt,"%02u-%02u-%04u %02u:%02u:%02u",day(),month(),year(),hour(),minute(),second());
        Serial.print(strt);
        Serial.println("|EVENT_TRIGGER : Water valve opened by user");
        ustate = 1;
      }else{
        mcp.digitalWrite(7, 0);
        sprintf(strt,"%02u-%02u-%04u %02u:%02u:%02u",day(),month(),year(),hour(),minute(),second());
        Serial.print(strt);
        Serial.println("|EVENT_TRIGGER : Water valve closed by user");
        ustate = 0;
      }
      prevUserOn = 0;
    } else if(userOn == 1 && prevUserOn == 0){
      prevUserOn = 1;
    }

  }else if((manual == 0)){

    //VOD = 1;
    if((hour() == mhour) && (minute() == mmin) && (second() == 0)){
      timerOn = 1;
      lastValveOpened = nowt.TotalSeconds();
    }
  
    if((hour() == ehour) && (minute() == emin) && (second() == 0)){
      timerOn = 1;
      lastValveOpened = nowt.TotalSeconds();
    }

    //Serial.println(String(nowt.TotalSeconds() - lastValveOpened));
    
    if(nowt.TotalSeconds() - lastValveOpened >= VOD * 60){
      timerOn = 0;
    }

    if(timerOn == 1){
      mcp.digitalWrite(7, 1);
      if(prevTimerOn != timerOn) {
        Serial.println(String(nowt.Epoch32Time())+"| Water valve opened by timer");
      }
      prevTimerOn == timerOn;
    }else if(timerOn == 0){
      mcp.digitalWrite(7, 0);
      if(prevTimerOn != timerOn) {
        Serial.println(String(nowt.Epoch32Time())+"| Water valve closed by timer");
      }
      prevTimerOn == timerOn;
    }
    
  }
  
  server.handleClient();
}


void startWiFi() {
  //WiFiManager
  WiFiManager wifiManager;

  if (!wifiManager.autoConnect()) {
    
    Serial.println("failed to connect, we should reset as see if it connects");
    delay(3000);
    ESP.reset();
    delay(5000);
  }

  Serial.println("\r\n");
  Serial.print("Connected to ");
  Serial.println(WiFi.SSID());             // Tell us what network we're connected to
  Serial.print("IP address:\t");
  Serial.print(WiFi.localIP());            // Send the IP address of the ESP8266 to the computer
  Serial.println("\r\n");
}

///////// AUTENTICATION /////////////////////
//Check if header is present and correct
bool is_authentified(){
  Serial.println("Enter is authentified");
  if (server.hasHeader("Cookie")){
    Serial.print("Found cookie: ");
    String cookie = server.header("Cookie");
    Serial.println(cookie);
    if (cookie.indexOf("ESPSESSIONID=1") != -1) {
      Serial.println("Authentification Successful");
      return true;
    }
  }
  Serial.println("Authentification Failed");
  return false;
}

//login page, also called for disconnect
void handleLogin(){
  String msg;
  if (server.hasHeader("Cookie")){
    Serial.print("Found cookie: ");
    String cookie = server.header("Cookie");
    Serial.println(cookie);
  }
  if (server.hasArg("DISCONNECT")){
    Serial.println("Disconnection");
    server.sendHeader("Location","/login");
    server.sendHeader("Cache-Control","no-cache");
    server.sendHeader("Set-Cookie","ESPSESSIONID=0");
    server.send(301);
    return;
  }
  if (server.hasArg("USERNAME") && server.hasArg("PASSWORD")){
    if (server.arg("USERNAME") == admin_username &&  server.arg("PASSWORD") == admin_password ) // enter ur username and password you want
    {
      server.sendHeader("Location","/");
      server.sendHeader("Cache-Control","no-cache");
      server.sendHeader("Set-Cookie","ESPSESSIONID=1");
      server.send(301);
      Serial.println("Log in Successful");
      return;

      }

  msg = "Wrong username/password! try again.";
  Serial.println("Log in Failed");
  }

  //String content = "<!doctype html>\r\n<html>\r\n<head>\r\n";
  //content += "<meta charset=\"utf-8\">\r\n";
  //content += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1, shrink-to-fit=no\">\r\n";  
  //content += "<title>Smart water irrigation Login</title>\r\n";
  //content += "<link href=\"https://maxcdn.bootstrapcdn.com/bootstrap/4.0.0/css/bootstrap.min.css\" rel=\"stylesheet\">\r\n";
  //content += "\t<style>\r\n\tbody,html{height:100%}body{display:-ms-flexbox;display:-webkit-box;display:flex;-ms-flex-align:center;-ms-flex-pack:center;-webkit-box-align:center;align-items:center;-webkit-box-pack:center;justify-content:center;padding-top:40px;padding-bottom:40px;background-color:#f5f5f5}.form-signin{width:100%;max-width:330px;padding:15px;margin:0 auto}.form-signin .checkbox{font-weight:400}.form-signin .form-control{position:relative;box-sizing:border-box;height:auto;padding:10px;font-size:16px}.form-signin .form-control:focus{z-index:2}.form-signin input[type=text]{margin-bottom:-1px;border-bottom-right-radius:0;border-bottom-left-radius:0}.form-signin input[type=password]{margin-bottom:10px;border-top-left-radius:0;border-top-right-radius:0}\r\n\t</style>\r\n</head>\r\n";
  //content += "<body class=\"text-center\">\r\n\t<form action=\"/login\" method=\"POST\" class=\"form-signin\">\r\n\t\t<h1 class=\"h3 mb-3 font-weight-normal\">Please sign in</h1>\r\n\t\t<label for=\"inputUsername\" class=\"sr-only\">UserName</label>\r\n\t\t<input type=\"text\" id=\"USERNAME\" name=\"USERNAME\" class=\"form-control\" placeholder=\"user name\" required autofocus>\r\n\t\t<label for=\"inputPassword\" class=\"sr-only\">Password</label>\r\n\t\t<input type=\"password\" name=\"PASSWORD\" class=\"form-control\" placeholder=\"password\" required>\r\n\t\t<button class=\"btn btn-lg btn-primary btn-block\" type=\"submit\" name=\"SUBMIT\" value=\"Submit\">Sign in</button>\r\n\t</form>\r\n</body>\r\n</html>";
  //server.send(200, "text/html", content);

  server.serveStatic("/", SPIFFS, "/index.html");
}

//root page can be accessed only if authentification is ok
void handleRoot(){
  Serial.println("Enter handleRoot");
  String header;
  if (!is_authentified()){
    server.sendHeader("Location","/login");
    server.sendHeader("Cache-Control","no-cache");
    server.send(301);
    return;
  }
  server.serveStatic("/", SPIFFS, "/dashboard.html");
  
  //content += "You can access this page until you <a href=\"/login?DISCONNECT=YES\">disconnect</a></body></html>";

}

//no need authentification
void handleNotFound(){
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET)?"GET":"POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i=0; i<server.args(); i++){
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
}

///////////////////////////////


void adc_update(int rawValue)
{
  prevResponsiveValue = responsiveValue;
  responsiveValue = getResponsiveValue(rawValue);
  responsiveValueHasChanged = responsiveValue != prevResponsiveValue;
}

int getResponsiveValue(int newValue)
{
  // if sleep and edge snap are enabled and the new value is very close to an edge, drag it a little closer to the edges
  // This'll make it easier to pull the output values right to the extremes without sleeping,
  // and it'll make movements right near the edge appear larger, making it easier to wake up
  if(sleepEnable && edgeSnapEnable) {
    if(newValue < activityThreshold) {
      newValue = (newValue * 2) - activityThreshold;
    } else if(newValue > analogResolution - activityThreshold) {
      newValue = (newValue * 2) - analogResolution + activityThreshold;
    }
  }

  // get difference between new input value and current smooth value
  unsigned int diff = abs(newValue - smoothValue);

  // measure the difference between the new value and current value
  // and use another exponential moving average to work out what
  // the current margin of error is
  errorEMA += ((newValue - smoothValue) - errorEMA) * 0.4;

  // if sleep has been enabled, sleep when the amount of error is below the activity threshold
  if(sleepEnable) {
    // recalculate sleeping status
    sleeping = abs(errorEMA) < activityThreshold;
  }

  // if we're allowed to sleep, and we're sleeping
  // then don't update responsiveValue this loop
  // just output the existing responsiveValue
  if(sleepEnable && sleeping) {
    return (int)smoothValue;
  }

  // use a 'snap curve' function, where we pass in the diff (x) and get back a number from 0-1.
  // We want small values of x to result in an output close to zero, so when the smooth value is close to the input value
  // it'll smooth out noise aggressively by responding slowly to sudden changes.
  // We want a small increase in x to result in a much higher output value, so medium and large movements are snappy and responsive,
  // and aren't made sluggish by unnecessarily filtering out noise. A hyperbola (f(x) = 1/x) curve is used.
  // First x has an offset of 1 applied, so x = 0 now results in a value of 1 from the hyperbola function.
  // High values of x tend toward 0, but we want an output that begins at 0 and tends toward 1, so 1-y flips this up the right way.
  // Finally the result is multiplied by 2 and capped at a maximum of one, which means that at a certain point all larger movements are maximally snappy

  // then multiply the input by SNAP_MULTIPLER so input values fit the snap curve better.
  float snap = snapCurve(diff * snapMultiplier);

  // when sleep is enabled, the emphasis is stopping on a responsiveValue quickly, and it's less about easing into position.
  // If sleep is enabled, add a small amount to snap so it'll tend to snap into a more accurate position before sleeping starts.
  if(sleepEnable) {
    snap *= 0.5 + 0.5;
  }

  // calculate the exponential moving average based on the snap
  smoothValue += (newValue - smoothValue) * snap;

  // ensure output is in bounds
  if(smoothValue < 0.0) {
    smoothValue = 0.0;
  } else if(smoothValue > analogResolution - 1) {
    smoothValue = analogResolution - 1;
  }

  // expected output is an integer
  return (int)smoothValue;
}

float snapCurve(float x)
{
  float y = 1.0 / (x + 1.0);
  y = (1.0 - y) * 2.0;
  if(y > 1.0) {
    return 1.0;
  }
  return y;
}

void setSnapMultiplier(float newMultiplier)
{
  if(newMultiplier > 1.0) {
    newMultiplier = 1.0;
  }
  if(newMultiplier < 0.0) {
    newMultiplier = 0.0;
  }
  snapMultiplier = newMultiplier;
}
