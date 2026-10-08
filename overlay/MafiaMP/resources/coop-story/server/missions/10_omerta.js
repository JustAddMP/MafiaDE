const { PLACES, PARKING, WEAPONS, WEATHER } = require("./places.js");
const { foes, guards, car } = require("./helpers.js");

/**
 * Chapter 10 - Omertà (1933)
 * Frank has talked. Follow the trail across the city: the bank where the
 * papers are, the station where his family waits, the airport where he is
 * trying to leave.
 */
module.exports = {
    id: "10_omerta",
    title: "OMERTÀ",
    subtitle: "Frank",
    description: "Find out what Frank did: the bank, the station, the airport. Bring him in.",
    environment: { weather: WEATHER.omerta, time: 9.5 },
    spawn: PLACES.lobby,
    vehicles: {
        sedan: car("lassiter_v16", PARKING.lotA, "Salieri's Lassiter"),
    },
    weapons: [[WEAPONS.pistol, 120]],
    steps: [
        {
            type: "goto", who: "any", target: PLACES.frankBank.pos, radius: 22,
            objective: "Check the bank where Frank keeps the books",
            hint: "Someone goes in. The rest keep the engine running.",
            checkpoint: true, checkpointSpawn: PLACES.frankBank,
            outroTitle: "THE BOOKS", outro: "The books are gone. Frank's guards are still here.",
        },
        {
            type: "stealth", target: PLACES.frankBank.pos, radius: 8, detectRadius: 9, onAlarm: "combat", who: "any",
            guards: guards(PLACES.frankBank, 3, 14),
            objective: "Get to the manager's office without a fight",
            hint: "Frank's men guard the lobby. Quiet, or loud.",
            outroTitle: "THE OFFICE", outro: "His family is at the station",
        },
        {
            type: "goto", who: "all", target: PLACES.station.pos, radius: 28,
            objective: "Get to Little Italy station",
            hint: "Frank's wife and daughter are waiting for a train.",
            checkpoint: true, checkpointSpawn: PLACES.station,
            outroTitle: "THE STATION", outro: "Morello's men beat you here",
        },
        {
            type: "combat", enemies: foes(PLACES.station, 5, 20),
            objective: "Keep Morello's men away from Frank's family",
            hint: "They want Frank's family as leverage.",
            fallbackSeconds: 45,
            checkpoint: true, checkpointSpawn: PLACES.station,
            outroTitle: "SAFE", outro: "Frank is at the airport. Go.",
        },
        {
            type: "goto", who: "all", target: PLACES.airport.pos, radius: 40, timeLimit: 420,
            objective: "Get to the airport before Frank's plane leaves",
            hint: "7 minutes across the whole city.",
            failText: "Frank's plane left.",
            environment: { weather: WEATHER.sunset, time: 18.5 },
            outroTitle: "FRANK", outro: "He is on the tarmac",
        },
        {
            type: "wait", seconds: 15,
            objective: "Talk to Frank",
            hint: "He hands over the books. Let him go, or do not.",
            environment: { weather: WEATHER.funeral, time: 21.0 },
            outroTitle: "OMERTÀ", outro: "Salieri never asks what happened to Frank",
        },
    ],
};
