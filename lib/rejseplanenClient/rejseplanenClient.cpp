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
    if (minute1 != minute2) return minute1 < minute2;
    // Two services scheduled in the exact same minute (common on a busy S-tog board with several
    // lines converging) tie here, and std::sort isn't guaranteed stable - Rejseplanen doesn't always
    // return them in the same relative order between one fetch and the next either, so without a
    // deterministic tie-breaker, which one lands in position 0 could flip from fetch to fetch with
    // nothing actually having changed. Destination name is always present and gives a stable order.
    return strcmp(a.destination, b.destination) < 0;
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
    convertDanishToLatin1(svc.via, sizeof(svc.via));
    strlcpy(svc.opco, raw.opco, sizeof(svc.opco));
    convertDanishToLatin1(svc.opco, sizeof(svc.opco));
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

// Called when a stopLocationOrCoordLocation[]/StopLocation record's closing '}' is seen while
// searching. Deliberately does NOT run the result through convertDanishToLatin1() - unlike every
// other place that helper is used, this text is going into a JSON response for the browser (which
// wants UTF-8, the encoding Rejseplanen already sent it in), not onto the board's own Latin-1-only
// custom font.
void rejseplanenClient::finaliseStopSearchResult() {
    if (!stopSearchName[0] || !stopSearchExtId[0]) return;
    if (stopSearchCount >= stopSearchMax) return;
    if (stopSearchCount) stopSearchResult += ",";
    stopSearchResult += "{\"name\":\"";
    for (size_t i=0; stopSearchName[i]; i++) {
        char c = stopSearchName[i];
        if (c=='"' || c=='\\') stopSearchResult += '\\';
        stopSearchResult += c;
    }
    stopSearchResult += "\",\"id\":\"";
    stopSearchResult += stopSearchExtId;
    stopSearchResult += "\"}";
    stopSearchCount++;
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
    if (searchingStops) {
        if (strcmp(currentPath,"stopLocationOrCoordLocation/StopLocation/name")==0) strlcpy(stopSearchName,value,sizeof(stopSearchName));
        else if (strcmp(currentPath,"stopLocationOrCoordLocation/StopLocation/extId")==0) strlcpy(stopSearchExtId,value,sizeof(stopSearchExtId));
        return;
    }
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
        if (searchingStops) {
            isTarget = (strcmp(pendingKey,"stopLocationOrCoordLocation")==0);
        } else if (fetchingDepartures) {
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
        if (searchingStops) { stopSearchName[0] = '\0'; stopSearchExtId[0] = '\0'; }
        else if (fetchingDepartures) resetRawRecord();
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
        if (searchingStops) finaliseStopSearchResult();
        else if (fetchingDepartures) finaliseDepartureRecord();
        else finaliseCallingStop();
    }
}

void rejseplanenClient::endDocument() {}

// See declaration comment in rejseplanenClient.h.
int rejseplanenClient::readResponseHeaders(WiFiClientSecure &client, long &contentLength, bool &serverAllowsReuse, bool &chunked) {
    contentLength = -1;
    serverAllowsReuse = true;
    chunked = false;

    int retryCounter = 0;
    while (!client.available()) {
        delay(100);
        retryCounter++;
        if (retryCounter>=80) return UPD_TIMEOUT;
    }

    String statusLine = client.readStringUntil('\n');
    if (!statusLine.startsWith("HTTP/") || statusLine.indexOf("200 OK") == -1) {
        if (statusLine.indexOf("401") > 0 || statusLine.indexOf("403") > 0) return UPD_UNAUTHORISED;
        else if (statusLine.indexOf("500") > 0) return UPD_DATA_ERROR;
        else return UPD_HTTP_ERROR;
    }

    unsigned long headerDeadline = millis() + 1000UL;
    while ((client.available() || client.connected()) && (millis() < headerDeadline)) {
        String line = client.readStringUntil('\n');
        if (line.startsWith("Content-Length:")) contentLength = line.substring(16).toInt();
        else if (line.startsWith("Transfer-Encoding:") && line.indexOf("chunked") >= 0) chunked = true;
        else if (line.startsWith("Connection:") && line.indexOf("close") >= 0) serverAllowsReuse = false;
        if (line == "\r") break;
        delay(1);
    }
    return UPD_SUCCESS;
}

