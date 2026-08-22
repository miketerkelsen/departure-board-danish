/*
 * Departures Board (c) 2025-2026 Gadec Software
 *
 * rejseplanenClient Library
 *
 * https://github.com/gadec-uk/departures-board
 *
 * This work is licensed under Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International.
 * To view a copy of this license, visit https://creativecommons.org/licenses/by-nc-sa/4.0/
 */

#include <rejseplanenClient.h>
#include <jsonListenerGS.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <cctype>
#include <cstdlib>
#include <algorithm>

rejseplanenClient::rejseplanenClient(rdiStation *station, stnMessages *messages, sharedBufferSpace *sharedBuffer) : xStation(station), xMessages(messages), js(sharedBuffer) {}

// Custom comparator to sort by effective (realtime if numeric, else scheduled) time - same rollover
// fudge as the UK rail clients so late-evening/early-morning boards sort sensibly.
bool rejseplanenClient::compareTimes(const rdiService& a, const rdiService& b) {
    int hour1, minute1, hour2, minute2;
    sscanf(a.sortTime, "%d:%d", &hour1, &minute1);
    sscanf(b.sortTime, "%d:%d", &hour2, &minute2);
    if (hour1 != hour2) {
        if (hour1 < 2 && hour2 > 20) return false;
        if (hour2 < 2 && hour1 > 20) return true;
        else return hour1 < hour2;
    }
    return minute1 < minute2;
}

// Truncate a Rejseplanen "HH:MM:SS" time string down to "HH:MM" in place.
static void truncateSeconds(char* t) {
    if (t[0] && strlen(t) > 5 && t[2] == ':' && t[5] == ':') t[5] = '\0';
}

// Case-insensitive substring search (avoids pulling in strcasestr, which isn't universally
// available in the ESP32 toolchain's libc).
static bool containsCaseInsensitive(const char* haystack, const char* needle) {
    if (!haystack[0] || !needle[0]) return false;
    size_t hLen = strlen(haystack), nLen = strlen(needle);
    if (nLen > hLen) return false;
    for (size_t i=0; i<=hLen-nLen; i++) {
        size_t j=0;
        while (j<nLen && tolower((unsigned char)haystack[i+j])==tolower((unsigned char)needle[j])) j++;
        if (j==nLen) return true;
    }
    return false;
}

// Rejseplanen sends station/destination names as UTF-8, so æ/ø/å (and other Latin-1 supplement
// letters, e.g. é) arrive as 2-byte sequences (0xC3 0x80-0xBF). The board's fonts are built with
// -DU8G2_WITHOUT_UNICODE, but that flag only disables *multi-byte* UTF-8 decoding for codepoints
// above 255 - single-byte lookup for codepoints 0-255 (which is exactly what ISO 8859-1/Latin-1
// is) works regardless of it. U8g2's own "_tf" stock fonts already ship the full Latin-1 glyph
// set, so rather than transliterating to "ae"/"oe"/"aa" (illegible-looking, per user feedback),
// this converts each UTF-8 2-byte sequence down to its single-byte Latin-1 equivalent instead -
// e.g. \xC3\xA6 (UTF-8 æ) becomes the single byte \xE6 (Latin-1 æ) - which the board then renders
// with a real glyph, via setSmallFont()/setTallFont()/setTubeFont() switching to a Latin-1-aware
// stock font for Danish modes (see "Departures Board.cpp").
void rejseplanenClient::convertDanishToLatin1(char* input, size_t maxLen) {
    if (!input || !input[0]) return;
    char output[MAXLOCATIONSIZE*2];
    size_t outPos = 0;
    size_t len = strlen(input);
    for (size_t i=0; i<len && outPos < sizeof(output)-1;) {
        unsigned char c = (unsigned char)input[i];
        if (c == 0xC3 && i+1 < len) {
            unsigned char c2 = (unsigned char)input[i+1];
            if (c2 >= 0x80 && c2 <= 0xBF) {
                output[outPos++] = (char)(unsigned char)(0xC0 | (c2 & 0x3F)); // -> Latin-1 0xC0-0xFF
                i += 2;
                continue;
            }
        }
        output[outPos++] = input[i];
        i++;
    }
    output[outPos] = '\0';
    strlcpy(input, output, maxLen);
}

