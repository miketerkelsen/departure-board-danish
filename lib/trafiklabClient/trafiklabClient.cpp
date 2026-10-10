/*
 * Departures Board (c) 2025-2026 Gadec Software
 *
 * trafiklabClient Library
 *
 * https://github.com/gadec-uk/departures-board
 *
 * This work is licensed under Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International.
 * To view a copy of this license, visit https://creativecommons.org/licenses/by-nc-sa/4.0/
 */

#include <trafiklabClient.h>
#include <boardText.h>

#include <WiFiClientSecure.h>
#include <time.h>
#include <cctype>
#include <cstdlib>
#include <cstdarg>
#include <algorithm>
#include <new>
#include <esp_heap_caps.h>

// A TLS handshake is allowed 120 SECONDS by default - see the matching comment in rejseplanenClient.cpp for why
// the connect phase is bounded to a handful of short attempts here.
#define TL_MAXCONNECTTRIES 3

trafiklabClient::trafiklabClient(rdiStation *station, stnMessages *messages, sharedBufferSpace *sharedBuffer) : xStation(station), xMessages(messages), js(sharedBuffer) {
    cacheAreaId[0] = '\0';
    pendingKey[0] = '\0';
    path[0] = '\0';
}

void trafiklabClient::convertToBoardText(char* s, size_t maxLen) {
    convertUtf8ToBoardText(s, maxLen);
}

void trafiklabClient::logResult(const char *fmt, ...) {
    size_t used = strlen(js->lastResultMessage);
    if (used >= MAXRESULTMESSAGESIZE-1) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(js->lastResultMessage+used, MAXRESULTMESSAGESIZE-used, fmt, ap);
    va_end(ap);
}

// Sort by effective (realtime if numeric, else scheduled) time, then seconds - Stockholm's tunnelbana times are
// second-precise and several lines often leave the same minute. Same late-evening rollover fudge as the
// Rejseplanen client.
bool trafiklabClient::compareDepartures(const rdiService& a, const rdiService& b) {
    int hour1, minute1, hour2, minute2;
    sscanf(a.sortTime, "%d:%d", &hour1, &minute1);
    sscanf(b.sortTime, "%d:%d", &hour2, &minute2);
    if (hour1 != hour2) {
        if (hour1 < 2 && hour2 > 20) return false;
        if (hour2 < 2 && hour1 > 20) return true;
        return hour1 < hour2;
    }
    if (minute1 != minute2) return minute1 < minute2;
    if (a.sSec != b.sSec) return a.sSec < b.sSec;
    return strcmp(a.destination, b.destination) < 0;
}

//
// JSON path tracking
//

void trafiklabClient::buildPath() {
    path[0] = '\0';
    if (lvl > TL_MAXPATHDEPTH) return;     // deeper than anything this client reads
    size_t n = 0;
    for (int i=0; i<lvl; i++) {
        if (!frames[i].name[0]) continue;
        if (n && n < sizeof(path)-1) path[n++] = '.';
        n += strlcpy(path+n, frames[i].name, sizeof(path)-n);
        if (n >= sizeof(path)) { n = sizeof(path)-1; break; }
    }
    // Inside an object the value belongs to the key just read; a scalar inside an array has no key of its own.
    if (lvl > 0 && !frames[lvl-1].isArray && pendingKey[0]) {
        if (n && n < sizeof(path)-1) path[n++] = '.';
        strlcpy(path+n, pendingKey, sizeof(path)-n);
    }
}

void trafiklabClient::whitespace(char c) {}
void trafiklabClient::endDocument() {}

void trafiklabClient::startDocument() {
    lvl = 0;
    pendingKey[0] = '\0';
    path[0] = '\0';
}

void trafiklabClient::key(const char *k) {
    strlcpy(pendingKey, k, sizeof(pendingKey));
}

void trafiklabClient::startArray() {
    if (lvl < TL_MAXPATHDEPTH) {
        strlcpy(frames[lvl].name, pendingKey, TL_NAMESIZE);
        frames[lvl].isArray = true;
    }
    lvl++;
    if (parseMode == TL_PARSE_DEPARTURES && lvl == 2 && strcmp(pendingKey,"departures")==0) sawDeparturesArray = true;
}

void trafiklabClient::endArray() {
    if (lvl > 0) lvl--;
}