// See declaration comment in rejseplanenClient.h.
long rejseplanenClient::readResponseBody(WiFiClientSecure &client, long contentLength, JsonStreamingParserGS &parser, unsigned long timeoutMs, bool &timedOut) {
    timedOut = false;
    long received = 0;
    uint8_t chunk[512];
    unsigned long deadline = millis() + timeoutMs;

    if (contentLength >= 0) {
        // Known body length (the normal, keep-alive-eligible case) - read exactly that many bytes and
        // stop, leaving whatever the server sends next (if anything) untouched for the next request.
        while (received < contentLength) {
            if (millis() >= deadline) { timedOut = true; break; }
            int avail = client.available();
            if (avail > 0) {
                long want = contentLength - received;
                if (want > (long)sizeof(chunk)) want = sizeof(chunk);
                if (avail > (int)want) avail = (int)want;
                int n = client.read(chunk, avail);
                for (int i=0;i<n;i++) parser.parse((char)chunk[i]);
                received += n;
            } else if (!client.connected()) {
                timedOut = true; // connection dropped before delivering everything promised
                break;
            } else {
                delay(5);
            }
        }
    } else {
        // No Content-Length header seen - fall back to the pre-keep-alive behaviour of reading until
        // the connection itself closes. The caller must not try to reuse the connection afterward.
        while (client.available() || client.connected()) {
            if (millis() >= deadline) { timedOut = true; break; }
            int avail = client.available();
            if (avail > 0) {
                int n = client.read(chunk, avail > (int)sizeof(chunk) ? (int)sizeof(chunk) : avail);
                for (int i=0;i<n;i++) parser.parse((char)chunk[i]);
                received += n;
            } else {
                delay(5);
            }
        }
    }
    return received;
}

