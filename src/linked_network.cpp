#include "linked_network.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <iostream>
#include <omp.h>
#include <unistd.h>

// Default constructor
LinkedNetwork::LinkedNetwork() = default;

/**
 * @brief Construct a hexagonal linked network from scratch using netmc.inpt
 * parameters
 * @param numRings the number of nodes in lattice A
 * @param logger the logger object
 */
LinkedNetwork::LinkedNetwork(const int &numRings, LoggerPtr logger)
    : networkB(numRings), minBCnxs(6), maxBCnxs(6), minACnxs(3), maxACnxs(3), centreCoords(2), logger(logger) {

    networkA = networkB.constructDual(maxACnxs);
    networkA.maxNetCnxs = maxACnxs;
    currentCoords.resize(2 * networkA.nodes.size());
    networkA.getCoords(currentCoords);
    rescale(sqrt(3));
    dimensions = networkA.dimensions;
    centreCoords[0] = dimensions[0] / 2;
    centreCoords[1] = dimensions[1] / 2;
}

/**
 * @brief Construct by loading networks from files
 * @param inputData the input data object
 * @param logger the logger object
 */
LinkedNetwork::LinkedNetwork(const InputData &inputData, LoggerPtr loggerArg) : centreCoords(2), logger(loggerArg) {
    std::string prefix = inputData.inputFolder + '/' + inputData.inputFilePrefix;

    networkA = Network(prefix + "_A", inputData.maxRingSize, inputData.maxRingSize, logger);
    minACnxs = inputData.minCoordination;
    int loadedMinACnxs = networkA.getMinCnxs();
    maxACnxs = inputData.maxCoordination;
    int loadedMaxACnxs = networkA.getMaxCnxs();

    networkB = Network(prefix + "_B", inputData.maxRingSize, inputData.maxRingSize, logger);
    minBCnxs = inputData.minRingSize;
    int loadedMinBCnxs = networkB.getMinCnxs();
    maxBCnxs = inputData.maxRingSize;
    int loadedMaxBCnxs = networkB.getMaxCnxs();

    dimensions = networkA.dimensions;
    centreCoords[0] = dimensions[0] / 2;
    centreCoords[1] = dimensions[1] / 2;
    if (loadedMinACnxs < minACnxs) {
        logger->warn("Loaded network has a min coordination of {}, which is lower than {}", loadedMinACnxs, minACnxs);
    }
    if (loadedMaxACnxs > maxACnxs) {
        logger->warn("Loaded network has a max coordination of {}, which is higher than {}", loadedMaxACnxs, maxACnxs);
    }
    if (loadedMinBCnxs < minBCnxs) {
        logger->warn("Loaded network has a min ring size of {}, which is lower than {}", loadedMinBCnxs, minBCnxs);
    }
    if (loadedMaxBCnxs > maxBCnxs) {
        logger->warn("Loaded network has a max ring size of {}, which is higher than {}", loadedMaxBCnxs, maxBCnxs);
    }

    if (inputData.structureType == StructureType::GRAPHENE)
        lammpsNetwork = LammpsObject("C", inputData.inputFolder, logger);
    else if (inputData.structureType == StructureType::SILICENE)
        lammpsNetwork = LammpsObject("Si", inputData.inputFolder, logger);
    else if (inputData.structureType == StructureType::TRIANGLE_RAFT)
        lammpsNetwork = LammpsObject("Si2O3", inputData.inputFolder, logger);
    else if (inputData.structureType == StructureType::BILAYER)
        lammpsNetwork = LammpsObject("SiO2", inputData.inputFolder, logger);
    else if (inputData.structureType == StructureType::BORON_NITRIDE)
        lammpsNetwork = LammpsObject("BN", inputData.inputFolder, logger);

    if (inputData.isFixRingsEnabled) {
        findFixedRings(inputData.inputFolder + "/fixed_rings.dat");
        findFixedNodes();
    } else {
        logger->info("Fixed rings disabled, setting number of fixed rings to 0.");
    }
    writeMovie = inputData.writeMovie;
    if (writeMovie) {
        lammpsNetwork.startMovie();
        lammpsNetwork.writeMovie();
    }
    weightedDecay = inputData.weightedDecay;
    maximumBondLength = inputData.maximumBondLength;
    maximumAngle = inputData.maximumAngle * M_PI / 180; // Convert to radians

    lammpsNetwork.minimiseNetwork();
    std::vector<double> coords = lammpsNetwork.getCoords(2);
    energy = lammpsNetwork.getPotentialEnergy();
    pushCoords(coords);
    weights.resize(networkA.nodes.size());
    updateWeights();

    currentCoords.resize(2 * networkA.nodes.size());
    networkA.getCoords(currentCoords);

    mc = Metropolis(inputData.randomSeed, pow(10, inputData.startTemperature), energy);
    mtGen.seed(inputData.randomSeed);

    isOpenMPIEnabled = inputData.isOpenMPIEnabled;
    selectionType = inputData.randomOrWeighted;
}

/**
 * @brief Populate fixedNodes with all base nodes that are a member of all fixed rings
 */
void LinkedNetwork::findFixedNodes() {
    std::for_each(fixedRings.begin(), fixedRings.end(), [this](int fixedRing) {
        std::for_each(networkB.nodes[fixedRing].dualCnxs.begin(), networkB.nodes[fixedRing].dualCnxs.end(), [this](int fixedNode) {
            fixedNodes.insert(fixedNode);
        });
    });
}

/**
 * @brief read the fixed_rings.dat file and populate fixedRings with integers from each line
 * @param isFixedRingsEnabled boolean to enable or disable fixed rings
 * @param filename the name of the input file
 * @param logger the logger object
 */
void LinkedNetwork::findFixedRings(const std::string &filename) {
    std::ifstream fixedRingsFile(filename, std::ios::in);
    if (!fixedRingsFile.is_open()) {
        logger->warn("Failed to open file: {}.dat, setting number of fixed rings to 0 and they will be ignored", filename);
        return;
    }
    std::string line;
    while (getline(fixedRingsFile, line)) {
        int num;
        std::istringstream(line) >> num;
        fixedRings.insert(num);
    }
    logger->info("Number of fixed rings: {}", fixedRings.size());
    logger->info("Fixed rings: {}", setToString(fixedRings));
}

/**
 * @brief Perform a monte carlo switch move, evaluate energy, and accept or reject
 */
