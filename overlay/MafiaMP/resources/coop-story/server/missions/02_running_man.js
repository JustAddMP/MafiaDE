const { PLACES, WEATHER } = require("./places.js");
const { thugs } = require("./helpers.js");

/**
 * Chapter 2 - The Running Man (1930)
 * Morello's thugs wrecked the cab at the station. Get to the back door of
 * Salieri's Bar on foot, together, with the thugs on your heels.
 */
module.exports = {
    id: "02_running_man",
    title: "THE RUNNING MAN",
    subtitle: "No cab this time",
    description: "Run. Four minutes to Salieri's, and keep each other in sight.",
    environment: { weather: WEATHER.nightRain, time: 2.0 },
    spawn: PLACES.station,
    vehicles: {},
    weapons: [],
    steps: [
        {
            type: "together", target: PLACES.salieriBar.pos, radius: 14, maxSpread: 60, graceSeconds: 15,
            noVehicles: true, timeLimit: 240,
            enemies: thugs(PLACES.station, 3, 16),
            objective: "Reach Salieri's Bar on foot",
            hint: "4 minutes. Stay within 60 m of each other. The thugs are right behind you.",
            failText: "They caught up with you.",
            outroTitle: "SAFE", outro: "Salieri's door opens",
        },
        {
            type: "wait", seconds: 8,
            objective: "Catch your breath inside",
            hint: "Frank is on the phone. Something about a job for you.",
            environment: { weather: WEATHER.morning, time: 8.0 },
        },
    ],
};
