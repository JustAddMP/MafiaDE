/**
 * Campaign order: the 20 chapters of Mafia: Definitive Edition, rebuilt as
 * co-op missions. Each file exports one mission object (see lib/runner.js
 * for the schema). Add a file, require it here, done.
 */

const CAMPAIGN = [
    require("./01_offer.js"),
    require("./02_running_man.js"),
    require("./03_molotov_party.js"),
    require("./04_ordinary_routine.js"),
    require("./05_fair_play.js"),
    require("./06_sarah.js"),
    require("./07_better_get_used_to_it.js"),
    require("./08_saint_and_sinner.js"),
    require("./09_trip_to_the_country.js"),
    require("./10_omerta.js"),
    require("./11_visiting_rich_people.js"),
    require("./12_great_deal.js"),
    require("./13_bon_appetit.js"),
    require("./14_happy_birthday.js"),
    require("./15_you_lucky_bastard.js"),
    require("./16_creme_de_la_creme.js"),
    require("./17_election_campaign.js"),
    require("./18_just_for_relaxation.js"),
    require("./19_moonlighting.js"),
    require("./20_death_of_art.js"),
];

module.exports = { CAMPAIGN };