static const char* tlElementArrayName(int mode) {
    // 0 = departures, 1 = trip, 2 = search - the array whose elements are the records of interest
    return mode == 0 ? "departures" : (mode == 1 ? "calls" : "stop_groups");
}

void trafiklabClient::startObject() {
    // Array elements have no key of their own: they get an empty frame name, so a record's fields come out as
    // "<array>.<field>" and nested objects as "<array>.<object>.<field>".
    bool inArray = (lvl > 0 && lvl <= TL_MAXPATHDEPTH && frames[lvl-1].isArray);
    if (lvl == 2 && inArray && !enough && strcmp(frames[1].name, tlElementArrayName(parseMode)) == 0) {
        if (parseMode == TL_PARSE_DEPARTURES) resetRaw();
        else if (parseMode == TL_PARSE_TRIP) { tripCallStopName[0] = '\0'; tripCallAreaId[0] = '\0'; }
        else { searchId[0] = '\0'; searchName[0] = '\0'; searchAreaType[0] = '\0'; searchHasMode = false; }
    }
    if (lvl < TL_MAXPATHDEPTH) {
        if (inArray || lvl == 0) frames[lvl].name[0] = '\0';
        else strlcpy(frames[lvl].name, pendingKey, TL_NAMESIZE);
        frames[lvl].isArray = false;
    }
    lvl++;
}

void trafiklabClient::endObject() {
    if (lvl > 0) lvl--;
    if (lvl == 2 && !enough && frames[1].isArray && strcmp(frames[1].name, tlElementArrayName(parseMode)) == 0) {
        if (parseMode == TL_PARSE_DEPARTURES) finaliseDeparture();
        else if (parseMode == TL_PARSE_TRIP) finaliseTripCall();
        else finaliseSearchGroup();
    }
}

void trafiklabClient::value(const char *v) {
    if (enough) return;
    buildPath();
    if (!path[0]) return;
    if (parseMode == TL_PARSE_DEPARTURES) {
        if (strncmp(path,"departures.",11) != 0) return;
        const char *f = path + 11;
        if (strcmp(f,"scheduled")==0) strlcpy(raw.scheduled,v,sizeof(raw.scheduled));
        else if (strcmp(f,"realtime")==0) strlcpy(raw.realtime,v,sizeof(raw.realtime));
        else if (strcmp(f,"canceled")==0) raw.cancelled = (strcmp(v,"true")==0);
        else if (strcmp(f,"route.designation")==0) strlcpy(raw.line,v,sizeof(raw.line));
        else if (strcmp(f,"route.transport_mode")==0) strlcpy(raw.mode,v,sizeof(raw.mode));
        else if (strcmp(f,"route.direction")==0) strlcpy(raw.direction,v,sizeof(raw.direction));
        else if (strcmp(f,"route.destination.name")==0) strlcpy(raw.destName,v,sizeof(raw.destName));
        else if (strcmp(f,"trip.trip_id")==0) strlcpy(raw.tripId,v,sizeof(raw.tripId));
        else if (strcmp(f,"trip.start_date")==0) strlcpy(raw.startDate,v,sizeof(raw.startDate));
        else if (strcmp(f,"stop.name")==0) strlcpy(raw.stopName,v,sizeof(raw.stopName));
        else if (strcmp(f,"scheduled_platform.designation")==0) strlcpy(raw.platform,v,sizeof(raw.platform));
        else if (strcmp(f,"realtime_platform.designation")==0) strlcpy(raw.rtPlatform,v,sizeof(raw.rtPlatform));
        else if (strcmp(f,"agency.operator")==0) strlcpy(raw.opco,v,sizeof(raw.opco));
    } else if (parseMode == TL_PARSE_TRIP) {
        if (strcmp(path,"calls.stop.name")==0) strlcpy(tripCallStopName,v,sizeof(tripCallStopName));
        else if (strcmp(path,"calls.stop.area_id")==0) strlcpy(tripCallAreaId,v,sizeof(tripCallAreaId));
    } else {
        if (strcmp(path,"stop_groups.id")==0) strlcpy(searchId,v,sizeof(searchId));
        else if (strcmp(path,"stop_groups.name")==0) strlcpy(searchName,v,sizeof(searchName));
        else if (strcmp(path,"stop_groups.area_type")==0) strlcpy(searchAreaType,v,sizeof(searchAreaType));
        else if (strcmp(path,"stop_groups.transport_modes")==0 && searchRequireMode && strcmp(v,searchRequireMode)==0) searchHasMode = true;
    }
}