void LinkedNetwork::monteCarloSwitchMoveLAMMPS() {
    logger->debug("Finding move...");
    bool foundValidMove = false;
    int baseNode1;
    int baseNode2;
    int ringNode1;
    int ringNode2;

    std::vector<int> bondBreaks;
    std::vector<int> bondMakes;
    std::vector<int> angleBreaks;
    std::vector<int> angleMakes;
    std::vector<int> ringBondBreakMake;
    std::vector<int> involvedNodes;

    for (int i = 0; i < networkA.nodes.size() * networkA.nodes.size(); ++i) {
        std::tuple<int, int, int, int> result;
        result = pickRandomConnection();

        std::tie(baseNode1, baseNode2, ringNode1, ringNode2) = result;
        logger->debug("Picked base nodes: {} {} and ring nodes: {} {}", baseNode1, baseNode2, ringNode1, ringNode2);
        foundValidMove = genSwitchOperations(baseNode1, baseNode2, ringNode1, ringNode2,
                                             bondBreaks, bondMakes,
                                             angleBreaks, angleMakes,
                                             ringBondBreakMake, involvedNodes);
        if (foundValidMove)
            break;
    }

    if (!foundValidMove) {
        logger->error("Cannot find any valid switch moves");
        throw std::runtime_error("Cannot find any valid switch moves");
    }
    numSwitches++;
    logger->info("Switch number: {}", numSwitches);

    // Save current state
    double initialEnergy = energy;

    std::vector<int> saveNodeDistA = networkA.nodeDistribution;
    std::vector<int> saveNodeDistB = networkB.nodeDistribution;

    std::vector<std::vector<int>> saveEdgeDistA = networkA.edgeDistribution;
    std::vector<std::vector<int>> saveEdgeDistB = networkB.edgeDistribution;
    std::vector<Node> initialInvolvedNodesA;
    for (const auto &id : involvedNodes) {
        initialInvolvedNodesA.push_back(networkA.nodes[id]);
    }
    std::vector<Node> initialInvolvedNodesB;
    for (const auto &id : ringBondBreakMake) {
        initialInvolvedNodesB.push_back(networkB.nodes[id]);
    }

    // Switch and geometry optimise
    logger->debug("Switching NetMC Network...");
    switchNetMCGraphene(bondBreaks, ringBondBreakMake);
    std::vector<double> rotatedCoord1;
    std::vector<double> rotatedCoord2;
    std::vector<int> orderedRingNodes = {ringBondBreakMake[1], ringBondBreakMake[3], ringBondBreakMake[0], ringBondBreakMake[2]};
    std::tie(rotatedCoord1, rotatedCoord2) = rotateBond(baseNode1, baseNode2, getRingsDirection(orderedRingNodes));

    logger->debug("Switching LAMMPS Network...");

    lammpsNetwork.switchGraphene(bondBreaks, bondMakes, angleBreaks, angleMakes, rotatedCoord1, rotatedCoord2, logger);

    double finalEnergy;
    // Geometry optimisation of local region
    logger->debug("Minimising network...");
    lammpsNetwork.minimiseNetwork();
    std::vector<double> lammpsCoords = lammpsNetwork.getCoords(2);

    logger->info("Accepting or rejecting...");
    bool isAccepted = false;
    if (checkAnglesWithinRange(std::vector<int>{bondBreaks[0], bondBreaks[2]}, lammpsCoords)) {
        if (checkBondLengths(involvedNodes, lammpsCoords)) {
            finalEnergy = lammpsNetwork.getPotentialEnergy();
            if (mc.acceptanceCriterion(finalEnergy, initialEnergy, 1.0)) {
                logger->info("Accepted Move: Ei = {:.3f} Eh, Ef = {:.3f} Eh", initialEnergy, finalEnergy);
                isAccepted = true;
                numAcceptedSwitches++;
                logger->info("Syncing LAMMPS coordinates to NetMC coordinates...");
                currentCoords = lammpsCoords;
                pushCoords(lammpsCoords);
                updateWeights();
                energy = finalEnergy;
                if (writeMovie)
                    lammpsNetwork.writeMovie();
            } else {
                logger->warn("Rejected move: failed Metropolis criterion: Ei = {:.3f} Eh, Ef = {:.3f} Eh", initialEnergy, finalEnergy);
                failedEnergyChecks++;
            }
        } else {
            logger->warn("Rejected move: bond lengths are not within range");
            failedBondLengthChecks++;
        }
    } else {
        logger->warn("Rejected move: angles are not within range");
        failedAngleChecks++;
    }

    if (!isAccepted) {
        logger->debug("Reverting NetMC Network...");
        revertNetMCGraphene(initialInvolvedNodesA, initialInvolvedNodesB);

        logger->debug("Reverting LAMMPS Network...");
        lammpsNetwork.revertGraphene(bondBreaks, bondMakes, angleBreaks, angleMakes, logger);
        lammpsNetwork.setCoords(currentCoords, 2);

        networkA.nodeDistribution = saveNodeDistA;
        networkA.edgeDistribution = saveEdgeDistA;
        networkB.nodeDistribution = saveNodeDistB;
        networkB.edgeDistribution = saveEdgeDistB;
    }
}

void LinkedNetwork::showCoords(const std::vector<double> &coords) const {
    for (int i = 0; i < coords.size(); i += 2) {
        logger->debug("{}) {} {}", i, coords[i], coords[i + 1]);
    }
}

/**
 * @brief Scale the entire linked network by a given factor
 * @param scaleFactor the factor to scale the network by
 */
void LinkedNetwork::rescale(double scaleFactor) {
    vectorMultiply(dimensions, scaleFactor);
    networkA.rescale(scaleFactor);
    networkB.rescale(scaleFactor);
}

void LinkedNetwork::updateWeights() {
    if (selectionType == SelectionType::EXPONENTIAL_DECAY) {
        double boxLength = dimensions[0];
        for (int i = 0; i < networkA.nodes.size(); ++i) {
            double distance = networkA.nodes[i].distanceFrom(centreCoords) / boxLength;
            weights[i] = std::exp(-distance * weightedDecay);
        }

        // Normalize weights
        double total = std::accumulate(weights.begin(), weights.end(), 0.0);
        for (double &weight : weights) {
            weight /= total;
        }
    } else { // SelectionType::RANDOM
        std::fill(weights.begin(), weights.end(), 1.0);
    }
}

/**
 * @brief Chooses a random bond in the network and returns the IDs in the bond, the two rings either side and the connection type
 * @param generator Reference to a Mersenne Twister pseudo-random generator of 32-bit numbers with a state size of 19937 bits.
 * @param selectionType The type of selection to be used. Exponential decay or random.
 * @param logger The logger object.
 * @return A tuple containing the IDs of the two nodes in the bond, the two rings either side and the connection type
 * @throw std::runtime_error if the nodes in the random connection have coordinations other than 3 or 4.
 */
std::tuple<int, int, int, int> LinkedNetwork::pickRandomConnection() {

    // Create a discrete distribution based on the weights
    std::discrete_distribution<> distribution(weights.begin(), weights.end());
    int randNode;
    int randNodeConnection;
    int sharedRingNode1;
    int sharedRingNode2;
    bool pickingAcceptableRing = true;
    std::uniform_int_distribution<int> randomCnx;
    std::uniform_int_distribution randomDirection(0, 1);

    while (pickingAcceptableRing) {
        randNode = distribution(mtGen);
        int randNodeCoordination = networkA.nodes[randNode].netCnxs.size();
        randomCnx.param(std::uniform_int_distribution<int>::param_type(0, randNodeCoordination - 1));
        randNodeConnection = networkA.nodes[randNode].netCnxs[randomCnx(mtGen)];

        // Two connected nodes should always share two ring nodes.
        std::unordered_set<int> commonRings = intersectVectors(networkA.nodes[randNode].dualCnxs, networkA.nodes[randNodeConnection].dualCnxs);
        if (commonRings.size() == 2) {
            // Randomly assign ringNode1 and ringNode2 to those two common ring nodes
            auto it = commonRings.begin();
            std::advance(it, randomDirection(mtGen));
            sharedRingNode1 = *it;
            commonRings.erase(it);
            sharedRingNode2 = *commonRings.begin();
        } else {
            logger->warn("Selected random connection does not share two ring nodes: {} {}", randNode, randNodeConnection);
            continue;
        }
        // Check that the base nodes are not a member of a fixed ring.
        if (fixedNodes.count(randNode) == 0 || fixedNodes.count(randNodeConnection) == 0) {
            // If so, break and return the connection type
            pickingAcceptableRing = false;
        }
    }
    return std::make_tuple(randNode, randNodeConnection, sharedRingNode1, sharedRingNode2);
}