//
// Fetches the departure board for a stop and (optionally) the calling points for the first service
//
int rejseplanenClient::fetchDepartures(rdStation *station, stnMessages *messages, const char *stopId, const char *accessId, int numRows, int productsMask, bool fetchCallingPoints, const char *callingStopId, int timeOffsetMins, bool useLineDirCache) {

    unsigned long perfTimer = millis();
    bool bChunked = false;
    js->lastResultMessage[0] = '\0';

    // Lazily allocate the line+direction calling-at cache on the heap the first time it's actually
    // needed (see its own declaration comment for why heap, not a static array) - a board that
    // never uses S-tog mode never pays this ~15KB cost at all.
    if (useLineDirCache && !lineDirCache) {
        lineDirCache = new LineDirCallingEntry[MAXLINEDIRCACHE];
        lineDirCacheCount = 0;
        lineDirCacheBuiltAt = millis();
    }
    // Rebuild the line+direction calling-at cache from scratch once a day, so a genuine schedule
    // change eventually gets picked up without needing a reboot - see LineDirCallingEntry's own
    // comment. lineDirCacheBuiltAt starts at 0 alongside an empty cache, which is already the
    // correct "just built" state, so this deliberately doesn't fire on the very first fetch after
    // boot (millis() this soon after boot is always far less than a day).
    if (useLineDirCache && (millis() - lineDirCacheBuiltAt >= LINEDIRCACHE_MAX_AGE_MS)) {
        lineDirCacheCount = 0;
        lineDirCacheBuiltAt = millis();
    }

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

    // Kept open across this departureBoard request AND (when fetchCallingPoints ends up needing a
    // real fetch rather than a cache hit) the getServiceDetails() call(s) below, via HTTP keep-alive -
    // confirmed live against the real server (curl showed it answering "Connection: keep-alive" and
    // genuinely reusing one TCP connection for a second request). Previously each of up to 3 requests
    // per fetch cycle (this one + up to 2x calling-at) opened its own fresh TCP+TLS connection from
    // scratch. That cost barely mattered for Tog, where the calling-at cache-hit check below skips
    // the extra requests entirely on almost every cycle (a service usually stays primary for many
    // refreshes) - but S-tog's services turn over every few minutes, so it was needing BOTH extra
    // connections on nearly every cycle, each an independent full-handshake failure point. Reusing
    // one connection cuts that back down to a single handshake per cycle either way.
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
    request += " HTTP/1.1\r\nHost: " + String(rjHost) + "\r\nConnection: keep-alive\r\n\r\n";

    httpsClient.print(request);

    long contentLength = -1;
    bool serverAllowsReuse = true;
    int headerResult = readResponseHeaders(httpsClient, contentLength, serverAllowsReuse, bChunked);
    if (headerResult != UPD_SUCCESS) {
        httpsClient.stop();
        if (headerResult == UPD_TIMEOUT) strcpy(js->lastResultMessage,"Error: GET timed out");
        else sprintf(js->lastResultMessage,"Error: HTTP status %d",headerResult);
        return headerResult;
    }

    JsonStreamingParserGS parser;
    parser.setListener(this);
    parser.reset();
    fetchingDepartures = true;

    perfTimer = millis();
    bool bodyTimedOut = false;
    long dataReceived = readResponseBody(httpsClient, contentLength, parser, 12000UL, bodyTimedOut);

    // Only safe to hand this same connection to getServiceDetails() below when the server didn't ask
    // to close it, the body actually finished (not abandoned mid-read), and the socket's still up -
    // readResponseBody() already stopped tracking as "reusable" the moment any of those went wrong.
    bool canReuseConnection = serverAllowsReuse && !bodyTimedOut && contentLength >= 0 && httpsClient.connected();
    if (!canReuseConnection) httpsClient.stop();

    if (bodyTimedOut) {
        sprintf(js->lastResultMessage,"Error: Timeout after %ld bytes",dataReceived);
        return UPD_TIMEOUT;
    }

    if (xStation->numServices == 0 && dataReceived < 20) {
        httpsClient.stop();
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

    // The calling-at list only actually changes when the primary service itself changes, but this
    // used to call getServiceDetails() (a second, full HTTP request) on every single refresh
    // regardless - doubling the API call count for no benefit on the many cycles where the same
    // service is still sitting at the top of the board. When it matches, just carry the previous
    // load's calling/origin forward instead of spending a real call to re-fetch text that hasn't
    // changed.
    // station->calling is a station-level field ("only the first service returned"), not tied to any
    // particular service - so it's only trustworthy to reuse when it's known to actually describe
    // the CURRENT service[0]. The main sketch's departed-train animation promotes the next service
    // into position 0 locally (no fetch involved) and clears station->calling when it does, precisely
    // so this check can tell "same primary as last real fetch" apart from "same primary as whatever
    // was just promoted locally, whose calling-at was never actually fetched" - requiring it non-empty
    // catches that case and forces a real fetch instead of carrying the departed service's stops
    // forward onto the one that replaced it.
    // serviceID (the JourneyDetailRef.ref token) is included alongside sTime+destination because
    // those two alone aren't a reliably unique fingerprint - two genuinely different services can
    // share a scheduled time and destination (a common round time, a shared hub like København H
    // reached by several lines), and a stale station->service[0] left over from switching to a
    // different location entirely (scheduler/carousel) could coincidentally match too. serviceID is
    // Rejseplanen's own unique identifier for a specific journey, so requiring it as well can only
    // make this check stricter (fewer accidental "same service" matches, never more) - the failure
    // mode of getting this wrong was a real one: calling-at text visibly left over from a different,
    // no longer relevant departure.
    // useLineDirCache (S-tog only - see LineDirCallingEntry's own comment) takes priority over the
    // sTime/destination/serviceID cache-hit check below: a line+direction cache hit is just as valid
    // regardless of whether THIS EXACT trip was also primary last cycle, and covers the much more
    // common case for S-tog (a brand new trip, never seen as primary before, but on a line+direction
    // already known from an earlier departure) that the check below can never catch on its own.
    int lineDirIdx = useLineDirCache && xStation->numServices ? findLineDirCacheEntry(xStation->service[0].via, xStation->service[0].destination) : -1;
    bool samePrimaryService = !useLineDirCache && fetchCallingPoints && xStation->numServices && station->numServices &&
        station->callingKnown &&
        strcmp(xStation->service[0].sTime,station->service[0].sTime)==0 &&
        strcmp(xStation->service[0].destination,station->service[0].destination)==0 &&
        strcmp(xStation->service[0].serviceID,station->service[0].serviceID)==0;
    if (lineDirIdx >= 0) {
        strlcpy(xStation->service[0].calling, lineDirCache[lineDirIdx].calling, sizeof(xStation->service[0].calling));
        strlcpy(xStation->service[0].origin, lineDirCache[lineDirIdx].origin, sizeof(xStation->service[0].origin));
        callingFetchKnown = true;
    } else if (samePrimaryService) {
        strlcpy(xStation->service[0].calling, station->calling, sizeof(xStation->service[0].calling));
        strlcpy(xStation->service[0].origin, station->origin, sizeof(xStation->service[0].origin));
        callingFetchKnown = true;
    } else if (fetchCallingPoints && xStation->numServices && xStation->service[0].serviceID[0]) {
        // The overall board fetch above already succeeded (that's how we got here), so this being
        // slow/failing shouldn't fail the whole board - the board just displays with a blank
        // calling-at line until it succeeds. getServiceDetails() logs its own compact timing/result
        // into js->lastResultMessage regardless of outcome, so a failure here is visible via /info
        // instead of being entirely silent. callingFetchKnown only becomes true on an actual success -
        // loadDepartures() uses that (not calling[0]/origin[0] being non-empty) to tell "we know this
        // service has no further calling points" apart from "we don't know yet, the fetch failed" -
        // see rdStation::callingKnown's own comment for why that distinction matters.
        callingFetchKnown = (getServiceDetails(httpsClient, xStation->service[0].serviceID, accessId, stopId, 0) == UPD_SUCCESS);
        if (callingFetchKnown && useLineDirCache) storeLineDirCacheEntry(xStation->service[0].via, xStation->service[0].destination, xStation->service[0].calling, xStation->service[0].origin);
    } else {
        callingFetchKnown = false;
    }

    // Pre-fetch calling-at for whatever's sitting in position [1] too, the same way as above but one
    // slot over - by the time it's eventually promoted into position [0] (the current primary having
    // departed), it's usually already known instead of racing a fresh fetch against the ~3.5s
    // departed-train animation window. This does NOT raise the steady-state fetch rate: every service
    // that ever reaches position [0] already sat in position [1] first, so it's the same one fetch
    // per service - just made a cycle (or more) earlier, while there's still real lead time, instead
    // of urgently right after the promotion that needs it. See rdStation::nextCalling and the
    // promotion code in the main sketch for how this gets consumed.
    if (fetchCallingPoints && xStation->numServices>1) {
        // Same line+direction-cache-first priority as position [0] above - see its own comment.
        int lineDirIdx1 = useLineDirCache ? findLineDirCacheEntry(xStation->service[1].via, xStation->service[1].destination) : -1;
        // serviceID included here for the same reason as samePrimaryService above - sTime+
        // destination alone isn't a reliably unique fingerprint.
        bool sameNext = !useLineDirCache && station->numServices>1 && station->nextCallingKnown &&
            strcmp(xStation->service[1].sTime,station->service[1].sTime)==0 &&
            strcmp(xStation->service[1].destination,station->service[1].destination)==0 &&
            strcmp(xStation->service[1].serviceID,station->service[1].serviceID)==0;
        if (lineDirIdx1 >= 0) {
            strlcpy(xStation->service[1].calling, lineDirCache[lineDirIdx1].calling, sizeof(xStation->service[1].calling));
            strlcpy(xStation->service[1].origin, lineDirCache[lineDirIdx1].origin, sizeof(xStation->service[1].origin));
            nextCallingFetchKnown = true;
        } else if (sameNext) {
            strlcpy(xStation->service[1].calling, station->nextCalling, sizeof(xStation->service[1].calling));
            strlcpy(xStation->service[1].origin, station->nextOrigin, sizeof(xStation->service[1].origin));
            nextCallingFetchKnown = true;
        } else if (xStation->service[1].serviceID[0]) {
            nextCallingFetchKnown = (getServiceDetails(httpsClient, xStation->service[1].serviceID, accessId, stopId, 1) == UPD_SUCCESS);
            if (nextCallingFetchKnown && useLineDirCache) storeLineDirCacheEntry(xStation->service[1].via, xStation->service[1].destination, xStation->service[1].calling, xStation->service[1].origin);
        } else {
            nextCallingFetchKnown = false;
        }
    } else {
        nextCallingFetchKnown = false;
    }

    // Whatever's left of the connection (still open if the last getServiceDetails() call above left
    // it reusable) is done being useful for this cycle - release it now rather than holding a socket
    // open between fetchDepartures() invocations (~45-90s apart) for no benefit.
    httpsClient.stop();

    UBaseType_t uxHighWaterMark = uxTaskGetStackHighWaterMark(NULL);
    sprintf(js->lastResultMessage+strlen(js->lastResultMessage),"[RP] OK: D:%d T:%d S:%d %s",dataReceived,millis()-perfTimer,uxHighWaterMark,bChunked?"C!":"");
    return UPD_SUCCESS;
}

//
// Fetches the calling points (journeyDetail) for the primary service and builds the
// "Stopper ved: ..." list for whatever stops remain after the requested stop.
//
int rejseplanenClient::getServiceDetails(WiFiClientSecure &httpsClient, const char *ref, const char *accessId, const char *stopId, int targetIdx) {
    // Live-tested this against the real API: response sizes (18-34KB) and this machine's own
    // transfer time (<0.3s) didn't point at either being the bottleneck on their own, which means
    // whatever's actually slow is specific to the ESP32's own connect/TLS/parse path - something
    // that can't be measured from here. Timing each phase and surfacing it via /info (js-
    // >lastResultMessage) means the NEXT check has real numbers from the actual hardware instead of
    // another guess. Kept deliberately compact (js->lastResultMessage is only 80 bytes and the
    // caller in fetchDepartures() appends its own summary after this).
    unsigned long tStart = millis();

    // httpsClient normally arrives here already connected - fetchDepartures() hands over the same
    // keep-alive connection it just used for the departureBoard request (see its own comment for
    // why). Only reconnect from scratch if that didn't pan out (server closed it, a previous call on
    // it timed out, or - for the position-1 pre-fetch - the position-0 call above already used and
    // released it): this makes reuse a pure bonus, never a new failure mode of its own.
    if (!httpsClient.connected()) {
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
            sprintf(js->lastResultMessage+strlen(js->lastResultMessage),"CD:conn-fail %lums ",millis()-tStart);
            return UPD_NO_RESPONSE;
        }
    }
    unsigned long tConnected = millis();

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

    String request = String("GET ") + rjJourneyDetailApi + "?accessId=" + String(accessId) + "&format=json&id=" + encodedRef + " HTTP/1.1\r\nHost: " + String(rjHost) + "\r\nConnection: keep-alive\r\n\r\n";
    httpsClient.print(request);

    long contentLength = -1;
    bool serverAllowsReuse = true;
    bool chunkedUnused = false;
    int headerResult = readResponseHeaders(httpsClient, contentLength, serverAllowsReuse, chunkedUnused);
    unsigned long tHeaders = millis();
    if (headerResult != UPD_SUCCESS) {
        httpsClient.stop();
        sprintf(js->lastResultMessage+strlen(js->lastResultMessage),"CD:conn%lu %s %lums ",tConnected-tStart,headerResult==UPD_TIMEOUT?"resp-fail":"http-err",millis()-tStart);
        return headerResult;
    }

    JsonStreamingParserGS parser;
    parser.setListener(this);
    parser.reset();
    fetchingDepartures = false;
    // callingStops[]/numCallingStops is scratch space shared across BOTH calls this function can make
    // in one fetchDepartures() cycle (targetIdx 0, then 1 for the position-1 pre-fetch) - fetchDepartures()
    // only zeroes numCallingStops once, before either call. Without resetting it here too, the second
    // call would append its stops after whatever the first call left behind instead of starting clean,
    // and the match-the-requested-stop search below (which breaks on the FIRST extId match) could then
    // land inside the FIRST call's leftover stops - building the "calling at" list from the wrong
    // service's route entirely. Resetting per-call keeps each fetch's result scoped to its own journey.
    numCallingStops = 0;

    // S-tog stops at every local station, so its journeyDetail responses run far larger than a
    // typical intercity Tog service's (a real København H S-tog journey measured here came to
    // 32KB across 27 stops, each with its own Notes block). Reading a single byte per
    // WiFiClientSecure::read() call, as this used to, means ~32000 individual TLS/socket calls for
    // a response that size - real, measurable overhead on top of the parser's own per-byte work, and
    // the actual reason this didn't reliably fit the old 12s window. Reading in chunks instead cuts
    // that network-call overhead by ~500x (the parser itself is still fed byte-by-byte - it's a
    // streaming state machine, that part is inherent) - genuinely faster, not just given more time
    // to be slow. Response size/transfer time tested fine from a normal connection (18-34KB,
    // <0.3s), so whatever's actually slow is specific to the ESP32's own connect/parse path -
    // raising this further to 35s as a safe hedge while the timing log below (js->lastResultMessage,
    // see /info) gathers real numbers from the actual hardware.
    bool bodyTimedOut = false;
    long bytesReceived = readResponseBody(httpsClient, contentLength, parser, 35000UL, bodyTimedOut);
    unsigned long tDone = millis();

    // Same reuse conditions as fetchDepartures() - only leave the connection open for a possible
    // follow-up call (the position-1 pre-fetch, reusing what position-0 just used) when the server
    // allowed it, the body genuinely finished, and the socket's still actually up.
    bool canReuseConnection = serverAllowsReuse && !bodyTimedOut && contentLength >= 0 && httpsClient.connected();
    if (!canReuseConnection) httpsClient.stop();

    if (bodyTimedOut || numCallingStops == 0) {
        sprintf(js->lastResultMessage+strlen(js->lastResultMessage),"CD:c%luh%lup%lu B%ld stops%d TIMEOUT ",tConnected-tStart,tHeaders-tConnected,tDone-tHeaders,bytesReceived,numCallingStops);
        return UPD_TIMEOUT;
    }
    sprintf(js->lastResultMessage+strlen(js->lastResultMessage),"CD:c%luh%lup%lu B%ld ",tConnected-tStart,tHeaders-tConnected,tDone-tHeaders,bytesReceived);

    // Find the requested stop in the route, and build the "calling at" list from what follows it
    int matchIdx = -1;
    for (int i=0;i<numCallingStops;i++) {
        if (strcmp(callingStops[i].extId, stopId)==0) { matchIdx = i; break; }
    }

    xStation->service[targetIdx].calling[0] = '\0';
    xStation->service[targetIdx].origin[0] = '\0';

    // S-tog services can call at a dozen-plus stops, and with a "(HH:MM)" on every one the
    // "Stopper ved" list gets long enough to feel endless - drop the times there and just list stop
    // names (the board still shows the S-tog line's own minute countdown separately). Not tied to
    // any specific station - any S-tog service benefits, whether reached via a mixed DK Rail board
    // at København H or the dedicated S-tog mode at any other S-tog-served stop.
    bool omitCallingTimes = xStation->service[targetIdx].isSTog;

    if (matchIdx >= 0) {
        if (matchIdx > 0) strlcpy(xStation->service[targetIdx].origin, callingStops[0].name, sizeof(xStation->service[targetIdx].origin));
        for (int i=matchIdx+1; i<numCallingStops; i++) {
            // "Stop name (HH:MM)" - matches the UK rail board's calling-point format
            char entry[MAXLOCATIONSIZE+10];
            if (!omitCallingTimes && callingStops[i].time[0]) sprintf(entry,"%s (%s)",callingStops[i].name,callingStops[i].time);
            else strlcpy(entry,callingStops[i].name,sizeof(entry));

            size_t curLen = strlen(xStation->service[targetIdx].calling);
            size_t addLen = strlen(entry) + (curLen ? 2 : 0);
            if (curLen + addLen >= sizeof(xStation->service[targetIdx].calling)) break;
            if (curLen) strcat(xStation->service[targetIdx].calling, ", ");
            strcat(xStation->service[targetIdx].calling, entry);
        }
    }

    return UPD_SUCCESS;
}

