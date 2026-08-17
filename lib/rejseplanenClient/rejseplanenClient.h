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

#define MAXPATHSTACK 6

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
          char track[4];
          char rtTrack[4];
          bool cancelled;
          int catCode;
          char ref[MAXJOURNEYREFSIZE];
          char opco[50];
          char direction[MAXLOCATIONSIZE];
          char name[MAXLINESIZE];
        };

        const char* rjHost = "www.rejseplanen.dk";
        const char* rjDepartureBoardApi = "/api/departureBoard";
        const char* rjJourneyDetailApi = "/api/journeyDetail";

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

        bool fetchingDepartures;       // true = parsing departureBoard, false = parsing journeyDetail
        bool inTargetArray = false;    // inside the "Departure" (or "Stops/Stop") array
        int arrayDepth = 0;

        rjRawRecord raw;
        int arrayBaseDepth = -1;       // stackTop depth of the target array's elements
        int targetArrayDepth = -1;     // arrayDepth value at which the target array was entered

        // journeyDetail (calling points) state
        rjStopEntry callingStops[40];
        int numCallingStops = 0;

        static bool compareTimes(const rdiService& a, const rdiService& b);
        void resetRawRecord();
        void finaliseDepartureRecord();
        void finaliseCallingStop();
        void transliterateDanish(char* input, size_t maxLen);
        void buildCurrentPath(const char* key);
        int getServiceDetails(const char *ref, const char *accessId, const char *stopId);

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
        int fetchDepartures(rdStation *station, stnMessages *messages, const char *stopId, const char *accessId, int numRows, int productsMask, bool fetchCallingPoints, const char *callingStopId, int timeOffsetMins);
        void loadDepartures(rdStation *station, stnMessages *messages);
};