/**
 * @brief Generate the bond and angle operations for a switch move
 * @param baseNode1 the id of the first node in lattice A
 * @param baseNode2 the id of the second node in lattice A
 * @param ringNode1 the id of the first node in lattice B
 * @param ringNode2 the id of the second node in lattice B
 * @param bondBreaks the bonds to break
 * @param bondMakes the bonds to make
 * @param angleBreaks the angles to break
 * @param angleMakes the angles to make
 * @param ringBondBreakMake the ring bond to break and make, first two indexes are bond to break, second two are bond to make
 * @return true if the switch move is possible, false otherwise
 */
bool LinkedNetwork::genSwitchOperations(int baseNode1, int baseNode2, int ringNode1, int ringNode2,
                                        std::vector<int> &bondBreaks, std::vector<int> &bondMakes,
                                        std::vector<int> &angleBreaks, std::vector<int> &angleMakes,
                                        std::vector<int> &ringBondBreakMake, std::vector<int> &convexCheckIDs) {
    // lots of error checking to remove any potential pathological cases
    if (baseNode1 == baseNode2 || ringNode1 == ringNode2) {
        logger->warn("Switch move not possible as baseNode1 = baseNode2 or ringNode1 = ringNode2: {} {} {} {}",
                     baseNode1, baseNode2, ringNode1, ringNode2);
        return false;
    }
    /*
     *                7-----8                               7-----8
     *               /       \                              |     |
     *              /         \                      11-----3  2  4-----12
     *      11-----3     2     4-----12                      \   /
     *              \         /                               \ /
     *               \       /                                 1
     *          3     1-----2     4         --->         3     |      4
     *               /       \                                 2
     *              /         \                               /  \
     *      13-----5     1     6-----14                      /    \
     *              \         /                      13-----5  1   6-----14
     *               \       /        (6 membered case)     |      |
     *                9-----10                              9------10
     *
     *      Bonds to break       Bonds to Make
     *      1-5, 2-4             1-4, 2-5
     *
     *      Angles to break      Angles to Make
     *      1-5-9, 1-5-13        1-4-8, 1-4-12
     *      2-4-8, 2-4-12        2-5-9, 2-5-13
     *      4-2-1, 4-2-6         4-1-2, 4-1-3
     *      5-1-2, 5-1-3         1-2-5, 6-2-5
     */
    int baseNode5 = findCommonConnection(baseNode1, ringNode1, baseNode2);
    int baseNode6 = findCommonConnection(baseNode2, ringNode1, baseNode1);
    int baseNode3 = findCommonConnection(baseNode1, ringNode2, baseNode2);
    int baseNode4 = findCommonConnection(baseNode2, ringNode2, baseNode1);

    int ringNode3 = findCommonRing(baseNode1, baseNode5, ringNode1);
    int ringNode4 = findCommonRing(baseNode2, baseNode6, ringNode1);

    int baseNode11 = findCommonConnection(baseNode3, ringNode3, baseNode1);
    int baseNode7 = findCommonConnection(baseNode3, ringNode2, baseNode1);
    int baseNode8 = findCommonConnection(baseNode4, ringNode2, baseNode2);
    int baseNode12 = findCommonConnection(baseNode4, ringNode4, baseNode2);
    int baseNode14 = findCommonConnection(baseNode6, ringNode4, baseNode2);
    int baseNode10 = findCommonConnection(baseNode6, ringNode1, baseNode2);
    int baseNode9 = findCommonConnection(baseNode5, ringNode1, baseNode1);
    int baseNode13 = findCommonConnection(baseNode5, ringNode3, baseNode1);

    // Additional error checking
    if (baseNode5 == baseNode6 || baseNode3 == baseNode4) {
        logger->debug("No valid move: Selected nodes describe an edge of two edge sharing triangles");
        return false;
    }
    // Prevent rings having only two or fewer neighbours
    if (getUniqueValues(networkB.nodes[ringNode1].netCnxs).size() <= 3 || getUniqueValues(networkB.nodes[ringNode2].netCnxs).size() <= 3) {
        logger->debug("No valid move: Switch would result in a ring size less than 3");
        return false;
    }

    // check move will not violate dual connectivity limits
    logger->debug("min ring size: {} max ring size: {}", minBCnxs, maxBCnxs);
    logger->debug("Ring 1: {} Ring 2: {}", networkB.nodes[ringNode1].netCnxs.size(), networkB.nodes[ringNode2].netCnxs.size());
    logger->debug("Ring 3: {} Ring 4: {}", networkB.nodes[ringNode3].netCnxs.size(), networkB.nodes[ringNode4].netCnxs.size());
    if (networkB.nodes[ringNode1].netCnxs.size() == minBCnxs ||
        networkB.nodes[ringNode2].netCnxs.size() == minBCnxs ||
        networkB.nodes[ringNode3].netCnxs.size() == maxBCnxs ||
        networkB.nodes[ringNode4].netCnxs.size() == maxBCnxs) {
        logger->debug("No valid move: Switch would violate dual connectivity limits");
        return false;
    }
    bondBreaks = {baseNode1, baseNode5, baseNode2, baseNode4};

    bondMakes = {baseNode1, baseNode4, baseNode2, baseNode5};
    //                          Break                   Make
    ringBondBreakMake = {ringNode1, ringNode2, ringNode3, ringNode4};

    convexCheckIDs = {baseNode1, baseNode2, baseNode3, baseNode4, baseNode5,
                      baseNode6, baseNode7, baseNode8, baseNode9, baseNode10,
                      baseNode11, baseNode12, baseNode13, baseNode14};

    if (baseNode7 == baseNode4) {
        // 4 membered ringNode2
        angleBreaks = {baseNode3, baseNode4, baseNode2,
                       baseNode12, baseNode4, baseNode2,
                       baseNode1, baseNode2, baseNode4,
                       baseNode6, baseNode2, baseNode4};
        angleMakes = {baseNode3, baseNode4, baseNode1,
                      baseNode12, baseNode4, baseNode1,
                      baseNode3, baseNode1, baseNode4,
                      baseNode2, baseNode1, baseNode4};
        logger->debug(" {:03}------{:03}------{:03}------{:03}             {:03}-----{:03}-----{:03}-----{:03} ", baseNode11, baseNode3, baseNode4, baseNode12, baseNode11, baseNode3, baseNode4, baseNode12);
        logger->debug("            |       |                                \\ {:03}  / ", ringNode2);
        logger->debug("            |  {:03}  |                             \\    /", ringNode2);
        logger->debug("            |       |                                  {:03}", baseNode1);

    } else if (baseNode7 == baseNode8) {
        // 5 membered ringNode2
        angleBreaks = {baseNode7, baseNode4, baseNode2,
                       baseNode12, baseNode4, baseNode2,
                       baseNode1, baseNode2, baseNode4,
                       baseNode6, baseNode2, baseNode4};
        angleMakes = {baseNode7, baseNode4, baseNode1,
                      baseNode12, baseNode4, baseNode1,
                      baseNode2, baseNode1, baseNode4,
                      baseNode3, baseNode1, baseNode4};
        logger->debug("");
        logger->debug("               {:03}                                      {:03}", baseNode7, baseNode7);
        logger->debug("            /      \\                                   /   \\");
        logger->debug("           /        \\                                 /     \\");
        logger->debug(" {:03}-----{:03}   {:03}  {:03}-----{:03}             {:03}-----{:03} {:03} {:03}-----{:03}", baseNode11, baseNode3, ringNode2, baseNode4, baseNode12, baseNode11, baseNode3, ringNode2, baseNode4, baseNode12);
        logger->debug("          \\          /                                \\     /");
        logger->debug("           \\        /                                   {:03}", baseNode1);

    } else {
        // 6+ membered ringNode2
        angleBreaks = {baseNode2, baseNode4, baseNode8,
                       baseNode2, baseNode4, baseNode12,
                       baseNode4, baseNode2, baseNode1,
                       baseNode4, baseNode2, baseNode6};
        angleMakes = {baseNode1, baseNode4, baseNode8,
                      baseNode1, baseNode4, baseNode12,
                      baseNode4, baseNode1, baseNode2,
                      baseNode4, baseNode1, baseNode3};
        logger->debug("");
        logger->debug("           {:03}~~~~~{:03}                              {:03}~~~~~{:03}", baseNode7, baseNode8, baseNode7, baseNode8);
        logger->debug("           /        \\                                |       |");
        logger->debug("          /          \\                      {:03}-----{:03} {:03} {:03}-----{:03}", baseNode11, baseNode3, ringNode2, baseNode4, baseNode12);
        logger->debug(" {:03}-----{:03}   {:03}   {:03}-----{:03}                      \\     /", baseNode11, baseNode3, ringNode2, baseNode4, baseNode12);
        logger->debug("          \\          /                                 \\   /");
        logger->debug("           \\        /                                   {:03}", baseNode1);
    }
    logger->debug("    {:03}    {:03}-----{:03}   {:03}          --->     {:03}       |      {:03}", ringNode3, baseNode1, baseNode2, ringNode4, ringNode3, ringNode4);
    if (baseNode5 == baseNode10) {
        // 4 membered ringNode1
        angleBreaks.insert(angleBreaks.end(), {baseNode13, baseNode5, baseNode1,
                                               baseNode6, baseNode5, baseNode1,
                                               baseNode3, baseNode1, baseNode5,
                                               baseNode2, baseNode1, baseNode5});
        angleMakes.insert(angleMakes.end(), {baseNode13, baseNode5, baseNode2,
                                             baseNode6, baseNode5, baseNode2,
                                             baseNode1, baseNode2, baseNode5,
                                             baseNode6, baseNode2, baseNode5});
        logger->debug("            |       |                                   {:03}", baseNode2);
        logger->debug("            |  {:03}  |                                /   \\, ringNode1");
        logger->debug("            |       |                                 / {:03} \\ ", ringNode1);
        logger->debug(" {:03}-------{:03}-----{:03}-------{:03}            {:03}-----{:03}-----{:03}-----{:03} ", baseNode13, baseNode5, baseNode6, baseNode14, baseNode13, baseNode5, baseNode6, baseNode14);
        logger->debug("");
    } else if (baseNode9 == baseNode10) {
        // 5 membered ringNode1
        angleBreaks.insert(angleBreaks.end(), {baseNode13, baseNode5, baseNode1,
                                               baseNode9, baseNode5, baseNode1,
                                               baseNode3, baseNode1, baseNode5,
                                               baseNode2, baseNode1, baseNode5});
        angleMakes.insert(angleMakes.end(), {baseNode13, baseNode5, baseNode2,
                                             baseNode9, baseNode5, baseNode2,
                                             baseNode1, baseNode2, baseNode5,
                                             baseNode6, baseNode2, baseNode5});

        logger->debug("           /        \\                                   {:03}", baseNode2);
        logger->debug("          /          \\                                /     \\");
        logger->debug(" {:03}-----{:03}   {:03}  {:03}-----{:03}             {:03}-----{:03} {:03} {:03}-----{:03}", baseNode13, baseNode5, ringNode1, baseNode6, baseNode14, baseNode13, baseNode5, ringNode1, baseNode6, baseNode14);
        logger->debug("           \\        /                                 \\     /");
        logger->debug("            \\      /                                   \\   /");
        logger->debug("              {:03}                                       {:03}", baseNode9, baseNode9);
        logger->debug("");
    } else {
        // 6+ membered ringNode1
        angleBreaks.insert(angleBreaks.end(), {baseNode1, baseNode5, baseNode9,
                                               baseNode1, baseNode5, baseNode13,
                                               baseNode5, baseNode1, baseNode2,
                                               baseNode5, baseNode1, baseNode3});
        angleMakes.insert(angleMakes.end(), {baseNode2, baseNode5, baseNode9,
                                             baseNode2, baseNode5, baseNode13,
                                             baseNode1, baseNode2, baseNode5,
                                             baseNode6, baseNode2, baseNode5});
        logger->debug("           /        \\                                   {:03}", baseNode2);
        logger->debug("          /          \\                                 /   \\");
        logger->debug(" {:03}-----{:03}   {:03}   {:03}-----{:03}                      /     \\", baseNode13, baseNode5, ringNode1, baseNode6, baseNode14);
        logger->debug("          \\          /                      {:03}-----{:03} {:03} {:03}-----{:03}", baseNode13, baseNode5, ringNode1, baseNode6, baseNode14);
        logger->debug("           \\        /                                |       |");
        logger->debug("           {:03}~~~~~{:03}                              {:03}~~~~~{:03}", baseNode9, baseNode10, baseNode9, baseNode10);
        logger->debug("");
    }

    return true;
}

