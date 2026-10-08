/**
 * Known Lost Heaven coordinates (Mafia: Definitive Edition, Z-up).
 *
 * VERIFIED entries come from sources that were checked in-game:
 *   - MafiaMP freeroam resource (spawn point + vehicle lot rows)
 *   - MafiaMP debug UI "Teleport to Salieri's Bar"
 *   - NOMAD ScriptHook teleport table (Salieri's door, station, airport)
 *
 * STAND-IN entries are story locations whose real coordinates have not been
 * recorded yet. Each one currently aliases a verified spot nearby so every
 * chapter is playable today. To move a chapter to its real location, stand
 * on the spot in-game, type `/mark <name>` (name = the key below), paste the
 * printed line over the stand-in, and remove the name from UNVERIFIED.
 */

const { v3, quat } = require("../lib/vec.js");

const Q = quat;

// ----------------------------------------------------------------- verified
const VERIFIED = {
    // Team spawn in front of Salieri's (freeroam spawn point)
    lobby:          { pos: v3(-985.871, -299.401, 2.1),   rot: Q(0.291, 0, 0, -0.957) },
    // Pavement at Salieri's Bar door (debug teleport)
    salieriBar:     { pos: v3(-916.0, -210.0, 2.605),     rot: Q(1, 0, 0, 0) },
    // Salieri's, ScriptHook teleport spot (around the corner from the door)
    salieriCorner:  { pos: v3(-907.655, -230.692, 2.801) },
    // Little Italy train station forecourt (ScriptHook teleport)
    station:        { pos: v3(-560.6048, -136.93648, 13.01631) },
    // Lost Heaven airport (ScriptHook teleport)
    airport:        { pos: v3(1225.7761, 1174.7006, 29.581974) },
    // Autodrome / racetrack (freeroam /tp autodrome)
    autodrome:      { pos: v3(-1915.627, 10.715, 17.566), rot: Q(0.99, 0, 0, 0.139) },
    // Street row north of the bar (freeroam lot)
    garageRow:      { pos: v3(-914.322388, -120.731689, 3.724135), rot: Q(0.999, 0, 0, 0.044) },
    // Row near the bridge approach (freeroam lot)
    bridgeRow:      { pos: v3(-962.758606, -65.251747, 4.112756) },
    // Long row by the river (freeroam lot), used as race start grid
    riverRow:       { pos: v3(-970.537109, -97.723427, 3.664779) },
    // Side of the Salieri's lot, away from the cars
    lotSide:        { pos: v3(-1000.0, -330.0, 2.8) },
};

// ----------------------------------------------------------------- stand-ins
// Story locations -> nearest verified stand-in. Replace with /mark output.
const STAND_INS = {
    morelloLot:     VERIFIED.bridgeRow,      // ch3: Morello's parking lot
    shopRound:      VERIFIED.garageRow,      // ch4: protection round (shops)
    motel:          VERIFIED.autodrome,      // ch4: Clark's Motel (countryside)
    bertoneGarage:  VERIFIED.garageRow,      // ch5: Bertone's garage
    sarahHome:      VERIFIED.station,        // ch6: Sarah's place
    hoodlumHangout: VERIFIED.bridgeRow,      // ch7: hoodlum hideout
    brothel:        VERIFIED.riverRow,       // ch8: Hotel Corleone
    church:         VERIFIED.station,        // ch8: church funeral
    farm:           VERIFIED.autodrome,      // ch9: countryside farm
    frankBank:      VERIFIED.riverRow,       // ch10: the bank
    mansion:        VERIFIED.airport,        // ch11: prosecutor's mansion (Oak Hill)
    parkingGarage:  VERIFIED.garageRow,      // ch12: multi-storey garage
    restaurant:     VERIFIED.salieriCorner,  // ch13: Pepe's restaurant
    steamboat:      VERIFIED.riverRow,       // ch14: steamboat pier
    harbor:         VERIFIED.riverRow,       // ch15/18: harbor warehouse
    morelloHotel:   VERIFIED.bridgeRow,      // ch16: Morello's hotel
    prison:         VERIFIED.autodrome,      // ch17: the old prison (sniper perch)
    bank:           VERIFIED.riverRow,       // ch19: First National Bank
    gallery:        VERIFIED.station,        // ch20: art gallery
};

const PLACES = { ...VERIFIED, ...STAND_INS };
const UNVERIFIED = Object.keys(STAND_INS);

