#include "node.h"

/**
 * @brief Construct a node with an ID of 0, no connections and coordinate at [0, 0]
 */
Node::Node() = default;

/**
 * @brief Construct node with a given ID with no connections and coordinate at [0, 0]
 * @param nodeId The ID of the node
 */
Node::Node(const int &nodeId) : id(nodeId) {
}

/**
 * @brief Constructor with a given node ID and coordinates and no connections
 * @param nodeID The ID of the node
 * @param crd The coordinates of the node
 */
Node::Node(const int &nodeID, const std::vector<double> &crd) : id(nodeID), crd(crd) {
}

/**
 * @brief Constructor with a given node ID, coordinates and connections
 * @param nodeID The ID of the node
 * @param crd The coordinates of the node
 * @param netConnections The connections to nodes in the network
 * @param dualConnections The connections to nodes in the dual network
 */
Node::Node(const int &nodeID, const std::vector<double> &crd, const std::vector<int> &netConnections, const std::vector<int> &dualConnections)
    : id(nodeID), crd(crd), netConnections(netConnections), dualConnections(dualConnections) {
}

/**
 * @brief Convert the node to a string
 */
std::string Node::toString() const {
    std::string str = "Node " + std::to_string(id) + " at " + std::to_string(crd[0]) + ", " + std::to_string(crd[1]) +
                      " with neighbours: ";
    std::for_each(netConnections.begin(), netConnections.end(), [&str](int i) { str += std::to_string(i) + " "; });
    str += " and ring neighbours: ";
    std::for_each(dualConnections.begin(), dualConnections.end(), [&str](int i) { str += std::to_string(i) + " "; });
    return str;
}
