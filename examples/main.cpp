#include "Arduino.h"
#include "WiFiMulti.h"
#include "DLNAClient.h"

DLNA_Client dlna;

WiFiMulti wifiMulti;
String ssid =     "xxxxx";
String password = "xxxxx";

bool f_seek   = false;
bool f_browse = false;

void setup(){
    Serial.begin(115200);
    Serial.print("\n\n");

    WiFi.mode(WIFI_STA);
    wifiMulti.addAP(ssid.c_str(), password.c_str());
    while (WiFi.status() != WL_CONNECTED){
        WiFi.disconnect(true);
        wifiMulti.run();
        vTaskDelay(800);
    }
    Serial.println("WiFi connected\n");
    f_seek = true;
}

void loop(){
    dlna.loop();

    if(f_seek){
        dlna.seekServer();
        f_seek = false;
    }

    if(f_browse){
        dlna.browseServer(0, "0"); // objectId "0" is root
        f_browse = false;
    }
}
// --------------------------------------------------------------------------------------------------
void getserver(){  // example: read server items
    DLNA_Client::dlnaServer_t srv = dlna.getServer();
    for(int i = 0; i< srv.size; i++){
        Serial.printf("[%i] %s\n", i, srv.controlURL[i]);
    }
    Serial.println();
}

void getBrowseContent(){ // example: read some content after browsing
    DLNA_Client::srvContent srvCt = dlna.getBrowseResult();
    for(int i = 0; i< srvCt.size; i++){
        Serial.printf("[%i] %s\n", i, srvCt.title[i]);
    }
    Serial.println();
}

//--------------EVENTS--------------------------------------------------------------------------------
void dlna_info(const char* info){
    Serial.printf("dlna_info: %s\n", info);
}

void dlna_server(uint8_t serverId, const char* IP_addr, uint16_t port, const char* friendlyName, const char* controlURL){
    Serial.printf("id %i, name: %s, IP %s:%i\n", serverId, friendlyName, IP_addr, port);
}

void dlna_seekReady(uint8_t numberOfServer){
    Serial.printf("%i media servers found\n\n", numberOfServer);
    f_browse = true;
    Serial.printf("%s\n\n", dlna.stringifyServer()); // and now stringify the found servers, make JSONstring:
}

void dlna_browseResult(const char* objectId, const char* parentId, uint16_t childCount, const char* title, bool isAudio, uint32_t itemSize, const char* duration, const char* itemURL){
    Serial.printf("objectId %s, parentId %s, childCount %i, title %s, isAudio %i, itemSize %lu, duration %s, itemURL %s\n", objectId, parentId, childCount, title, isAudio, (long unsigned int)itemSize, duration, itemURL);
}

void dlna_browseReady(uint16_t numbertReturned, uint16_t totalMatches){
    Serial.printf("returned %i from %i\n", numbertReturned, totalMatches);
    Serial.printf("\n%s\n\n", dlna.stringifyContent()); // and now stringify the browse result, make JSONstring:
    getserver();
    getBrowseContent();
}
