/*
 * Departures Board (c) 2025-2026 Gadec Software
 *
 * trafiklabClient Library
 *
 * Swedish public transport via the Trafiklab Realtime APIs (https://www.trafiklab.se) - currently the
 * Stockholm tunnelbana (METRO) board: departures, the stops each train calls at (Trafiklab Trips), and
 * a stop-name search for the web config.
 *
 * https://github.com/gadec-uk/departures-board
 *
 * This work is licensed under Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International.
 * To view a copy of this license, visit https://creativecommons.org/licenses/by-nc-sa/4.0/
 */

#pragma once
#include "JsonListenerGS.h"
#include "JsonStreamingParserGS.h"
#include <sharedDataStructs.h>
#include <responseCodes.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

#define TL_MAXPATHDEPTH 8
#define TL_NAMESIZE 20

// Floor on the largest free heap block below which a new HTTPS connection isn't even attempted - same
// reasoning (and number) as MIN_SAFE_HEAP_FOR_FETCH in rejseplanenClient.h.
#define TL_MIN_SAFE_HEAP 20000

// Calling-at cache: a tunnelbana line's stop sequence is a property of line + direction (the board's own
// stop is fixed), so once one trip has been looked up the text is reused for every later departure on that
// line and direction - the same idea as rejseplanenClient's line+direction cache for S-tog, just smaller
// (Stockholm's seven lines have 14 line+direction combinations at the busiest stop).
#define TL_CACHE_ENTRIES 14
#define TL_CACHE_LINESIZE 5
#define TL_CACHE_DESTSIZE 36
#define TL_CACHE_CALLINGSIZE 300
#define TL_CACHE_MAX_AGE_MS 86400000UL

class trafiklabClient: public JsonListenerGS {

    private:

        struct tlFrame {
            char name[TL_NAMESIZE];
            bool isArray;
        };

        struct tlRawDeparture {
            char scheduled[20];       // "2026-10-09T22:35:00"
            char realtime[20];
            char line[8];             // route.designation, e.g. "18"
            char mode[8];             // route.transport_mode, e.g. "METRO"
            char direction[44];
            char destName[44];
            char tripId[24];
            char startDate[12];
            char stopName[44];
            char platform[8];
            char rtPlatform[8];
            char opco[40];
            bool cancelled;
        };

        struct tlCacheEntry {
            char line[TL_CACHE_LINESIZE];
            char destination[TL_CACHE_DESTSIZE];
            char calling[TL_CACHE_CALLINGSIZE];
        };

        enum tlParseMode { TL_PARSE_DEPARTURES, TL_PARSE_TRIP, TL_PARSE_SEARCH };

        const char* tlHost = "realtime-api.trafiklab.se";

        rdiStation* xStation = nullptr;
        stnMessages* xMessages = nullptr;
        sharedBufferSpace* js = nullptr;

        // Path tracking: every open object/array pushes a frame (array elements get an empty name), so a
        // field's path is the joined, non-empty names - e.g. "departures.route.designation".
        tlFrame frames[TL_MAXPATHDEPTH];
        int lvl = 0;
        char pendingKey[TL_NAMESIZE];
        char path[80];
        void buildPath();

        tlParseMode parseMode = TL_PARSE_DEPARTURES;
        bool enough = false;           // parsing can stop early (the rows asked for are in)
        bool sawDeparturesArray = false;
        bool metroOnlyFilter = false;
        int maxRows = 0;

        tlRawDeparture raw;
        void resetRaw();
        void finaliseDeparture();

        // Trip (stop list) parsing
        char tripBoardAreaId[12];
        char tripBoardStopName[44];
        bool tripSeenBoardStop = false;
        bool tripAnyCall = false;
        char tripCallStopName[44];
        char tripCallAreaId[12];
        char tripCalling[TL_CACHE_CALLINGSIZE];
        void finaliseTripCall();

        // Stop search parsing
        char searchId[12];
        char searchName[44];
        char searchAreaType[16];
        bool searchHasMode = false;
        const char* searchRequireMode = nullptr;
        String searchResult;
        int searchCount = 0;
        int searchMax = 8;
        void finaliseSearchGroup();

        // Cache (heap, nothrow, lazily allocated - see TL_CACHE_* above). Written by the fetch task on Core 0
        // and read by the main loop on Core 1 (promotion of the next departure), hence the spinlock.
        tlCacheEntry* cache = nullptr;
        int cacheCount = 0;
        unsigned long cacheBuiltAt = 0;
        char cacheAreaId[12];
        portMUX_TYPE cacheMux = portMUX_INITIALIZER_UNLOCKED;
        int findCache(const char* line, const char* destination);
        void storeCache(const char* line, const char* destination, const char* calling);

        bool callingFetchKnown = false;
        bool nextCallingFetchKnown = false;
        unsigned long apiRequestCount = 0;

        void logResult(const char *fmt, ...);
        static void convertToBoardText(char* s, size_t maxLen);
        static bool compareDepartures(const rdiService& a, const rdiService& b);
        bool connect(WiFiClientSecure &client);
        int readResponseHeaders(WiFiClientSecure &client, bool &chunked, long &contentLength);
        long readBody(WiFiClientSecure &client, bool chunked, long contentLength, JsonStreamingParserGS &parser, unsigned long timeoutMs, bool &timedOut);
        void beginParse(tlParseMode mode);
        // Looks up the stops after the board's own stop for one trip and writes "A, B, C" (no times) into out.
        int fetchTripCalling(const char* tripId, const char* startDate, const char* areaId, const char* boardStopName, const char* key, char* out, size_t outSize);
        // Fills xStation->service[idx].calling for one of the first two departures: cache, then the previous
        // load if it is the same trip, then a Trips request. Returns whether the stops are now known.
        bool resolveCalling(int idx, rdStation *station, const char* areaId, const char* key);

        virtual void whitespace(char c);
        virtual void startDocument();
        virtual void key(const char *key);
        virtual void value(const char *value);
        virtual void endArray();
        virtual void endObject();
        virtual void endDocument();
        virtual void startArray();
        virtual void startObject();

    public:
        trafiklabClient(rdiStation *station, stnMessages *messages, sharedBufferSpace *sharedBuffer);
        // Fetches the departures for a Trafiklab stop area (a 9-digit id from the stop search) and, when
        // fetchCallingPoints is set, the stop lists for the first two. metroOnly drops everything that
        // isn't a tunnelbana (METRO) departure.
        int fetchDepartures(rdStation *station, stnMessages *messages, const char *areaId, const char *accessKey, int numRows, bool metroOnly, bool fetchCallingPoints);
        void loadDepartures(rdStation *station, stnMessages *messages);
        // Pure local lookup in the line+direction cache - no network. True only on a hit.
        bool lookupCachedCalling(const char *line, const char *destination, char *callingOut, size_t callingOutSize);
        // Stop-name search for the web config: "[{"name":"...","id":"..."},...]". requireMode (e.g. "METRO")
        // keeps only stops served by that kind of transport.
        String searchStops(const char *query, const char *accessKey, const char *requireMode, int maxResults = 8);
        unsigned long getApiRequestCount() { return apiRequestCount; }
};
