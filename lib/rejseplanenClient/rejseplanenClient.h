/*
 * Departures Board (c) 2025-2026 Gadec Software
 *
 * rejseplanenClient Library
 *
 * Danish public transport departures via the Rejseplanen API 2.0 (HAFAS REST),
 * covering DK Rail (IC/ICL/Re/Togbus/S-tog) and Odense Letbane.
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

#define MAXPATHSTACK 12                // headroom for deeply-nested fields (e.g. service alert Messages)

// København H's Rejseplanen stop id - gates the S-tog line-badge board style (see
// Departures Board.cpp's useSTogStyle()) and the shortened "Stopper ved" list below.
#define RJ_KBH_H_STOP_ID "8600626"

// Floor on heap_caps_get_largest_free_block() below which fetchDepartures()/getServiceDetails()/
// searchStops() skip opening a new HTTPS connection entirely, rather than attempt one. Live-caught
// evidence: a board crashed and rebooted after ~25 minutes of sustained "GET timed out" failures
// against an unresponsive Rejseplanen - largest free block measured at 1588 bytes right before the
// crash, back to ~35-38KB immediately after reboot. Each failing cycle still completes a full TLS
// handshake (connect() succeeds; only the response itself times out) before tearing down, and that
// repeated handshake/teardown churn is a known ESP32/mbedTLS source of heap fragmentation. Below this
// floor, allocations elsewhere in that stack (WiFiClientSecure/mbedTLS buffers, Strings) are NOT
// nothrow-guarded like the line+direction cache is, so a failure there aborts the whole program -
// indistinguishable from a spontaneous reboot. Skipping the attempt caps how much worse a
// fragmentation spiral can get; the cost is one skipped fetch cycle (existing display data is left
// exactly as any other failed cycle would leave it). Set comfortably below every healthy reading seen
// this session (34-38KB+) so this never fires under normal conditions.
#define MIN_SAFE_HEAP_FOR_FETCH 20000

class rejseplanenClient: public JsonListenerGS {

    private:

        struct rjStopEntry {
          char name[MAXLOCATIONSIZE];
          char extId[16];
          char time[6];          // HH:MM this service calls at this stop (arrTime, falling back to depTime)
        };

        char stopScratchName[MAXLOCATIONSIZE];
        char stopScratchExtId[16];
        char stopScratchArrTime[9];
        char stopScratchDepTime[9];
        bool boardChanged = false;

        // Raw, per-record scratch space (a Departure or a journeyDetail Stop entry),
        // collected across value() calls in whatever order the JSON delivers them and
        // reconciled once the record's endObject() closes it.
        struct rjRawRecord {
          char time[9];
          char rtTime[9];
          char track[8];   // was 4 - matches rdService::platform; København H's S-tog shared
          char rtTrack[8]; // island platforms report ranges like "9-10"/"11-12" (5 chars)
          bool cancelled;
          int catCode;
          char catOut[16];      // e.g. "IC", "Re", "ECE", "Bus", "Togbus" - the reliable signal for
                                 // whether a service is actually a bus (catCode is reused for
                                 // unrelated categories like international trains, e.g. catCode 3
                                 // covers both Togbus AND EuroCity/RailJet/Snalltaget at some stations)
          char ref[MAXJOURNEYREFSIZE];
          char opco[50];
          char direction[MAXLOCATIONSIZE];
          char name[MAXLINESIZE];
          char stop[64];        // Departure.stop - the specific stand/platform-area at hub stops
                                 // that combine several physical stands under one stop id
        };

        const char* rjHost = "www.rejseplanen.dk";
        const char* rjDepartureBoardApi = "/api/departureBoard";
        const char* rjJourneyDetailApi = "/api/journeyDetail";
        const char* rjLocationNameApi = "/api/location.name";

        rdiStation* xStation = nullptr;
        stnMessages* xMessages = nullptr;
        sharedBufferSpace* js = nullptr;

        // Generic path-stack tracking (needed because Rejseplanen's JSON nests deeper than
        // National Rail's - e.g. ProductAtStop/operatorInfo appears *before* ProductAtStop/catCode
        // in the same object - so a single-level "objectCurrentKey" reset-to-root on endObject()
        // is not safe here, unlike rdmRailClient).
        char pathStack[MAXPATHSTACK][MAXKEYNAMESIZE];
        int stackTop = 0;
        char pendingKey[MAXKEYNAMESIZE];
        char currentPath[(MAXKEYNAMESIZE*MAXPATHSTACK)+MAXPATHSTACK];

        // The key that named the target array itself ("Departure" or "Stop"), captured once when
        // the array is entered. Array elements have no key() call of their own - normally an
        // element's startObject() would reuse whatever pendingKey last held, but pendingKey keeps
        // getting overwritten by every key() call made while walking the PREVIOUS element's own
        // fields, so by the time element 2+ starts, pendingKey no longer says "Departure"/"Stop".
        // targetElementKey is the one thing that must stay stable across every element of the
        // target array, so it's captured separately here instead of relying on pendingKey for it.
        char targetElementKey[MAXKEYNAMESIZE];

        bool fetchingDepartures;       // true = parsing departureBoard, false = parsing journeyDetail
        bool searchingStops = false;   // true = parsing location.name (stop-name search), see searchStops()
        bool inTargetArray = false;    // inside the "Departure" (or "Stops/Stop") array
        int arrayDepth = 0;

        // location.name (stop-name search) state - a location.name response nests a "name" field at
        // several levels (each StopLocation's own name, but also every one of its productAtStop
        // entries), so - same as everywhere else in this file - only an exact full-path match
        // ("stopLocationOrCoordLocation/StopLocation/name") is trusted, never a bare key() check.
        char stopSearchName[MAXLOCATIONSIZE];
        char stopSearchExtId[16];
        String stopSearchResult;
        int stopSearchCount = 0;
        int stopSearchMax = 8;
        void finaliseStopSearchResult();

        rjRawRecord raw;
        int arrayBaseDepth = -1;       // stackTop depth of the target array's elements
        int targetArrayDepth = -1;     // arrayDepth value at which the target array was entered

        // journeyDetail (calling points) state
        rjStopEntry callingStops[40];
        int numCallingStops = 0;

        // Persistent cache of calling-at text keyed by (line, destination) - e.g. ("B","Farum St.").
        // Confirmed live against the real API (multiple different S-tog trips on the same
        // line+direction, minutes apart) that calling points are a property of the LINE+DIRECTION,
        // not the individual trip - identical stop-for-stop every time. So once known for a given
        // combo it's safe to reuse for every future departure on that combo, without a fresh
        // per-trip journeyDetail fetch. Deliberately only used for MODE_STOG (see useLineDirCache) -
        // other DK modes have more schedule variability per "line" (fewer trips, more partial runs)
        // where this assumption is less safe, and haven't shown the problem this exists to fix.
        // Sized for Copenhagen S-tog's ~7 lines x 2 directions, with headroom.
        struct LineDirCallingEntry {
            char line[MAXLINESIZE];
            char destination[MAXLOCATIONSIZE];
            char calling[MAXCALLINGSIZE];
            char origin[MAXLOCATIONSIZE];
        };
        // 14, not 24 - live-isolated on real hardware: a plain, empty ~15KB heap allocation
        // (24 entries), by itself, with the cache's find/store logic never even engaging, reliably
        // broke fetch reliability to 0% on a board that fetched normally seconds before and after
        // (confirmed via an A/B comparison against an otherwise-identical board with no cache at all,
        // succeeding 38/39 in the same window). Bisected by allocation size alone (cache logic held
        // disabled throughout): 2 entries (~1.3KB) fetched reliably from the very first cycle, 24
        // entries (~15KB) failed every cycle. 14 entries (~9KB) - exactly Copenhagen S-tog's real
        // line+direction count, no spare headroom - was the value actually tested and confirmed
        // reliable end-to-end (cache logic re-enabled) before settling on it; the exact mechanism
        // (suspected: this size competing with WiFi/mbedTLS for some more limited pool than plain
        // heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) reports, though never fully confirmed)
        // wasn't pinned down, and a static (non-heap) array was ruled out separately - even at 16
        // entries (~10KB) it overflowed the ESP32's DRAM link-time segment by ~9KB, meaning only
        // ~1.2KB of margin exists there. If this cache ever needs to grow past 14 combos, re-run the
        // same bisection before raising this - don't just bump the number.
        #define MAXLINEDIRCACHE 14
        // Heap-allocated (nothrow) - the same allocation is trivial against 100KB+ of free heap, it's
        // specifically the SIZE that matters here (see above), not heap vs. static in general. Lazily
        // allocated only once useLineDirCache is actually used (true for MODE_STOG only), so a board
        // that never uses S-tog mode never pays this cost. The actual allocation call itself must
        // never run inside lineDirCacheMux's critical section - ESP-IDF explicitly documents that
        // heap allocation must never happen inside a critical section, since the allocator has its
        // own internal locking and blocking while a critical section has interrupts and the scheduler
        // disabled on that core is undefined behaviour - so fetchDepartures() does the actual
        // allocation (and value-initialisation - zeroing every slot) as a completely ordinary,
        // unlocked call, and only takes the lock afterwards, briefly, to publish the already-fully-
        // prepared pointer. portENTER_CRITICAL/EXIT_CRITICAL's underlying spinlock acquire/release
        // acts as a memory barrier, so this still guarantees the other core sees the zeroed contents
        // before it can ever see the non-null pointer.
        //
        // nothrow rather than a plain new[]: a plain new[] aborts the whole program on allocation
        // failure (no exception handler catches it in this build), which would look exactly like a
        // spontaneous reboot if this is ever attempted when the heap doesn't have a large enough
        // CONTIGUOUS free block, even with plenty of total free heap. Null until allocated - every
        // access point (findLineDirCacheEntry/storeLineDirCacheEntry/lookupCachedCalling) guards
        // against that, so a failed allocation just means the cache silently stays off.
        LineDirCallingEntry* lineDirCache = nullptr;
        int lineDirCacheCount = 0;
        // millis() this generation of the cache started - rebuilt from scratch once a day (see
        // LINEDIRCACHE_MAX_AGE_MS) so a genuine schedule change eventually gets picked up without
        // needing a reboot. Starts at 0, which combined with an empty cache is already the correct
        // "just built" state, so this deliberately doesn't fire on the very first fetch after boot.
        unsigned long lineDirCacheBuiltAt = 0;
        #define LINEDIRCACHE_MAX_AGE_MS 86400000UL
        // Guards every access to lineDirCache/lineDirCacheCount. Needed because this cache is
        // genuinely cross-core: it's written from fetchDeparturesTask() (pinned to Core 0, see
        // "Departures Board.cpp") but also read directly from the main sketch's promotion code in
        // departureBoardLoop(), which runs in the default Arduino loop task on Core 1 - via
        // lookupCachedCalling(), called with no relation at all to whatever fetchDepartures() might
        // be doing on the other core at that exact moment. A plain spinlock (not a FreeRTOS
        // semaphore) because every critical section here is tiny (a few strlcpy calls, never a
        // network operation), so the cost of contention is negligible - see rejseplanenClient.cpp for
        // the incident this fixes.
        portMUX_TYPE lineDirCacheMux = portMUX_INITIALIZER_UNLOCKED;
        // -1 if not cached. Empty line/destination never matches (guards against an unset via/
        // destination field ever being looked up or stored as if it were a real key). Deliberately
        // does NOT take lineDirCacheMux itself - it's a raw internal helper, always called from
        // inside a region its caller already holds the lock for (see storeLineDirCacheEntry(),
        // lookupCachedCalling(), and both call sites in fetchDepartures()).
        int findLineDirCacheEntry(const char *line, const char *destination);
        void storeLineDirCacheEntry(const char *line, const char *destination, const char *calling, const char *origin);

        // Whether calling-at info is actually known this cycle for xStation->service[0] / [1]
        // respectively - either just freshly fetched, or validly reused from a previous fetch. False
        // when it was never attempted, still in flight, or the last attempt failed - read by
        // loadDepartures() to set rdStation::callingKnown / nextCallingKnown (see their own comments
        // for why this distinction matters).
        bool callingFetchKnown = false;
        bool nextCallingFetchKnown = false;

        static bool compareTimes(const rdiService& a, const rdiService& b);
        void resetRawRecord();
        void finaliseDepartureRecord();
        void finaliseCallingStop();
        void convertDanishToLatin1(char* input, size_t maxLen);
        void buildCurrentPath(const char* key);
        // targetIdx selects which xStation->service[] slot to populate (calling/origin) - 0 for the
        // primary service, 1 for the pre-fetch of whatever's next in line (see rdStation::nextCalling).
        // httpsClient is the SAME connection fetchDepartures() already used for the departureBoard
        // request, passed in by reference so this can reuse it (HTTP keep-alive) instead of paying
        // for a whole new TCP+TLS handshake - see the keep-alive comment in fetchDepartures() for why.
        // Reconnects on its own if the connection isn't already open (e.g. the server closed it, or
        // this is being called for slot 1 after slot 0's connection turned out non-reusable), so this
        // is never worse than the old always-fresh-connection behaviour, only sometimes better.
        int getServiceDetails(WiFiClientSecure &httpsClient, const char *ref, const char *accessId, const char *stopId, int targetIdx);
        // Reads the status line + headers of an HTTP response already sent on `client`, returning the
        // Content-Length (-1 if the header was absent) and whether the server/response allows the
        // connection to be kept open for a follow-up request (false if the server itself sent
        // "Connection: close"). Shared by fetchDepartures() and getServiceDetails() so the keep-alive
        // bookkeeping only needs to be right in one place. Waits up to 8s for the response to start,
        // then up to 1s to walk the header block - same budget both call sites used before this was
        // factored out.
        int readResponseHeaders(WiFiClientSecure &client, long &contentLength, bool &serverAllowsReuse, bool &chunked);
        // Reads exactly contentLength bytes (or, if contentLength<0, until the connection closes - the
        // pre-keep-alive fallback for a response with no Content-Length header) from `client`, feeding
        // each byte to parser as it arrives. Returns bytes actually read; sets timedOut true if the
        // deadline was hit (contentLength case) or the connection dropped before delivering everything
        // promised - callers treat that as a failed fetch and must NOT reuse the connection afterward.
        long readResponseBody(WiFiClientSecure &client, long contentLength, JsonStreamingParserGS &parser, unsigned long timeoutMs, bool &timedOut);

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
        rejseplanenClient(rdiStation *station, stnMessages *messages, sharedBufferSpace *sharedBuffer);
        // useLineDirCache: MODE_STOG passes true - see LineDirCallingEntry's own comment for what
        // this changes and why it's scoped to S-tog only. Every other mode leaves this at its
        // default (false) and behaves exactly as before.
        int fetchDepartures(rdStation *station, stnMessages *messages, const char *stopId, const char *accessId, int numRows, int productsMask, bool fetchCallingPoints, const char *callingStopId, int timeOffsetMins, bool useLineDirCache = false);
        void loadDepartures(rdStation *station, stnMessages *messages);
        // Looks up calling-at for a given line+destination directly from the persistent S-tog cache
        // (see LineDirCallingEntry), with NO network activity - a pure local lookup. Returns true
        // (and fills callingOut/originOut) only on a cache hit. The main sketch's promotion code
        // calls this for MODE_STOG so a newly-promoted primary can show its calling-at the INSTANT
        // it's promoted, with no fetch needed at all once its line+direction has been seen once.
        bool lookupCachedCalling(const char *line, const char *destination, char *callingOut, size_t callingOutSize, char *originOut, size_t originOutSize);
        // Searches Rejseplanen stops by (partial) name - powers the web config's name-search typeahead
        // for every DK mode, in place of making the user look up/type a numeric stop id by hand.
        // Returns a compact JSON array "[{"name":"...","id":"..."},...]", capped at maxResults.
        String searchStops(const char *query, const char *accessId, int maxResults = 8);
};