//
// Records
//

void trafiklabClient::resetRaw() {
    raw.scheduled[0] = '\0';
    raw.realtime[0] = '\0';
    raw.line[0] = '\0';
    raw.mode[0] = '\0';
    raw.direction[0] = '\0';
    raw.destName[0] = '\0';
    raw.tripId[0] = '\0';
    raw.startDate[0] = '\0';
    raw.stopName[0] = '\0';
    raw.platform[0] = '\0';
    raw.rtPlatform[0] = '\0';
    raw.opco[0] = '\0';
    raw.cancelled = false;
}

// A departure record has closed: turn the scratch fields into the next service slot.
void trafiklabClient::finaliseDeparture() {
    if (xStation->numServices >= MAXBOARDSERVICES) return;
    if (metroOnlyFilter && strcasecmp(raw.mode,"METRO") != 0) return;
    if (strlen(raw.scheduled) < 19) return;                   // "2026-10-09T22:35:00"
    const char *dest = raw.direction[0] ? raw.direction : raw.destName;
    if (!dest[0]) return;

    rdiService &svc = xStation->service[xStation->numServices];

    // Scheduled and (when the vehicle is tracked) real-time clock times; the seconds come from whichever one the
    // countdown is based on, so a half-minute countdown stays right.
    char schedHM[6], rtHM[6];
    strlcpy(schedHM, raw.scheduled+11, sizeof(schedHM));
    const char *base = (raw.realtime[0] && strlen(raw.realtime) >= 19) ? raw.realtime : raw.scheduled;
    strlcpy(rtHM, base+11, sizeof(rtHM));

    strlcpy(svc.sTime, schedHM, sizeof(svc.sTime));
    strlcpy(svc.destination, dest, sizeof(svc.destination));
    convertToBoardText(svc.destination, sizeof(svc.destination));
    strlcpy(svc.via, raw.line, sizeof(svc.via));             // the line number is the badge label
    strlcpy(svc.opco, raw.opco, sizeof(svc.opco));
    convertToBoardText(svc.opco, sizeof(svc.opco));
    strlcpy(svc.stopArea, raw.stopName, sizeof(svc.stopArea));
    convertToBoardText(svc.stopArea, sizeof(svc.stopArea));
    strlcpy(svc.platform, raw.rtPlatform[0] ? raw.rtPlatform : raw.platform, sizeof(svc.platform));

    // tripId + start date identifies one run of one trip, and is what the Trips request needs.
    if (raw.tripId[0] && raw.startDate[0]) snprintf(svc.serviceID, sizeof(svc.serviceID), "%s/%s", raw.tripId, raw.startDate);
    else snprintf(svc.serviceID, sizeof(svc.serviceID), "%s|%s|%s", raw.line, raw.scheduled, raw.direction);

    svc.serviceType = TRAIN;
    svc.isSTog = false;
    svc.isMetro = true;                                       // drives the round line badge and half-minute countdown
    svc.isCancelled = false;
    svc.isDelayed = false;
    svc.calling[0] = '\0';

    if (raw.cancelled) {
        strlcpy(svc.etd, "Inst\xE4lld", sizeof(svc.etd));
        svc.isCancelled = true;
        base = raw.scheduled;
    } else if (strcmp(rtHM, schedHM) != 0) {
        strlcpy(svc.etd, rtHM, sizeof(svc.etd));
        svc.isDelayed = true;
    } else {
        strlcpy(svc.etd, "I tid", sizeof(svc.etd));
    }
    strlcpy(svc.sortTime, isdigit((unsigned char)svc.etd[0]) ? svc.etd : svc.sTime, sizeof(svc.sortTime));
    svc.sSec = (uint8_t)atoi(base+17);

    xStation->numServices++;
    if (xStation->numServices >= maxRows) enough = true;
}

