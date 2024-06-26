// Network contains nodes and topological information
#ifndef NL_NETWORK_H
#define NL_NETWORK_H

#include "node.h"
#include "output_file.h"
#include "vector_tools.h"
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>

#include <spdlog/spdlog.h>

using LoggerPtr = std::shared_ptr<spdlog::logger>;

enum class NetworkType {
  BASE_NETWORK,
  DUAL_NETWORK
};

struct Network {
    // Member variables
    NetworkType type;
    std::string networkString;
    int numNodes;
    std::vector<Node> nodes;
    std::vector<double> dimensions;

    // Statistics
    double pearsonsCoeff;
    double entropy;
    std::map<int, double> nodeSizes;
    std::map<int, std::map<int, double>> assortativityDistribution;

    // Constructors
    Network();
    Network(const NetworkType networkType, const LoggerPtr &logger); // construct by loading from files
    void readInfo(const std::string &filePath);
    void readCoords(const std::string &filePath);
    void readConnections(const std::string &filePath, const bool &isDual);



    // Member Functions
    void rescale(const double &scaleFactor);   // rescale coordinates
    void refreshStatistics();
    void refreshAssortativityDistribution();
    void refreshCoordinationDistribution();
    void refreshPearsonsCoeff();
    void refreshEntropy();

    double getAverageCoordination() const;
    double getAboavWeaire() const;
    double getAverageCoordination(const int &power) const;

    // Write functions
    void writeInfo(std::ofstream &infoFile) const;
    void writeCoords(std::ofstream &crdFile) const;
    void writeConnections(std::ofstream &cnxFile, const std::vector<std::vector<int>> &cnxs) const;
    std::vector<std::vector<int>> getConnections() const;
    std::vector<std::vector<int>> getDualConnections() const;
    void write() const;

    int getMaxConnections() const;
    int getMaxConnections(const std::unordered_set<int> &fixedNodes) const;

    int getMinConnections() const;
    int getMinConnections(const std::unordered_set<int> &fixedNodes) const;

    int getMaxDualConnections() const;
    int getMinDualConnections() const;
    int getMinDualConnections(const std::unordered_set<int> &fixedNodes) const;

    std::vector<double> getCoords();
    void centreRings(const Network &baseNetwork);

    int findNumberOfUniqueDualNodes();
    void display(const LoggerPtr &logger) const;
};

#endif // NL_NETWORK_H
