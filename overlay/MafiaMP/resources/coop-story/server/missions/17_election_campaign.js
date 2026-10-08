const { PLACES, PARKING, WEAPONS, WEATHER } = require("./places.js");
const { foes, guards, mark, car } = require("./helpers.js");

/**
 * Chapter 17 - Election Campaign (1938)
 * The councilman gives his speech by the old prison. Get inside the ruin
 * unseen, take the shot from the perch, and get out before his men close in.
 */
module.exports = {
    id: "17_election_campaign",
    title: "ELECTION CAMPAIGN",
    subtitle: "One shot",
    description: "Sneak into the old prison, take out the councilman from the perch, and get away.",
    environment: { weather: WEATHER.noon, time: 12.0 },
    spawn: PLACES.lobby,
    vehicles: {
        sedan: car("shubert_six", PARKING.lotA, "The Shubert"),
    },
    weapons: [[WEAPONS.pistol, 120]],
    steps: [
        {
            type: "goto", who: "all", target: PLACES.prison.pos, radius: 40,
            objective: "Drive out to the old prison",
            hint: "Everyone arrives, then on foot.",
            checkpoint: true, checkpointSpawn: PLACES.prison,
            outroTitle: "THE PRISON", outro: "The rally is below. Guards on the gate.",
        },
        {
            type: "stealth", target: PLACES.prison.pos, radius: 6, detectRadius: 10, onAlarm: "combat", who: "any",
            guards: guards(PLACES.prison, 4, 18),
            objective: "Get to the perch inside the prison unseen",
            hint: "One of you reaching the perch is enough. Spotted means a fight.",
            checkpoint: true, checkpointSpawn: PLACES.prison,
            outroTitle: "THE PERCH", outro: "The councilman is at the podium",
        },
        {
            type: "killTarget", target: mark("The councilman", PLACES.prison), bodyguards: foes(PLACES.prison, 3, 14),
            objective: "Take the shot",
            hint: "The councilman first. Then his bodyguards will come for you.",
            fallbackSeconds: 20,
            outroTitle: "DONE", outro: "Every cop in the county heard that",
        },
        {
            type: "goto", who: "all", target: PLACES.salieriBar.pos, radius: 25, timeLimit: 420,
            enemies: foes(PLACES.prison, 4, 30),
            objective: "Get out and back to the bar",
            hint: "7 minutes. Everyone makes it or nobody does.",
            failText: "They cut you off.",
            environment: { weather: WEATHER.sunset, time: 18.0 },
            outroTitle: "ELECTION CAMPAIGN", outro: "Salieri's candidate wins",
        },
    ],
};
