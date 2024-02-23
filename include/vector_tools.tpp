#include "vector_tools.h"
/**
 * @brief Checks if a vector contains a value
 * @tparam T The type of the vector
 * @param vector The vector to be checked
 * @param value The value to be checked for
 * @return True if the value is in the vector, false otherwise
 */
template <typename T>
bool vectorContains(const std::vector<T> &vector, const T &value) {
    return std::find(vector.begin(), vector.end(), value) != vector.end();
}
/**
 * @brief Perform linear regression on two vectors
 * @tparam T The type of the vectors
 * @param vector1 The first vector
 * @param vector2 The second vector
 * @return A tuple containing the gradient, intercept and r-squared value
 * @throw std::runtime_error if the vectors are not of the same size
 */
template <typename T>
std::tuple<double, double, double> vectorLinearRegression(const std::vector<T> &vector1, const std::vector<T> &vector2) {
    if (vector1.size() != vector2.size())
        throw std::runtime_error("Regression error - must be equal number of vector1 and vector2 values");

    double sumX = 0.0;
    double sumY = 0.0;
    double sumXY = 0.0;
    double sumXX = 0.0;
    double sumYY = 0.0;

    auto vecSize = static_cast<int>(vector1.size());
    for (int i = 0; i < vecSize; ++i) {
        sumX += vector1[i];
        sumY += vector2[i];
        sumXY += vector1[i] * vector2[i];
        sumXX += vector1[i] * vector1[i];
        sumYY += vector2[i] * vector2[i];
    }

    double sqSumX = sumX * sumX;
    double sqSumY = sumY * sumY;
    double sdX = sqrt(vecSize * sumXX - sqSumX);
    double sdY = sqrt(vecSize * sumYY - sqSumY);
    double r = (vecSize * sumXY - sumX * sumY) / (sdX * sdY);

    // gradient, intercept and r-squared
    double gradient = r * sdY / sdX;
    return std::make_tuple(gradient, (sumY - gradient * sumX) / vecSize, r * r);
}
/**
 * @brief Divides all the values in a vector by a constant
 * @tparam T The type of the vector
 * @param vector The vector to be divided
 * @param divideBy The value to divide the vector by
 */
template <typename T>
void divideVector(std::vector<T> &vector, const double &divideBy) {
    for (auto &value : vector) {
        value /= divideBy;
    }
}

/**
 * @brief Multiplies all the values in a vector by a constant
 * @tparam T The type of the vector
 * @param vector The vector to be multiplied
 * @param multiplyBy The value to multiply the vector by
 */
template <typename T>
void multiplyVector(std::vector<T> &vector, const double &multiplyBy) {
    for (auto &value : vector) {
        value *= multiplyBy;
    }
}

/**
 * @brief Adds a constant to all the values in a vector
 * @tparam T The type of the vector
 * @param vector The vector to be added to
 * @param addition The value to add to the vector
 */
template <typename T>
void addToVector(std::vector<double> &vector, const double &addition) {
    for (auto &value : vector) {
        value += addition;
    }
}

/**
 * @brief Subtracts a constant from all the values in a vector
 * @tparam T The type of the vector
 * @param vector The vector to be subtracted from
 * @param subtraction The value to subtract from the vector
 */
template <typename T>
void subtractFromVector(std::vector<T> &vector, const double &subtraction) {
    for (auto &value : vector) {
        value -= subtraction;
    }
}

/**
 * @brief Sums all the values in a vector
 * @tparam T The type of the vector
 * @param vector The vector to be summed
 * @return The sum of the vector values
 */
template <typename T>
T vectorSum(const std::vector<T> &vector) {
    return std::accumulate(vector.begin(), vector.end(), static_cast<T>(0));
}
/**
 * @brief Multiplies two vectors element-wise
 * @tparam T The type of the vectors
 * @param vector1 The first vector
 * @param vector2 The second vector
 * @return A vector of the multiplied values
 * @throw std::runtime_error if the vectors are not of the same size
 */
template <typename T>
std::vector<T> multiplyVectors(const std::vector<T> &vector1, const std::vector<T> &vector2) {
    auto vecSize = static_cast<int>(vector1.size());
    if (vecSize != vector2.size()) {
        throw std::runtime_error("Vectors must be of the same size");
    }
    std::vector<T> result;
    result.reserve(vecSize);
    for (int i = 0; i < vecSize; ++i) {
        result.push_back(vector1[i] * vector2[i]);
    }
    return result;
}

/**
 * @brief Finds the common values between two vectors
 * @tparam T The type of the vectors
 * @param vector1 The first vector
 * @param vector2 The second vector
 * @return A vector of common values
 */
template <typename T>
std::vector<T> intersectVectors(const std::vector<T> &vector1, const std::vector<T> &vector2) {
    std::unordered_set<T> set2(vector2.begin(), vector2.end());
    std::vector<T> result;
    for (const auto &value : vector1) {
        if (set2.find(value) != set2.end()) {
            result.push_back(value);
        }
    }
    return result;
}

/**
 * @brief Deletes all occurrences of a value from a vector
 * @tparam T The type of the vector
 * @param vector The vector from which to delete the value
 * @param args The values to be deleted
 */
template <typename T, typename... Args>
void deleteByValues(std::vector<T> &vector, const Args &...args) {
    std::unordered_set<T> valuesToDelete{args...};
    vector.erase(std::remove_if(vector.begin(), vector.end(),
                                [&valuesToDelete](const T &value) {
                                    return valuesToDelete.find(value) != valuesToDelete.end();
                                }),
                 vector.end());
}

/**
 * @brief Replaces all occurrences of a value in a vector with a new value
 * @tparam T The type of the vector
 * @param vector The vector in which to replace the value
 * @param oldValue The value to be replaced
 * @param newValue The value to replace the old value
 */
template <typename T>
void replaceValue(std::vector<T> &vector, const T &oldValue, const T &newValue) {
    std::replace(vector.begin(), vector.end(), oldValue, newValue);
}

/**
 * @brief Averages the values in a vector
 * @tparam T The type of the vector
 * @param vector The vector to be averaged
 * @return The average of the vector values
 */
template <typename T>
double vectorMean(const std::vector<T> &vector) {
    return vectorSum(vector) / vector.size();
}

/**
 * @brief Gets the unique values in a vector
 * @tparam T The type of the vector
 * @param vector The vector to be checked
 * @return A vector of unique values
 */
template <typename T>
std::vector<T> getUniqueValues(const std::vector<T> &vector) {
    std::unordered_set<T> uniqueSet;
    std::vector<T> uniqueVector;
    for (const auto &value : vector) {
        if (uniqueSet.insert(value).second) {
            uniqueVector.push_back(value);
        }
    }
    return uniqueVector;
}