// A call (one stop) of a trip has closed: skip up to and including the board's own stop, then append every
// stop after it to the "A, B, C" list.
void trafiklabClient::finaliseTripCall() {
    if (!tripCallStopName[0]) return;
    tripAnyCall = true;
    convertToBoardText(tripCallStopName, sizeof(tripCallStopName));
    if (!tripSeenBoardStop) {
        if ((tripCallAreaId[0] && strcmp(tripCallAreaId, tripBoardAreaId) == 0) || strcmp(tripCallStopName, tripBoardStopName) == 0) tripSeenBoardStop = true;
        return;
    }
    size_t cur = strlen(tripCalling);
    size_t add = strlen(tripCallStopName) + (cur ? 2 : 0);
    if (cur + add >= sizeof(tripCalling)) return;             // list full - later stops simply don't fit
    if (cur) strcat(tripCalling, ", ");
    strcat(tripCalling, tripCallStopName);
}

void trafiklabClient::finaliseSearchGroup() {
    if (searchCount >= searchMax) { enough = true; return; }
    if (!searchId[0] || !searchName[0]) return;
    if (strcmp(searchAreaType, "META_STOP") == 0) return;     // a whole town's stops together - the specific stop is what's wanted
    if (searchRequireMode && !searchHasMode) return;
    // Deliberately kept as UTF-8: this goes to the browser, not onto the board's single-byte display.
    if (searchCount) searchResult += ",";
    searchResult += "{\"name\":\"";
    for (size_t i=0; searchName[i]; i++) {
        char c = searchName[i];
        if (c=='"' || c=='\\') searchResult += '\\';
        searchResult += c;
    }
    searchResult += "\",\"id\":\"";
    searchResult += searchId;
    searchResult += "\"}";
    searchCount++;
    if (searchCount >= searchMax) enough = true;
}

//
// Transport
//

bool trafiklabClient::connect(WiFiClientSecure &client) {
    // See TL_MIN_SAFE_HEAP: a new TLS connection needs one big contiguous block.
    if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < TL_MIN_SAFE_HEAP) {
        strcpy(js->lastResultMessage, "Error: Heap too fragmented, skipping cycle");
        return false;
    }
    client.setInsecure();
    client.setTimeout(8000);
    client.setConnectionTimeout(8000);
    client.setHandshakeTimeout(10);   // seconds
    client.setNoDelay(false);
    int tries = 0;
    while (!client.connect(tlHost, 443) && tries < TL_MAXCONNECTTRIES) {
        delay(100);
        tries++;
    }
    if (tries >= TL_MAXCONNECTTRIES) {
        strcpy(js->lastResultMessage, "Error: Connect timed out");
        return false;
    }
    return true;
}

// Reads the status line and headers. Trafiklab sends "HTTP/1.1 200 " with no reason phrase, so the code is read
// as a number rather than matched against "200 OK".
int trafiklabClient::readResponseHeaders(WiFiClientSecure &client, bool &chunked, long &contentLength) {
    chunked = false;
    contentLength = -1;
    int waited = 0;
    while (!client.available()) {
        delay(100);
        if (++waited >= 80) return UPD_TIMEOUT;
    }
    String statusLine = client.readStringUntil('\n');
    int code = (statusLine.startsWith("HTTP/") && statusLine.length() >= 12) ? statusLine.substring(9,12).toInt() : 0;
    if (code != 200) {
        logResult("HTTP %d", code);
        if (code == 401 || code == 403) return UPD_UNAUTHORISED;      // 403 error.key.invalid for a bad key
        if (code == 404 || code >= 500) return UPD_DATA_ERROR;        // unknown stop / trip
        return UPD_HTTP_ERROR;
    }
    unsigned long headerDeadline = millis() + 1500UL;
    while ((client.available() || client.connected()) && millis() < headerDeadline) {
        String line = client.readStringUntil('\n');
        if (line.startsWith("Content-Length:")) contentLength = line.substring(15).toInt();
        else if (line.startsWith("Transfer-Encoding:") && line.indexOf("chunked") >= 0) chunked = true;
        if (line == "\r") break;
        delay(1);
    }
    return UPD_SUCCESS;
}