const int MIN_COORDINATION_NUMBER = 2;
const int NUM_MIX_IDS_A = 6;
const int NUM_MIX_IDS_B = 7;

/**
 * @brief Find a common base node connection between a base node and ring node excluding a given node
 * @param baseNode ID of the base node
 * @param ringNode ID of the ring node
 * @param excludeNode ID of the node to be excluded
 * @return ID of the common connection
 * @throw std::runtime_error if the associated node cannot be found
 */
int LinkedNetwork::findCommonConnection(const int &baseNode, const int &ringNode, const int &excludeNode) const {
    // Find node that shares baseNode and ringNode but is not excludeNode
    std::unordered_set<int> commonConnections = intersectVectors(networkA.nodes[baseNode].netCnxs, networkB.nodes[ringNode].dualCnxs);
    commonConnections.erase(excludeNode);
    if (commonConnections.size() != 1) {
        throw std::runtime_error("Could not find common base node for base node " + std::to_string(baseNode) +
                                 " and ring node " + std::to_string(ringNode) +
                                 " excluding node " + std::to_string(excludeNode));
    }
    return *commonConnections.begin();
}
/**
 * @brief Find a common ring connection between two base nodes that exlcudes a given node
 * @param baseNode1 ID of the first base node
 * @param baseNode2 ID of the second base node
 * @param excludeNode ID of the ring node to be excluded
 * @return ID of the common ring connection
 * @throw std::runtime_error if the associated node cannot be found
 */
