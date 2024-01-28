#include "input_data.h"
#include "output_file.h"
#include <map>
#include <utility>
#include <numeric>
#include <fstream>
#include <climits>
#include <set>
#include <stdexcept>
#include <limits>
#include <type_traits>
#include <sstream>
#include <functional>
#include <spdlog/spdlog.h>

/**
 * @brief Converts a string to a boolean
 * @param str The string to be converted
 * @return The boolean value
 * @throws std::invalid_argument if the string is not "true" or "false"
 */
bool InputData::stringToBool(const std::string &str)
{
    if (str == "true")
        return true;
    else if (str == "false")
        return false;
    else
        throw std::invalid_argument("Invalid boolean: " + str);
}

/**
 * @brief Gets the first word from a line in the input file
 * @param inputFile The input file stream
 * @param iss The input string stream
 * @return The first word from the line
 */
std::string InputData::getFirstWord(std::ifstream &inputFile, std::istringstream &iss)
{
    std::string line;
    getline(inputFile, line);
    lineNumber++;
    iss.str(line);
    iss.clear();
    std::string firstWord;
    iss >> firstWord;
    return firstWord;
}

/**
 * @brief Reads a section of the input file
 * @param inputFile The input file stream
 * @param args The values to be read
 * @tparam Args The types of the values to be read
 * @return void
 */

void InputData::readIO(std::ifstream &inputFile, const LoggerPtr &logger)
{
    readSection(inputFile, "IO", logger,
                outputFolder,
                outputFilePrefix,
                inputFolder,
                inputFilePrefix,
                isFromScratchEnabled,
                isRestartUsingLammpsObjectsEnabled);
}

void InputData::readNetworkProperties(std::ifstream &inputFile, const LoggerPtr &logger)
{
    readSection(inputFile, "Network Properties", logger,
                numRings,
                minRingSize,
                maxRingSize,
                minCoordination,
                maxCoordination,
                isFixRingsEnabled,
                fixedRingsFile);
}

void InputData::readNetworkMinimisationProtocols(std::ifstream &inputFile, const LoggerPtr &logger)
{
    readSection(inputFile, "Network Minimisation Protocols", logger,
                isOpenMPIEnabled,
                isSimpleGrapheneEnabled,
                isTriangleRaftEnabled,
                isBilayerEnabled,
                isTersoffGrapheneEnabled,
                isBNEnabled,
                selectedMinimisationProtocol);
}

void InputData::readMonteCarloProcess(std::ifstream &inputFile, const LoggerPtr &logger)
{
    readSection(inputFile, "Monte Carlo Process", logger,
                moveType,
                randomSeed,
                isSpiralEnabled,
                spiralRadius,
                randomOrWeighted);
}

void InputData::readMonteCarloEnergySearch(std::ifstream &inputFile, const LoggerPtr &logger)
{
    readSection(inputFile, "Monte Carlo Energy Search", logger,
                startTemperature,
                endTemperature,
                temperatureIncrement,
                thermalisationTemperature,
                stepsPerTemperature,
                initialThermalisationSteps);
}

void InputData::readPotentialModel(std::ifstream &inputFile, const LoggerPtr &logger)
{
    readSection(inputFile, "Potential Model", logger,
                harmonicBondForceConstant,
                harmonicAngleForceConstant,
                harmonicGeometryConstraint,
                isMaintainConvexityEnabled);
}

void InputData::readGeometryOptimisation(std::ifstream &inputFile, const LoggerPtr &logger)
{
    readSection(inputFile, "Geometry Optimisation", logger,
                monteCarloLocalMaxIterations,
                globalMinimisationMaxIterations,
                tauBacktrackingParameter,
                tolerance,
                localRegionSize);
}

void InputData::readAnalysis(std::ifstream &inputFile, const LoggerPtr &logger)
{
    readSection(inputFile, "Analysis", logger,
                analysisWriteFrequency,
                isWriteSamplingStructuresEnabled,
                structureWriteFrequency);
}

void InputData::readOutput(std::ifstream &inputFile, const LoggerPtr &logger)
{
    readSection(inputFile, "Output", logger,
                ljPairsCalculationDistance);
}

void InputData::checkInSet(const std::string &value, const std::set<std::string> &validValues, const std::string &errorMessage)
{
    if (validValues.count(value) == 0)
    {
        throw std::runtime_error(errorMessage);
    }
}