// Feeds the response body to the parser, decoding chunked framing, and stops as soon as the listener says it has
// everything it wants (`enough`) - a busy stop's 60-minute window is 60-90KB and only the first rows are used.
long trafiklabClient::readBody(WiFiClientSecure &client, bool chunked, long contentLength, JsonStreamingParserGS &parser, unsigned long timeoutMs, bool &timedOut) {
    timedOut = false;
    long received = 0;
    uint8_t chunk[512];
    unsigned long deadline = millis() + timeoutMs;

    if (chunked) {
        while (!enough) {
            if (millis() >= deadline) { timedOut = true; break; }
            String sizeLine = client.readStringUntil('\n');
            sizeLine.trim();
            if (!sizeLine.length()) {
                if (!client.connected() || millis() >= deadline) { timedOut = true; break; }
                continue;
            }
            long chunkSize = strtol(sizeLine.c_str(), nullptr, 16);
            if (chunkSize <= 0) break;                      // final chunk - body complete
            long chunkReceived = 0;
            while (chunkReceived < chunkSize && !enough) {
                if (millis() >= deadline) { timedOut = true; break; }
                int avail = client.available();
                if (avail > 0) {
                    long want = chunkSize - chunkReceived;
                    if (want > (long)sizeof(chunk)) want = sizeof(chunk);
                    if (avail > (int)want) avail = (int)want;
                    int n = client.read(chunk, avail);
                    for (int i=0;i<n && !enough;i++) parser.parse((char)chunk[i]);
                    chunkReceived += n;
                    received += n;
                } else if (!client.connected()) {
                    timedOut = true;
                    break;
                } else delay(5);
            }
            if (timedOut || enough) break;
            client.readStringUntil('\n');                   // CRLF after the chunk data
        }
        return received;
    }

    while (!enough && (contentLength < 0 || received < contentLength)) {
        if (millis() >= deadline) { timedOut = true; break; }
        int avail = client.available();
        if (avail > 0) {
            int n = client.read(chunk, avail > (int)sizeof(chunk) ? (int)sizeof(chunk) : avail);
            for (int i=0;i<n && !enough;i++) parser.parse((char)chunk[i]);
            received += n;
        } else if (!client.connected()) {
            if (contentLength >= 0 && received < contentLength) timedOut = true;
            break;
        } else delay(5);
    }
    return received;
}

void trafiklabClient::beginParse(tlParseMode mode) {
    parseMode = mode;
    enough = false;
    lvl = 0;
    pendingKey[0] = '\0';
    path[0] = '\0';
}

//
// Public API
//

