const { PLACES, PARKING, WEAPONS, WEATHER } = require("./places.js");
const { foes, car } = require("./helpers.js");

/**
 * Chapter 1 - An Offer You Can't Refuse (1930)
 * Tommy's taxi night. One player drives the cab, the rest ride along. Paulie
 * and Sam's fares to the station and the airport, Morello's men waiting at
 * the airport, then the escape back to Salieri's.
 */
module.exports = {
    id: "01_offer",
    title: "AN OFFER YOU CAN'T REFUSE",
    subtitle: "Lost Heaven, 1930",
    description: "A cab, two fares and a bad night. Nobody leaves the taxi.",
    environment: { weather: WEATHER.cloudy, time: 22.0 },
    spawn: PLACES.lobby,
    vehicles: {
        taxi: car("shubert_six_taxi", PARKING.lotA, "Taxi"),
    },
    weapons: [[WEAPONS.pistol, 60]],
    steps: [
        {
            type: "board", vehicle: "taxi",
            objective: "Everyone get in the taxi",
            hint: "One of you drives. The rest are the fare.",
        },
        {
            type: "goto", vehicle: "taxi", target: PLACES.station.pos, radius: 28,
            objective: "Take the fare to Little Italy station",
            hint: "Keep the whole crew in the cab.",
            checkpoint: true, checkpointSpawn: PLACES.station,
            outroTitle: "FARE PAID", outro: "Two more gentlemen want a ride to the airport",
        },
        {
            type: "wait", seconds: 6,
            objective: "New fare: two gentlemen for the airport",
            hint: "They are in a hurry.",
        },
        {
            type: "goto", vehicle: "taxi", target: PLACES.airport.pos, radius: 35, timeLimit: 420,
            objective: "Drive the fare to the airport",
            hint: "You have 7 minutes. Cross town, follow the main roads.",
            failText: "The fare missed the flight.",
            checkpoint: true, checkpointSpawn: PLACES.airport,
            outroTitle: "AIRPORT", outro: "Morello's boys are waiting outside",
            environment: { weather: WEATHER.rainy, time: 23.5 },
        },
        {
            type: "combat", enemies: foes(PLACES.airport, 4, 18),
            objective: "Morello's men. Deal with them",
            hint: "Use the cab for cover. Nobody drives off without the crew.",
            fallbackSeconds: 30,
            checkpoint: true, checkpointSpawn: PLACES.airport,
            outroTitle: "CLEAR", outro: "More of them will be on the way",
        },
        {
            type: "survive", seconds: 45, stayIn: "taxi",
            objective: "Lose Morello's men - keep driving for 45 seconds",
            hint: "Stay in the taxi. If anyone bails, the ride is over.",
        },
        {
            type: "goto", vehicle: "taxi", target: PLACES.salieriBar.pos, radius: 25,
            objective: "Get the cab back to Salieri's Bar",
            hint: "Salieri owes you one.",
            environment: { weather: WEATHER.nightRain, time: 1.0 },
            outroTitle: "SALIERI'S", outro: "Welcome to the family",
        },
    ],
};
