const { PLACES, PARKING, WEAPONS, WEATHER } = require("./places.js");
const { foes, waves, car } = require("./helpers.js");

/**
 * Chapter 9 - A Trip to the Country (1933)
 * Drive the truck out to the farm for the whiskey. Morello's men are already
 * there. Fight through the farm, load up, and get the truck home under fire.
 */
module.exports = {
    id: "09_trip_to_the_country",
    title: "A TRIP TO THE COUNTRY",
    subtitle: "Whiskey run",
    description: "Truck to the farm, fight for the whiskey, truck back. Nobody leaves the truck on the way home.",
    environment: { weather: WEATHER.farm, time: 6.5 },
    spawn: PLACES.lobby,
    vehicles: {
        truck: car("bolt_truck", PARKING.lotD, "The truck"),
    },
    weapons: [[WEAPONS.pistol, 120]],
    steps: [
        {
            type: "board", vehicle: "truck",
            objective: "Everyone on the truck",
            hint: "Cab or bed, nobody stays behind.",
        },
        {
            type: "goto", vehicle: "truck", target: PLACES.farm.pos, radius: 34, timeLimit: 540,
            objective: "Drive the truck out to the farm",
            hint: "9 minutes. Keep the crew on the truck.",
            failText: "You were too late. The whiskey is gone.",
            checkpoint: true, checkpointSpawn: PLACES.farm,
            outroTitle: "THE FARM", outro: "Morello's men are already here",
        },
        {
            type: "combat",
            enemies: foes(PLACES.farm, 4, 18),
            waves: waves([[20, PLACES.farm, 3, 26, undefined, "FROM THE BARN"], [45, PLACES.farm, 4, 30, undefined, "FROM THE FIELDS"]]),
            objective: "Clear the farm",
            hint: "Barn first, then the fields.",
            fallbackSeconds: 60,
            checkpoint: true, checkpointSpawn: PLACES.farm,
            outroTitle: "FARM CLEARED", outro: "Load the whiskey",
        },
        {
            type: "wait", seconds: 20, countdown: true,
            objective: "Load the whiskey onto the truck",
            hint: "Keep watch while it is loaded.",
        },
        {
            type: "board", vehicle: "truck", summonVehicles: ["truck"],
            objective: "Everyone back on the truck",
            hint: "The truck is by the farm gate.",
        },
        {
            type: "survive", seconds: 40, stayIn: "truck",
            enemies: foes(PLACES.farm, 3, 30),
            objective: "Get the truck moving. They are shooting at it",
            hint: "Drive. Nobody gets off.",
            environment: { weather: WEATHER.overcast, time: 9.0 },
        },
        {
            type: "goto", vehicle: "truck", target: PLACES.salieriBar.pos, radius: 28, timeLimit: 600,
            objective: "Bring the whiskey back to Salieri's",
            hint: "10 minutes, with the police on the lookout for a truck.",
            failText: "The whiskey never made it.",
            outroTitle: "DELIVERED", outro: "Salieri's is wet again",
        },
    ],
};