// See declaration comment in rejseplanenClient.h.
int rejseplanenClient::findLineDirCacheEntry(const char *line, const char *destination) {
    // lineDirCache is only allocated once useLineDirCache is actually used (see fetchDepartures())
    // - null here means nothing's been cached yet (or S-tog mode has never fetched at all), not an
    // error. lineDirCacheCount should already be 0 whenever this is null, but check the pointer
    // directly too rather than relying on that staying true forever.
    if (!lineDirCache || !line[0] || !destination[0]) return -1;
    for (int i=0; i<lineDirCacheCount; i++) {
        if (strcmp(lineDirCache[i].line,line)==0 && strcmp(lineDirCache[i].destination,destination)==0) return i;
    }
    return -1;
}

// See declaration comment in rejseplanenClient.h.
void rejseplanenClient::storeLineDirCacheEntry(const char *line, const char *destination, const char *calling, const char *origin) {
    if (!lineDirCache || !line[0] || !destination[0]) return;
    int idx = findLineDirCacheEntry(line, destination);
    if (idx < 0) {
        // Full is extremely unlikely (Copenhagen S-tog has ~14 line+direction combos against this
        // cache's 24 slots) - if it ever does happen, just stop growing rather than overflow;
        // whichever combos got cached first keep working, the rest fall back to a real fetch each
        // time, same as before this cache existed.
        if (lineDirCacheCount >= MAXLINEDIRCACHE) return;
        idx = lineDirCacheCount++;
        strlcpy(lineDirCache[idx].line, line, sizeof(lineDirCache[idx].line));
        strlcpy(lineDirCache[idx].destination, destination, sizeof(lineDirCache[idx].destination));
    }
    strlcpy(lineDirCache[idx].calling, calling, sizeof(lineDirCache[idx].calling));
    strlcpy(lineDirCache[idx].origin, origin, sizeof(lineDirCache[idx].origin));
}

