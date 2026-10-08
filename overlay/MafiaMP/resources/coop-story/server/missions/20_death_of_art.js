const { PLACES, PARKING, WEAPONS, WEATHER } = require("./places.js");
const { foes, mark, waves, car, TOMMY } = require("./helpers.js");

/**
 * Chapter 20 - The Death of Art (1938)
 * Sam sold everyone out. The art gallery: three floors of Salieri's men, then
 * Sam himself at the top. Then the long drive out of Lost Heaven.
 */
module.exports = {
    id: "20_death_of_art",
    title: "THE DEATH OF ART",
    subtitle: "Sam",
    description: "Fight through the gallery to Sam. Then leave Lost Heaven for good.",
    environment: { weather: WEATHER.foggy, time: 5.5 },
    spawn: PLACES.lobby,
    vehicles: {
        car: car("lassiter_v16_appolyon", PARKING.lotA, "The last car"),
    },
    weapons: [[WEAPONS.tommy, 300], [WEAPONS.pistol, 120]],
    steps: [
        {
            type: "goto", who: "all", target: PLACES.gallery.pos, radius: 30,
            objective: "Go to the art gallery",
            hint: "Sam said to come alone. You did not.",
            checkpoint: true, checkpointSpawn: PLACES.gallery,
            outroTitle: "THE GALLERY", outro: "It is a trap. Of course it is.",
        },
        {
            type: "combat",
            enemies: foes(PLACES.gallery, 4, 14, TOMMY),
            waves: waves([[25, PLACES.gallery, 4, 22, TOMMY, "SECOND FLOOR"], [50, PLACES.gallery, 5, 28, TOMMY, "THIRD FLOOR"]]),
            objective: "Fight up through the gallery",
            hint: "Floor by floor. Sam is at the top.",
            fallbackSeconds: 75,
            checkpoint: true, checkpointSpawn: PLACES.gallery,
            outroTitle: "THE TOP FLOOR", outro: "Sam",
        },
        {
            type: "killTarget", target: mark("Sam", PLACES.gallery, TOMMY, 200), bodyguards: foes(PLACES.gallery, 2, 10, TOMMY),
            objective: "Sam",
            hint: "He will not go quietly.",
            fallbackSeconds: 25,
            checkpoint: true, checkpointSpawn: PLACES.gallery,
            outroTitle: "THE DEATH OF ART", outro: "Nothing is ever the same",
            environment: { weather: WEATHER.morning, time: 7.0 },
        },
        {
            type: "board", vehicle: "car",
            objective: "Everyone in the car",
            hint: "There is nothing left here.",
        },
        {
            type: "goto", vehicle: "car", target: PLACES.airport.pos, radius: 40, timeLimit: 480,
            objective: "Leave Lost Heaven. Get to the airport",
            hint: "8 minutes. One last drive through the city.",
            failText: "Salieri's men found you first.",
            outroTitle: "THE END", outro: "You made it out of Lost Heaven. For now.",
        },
    ],
};
