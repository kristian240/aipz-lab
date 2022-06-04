#include <WiFi.h>
#include <Preferences.h>

int digitalGPIOPin = 8;
int photoGPIOPin = 20;

String header;

unsigned long currentTime = millis();
unsigned long previousTime = 0;
const long timeoutTime = 2000;

Preferences preferences;

WiFiServer server(80);

void setup() {
  // initialize serial communication at 115200 bits per second:
  Serial.begin(115200);

  preferences.begin("app-config");
      
  digitalGPIOPin = preferences.getShort("digital-pin", digitalGPIOPin);
  photoGPIOPin = preferences.getShort("analog-pin", photoGPIOPin);

  String defaultIp = "192.168.178.104";
  String ip = preferences.getString("ip", defaultIp);
  if (ip.length() == 0) {
    preferences.putString("ip", defaultIp);
    ip = defaultIp;
  }
  IPAddress local_IP;
  local_IP.fromString(ip);

  String defaultGw = "192.168.178.1";
  String gw = preferences.getString("gw", defaultGw);
  if (gw == defaultGw) preferences.putString("gw", defaultGw);
  IPAddress gateway;
  gateway.fromString(gw);

  String defaultSn = "255.255.255.0";
  String sn = preferences.getString("sn", defaultSn);
  if (sn == defaultSn) preferences.putString("sn", defaultSn);
  IPAddress subnet;
  subnet.fromString(sn);

  Serial.println("STA Config:");
  Serial.println(" - " + local_IP.toString());
  Serial.println(" - " + gateway.toString());
  Serial.println(" - " + subnet.toString());
  
  if (!WiFi.config(local_IP, gateway, subnet)) {
    Serial.println("STA Failed to configure");
  }

  char ssid_char[50], password_char[50];

  String defaultSsid = "FRITZ!Box 6660 Cable WY";
  String ssid = preferences.getString("ssid", defaultSsid);
  if (ssid == defaultSsid) preferences.putString("ssid", defaultSsid);
  ssid.toCharArray(ssid_char, 50);

  String defaultPassword = "39886939043466769499";
  String password = preferences.getString("wifi_password", defaultPassword);
  if (password == defaultPassword) preferences.putString("wifi_password", defaultPassword);
  password.toCharArray(password_char, 50);

  preferences.end();

  pinMode(digitalGPIOPin, INPUT);
  pinMode(photoGPIOPin, INPUT);

  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.begin(ssid_char, password_char);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }


  Serial.println("");
  Serial.println("WiFi connected.");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
  server.begin();
}

void loop() {
  if (Serial.available() > 0) {
    String command = Serial.readStringUntil(' ');
    if (command.equals("set")) {
      String param = Serial.readStringUntil(' ');
      char key[10];
      param.toCharArray(key, 10);
      String value = Serial.readStringUntil('\n');

      preferences.begin("app-config");
      preferences.putString(key, value);
      preferences.end();

      Serial.println("Set " + param + " to " + value);
    } else if (command.equals("get")) {
      String param = Serial.readStringUntil('\n');
      char key[10];
      param.toCharArray(key, 10);

      preferences.begin("app-config");
      String value = preferences.getString(key);
      preferences.end();

      Serial.println("Value of " + param + " is " + value);
    } else {
      Serial.println("Unkown command: " + command);
    }
  }

  WiFiClient client = server.available();   // Listen for incoming clients

  if (client) {                             // If a new client connects,
    currentTime = millis();
    previousTime = currentTime;
    Serial.println("New Client.");          // print a message out in the serial port

    int digitalValue = digitalRead(digitalGPIOPin);
    int analogValue = analogRead(photoGPIOPin);

    // print out the values you read:
    Serial.printf("digital value = %d\n", digitalValue);
    Serial.printf("ADC analog value = %d\n", analogValue);

    String currentLine = "";                // make a String to hold incoming data from the client
    while (client.connected() && currentTime - previousTime <= timeoutTime) {  // loop while the client's connected
      currentTime = millis();
      if (client.available()) {             // if there's bytes to read from the client,
        char c = client.read();             // read a byte, then
        Serial.write(c);                    // print it out the serial monitor
        header += c;
        if (c == '\n') {                    // if the byte is a newline character
          // if the current line is blank, you got two newline characters in a row.
          // that's the end of the client HTTP request, so send a response:
          if (currentLine.length() == 0) {
            // HTTP headers always start with a response code (e.g. HTTP/1.1 200 OK)
            // and a content-type so the client knows what's coming, then a blank line:
            client.println("HTTP/1.1 200 OK");
            client.println("Content-type:application/json");
            client.println("Connection: close");
            client.println();


            // Web Page Heading
            client.println("{\"light\":" + String(analogValue) + ", \"digital\": " + String(digitalValue) + "}");

            // The HTTP response ends with another blank line
            client.println();
            // Break out of the while loop
            break;
          } else { // if you got a newline, then clear currentLine
            currentLine = "";
          }
        } else if (c != '\r') {  // if you got anything else but a carriage return character,
          currentLine += c;      // add it to the end of the currentLine
        }
      }
    }
    // Clear the header variable
    header = "";
    // Close the connection
    client.stop();
    Serial.println("Client disconnected.");
    Serial.println("");
  }
}