void rejseplanenClient::resetRawRecord() {
    raw.time[0] = '\0';
    raw.rtTime[0] = '\0';
    raw.track[0] = '\0';
    raw.rtTrack[0] = '\0';
    raw.cancelled = false;
    raw.catCode = -1;
    raw.catOut[0] = '\0';
    raw.ref[0] = '\0';
    raw.opco[0] = '\0';
    raw.direction[0] = '\0';
    raw.name[0] = '\0';
    raw.stop[0] = '\0';
}

// Rejseplanen's Departure.stop is e.g. "OBC Nord Plads H (Odense Kommune)" - strip the trailing
// " (<municipality>)" so callers get just the stand/platform-area name to match against.
static void stripMunicipalitySuffix(char* stop) {
    char* paren = strchr(stop, '(');
    if (paren) {
        if (paren > stop && *(paren-1) == ' ') paren--;
        *paren = '\0';
    }
}

// Called when a Departure record's closing '}' is seen - reconcile the scratch fields
// collected (in whatever order the JSON delivered them) into the next service slot.
void rejseplanenClient::finaliseDepartureRecord() {
    if (xStation->numServices >= MAXBOARDSERVICES) return;
    if (!raw.time[0] || !raw.direction[0]) return; // incomplete record, skip it

    int i = xStation->numServices;
    rdiService &svc = xStation->service[i];

    strlcpy(svc.sTime, raw.time, sizeof(svc.sTime));
    truncateSeconds(svc.sTime);

    strlcpy(svc.destination, raw.direction, sizeof(svc.destination));
    convertDanishToLatin1(svc.destination, sizeof(svc.destination));

    strlcpy(svc.via, raw.name, sizeof(svc.via));
    strlcpy(svc.opco, raw.opco, sizeof(svc.opco));
    strlcpy(svc.serviceID, raw.ref, sizeof(svc.serviceID));

    strlcpy(svc.stopArea, raw.stop, sizeof(svc.stopArea));
    stripMunicipalitySuffix(svc.stopArea);
    convertDanishToLatin1(svc.stopArea, sizeof(svc.stopArea));

    svc.platform[0] = '\0';
    if (raw.rtTrack[0]) strlcpy(svc.platform, raw.rtTrack, sizeof(svc.platform));
    else if (raw.track[0]) strlcpy(svc.platform, raw.track, sizeof(svc.platform));

    // catCode is *not* a reliable bus signal on its own - Rejseplanen reuses the same catCode
    // number for unrelated categories at different stations (e.g. catCode 3 covers both genuine
    // Togbus rail-replacement buses AND international long-distance trains like EuroCity/RailJet/
    // Snalltaget at Odense). catOut is the human-readable product abbreviation and reliably says
    // "Bus" for anything bus-shaped, so match on that instead.
    svc.serviceType = containsCaseInsensitive(raw.catOut,"bus") ? BUS : TRAIN;
    svc.isSTog = containsCaseInsensitive(raw.catOut,"S-Tog");

    char rtTimeShort[6];
    strlcpy(rtTimeShort, raw.rtTime, sizeof(rtTimeShort));
    truncateSeconds(rtTimeShort);

    if (raw.cancelled) {
        strlcpy(svc.etd, "Aflyst", sizeof(svc.etd));
        svc.isCancelled = true;
    } else if (rtTimeShort[0] && strcmp(rtTimeShort, svc.sTime) != 0) {
        strlcpy(svc.etd, rtTimeShort, sizeof(svc.etd));
        svc.isDelayed = true;
    } else {
        strlcpy(svc.etd, "Rettidig", sizeof(svc.etd));
    }

    strlcpy(svc.sortTime, isdigit((unsigned char)svc.etd[0]) ? svc.etd : svc.sTime, sizeof(svc.sortTime));

    xStation->numServices++;
}