int LinkedNetwork::findCommonRing(const int &baseNode1, const int &baseNode2, const int &excludeNode) const {
    // Find node that shares baseNode1 and baseNode2 but is not excludeNode
    std::unordered_set<int> commonRings = intersectVectors(networkA.nodes[baseNode1].dualCnxs, networkA.nodes[baseNode2].dualCnxs);
    commonRings.erase(excludeNode);
    if (commonRings.size() != 1) {
        throw std::runtime_error("Could not find common ring node for base node " + std::to_string(baseNode1) +
                                 " and base node " + std::to_string(baseNode2) +
                                 " excluding ring node " + std::to_string(excludeNode));
    }
    return *commonRings.begin();
}

/**
 * @brief Switch the NetMC network by breaking and making bonds
 * @param bondBreaks the bonds to break (vector of pairs)
 * @param bondMakes the bonds to make (vector of pairs)
 */
void LinkedNetwork::switchNetMCGraphene(const std::vector<int> &bondBreaks, const std::vector<int> &ringBondBreakMake) {
    if (bondBreaks.size() != 4 || ringBondBreakMake.size() != 4) {
        throw std::invalid_argument("Invalid input sizes for switchNetMCGraphene");
    }

    int atom1 = bondBreaks[0];
    int atom2 = bondBreaks[2];
    int atom4 = bondBreaks[3];
    int atom5 = bondBreaks[1];

    int ringNode1 = ringBondBreakMake[0];
    int ringNode2 = ringBondBreakMake[1];
    int ringNode3 = ringBondBreakMake[2];
    int ringNode4 = ringBondBreakMake[3];

    auto &nodeA1 = networkA.nodes[atom1];
    auto &nodeA2 = networkA.nodes[atom2];
    auto &nodeA4 = networkA.nodes[atom4];
    auto &nodeA5 = networkA.nodes[atom5];

    auto &nodeB1 = networkB.nodes[ringNode1];
    auto &nodeB2 = networkB.nodes[ringNode2];
    auto &nodeB3 = networkB.nodes[ringNode3];
    auto &nodeB4 = networkB.nodes[ringNode4];

    // A-A connectivities
    replaceValue(nodeA1.netCnxs, atom5, atom4);
    replaceValue(nodeA2.netCnxs, atom4, atom5);
    replaceValue(nodeA4.netCnxs, atom2, atom1);
    replaceValue(nodeA5.netCnxs, atom1, atom2);

    // A-B connectvities
    replaceValue(nodeA1.dualCnxs, ringNode1, ringNode4);
    replaceValue(nodeA2.dualCnxs, ringNode2, ringNode3);

    // B-B connectivities
    deleteByValues(nodeB1.netCnxs, ringNode2);
    deleteByValues(nodeB2.netCnxs, ringNode1);
    nodeB3.netCnxs.emplace_back(ringNode4);
    nodeB4.netCnxs.emplace_back(ringNode3);

    // B-A connectivities
    deleteByValues(nodeB1.dualCnxs, atom1);
    deleteByValues(nodeB2.dualCnxs, atom2);

    nodeB3.dualCnxs.emplace_back(atom2);
    nodeB4.dualCnxs.emplace_back(atom1);
}

void LinkedNetwork::revertNetMCGraphene(const std::vector<Node> &initialInvolvedNodesA, const std::vector<Node> &initialInvolvedNodesB) {
    // Revert changes to descriptors due to breaking connections
    for (const Node &node : initialInvolvedNodesA) {
        networkA.nodes[node.id] = node;
    }
    for (const Node &node : initialInvolvedNodesB) {
        networkB.nodes[node.id] = node;
    }
}

/**
 * @brief Sets the coordinates of nodes in the network. Ring network coordinates are recalculated.
 * @param coords Coordinates of the base nodes
 * @throw std::invalid_argument if the number of coordinates does not match the number of nodes in network A
 */
void LinkedNetwork::pushCoords(const std::vector<double> &coords) {
    if (coords.size() != 2 * networkA.nodes.size()) {
        throw std::invalid_argument("Number of coordinates does not match number of nodes in network A");
    }

    // Sync A coordinates
    for (int i = 0; i < networkA.nodes.size(); ++i) {
        networkA.nodes[i].crd[0] = coords[2 * i];
        networkA.nodes[i].crd[1] = coords[2 * i + 1];
    }
    // Centre all the rings relative to their dual connections
    networkB.centreRings(networkA);
}

// Check linked networks for consistency
bool LinkedNetwork::checkConsistency() {
    return (checkCnxConsistency() && checkDescriptorConsistency());
}

