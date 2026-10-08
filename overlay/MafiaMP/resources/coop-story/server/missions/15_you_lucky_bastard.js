const { PLACES, PARKING, WEAPONS, WEATHER } = require("./places.js");
const { foes, mark, waves, car, TOMMY } = require("./helpers.js");

/**
 * Chapter 15 - You Lucky Bastard (1935)
 * Sergio Morello has nine lives. The bomb under his car, the ambush, and
 * finally the harbor warehouse where the luck runs out.
 */
module.exports = {
    id: "15_you_lucky_bastard",
    title: "YOU LUCKY BASTARD",
    subtitle: "Sergio",
    description: "Three tries at Sergio Morello. The last one is at the harbor.",
    environment: { weather: WEATHER.morning, time: 7.5 },
    spawn: PLACES.lobby,
    vehicles: {
        sedan: car("shubert_six", PARKING.lotA, "The Shubert"),
    },
    weapons: [[WEAPONS.tommy, 200], [WEAPONS.pistol, 90]],
    steps: [
        {
            type: "goto", who: "any", target: PLACES.morelloHotel.pos, radius: 16,
            objective: "Plant the bomb under Sergio's car outside the hotel",
            hint: "One of you gets to the car. The rest watch the street.",
            checkpoint: true, checkpointSpawn: PLACES.morelloHotel,
            outroTitle: "PLANTED", outro: "Now wait for him",
        },
        {
            type: "survive", seconds: 30,
            objective: "Wait for Sergio to leave the hotel",
            hint: "Keep out of sight.",
            outroTitle: "DAMN", outro: "His driver took the car. Sergio walks. Try again at the harbor.",
        },
        {
            type: "combat", enemies: foes(PLACES.morelloHotel, 4, 18),
            objective: "His bodyguards saw you. Deal with them",
            hint: "Quick, before the street fills with cops.",
            fallbackSeconds: 40,
            checkpoint: true, checkpointSpawn: PLACES.morelloHotel,
            outroTitle: "GONE", outro: "He is heading for the harbor warehouse",
        },
        {
            type: "goto", who: "all", target: PLACES.harbor.pos, radius: 30, timeLimit: 360,
            objective: "Get to the harbor warehouse",
            hint: "6 minutes.",
            failText: "Sergio's boat left.",
            checkpoint: true, checkpointSpawn: PLACES.harbor,
            environment: { weather: WEATHER.harbor, time: 21.0 },
            outroTitle: "THE WAREHOUSE", outro: "He is in there with everything he has",
        },
        {
            type: "combat",
            enemies: foes(PLACES.harbor, 4, 16, TOMMY),
            waves: waves([[25, PLACES.harbor, 4, 24, TOMMY, "FROM THE DOCKS"]]),
            objective: "Clear the warehouse",
            hint: "Thompsons on both sides.",
            fallbackSeconds: 60,
            checkpoint: true, checkpointSpawn: PLACES.harbor,
        },
        {
            type: "killTarget", target: mark("Sergio Morello", PLACES.harbor, TOMMY, 150), bodyguards: foes(PLACES.harbor, 2, 10, TOMMY),
            objective: "Kill Sergio Morello",
            hint: "No more luck.",
            fallbackSeconds: 20,
            outroTitle: "YOU LUCKY BASTARD", outro: "Not this time",
        },
    ],
};