int trafiklabClient::fetchDepartures(rdStation *station, stnMessages *messages, const char *areaId, const char *accessKey, int numRows, bool metroOnly, bool fetchCallingPoints) {
    unsigned long perfTimer = millis();
    js->lastResultMessage[0] = '\0';
    metroOnlyFilter = metroOnly;
    maxRows = numRows > MAXBOARDSERVICES ? MAXBOARDSERVICES : numRows;
    sawDeparturesArray = false;

    if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < TL_MIN_SAFE_HEAP) {
        strcpy(js->lastResultMessage, "Error: Heap too fragmented, skipping cycle");
        return UPD_NO_RESPONSE;
    }
    if (!accessKey[0]) return UPD_UNAUTHORISED;

    // The cache is specific to one board stop (the stops listed are those AFTER it), so a different stop starts
    // it over; it is also rebuilt once a day so a timetable change is eventually picked up.
    if (fetchCallingPoints && !cache) {
        tlCacheEntry* fresh = new (std::nothrow) tlCacheEntry[TL_CACHE_ENTRIES]();
        if (fresh) {
            portENTER_CRITICAL(&cacheMux);
            cache = fresh;
            cacheCount = 0;
            cacheBuiltAt = millis();
            portEXIT_CRITICAL(&cacheMux);
        }
    }
    if (cache && (strcmp(cacheAreaId, areaId) != 0 || millis() - cacheBuiltAt >= TL_CACHE_MAX_AGE_MS)) {
        portENTER_CRITICAL(&cacheMux);
        cacheCount = 0;
        cacheBuiltAt = millis();
        strlcpy(cacheAreaId, areaId, sizeof(cacheAreaId));
        portEXIT_CRITICAL(&cacheMux);
    }

    xStation->numServices = 0;
    xMessages->numMessages = 0;
    xStation->platformAvailable = false;
    strlcpy(xStation->location, areaId, sizeof(xStation->location));
    for (int i=0; i<MAXBOARDSERVICES; ++i) {
        rdiService &s = xStation->service[i];
        s.sTime[0] = '\0';
        s.destination[0] = '\0';
        s.via[0] = '\0';
        s.origin[0] = '\0';
        s.etd[0] = '\0';
        s.platform[0] = '\0';
        s.opco[0] = '\0';
        s.calling[0] = '\0';
        s.serviceID[0] = '\0';
        s.stopArea[0] = '\0';
        s.trainLength = 0;
        s.classesAvailable = 0;
        s.serviceType = 0;
        s.isCancelled = false;
        s.isDelayed = false;
        s.isSTog = false;
        s.isMetro = false;
        s.sSec = 0;
    }
    callingFetchKnown = false;
    nextCallingFetchKnown = false;

    WiFiClientSecure httpsClient;
    if (!connect(httpsClient)) return UPD_NO_RESPONSE;

    String request = String("GET /v1/departures/") + areaId + "?key=" + accessKey + " HTTP/1.1\r\nHost: " + tlHost + "\r\nAccept: application/json\r\nConnection: close\r\n\r\n";
    httpsClient.print(request);
    apiRequestCount++;

    bool chunked = false;
    long contentLength = -1;
    int headerResult = readResponseHeaders(httpsClient, chunked, contentLength);
    if (headerResult != UPD_SUCCESS) {
        httpsClient.stop();
        if (headerResult == UPD_TIMEOUT) strcpy(js->lastResultMessage, "Error: GET timed out");
        return headerResult;
    }

    JsonStreamingParserGS parser;
    parser.setListener(this);
    parser.reset();
    beginParse(TL_PARSE_DEPARTURES);
    bool timedOut = false;
    long received = readBody(httpsClient, chunked, contentLength, parser, 15000UL, timedOut);
    httpsClient.stop();

    if (timedOut && !enough) {
        sprintf(js->lastResultMessage, "Error: Timeout after %ld bytes", received);
        return UPD_TIMEOUT;
    }
    if (!sawDeparturesArray) {
        strcpy(js->lastResultMessage, "Error: Incomplete data");
        return UPD_DATA_ERROR;
    }

    if (xStation->numServices > 1) std::sort(xStation->service, xStation->service + xStation->numServices, compareDepartures);
    xStation->platformAvailable = true;

    // No departures is a real answer here (the tunnelbana does not run all night): the array was there and parsed.
    if (fetchCallingPoints && xStation->numServices) {
        callingFetchKnown = resolveCalling(0, station, areaId, accessKey);
        if (xStation->numServices > 1) nextCallingFetchKnown = resolveCalling(1, station, areaId, accessKey);
    }

    UBaseType_t uxHighWaterMark = uxTaskGetStackHighWaterMark(NULL);
    logResult("[TL] OK: D:%ld T:%lu S:%u", received, millis()-perfTimer, (unsigned)uxHighWaterMark);
    return UPD_SUCCESS;
}

int trafiklabClient::findCache(const char* line, const char* destination) {
    if (!cache || !line[0] || !destination[0]) return -1;
    for (int i=0; i<cacheCount; i++) {
        if (strcmp(cache[i].line, line) == 0 && strcmp(cache[i].destination, destination) == 0) return i;
    }
    return -1;
}

void trafiklabClient::storeCache(const char* line, const char* destination, const char* calling) {
    if (!cache || !line[0] || !destination[0]) return;
    portENTER_CRITICAL(&cacheMux);
    int idx = findCache(line, destination);
    if (idx < 0) {
        if (cacheCount >= TL_CACHE_ENTRIES) { portEXIT_CRITICAL(&cacheMux); return; }
        idx = cacheCount;
        strlcpy(cache[idx].line, line, sizeof(cache[idx].line));
        strlcpy(cache[idx].destination, destination, sizeof(cache[idx].destination));
        strlcpy(cache[idx].calling, calling, sizeof(cache[idx].calling));
        cacheCount++;                                       // published last, once the slot is complete
    } else {
        strlcpy(cache[idx].calling, calling, sizeof(cache[idx].calling));
    }
    portEXIT_CRITICAL(&cacheMux);
}

bool trafiklabClient::lookupCachedCalling(const char *line, const char *destination, char *callingOut, size_t callingOutSize) {
    portENTER_CRITICAL(&cacheMux);
    int idx = findCache(line, destination);
    if (idx < 0) { portEXIT_CRITICAL(&cacheMux); return false; }
    strlcpy(callingOut, cache[idx].calling, callingOutSize);
    portEXIT_CRITICAL(&cacheMux);
    return true;
}