// See declaration comment in rejseplanenClient.h.
bool rejseplanenClient::lookupCachedCalling(const char *line, const char *destination, char *callingOut, size_t callingOutSize, char *originOut, size_t originOutSize) {
    int idx = findLineDirCacheEntry(line, destination);
    if (idx < 0) return false;
    strlcpy(callingOut, lineDirCache[idx].calling, callingOutSize);
    strlcpy(originOut, lineDirCache[idx].origin, originOutSize);
    return true;
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
        strlcpy(station->service[i].serviceID, xStation->service[i].serviceID, sizeof(station->service[0].serviceID));
    }
    if (xStation->numServices) {
        strlcpy(station->calling, xStation->service[0].calling, sizeof(station->calling));
        strlcpy(station->origin, xStation->service[0].origin, sizeof(station->origin));
        strlcpy(station->serviceMessage, xStation->service[0].serviceMessage, sizeof(station->serviceMessage));
        station->callingKnown = callingFetchKnown;
    } else {
        station->callingKnown = false;
    }
    if (xStation->numServices>1) {
        strlcpy(station->nextCalling, xStation->service[1].calling, sizeof(station->nextCalling));
        strlcpy(station->nextOrigin, xStation->service[1].origin, sizeof(station->nextOrigin));
        station->nextCallingKnown = nextCallingFetchKnown;
    } else {
        station->nextCallingKnown = false;
    }
}