// Called when a Stops/Stop record's closing '}' is seen while parsing journeyDetail.
void rejseplanenClient::finaliseCallingStop() {
    if (!stopScratchName[0]) return;
    if (numCallingStops >= (int)(sizeof(callingStops)/sizeof(callingStops[0]))) return;
    strlcpy(callingStops[numCallingStops].name, stopScratchName, sizeof(callingStops[0].name));
    convertDanishToLatin1(callingStops[numCallingStops].name, sizeof(callingStops[0].name));
    strlcpy(callingStops[numCallingStops].extId, stopScratchExtId, sizeof(callingStops[0].extId));
    // Prefer the arrival time (when the train reaches this calling point), falling back to the
    // departure time for stops that only have one (e.g. the very first/last stop of the journey).
    strlcpy(callingStops[numCallingStops].time, stopScratchArrTime[0] ? stopScratchArrTime : stopScratchDepTime, sizeof(callingStops[0].time));
    numCallingStops++;
}

void rejseplanenClient::whitespace(char c) {}

void rejseplanenClient::startDocument() {
    stackTop = 0;
    pendingKey[0] = '\0';
    targetElementKey[0] = '\0';
    currentPath[0] = '\0';
    inTargetArray = false;
    arrayDepth = 0;
    arrayBaseDepth = -1;
    targetArrayDepth = -1;
}

void rejseplanenClient::buildCurrentPath(const char* k) {
    currentPath[0] = '\0';
    for (int i=0;i<stackTop;i++) {
        strlcat(currentPath, pathStack[i], sizeof(currentPath));
        strlcat(currentPath, "/", sizeof(currentPath));
    }
    strlcat(currentPath, k, sizeof(currentPath));
}

void rejseplanenClient::key(const char *k) {
    strlcpy(pendingKey, k, sizeof(pendingKey));
    buildCurrentPath(k);
}

void rejseplanenClient::value(const char *value) {
    if (fetchingDepartures) {
        if (strcmp(currentPath,"Departure/name")==0) strlcpy(raw.name,value,sizeof(raw.name));
        else if (strcmp(currentPath,"Departure/direction")==0) strlcpy(raw.direction,value,sizeof(raw.direction));
        else if (strcmp(currentPath,"Departure/stop")==0) strlcpy(raw.stop,value,sizeof(raw.stop));
        else if (strcmp(currentPath,"Departure/time")==0) strlcpy(raw.time,value,sizeof(raw.time));
        else if (strcmp(currentPath,"Departure/rtTime")==0) strlcpy(raw.rtTime,value,sizeof(raw.rtTime));
        else if (strcmp(currentPath,"Departure/track")==0) strlcpy(raw.track,value,sizeof(raw.track));
        else if (strcmp(currentPath,"Departure/rtTrack")==0) strlcpy(raw.rtTrack,value,sizeof(raw.rtTrack));
        else if (strcmp(currentPath,"Departure/cancelled")==0) raw.cancelled = (strcmp(value,"true")==0);
        else if (strcmp(currentPath,"Departure/JourneyDetailRef/ref")==0) strlcpy(raw.ref,value,sizeof(raw.ref));
        else if (strcmp(currentPath,"Departure/ProductAtStop/catCode")==0) raw.catCode = atoi(value);
        else if (strcmp(currentPath,"Departure/ProductAtStop/catOut")==0) strlcpy(raw.catOut,value,sizeof(raw.catOut));
        else if (strcmp(currentPath,"Departure/ProductAtStop/operator")==0) strlcpy(raw.opco,value,sizeof(raw.opco));
    } else {
        if (strcmp(currentPath,"Stops/Stop/name")==0) strlcpy(stopScratchName,value,sizeof(stopScratchName));
        else if (strcmp(currentPath,"Stops/Stop/extId")==0) strlcpy(stopScratchExtId,value,sizeof(stopScratchExtId));
        else if (strcmp(currentPath,"Stops/Stop/arrTime")==0) strlcpy(stopScratchArrTime,value,sizeof(stopScratchArrTime));
        else if (strcmp(currentPath,"Stops/Stop/depTime")==0) strlcpy(stopScratchDepTime,value,sizeof(stopScratchDepTime));
    }
}

