const { PLACES, PARKING, WEAPONS, WEATHER } = require("./places.js");
const { foes, guards, car } = require("./helpers.js");

/**
 * Chapter 11 - Visiting Rich People (1933)
 * Night break-in at the prosecutor's mansion on Oak Hill. Get past the guards,
 * steal the documents, and get out before the police arrive.
 */
module.exports = {
    id: "11_visiting_rich_people",
    title: "VISITING RICH PEOPLE",
    subtitle: "Oak Hill",
    description: "Sneak into the prosecutor's mansion, take the papers, get out. Loud is allowed, but it costs.",
    environment: { weather: WEATHER.mansion, time: 1.0 },
    spawn: PLACES.lobby,
    vehicles: {
        sedan: car("shubert_six", PARKING.lotA, "Black Shubert"),
    },
    weapons: [[WEAPONS.pistol, 60]],
    steps: [
        {
            type: "goto", who: "all", target: PLACES.mansion.pos, radius: 45,
            objective: "Drive up to the mansion. Park out of sight",
            hint: "Everyone arrives, then on foot.",
            checkpoint: true, checkpointSpawn: PLACES.mansion,
            outroTitle: "THE MANSION", outro: "Guards on the grounds, dogs in the yard",
        },
        {
            type: "stealth", target: PLACES.mansion.pos, radius: 6, detectRadius: 10, onAlarm: "combat", who: "any",
            guards: guards(PLACES.mansion, 4, 18),
            objective: "Get into the house unseen",
            hint: "Four guards walk the grounds. One of you reaching the door is enough.",
            outroTitle: "INSIDE", outro: "The study is upstairs",
        },
        {
            type: "wait", seconds: 25, countdown: true,
            enemies: foes(PLACES.mansion, 2, 24),
            objective: "Find the documents in the study",
            hint: "25 seconds. Keep the guards off whoever is searching.",
            checkpoint: true, checkpointSpawn: PLACES.mansion,
            outroTitle: "GOT THEM", outro: "Alarm. The police are coming. Out!",
        },
        {
            type: "goto", who: "all", target: PLACES.salieriBar.pos, radius: 25, timeLimit: 420,
            enemies: foes(PLACES.mansion, 4, 30),
            objective: "Escape to Salieri's with the papers",
            hint: "7 minutes. Everyone has to make it back.",
            failText: "The police caught you with the papers.",
            environment: { weather: WEATHER.nightRain, time: 2.5 },
            outroTitle: "DELIVERED", outro: "Salieri's lawyers will enjoy these",
        },
    ],
};
