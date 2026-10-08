const { PLACES, PARKING, WEAPONS, WEATHER } = require("./places.js");
const { foes, waves, car } = require("./helpers.js");

/**
 * Chapter 19 - Moonlighting (1938)
 * Paulie's bank job. Hold the lobby while the vault is opened, then run from
 * the whole police force. Paulie does not come home from this one.
 */
module.exports = {
    id: "19_moonlighting",
    title: "MOONLIGHTING",
    subtitle: "Paulie's job",
    description: "Rob the First National Bank with Paulie. Hold the lobby, crack the vault, outrun the police.",
    environment: { weather: WEATHER.day, time: 10.5 },
    spawn: PLACES.lobby,
    vehicles: {
        getaway: car("bolt_v8", PARKING.lotA, "Getaway"),
    },
    weapons: [[WEAPONS.tommy, 250], [WEAPONS.pistol, 90]],
    steps: [
        {
            type: "board", vehicle: "getaway",
            objective: "Everyone in the getaway car",
            hint: "Paulie's plan. What could go wrong.",
        },
        {
            type: "goto", vehicle: "getaway", target: PLACES.bank.pos, radius: 26,
            objective: "Drive to the First National Bank",
            hint: "Park where the car can leave fast.",
            checkpoint: true, checkpointSpawn: PLACES.bank,
            outroTitle: "THE BANK", outro: "Masks on",
        },
        {
            type: "combat", enemies: foes(PLACES.bank, 3, 14),
            objective: "Take the lobby",
            hint: "The guards first.",
            fallbackSeconds: 30,
            checkpoint: true, checkpointSpawn: PLACES.bank,
            outroTitle: "LOBBY'S OURS", outro: "Paulie is on the vault. Hold the doors.",
        },
        {
            type: "defend", seconds: 75, area: PLACES.bank.pos, radius: 35, graceSeconds: 12,
            waves: waves([[10, PLACES.bank, 3, 30, undefined, "FIRST PATROL"], [35, PLACES.bank, 4, 32, undefined, "SECOND PATROL"], [60, PLACES.bank, 4, 34, undefined, "THEY BROUGHT EVERYONE"]]),
            objective: "Hold the bank while Paulie opens the vault",
            hint: "75 seconds. Nobody leaves the bank.",
            failText: "You left Paulie in the vault.",
            outroTitle: "VAULT'S OPEN", outro: "Grab the bags. Out the front.",
        },
        {
            type: "board", vehicle: "getaway", summonVehicles: ["getaway"],
            objective: "Back in the getaway car",
            hint: "Bags in the back. Go, go, go.",
        },
        {
            type: "goto", vehicle: "getaway", target: PLACES.salieriBar.pos, radius: 25, timeLimit: 300,
            enemies: foes(PLACES.bank, 3, 28),
            objective: "Lose the police and get the money to the bar",
            hint: "5 minutes. Keep everyone in the car.",
            failText: "The police got the car.",
            environment: { weather: WEATHER.overcast, time: 12.0 },
            outroTitle: "MOONLIGHTING", outro: "Paulie never gets to spend his share",
        },
    ],
};