bool trafiklabClient::resolveCalling(int idx, rdStation *station, const char* areaId, const char* key) {
    rdiService &svc = xStation->service[idx];

    // 1. Line + direction already looked up today.
    portENTER_CRITICAL(&cacheMux);
    int c = findCache(svc.via, svc.destination);
    if (c >= 0) strlcpy(svc.calling, cache[c].calling, sizeof(svc.calling));
    portEXIT_CRITICAL(&cacheMux);
    if (c >= 0) return true;

    // 2. The very same trip was already known on the board last time (it is simply moving up the list).
    for (int j=0; j<2 && j<station->numServices; j++) {
        if (strcmp(station->service[j].serviceID, svc.serviceID) != 0) continue;
        if (j == 0 && station->callingKnown) { strlcpy(svc.calling, station->calling, sizeof(svc.calling)); return true; }
        if (j == 1 && station->nextCallingKnown) { strlcpy(svc.calling, station->nextCalling, sizeof(svc.calling)); return true; }
    }

    // 3. Ask Trafiklab for the trip.
    char *slash = strchr(svc.serviceID, '/');
    if (!slash) return false;
    char tripId[24], startDate[12];
    size_t idLen = slash - svc.serviceID;
    if (idLen >= sizeof(tripId)) return false;
    memcpy(tripId, svc.serviceID, idLen);
    tripId[idLen] = '\0';
    strlcpy(startDate, slash+1, sizeof(startDate));

    char calling[TL_CACHE_CALLINGSIZE];
    if (fetchTripCalling(tripId, startDate, areaId, svc.stopArea, key, calling, sizeof(calling)) != UPD_SUCCESS) return false;
    strlcpy(svc.calling, calling, sizeof(svc.calling));
    storeCache(svc.via, svc.destination, calling);
    return true;
}

int trafiklabClient::fetchTripCalling(const char* tripId, const char* startDate, const char* areaId, const char* boardStopName, const char* key, char* out, size_t outSize) {
    unsigned long tStart = millis();
    WiFiClientSecure httpsClient;
    if (!connect(httpsClient)) { logResult(" TRIP:conn-fail"); return UPD_NO_RESPONSE; }

    String request = String("GET /v1/trips/") + tripId + "/" + startDate + "?key=" + key + " HTTP/1.1\r\nHost: " + tlHost + "\r\nAccept: application/json\r\nConnection: close\r\n\r\n";
    httpsClient.print(request);
    apiRequestCount++;

    bool chunked = false;
    long contentLength = -1;
    int headerResult = readResponseHeaders(httpsClient, chunked, contentLength);
    if (headerResult != UPD_SUCCESS) { httpsClient.stop(); return headerResult; }

    strlcpy(tripBoardAreaId, areaId, sizeof(tripBoardAreaId));
    strlcpy(tripBoardStopName, boardStopName, sizeof(tripBoardStopName));
    tripSeenBoardStop = false;
    tripAnyCall = false;
    tripCalling[0] = '\0';

    JsonStreamingParserGS parser;
    parser.setListener(this);
    parser.reset();
    beginParse(TL_PARSE_TRIP);
    bool timedOut = false;
    readBody(httpsClient, chunked, contentLength, parser, 12000UL, timedOut);
    httpsClient.stop();

    if (timedOut) { logResult(" TRIP:timeout"); return UPD_TIMEOUT; }
    if (!tripAnyCall || !tripSeenBoardStop) { logResult(" TRIP:nostop"); return UPD_DATA_ERROR; }
    strlcpy(out, tripCalling, outSize);
    logResult(" TRIP:%lums", millis()-tStart);
    return UPD_SUCCESS;
}