//
// Searches Rejseplanen stops by name - called synchronously from the web server's own request
// handler (handleDkStationPicker() in "Departures Board.cpp"), same pattern as the National Rail
// station picker proxy, just with real JSON parsing here (rather than a raw passthrough) since a
// location.name response embeds each stop's full product list and can run to several KB per match -
// far more than a name+id typeahead needs to carry over WiFi to the browser.
//
String rejseplanenClient::searchStops(const char *query, const char *accessId, int maxResults) {
    stopSearchResult = "[";
    stopSearchCount = 0;
    stopSearchMax = maxResults;
    stopSearchName[0] = '\0';
    stopSearchExtId[0] = '\0';

    WiFiClientSecure httpsClient;
    httpsClient.setInsecure();
    httpsClient.setTimeout(6000);
    httpsClient.setConnectionTimeout(6000);
    httpsClient.setNoDelay(false);

    int retryCounter = 0;
    while ((!httpsClient.connect(rjHost,443)) && (retryCounter < 5)) {
        delay(100);
        retryCounter++;
    }
    if (retryCounter>=5) return "[]";

    // URL-encode the query (Danish letters, spaces, etc.)
    String encodedQuery;
    for (size_t i=0; i<strlen(query); ++i) {
        unsigned char ch = (unsigned char)query[i];
        if (isalnum(ch)) encodedQuery += (char)ch;
        else {
            char buf[4];
            sprintf(buf,"%%%02X",ch);
            encodedQuery += buf;
        }
    }

    String request = String("GET ") + rjLocationNameApi + "?accessId=" + String(accessId) + "&format=json&input=" + encodedQuery + " HTTP/1.0\r\nHost: " + String(rjHost) + "\r\nConnection: close\r\n\r\n";
    httpsClient.print(request);

    retryCounter = 0;
    while (!httpsClient.available()) {
        delay(100);
        retryCounter++;
        if (retryCounter>=60) { httpsClient.stop(); return "[]"; }
    }

    String statusLine = httpsClient.readStringUntil('\n');
    if (!statusLine.startsWith("HTTP/") || statusLine.indexOf("200 OK") == -1) {
        httpsClient.stop();
        return "[]";
    }
    unsigned long headerTimeout = millis() + 1000UL;
    while ((httpsClient.available() || httpsClient.connected()) && millis() < headerTimeout) {
        String line = httpsClient.readStringUntil('\n');
        if (line == "\r") break;
    }

    JsonStreamingParserGS parser;
    parser.setListener(this);
    parser.reset();
    searchingStops = true;
    fetchingDepartures = false;
    stackTop = 0;
    pendingKey[0] = '\0';
    currentPath[0] = '\0';
    targetElementKey[0] = '\0';
    inTargetArray = false;
    arrayDepth = 0;
    arrayBaseDepth = -1;
    targetArrayDepth = -1;

    char c;
    unsigned long dataTimeout = millis() + 8000UL;
    while ((httpsClient.available() || httpsClient.connected()) && millis() < dataTimeout && stopSearchCount < stopSearchMax) {
        while (httpsClient.available() && stopSearchCount < stopSearchMax) {
            c = httpsClient.read();
            parser.parse(c);
        }
        delay(2);
    }
    httpsClient.stop();
    searchingStops = false;

    stopSearchResult += "]";
    return stopSearchResult;
}