void rejseplanenClient::startArray() {
    arrayDepth++;
    if (!inTargetArray) {
        bool isTarget;
        if (fetchingDepartures) {
            isTarget = (strcmp(pendingKey,"Departure")==0);
        } else {
            isTarget = (strcmp(pendingKey,"Stop")==0 && stackTop>0 && strcmp(pathStack[stackTop-1],"Stops")==0);
        }
        if (isTarget) {
            inTargetArray = true;
            targetArrayDepth = arrayDepth;
            arrayBaseDepth = stackTop;
            // Capture the array's own key NOW, before it can be overwritten by key() calls made
            // while walking each element's fields (see targetElementKey's declaration comment).
            strlcpy(targetElementKey, pendingKey, sizeof(targetElementKey));
        }
    }
}

void rejseplanenClient::endArray() {
    if (inTargetArray && arrayDepth == targetArrayDepth) inTargetArray = false;
    arrayDepth--;
}

void rejseplanenClient::startObject() {
    // Are we about to start a new element of the target array (Departure, or Stops/Stop)?
    bool isNewTargetElement = inTargetArray && stackTop == arrayBaseDepth;

    if (isNewTargetElement) {
        if (fetchingDepartures) resetRawRecord();
        else { stopScratchName[0] = '\0'; stopScratchExtId[0] = '\0'; stopScratchArrTime[0] = '\0'; stopScratchDepTime[0] = '\0'; }
    }

    // Array elements have no key() call of their own. For a NEW target-array element, use the
    // stable targetElementKey captured back in startArray() - NOT the live pendingKey, which by
    // this point has been overwritten by whatever field key() was last called while walking the
    // PREVIOUS element's own contents (e.g. "directionFlag", the last field of a Departure record).
    // Pushing that stale value instead of "Departure"/"Stop" would build the wrong path prefix for
    // every field in every element after the first, so none of them would ever match a known path.
    // For every other (non-boundary) object push, pendingKey is correct as-is, since those always
    // have a proper, immediately-preceding key() call.
    const char *keyToPush = isNewTargetElement ? targetElementKey : pendingKey;

    // The anonymous document-root object has no preceding key() call - skip pushing it so
    // paths don't gain a spurious leading "/" (every other startObject() always has a non-empty
    // key to push, per the above).
    //
    // IMPORTANT: stackTop must increment every time we push (whenever keyToPush is non-empty),
    // in lockstep with endObject()'s unconditional decrement - regardless of whether there's
    // still room in pathStack to record the name. Some real Rejseplanen records (service alerts
    // with Messages/Message/affectedStops/StopLocation chains) nest deeper than MAXPATHSTACK.
    // Gating the increment on the same "is there room" check as the write would mean that once
    // a deeply-nested record exceeded the cap, every further pop (endObject) would have no
    // matching push to cancel out - permanently desyncing stackTop for the rest of the document
    // and corrupting every subsequent record boundary. Fields nested deeper than MAXPATHSTACK
    // just won't build a full/matchable path (harmless - nothing we parse lives that deep), but
    // the depth bookkeeping itself must never drift.
    if (keyToPush[0]) {
        if (stackTop < MAXPATHSTACK) strlcpy(pathStack[stackTop], keyToPush, sizeof(pathStack[0]));
        stackTop++;
    }
}

void rejseplanenClient::endObject() {
    if (stackTop > 0) stackTop--;
    if (inTargetArray && stackTop == arrayBaseDepth) {
        if (fetchingDepartures) finaliseDepartureRecord();
        else finaliseCallingStop();
    }
}

