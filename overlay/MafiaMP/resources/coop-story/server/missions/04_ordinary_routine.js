const { PLACES, PARKING, WEAPONS, WEATHER } = require("./places.js");
const { foes, ally, waves, car } = require("./helpers.js");

/**
 * Chapter 4 - Ordinary Routine (1930)
 * Collecting protection money with Paulie and Sam, then out to Clark's Motel
 * where Morello's men are waiting. Rescue Sam, hold the motel, and get out.
 */
module.exports = {
    id: "04_ordinary_routine",
    title: "ORDINARY ROUTINE",
    subtitle: "Just collecting",
    description: "The protection round, then the motel. It is never just a routine.",
    environment: { weather: WEATHER.day, time: 11.0 },
    spawn: PLACES.lobby,
    vehicles: {
        sedan: car("bolt_v8", PARKING.lotA, "Paulie's Bolt"),
    },
    weapons: [[WEAPONS.pistol, 90]],
    steps: [
        {
            type: "board", vehicle: "sedan",
            objective: "Everyone in the Bolt",
            hint: "Paulie drives, or you do.",
        },
        {
            type: "goto", vehicle: "sedan", target: PLACES.shopRound.pos, radius: 22,
            objective: "First stop: the shops on the round",
            hint: "Collect what Salieri is owed.",
            checkpoint: true, checkpointSpawn: PLACES.shopRound,
            outroTitle: "COLLECTED", outro: "One more stop at the station",
        },
        {
            type: "goto", vehicle: "sedan", target: PLACES.station.pos, radius: 28,
            objective: "Second stop: the station bar",
            hint: "Keep the crew in the car.",
            outroTitle: "COLLECTED", outro: "Now the motel out of town. Sam is already there.",
        },
        {
            type: "goto", vehicle: "sedan", target: PLACES.motel.pos, radius: 35, timeLimit: 480,
            objective: "Drive out to Clark's Motel",
            hint: "8 minutes. It is a long way out.",
            failText: "Sam ran out of time.",
            checkpoint: true, checkpointSpawn: PLACES.motel,
            environment: { weather: WEATHER.motel, time: 15.0 },
            outroTitle: "THE MOTEL", outro: "Shots. They have Sam inside.",
        },
        {
            type: "combat", enemies: foes(PLACES.motel, 5, 22),
            allies: [ally("Sam", PLACES.motel)],
            objective: "Clear the motel and find Sam",
            hint: "Sam is hurt. Keep him alive.",
            fallbackSeconds: 45,
            checkpoint: true, checkpointSpawn: PLACES.motel,
            outroTitle: "SAM'S ALIVE", outro: "More of Morello's men are coming up the road",
        },
        {
            type: "defend", seconds: 90, area: PLACES.motel.pos, radius: 40, graceSeconds: 15,
            waves: waves([[0, PLACES.motel, 3, 28], [30, PLACES.motel, 4, 30, undefined, "SECOND CAR"], [60, PLACES.motel, 4, 32, undefined, "LAST OF THEM"]]),
            allies: [ally("Sam", PLACES.motel, undefined, [-2, 2])],
            objective: "Hold the motel until the road is clear",
            hint: "90 seconds. Do not leave the motel grounds.",
            failText: "You left Sam behind.",
            outroTitle: "ROAD CLEAR", outro: "Get Sam to the car",
        },
        {
            type: "board", vehicle: "sedan", summonVehicles: ["sedan"],
            objective: "Everyone back in the Bolt",
            hint: "The car is where you left it. Go.",
        },
        {
            type: "goto", vehicle: "sedan", target: PLACES.salieriBar.pos, radius: 25, timeLimit: 540,
            objective: "Get Sam back to Salieri's",
            hint: "9 minutes. Do not stop for anyone.",
            failText: "Sam did not make it back.",
            environment: { weather: WEATHER.sunset, time: 19.0 },
            outroTitle: "HOME", outro: "Sam will live. Salieri wants a word about Morello.",
        },
    ],
};