void trafiklabClient::loadDepartures(rdStation *station, stnMessages *messages) {
    station->boardChanged = false;
    messages->numMessages = 0;
    station->numServices = xStation->numServices;
    strlcpy(station->location, xStation->location, sizeof(station->location));
    station->platformAvailable = xStation->platformAvailable;
    for (int i=0; i<xStation->numServices; ++i) {
        strlcpy(station->service[i].sTime, xStation->service[i].sTime, sizeof(station->service[0].sTime));
        strlcpy(station->service[i].destination, xStation->service[i].destination, sizeof(station->service[0].destination));
        strlcpy(station->service[i].via, xStation->service[i].via, sizeof(station->service[0].via));
        strlcpy(station->service[i].etd, xStation->service[i].etd, sizeof(station->service[0].etd));
        strlcpy(station->service[i].platform, xStation->service[i].platform, sizeof(station->service[0].platform));
        station->service[i].isCancelled = xStation->service[i].isCancelled;
        station->service[i].isDelayed = xStation->service[i].isDelayed;
        station->service[i].trainLength = xStation->service[i].trainLength;
        station->service[i].classesAvailable = xStation->service[i].classesAvailable;
        strlcpy(station->service[i].opco, xStation->service[i].opco, sizeof(station->service[0].opco));
        strlcpy(station->service[i].stopArea, xStation->service[i].stopArea, sizeof(station->service[0].stopArea));
        station->service[i].serviceType = xStation->service[i].serviceType;
        station->service[i].isSTog = xStation->service[i].isSTog;
        station->service[i].isMetro = xStation->service[i].isMetro;
        station->service[i].sSec = xStation->service[i].sSec;
        strlcpy(station->service[i].serviceID, xStation->service[i].serviceID, sizeof(station->service[0].serviceID));
    }
    station->splitInfo[0] = '\0';
    station->nextSplitInfo[0] = '\0';
    station->serviceMessage[0] = '\0';
    station->origin[0] = '\0';
    station->nextOrigin[0] = '\0';
    if (xStation->numServices) {
        strlcpy(station->calling, xStation->service[0].calling, sizeof(station->calling));
        station->callingKnown = callingFetchKnown;
    } else {
        station->calling[0] = '\0';
        station->callingKnown = false;
    }
    if (xStation->numServices > 1) {
        strlcpy(station->nextCalling, xStation->service[1].calling, sizeof(station->nextCalling));
        station->nextCallingKnown = nextCallingFetchKnown;
    } else {
        station->nextCalling[0] = '\0';
        station->nextCallingKnown = false;
    }
}

// Stop-name search, called synchronously from the web server's request handler.
String trafiklabClient::searchStops(const char *query, const char *accessKey, const char *requireMode, int maxResults) {
    searchResult = "[";
    searchCount = 0;
    searchMax = maxResults;
    searchRequireMode = requireMode;
    if (!accessKey[0]) return "[]";
    if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < TL_MIN_SAFE_HEAP) return "[]";

    WiFiClientSecure httpsClient;
    httpsClient.setInsecure();
    httpsClient.setTimeout(6000);
    httpsClient.setConnectionTimeout(6000);
    httpsClient.setHandshakeTimeout(10);
    httpsClient.setNoDelay(false);
    int tries = 0;
    while (!httpsClient.connect(tlHost, 443) && tries < 5) {
        delay(100);
        tries++;
    }
    if (tries >= 5) return "[]";

    // Percent-encode the (UTF-8) query - Swedish letters, spaces, hyphens...
    String encoded;
    for (size_t i=0; i<strlen(query); ++i) {
        unsigned char ch = (unsigned char)query[i];
        if (isalnum(ch)) encoded += (char)ch;
        else {
            char buf[4];
            sprintf(buf, "%%%02X", ch);
            encoded += buf;
        }
    }
    String request = String("GET /v1/stops/name/") + encoded + "?key=" + accessKey + " HTTP/1.1\r\nHost: " + tlHost + "\r\nAccept: application/json\r\nConnection: close\r\n\r\n";
    httpsClient.print(request);
    apiRequestCount++;

    bool chunked = false;
    long contentLength = -1;
    if (readResponseHeaders(httpsClient, chunked, contentLength) != UPD_SUCCESS) {
        httpsClient.stop();
        return "[]";
    }
    JsonStreamingParserGS parser;
    parser.setListener(this);
    parser.reset();
    beginParse(TL_PARSE_SEARCH);
    bool timedOut = false;
    readBody(httpsClient, chunked, contentLength, parser, 8000UL, timedOut);
    httpsClient.stop();
    searchRequireMode = nullptr;

    searchResult += "]";
    return searchResult;
}
