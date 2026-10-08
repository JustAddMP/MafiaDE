/**
 * Small vector helpers. Mafia DE is Z-up, so "ground distance" ignores z.
 * Positions coming from the scripting layer are Core.Vector3 instances; plain
 * {x,y,z} objects are accepted everywhere too.
 */

// Builtins live at the global root on current Framework builds; older ones nest them under Core.
const V3 = globalThis.Vector3 || (globalThis.Core && globalThis.Core.Vector3);
const Q4 = globalThis.Quaternion || (globalThis.Core && globalThis.Core.Quaternion);

function v3(x, y, z) {
    return new V3(x, y, z);
}

function quat(w, x, y, z) {
    return new Q4(w, x, y, z);
}

function dist3(a, b) {
    const dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return Math.sqrt(dx * dx + dy * dy + dz * dz);
}

function dist2(a, b) {
    const dx = a.x - b.x, dy = a.y - b.y;
    return Math.sqrt(dx * dx + dy * dy);
}

/** Within `radius` metres on the ground plane and within `zTol` metres vertically. */
function within(a, b, radius, zTol = 6.0) {
    return dist2(a, b) <= radius && Math.abs(a.z - b.z) <= zTol;
}

/** Yaw (degrees, around Z) as a Vector3 euler rotation the entity setter accepts. */
function yaw(deg) {
    return v3(0, 0, deg);
}

/** Offset a point by dx/dy/dz. */
function add(p, dx, dy, dz = 0) {
    return v3(p.x + dx, p.y + dy, p.z + dz);
}

function fmt(p) {
    return `${p.x.toFixed(2)}, ${p.y.toFixed(2)}, ${p.z.toFixed(2)}`;
}

module.exports = { v3, quat, dist3, dist2, within, yaw, add, fmt };
