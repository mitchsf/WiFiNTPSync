// WiFiNTPSync / Basic
// Minimal example: connect to WiFi, sync NTP, maintain both in loop().

#include <WiFi.h>
#include <WiFiUdp.h>
#include <NTP2.h>
#include <WiFiNTPSync.h>

String mySsid            = "YourSSID";
String myPassword        = "YourPassword";
String myNtpServer       = "time.cloudflare.com";
String myNtpServer2      = "time.google.com";

WiFiUDP wifiUDP;
NTP2    ntp(wifiUDP);

WiFiNTPConfig netCfg;
WiFiNTPHooks  netHooks;

void setup() {
  Serial.begin(115200);

  netCfg.ntpInstance        = &ntp;
  netCfg.ssid               = &mySsid;
  netCfg.password           = &myPassword;
  netCfg.primaryNtpServer   = &myNtpServer;
  netCfg.alternateNtpServer = &myNtpServer2;

  netHooks.onProgress = [](bool wifi, bool, uint32_t ms) {
    Serial.print(wifi ? "NTP..." : "WiFi...");
    Serial.println(ms);
  };
  netHooks.onSuccess = [](bool wifi, bool ntpOk) {
    if (wifi && ntpOk) {
      Serial.print("Synced; epoch=");
      Serial.println(ntp.epoch());
    }
  };

  bootWiFiAndNTP(netCfg, netHooks);
}

void loop() {
  serviceWiFiAndNTP(netCfg, netHooks);
  delay(10);
}
