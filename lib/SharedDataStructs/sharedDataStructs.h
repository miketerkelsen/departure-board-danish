// Common station data structures shared by both data clients
#pragma once
#include <Arduino.h>

#define MAXBOARDMESSAGES 4
#define MAXMESSAGESIZE 400
#define MAXCALLINGSIZE 450
#define MAXBOARDSERVICES 24   // was 9 - bumped for the Odense DK Bus board (odenseBusLoop() in
                               // Departures Board.cpp), which splits departures at Odense St. three
                               // ways (OBC Nord/OBC Syd/Ejlskovsgade) and needs enough depth per
                               // group that a quieter one doesn't come up empty. Applies to every
                               // mode's fetch depth, not just that one - the RAM/flash cost is small
                               // (checked against the build's reported usage) and more departures to
                               // rotate through is a mild improvement everywhere, not a downside.
#define MAXLOCATIONSIZE 85
#define MAXBUSTUBELOCATIONSIZE 50
#define MAXFILTERSIZE 25
#define MAXLINESIZE 20
#define MAXTUBEBUSREADSERVICES 20

#define MAXKEYNAMESIZE 50
#define MAXRESULTMESSAGESIZE 80

#define MAXJOURNEYREFSIZE 220      // Rejseplanen JourneyDetailRef.ref tokens are long opaque strings (~168 chars observed)

#define MAXWEATHERSIZE 50

#define OTHER 0
#define TRAIN 1
#define BUS 2


struct stnMessages {
    int numMessages;
    char messages[MAXBOARDMESSAGES][MAXMESSAGESIZE];
};

struct rdService {
    char sTime[6];
    char destination[MAXLOCATIONSIZE];
    char via[MAXLOCATIONSIZE];  // also used for line name for TfL
    char etd[11];
    char platform[8]; // was 4 - København H's S-tog tracks are shared island platforms, and
                       // Rejseplanen reports those as a range like "9-10"/"11-12" (5 chars), not a
                       // single number - the old 3-usable-char buffer (sized for UK platform
                       // numbers, always 1-3 chars) was silently truncating those to "9-1"/"11-1"
    bool isCancelled;
    bool isDelayed;
    int trainLength;
    byte classesAvailable;
    char opco[50];
    char stopArea[40];  // Rejseplanen Departure.stop, minus its trailing "(<municipality>)" - the
                         // specific stand/platform-area a service leaves from at hub stops that
                         // combine several physical stands under one stop id (e.g. Odense St.'s
                         // "OBC Nord Plads H"/"OBC Syd Plads A"/"Ejlskovsgade"). Empty for clients
                         // that don't populate it.

    int serviceType;
    int timeToStation;  // Only for TfL
    bool isSTog;  // Rejseplanen catOut=="S-Tog" - drives the København H S-tog line-badge board style
  };

  struct rdStation {
    char location[MAXLOCATIONSIZE];
    bool platformAvailable;
    int numServices;
    bool boardChanged;  // Only for TfL
    char calling[MAXCALLINGSIZE];   // Only store the calling stops for the first service returned
    char origin[MAXLOCATIONSIZE]; // Only store the origin for the first service returned
    // True once a calling-at fetch has actually succeeded for the current primary service - false
    // means "we don't know yet" (still fetching, or the last attempt failed/timed out), NOT "this
    // service has no further stops"/"genuinely originates here". Without this, an empty calling/
    // origin from a failed or not-yet-completed fetch looked identical to a real origin service with
    // nothing to show, so the board displayed "starts here" as if it knew that, when it didn't.
    bool callingKnown;
    // Calling-at for whichever service is currently in position [1] (the "next" departure),
    // pre-fetched ahead of time so that when the current primary departs and this one is promoted
    // into position [0], its calling-at is usually already known instead of racing a fresh fetch
    // against the short departed-train animation window. Doesn't increase the steady-state fetch
    // rate - every service that ever becomes primary already sat in position [1] first, so this is
    // the same one fetch per service, just happening a cycle earlier. See fetchDepartures() and the
    // promotion code in the main sketch for how this is kept fresh and consumed.
    char nextCalling[MAXCALLINGSIZE];
    char nextOrigin[MAXLOCATIONSIZE];
    bool nextCallingKnown;
    char serviceMessage[MAXMESSAGESIZE];  // Only store the service message for the first service returned
    rdService service[MAXBOARDSERVICES];
  };

  // Rail structure for data downloads
  struct rdiService {
    char sTime[6];
    char destination[MAXLOCATIONSIZE];
    char via[MAXLOCATIONSIZE];
    char origin[MAXLOCATIONSIZE];
    char etd[11];
    char platform[8]; // was 4 - København H's S-tog tracks are shared island platforms, and
                       // Rejseplanen reports those as a range like "9-10"/"11-12" (5 chars), not a
                       // single number - the old 3-usable-char buffer (sized for UK platform
                       // numbers, always 1-3 chars) was silently truncating those to "9-1"/"11-1"
    bool isCancelled;
    bool isDelayed;
    int trainLength;
    byte classesAvailable;
    char opco[50];
    char calling[MAXCALLINGSIZE];
    char serviceMessage[MAXMESSAGESIZE];
    char stopArea[40];  // see rdService.stopArea
    int serviceType;
    char serviceID[MAXJOURNEYREFSIZE];  // LDBWS service id (UK) or JourneyDetailRef.ref token (Rejseplanen)
    char sortTime[6];
    bool isSTog;  // see rdService.isSTog
  };

  struct rdiStation {
    char location[MAXLOCATIONSIZE];
    bool platformAvailable;
    int numServices;
    rdiService service[MAXBOARDSERVICES];
  };

  // Common structure for tube/bus data downloads
    struct busTubeService {
      char destinationName[MAXBUSTUBELOCATIONSIZE];
      char currentLocation[MAXBUSTUBELOCATIONSIZE];
      char lineName[MAXLINESIZE];
      int timeToStation;
      char scheduled[6];
      char expected[6];
  };

  struct busTubeStation {
      int numServices;
      busTubeService service[MAXTUBEBUSREADSERVICES];
  };

  // Common data buffers for parsing JSON
  struct sharedBufferSpace {
    char currentKey[MAXKEYNAMESIZE];
    char objectCurrentKey[MAXKEYNAMESIZE];
    char currentPath[(MAXKEYNAMESIZE*2+1)];
    char arrayName[(MAXKEYNAMESIZE*2)+1];
    char lastResultMessage[MAXRESULTMESSAGESIZE];
  };