void InputData::checkFileExists(const std::string &filename)
{
    std::ifstream file(filename);
    if (!file)
    {
        throw std::runtime_error("File does not exist: " + filename);
    }
}

void InputData::validate()
{
    double maxDouble = std::numeric_limits<double>::max();
    // Network Properties
    checkInRange(numRings, 1, INT_MAX, "Number of rings must be at least 1");
    checkInRange(minRingSize, 3, INT_MAX, "Minimum ring size must be at least 3");
    checkInRange(maxRingSize, minRingSize, INT_MAX, "Maximum ring size must be at least the minimum ring size");
    checkInRange(minCoordination, 1, INT_MAX, "Minimum coordination must be at least 1");
    checkInRange(maxCoordination, minCoordination, INT_MAX, "Maximum coordination must be at least the minimum coordination");
    if (isFixRingsEnabled)
    {
        checkFileExists(fixedRingsFile);
    }

    // Minimisation Protocols
    std::map<int, std::pair<bool *, std::string>> protocolMap = {
        {1, {&isSimpleGrapheneEnabled, "Simple Graphene"}},
        {2, {&isTriangleRaftEnabled, "Triangle Raft"}},
        {3, {&isTriangleRaftEnabled, "Bilayer"}},
        {4, {&isTersoffGrapheneEnabled, "Tersoff Graphene"}},
        {5, {&isBNEnabled, "BN"}}};

    if (protocolMap.count(selectedMinimisationProtocol) == 1)
    {
        auto &[isEnabled, protocolName] = protocolMap[selectedMinimisationProtocol];
        if (!*isEnabled)
        {
            throw std::runtime_error("Selected minimisation protocol is " +
                                     std::to_string(selectedMinimisationProtocol) + " but " + protocolName + " is disabled");
        }
    }
    else
    {
        throw std::runtime_error("Selected minimisation protocol, " +
                                 std::to_string(selectedMinimisationProtocol) + " is out of range");
    }

    // Monte Carlo Process
    checkInSet(moveType, {"switch", "mix"}, "Invalid move type: " + moveType + " must be either 'switch' or 'mix'");
    checkInRange(randomSeed, 0, INT_MAX, "Random seed must be at least 0");
    if (isSpiralEnabled)
    {
        checkInRange(spiralRadius, 1, INT_MAX, "Spiral radius must be at least 1");
    }
    checkInSet(randomOrWeighted, {"random", "weighted"}, "Invalid random or weighted: " + randomOrWeighted + " must be either 'random' or 'weighted'");

    // Potential Model
    checkInRange(harmonicBondForceConstant, 0.0, maxDouble, "Harmonic bond force constant must be at least 0");
    checkInRange(harmonicAngleForceConstant, 0.0, maxDouble, "Harmonic angle force constant must be at least 0");
    checkInRange(harmonicGeometryConstraint, 0.0, maxDouble, "Harmonic geometry constraint must be at least 0");

    // Geometry Optimisation
    checkInRange(monteCarloLocalMaxIterations, 0, INT_MAX, "Monte Carlo local max iterations must be at least 0");
    checkInRange(globalMinimisationMaxIterations, 0, INT_MAX, "Global minimisation max iterations must be at least 0");

    // Analysis
    checkInRange(analysisWriteFrequency, 0, 1000, "Analysis write frequency must be between 0 and 1000");

    // Output
    checkInRange(ljPairsCalculationDistance, 0, INT_MAX, "LJ pairs calculation distance must be at least 0");
}

/**
 * @brief Reads the input file
 * @param filePath The path to the input file
 * @param logger The log file
 */
InputData::InputData(const std::string &filePath, const LoggerPtr &logger)
{
    // Open the input file
    std::ifstream inputFile(filePath);

    // Check if the file was opened successfully
    if (!inputFile)
    {
        throw std::runtime_error("Unable to open file: " + filePath);
    }
    logger->info("Reading input file: " + filePath);

    // Skip the title line
    inputFile.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    lineNumber++;

    // Read sections
    readIO(inputFile, logger);
    readNetworkProperties(inputFile, logger);
    readNetworkMinimisationProtocols(inputFile, logger);
    readMonteCarloProcess(inputFile, logger);
    readMonteCarloEnergySearch(inputFile, logger);
    readPotentialModel(inputFile, logger);
    readGeometryOptimisation(inputFile, logger);
    readAnalysis(inputFile, logger);
    logger->info("Succeessfully read input file!");

    // Validate input data
    logger->info("Validating input data...");
    validate();
    logger->info("Successfully validated input data!");
}