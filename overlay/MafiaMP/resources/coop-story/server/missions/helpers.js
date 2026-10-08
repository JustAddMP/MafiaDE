/**
 * Shared helpers for chapter files: enemy placement around a place, guards,
 * allies and vehicle definitions. All positions stay on the ground plane of
 * the place they are built from, so stand-in locations keep working.
 */

const { ring, line } = require("../lib/npcs.js");
const V = require("../lib/vec.js");
const { WEAPONS } = require("./places.js");

const PISTOL = [WEAPONS.pistol, 90];
const TOMMY = [WEAPONS.tommy, 200];

/** n armed enemies in a ring of `radius` around `place.pos`. */
function foes(place, n, radius = 12, weapon = PISTOL, extra = {}) {
    return ring(place.pos, n, radius, { weapon, ...extra });
}

/** n enemies without guns (fists / melee). */
function thugs(place, n, radius = 8, extra = {}) {
    return ring(place.pos, n, radius, { ...extra });
}

/** n guards standing around a place; they only react when a player gets close. */
function guards(place, n, radius = 14, weapon = PISTOL) {
    return ring(place.pos, n, radius, { role: "guard", weapon });
}

/** One named ally that follows the crew and shoots back. */
function ally(name, place, weapon = PISTOL, offset = [2, 2]) {
    return { name, pos: V.add(place.pos, offset[0], offset[1]), weapon, role: "ally" };
}

/** A marked man with his bodyguards. */
function mark(name, place, weapon = PISTOL, health = 100) {
    return { name, pos: V.add(place.pos, 0, 0), weapon, health };
}

/** Enemies strung out between two places (an ambush line). */
function ambush(from, to, n, weapon = PISTOL) {
    return line(from.pos, to.pos, n, { weapon });
}

/** Wave list: [[at, place, n, radius?, weapon?], ...] */
function waves(spec) {
    return spec.map(([at, place, n, radius, weapon, title]) => ({ at, title, enemies: foes(place, n, radius || 14, weapon || PISTOL) }));
}

/** Vehicle definition at a parking spot. */
function car(model, spot, label) {
    return { model, label: label || model, ...spot };
}

module.exports = { PISTOL, TOMMY, foes, thugs, guards, ally, mark, ambush, waves, car };
