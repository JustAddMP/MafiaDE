const { PLACES, PARKING, WEAPONS, WEATHER } = require("./places.js");
const { foes, guards, car } = require("./helpers.js");

/**
 * Chapter 3 - Molotov Party (1930)
 * Paulie and Sam's job: sneak into Morello's parking lot, deal with the
 * guards, take two of his cars to the chop shop at the autodrome and get
 * everyone back to the bar before Morello's men catch up.
 */
module.exports = {
    id: "03_molotov_party",
    title: "MOLOTOV PARTY",
    subtitle: "Two cars, one night",
    description: "Sneak into Morello's lot, take both cars to the chop shop, then everyone back to the bar.",
    environment: { weather: WEATHER.overcast, time: 21.0 },
    spawn: PLACES.lobby,
    vehicles: {
        carA: car("lassiter_v16", PARKING.bridge1, "Lassiter V16"),
        carB: car("bolt_v8", PARKING.lotD, "Bolt V8"),
        getaway: car("bolt_model_b", PARKING.lotC, "Getaway"),
    },
    weapons: [[WEAPONS.pistol, 60]],
    steps: [
        {
            type: "stealth", target: PLACES.morelloLot.pos, radius: 8, detectRadius: 9, onAlarm: "combat", who: "any",
            guards: guards(PLACES.morelloLot, 3, 16),
            objective: "Get into Morello's parking lot without being seen",
            hint: "Guards walk the lot. Stay out of their sight, or take them quietly.",
            checkpoint: true, checkpointSpawn: PLACES.morelloLot,
            outroTitle: "IN", outro: "The Lassiter is right there. The Bolt is back at the lot by the bar.",
        },
        {
            type: "convoy", vehicles: ["carA", "carB"], target: PLACES.autodrome.pos, radius: 30,
            objective: "Deliver both cars to the chop shop at the autodrome",
            hint: "Two drivers, two cars. Solo? Make two trips.",
            checkpoint: true, checkpointSpawn: PLACES.autodrome,
            outroTitle: "CHOP SHOP", outro: "Both cars are off the street. Someone followed you.",
            environment: { weather: WEATHER.nightRain, time: 23.0 },
        },
        {
            type: "combat", enemies: foes(PLACES.autodrome, 4, 20),
            objective: "Morello's men followed you. Finish them",
            hint: "Keep them away from the cars.",
            fallbackSeconds: 30,
            checkpoint: true, checkpointSpawn: PLACES.autodrome,
        },
        {
            type: "goto", who: "all", target: PLACES.salieriBar.pos, radius: 25,
            objective: "Get the crew back to Salieri's Bar",
            hint: "Take anything with wheels. Everyone has to arrive.",
            outroTitle: "PAID", outro: "Salieri is pleased. Morello is not.",
        },
    ],
};