// Check linked networks have mutual network and dual connections
bool LinkedNetwork::checkCnxConsistency() {

    // check number of network connections is equal to number of dual connections
    bool netDualEquality = true;
    for (int i = 0; i < networkA.nodes.size(); ++i) {
        if (networkA.nodes[i].netCnxs.size() != networkA.nodes[i].dualCnxs.size())
            netDualEquality = false;
    }
    for (int i = 0; i < networkB.nodes.size(); ++i) {
        if (networkB.nodes[i].netCnxs.size() != networkB.nodes[i].dualCnxs.size())
            netDualEquality = false;
    }

    // check mutual network connections
    bool mutualNetCnx = true;
    int id0;
    int id1;
    for (int i = 0; i < networkA.nodes.size(); ++i) {
        id0 = i;
        for (int j = 0; j < networkA.nodes[i].netCnxs.size(); ++j) {
            id1 = networkA.nodes[i].netCnxs[j];
            mutualNetCnx = vectorContains(networkA.nodes[id1].netCnxs, id0);
        }
    }
    for (int i = 0; i < networkB.nodes.size(); ++i) {
        id0 = i;
        for (int j = 0; j < networkB.nodes[i].netCnxs.size(); ++j) {
            id1 = networkB.nodes[i].netCnxs[j];
            mutualNetCnx = vectorContains(networkB.nodes[id1].netCnxs, id0);
        }
    }

    // check mutual dual connections
    bool mutualDualCnx = true;
    for (int i = 0; i < networkA.nodes.size(); ++i) {
        id0 = i;
        for (int j = 0; j < networkA.nodes[i].dualCnxs.size(); ++j) {
            id1 = networkA.nodes[i].dualCnxs[j];
            mutualDualCnx = vectorContains(networkB.nodes[id1].dualCnxs, id0);
        }
    }
    for (int i = 0; i < networkB.nodes.size(); ++i) {
        id0 = i;
        for (int j = 0; j < networkB.nodes[i].dualCnxs.size(); ++j) {
            id1 = networkB.nodes[i].dualCnxs[j];
            mutualDualCnx = vectorContains(networkA.nodes[id1].dualCnxs, id0);
        }
    }

    // check network connections are neighbours by lying on same ring (some highly
    // strained cases could give a false positive)
    bool nbNetCnx = true;
    for (int i = 0; i < networkA.nodes.size(); ++i) {
        int nCnxs = networkA.nodes[i].netCnxs.size();
        for (int j = 0; j < nCnxs; ++j) {
            id0 = networkA.nodes[i].netCnxs[j];
            id1 = networkA.nodes[i].netCnxs[(j + 1) % nCnxs];
            std::unordered_set<int> common = intersectVectors(networkA.nodes[id0].dualCnxs,
                                                              networkA.nodes[id1].dualCnxs);
            if (common.empty())
                nbNetCnx = false;
        }
    }
    for (int i = 0; i < networkB.nodes.size(); ++i) {
        int nCnxs = networkB.nodes[i].netCnxs.size();
        for (int j = 0; j < nCnxs; ++j) {
            id0 = networkB.nodes[i].netCnxs[j];
            id1 = networkB.nodes[i].netCnxs[(j + 1) % nCnxs];
            std::unordered_set<int> common = intersectVectors(networkB.nodes[id0].dualCnxs,
                                                              networkB.nodes[id1].dualCnxs);
            if (common.empty())
                nbNetCnx = false;
        }
    }

    // check dual connections are neighbours by lying on same ring (some highly
    // strained cases could give a false positive)
    bool nbDualCnx = true;
    for (int i = 0; i < networkA.nodes.size(); ++i) {
        int nCnxs = networkA.nodes[i].dualCnxs.size();
        for (int j = 0; j < nCnxs; ++j) {
            id0 = networkA.nodes[i].dualCnxs[j];
            id1 = networkA.nodes[i].dualCnxs[(j + 1) % nCnxs];
            std::unordered_set<int> common = intersectVectors(networkB.nodes[id0].dualCnxs,
                                                              networkB.nodes[id1].dualCnxs);
            common.erase(i);
            if (common.empty())
                nbDualCnx = false;
        }
    }
    for (int i = 0; i < networkB.nodes.size(); ++i) {
        int nCnxs = networkB.nodes[i].dualCnxs.size();
        for (int j = 0; j < nCnxs; ++j) {
            id0 = networkB.nodes[i].dualCnxs[j];
            id1 = networkB.nodes[i].dualCnxs[(j + 1) % nCnxs];
            std::unordered_set<int> common = intersectVectors(networkA.nodes[id0].dualCnxs,
                                                              networkA.nodes[id1].dualCnxs);
            common.erase(i);
            if (common.empty())
                nbDualCnx = false;
        }
    }

    return netDualEquality && mutualNetCnx && mutualDualCnx && nbNetCnx && nbDualCnx;
}

// Check linked networks have accurate descriptors
bool LinkedNetwork::checkDescriptorConsistency() {

    std::vector<int> checkNodeA = networkA.nodeDistribution;
    std::vector<int> checkNodeB = networkB.nodeDistribution;
    std::vector<std::vector<int>> checkEdgeA = networkA.edgeDistribution;
    std::vector<std::vector<int>> checkEdgeB = networkB.edgeDistribution;

    for (auto &element : checkNodeA) {
        element = 0;
    }
    for (auto &element : checkNodeB) {
        element = 0;
    }

    for (auto &innerVec : checkEdgeA) {
        for (auto &element : innerVec) {
            element = 0;
        }
    }
    for (auto &innerVec : checkEdgeB) {
        for (auto &element : innerVec) {
            element = 0;
        }
    }

    // Check node distribution
    for (int i = 0; i < networkA.nodes.size(); ++i) {
        int n = networkA.nodes[i].netCnxs.size();
        ++checkNodeA[n];
    }
    for (int i = 0; i < networkB.nodes.size(); ++i) {
        int n = networkB.nodes[i].netCnxs.size();
        ++checkNodeB[n];
    }
    bool nodeA = checkNodeA == networkA.nodeDistribution;
    bool nodeB = checkNodeB == networkB.nodeDistribution;

    // Check edge distribution
    bool edgeA = true;
    bool edgeB = true;

    for (int i = 0; i < networkA.nodes.size(); ++i) {
        int m = networkA.nodes[i].netCnxs.size();
        for (int j = 0; j < m; ++j) {
            int n = networkA.nodes[networkA.nodes[i].netCnxs[j]].netCnxs.size();
            ++checkEdgeA[m][n];
        }
    }
    for (int i = 0; i < networkB.nodes.size(); ++i) {
        int m = networkB.nodes[i].netCnxs.size();
        for (int j = 0; j < m; ++j) {
            int n = networkB.nodes[networkB.nodes[i].netCnxs[j]].netCnxs.size();
            ++checkEdgeB[m][n];
        }
    }
    for (int i = 0; i < checkEdgeA.size(); ++i) {
        if (!(checkEdgeA[i] == networkA.edgeDistribution[i])) {
            edgeA = false;
            break;
        }
    };
    for (int i = 0; i < checkEdgeB.size(); ++i) {
        if (!(checkEdgeB[i] == networkB.edgeDistribution[i])) {
            edgeB = false;
            break;
        }
    };

    return nodeA && nodeB && edgeA && edgeB;
}

/**
 * @brief Wraps the coordinates out of bounds back into the periodic box, only for 2 dimensions
 * @param coords Coordinates to be wrapped (1D vector of pairs)
 */
void LinkedNetwork::wrapCoords(std::vector<double> &coords) const {
    for (int i = 0, j = 1; i < coords.size(); i += 2, j += 2) {
        coords[i] -= networkA.dimensions[0] * nearbyint(coords[i] * networkA.reciprocalDimensions[0]) - networkA.dimensions[0] * 0.5;
        coords[j] -= networkA.dimensions[1] * nearbyint(coords[j] * networkA.reciprocalDimensions[1]) - networkA.dimensions[1] * 0.5;
    }
}

// Write networks in format that can be loaded and visualised
void LinkedNetwork::write(const std::string &prefix) {
    networkA.write(prefix + "_A");
    networkB.write(prefix + "_B");
}

/**
 * @brief Calculates angle between two vectors
 * @param vector1 First vector
 * @param vector2 Second vector
 * @return Angle between the two vectors in radians
 */
