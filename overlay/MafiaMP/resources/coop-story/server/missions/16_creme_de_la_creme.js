const { PLACES, PARKING, WEAPONS, WEATHER } = require("./places.js");
const { foes, mark, waves, car, TOMMY } = require("./helpers.js");

/**
 * Chapter 16 - Crème de la Crème (1935)
 * The night Morello runs. Hit the hotel, chase his car across the city to
 * the airport, and stop the plane.
 */
module.exports = {
    id: "16_creme_de_la_creme",
    title: "CRÈME DE LA CRÈME",
    subtitle: "Morello",
    description: "Hit Morello at his hotel. When he runs for the airport, run faster.",
    environment: { weather: WEATHER.nightRain, time: 23.0 },
    spawn: PLACES.lobby,
    vehicles: {
        sedan: car("lassiter_v16", PARKING.lotA, "Salieri's Lassiter"),
        fast: car("smith_v12", PARKING.lotB, "Smith V12"),
    },
    weapons: [[WEAPONS.tommy, 250], [WEAPONS.pistol, 90]],
    steps: [
        {
            type: "goto", who: "all", target: PLACES.morelloHotel.pos, radius: 26,
            objective: "Get to Morello's hotel",
            hint: "Both cars. Everyone arrives.",
            checkpoint: true, checkpointSpawn: PLACES.morelloHotel,
            outroTitle: "THE HOTEL", outro: "His men are on the steps",
        },
        {
            type: "combat",
            enemies: foes(PLACES.morelloHotel, 5, 16, TOMMY),
            waves: waves([[25, PLACES.morelloHotel, 4, 24, TOMMY, "FROM THE LOBBY"]]),
            objective: "Fight through Morello's men",
            hint: "He is upstairs. Make him come down.",
            fallbackSeconds: 60,
            checkpoint: true, checkpointSpawn: PLACES.morelloHotel,
            outroTitle: "HE RUNS", outro: "Morello's car just left for the airport",
        },
        {
            type: "goto", who: "all", target: PLACES.airport.pos, radius: 45, timeLimit: 360,
            objective: "Chase Morello to the airport",
            hint: "6 minutes. If his plane takes off, this was for nothing.",
            failText: "Morello's plane took off.",
            checkpoint: true, checkpointSpawn: PLACES.airport,
            environment: { weather: WEATHER.foggy, time: 4.5 },
            outroTitle: "THE RUNWAY", outro: "The plane is taxiing. His guards are on the tarmac.",
        },
        {
            type: "killTarget", target: mark("Don Morello", PLACES.airport, TOMMY, 180), bodyguards: foes(PLACES.airport, 4, 16, TOMMY),
            objective: "Stop Morello before the plane leaves",
            hint: "Kill him on the tarmac. His guards will not make it easy.",
            fallbackSeconds: 25,
            outroTitle: "CRÈME DE LA CRÈME", outro: "The Morello family is finished",
        },
    ],
};
