const { PLACES, WEATHER } = require("./places.js");
const { thugs, ally } = require("./helpers.js");

/**
 * Chapter 6 - Sarah (1932)
 * Walk Sarah home through the back streets. Nobody drives, nobody wanders
 * off. The hoodlums who have been bothering her are waiting in the alley.
 */
module.exports = {
    id: "06_sarah",
    title: "SARAH",
    subtitle: "Walk her home",
    description: "On foot, side by side, from Salieri's to Sarah's place. Then the alley.",
    environment: { weather: WEATHER.sarah, time: 20.5 },
    spawn: PLACES.salieriBar,
    vehicles: {},
    weapons: [],
    steps: [
        {
            type: "escort", ally: { name: "Sarah", pos: PLACES.salieriBar.pos }, target: PLACES.sarahHome.pos, radius: 14,
            objective: "Walk Sarah home",
            hint: "On foot. She follows whoever is closest. Keep her out of trouble.",
            failText: "Sarah got hurt.",
            checkpoint: true, checkpointSpawn: PLACES.sarahHome,
            environment: { weather: WEATHER.nightRain, time: 23.0 },
            outroTitle: "HER DOOR", outro: "The hoodlums. Right there in the alley.",
        },
        {
            type: "combat", enemies: thugs(PLACES.sarahHome, 4, 10),
            objective: "Teach the hoodlums a lesson",
            hint: "Fists only. Nobody runs.",
            fallbackSeconds: 40,
            outroTitle: "LESSON TAUGHT", outro: "Sarah is safe. For tonight.",
        },
        {
            type: "together", target: PLACES.salieriBar.pos, radius: 14, maxSpread: 40, graceSeconds: 15,
            objective: "Get back to the bar",
            hint: "Any way you like. Keep it tight.",
        },
    ],
};
