const { PLACES, PARKING, WEAPONS, WEATHER } = require("./places.js");
const { foes, waves, car } = require("./helpers.js");

/**
 * Chapter 7 - Better Get Used To It (1932)
 * Payback on the hoodlum gang with Paulie: drive to their hangout, clean it
 * out, and run down the leader when he bolts.
 */
module.exports = {
    id: "07_better_get_used_to_it",
    title: "BETTER GET USED TO IT",
    subtitle: "Payback",
    description: "Drive to the hoodlums' hangout, clear it, catch the one who runs.",
    environment: { weather: WEATHER.hoodlums, time: 14.0 },
    spawn: PLACES.lobby,
    vehicles: {
        sedan: car("bolt_v8", PARKING.lotA, "Paulie's Bolt"),
    },
    weapons: [[WEAPONS.pistol, 120]],
    steps: [
        {
            type: "board", vehicle: "sedan",
            objective: "Get in with Paulie",
            hint: "Baseball bats are in the trunk. Pistols are on you.",
        },
        {
            type: "goto", vehicle: "sedan", target: PLACES.hoodlumHangout.pos, radius: 26,
            objective: "Drive to the hoodlums' hangout",
            hint: "They will not be expecting you.",
            checkpoint: true, checkpointSpawn: PLACES.hoodlumHangout,
            outroTitle: "THE HANGOUT", outro: "There they are",
        },
        {
            type: "combat",
            enemies: foes(PLACES.hoodlumHangout, 3, 12),
            waves: waves([[20, PLACES.hoodlumHangout, 4, 22, undefined, "THE REST OF THEM"]]),
            objective: "Clean out the hangout",
            hint: "Watch the second group coming from the street.",
            fallbackSeconds: 45,
            checkpoint: true, checkpointSpawn: PLACES.hoodlumHangout,
            outroTitle: "CLEARED", outro: "The leader is running. After him!",
        },
        {
            type: "chase", runner: { name: "The leader", pos: PLACES.hoodlumHangout.pos }, catchRadius: 4,
            path: [PLACES.garageRow.pos, PLACES.salieriBar.pos],
            objective: "Catch the leader before he gets away",
            hint: "On foot or in the car. Get within arm's reach.",
            failText: "He got away.",
            outroTitle: "GOT HIM", outro: "He will not bother anybody again",
        },
    ],
};
