const { PLACES, PARKING, WEAPONS, WEATHER } = require("./places.js");
const { foes, guards, mark, waves, car } = require("./helpers.js");

/**
 * Chapter 14 - Happy Birthday (1935)
 * The councilman celebrates on the steamboat. Board quietly, get to him, and
 * fight your way off the ship to the getaway boat.
 */
module.exports = {
    id: "14_happy_birthday",
    title: "HAPPY BIRTHDAY",
    subtitle: "The steamboat",
    description: "Board the councilman's party, find him, and get off the boat alive.",
    environment: { weather: WEATHER.boat, time: 17.0 },
    spawn: PLACES.lobby,
    vehicles: {
        getaway: car("bolt_ace", PARKING.bridge1, "Getaway"),
    },
    weapons: [[WEAPONS.pistol, 90]],
    steps: [
        {
            type: "goto", who: "all", target: PLACES.steamboat.pos, radius: 24,
            objective: "Get to the pier",
            hint: "The boat leaves at sunset.",
            checkpoint: true, checkpointSpawn: PLACES.steamboat,
            outroTitle: "THE PIER", outro: "Guests only. Look like one.",
        },
        {
            type: "stealth", target: PLACES.steamboat.pos, radius: 6, detectRadius: 8, onAlarm: "combat", who: "any",
            guards: guards(PLACES.steamboat, 4, 16),
            objective: "Board the steamboat without raising the alarm",
            hint: "Keep your distance from the guards. One of you reaching the gangway is enough.",
            outroTitle: "ABOARD", outro: "The councilman is on the upper deck",
        },
        {
            type: "killTarget", target: mark("The councilman", PLACES.steamboat), bodyguards: foes(PLACES.steamboat, 3, 12),
            objective: "Find the councilman",
            hint: "He has three bodyguards and the whole crew is armed.",
            fallbackSeconds: 20,
            checkpoint: true, checkpointSpawn: PLACES.steamboat,
            environment: { weather: WEATHER.sunset, time: 19.5 },
            outroTitle: "HAPPY BIRTHDAY", outro: "Every gun on the boat is pointed at you",
        },
        {
            type: "combat",
            enemies: foes(PLACES.steamboat, 4, 18),
            waves: waves([[20, PLACES.steamboat, 4, 26, undefined, "LOWER DECK"]]),
            objective: "Fight your way off the boat",
            hint: "Deck by deck to the stern.",
            fallbackSeconds: 50,
            outroTitle: "OFF THE BOAT", outro: "The getaway is waiting by the bridge",
        },
        {
            type: "goto", who: "all", target: PLACES.salieriBar.pos, radius: 25, timeLimit: 360,
            objective: "Get back to Salieri's",
            hint: "6 minutes. The whole harbor is awake.",
            failText: "The police were waiting at the pier.",
            environment: { weather: WEATHER.nightRain, time: 22.0 },
        },
    ],
};
