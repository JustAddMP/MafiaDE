const { PLACES, PARKING, WEAPONS, WEATHER } = require("./places.js");
const { foes, ally, waves, car } = require("./helpers.js");

/**
 * Chapter 13 - Bon Appetit (1935)
 * Lunch with Don Salieri at Pepe's turns into a hit. Protect the Don, get
 * him to the car, and chase the attackers' sedan across town.
 */
module.exports = {
    id: "13_bon_appetit",
    title: "BON APPETIT",
    subtitle: "Lunch at Pepe's",
    description: "Morello tries to kill the Don at lunch. Keep Salieri alive, get him to the car, chase the shooters.",
    environment: { weather: WEATHER.salieri, time: 12.5 },
    spawn: PLACES.salieriBar,
    vehicles: {
        donsCar: car("lassiter_v16_appolyon", PARKING.lotA, "The Don's car"),
    },
    weapons: [[WEAPONS.pistol, 120]],
    steps: [
        {
            type: "together", target: PLACES.restaurant.pos, radius: 14, maxSpread: 30, graceSeconds: 15, noVehicles: true,
            objective: "Walk the Don to Pepe's",
            hint: "On foot, as a group. The Don does not hurry.",
            checkpoint: true, checkpointSpawn: PLACES.restaurant,
            outroTitle: "PEPE'S", outro: "Shots through the window!",
        },
        {
            type: "combat",
            enemies: foes(PLACES.restaurant, 4, 16),
            waves: waves([[20, PLACES.restaurant, 4, 24, undefined, "A SECOND CAR"]]),
            allies: [ally("Don Salieri", PLACES.restaurant)],
            objective: "Protect the Don",
            hint: "If Salieri dies, it is over.",
            fallbackSeconds: 50,
            checkpoint: true, checkpointSpawn: PLACES.restaurant,
            outroTitle: "CLEAR", outro: "Get the Don to his car",
        },
        {
            type: "escort", ally: { name: "Don Salieri", pos: PLACES.restaurant.pos }, target: PLACES.lobby.pos, radius: 14,
            enemies: foes(PLACES.restaurant, 2, 26),
            objective: "Get Don Salieri to his car",
            hint: "He follows whoever is closest. Stay between him and the shooters.",
            failText: "The Don was hit.",
            outroTitle: "THE DON IS SAFE", outro: "The shooters' sedan is heading for the station. Go!",
        },
        {
            type: "goto", who: "any", target: PLACES.station.pos, radius: 28, timeLimit: 240,
            objective: "Chase the shooters to the station",
            hint: "4 minutes. Take the Don's car.",
            failText: "They got away.",
            outroTitle: "CORNERED", outro: "Their car is wrecked at the station",
        },
        {
            type: "combat", enemies: foes(PLACES.station, 3, 14),
            objective: "Finish the shooters",
            hint: "No witnesses for Morello.",
            fallbackSeconds: 30,
            outroTitle: "BON APPETIT", outro: "Morello declared war today",
        },
    ],
};
