const { PLACES, PARKING, WEAPONS, WEATHER } = require("./places.js");
const { foes, guards, mark, waves, car } = require("./helpers.js");

/**
 * Chapter 8 - The Saint and the Sinner (1932)
 * The informant hides in the brothel. Get in quietly, find him, then the
 * church: Morello's men turn the funeral into a shootout.
 */
module.exports = {
    id: "08_saint_and_sinner",
    title: "THE SAINT AND THE SINNER",
    subtitle: "Confession time",
    description: "The brothel first: find the rat. Then the church, where it all goes loud.",
    environment: { weather: WEATHER.brothel, time: 21.5 },
    spawn: PLACES.lobby,
    vehicles: {
        sedan: car("shubert_six", PARKING.lotA, "Shubert"),
    },
    weapons: [[WEAPONS.pistol, 90]],
    steps: [
        {
            type: "goto", who: "all", target: PLACES.brothel.pos, radius: 24,
            objective: "Go to the Hotel Corleone",
            hint: "Take the Shubert, or walk. Everyone has to arrive.",
            checkpoint: true, checkpointSpawn: PLACES.brothel,
        },
        {
            type: "stealth", target: PLACES.brothel.pos, radius: 6, detectRadius: 8, onAlarm: "combat", who: "any",
            guards: guards(PLACES.brothel, 3, 15),
            objective: "Get inside without alerting the doormen",
            hint: "Reach the entrance unseen. If they spot you, it is a shootout.",
            outroTitle: "INSIDE", outro: "The rat is upstairs",
        },
        {
            type: "killTarget", target: mark("The informant", PLACES.brothel), bodyguards: foes(PLACES.brothel, 2, 10),
            objective: "Find the informant and deal with him",
            hint: "He has two friends with him.",
            fallbackSeconds: 15,
            checkpoint: true, checkpointSpawn: PLACES.brothel,
            outroTitle: "DONE", outro: "Now the funeral. Morello's men will be there.",
            environment: { weather: WEATHER.funeral, time: 10.0 },
        },
        {
            type: "goto", who: "all", target: PLACES.church.pos, radius: 26,
            objective: "Drive to the church",
            hint: "The funeral is about to start.",
            checkpoint: true, checkpointSpawn: PLACES.church,
            outroTitle: "THE CHURCH", outro: "They are inside. Guns out.",
        },
        {
            type: "combat",
            enemies: foes(PLACES.church, 4, 14),
            waves: waves([[25, PLACES.church, 4, 24, undefined, "FROM THE STREET"]]),
            objective: "Shoot your way out of the church",
            hint: "Cover each other between the pews.",
            fallbackSeconds: 50,
            outroTitle: "AMEN", outro: "Get back to the bar before the cops arrive",
        },
        {
            type: "goto", who: "all", target: PLACES.salieriBar.pos, radius: 25, timeLimit: 300,
            objective: "Get back to Salieri's",
            hint: "5 minutes. Steal a car if you have to.",
            failText: "The police got you.",
        },
    ],
};
