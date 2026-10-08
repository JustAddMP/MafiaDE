const { PLACES, PARKING, WEAPONS, WEATHER } = require("./places.js");
const { foes, guards, waves, car } = require("./helpers.js");

/**
 * Chapter 18 - Just for Relaxation (1938)
 * The cigar shipment at the harbor. Get in quietly, take both trucks, and
 * drive them out past Morello's leftovers and the police.
 */
module.exports = {
    id: "18_just_for_relaxation",
    title: "JUST FOR RELAXATION",
    subtitle: "Cigars",
    description: "Steal the cigar trucks from the harbor. Quietly in, loudly out.",
    environment: { weather: WEATHER.harbor, time: 23.5 },
    spawn: PLACES.lobby,
    vehicles: {
        truckA: car("bolt_truck", PARKING.grid5, "Truck 1"),
        truckB: car("bolt_truck", PARKING.grid6, "Truck 2"),
        ride: car("bolt_model_b", PARKING.lotA, "The ride"),
    },
    weapons: [[WEAPONS.pistol, 120]],
    steps: [
        {
            type: "goto", who: "all", target: PLACES.harbor.pos, radius: 30,
            objective: "Get to the harbor",
            hint: "The trucks are parked by the river row.",
            checkpoint: true, checkpointSpawn: PLACES.harbor,
            outroTitle: "THE HARBOR", outro: "Guards on the trucks",
        },
        {
            type: "stealth", target: PLACES.riverRow.pos, radius: 10, detectRadius: 9, onAlarm: "combat", who: "any",
            guards: guards(PLACES.riverRow, 4, 18),
            objective: "Reach the trucks without being seen",
            hint: "If they see you, take them all before the trucks get hit.",
            checkpoint: true, checkpointSpawn: PLACES.riverRow,
            outroTitle: "THE TRUCKS", outro: "Two trucks, two drivers. Go.",
        },
        {
            type: "convoy", vehicles: ["truckA", "truckB"], target: PLACES.garageRow.pos, radius: 26,
            waves: waves([[15, PLACES.riverRow, 4, 26, undefined, "THEY HEARD THE ENGINES"]]),
            objective: "Drive both trucks to the garage",
            hint: "Two drivers, two trucks. Solo? Two trips.",
            checkpoint: true, checkpointSpawn: PLACES.garageRow,
            outroTitle: "DELIVERED", outro: "Both trucks are in. Someone followed.",
        },
        {
            type: "combat", enemies: foes(PLACES.garageRow, 4, 20),
            objective: "Deal with the men who followed the trucks",
            hint: "Keep them away from the cargo.",
            fallbackSeconds: 40,
            outroTitle: "JUST FOR RELAXATION", outro: "Salieri lights one up",
        },
    ],
};
