const { PLACES, PARKING, WEAPONS, WEATHER } = require("./places.js");
const { foes, waves, car, TOMMY } = require("./helpers.js");

/**
 * Chapter 12 - Great Deal (1933)
 * Ambush the weapons deal at the parking garage with Paulie and Sam. It goes
 * wrong on every floor. Then get out of there.
 */
module.exports = {
    id: "12_great_deal",
    title: "GREAT DEAL",
    subtitle: "The parking garage",
    description: "Crash the deal at the parking garage. Three floors of trouble, then run.",
    environment: { weather: WEATHER.parking, time: 22.5 },
    spawn: PLACES.lobby,
    vehicles: {
        sedan: car("bolt_v8", PARKING.lotA, "The Bolt"),
    },
    weapons: [[WEAPONS.tommy, 200], [WEAPONS.pistol, 90]],
    steps: [
        {
            type: "board", vehicle: "sedan",
            objective: "Everyone in the Bolt",
            hint: "Thompsons in the back seat.",
        },
        {
            type: "goto", vehicle: "sedan", target: PLACES.parkingGarage.pos, radius: 26,
            objective: "Drive to the parking garage",
            hint: "The deal is on the top floor.",
            checkpoint: true, checkpointSpawn: PLACES.parkingGarage,
            outroTitle: "THE GARAGE", outro: "Ground floor is theirs",
        },
        {
            type: "combat",
            enemies: foes(PLACES.parkingGarage, 4, 14, TOMMY),
            waves: waves([[25, PLACES.parkingGarage, 4, 22, TOMMY, "SECOND FLOOR"], [55, PLACES.parkingGarage, 5, 28, TOMMY, "TOP FLOOR"]]),
            objective: "Fight up through the garage",
            hint: "Floor by floor. They have Thompsons too.",
            fallbackSeconds: 70,
            checkpoint: true, checkpointSpawn: PLACES.parkingGarage,
            outroTitle: "THE DEAL IS OFF", outro: "Sirens. Back to the car.",
        },
        {
            type: "board", vehicle: "sedan", summonVehicles: ["sedan"],
            objective: "Back to the Bolt",
            hint: "It is where you parked it.",
        },
        {
            type: "goto", vehicle: "sedan", target: PLACES.salieriBar.pos, radius: 25, timeLimit: 300,
            objective: "Lose the police and get back to the bar",
            hint: "5 minutes.",
            failText: "The police boxed you in.",
            outroTitle: "GREAT DEAL", outro: "The guns are Salieri's now",
        },
    ],
};