void rejseplanenClient::endDocument() {}

//
// Fetches the departure board for a stop and (optionally) the calling points for the first service
//
int rejseplanenClient::fetchDepartures(rdStation *station, stnMessages *messages, const char *stopId, const char *accessId, int numRows, int productsMask, bool fetchCallingPoints, const char *callingStopId, int timeOffsetMins) {

    unsigned long perfTimer = millis();
    bool bChunked = false;
    js->lastResultMessage[0] = '\0';

    xStation->numServices = 0;
    xMessages->numMessages = 0;
    xStation->platformAvailable = false;
    strlcpy(xStation->location, stopId, sizeof(xStation->location));
    for (int i=0;i<MAXBOARDSERVICES;++i) {
        xStation->service[i].sTime[0] = '\0';
        xStation->service[i].destination[0] = '\0';
        xStation->service[i].via[0] = '\0';
        xStation->service[i].origin[0] = '\0';
        xStation->service[i].etd[0] = '\0';
        xStation->service[i].platform[0] = '\0';
        xStation->service[i].opco[0] = '\0';
        xStation->service[i].calling[0] = '\0';
        xStation->service[i].serviceMessage[0] = '\0';
        xStation->service[i].serviceID[0] = '\0';
        xStation->service[i].trainLength = 0;
        xStation->service[i].classesAvailable = 0;
        xStation->service[i].serviceType = 0;
        xStation->service[i].isCancelled = false;
        xStation->service[i].isDelayed = false;
        xStation->service[i].isSTog = false;
    }
    numCallingStops = 0;

    WiFiClientSecure httpsClient;
    httpsClient.setInsecure();
    httpsClient.setTimeout(8000);
    httpsClient.setConnectionTimeout(8000);
    httpsClient.setNoDelay(false);

    int retryCounter = 0;
    while ((!httpsClient.connect(rjHost,443)) && (retryCounter < 10)) {
        delay(100);
        retryCounter++;
    }
    if (retryCounter>=10) {
        strcpy(js->lastResultMessage,"Error: Connect timed out");
        return UPD_NO_RESPONSE;
    }

    String request = String("GET ") + rjDepartureBoardApi + "?accessId=" + String(accessId) + "&format=json&extId=" + String(stopId) + "&duration=120&maxJourneys=" + String(numRows) + "&products=" + String(productsMask);
    if (timeOffsetMins) {
        struct tm nowtime;
        getLocalTime(&nowtime);
        int totalMins = ((nowtime.tm_hour*60 + nowtime.tm_min + timeOffsetMins) % 1440 + 1440) % 1440;
        char timeParam[6];
        sprintf(timeParam,"%02d:%02d",totalMins/60,totalMins%60);
        request += "&time=" + String(timeParam);
    }
    if (callingStopId && callingStopId[0]) request += "&direction=" + String(callingStopId);
    request += " HTTP/1.0\r\nHost: " + String(rjHost) + "\r\nConnection: close\r\n\r\n";

    httpsClient.print(request);

    retryCounter = 0;
    while (!httpsClient.available()) {
        delay(100);
        retryCounter++;
        if (retryCounter>=80) {
            httpsClient.stop();
            strcpy(js->lastResultMessage,"Error: GET timed out");
            return UPD_TIMEOUT;
        }
    }

    String statusLine = httpsClient.readStringUntil('\n');
    if (!statusLine.startsWith("HTTP/") || statusLine.indexOf("200 OK") == -1) {
        httpsClient.stop();
        strlcpy(js->lastResultMessage,statusLine.c_str(),sizeof(js->lastResultMessage));
        if (statusLine.indexOf("401") > 0 || statusLine.indexOf("403") > 0) return UPD_UNAUTHORISED;
        else if (statusLine.indexOf("500") > 0) return UPD_DATA_ERROR;
        else return UPD_HTTP_ERROR;
    }
    unsigned long dataSendTimeout = millis() + 1000UL;
    while ((httpsClient.available() || httpsClient.connected()) && (millis() < dataSendTimeout)) {
        String line = httpsClient.readStringUntil('\n');
        if (line.startsWith("Transfer-Encoding:") && line.indexOf("chunked") >= 0) bChunked = true;
        if (line == "\r") break;
        delay(1);
    }

    JsonStreamingParserGS parser;
    parser.setListener(this);
    parser.reset();
    fetchingDepartures = true;

    long dataReceived = 0;
    char c;
    dataSendTimeout = millis() + 12000UL;
    perfTimer = millis();
    while ((httpsClient.available() || httpsClient.connected()) && (millis() < dataSendTimeout)) {
        while (httpsClient.available()) {
            c = httpsClient.read();
            parser.parse(c);
            dataReceived++;
        }
        delay(5);
    }
    httpsClient.stop();

    if (millis() >= dataSendTimeout) {
        sprintf(js->lastResultMessage,"Error: Timeout after %d bytes",dataReceived);
        return UPD_TIMEOUT;
    }

    if (xStation->numServices == 0 && dataReceived < 20) {
        strcpy(js->lastResultMessage,"Error: Incomplete data");
        return UPD_DATA_ERROR;
    }

    if (xStation->numServices > 1) {
        size_t arraySize = xStation->numServices;
        std::sort(xStation->service, xStation->service+arraySize, compareTimes);
    }

    xStation->platformAvailable = true; // Track/Spor numbers, where present, are shown per-service

    // Work out whether the primary service (or the count of services) has changed since the
    // last load, so the tube-style letbane board knows whether to animate a "new board" scroll.
    boardChanged = false;
    if (xStation->numServices != station->numServices) boardChanged = true;
    else if (xStation->numServices && strcmp(xStation->service[0].destination,station->service[0].destination)) boardChanged = true;

    if (fetchCallingPoints && xStation->numServices && xStation->service[0].serviceID[0]) {
        getServiceDetails(xStation->service[0].serviceID, accessId, stopId);
    }

    UBaseType_t uxHighWaterMark = uxTaskGetStackHighWaterMark(NULL);
    sprintf(js->lastResultMessage+strlen(js->lastResultMessage),"[RP] OK: D:%d T:%d S:%d %s",dataReceived,millis()-perfTimer,uxHighWaterMark,bChunked?"C!":"");
    return UPD_SUCCESS;
}