double getClockwiseAngleBetweenVectors(const std::vector<double> &vector1, const std::vector<double> &vector2) {
    double dotProduct = vector1[0] * vector2[0] + vector1[1] * vector2[1];
    double magnitudeProduct = std::sqrt(vector1[0] * vector1[0] + vector1[1] * vector1[1]) *
                              std::sqrt(vector2[0] * vector2[0] + vector2[1] * vector2[1]);
    double angle = std::acos(dotProduct / magnitudeProduct);

    // If the cross product is positive, subtract the angle from 2π to get the angle in the range [π, 2π]
    if (vector1[0] * vector2[1] - vector1[1] * vector2[0] > 0) {
        angle = 2 * M_PI - angle;
    }

    return angle;
}

/**
 *
 * @brief Get the clockwise angle of the vector between two nodes relative to the x axis
 * @param coord1 Coordinates of the first node
 * @param coord2 Coordinates of the second node
 * @param dimensions Dimensions of the periodic box
 * @return Angle of the vector between the two nodes relative to the x axis in radians
 */
double getClockwiseAngle(const std::vector<double> &coord1, const std::vector<double> &coord2, const std::vector<double> &dimensions) {
    std::vector<double> periodicVector = pbcVector(coord1, coord2, dimensions);

    // Range of this function is -pi to pi
    double angle = std::atan2(periodicVector[1], periodicVector[0]);

    // So we have to convert it to 0 to 2pi
    if (angle < 0) {
        angle += 2 * M_PI;
    }
    return 2 * M_PI - angle;
}

/**
 * @brief Checks if a given node has clockwise neighbours
 * @param nodeID ID of the node to check
 * @return true if the node has clockwise neighbours, false otherwise
 */
bool LinkedNetwork::checkClockwiseNeighbours(const int &nodeID) const {
    Node node = networkA.nodes[nodeID];
    double prevAngle = getClockwiseAngle(node.crd, networkA.nodes[node.netCnxs.back()].crd, dimensions);

    // The angle a neighbour makes with the x axis should always increase, EXCEPT when the angle wraps around from 2π to 0
    // This should only happen once if the neighbours are clockwise
    int timesDecreased = 0;
    for (int id : node.netCnxs) {
        double angle = getClockwiseAngle(node.crd, networkA.nodes[id].crd, dimensions);
        if (angle < prevAngle) {
            timesDecreased++;
            if (timesDecreased == 2) {
                // So return false if it decreases twice
                return false;
            }
        }
        prevAngle = angle;
    }
    return true;
}
/**
 * @brief Checks if a given node in network A has clockwise neighbours with given coordinates
 * @param nodeID ID of the node to check
 * @return true if the node has clockwise neighbours, false otherwise
 */
bool LinkedNetwork::checkClockwiseNeighbours(const int &nodeID, const std::vector<double> &coords) const {
    const std::vector<double> nodeCoord = {coords[2 * nodeID], coords[2 * nodeID + 1]};
    const int lastNeighbourCoordsID = networkA.nodes[nodeID].netCnxs.back();
    const std::vector<double> lastNeighbourCoord = {coords[2 * lastNeighbourCoordsID], coords[2 * lastNeighbourCoordsID + 1]};
    double prevAngle = getClockwiseAngle(nodeCoord, lastNeighbourCoord, dimensions);
    int timesDecreased = 0;
    for (const auto &id : networkA.nodes[nodeID].netCnxs) {
        double angle = getClockwiseAngle(nodeCoord, {coords[2 * id], coords[2 * id + 1]}, dimensions);
        if (angle < prevAngle) {
            timesDecreased++;
            if (timesDecreased == 2) {
                return false;
            }
        }
        prevAngle = angle;
    }
    return true;
}

/**
 * @brief Checks if all nodes have clockwise neighbours
 * @param logger Logger to log any anticlockwise neighbours
 * @return true if all nodes have clockwise neighbours, false otherwise
 */
bool LinkedNetwork::checkAllClockwiseNeighbours() const {
    bool check = true;
    for (int nodeID = 0; nodeID < networkA.nodes.size(); ++nodeID) {
        if (!checkClockwiseNeighbours(nodeID)) {
            std::ostringstream oss;
            oss << "Node " << nodeID << " has anticlockwise neighbours: ";
            for (int neighbourID : networkA.nodes[nodeID].netCnxs) {
                oss << neighbourID << " ";
            }
            logger->warn(oss.str());
            check = false;
        }
    }
    return check;
}

void LinkedNetwork::arrangeNeighboursClockwise(const int &nodeID, const std::vector<double> &coords) {
    // Get the coordinates of the center node
    const std::vector<double> nodeCoord = {coords[2 * nodeID], coords[2 * nodeID + 1]};

    // Get the neighbour IDs of the center node
    std::vector<int> &neighbours = networkA.nodes[nodeID].netCnxs;

    // Create a vector of pairs, where each pair contains a neighbour ID and the angle between the center node and that neighbour
    std::vector<std::pair<int, double>> neighbourAngles;
    for (const auto &neighbourID : neighbours) {
        std::vector<double> neighbourCoord = {coords[2 * neighbourID], coords[2 * neighbourID + 1]};
        double angle = getClockwiseAngle(nodeCoord, neighbourCoord, dimensions);
        neighbourAngles.push_back({neighbourID, angle});
    }

    // Sort the vector of pairs in ascending order of angle
    std::sort(neighbourAngles.begin(), neighbourAngles.end(), [](const std::pair<int, double> &a, const std::pair<int, double> &b) {
        return a.second < b.second;
    });

    // Replace the neighbour IDs in the center node with the sorted neighbour IDs
    for (size_t i = 0; i < neighbours.size(); ++i) {
        neighbours[i] = neighbourAngles[i].first;
    }
}

/**
 * @brief Checks if all angles around all nodes are less than or equal to maximum angle
 * @param coords Coordinates of all nodes as a 1D vector of coordinate pairs
 * @param logger Logger to log any non-convex angles
 * @return true if all angles are convex, false otherwise
 */
bool LinkedNetwork::checkAnglesWithinRange(const std::vector<double> &coords) {
    for (int nodeID = 0; nodeID < networkA.nodes.size(); ++nodeID) {
        arrangeNeighboursClockwise(nodeID, coords);
        for (int i = 0; i < networkA.nodes[nodeID].netCnxs.size(); ++i) {
            int neighbourID = networkA.nodes[nodeID].netCnxs[i];
            int nextNeighbourID = networkA.nodes[nodeID].netCnxs[(i + 1) % networkA.nodes[nodeID].netCnxs.size()];
            std::vector<double> v1 = pbcVector(std::vector<double>{coords[neighbourID * 2], coords[neighbourID * 2 + 1]},
                                               std::vector<double>{coords[nodeID * 2], coords[nodeID * 2 + 1]}, dimensions);
            std::vector<double> v2 = pbcVector(std::vector<double>{coords[nextNeighbourID * 2], coords[nextNeighbourID * 2 + 1]},
                                               std::vector<double>{coords[nodeID * 2], coords[nodeID * 2 + 1]}, dimensions);
            double angle = getClockwiseAngleBetweenVectors(v1, v2);
            if (angle > maximumAngle) {
                logger->warn("Node {} has an out of bounds angle between neighbours {} and {}: {:.2f} degrees", nodeID, neighbourID, nextNeighbourID, angle * 180 / M_PI);

                return false;
            }
        }
    }
    return true;
}

