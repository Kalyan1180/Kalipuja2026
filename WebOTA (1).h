#pragma once
#include <ESP8266WebServer.h>

// Sets up the /update firmware upload endpoint and a friendly /ota landing
// page. If both otaUser and otaPass are non-empty, the /update page requires
// HTTP Basic Auth. Leave either blank to disable auth (not recommended once
// this leaves your bench).
void setupWebOTA(ESP8266WebServer &server, const char* otaUser = "", const char* otaPass = "");