//
// Fetches the calling points (journeyDetail) for the primary service and builds the
// "Stopper ved: ..." list for whatever stops remain after the requested stop.
//
int rejseplanenClient::getServiceDetails(const char *ref, const char *accessId, const char *stopId) {

    WiFiClientSecure httpsClient;
    httpsClient.setInsecure();
    httpsClient.setTimeout(8000);
    httpsClient.setConnectionTimeout(8000);
    httpsClient.setNoDelay(false);

    int retryCounter = 0;
    while ((!httpsClient.connect(rjHost,443)) && (retryCounter < 10)) {
        delay(100);
        retryCounter++;
    }
    if (retryCounter>=10) return UPD_NO_RESPONSE;

    // URL-encode the ref token (it's built from #, |, spaces and digits/letters only)
    String encodedRef;
    for (size_t i=0; i<strlen(ref); ++i) {
        char ch = ref[i];
        if (isalnum((unsigned char)ch)) encodedRef += ch;
        else {
            char buf[4];
            sprintf(buf,"%%%02X",(unsigned char)ch);
            encodedRef += buf;
        }
    }

    String request = String("GET ") + rjJourneyDetailApi + "?accessId=" + String(accessId) + "&format=json&id=" + encodedRef + " HTTP/1.0\r\nHost: " + String(rjHost) + "\r\nConnection: close\r\n\r\n";
    httpsClient.print(request);

    retryCounter = 0;
    while (!httpsClient.available()) {
        delay(100);
        retryCounter++;
        if (retryCounter>=80) {
            httpsClient.stop();
            return UPD_TIMEOUT;
        }
    }

    String statusLine = httpsClient.readStringUntil('\n');
    if (!statusLine.startsWith("HTTP/") || statusLine.indexOf("200 OK") == -1) {
        httpsClient.stop();
        return UPD_HTTP_ERROR;
    }
    unsigned long dataSendTimeout = millis() + 1000UL;
    while ((httpsClient.available() || httpsClient.connected()) && (millis() < dataSendTimeout)) {
        String line = httpsClient.readStringUntil('\n');
        if (line == "\r") break;
        delay(1);
    }

    JsonStreamingParserGS parser;
    parser.setListener(this);
    parser.reset();
    fetchingDepartures = false;

    char c;
    dataSendTimeout = millis() + 12000UL;
    while ((httpsClient.available() || httpsClient.connected()) && (millis() < dataSendTimeout)) {
        while (httpsClient.available()) {
            c = httpsClient.read();
            parser.parse(c);
        }
        delay(5);
    }
    httpsClient.stop();

    if (millis() >= dataSendTimeout || numCallingStops == 0) return UPD_TIMEOUT;

    // Find the requested stop in the route, and build the "calling at" list from what follows it
    int matchIdx = -1;
    for (int i=0;i<numCallingStops;i++) {
        if (strcmp(callingStops[i].extId, stopId)==0) { matchIdx = i; break; }
    }

    xStation->service[0].calling[0] = '\0';
    xStation->service[0].origin[0] = '\0';

    // S-tog services can call at a dozen-plus stops, and with a "(HH:MM)" on every one the
    // "Stopper ved" list gets long enough to feel endless - drop the times there and just list stop
    // names (the board still shows the S-tog line's own minute countdown separately). Not tied to
    // any specific station - any S-tog service benefits, whether reached via a mixed DK Rail board
    // at København H or the dedicated S-tog mode at any other S-tog-served stop.
    bool omitCallingTimes = xStation->service[0].isSTog;

    if (matchIdx >= 0) {
        if (matchIdx > 0) strlcpy(xStation->service[0].origin, callingStops[0].name, sizeof(xStation->service[0].origin));
        for (int i=matchIdx+1; i<numCallingStops; i++) {
            // "Stop name (HH:MM)" - matches the UK rail board's calling-point format
            char entry[MAXLOCATIONSIZE+10];
            if (!omitCallingTimes && callingStops[i].time[0]) sprintf(entry,"%s (%s)",callingStops[i].name,callingStops[i].time);
            else strlcpy(entry,callingStops[i].name,sizeof(entry));

            size_t curLen = strlen(xStation->service[0].calling);
            size_t addLen = strlen(entry) + (curLen ? 2 : 0);
            if (curLen + addLen >= sizeof(xStation->service[0].calling)) break;
            if (curLen) strcat(xStation->service[0].calling, ", ");
            strcat(xStation->service[0].calling, entry);
        }
    }

    return UPD_SUCCESS;
}

void rejseplanenClient::loadDepartures(rdStation *station, stnMessages *messages) {
    station->boardChanged = boardChanged;
    messages->numMessages = xMessages->numMessages;
    station->numServices = xStation->numServices;
    strlcpy(station->location, xStation->location, sizeof(station->location));
    station->platformAvailable = xStation->platformAvailable;
    for (int i=0;i<xMessages->numMessages;++i) strlcpy(messages->messages[i], xMessages->messages[i], sizeof(messages->messages[0]));
    for (int i=0;i<xStation->numServices;++i) {
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
    }
    if (xStation->numServices) {
        strlcpy(station->calling, xStation->service[0].calling, sizeof(station->calling));
        strlcpy(station->origin, xStation->service[0].origin, sizeof(station->origin));
        strlcpy(station->serviceMessage, xStation->service[0].serviceMessage, sizeof(station->serviceMessage));
    }
}