/**
 * @brief Checks if all angles around the given nodes are less than or equal to maximum angle
 * @param nodeIDs IDs of the nodes to check
 * @param coords Coordinates of all nodes as a 1D vector of coordinate pairs
 * @param logger Logger to log any non-convex angles
 * @return true if all angles are convex, false otherwise
 */
bool LinkedNetwork::checkAnglesWithinRange(const std::vector<int> &nodeIDs, const std::vector<double> &coords) {
    for (int nodeID : nodeIDs) {
        arrangeNeighboursClockwise(nodeID, coords);
        for (int i = 0; i < networkA.nodes[nodeID].netCnxs.size(); ++i) {
            int neighbourID = networkA.nodes[nodeID].netCnxs[i];
            int nextNeighbourID = networkA.nodes[nodeID].netCnxs[(i + 1) % networkA.nodes[nodeID].netCnxs.size()];
            std::vector<double> v1 = pbcVector(std::vector<double>{coords[neighbourID * 2], coords[neighbourID * 2 + 1]},
                                               std::vector<double>{coords[nodeID * 2], coords[nodeID * 2 + 1]}, dimensions);
            std::vector<double> v2 = pbcVector(std::vector<double>{coords[nextNeighbourID * 2], coords[nextNeighbourID * 2 + 1]},
                                               std::vector<double>{coords[nodeID * 2], coords[nodeID * 2 + 1]}, dimensions);
            double angle = getClockwiseAngleBetweenVectors(v1, v2);
            if (angle > maximumAngle) {
                logger->warn("Node {} has an out of bounds angle between neighbours {} and {}: {:.2f} degrees", nodeID, neighbourID, nextNeighbourID, angle * 180 / M_PI);
                return false;
            }
        }
    }
    return true;
}

// Constants for connection types
const int CNX_TYPE_33 = 33;
const int CNX_TYPE_44 = 44;
const int CNX_TYPE_43 = 43;
/**
 * @brief Gets the connection type of two connected nodes
 * @param node1Coordination Coordination of the first node
 * @param node2Coordination Coordination of the second node
 * @return The type of the selected connection. 33, 34 or 44.
 * @throw std::runtime_error if the nodes in the random connection have coordinations other than 3 or 4.
 */
int LinkedNetwork::assignValues(int node1Coordination, int node2Coordination) const {
    if (node1Coordination == 3 && node2Coordination == 3) {
        return CNX_TYPE_33;
    } else if (node1Coordination == 4 && node2Coordination == 4) {
        return CNX_TYPE_44;
    } else if ((node1Coordination == 3 && node2Coordination == 4) ||
               (node1Coordination == 4 && node2Coordination == 3)) {
        return CNX_TYPE_43;
    } else {
        std::ostringstream oss;
        oss << "Two nodes have unsupported cooordinations: " << node1Coordination
            << " and " << node2Coordination;
        throw std::runtime_error(oss.str());
    }
}

bool LinkedNetwork::checkBondLengths(const int &nodeID, const std::vector<double> &coords) const {
    for (const auto &neighbourID : networkA.nodes[nodeID].netCnxs) {
        std::vector<double> pbcVec = pbcVector(std::vector<double>{coords[2 * nodeID], coords[2 * nodeID + 1]},
                                               std::vector<double>{coords[2 * neighbourID], coords[2 * neighbourID + 1]}, dimensions);
        if (std::sqrt(pbcVec[0] * pbcVec[0] + pbcVec[1] * pbcVec[1]) > maximumBondLength) {
            logger->warn("Node {} has a bond length greater than the maximum bond length with neighbour {}: {:.2f}", nodeID, neighbourID, std::sqrt(pbcVec[0] * pbcVec[0] + pbcVec[1] * pbcVec[1]));
            return false;
        }
    }
    return true;
}

bool LinkedNetwork::checkBondLengths(const std::vector<int> &nodeIDs, const std::vector<double> &coords) const {
    return std::all_of(nodeIDs.begin(), nodeIDs.end(), [this, &coords](int nodeID) {
        return checkBondLengths(nodeID, coords);
    });
}

Direction LinkedNetwork::getRingsDirection(const std::vector<int> &ringNodeIDs) const {
    if (ringNodeIDs.size() != 4) {
        throw std::invalid_argument("Error getting ring direction, ringNodeIDs size is not 4");
    }
    std::vector<double> midCoords(2, 0.0);
    for (int id : ringNodeIDs) {
        midCoords[0] += currentCoords[id * 2];
        midCoords[1] += currentCoords[id * 2 + 1];
    }
    midCoords[0] /= 4;
    midCoords[1] /= 4;
    int timesDecreased = 0;
    double prevAngle = getClockwiseAngle(midCoords,
                                         {currentCoords[ringNodeIDs.back() * 2], currentCoords[ringNodeIDs.back() * 2 + 1]},
                                         dimensions);
    for (int id : ringNodeIDs) {
        double angle = getClockwiseAngle(midCoords, {currentCoords[id * 2], currentCoords[id * 2 + 1]}, dimensions);
        if (angle < prevAngle) {
            timesDecreased++;
            if (timesDecreased == 2) {
                logger->debug("Anticlockwise");
                return Direction::ANTICLOCKWISE;
            }
        }
        prevAngle = angle;
    }
    logger->debug("Clockwise");
    return Direction::CLOCKWISE;
}

std::tuple<std::vector<double>, std::vector<double>> LinkedNetwork::rotateBond(const int &atomID1, const int &atomID2,
                                                                               const Direction &direct) const {
    logger->debug("Rotating bond between atoms {} and {}", atomID1, atomID2);
    std::vector<double> atom1Coord = {currentCoords[atomID1 * 2], currentCoords[atomID1 * 2 + 1]};
    std::vector<double> atom2Coord = {currentCoords[atomID2 * 2], currentCoords[atomID2 * 2 + 1]};

    // Calculate the center point
    double centerX = (atom1Coord[0] + atom2Coord[0]) / 2.0;
    double centerY = (atom1Coord[1] + atom2Coord[1]) / 2.0;

    // Translate the points to the origin
    atom1Coord[0] -= centerX;
    atom1Coord[1] -= centerY;
    atom2Coord[0] -= centerX;
    atom2Coord[1] -= centerY;

    std::vector<double> rotatedAtom1Coord;
    std::vector<double> rotatedAtom2Coord;
    // Rotate the points by 90 degrees
    if (direct == Direction::CLOCKWISE) {
        rotatedAtom1Coord = {atom1Coord[1], -atom1Coord[0]};
        rotatedAtom2Coord = {atom2Coord[1], -atom2Coord[0]};
    } else {
        rotatedAtom1Coord = {-atom1Coord[1], atom1Coord[0]};
        rotatedAtom2Coord = {-atom2Coord[1], atom2Coord[0]};
    }

    // Translate the points back
    rotatedAtom1Coord[0] += centerX;
    rotatedAtom1Coord[1] += centerY;
    rotatedAtom2Coord[0] += centerX;
    rotatedAtom2Coord[1] += centerY;

    return {rotatedAtom1Coord, rotatedAtom2Coord};
}
