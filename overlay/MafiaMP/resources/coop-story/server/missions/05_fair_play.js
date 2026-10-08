const { PLACES, PARKING, WEATHER } = require("./places.js");
const { car } = require("./helpers.js");

/**
 * Chapter 5 - Fair Play (1932)
 * Night: fetch the race car and get it to Bertone's garage in one piece.
 * Day: the Lost Heaven Grand Prix. Every player takes a race car from the
 * grid by the river and runs two laps: out to the autodrome and back. First
 * across the line wins, but the chapter only ends when the whole crew has
 * finished.
 */
module.exports = {
    id: "05_fair_play",
    title: "FAIR PLAY",
    subtitle: "Two laps. No brakes.",
    description: "Deliver the race car tonight. Tomorrow: grid -> autodrome -> grid, two laps.",
    environment: { weather: WEATHER.nightRain, time: 23.0 },
    spawn: PLACES.riverRow,
    vehicles: {
        race1: car("trautenberg_sport", PARKING.grid1, "Trautenberg"),
        race2: car("smith_v12", PARKING.grid2, "Smith V12"),
        race3: car("lassiter_v16_roadster", PARKING.grid3, "Roadster"),
        race4: car("carrozella_c_series", PARKING.grid4, "Carrozella"),
    },
    weapons: [],
    steps: [
        {
            type: "goto", vehicle: "race1", who: "any", target: PLACES.bertoneGarage.pos, radius: 18, timeLimit: 300,
            objective: "Get the Trautenberg to Bertone's garage, in one piece",
            hint: "5 minutes. One of you drives it, the rest follow in anything.",
            failText: "Bertone will not touch that wreck in time.",
            checkpoint: true, checkpointSpawn: PLACES.riverRow,
            outroTitle: "BERTONE'S", outro: "He will have it ready by morning",
        },
        {
            type: "wait", seconds: 10, countdown: true, teleport: PLACES.riverRow, resetVehicles: ["race1", "race2", "race3", "race4"],
            environment: { weather: WEATHER.race, time: 13.0 },
            objective: "Race day. Pick a race car on the grid",
            hint: "Engines start in 10 seconds.",
        },
        {
            type: "race", laps: 2, radius: 22,
            gates: [PLACES.autodrome.pos, PLACES.riverRow.pos],
            vehicles: ["race1", "race2", "race3", "race4"],
            objective: "Race: grid -> autodrome -> grid, 2 laps",
            hint: "Gate 1 is the autodrome, gate 2 is the grid. You must be in a race car.",
            outroTitle: "CHEQUERED FLAG", outro: "Salieri's driver takes the cup",
        },
    ],
};
