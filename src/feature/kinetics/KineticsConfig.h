#pragma once

// `config/kinetics.json`: Create's CKinetics values used by the kinetic network and the stress
// table of every unit (Create's CStress defaults, SU per RPM). Keys are block paths (no namespace).

#include <map>
#include <string>

namespace lv::feature::kinetics {

struct KineticsConfig {
    int version = 1;

    // ---- CKinetics ----
    /// Disable the stress mechanic (networks never overstress).
    bool  disableStress = false;
    /// Components break when a ratio would turn them faster than this (RPM).
    float maxRotationSpeed = 256;
    /// Ticks between the consistency checks of a kinetic block.
    int   kineticValidationFrequency = 60;
    /// Speed level thresholds (RPM): slow < mediumSpeed <= medium < fastSpeed <= fast.
    float mediumSpeed = 30;
    float fastSpeed   = 100;
    /// Stress impact thresholds for tooltips (SU per RPM): low < medium <= medium < high <= high.
    float mediumStressImpact = 4;
    float highStressImpact   = 8;
    /// Capacity thresholds for tooltips (SU per RPM).
    float mediumCapacity = 256;
    float highCapacity   = 1024;

    // ---- CStress ----
    /// Stress impact (SU per RPM) per block path.
    std::map<std::string, double> impact = {
        {"mechanical_press",          8},
        {"crushing_wheel",            8},
        {"mechanical_mixer",          4},
        {"millstone",                 4},
        {"mechanical_drill",          4},
        {"mechanical_saw",            4},
        {"deployer",                  4},
        {"mechanical_bearing",        4},
        {"clockwork_bearing",         4},
        {"mechanical_piston",         4},
        {"sticky_mechanical_piston",  4},
        {"rope_pulley",               4},
        {"elevator_pulley",           4},
        {"hose_pulley",               4},
        {"gantry_shaft",              4},
        {"turntable",                 4},
        {"mechanical_pump",           4},
        {"copper_backtank",           4},
        {"netherite_backtank",        4},
        {"encased_fan",               2},
        {"mechanical_arm",            2},
        {"mechanical_crafter",        2},
        {"weighted_ejector",          2},
        {"chain_conveyor",            1},
        {"cuckoo_clock",              1},
        {"mysterious_cuckoo_clock",   1},
        {"shaft",                     0},
        {"cogwheel",                  0},
        {"large_cogwheel",            0},
        {"gearbox",                   0},
        {"clutch",                    0},
        {"gearshift",                 0},
        {"encased_chain_drive",       0},
        {"adjustable_chain_gearshift", 0},
        {"sequenced_gearshift",       0},
        {"rotation_speed_controller", 0},
        {"speedometer",               0},
        {"stressometer",              0},
        {"belt",                      0},
        {"depot",                     0},
    };
    /// Stress capacity (SU per RPM at the generated speed) per block path.
    std::map<std::string, double> capacity = {
        {"creative_motor",            16384},
        {"steam_engine",              1024},
        {"windmill_bearing",          512},
        {"large_water_wheel",         128},
        {"water_wheel",               32},
        {"hand_crank",                8},
        {"copper_valve_handle",       8},
        {"white_valve_handle",        8},
        {"orange_valve_handle",       8},
        {"magenta_valve_handle",      8},
        {"light_blue_valve_handle",   8},
        {"yellow_valve_handle",       8},
        {"lime_valve_handle",         8},
        {"pink_valve_handle",         8},
        {"gray_valve_handle",         8},
        {"light_gray_valve_handle",   8},
        {"cyan_valve_handle",         8},
        {"purple_valve_handle",       8},
        {"blue_valve_handle",         8},
        {"brown_valve_handle",        8},
        {"green_valve_handle",        8},
        {"red_valve_handle",          8},
        {"black_valve_handle",        8},
    };

    // ---- Levitate ----
    /// Send kinetic speeds to players with the client mod (animated rotating parts).
    bool syncToClients = true;
};

} // namespace lv::feature::kinetics
