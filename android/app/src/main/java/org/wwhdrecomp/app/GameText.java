package org.wwhdrecomp.app;

/**
 * Short English names for the context buttons: the game's button actions (dActStts codes, as named
 * in the GameCube version's decompilation) and the items on X / Y / R (item ids).
 */
final class GameText {
    private GameText() {}

    static String action(int code) {
        switch (code) {
            case 0x01: return "Look";
            case 0x02: return "Speak";
            case 0x03: return "Charts";
            case 0x04: return "Lift";
            case 0x05: return "Climb";
            case 0x06: case 0x16: return "Let go";
            case 0x07: return "Return";
            case 0x08: return "Put away";
            case 0x09: return "Drop";
            case 0x0A: return "Check";
            case 0x0B: return "Open";
            case 0x0C: return "Roll";  // the decompilation's "Attack": A while running rolls
            case 0x0E: return "Throw";
            case 0x0F: return "Crouch";
            case 0x10: return "Sidle";
            case 0x11: return "Grab";
            case 0x12: return "Jump";
            case 0x13: return "Stop";
            case 0x17: return "Choose";
            case 0x19: return "Next";
            case 0x1B: return "Pick up";
            case 0x1C: return "Get in";
            case 0x1D: return "Get out";
            case 0x20: return "Photo";
            case 0x21: return "Info";
            case 0x22: return "Swap";
            case 0x23: return "Fly";
            case 0x24: return "Call";
            case 0x25: return "Bid";
            case 0x26: return "Read";
            case 0x27: return "Cancel";
            case 0x2C: return "Cruise";
            case 0x2F: return "Swing";
            case 0x30: return "Chart";
            case 0x36: return "Defend";
            default: return null;
        }
    }

    static String item(int id) {
        switch (id) {
            case 0x20: return "Telescope";
            case 0x21: return "Tingle";
            case 0x22: return "Baton";
            case 0x23: case 0x26: return "Picto";
            case 0x24: return "Spoils";
            case 0x25: return "Hook";
            case 0x27: return "Bow";
            case 0x29: return "Boots";
            case 0x2A: return "Armor";
            case 0x2C: return "Bait";
            case 0x2D: return "Boomer.";
            case 0x2F: return "Hookshot";
            case 0x30: return "Mail";
            case 0x31: return "Bombs";
            case 0x33: return "Hammer";
            case 0x34: return "Leaf";
            case 0x35: case 0x36: return "Bow";
            case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: case 0x59:
                return "Bottle";
            case 0x77: case 0x78: return "Sail";
            default: return null;
        }
    }
}
