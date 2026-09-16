#include "WebOTA.h"
#include <ESP8266HTTPUpdateServer.h>
#include <string.h>

static ESP8266HTTPUpdateServer httpUpdater;
static ESP8266WebServer *webServer = nullptr;

void setupWebOTA(ESP8266WebServer &server, const char* otaUser, const char* otaPass) {
  webServer = &server;

  bool authEnabled = (otaUser && otaPass && strlen(otaUser) > 0 && strlen(otaPass) > 0);

  // Firmware upload page: http://192.168.4.1/update
  if (authEnabled) {
    httpUpdater.setup(&server, "/update", otaUser, otaPass);
  } else {
    httpUpdater.setup(&server, "/update");
  }

  // Friendly OTA landing page: http://192.168.4.1/ota
  server.on("/ota", HTTP_GET, [authEnabled]() {
    String html;
    html.reserve(700);
    html += F("<!DOCTYPE html><html><head>");
    html += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
    html += F("<title>Garur Sensor OTA</title>");
    html += F("<style>body{font-family:Arial;background:#111;color:#eee;padding:18px}");
    html += F(".card{background:#1f1f1f;padding:16px;border-radius:12px;max-width:520px}");
    html += F("a.btn{display:inline-block;background:#ffb300;color:#111;padding:12px 16px;");
    html += F("border-radius:8px;text-decoration:none;font-weight:bold;margin:6px 0}</style>");
    html += F("</head><body><div class='card'><h2>Garur Sensor OTA Update</h2>");
    html += F("<p>Upload compiled .bin firmware from Arduino IDE.</p>");
    if (authEnabled) {
      html += F("<p><small>The upload page will prompt for the OTA username/password.</small></p>");
    }
    html += F("<p><a class='btn' href='/update'>Open Firmware Upload</a></p>");
    html += F("<p><a class='btn' href='/'>Back to Dashboard</a></p>");
    html += F("</div></body></html>");
    webServer->send(200, "text/html", html);
  });
}