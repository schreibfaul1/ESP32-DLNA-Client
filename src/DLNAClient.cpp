#include "DLNAClient.h"

// Created on: 30.11.2023
// Updated on: 01.08.2025
/*
//example
DLNA dlna;

void setup(){
    dlna.seekServer(); // send a multicast around the local network
}

void loop(){
    dlna.loop();
}

*/

DLNA_Client::DLNA_Client(){
    m_state = IDLE;
    m_chunked = false;
    m_PSRAMfound = psramInit();
    m_chbuf = (char*)malloc(512);
    m_chbufSize = 512;
}

DLNA_Client::~DLNA_Client(){
    dlnaServer_clear_and_shrink();
    srvContent_clear_and_shrink();
    vector_clear_and_shrink(m_content);
    if(m_chbuf){free(m_chbuf); m_chbuf = NULL;}
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool DLNA_Client::seekServer(){
    if(WiFi.status() != WL_CONNECTED) return false; // guard

    if(m_chbuf) {free(m_chbuf); m_chbuf = NULL;}
    if(m_PSRAMfound == false) {
        m_chbuf = (char*)malloc(512);
        m_chbufSize = 512;
    }
    else {
        m_chbuf = (char*)ps_malloc(4 * 4096);
        m_chbufSize = 4 *4096;
    }

    dlnaServer_clear_and_shrink();
    m_dlnaServer.size = 0;
    uint8_t ret = 0;
    const char searchTX[] = "M-SEARCH * HTTP/1.1\r\n"\
                            "HOST: 239.255.255.250:1900\r\n"\
                            "MAN: \"ssdp:discover\"\r\n"\
                            "MX: 3\r\n"\
                            "ST: urn:schemas-upnp-org:device:MediaServer:1\r\n\r\n";

    {
            ret = m_udp.beginMulticast(IPAddress(SSDP_MULTICAST_IP), SSDP_LOCAL_PORT);
            if(!ret){
                m_udp.stop(); log_e("error sending SSDP multicast packets");
                return false;
            }
            for (int i = 0; i < 3; i++) {
                ret = m_udp.beginPacket(IPAddress(SSDP_MULTICAST_IP), SSDP_MULTICAST_PORT);
                if(!ret){
                    log_e("udp beginPacket error");
                    return false;
                }
                ret = m_udp.write((const uint8_t*)searchTX, strlen(searchTX));
                if(!ret){
                    log_e("udp write error");
                    return false;
                }
                ret = m_udp.endPacket();
                if(!ret){
                    log_e("endPacket error");
                    return false;
                }
                vTaskDelay(100 / portTICK_PERIOD_MS);
            }
        }
    m_state = SEEK_SERVER;
    m_timeStamp = millis();
    return true;
}

//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
int8_t DLNA_Client::listServer(){
    if(m_state == SEEK_SERVER) return -1; // seek in progress
    for(uint8_t i = 0; i < m_dlnaServer.size; i++){
        if(dlna_server) dlna_server(i, m_dlnaServer.ip[i], m_dlnaServer.port[i], m_dlnaServer.friendlyName[i], m_dlnaServer.controlURL[i]);
    }
    return m_dlnaServer.size;
}

DLNA_Client::dlnaServer_t DLNA_Client::getServer(){
    return m_dlnaServer;
}

DLNA_Client::srvContent_t DLNA_Client::getBrowseResult(){
    return m_srvContent;;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void DLNA_Client::parseDlnaServer(uint16_t len){
    if(len > m_chbufSize - 1) len = m_chbufSize - 1; // guard
    char* dummy = strdup("?");
    memset(m_chbuf, 0, m_chbufSize);
    vTaskDelay(200);
    m_udp.read(m_chbuf, len); // read packet into the buffer
    char* p = strcasestr(m_chbuf, "Location: http");
    if(!p) return;
    int idx1 = indexOf(p, "://",  0) + 3;  // pos IP
    int idx2 = indexOf(p, ":",  idx1);      // pos ':'
    int idx3 = indexOf(p, "/",  idx2);      // pos '/'
    int idx4 = indexOf(p, "\r", idx3);      // pos '\r'
    *(p + idx2) = '\0';
    *(p + idx3) = '\0';
    *(p + idx4) = '\0';
    for(int i = 0; i< m_dlnaServer.size; i++){
        if(strcmp(m_dlnaServer.ip[i], p + idx1) == 0){      // same IP
            if(m_dlnaServer.port[i] == atoi(p + idx2 + 1)){ // same port
                return;
            }
        }
    }
    if(strcmp(p + idx1, "0.0.0.0") == 0) {log_e("invalid IP address found %s", p + idx1); return;}
    m_dlnaServer.ip.push_back(x_ps_strdup(p + idx1));
    m_dlnaServer.port.push_back(atoi(p + idx2 + 1));
    m_dlnaServer.location.push_back(x_ps_strdup(p + idx3 + 1));
    m_dlnaServer.controlURL.push_back(dummy);
    m_dlnaServer.friendlyName.push_back(dummy);
    m_dlnaServer.presentationPort.push_back(0);
    m_dlnaServer.presentationURL.push_back(dummy);
    m_dlnaServer.size++;
}

//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool DLNA_Client::srvGet(uint8_t srvNr){
    bool ret = false;
    m_client.stop();
    m_client.setTimeout(CONNECT_TIMEOUT);
    uint32_t t = millis();
    ret = m_client.connect(m_dlnaServer.ip[srvNr], m_dlnaServer.port[srvNr]);
    if(!ret){
        m_client.stop();
        sprintf(m_chbuf, "The server %s:%d did not answer within %lums [%s:%d]", m_dlnaServer.ip[srvNr], m_dlnaServer.port[srvNr], millis() - t, __FILENAME__, __LINE__);
        if(dlna_info) dlna_info(m_chbuf);
        return false;
    }
    t = millis() + 250;
    while(true){
        if(m_client.connected()) break;
        if(t < millis()){
            sprintf(m_chbuf, "The server %s:%d refuses the connection [%s:%d]", m_dlnaServer.ip[srvNr], m_dlnaServer.port[srvNr], __FILENAME__, __LINE__);
            if(dlna_info) dlna_info(m_chbuf);
            return false;
        }
    }
    // assemble HTTP header
    sprintf(m_chbuf, "GET /%s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\nUser-Agent: ESP32/Player/UPNP1.0\r\n\r\n",
                      m_dlnaServer.location[srvNr], m_dlnaServer.ip[srvNr], m_dlnaServer.port[srvNr]);
    m_client.print(m_chbuf);
    t = millis() + AVAIL_TIMEOUT;
    while(true){
        if(m_client.available()) break;
        if(t < millis()){
            sprintf(m_chbuf, "The server %s:%d is not responding after request [%s:%d]", m_dlnaServer.ip[srvNr], m_dlnaServer.port[srvNr], __FILENAME__, __LINE__);
            if(dlna_info) dlna_info(m_chbuf);
            return false;
        }
    }
    return true;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool DLNA_Client::readHttpHeader(){

    bool ct_seen = false;
    m_timeStamp  = millis();
    uint16_t rhlSize = 512;
    char* rhl = x_ps_malloc(rhlSize); // response header line
    while(true){  // outer while
        uint16_t pos = 0;
        if((m_timeStamp + READ_TIMEOUT) < millis()) {
            sprintf(m_chbuf, "timeout in readHttpHeader [%s:%d]", __FILENAME__, __LINE__);
            if(dlna_info) dlna_info(m_chbuf);
            goto error;
        }
        while(m_client.available()) {
            uint8_t b = m_client.read();
            if(b == '\n') {
                if(!pos) {  // empty line received, is the last line of this responseHeader
                    goto exit;
                }
                break;
            }
            if(b == '\r') rhl[pos] = 0;
            if(b < 0x20) continue;
            rhl[pos] = b;
            pos++;
            if(pos == rhlSize -1) {
                pos--;
                continue;
            }
            if(pos == rhlSize - 2) {
                rhl[pos] = '\0';
                sprintf(m_chbuf, "responseHeaderline overflow, response was: %s", rhl);
                if(dlna_info) dlna_info(m_chbuf);
            }
        } // inner while
    //    log_w("%s", rhl);
        int16_t posColon = indexOf(rhl, ":", 0);  // lowercase all letters up to the colon
        if(posColon >= 0) {
            for(int i = 0; i < posColon; i++) { rhl[i] = toLowerCase(rhl[i]); }
        }
        if(startsWith(rhl, "content-length:")){
            const char* c_cl = (rhl + 15);
            int32_t     i_cl = atoi(c_cl);
            m_contentlength = i_cl;
        //    log_i("content-length: %lu", (long unsigned int)m_contentlength);
        }
        else if(startsWith(rhl, "content-type:")) {  // content-type: text/html; charset=UTF-8
            int idx = indexOf(rhl + 13, ";", 0);
            if(idx > 0) rhl[13 + idx] = '\0';
            if(indexOf(rhl + 13, "text/xml", 0) > 0) ct_seen = true;
            else if(indexOf(rhl + 13, "text/html", 0) > 0) ct_seen = true;
            else{
                sprintf(m_chbuf, "content type expected: text/xml or text/html, got %s", rhl + 13);
                if(dlna_info) dlna_info(m_chbuf);
                goto exit; // wrong content type
            }
        }
        else if((startsWith(rhl, "transfer-encoding:"))) {
            if(endsWith(rhl, "chunked") || endsWith(rhl, "Chunked")) {  // Station provides chunked transfer
                m_chunked = true;
            }
        }
        else { ; }
    //    log_w("%s", rhl);
    } // outer while

exit:
    if(rhl) {free(rhl); rhl = NULL;}
    if(!m_contentlength) log_e("contentlength is not given");
    if(!ct_seen) log_e("content type not found");
    return true;

error:
    if(rhl) {free(rhl); rhl = NULL;}
    return false;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool DLNA_Client::readContent(){

    m_timeStamp  = millis();
    uint32_t idx = 0;
    uint8_t lastChar = 0;
    uint8_t b = 0;
    bool f_overflow = false;
    vector_clear_and_shrink(m_content);

    while(true){  // outer while
        uint16_t pos = 0;
        if((m_timeStamp + READ_TIMEOUT) < millis()) {
            sprintf(m_chbuf, "timeout in readContent [%s:%d]", __FILENAME__, __LINE__);
            if(dlna_info) dlna_info(m_chbuf);
            goto error;
        }
        while(m_client.available()) {
            if(lastChar){
                b = lastChar;
                lastChar = 0;
            }
            else{
                b = m_client.read();
                idx++;
            }
            if(b == '\n') {
                m_chbuf[pos] = '\0';
                break;
            }
            if(b == '<' &&  m_chbuf[pos - 1] == '>'){
                lastChar = '<';
                m_chbuf[pos] = '\0';
                break; // simulate new line
            }
            if(b == ';'){
                m_chbuf[pos] = '\0';
                break; // simulate new line
            }
            if(b == '\r') m_chbuf[pos] = '\0';
            if(b < 0x20) continue;
            m_chbuf[pos] = b;
            pos++;
            if(pos >= m_chbufSize -1) {
                m_chbuf[pos] = '\0';
                f_overflow = true;
                pos--;
                continue;
            }
        }
        if(f_overflow)log_e("line overflow");

//        log_w("%s %i", m_chbuf, m_content.size());
        m_content.push_back(x_ps_strdup(m_chbuf));
        if(!m_chunked &&  idx == m_contentlength) break;
        if(!m_client.available()){
            vTaskDelay(10);
               if(m_chunked == true) break; // ok
        //    if(m_contentlength)   break; // ok
        //    goto error; // not ok
        }
        m_timeStamp  = millis();
    }
    return true;

error:
    return false;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool DLNA_Client::getServerItems(uint8_t srvNr){
    if(m_dlnaServer.size == 0) return 0;  // return if none detected


    bool gotFriendlyName = false;
    bool gotServiceType  = false;
    bool URNschemaFound  = false;

    for(int i = 0; i < m_content.size(); i++){
        uint16_t idx = 0;
        while(*(m_content[i] + idx) == 0x20) idx++;  // same as trim left
        char* content = m_content[i] + idx;
        if(!gotFriendlyName) {
            if(startsWith(content, "<friendlyName>")){
                uint16_t pos = indexOf(content, "<", 14);
                *(content + pos) = '\0';
                if(strlen(content) == 0){
                    m_dlnaServer.friendlyName[srvNr] = (char*)"Server name not provided";
                }
                else{
                    m_dlnaServer.friendlyName[srvNr] = x_ps_strdup(content + 14);
                }
                gotFriendlyName = true;
            }
        }
        if(!gotServiceType) {
            if(indexOf(content, "urn:schemas-upnp-org:service:ContentDirectory:1", 0) > 0){URNschemaFound = true; continue;}
            if(URNschemaFound){
                if(startsWith(content, "<controlURL>")){
                    uint16_t pos = indexOf(content, "<", 12);
                    *(content + pos) = '\0';
                    m_dlnaServer.controlURL[srvNr] = x_ps_strdup(content + 13);
                    gotServiceType = true;
                }
            }
        }
        if(startsWith(content, "<presentationURL>")){
            uint16_t pos = indexOf(content, "<", 17);
            *(content + pos) = '\0';
            char* presentationURL = x_ps_strdup(content + 17);
            if(!startsWith(presentationURL, "http://")) continue;
            int8_t posColon = (indexOf(presentationURL, ":", 8));
            if(posColon > 0){ // we have ip and port
                presentationURL[posColon] = '\0';
                m_dlnaServer.presentationURL[srvNr] = x_ps_strdup(presentationURL + 7); // add presentationURL(IP)
                m_dlnaServer.presentationPort[srvNr] = atoi(presentationURL + posColon + 1);
            } // only ip is given
            else{
                m_dlnaServer.presentationURL[srvNr] = x_ps_strdup(presentationURL + 7);
            }
            if(presentationURL){free(presentationURL); presentationURL = NULL;}
        }
    }

    // we finally got all infos we need
    uint16_t idx = 0;
    if(m_dlnaServer.location[srvNr] && endsWith(m_dlnaServer.location[srvNr], "/")){
        char* tmp = (char*)malloc(strlen(m_dlnaServer.location[srvNr]) + strlen(m_dlnaServer.controlURL[srvNr]) + 1);
        strcpy(tmp, m_dlnaServer.location[srvNr]); // location string becomes first part of controlURL
        strcat(tmp, m_dlnaServer.controlURL[srvNr]);
        free(m_dlnaServer.controlURL[srvNr]);
        m_dlnaServer.controlURL[srvNr] = x_ps_strdup(tmp);
        free(tmp);
    }
    if(m_dlnaServer.controlURL[srvNr] && startsWith(m_dlnaServer.controlURL[srvNr], "http://")) { // remove "http://ip:port/" from begin of string
        idx = indexOf(m_dlnaServer.controlURL[srvNr], "/", 7);
        memcpy(m_dlnaServer.controlURL[srvNr], m_dlnaServer.controlURL[srvNr] + idx + 1, strlen(m_dlnaServer.controlURL[srvNr]) + idx + 2);
    }
    if(strcmp(m_dlnaServer.friendlyName[srvNr], "?") == 0){log_e("friendlyName %s, [%i]", m_dlnaServer.friendlyName[srvNr], srvNr); return false;}
    if(strcmp(m_dlnaServer.controlURL[srvNr], "?") == 0){log_e("controlURL %s, [%i]", m_dlnaServer.controlURL[srvNr], srvNr); return false;}
    if(dlna_server) dlna_server(srvNr, m_dlnaServer.ip[srvNr], m_dlnaServer.port[srvNr], m_dlnaServer.friendlyName[srvNr],m_dlnaServer.controlURL[srvNr]);
    return true;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool DLNA_Client::browseResult(){

    auto makeContentPushBack = [&](){ // lambda, inner function
        char* dummy1 = strdup("?");
        char* dummy2 = strdup("?");
        char* dummy3 = strdup("?");
        char* dummy4 = strdup("?");
        char* dummy5 = strdup("?");
        DLNA_Client::m_srvContent.childCount.push_back(0);
        DLNA_Client::m_srvContent.isAudio.push_back(0);
        DLNA_Client::m_srvContent.itemSize.push_back(0);
        DLNA_Client::m_srvContent.itemURL.push_back(dummy1);
        DLNA_Client::m_srvContent.duration.push_back(dummy2);
        DLNA_Client::m_srvContent.objectId.push_back(dummy3);
        DLNA_Client::m_srvContent.parentId.push_back(dummy4);
        DLNA_Client::m_srvContent.title.push_back(dummy5);
        DLNA_Client::m_srvContent.size++;
    };

    m_numberReturned = 0;
    m_totalMatches = 0;
    bool item1 = false;
    bool item2 = false;
    int a, b, c, d;
    srvContent_clear_and_shrink();
    for(int i = 0; i < m_content.size(); i++){
        uint16_t idx = 0;
        while(*(m_content[i] + idx) == 0x20) idx++;  // same as trim left
        char* content = m_content[i] + idx;
        // log_i("%s", content);
        /*------C O N T A I N E R -------*/
        if(startsWith(content, "container id")) {item1 = true; memset(m_chbuf, 0, m_chbufSize);}
        if(item1){
            strcat(m_chbuf, content);
        }
        if(startsWith(content, "/container")) {
        //    log_i("%s", m_chbuf);
            item1 = false;
            uint16_t cNr = m_srvContent.size;
            makeContentPushBack();
            replacestr(m_chbuf, "&quot", "\"");
            replacestr(m_chbuf, "&ampamp", "&");   // ampersand
            replacestr(m_chbuf, "&ampapos", "'");  // apostrophe
            replacestr(m_chbuf, "&ampquot", "\""); // quotation

            a = indexOf(m_chbuf, "container id=", 0);
            if(a >= 0) {
                a += 14;
                b = indexOf(m_chbuf, "\"", a);
                m_srvContent.objectId[cNr] = x_ps_strndup(m_chbuf + a, b - a);
            }

            a = indexOf(m_chbuf, "parentID=", 0);
            if(a >= 0) {
                a += 10;
                b = indexOf(m_chbuf, "\"", a);
                m_srvContent.parentId[cNr] = x_ps_strndup(m_chbuf + a, b - a);
            }

            a = indexOf(m_chbuf, "childCount=", 0);
            if(a >= 0) {
                a += 12;
                b = indexOf(m_chbuf, "\"", a);
                char tmp[10] = {0}; memcpy(tmp, m_chbuf + a, b - a);
                m_srvContent.childCount[cNr] = atoi(tmp);
            }
            a = indexOf(m_chbuf, "dc:title", 0);
            if(a >= 0) {
                a += 11;
                b = indexOf(m_chbuf, "/dc:title", a);
                b -= 3;
                m_srvContent.title[cNr] = x_ps_strndup(m_chbuf + a, b - a);
                if(strlen(m_srvContent.title[cNr]) == 0) m_srvContent.title[cNr] = x_ps_strndup("Unknown\0", 8);
            }

            if(dlna_browseResult) dlna_browseResult(m_srvContent.objectId[cNr],
                                                    m_srvContent.parentId[cNr],
                                                    m_srvContent.childCount[cNr],
                                                    m_srvContent.title[cNr],
                                                    m_srvContent.isAudio[cNr],
                                                    m_srvContent.itemSize[cNr],
                                                    m_srvContent.duration[cNr],
                                                    m_srvContent.itemURL[cNr]);

        }
        /*------ I T E M -------*/
        if(startsWith(content, "item id")) {item2 = true; memset(m_chbuf, 0, m_chbufSize);}
        if(item2){
            strcat(m_chbuf, content);
        }
        if(startsWith(content, "/item")) {
            item2 = false;
            uint16_t cNr = m_srvContent.size;
            makeContentPushBack();

            replacestr(m_chbuf, "&quot", "\"");
            replacestr(m_chbuf, "&ampamp", "&");   // ampersand
            replacestr(m_chbuf, "&ampapos", "'");  // apostrophe
            replacestr(m_chbuf, "&ampquot", "\""); // quotation
            replacestr(m_chbuf, "&lt", "<");
            replacestr(m_chbuf, "&gt", ">");

            a = indexOf(m_chbuf, "item id=", 0);
            if(a >= 0) {
                a += 9;
                b = indexOf(m_chbuf, "\"", a);
                if(m_srvContent.objectId[cNr]) { free(m_srvContent.objectId[cNr]); m_srvContent.objectId[cNr] = NULL;}
                m_srvContent.objectId[cNr] = x_ps_strndup(m_chbuf + a, b - a);
            }

            a = indexOf(m_chbuf, "parentID=", 0);
            if(a >= 0){
                a += 10;
                b = indexOf(m_chbuf, "\"", a);
                if(m_srvContent.parentId[cNr]) { free(m_srvContent.parentId[cNr]); m_srvContent.parentId[cNr] = NULL;}
                m_srvContent.parentId[cNr] = x_ps_strndup(m_chbuf + a, b - a);
            }

            a = indexOf(m_chbuf, "object.item.audioItem", 0);
            if(a < 0) {m_srvContent.isAudio[cNr] = 0;}
            else      {m_srvContent.isAudio[cNr] = 1;}

            a = indexOf(m_chbuf, "dc:title", 0);
            if(a >= 0){
                a += 9;
                b = indexOf(m_chbuf, "/dc:title", a);
                b -= 1;
                if(m_srvContent.title[cNr]) { free(m_srvContent.title[cNr]); m_srvContent.title[cNr] = NULL;}
                m_srvContent.title[cNr] = x_ps_strndup(m_chbuf + a, b - a);
            }

            a = indexOf(m_chbuf, "<res", 0);
            b = indexOf(m_chbuf, "/res>", a);
            if(a > 0){
                if(b > a) m_chbuf[b] = '\0';

                c = indexOf(m_chbuf, ">http", a);
                if(c >= 0){
                    c += 1;
                    d = indexOf(m_chbuf, "<", c);
                    if(m_srvContent.itemURL[cNr]) { free(m_srvContent.itemURL[cNr]); m_srvContent.itemURL[cNr] = NULL;}
                    m_srvContent.itemURL[cNr] = x_ps_strndup(m_chbuf + c, d - c);
                }

                c = indexOf(m_chbuf, "duration=", a);
                if(c >= 0){
                    c += 10;
                    d = indexOf(m_chbuf, "\"", c) - 4;
                    if(d > c){
                        if(m_srvContent.duration[cNr]) { free(m_srvContent.duration[cNr]); m_srvContent.duration[cNr] = NULL;}
                        m_srvContent.duration[cNr] = x_ps_strndup(m_chbuf + c, d - c);
                    }
                }

                a = indexOf(m_chbuf, "size=", a);
                if(a > 0){
                    a += 6;
                    b = indexOf(m_chbuf, "\"", a);
                    char tmp[60] = {0}; memcpy(tmp, m_chbuf + a, b - a);
                    m_srvContent.itemSize[cNr] = atol(tmp);
                }
            }

            if(dlna_browseResult) dlna_browseResult(m_srvContent.objectId[cNr],
                                                    m_srvContent.parentId[cNr],
                                                    m_srvContent.childCount[cNr],
                                                    m_srvContent.title[cNr],
                                                    m_srvContent.isAudio[cNr],
                                                    m_srvContent.itemSize[cNr],
                                                    m_srvContent.duration[cNr],
                                                    m_srvContent.itemURL[cNr]);
        }

        if(startsWith(content, "<NumberReturned>")){;
            b= indexOf(content, "</NumberReturned>", 16);
            char tmp[10] = {0}; memcpy(tmp, content + 16, b - 16);
            m_numberReturned = atoi(tmp);
        }

        if(startsWith(content, "<TotalMatches>")){
            b= indexOf(content, "</TotalMatches>", 14);
            char tmp[10] = {0}; memcpy(tmp, content + 14, b - 14);
            m_totalMatches = atoi(tmp);
        }
    }
    if(dlna_browseReady) dlna_browseReady(m_numberReturned, m_totalMatches);
    return true;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool DLNA_Client::srvPost(uint8_t srvNr, const char* objectId, const uint16_t startingIndex, const uint16_t maxCount){

    bool ret;
    uint8_t cnt = 0;

    m_client.stop();
    uint32_t t = millis();
    m_client.setTimeout(CONNECT_TIMEOUT);
    ret = m_client.connect(m_dlnaServer.ip[srvNr], m_dlnaServer.port[srvNr]);

    if(!ret){
        m_client.stop();
        sprintf(m_chbuf, "The server %s:%d is not responding after %lums [%s:%d]", m_dlnaServer.ip[srvNr], m_dlnaServer.port[srvNr], millis() - t, __FILENAME__, __LINE__);
        if(dlna_info) dlna_info(m_chbuf);
        return false;
    }
    while(true){
        if(m_client.connected()) break;
        delay(100);
        cnt++;
        if(cnt == 10){
            sprintf(m_chbuf, "The server %s:%d refuses the connection [%s:%d]", m_dlnaServer.ip[srvNr], m_dlnaServer.port[srvNr], __FILENAME__, __LINE__);
            if(dlna_info) dlna_info(m_chbuf);
            return false;
        }
    }

    sprintf(m_chbuf, "POST /%s HTTP/1.1\r\n"                                                                                                         \
                     "Host: %s:%d\r\n"                                                                                                               \
                     "CACHE-CONTROL: no-cache\r\nPRAGMA: no-cache\r\n"                                                                               \
                     "Connection: close\r\n"                                                                                                         \
                     "Content-Length: 000\r\n"                         /* dummy length, determine later*/                                            \
                     "Content-Type: text/xml; charset=\"utf-8\"\r\n"                                                                                 \
                     "SOAPAction: \"urn:schemas-upnp-org:service:ContentDirectory:1#Browse\"\r\n"                                                    \
                     "User-Agent: ESP32/Player/UPNP1.0\r\n"                                                                                          \
                     "\r\n"                                            /*end header, begin message */                                                \
                     "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">\r\n"\
                     "<s:Body>"                                                                                                                      \
                     "<u:Browse xmlns:u=\"urn:schemas-upnp-org:service:ContentDirectory:1\">\r\n"                                                    \
                     "<ObjectID>%s</ObjectID>\r\n"                                                                                                   \
                     "<BrowseFlag>BrowseDirectChildren</BrowseFlag>\r\n"                                                                             \
                     "<Filter>*</Filter>\r\n"                                                                                                        \
                     "<StartingIndex>%i</StartingIndex>\r\n"           /* startingIndex */                                                           \
                     "<RequestedCount>%i</RequestedCount>\r\n"         /* max count*/                                                                \
                     "<SortCriteria></SortCriteria>\r\n"                                                                                             \
                     "</u:Browse>\r\n"                                                                                                               \
                     "</s:Body>\r\n"                                                                                                                 \
                     "</s:Envelope>\r\n\r\n"
                     , m_dlnaServer.controlURL[srvNr], m_dlnaServer.ip[srvNr], m_dlnaServer.port[srvNr], objectId, startingIndex, maxCount);

    uint16_t msgBegin = indexOf(m_chbuf, "\r\n\r\n", 0);
    uint16_t msgLength = strlen(m_chbuf) - (msgBegin + 4);
    uint16_t insertIdx = indexOf(m_chbuf, "Content-Length:", 0) + 16;
    char tmp[10]; itoa(msgLength, tmp, 10);
    memcpy(m_chbuf + insertIdx, tmp, 3);
    m_chbuf[strlen(m_chbuf)+ 1] = '\0';

    m_client.print(m_chbuf);

    t = millis() + AVAIL_TIMEOUT;
    while(true){
        if(m_client.available()) break;
        if(t < millis()){
            sprintf(m_chbuf, "The server %s:%d is not responding after request [%s:%d]", m_dlnaServer.ip[srvNr], m_dlnaServer.port[srvNr], __FILENAME__, __LINE__);
            if(dlna_info) dlna_info(m_chbuf);
            return false;
        }
    }
    return true;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
int8_t DLNA_Client::browseServer(uint8_t srvNr, const char* objectId, const uint16_t startingIndex, const uint16_t maxCount){
    if(!objectId) {log_e("objectId is NULL"); return -1;} // no objectId given
    if(srvNr >= m_dlnaServer.size) {log_e("server index too high"); return -2;} // srvNr too high
    if(m_state != IDLE) {log_e("state is not idle"); return -3;}

    m_srvNr = srvNr;
    strcpy(m_objectId, objectId);
    m_startingIndex = startingIndex;
    m_maxCount = maxCount;
    m_state = BROWSE_SERVER;
    return 0;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
const char* DLNA_Client::stringifyServer() {
    if(m_dlnaServer.size == 0) return "[]"; // guard

    uint16_t JSONstrLength = 0;
    if(m_JSONstr){free(m_JSONstr); m_JSONstr = NULL;}
    if(m_dlnaServer.size == 0) return "[]"; // no content found
    if(m_PSRAMfound) { m_JSONstr = (char*)ps_malloc(2);}
    else             { m_JSONstr = (char*)malloc(2);}
    JSONstrLength += 2;
    memcpy(m_JSONstr, "[\0", 2);

    char id[5]; char port[6];

    for(int i = 0; i < m_dlnaServer.size; i++) { // build a JSON string in PSRAM, e.g. [{"name":"m","dir":true},{"name":"s","dir":false}]
        itoa(i, id, 10);
        itoa(m_dlnaServer.port[i], port, 10);
        JSONstrLength = strlen(id) + strlen(port) + strlen(m_dlnaServer.friendlyName[i])+ strlen(m_dlnaServer.ip[i]);
    //  [{"srvId":"1","friendlyName":"minidlna","ip":"192.168.178.1","port":"49000""}]
    //  {"srvId":"","friendlyName":"","ip":"","port":""},   --> 49 chars
        JSONstrLength += strlen(m_JSONstr) + 49 + 2;

        if(m_PSRAMfound) { m_JSONstr = (char*)ps_realloc(m_JSONstr, JSONstrLength); }
        else             { m_JSONstr = (char*)realloc(m_JSONstr, JSONstrLength); }

        strcat(m_JSONstr, "{\"srvId\":\""); strcat(m_JSONstr, id);
        strcat(m_JSONstr, "\",\"friendlyName\":\""); strcat(m_JSONstr, m_dlnaServer.friendlyName[i]);
        strcat(m_JSONstr, "\",\"ip\":\""); strcat(m_JSONstr, m_dlnaServer.ip[i]);
        strcat(m_JSONstr, "\",\"port\":\""); strcat(m_JSONstr, port);
        strcat(m_JSONstr, "\"},");
    }
    m_JSONstr[JSONstrLength - 3] = ']'; // replace comma by square bracket close
    m_JSONstr[JSONstrLength - 2] = '\0'; // and terminate
    return m_JSONstr;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
const char* DLNA_Client::stringifyContent() {
    if(m_srvContent.size == 0) return "[]"; // guard

    uint16_t JSONstrLength = 0;
    if(m_JSONstr){free(m_JSONstr); m_JSONstr = NULL;}
    if(m_srvContent.size == 0) return "[]"; // no content found
    if(m_PSRAMfound) { m_JSONstr = (char*)ps_malloc(2);}
    else             { m_JSONstr = (char*)malloc(2);}
    JSONstrLength += 2;
    memcpy(m_JSONstr, "[\0", 2);

    char childCount[5]; char isAudio[6]; char itemSize[12];

    for(int i = 0; i < m_srvContent.size; i++) { // build a JSON string in PSRAM, e.g. [{"name":"m","dir":true},{"name":"s","dir":false}]
        itoa(m_srvContent.childCount[i], childCount, 10);
        if(m_srvContent.isAudio[i]) strcpy(isAudio, "true"); else strcpy(isAudio, "false");
        ltoa(m_srvContent.itemSize[i], itemSize, 10);
        JSONstrLength = strlen(childCount) + strlen(isAudio) + strlen(itemSize) + strlen(m_srvContent.itemURL[i])+
                        strlen(m_srvContent.objectId[i]) + strlen(m_srvContent.parentId[i]) + strlen(m_srvContent.title[i]) + strlen(m_srvContent.duration[i]);

    //  [{"objectId":"1$4","parentId":"1","childCount":"5","title":"Bilder","isAudio":"false","itemSize":"342345","itemURL":"http://myPC/Pictues/myPicture.jpg"},{"objectId ...."}]
    //  {"objectId":"","parentId":"","childCount":"","title":"","isAudio":"","itemSize":"","dur:"","itemURL":""},   --> 105 chars
        JSONstrLength += strlen(m_JSONstr) + 105 + 2;

        if(m_PSRAMfound) { m_JSONstr = (char*)ps_realloc(m_JSONstr, JSONstrLength); }
        else             { m_JSONstr = (char*)realloc(m_JSONstr, JSONstrLength); }

        strcat(m_JSONstr, "{\"objectId\":\""); strcat(m_JSONstr, m_srvContent.objectId[i]);
        strcat(m_JSONstr, "\",\"parentId\":\""); strcat(m_JSONstr, m_srvContent.parentId[i]);
        strcat(m_JSONstr, "\",\"childCount\":\""); strcat(m_JSONstr, childCount);
        strcat(m_JSONstr, "\",\"title\":\""); strcat(m_JSONstr, m_srvContent.title[i]);
        strcat(m_JSONstr, "\",\"isAudio\":\""); strcat(m_JSONstr, isAudio);
        strcat(m_JSONstr, "\",\"itemSize\":\""); strcat(m_JSONstr, itemSize);
        strcat(m_JSONstr, "\",\"dur\":\""); strcat(m_JSONstr, m_srvContent.duration[i]);
        strcat(m_JSONstr, "\",\"itemURL\":\""); strcat(m_JSONstr, m_srvContent.itemURL[i]);
        strcat(m_JSONstr, "\"},");
    }
    m_JSONstr[JSONstrLength - 2] = ']';  // replace comma by square bracket close
    m_JSONstr[JSONstrLength - 1] = '\0'; // and terminate
    return m_JSONstr;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
uint8_t DLNA_Client::getState(){
    return m_state;
}
//------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void DLNA_Client::loop(){
    static uint8_t cnt = 0;
    static uint8_t fail = 0;
    bool res;
    switch(m_state){
        case IDLE:
            break;
        case SEEK_SERVER:
            if(m_timeStamp + SEEK_TIMEOUT > millis()){
                int len = m_udp.parsePacket();
                if(len > 0){
                    parseDlnaServer(len); // registers all media servers that respond within the time until the timeout
                }
                cnt = 0;
                fail = 0;
            }
            else{
                m_udp.stop();
                m_state = GET_SERVER_ITEMS;
            }
            break;
        case GET_SERVER_ITEMS:
            if(cnt < m_dlnaServer.size){
                if(fail == 3) {fail = 0; log_e("no response from svr [%i]", cnt); cnt++; break;}
                res = srvGet(cnt);
                if(!res){/* log_e("error in srvGet"); m_state = IDLE; */ fail++; break;}
                res = readHttpHeader();
                if(!res){/* log_e("error in readHttpHeader");  m_state = IDLE;*/ fail++; break;}
                res = readContent();
                if(!res){/* log_e("error in readContent"); m_state = IDLE; */ fail++; break;}
                res = getServerItems(cnt);
                if(!res){/* log_e("error in readContent"); m_state = IDLE; */ fail++; break;}
                cnt++;
                break;
            }
            cnt = 0;
            dlna_seekReady(m_dlnaServer.size);
            m_state = IDLE;
            break;
        case BROWSE_SERVER:
            res = srvPost(m_srvNr, m_objectId, m_startingIndex, m_maxCount);
            if(!res){m_state = IDLE; break;}
            res = readHttpHeader();
            if(!res) {m_state = IDLE; break;}
            res = readContent();
            if(!res) {m_state = IDLE; break;}
            res = browseResult();
            if(!res) {m_state = IDLE; break;}
            cnt = 0;
            m_state = IDLE;
            break;
        default: break;
    }
}