// Vehicle parking spots with exact orientation (freeroam lot) - safe to spawn cars on.
const PARKING = {
    lotA: { pos: v3(-986.40686, -304.061798, 2.292042),  rot: Q(0.70629, 0.006456, -0.004993, -0.707875) },
    lotB: { pos: v3(-985.365356, -336.348083, 2.892426), rot: Q(0.702591, 0.00959, 0.005905, -0.711505) },
    lotC: { pos: v3(-986.426086, -343.213989, 2.942883), rot: Q(0.734178, 0.009719, 0.005454, -0.678866) },
    lotD: { pos: v3(-1018.279968, -340.606567, 2.919076), rot: Q(0.72342, 0.023575, -0.020525, 0.689701) },
    lotE: { pos: v3(-1015.080139, -344.603149, 2.729358), rot: Q(0.70901, 0.019996, -0.011678, 0.704818) },
    grid1: { pos: v3(-970.537109, -97.723427, 3.664779),  rot: Q(1, 0, 0, 0) },
    grid2: { pos: v3(-973.756897, -98.136681, 3.633013),  rot: Q(1, 0, 0, 0) },
    grid3: { pos: v3(-977.302856, -98.2183, 3.608772),    rot: Q(1, 0, 0, 0) },
    grid4: { pos: v3(-980.90509, -97.99614, 3.381163),    rot: Q(1, 0, 0, 0) },
    grid5: { pos: v3(-984.109741, -98.175415, 3.511434),  rot: Q(1, 0, 0, 0) },
    grid6: { pos: v3(-986.901978, -98.312843, 3.462155),  rot: Q(1, 0, 0, 0) },
    garage1: { pos: v3(-914.322388, -120.731689, 3.724135), rot: Q(0.999, 0, 0, 0.044) },
    bridge1: { pos: v3(-962.758606, -65.251747, 4.112756), rot: Q(1, 0, 0, 0) },
    station1: { pos: v3(-560.6048, -136.93648, 13.01631), rot: Q(1, 0, 0, 0) },
    airport1: { pos: v3(1225.7761, 1180.7006, 29.581974), rot: Q(1, 0, 0, 0) },
    autodrome1: { pos: v3(-1915.627, 16.715, 17.566), rot: Q(0.99, 0, 0, 0.139) },
};

// Weapon ids as the MafiaMP freeroam resource / debug UI use them (numeric item ids).
// Only these are verified; others are unknown until an id table is reversed.
const WEAPONS = {
    pistol: 2,
    goldPistol: 3,
    tommy: 85,
};

// Weather sets. `_default_game*` are attested in MafiaMP's freeroam config; the
// `mm_*` per-chapter sets are listed by the MafiaMP debug UI and the ScriptHook
// WeatherIDs table (same names the game uses for story checkpoints).
const WEATHER = {
    day: "_default_game",
    cloudy: "_default_game_cloudy",
    foggy: "_default_game_foggy",
    morning: "_default_game_morning_sunny",
    overcast: "_default_game_overcast",
    rainy: "_default_game_rainy",
    nightRain: "cine_1700_night_rain",
    sunset: "cine_1700_sunset",
    noon: "cine_1700_noon",
    chase: "mm_010_chase_cp_020_escape",
    taxi: "mm_020_taxi_cp_010_arrival",
    molotov: "mm_030_molotov_cp_010_cine",
    motel: "mm_040_motel_cp_005_meet_salieri",
    race: "mm_050_race_cp_010",
    sarah: "mm_060_sarah_cp_010_cine_0600_sarah_intro",
    hoodlums: "mm_070_hoodlums_cp_010",
    brothel: "mm_080_brothel_cp_010_cs_start",
    farm: "mm_100_farm_cp_000",
    omerta: "mm_110_omerta_cp_010_cs_cs_park",
    funeral: "cine_1195_omerta_funeral_night",
    mansion: "mm_120_mansion_cp_010_cs_salvatore",
    parking: "mm_130_parking_cp_010_cine_parking",
    salieri: "mm_140_salieri_cp_010_cine_salieri",
    boat: "mm_150_boat_cp_010",
    harbor: "mm_160_harbor_cp_000_cinematic_night",
};

// Vehicle models verified spawnable by MafiaMP's freeroam resource.
const VEHICLE_MODELS = [
    "berkley_810", "bolt_ace", "bolt_ace_pickup", "bolt_cooler", "bolt_delivery", "bolt_delivery_amb", "bolt_hearse", "bolt_mail",
    "bolt_model_b", "bolt_pickup", "bolt_truck", "bolt_v8", "brubaker_forte", "bulworth_packhard", "bulworth_sentry",
    "carrozella_c_series", "celeste_mark_5", "culver_airmaster", "eckhart_crusader", "eckhart_elite", "eckhart_fletcher",
    "falconer_classic", "houston_coupe", "lassiter_v16", "lassiter_v16_appolyon", "lassiter_v16_roadster", "parry_bus",
    "samson_drifter", "samson_tanker", "shubert_e_six", "shubert_e_six_p", "shubert_e_six_taxi", "shubert_frigate",
    "shubert_six", "shubert_six_det", "shubert_six_p", "shubert_six_taxi", "smith_moray", "smith_thrower", "smith_v12",
    "smith_v12_chicago", "trautenberg_sport", "crazy_horse", "disorder", "flame_spear", "manta_prototype", "mutagen", "hank_a",
];

module.exports = { PLACES, VERIFIED, STAND_INS, UNVERIFIED, PARKING, WEAPONS, WEATHER, VEHICLE_MODELS };
